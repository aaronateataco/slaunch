#pragma once
#include <switch.h>
#include <cstddef>
#include <string>
#include <vector>

// Box art from GameTDB (https://www.gametdb.com).
//
// What GameTDB holds that SteamGridDB does not is a scan of the actual retail
// case front, which is literally what a Flow or Deck box front is drawing - and
// it needs no API key, so it is the only source of box art on a console that has
// never been near Theming > SteamGridDB key. It is asked before SteamGridDB and
// a miss falls straight through to it, so nothing that worked before stops.
//
// ---- the catch: it is not addressed by title id ----------------------------
//
// GameTDB files Switch art under the five-character product code printed on the
// cartridge - HAC-P-AAB6B becomes AAB6B:
//
//   https://art.gametdb.com/switch/coverHQ/US/AAB6B.jpg
//
// Nothing on the console carries that code. NsApplicationControlData gives the
// menu the NACP and the icon, and there is no product number anywhere in the
// NACP; a digital-only title has no cartridge and so no code to begin with. It
// therefore has to be looked up, and until the menu carries GameTDB's own
// database to look it up in, the mapping is a file you write:
//
//   sdmc:/slaunch/config/gametdb_ids.txt
//     0100152120B54000=AAB6B      # Title Name
//
// which the menu can write for you: X on a game > GameTDB code. A title with no
// entry is skipped without a request. Guessing - asking for
// /0100152120B54000.jpg - would 404 on every title on the card, so the whole
// source stays dormant until at least one mapping exists. That also means this
// changes nothing at all for anyone who has not written that file.
//
// ---- art paths -------------------------------------------------------------
//
//   <base>/<type>/<region>/<game id>.<ext>
//
// <region> is a language, with English split by territory: EN is UK English, US
// is American, AU Australian. The rest are the console's own languages, Chinese
// split by script:
//
//   EN US AU JA FR DE ES IT NL PT RU KO ZHCN ZHTW
//
// Art is not uploaded in every language, so the list is tried in turn. Every
// part of the path is configurable, because GameTDB is nine months into a site
// rewrite and a layout change must not need a new build of the menu:
//
//   sdmc:/slaunch/config/gametdb.txt
//     enabled=1
//     base=https://art.gametdb.com/switch
//     types=coverHQ,cover
//     regions=auto
//     ext=jpg
//     timeout=10
//     max_tries=9
//
// Only the front-cover types are defaults. `coverfullHQ` is the whole wrap -
// back, spine and front in one landscape image - and the shelf scales a cover
// straight to 480x720 without preserving aspect (DecodeCoverSurface), so a wrap
// arrives visibly squashed. Using those needs a crop step that does not exist
// yet; see docs/GAMETDB.md.

namespace sl::menu::net::gametdb {

    struct Settings {
        bool enabled = true;
        // No trailing slash; one is stripped on load if it is typed anyway.
        std::string base = "https://art.gametdb.com/switch";
        // Front covers only, highest quality first. Types are the outer loop, so
        // a coverHQ in any language beats a small cover in the first one.
        std::vector<std::string> types { "coverHQ", "cover" };
        // Filled in by the loader, which defaults it to what this console reads
        // followed by the English territories. Left empty here because the
        // answer is not known until the system language has been asked.
        std::vector<std::string> regions;
        std::string ext = "jpg";
        long timeout_s  = 10;
        // A title with no art anywhere costs one request per type/region pair,
        // so the product of those two lists is bounded rather than trusted.
        int  max_tries  = 9;
    };

    // Parsed once per boot from config/gametdb.txt; the defaults above when the
    // file is absent.
    const Settings &Cfg();

    // Whether a fetch is worth attempting at all: wanted, configured with
    // enough of a path to build a URL from, and holding at least one id mapping.
    bool Enabled();

    // Just the user's own on/off choice, without the "and is it usable yet"
    // part. This is what the Theming row shows: a console that is switched on
    // but has no codes yet should read as on, not off.
    bool Wanted();

    // Flip the switch and persist it. Only the `enabled=` line of
    // config/gametdb.txt is rewritten, so the advanced keys - and any comments
    // someone put in that file - survive being toggled from the menu.
    void SetEnabled(bool on);

    // The GameTDB product code for a title, or "" when none is known.
    //
    // By value, not by reference: the map behind this is written by the menu
    // thread while the cover worker reads it, and a reference into it would
    // dangle the moment an insert rehashed.
    std::string IdFor(u64 title_id);

    // Set (or, with an empty code, clear) the product code for one title and
    // persist it to config/gametdb_ids.txt, leaving every other line alone.
    // Lower case is folded up, since the codes are upper case and the on-screen
    // keyboard starts lower.
    void SetCodeFor(u64 title_id, const std::string &code);

    // How many mappings config/gametdb_ids.txt holds.
    size_t IdCount();

    std::string CoverUrl(const std::string &game_id, const std::string &type,
                         const std::string &region);

    struct Result {
        bool ok = false;
        // No product code for this title, so nothing was requested. The ordinary
        // case rather than an error: it is every digital-only title, and
        // everything nobody has mapped yet.
        bool no_id = false;
        std::string url;        // the URL that answered, or the last one tried
        long http  = 0;         // status of that last request
        int  curl  = 0;         // its CURLcode, for telling a 404 from no route
        int  tries = 0;
        // Nothing reached the server. Walking the rest of the list would cost a
        // full timeout each to learn the same thing, so the caller stops.
        bool unreachable = false;
    };

    // Download the first cover that exists into `path`, which is left alone
    // unless one does. Blocking: call it from the cover worker, not a frame.
    Result FetchCover(u64 title_id, const char *path);

} // namespace sl::menu::net::gametdb
