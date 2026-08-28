#include <sl/menu/ui/Menu.hpp>
#include <unordered_set>
#include <sl/menu/ui/Locale.hpp>
#include <sl/menu/net/Http.hpp>
#include <sl/smi/Protocol.hpp>
#include <SDL2/SDL_image.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include "Menu_Internal.hpp"

namespace sl::menu::ui {

    // Shelf mode: an Xbox-360 "My Games" style row of uniform covers. The selected
    // cover is anchored near the left inside a highlight card that shows its name
    // and platform; the rest of the row scrolls behind it. Unselected covers carry
    // a small caption underneath.
    void Menu::DrawMainShelf() {
        const Theme &t = m_theme.Current();
        m_icons.SetScale(0);
        DrawTopBar(nullptr);

        if (m_items.empty()) { DrawMainEmpty(); return; }

        if (!ScrollBusy())   // a finger or a throw owns the scroll instead
            m_scroll_pos += (m_cursor - m_scroll_pos) * 0.30f;
        if (std::abs(m_cursor - m_scroll_pos) < 0.01f) m_scroll_pos = (float)m_cursor;

        const int total = (int)m_items.size();
        const int tile  = ShelfTileW();      // tile width
        const int tileH = ShelfTileH();      // ...and height, which differ in
        const int top   = kShelfTop;         // vertical mode
        const int pitch = ShelfPitch();

        auto ellipsize = [&](const std::string &s, int maxw, gfx::FontSize fs) {
            return Ellipsize(s, maxw, fs);
        };

        // Header row below the top bar: sort on the left, position on the right.
        m_gfx->Text(FontSize::Small,  kShelfAnchorX, 64, t.dim, T("sort"));
        m_gfx->Text(FontSize::Normal, kShelfAnchorX, 82, t.fg,  SortLabel());
        {
            char cnt[32];
            snprintf(cnt, sizeof(cnt), "%d / %d", m_cursor + 1, total);
            // Position counters are optional; blanking the string here keeps
            // the layout arithmetic below untouched.
            if (!m_show_counter) cnt[0] = '\0';
            const int cw = m_gfx->TextWidth(FontSize::Large, cnt);
            m_gfx->Text(FontSize::Large, gfx::Gfx::Width - 44 - cw, 70, t.dim, cnt);
        }

        // Highlight card behind the anchored (selected) cover.
        const int pad   = 14;
        const int infoH = 104;
        m_gfx->FillRect(kShelfAnchorX - pad, top - pad,
                        tile + pad * 2, tileH + pad + infoH, WithAlpha(t.fg, 20));

        // Covers, painted right-to-left so the selected one lands on top of its
        // neighbours during a slide.
        int firstv = (int)m_scroll_pos - 1;
        if (firstv < 0) firstv = 0;
        int lastv = (int)m_scroll_pos + (gfx::Gfx::Width - kShelfAnchorX) / pitch + 2;
        if (lastv > total - 1) lastv = total - 1;
        for (int idx = lastv; idx >= firstv; idx--) {
            const int x = kShelfAnchorX + (int)((idx - m_scroll_pos) * pitch);
            if (x + tile < 0 || x > gfx::Gfx::Width) continue;
            const bool selg = (idx == m_cursor);
            // Vertical mode prefers real box art and falls back to the square
            // icon, drawn on a plate so a 1:1 image is letterboxed rather than
            // stretched onto a 2:3 tile.
            SDL_Texture *cov = m_shelf_vertical ? FlowCover(m_items[idx]) : nullptr;
            if (cov) {
                const Uint8 a = selg ? 255 : 225;
                m_gfx->FillRect(x, top, tile, tileH, WithAlpha(t.bg_bottom, a));
                m_gfx->DrawImage(cov, x, top, tile, tileH, a);
            } else if (m_shelf_vertical) {
                m_gfx->FillRect(x, top, tile, tileH, WithAlpha(t.bg_bottom, selg ? 255 : 225));
                const int s2 = (tile < tileH ? tile : tileH) - 16;
                DrawAppTile(m_items[idx], x + (tile - s2) / 2, top + (tileH - s2) / 2,
                            s2, selg, selg ? 255 : 225);
            } else {
                DrawAppTile(m_items[idx], x, top, tile, selg, selg ? 255 : 225);
            }
            if (!selg)
                m_gfx->Text(FontSize::Small, x, top + tileH + 12, t.dim,
                            ellipsize(m_items[idx].name, tile, FontSize::Small).c_str());
        }

        // Release covers well outside the visible run. The shelf shares Flow's
        // cover cache, and leaving it unbounded is what previously starved the
        // rest of the menu of memory.
        if (m_shelf_vertical) {
            const int keep_lo = std::max(0, firstv - 4);
            const int keep_hi = std::min(total - 1, lastv + 4);
            for (auto it2 = m_covers.begin(); it2 != m_covers.end(); ) {
                bool keep = false;
                for (int i = keep_lo; i <= keep_hi && !keep; i++)
                    keep = (m_items[i].app_id == it2->first);
                if (keep) { ++it2; continue; }
                if (it2->second) m_gfx->FreeImage(it2->second);
                it2 = m_covers.erase(it2);
            }
        }

        // Selected item's info block inside the card.
        const MenuItem &sel = m_items[m_cursor];
        m_gfx->Text(FontSize::Normal, kShelfAnchorX, top + tileH + 12, t.title,
                    ellipsize(sel.name, tile, FontSize::Normal).c_str());
        const char *sub = sel.is_gamecard ? T("Game card")
                        : (sel.kind == ItemKind::Game ? T("Nintendo Switch") : "");
        if (sub[0])
            m_gfx->Text(FontSize::Small, kShelfAnchorX, top + tileH + 54, t.dim, sub);
        // Last line of the card: the running badge, or how much this game has been
        // played (blank until the pdm worker lands, and for never-played titles).
        if (sel.app_id == m_suspended && m_suspended != 0) {
            m_gfx->Text(FontSize::Small, kShelfAnchorX, top + tileH + 76, t.accent, T("Running"));
        } else if (const play::PlayInfo *pi = Play(sel.app_id)) {
            if (pi->seconds > 0) {
                const std::string line = play::FormatPlaytime(pi->seconds) + "   " +
                                         play::FormatLastPlayed(pi->last_played);
                m_gfx->Text(FontSize::Small, kShelfAnchorX, top + tileH + 76, t.dim, line.c_str());
            }
        }

        DrawStatusHint({ {{"a"}, "Launch"}, {{"x"}, "Options"} });
    }
} // namespace sl::menu::ui
