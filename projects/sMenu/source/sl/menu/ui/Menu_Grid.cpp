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

    // Grid mode: a page of icon tiles. The page scrolls smoothly (an eased row
    // offset) and the selection is a highlight frame that glides to the cursor,
    // so both axes animate instead of snapping. Tiles fade as they cross the top
    // and bottom edges of the viewport.
    // The unit is whatever squares up inside the band at the chosen counts -
    // limited by whichever axis is tighter, so a wide-and-short wall is sized by
    // its columns and a tall-and-narrow one by its rows.
    int Menu::TileUnit() const {
        const int aw = gfx::Gfx::Width - kWallMargin * 2;
        const int ah = kWallBot - kWallTop;
        const int by_w = (aw - (TileCols()    - 1) * kGridGap) / TileCols();
        const int by_h = (ah - (TileRowsVis() - 1) * kGridGap) / TileRowsVis();
        return std::max(24, std::min(by_w, by_h));
    }
    int Menu::TilePitch() const { return TileUnit() + kGridGap; }
    int Menu::TileWideW() const { return TileUnit() * 2 + kGridGap; }
    int Menu::TileLeft() const {
        const int wall = TileCols() * TileUnit() + (TileCols() - 1) * kGridGap;
        return (gfx::Gfx::Width - wall) / 2;
    }
    // Centred in the band as well as across it: whichever axis did not set the
    // unit has slack, and leaving it all at the bottom looks like a mistake.
    int Menu::TileTop() const {
        const int wall = TileRowsVis() * TileUnit() + (TileRowsVis() - 1) * kGridGap;
        return kWallTop + ((kWallBot - kWallTop) - wall) / 2;
    }
    // How many rows a tile covers, from the height the packer gave it.
    int Menu::TileRowsOf(int h) const { return (h + kGridGap) / TilePitch(); }
    void Menu::SetTileCols(int n) {
        m_tile_cols = std::min(std::max(kTileColsMin, n), kTileColsMax);
        FreeWidgetTileTextures();   // sized to the boxes they were made for
        SaveSettings();
    }
    void Menu::SetTileRows(int n) {
        m_tile_rows = std::min(std::max(kTileRowsMin, n), kTileRowsMax);
        FreeWidgetTileTextures();
        SaveSettings();
    }
    // ---- tiles ---------------------------------------------------------------
    //
    // Packing is row-major with a wrap, which is what makes mixed tile sizes
    // work without a bin-packer: a wide tile that will not fit in what is left
    // of a row starts the next one. Every tile still maps to an m_items index,
    // so A, X and the options overlay need no special case here.
    void Menu::BuildTiles(std::vector<TileRect> &out) const {
        out.clear();
        out.reserve(m_items.size());

        const int x0 = TileLeft();

        // An occupancy grid rather than a running column, because a tile can now
        // be two rows tall: once one of those is placed, the cells beside it on
        // BOTH its rows are what the following tiles have to flow around, and a
        // single "next free column" cannot express that.
        std::vector<std::vector<char>> occ;
        auto ensure = [&](int rows) {
            while ((int)occ.size() < rows) occ.push_back(std::vector<char>(TileCols(), 0));
        };
        auto free_at = [&](int r, int c, int w, int h) {
            ensure(r + h);
            for (int y = r; y < r + h; y++)
                for (int x = c; x < c + w; x++)
                    if (occ[y][x]) return false;
            return true;
        };

        // The search never starts above the row the last tile landed on. That
        // keeps the wall in list order - a small tile drops into a hole beside a
        // large one, which is the point, but it can never leap back up the wall
        // past entries that come before it.
        int scan = 0;
        for (int i = 0; i < (int)m_items.size(); i++) {
            int sw, sh;
            TileSpan(m_items[i], sw, sh);

            int pr = -1, pc = -1;
            for (int r = scan; pr < 0 && r < scan + 64; r++)
                for (int c = 0; c + sw <= TileCols(); c++)
                    if (free_at(r, c, sw, sh)) { pr = r; pc = c; break; }
            if (pr < 0) continue;   // cannot happen with sw <= TileCols(), but be safe

            ensure(pr + sh);
            for (int y = pr; y < pr + sh; y++)
                for (int x = pc; x < pc + sw; x++) occ[y][x] = 1;
            scan = pr;

            TileRect t;
            t.item = i;
            t.x = x0 + pc * TilePitch();
            t.y = TileTop() + pr * TilePitch();
            t.w = sw * TileUnit() + (sw - 1) * kGridGap;
            t.h = sh * TileUnit() + (sh - 1) * kGridGap;
            out.push_back(t);
        }
    }
    // Nearest tile in a direction, resolved entirely from the packed geometry.
    //
    // Every direction has to be geometric, left and right included. Walking the
    // packed list instead looks right until a tall tile is on the wall: it is
    // placed before the entries that flow around it, so from the row under it
    // the list order steps to the end of the row ABOVE, and the tall tile is
    // skipped over - unreachable from its own lower half.
    int Menu::TileNeighbour(int dir) const {
        std::vector<TileRect> tiles;
        BuildTiles(tiles);
        if (tiles.empty()) return m_cursor;

        const int n = (int)tiles.size();
        int cur = 0;
        for (int i = 0; i < n; i++)
            if (tiles[i].item == m_cursor) { cur = i; break; }

        const TileRect &c = tiles[cur];
        const int c_r0   = (c.y - TileTop()) / TilePitch();
        const int c_rows = TileRowsOf(c.h);
        const int c_r1   = c_r0 + c_rows;          // one past the last row it covers
        auto rowOf  = [&](const TileRect &t) { return (t.y - TileTop()) / TilePitch(); };

        if (dir == 0 || dir == 1) {
            const bool right = (dir == 1);
            // Anything sharing a row with the current tile is on the same line as
            // far as the eye is concerned, whichever of its rows that is.
            int best = -1, best_dx = 1 << 30, best_dr = 1 << 30;
            for (int i = 0; i < n; i++) {
                if (i == cur) continue;
                const int r0 = rowOf(tiles[i]), r1 = r0 + TileRowsOf(tiles[i].h);
                if (r1 <= c_r0 || r0 >= c_r1) continue;          // no row in common
                if (right ? (tiles[i].x <= c.x) : (tiles[i].x >= c.x)) continue;
                const int dx = std::abs(tiles[i].x - c.x);
                const int dr = std::abs(r0 - c_r0);
                // Nearest across, then the one starting on the same row - which
                // is the tiebreak that matters when leaving a tall tile, where
                // two candidates can sit at the same x on different rows.
                if (dx < best_dx || (dx == best_dx && dr < best_dr))
                    { best_dx = dx; best_dr = dr; best = i; }
            }
            if (best >= 0) return tiles[best].item;

            // Off the end of the line: continue onto the next one, the way text
            // wraps. Right leaves from the bottom of the tile, left from the top.
            //
            // Two passes, because a tall tile covers the next row without
            // starting on it: wrapping onto its lower half would put the cursor
            // back beside where it just came from, which reads as going
            // backwards. Tiles that BEGIN on the row win; one that merely covers
            // it is the fallback for a row that has nothing else.
            const int want_row = right ? c_r1 : c_r0 - 1;
            int edge = -1, edge_any = -1;
            for (int i = 0; i < n; i++) {
                const int r0 = rowOf(tiles[i]), r1 = r0 + TileRowsOf(tiles[i].h);
                if (want_row < r0 || want_row >= r1) continue;
                int &slot = (r0 == want_row) ? edge : edge_any;
                if (slot < 0 || (right ? tiles[i].x < tiles[slot].x
                                       : tiles[i].x > tiles[slot].x)) slot = i;
            }
            if (edge < 0) edge = edge_any;
            if (edge >= 0) return tiles[edge].item;

            // Off the wall entirely: the same wrap rule as every other mode.
            if (!(m_nav_fresh && m_wrap_nav)) return m_cursor;
            return right ? tiles[0].item : tiles[n - 1].item;
        }

        // Leaving a tall tile downwards means clearing all of it, not just its
        // top row; leaving upwards is always the row above its top.
        const int want = (dir == 2) ? c_r0 - 1 : c_r1;
        if (want < 0) return m_cursor;
        const int cx = c.x + c.w / 2;

        int best = -1, best_d = 1 << 30;
        for (int i = 0; i < n; i++) {
            const int r0 = rowOf(tiles[i]);
            // A tall tile answers for every row it covers, so it can be reached
            // from either side of it.
            if (want < r0 || want >= r0 + TileRowsOf(tiles[i].h)) continue;
            const int tc = tiles[i].x + tiles[i].w / 2;
            const int d  = std::abs(tc - cx);
            if (d < best_d) { best_d = d; best = i; }
        }
        return (best >= 0) ? tiles[best].item : m_cursor;
    }
    // Metro's start screen is a wall of DIFFERENT colours, so a grid painted in
    // one accent misses the whole look. Rather than hard-code a palette that
    // would fight every theme, the theme's own accent is rotated round the hue
    // wheel by a fixed offset per tile: the wall stays recognisably the user's
    // colour scheme while reading as a set of tiles rather than one slab.
    SDL_Color Menu::TileColor(int idx) const {
        // A colour the user picked for this entry beats the generated one.
        if (idx >= 0 && idx < (int)m_items.size()) {
            const auto c = m_tilecfg.find(ItemKey(m_items[idx]));
            if (c != m_tilecfg.end() && c->second.has_color) return c->second.color;
        }
        const SDL_Color b = m_theme.Current().accent;

        const float r = b.r / 255.0f, g = b.g / 255.0f, bl = b.b / 255.0f;
        const float mx = std::max(r, std::max(g, bl));
        const float mn = std::min(r, std::min(g, bl));
        const float d  = mx - mn;

        float h = 0.0f;
        if (d > 0.0001f) {
            if      (mx == r)  h = 60.0f * fmodf((g - bl) / d, 6.0f);
            else if (mx == g)  h = 60.0f * (((bl - r) / d) + 2.0f);
            else               h = 60.0f * (((r - g) / d) + 4.0f);
        }
        // A flat accent has no hue to rotate, so give it one to spread from.
        float s = (mx > 0.0001f) ? d / mx : 0.35f;
        float v = mx;
        if (s < 0.15f) { s = 0.35f; h = 205.0f; }

        static const float kShift[] = { 0, 34, -40, 68, -20, 104, 16, -68 };
        h = fmodf(h + kShift[idx % (int)(sizeof(kShift) / sizeof(kShift[0]))] + 360.0f, 360.0f);
        // Nudge value as well, so neighbouring hues never read as one block.
        v = std::min(1.0f, std::max(0.30f, v + ((idx % 3) - 1) * 0.07f));

        const float c = v * s, x = c * (1.0f - std::fabs(fmodf(h / 60.0f, 2.0f) - 1.0f));
        const float m = v - c;
        float rr = 0, gg = 0, bb = 0;
        if      (h <  60) { rr = c; gg = x; }
        else if (h < 120) { rr = x; gg = c; }
        else if (h < 180) { gg = c; bb = x; }
        else if (h < 240) { gg = x; bb = c; }
        else if (h < 300) { rr = x; bb = c; }
        else              { rr = c; bb = x; }
        return SDL_Color{ (Uint8)((rr + m) * 255), (Uint8)((gg + m) * 255),
                          (Uint8)((bb + m) * 255), b.a };
    }
    void Menu::LoadTileCfg() {
        m_tilecfg.clear();
        FILE *fp = fopen(GetUserConfigPath(kTileCfgFile).c_str(), "r");
        if (!fp) return;
        char line[192];
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            char *eq = strchr(line, '=');
            if (!eq || eq == line) continue;
            *eq = '\0';
            int w = 0, h = 0; char col[16] = "-";
            if (sscanf(eq + 1, "%dx%d,%15s", &w, &h, col) < 2) continue;
            TileCfg c;
            c.w = w; c.h = h;
            unsigned rgb = 0;
            if (col[0] != '-' && sscanf(col, "%x", &rgb) == 1) {
                c.has_color = true;
                c.color = SDL_Color{ (Uint8)((rgb >> 16) & 0xFF), (Uint8)((rgb >> 8) & 0xFF),
                                     (Uint8)(rgb & 0xFF), 255 };
            }
            m_tilecfg[line] = c;
        }
        fclose(fp);
    }
    void Menu::SaveTileCfg() {
        EnsureUserConfigDir();
        FILE *fp = fopen(GetUserConfigPath(kTileCfgFile).c_str(), "w");
        if (!fp) return;
        for (const auto &kv : m_tilecfg) {
            char col[8] = "-";
            if (kv.second.has_color)
                snprintf(col, sizeof(col), "%02x%02x%02x", kv.second.color.r,
                         kv.second.color.g, kv.second.color.b);
            fprintf(fp, "%s=%dx%d,%s\n", kv.first.c_str(),
                    kv.second.w, kv.second.h, col);
        }
        fclose(fp);
    }
    Menu::TileCfg &Menu::TileCfgFor(const std::string &key) {
        auto it = m_tilecfg.find(key);
        if (it != m_tilecfg.end()) return it->second;
        return m_tilecfg[key];   // default-constructed: 0x0 means "kind default"
    }
    // How many units an entry covers. The kind sets the default; a saved config
    // overrides it.
    void Menu::TileSpan(const MenuItem &it, int &w, int &h) const {
        w = 1; h = 1;
        if (it.kind == ItemKind::Album || it.kind == ItemKind::MusicPlayer) w = 2;
        if (it.kind == ItemKind::WidgetTile)                              { w = 2; h = 2; }
        const auto c = m_tilecfg.find(ItemKey(it));
        if (c != m_tilecfg.end() && c->second.w > 0) { w = c->second.w; h = c->second.h; }
        w = std::min(std::max(1, w), TileCols());
        h = std::min(std::max(1, h), TileRowsVis());
    }
    // Small -> Wide -> Large -> Small, the three shapes Windows 8 offered.
    void Menu::CycleTileSize(const std::string &key) {
        for (const auto &it : m_items) {
            if (ItemKey(it) != key) continue;
            int w, h; TileSpan(it, w, h);
            if      (w == 1 && h == 1) { w = 2; h = 1; }
            else if (w == 2 && h == 1) { w = 2; h = 2; }
            else                       { w = 1; h = 1; }
            TileCfg &c = TileCfgFor(key);
            c.w = w; c.h = h;
            SaveTileCfg();
            // The widget render targets are sized to the box they were made for.
            FreeWidgetTileTextures();
            return;
        }
    }
    const char *Menu::TileSizeLabel(const std::string &key) const {
        const auto c = m_tilecfg.find(key);
        int w = 0, h = 0;
        if (c != m_tilecfg.end()) { w = c->second.w; h = c->second.h; }
        if (w == 0) {
            for (const auto &it : m_items)
                if (ItemKey(it) == key) { TileSpan(it, w, h); break; }
        }
        if (w >= 2 && h >= 2) return "Large";
        if (w >= 2)           return "Wide";
        return "Small";
    }
    // Ease the drawn colour toward the configured one. The +/-1 nudge on top of
    // the proportional step is what guarantees it actually arrives: the
    // proportional part truncates to zero over the last few units.
    SDL_Color Menu::TileShownColor(const std::string &key, SDL_Color target) {
        auto it = m_tile_shown.find(key);
        if (it == m_tile_shown.end()) { m_tile_shown[key] = target; return target; }
        SDL_Color &c = it->second;
        auto ease = [](Uint8 &v, Uint8 t) {
            const int d = (int)t - (int)v;
            if (d == 0) return;
            if (std::abs(d) <= 2) { v = t; return; }
            v = (Uint8)((int)v + (int)(d * 0.22f) + (d > 0 ? 1 : -1));
        };
        ease(c.r, target.r); ease(c.g, target.g); ease(c.b, target.b);
        c.a = target.a;
        return c;
    }
    void Menu::FreeWidgetTileTextures() {
        if (m_tile_wscratch) { SDL_DestroyTexture(m_tile_wscratch); m_tile_wscratch = nullptr; }
    }
    int Menu::WidgetIndexByName(const std::string &n) {
        if (!m_deferred_joined) return -1;
        for (int i = 0; i < m_widgets.Count(); i++) {
            widgets::IWidget *w = m_widgets.At(i);
            if (w && w->Name() == n) return i;
        }
        return -1;
    }
    void Menu::AddWidgetTile(int widget_index) {
        widgets::IWidget *w = m_widgets.At(widget_index);
        if (!w) return;
        const std::string key = "w" + w->Name();
        TileCfg &c = TileCfgFor(key);
        if (c.w == 0) { c.w = 2; c.h = 2; }
        SaveTileCfg();
        // Deliberately NOT SetEnabled: that flag is the user's choice about the
        // floating home-screen widget, and flipping it here made a tile added to
        // the wall show up on every other layout too. The fetch thread ticks
        // tiled widgets on their own account.
        m_widgets.SetTiled(widget_index, true);
        RebuildItems();
        SelectByKey(key);
    }
    void Menu::RemoveWidgetTile(std::string name) {
        m_tilecfg.erase("w" + name);
        SaveTileCfg();
        const int i = WidgetIndexByName(name);
        if (i >= 0) m_widgets.SetTiled(i, false);
        FreeWidgetTileTextures();
        RebuildItems();
    }
    int Menu::TileRowOf(int item) const {
        std::vector<TileRect> tiles;
        BuildTiles(tiles);
        for (const TileRect &r : tiles)
            if (r.item == item) return (r.y - TileTop()) / TilePitch();
        return 0;
    }
    int Menu::TileRowCount() const {
        std::vector<TileRect> tiles;
        BuildTiles(tiles);
        // Max over every tile, not just the last one: the packer fills holes, so
        // the final entry in the list is not necessarily the lowest on the wall.
        int rows = 0;
        for (const TileRect &r : tiles)
            rows = std::max(rows, (r.y - TileTop()) / TilePitch() + TileRowsOf(r.h));
        return rows;
    }
    int Menu::TileFirstInRow(int row) const {
        std::vector<TileRect> tiles;
        BuildTiles(tiles);
        for (const TileRect &r : tiles)
            if ((r.y - TileTop()) / TilePitch() == row) return r.item;
        return m_cursor;
    }
    int Menu::TileMaxScroll() const {
        return std::max(0, TileRowCount() - TileRowsVis());
    }
    // The picture tile, cycling through the album. One image is held and the
    // next cross-fades in over it, so only two are ever decoded.
    void Menu::UpdateLiveAlbum() {
        if (!m_album_scanned) ScanAlbum();
        if (m_album.empty()) return;

        const u64 now = armGetSystemTick();
        const u64 hz  = armGetSystemTickFreq();
        if (m_tile_pic_tick == 0) m_tile_pic_tick = now;
        const u64 ms = (now - m_tile_pic_tick) * 1000 / hz;

        // Mid-fade: advance it, and promote once it completes.
        if (m_tile_pic_next) {
            m_tile_pic_fade = (float)ms / (float)kTileFadeMs;
            if (m_tile_pic_fade >= 1.0f) {
                if (m_tile_pic) m_gfx->FreeImage(m_tile_pic);
                m_tile_pic      = m_tile_pic_next;
                m_tile_pic_next = nullptr;
                m_tile_pic_fade = 1.0f;
                m_tile_pic_tick = now;
            }
            return;
        }

        // Time for the next picture. Newest first, which is what you want to see.
        if (m_tile_pic && ms < kTilePicMs) return;
        const int n = (int)m_album.size();
        const int next = (m_tile_pic_idx < 0) ? n - 1
                       : ((m_tile_pic_idx - 1) + n) % n;
        SDL_Texture *tex = m_gfx->LoadImageScaled(m_album[next].c_str(),
                                                  TileWideW(), TileUnit());
        if (!tex) { m_tile_pic_tick = now; return; }   // unreadable: try later
        m_tile_pic_idx = next;
        if (!m_tile_pic) {                 // first one: no fade to run
            m_tile_pic = tex;
            m_tile_pic_fade = 1.0f;
        } else {
            m_tile_pic_next = tex;
            m_tile_pic_fade = 0.0f;
        }
        m_tile_pic_tick = now;
    }
    // A widget tile. The widget draws through a render target the size of the
    // box, so one that reports a taller height than it was given is cropped by
    // the texture instead of spilling over its neighbours - the widget API
    // returns a height, it does not accept a limit.
    void Menu::DrawWidgetTile(const TileRect &r, const MenuItem &it, Uint8 a) {
        const int wi = WidgetIndexByName(it.name);
        if (wi < 0) return;
        widgets::IWidget *w = m_widgets.At(wi);
        SDL_Renderer *ren = m_gfx->Renderer();
        if (!w || !ren) return;

        // The widget always draws at the width it was written for, so its text
        // metrics stay the ones it was designed against and its own cached
        // texture is never reallocated between here and the home screen. What it
        // is asked for is a HEIGHT in that same space with the tile's aspect
        // ratio: a script that honours it lays its frame and content out to fill
        // exactly that, and the blit below then scales the whole thing into the
        // box with nothing left over. A script that ignores it returns its
        // natural height instead and gets scaled to fit, which is why an old
        // widget still works - it just does not fill the tile.
        if (!m_tile_wscratch) {
            m_tile_wscratch = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA8888,
                                                SDL_TEXTUREACCESS_TARGET,
                                                kTileWidgetW, gfx::Gfx::Height);
            if (!m_tile_wscratch) return;
            SDL_SetTextureBlendMode(m_tile_wscratch, SDL_BLENDMODE_BLEND);
        }

        // The wall draws inside a clip rect, and that clip is in the screen's
        // coordinates - left in place it would carve the same band out of this
        // texture, whose origin is its own corner. Dropped for the duration of
        // the widget's own drawing and put back before the blit, which is the
        // part that does need clipping.
        const SDL_bool clipped = SDL_RenderIsClipEnabled(ren);
        SDL_Rect saved{};
        SDL_RenderGetClipRect(ren, &saved);
        SDL_RenderSetClipRect(ren, nullptr);

        // The widget saves and restores the target around its own cache, so
        // nesting one inside ours is safe.
        SDL_Texture *prev = SDL_GetRenderTarget(ren);
        SDL_SetRenderTarget(ren, m_tile_wscratch);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 0);
        SDL_RenderClear(ren);
        int reqH = (r.w > 0) ? (int)lroundf((float)kTileWidgetW * (float)r.h / (float)r.w)
                             : 0;
        reqH = std::min(reqH, (int)gfx::Gfx::Height);
        const int natH = w->Render(m_gfx, m_theme.Current(), 0, 0, kTileWidgetW, reqH);
        SDL_SetRenderTarget(ren, prev);

        SDL_RenderSetClipRect(ren, clipped ? &saved : nullptr);
        if (natH <= 0) return;   // nothing rendered yet (first frames)

        // Fit rather than stretch: a calendar grid or a clock face pulled to a
        // different aspect ratio looks broken, so the widget is scaled as large
        // as fits and centred, and the tile colour fills what is left.
        const float s = std::min((float)r.w / (float)kTileWidgetW,
                                 (float)r.h / (float)natH);
        const int dw = std::max(1, (int)(kTileWidgetW * s));
        const int dh = std::max(1, (int)(natH * s));

        SDL_SetTextureAlphaMod(m_tile_wscratch, a);
        SDL_Rect src{ 0, 0, kTileWidgetW, natH };
        SDL_Rect dst{ r.x + (r.w - dw) / 2, r.y + (r.h - dh) / 2, dw, dh };
        SDL_RenderCopy(ren, m_tile_wscratch, &src, &dst);
    }
    // One tile face. Flat, no border and no shadow: the colour block and the
    // label in the bottom-left corner are the whole of Metro's vocabulary.
    void Menu::DrawTileFace(const TileRect &r, const MenuItem &it, bool sel, Uint8 a) {
        const Theme &t = m_theme.Current();
        const bool wide = (r.w > TileUnit());
        const std::string key = ItemKey(it);
        // Eased every frame, so a recolour arrives as a fade rather than a jump.
        const SDL_Color face = TileShownColor(key, TileColor(r.item));

        if (it.kind == ItemKind::WidgetTile) {
            m_gfx->FillRect(r.x, r.y, r.w, r.h, WithAlpha(face, a));
            DrawWidgetTile(r, it, a);
            if (sel) {
                const SDL_Color c = WithAlpha(t.title, a);
                m_gfx->FillRect(r.x - 3, r.y - 3, r.w + 6, 3, c);
                m_gfx->FillRect(r.x - 3, r.y + r.h, r.w + 6, 3, c);
                m_gfx->FillRect(r.x - 3, r.y - 3, 3, r.h + 6, c);
                m_gfx->FillRect(r.x + r.w, r.y - 3, 3, r.h + 6, c);
            }
            return;   // no label band: the widget is the content
        }

        if (it.kind == ItemKind::MusicPlayer) {
            // The music tile is always a colour block: it is showing text, and a
            // picture behind a track name would only make it harder to read.
            m_gfx->FillRect(r.x, r.y, r.w, r.h, WithAlpha(face, a));
            if (SDL_Texture *g = SystemIcon(it.kind))
                m_gfx->DrawImage(g, r.x + 14, r.y + 20, 52, 52, a);
            if (m_deferred_joined) {
                const int lh = m_gfx->LineHeight(FontSize::Small);
                m_gfx->Text(FontSize::Small, r.x + 78, r.y + 24, WithAlpha(t.fg, a),
                            m_music.Enabled() ? T("Playing") : T("Paused"));
                char n[48];
                snprintf(n, sizeof(n), "%d / %d", m_music.TrackIndex() + 1,
                         std::max(1, m_music.TrackCount()));
                m_gfx->Text(FontSize::Small, r.x + 78, r.y + 24 + lh + 4,
                            WithAlpha(t.dim, a), n);
            }
        } else if (it.kind == ItemKind::Album && m_tile_pic) {
            m_gfx->DrawImage(m_tile_pic, r.x, r.y, r.w, r.h, a);
            if (m_tile_pic_next)
                m_gfx->DrawImage(m_tile_pic_next, r.x, r.y, r.w, r.h,
                                 (Uint8)(a * m_tile_pic_fade));
        } else if (it.kind == ItemKind::Game || it.kind == ItemKind::Homebrew) {
            // A game's own icon fills the tile, which is what gives the wall its
            // colour - the same job Metro gave photo and people tiles.
            SDL_Texture *icon = (it.kind == ItemKind::Homebrew)
                                  ? m_hb_icons.Get(it.hb_icon)
                                  : m_icons.Get(it.app_id);
            // Picking a colour for a game turns its tile into the other kind of
            // Windows 8 tile: a solid block with the app's logo inset, rather
            // than artwork edge to edge. Without this the choice would be
            // invisible on a square tile, because the icon covers every pixel
            // of it - and a colour you cannot see is not worth choosing.
            const auto cfg = m_tilecfg.find(key);
            const bool tinted = (cfg != m_tilecfg.end() && cfg->second.has_color);
            const int  fit    = std::min(r.w, r.h);
            const int  sz     = tinted ? (fit * 5) / 8 : fit;
            if (!icon || tinted || sz != r.w || sz != r.h)
                m_gfx->FillRect(r.x, r.y, r.w, r.h, WithAlpha(face, a));
            if (icon)
                m_gfx->DrawImage(icon, r.x + (r.w - sz) / 2,
                                 r.y + (r.h - sz) / 2 - (tinted ? 8 : 0), sz, sz, a);
        } else {
            // System tiles are a solid block with the glyph centred.
            m_gfx->FillRect(r.x, r.y, r.w, r.h, WithAlpha(face, a));
            if (SDL_Texture *g = SystemIcon(it.kind)) {
                const int sz = wide ? 56 : 64;
                m_gfx->DrawImage(g, r.x + (r.w - sz) / 2,
                                 r.y + (r.h - sz) / 2 - (wide ? 10 : 8), sz, sz, a);
            }
        }

        // A band under the label so it stays readable over artwork.
        const int band = 30;
        m_gfx->FillRect(r.x, r.y + r.h - band, r.w, band, SDL_Color{0, 0, 0,
                        (Uint8)(a * 0.55f)});

        std::string label = it.name;
        // The music tile names the track rather than naming itself - the play
        // state is already spelled out on its face, so it is not repeated here.
        if (it.kind == ItemKind::MusicPlayer && m_deferred_joined) {
            const std::string now_playing = m_music.CurrentName();
            if (!now_playing.empty()) label = now_playing;
        }
        m_gfx->Text(FontSize::Small, r.x + 10,
                    r.y + r.h - band + (band - m_gfx->LineHeight(FontSize::Small)) / 2,
                    WithAlpha(t.fg, a),
                    Ellipsize(label, r.w - 20, FontSize::Small).c_str());

        // Selection is a ring, not a fill: Metro never dims the tile itself.
        if (sel) {
            const SDL_Color c = WithAlpha(t.title, a);
            m_gfx->FillRect(r.x - 3, r.y - 3, r.w + 6, 3, c);
            m_gfx->FillRect(r.x - 3, r.y + r.h, r.w + 6, 3, c);
            m_gfx->FillRect(r.x - 3, r.y - 3, 3, r.h + 6, c);
            m_gfx->FillRect(r.x + r.w, r.y - 3, 3, r.h + 6, c);
        }
    }
    void Menu::DrawMainGrid() {
        const Theme &t = m_theme.Current();
        m_icons.SetScale(gfx::IconCache::GridScale);
        DrawTopBar(nullptr);

        if (m_items.empty()) { DrawMainEmpty(); return; }

        UpdateLiveAlbum();

        std::vector<TileRect> tiles;
        BuildTiles(tiles);
        if (tiles.empty()) { DrawMainEmpty(); return; }

        // Scroll so the whole of the selected tile sits inside the visible band.
        int cur_row = 0, cur_rows = 1, rows = 0;
        for (const TileRect &r : tiles) {
            const int r0 = (r.y - TileTop()) / TilePitch();
            // Over every tile, not just the last one: the packer fills holes, so
            // the final entry is not necessarily the lowest thing on the wall.
            rows = std::max(rows, r0 + TileRowsOf(r.h));
            if (r.item == m_cursor) { cur_row = r0; cur_rows = TileRowsOf(r.h); }
        }

        const int maxScroll = std::max(0, rows - TileRowsVis());
        // One row of lead-in above the selection, except that a tall tile whose
        // bottom would fall off the band wins - showing half a tile is worse
        // than losing the lead-in.
        int target_i = std::min(std::max(0, cur_row - 1), maxScroll);
        target_i = std::min(std::max(target_i, cur_row + cur_rows - TileRowsVis()), maxScroll);
        const float target = (float)std::max(0, target_i);
        if (!ScrollBusy()) m_grid_scroll += (target - m_grid_scroll) * 0.30f;
        if (std::abs(target - m_grid_scroll) < 0.01f) m_grid_scroll = target;
        const int scrollPx = (int)lroundf(m_grid_scroll * TilePitch());

        const int bandTop = TileTop() - 4;
        const int bandBot = TileTop() + TileRowsVis() * TilePitch();

        // Clip to the band. The fade alone was enough while every tile was one
        // row high, but a two-row tile reaches a whole pitch further up than the
        // row it is fading with, and painted straight over the clock.
        SDL_Renderer *ren = m_gfx->Renderer();
        const SDL_Rect band{ 0, bandTop - 4, gfx::Gfx::Width,
                             (bandBot + 4) - (bandTop - 4) };
        if (ren) SDL_RenderSetClipRect(ren, &band);

        for (const TileRect &src : tiles) {
            TileRect r = src;
            r.y -= scrollPx;
            if (r.y + r.h < bandTop - TilePitch() || r.y > bandBot + TilePitch()) continue;

            // Fade rather than clip at the band edges, so a row scrolling away
            // does not cut off against nothing.
            //
            // Measured from the tile's LAST row going off the top and its FIRST
            // row going off the bottom, not from its outer edges: a two-row tile
            // has its top edge a whole pitch above its bottom row, so measuring
            // from the edge faded it to nothing while most of it was still on
            // screen - which read as a hole in the wall.
            const int over_top = bandTop - (r.y + r.h - TileUnit());
            const int over_bot = (r.y + TileUnit()) - bandBot;
            Uint8 a = 255;
            if (over_top > 0)      a = (Uint8)std::max(0, 255 - over_top * 5);
            else if (over_bot > 0) a = (Uint8)std::max(0, 255 - over_bot * 5);
            if (a == 0) continue;

            DrawTileFace(r, m_items[src.item], src.item == m_cursor, a);
        }
        if (ren) SDL_RenderSetClipRect(ren, nullptr);

        // The selected entry's name, in the corner Metro puts its page title.
        const MenuItem &sel = m_items[m_cursor];
        m_gfx->Text(FontSize::Large, TileLeft(), TileTop() - 58, t.title,
                    Ellipsize(sel.name, 900, FontSize::Large).c_str());

        if (sel.kind == ItemKind::MusicPlayer)
            DrawStatusHint({ {{"a"}, "Open"}, {{"y"}, "Play/pause"}, {{"x"}, "Options"} });
        else
            DrawStatusHint({ {{"a"}, "Select"}, {{"x"}, "Options"} });
    }
} // namespace sl::menu::ui
