#include <sl/menu/net/GameTdb.hpp>
#include <sl/menu/net/Http.hpp>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

namespace sl::menu::net::gametdb {

    namespace {
        constexpr const char *kConfigPath = "sdmc:/slaunch/config/gametdb.txt";
        constexpr const char *kIdsPath    = "sdmc:/slaunch/config/gametdb_ids.txt";

        // Trim space, tab, CR and LF off both ends, so a value typed with a
        // stray newline still works - the same courtesy the SteamGridDB key
        // gets when it is read off the card.
        std::string Trim(const std::string &s) {
            size_t b = 0, e = s.size();
            while (b < e && (s[b] == ' ' || s[b] == '\t' ||
                             s[b] == '\r' || s[b] == '\n')) b++;
            while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' ||
                             s[e - 1] == '\r' || s[e - 1] == '\n')) e--;
            return s.substr(b, e - b);
        }

        // Split "EN, US, AU" on commas. Empty pieces are dropped, so a trailing
        // comma cannot turn into a request for <base>/<type>//<id>.jpg.
        std::vector<std::string> SplitList(const std::string &v) {
            std::vector<std::string> out;
            size_t i = 0;
            for (;;) {
                const size_t c = v.find(',', i);
                const size_t end = (c == std::string::npos) ? v.size() : c;
                const std::string piece = Trim(v.substr(i, end - i));
                if (!piece.empty()) out.push_back(piece);
                if (c == std::string::npos) break;
                i = c + 1;
            }
            return out;
        }

        // The default language list for this console.
        //
        // English is the case that needs care: GameTDB splits it by territory,
        // where EN is the UK edition, US the American one and AU the Australian.
        // A cover in the wrong English is still the right game in the right
        // language, so all three are tried - just in the order this console's
        // own setting implies, since en-GB and en-US are distinguishable and the
        // local edition is the one whose ratings flash and cover text match.
        //
        // Every other language gets its own folder first and then the English
        // ones, which is where the art actually is when nobody localised it.
        // Deliberately not all fourteen folders: a title with no scan anywhere
        // is the ordinary case, and probing ten more to establish that would
        // spend ten requests per title.
        std::vector<std::string> AutoRegions() {
            u64 lc = 0;
            char buf[9] = {};
            if (R_SUCCEEDED(setGetSystemLanguage(&lc))) memcpy(buf, &lc, 8);

            char lang[3] = {};
            for (int i = 0; i < 2 && buf[i] && buf[i] != '-'; i++)
                lang[i] = (char)('a' <= buf[i] && buf[i] <= 'z'
                                 ? buf[i] : buf[i] + ('a' - 'A'));

            // English, or a language GameTDB has no folder for.
            if (strcmp(lang, "en") == 0 || !lang[0] || !lang[1]) {
                // UK first only when the console actually says so; en-US and a
                // console that did not answer both start American, which is the
                // larger of the two catalogues.
                if (strstr(buf, "GB") || strstr(buf, "UK"))
                    return { "EN", "US", "AU" };
                return { "US", "EN", "AU" };
            }

            std::string mine;
            if (strcmp(lang, "zh") == 0) {
                // Traditional is spelled several ways across firmwares
                // (zh-Hant, zh-TW); anything else Chinese is Simplified.
                mine = (strstr(buf, "Hant") || strstr(buf, "TW")) ? "ZHTW" : "ZHCN";
            } else {
                static const char *kKnown[] = {
                    "ja", "fr", "de", "es", "it", "nl", "pt", "ru", "ko"
                };
                for (const char *k : kKnown) {
                    if (strcmp(lang, k) == 0) {
                        const char up[3] = { (char)(lang[0] - ('a' - 'A')),
                                             (char)(lang[1] - ('a' - 'A')), 0 };
                        mine = up;
                        break;
                    }
                }
            }
            if (mine.empty()) return { "US", "EN", "AU" };
            return { mine, "US", "EN" };
        }

        Settings Load() {
            Settings s;
            bool regions_set = false;

            FILE *fp = fopen(kConfigPath, "r");
            if (!fp) {                      // no file: the defaults
                s.regions = AutoRegions();
                return s;
            }

            char line[512];
            while (fgets(line, sizeof(line), fp)) {
                if (line[0] == '#' || line[0] == ';') continue;
                char *eq = strchr(line, '=');
                if (!eq) continue;
                *eq = 0;
                const std::string key = Trim(line);
                const std::string val = Trim(eq + 1);
                // An empty value is a typo, not an instruction to clear the
                // field: silently emptying `regions` would turn the feature off
                // with nothing to show why.
                if (key.empty() || val.empty()) continue;

                if (key == "enabled") {
                    s.enabled = (atoi(val.c_str()) != 0);
                } else if (key == "base") {
                    s.base = val;
                } else if (key == "types" || key == "type") {
                    const std::vector<std::string> l = SplitList(val);
                    if (!l.empty()) s.types = l;
                } else if (key == "regions" || key == "region") {
                    // "auto" is the default spelled out, so it can be written
                    // explicitly and a list reverted without deleting the line.
                    if (val == "auto") {
                        regions_set = false;
                    } else {
                        const std::vector<std::string> l = SplitList(val);
                        if (!l.empty()) { s.regions = l; regions_set = true; }
                    }
                } else if (key == "ext") {
                    s.ext = val;
                } else if (key == "timeout") {
                    const long t = atol(val.c_str());
                    if (t > 0) s.timeout_s = t;
                } else if (key == "max_tries") {
                    const int t = atoi(val.c_str());
                    if (t > 0) s.max_tries = t;
                }
            }
            fclose(fp);

            if (!regions_set || s.regions.empty()) s.regions = AutoRegions();

            // Tolerate the two ways a hand-typed path goes wrong: a trailing
            // slash on the base and a leading dot on the extension. Both would
            // otherwise produce a URL that 404s for every title.
            while (!s.base.empty() && s.base[s.base.size() - 1] == '/')
                s.base.erase(s.base.size() - 1);
            while (!s.ext.empty() && s.ext[0] == '.')
                s.ext.erase(0, 1);

            return s;
        }

        // title id -> GameTDB product code, from config/gametdb_ids.txt.
        //
        //   0100152120B54000=AAB6B      # anything after a # is a note
        //
        // A space or a colon separates just as well as '=', because this is a
        // list people will paste together from several places.
        std::unordered_map<u64, std::string> LoadIds() {
            std::unordered_map<u64, std::string> m;
            FILE *fp = fopen(kIdsPath, "r");
            if (!fp) return m;

            char line[256];
            while (fgets(line, sizeof(line), fp)) {
                if (char *hash = strchr(line, '#')) *hash = 0;
                if (char *semi = strchr(line, ';')) *semi = 0;

                std::string l = Trim(line);
                if (l.empty()) continue;

                const size_t sep = l.find_first_of("=: \t");
                if (sep == std::string::npos) continue;

                const std::string lhs = Trim(l.substr(0, sep));
                // Skip the whole run of separator characters, not just the one
                // that was found: "<id> = AAB6B" and "<id> : AAB6B" are both
                // what this ends up looking like once it has been pasted
                // together by hand, and taking only the first would leave the
                // '=' on the front of the code.
                const size_t val = l.find_first_not_of("=: \t", sep);
                if (val == std::string::npos) continue;
                const std::string rhs = Trim(l.substr(val));
                if (lhs.empty() || rhs.empty()) continue;

                // strtoull rather than sscanf: it reports where it stopped, so a
                // line whose left side is not actually a title id is dropped
                // instead of quietly mapping title 0.
                char *endp = nullptr;
                const u64 tid = (u64)strtoull(lhs.c_str(), &endp, 16);
                if (tid == 0 || !endp || *endp != '\0') continue;

                // Anything still carrying a space is two fields, not one code.
                if (rhs.find_first_of(" \t") != std::string::npos) continue;

                m[tid] = rhs;
            }
            fclose(fp);
            return m;
        }

        // The id map is read by the cover worker and written by the menu thread
        // (X on a game > GameTDB code), so every touch of it goes behind this. A
        // zero-initialised libnx Mutex is a valid unlocked one, which is why
        // there is no init call here to forget.
        Mutex g_ids_mx;

        std::unordered_map<u64, std::string> &Ids() {
            static std::unordered_map<u64, std::string> m = LoadIds();
            return m;
        }

        // Rewrite one config file, replacing the single line whose key matches -
        // appending it when there is none, dropping it when `drop` - and leaving
        // every other line, comments included, exactly as it was. Both config
        // files are edited this way, because a toggle or a code typed in the menu
        // must not throw away hand-written settings.
        //
        // With `numeric_key` the key is compared as a hex number, so a title id
        // written in lower case, or with fewer leading zeros, still matches the
        // line it should.
        void RewriteLine(const char *path, const std::string &key,
                         const std::string &replacement, bool drop,
                         bool numeric_key) {
            mkdir("sdmc:/slaunch", 0777);
            mkdir("sdmc:/slaunch/config", 0777);

            std::vector<std::string> lines;
            bool found = false;

            if (FILE *fp = fopen(path, "r")) {
                char buf[512];
                while (fgets(buf, sizeof(buf), fp)) {
                    std::string l(buf);
                    while (!l.empty() && (l[l.size() - 1] == '\n' ||
                                          l[l.size() - 1] == '\r'))
                        l.erase(l.size() - 1);

                    const bool comment = !l.empty() && (l[0] == '#' || l[0] == ';');
                    if (!comment) {
                        const size_t sep = l.find_first_of("=: \t");
                        if (sep != std::string::npos) {
                            const std::string lhs = Trim(l.substr(0, sep));
                            bool hit;
                            if (numeric_key) {
                                char *e = nullptr;
                                const u64 a = (u64)strtoull(lhs.c_str(), &e, 16);
                                hit = e && *e == '\0' &&
                                      a == (u64)strtoull(key.c_str(), nullptr, 16);
                            } else {
                                hit = (lhs == key);
                            }
                            if (hit) {
                                found = true;
                                if (drop) continue;     // clearing: drop the line
                                l = replacement;
                            }
                        }
                    }
                    lines.push_back(l);
                }
                fclose(fp);
            }
            if (!found && !drop) lines.push_back(replacement);

            if (FILE *fp = fopen(path, "w")) {
                for (size_t i = 0; i < lines.size(); i++)
                    fprintf(fp, "%s\n", lines[i].c_str());
                fclose(fp);
            }
        }

        // Is this file actually an image?
        //
        // A 200 carrying an HTML "no such cover" page is the failure mode that
        // matters here: it would be saved as <title id>.jpg, fail to decode, and
        // be remembered as a title with no art for the rest of the session - a
        // permanent miss from a transient server mood. Sniffing the magic bytes
        // costs one 12-byte read and turns that into an ordinary miss that falls
        // through to the next candidate.
        bool LooksLikeImage(const char *path) {
            FILE *fp = fopen(path, "rb");
            if (!fp) return false;
            unsigned char h[12] = {};
            const size_t n = fread(h, 1, sizeof(h), fp);
            fclose(fp);
            if (n < 12) return false;           // nothing this small is a cover

            if (h[0] == 0xFF && h[1] == 0xD8 && h[2] == 0xFF) return true;   // JPEG
            if (h[0] == 0x89 && h[1] == 'P' && h[2] == 'N' && h[3] == 'G')   // PNG
                return true;
            if (h[0] == 'G' && h[1] == 'I' && h[2] == 'F') return true;      // GIF
            if (h[0] == 'B' && h[1] == 'M') return true;                     // BMP
            if (memcmp(h, "RIFF", 4) == 0 && memcmp(h + 8, "WEBP", 4) == 0)  // WebP
                return true;
            return false;
        }
    }

    const Settings &Cfg() {
        // Read once per boot. The fetch runs on a worker for whatever the cursor
        // rests on, and rereading here would mean an SD open per title to learn
        // the same thing. Editing the file takes effect on the next launch; the
        // one setting the menu itself can change, `enabled`, is held live below.
        static const Settings s = Load();
        return s;
    }

    namespace {
        // The live on/off value, seeded from the config file exactly once. A
        // function-local static, so that seeding is thread-safe: Enabled() runs
        // on the cover worker and SetEnabled() on the menu thread.
        bool &WantedRef() {
            static bool w = Cfg().enabled;
            return w;
        }
    }

    bool Wanted() { return WantedRef(); }

    void SetEnabled(bool on) {
        WantedRef() = on;
        RewriteLine(kConfigPath, "enabled", on ? "enabled=1" : "enabled=0",
                    false, false);
    }

    size_t IdCount() {
        mutexLock(&g_ids_mx);
        const size_t n = Ids().size();
        mutexUnlock(&g_ids_mx);
        return n;
    }

    std::string IdFor(u64 title_id) {
        mutexLock(&g_ids_mx);
        const std::unordered_map<u64, std::string> &m = Ids();
        const std::unordered_map<u64, std::string>::const_iterator it = m.find(title_id);
        const std::string code = (it == m.end()) ? std::string() : it->second;
        mutexUnlock(&g_ids_mx);
        return code;
    }

    void SetCodeFor(u64 title_id, const std::string &code) {
        std::string c = Trim(code);
        for (size_t i = 0; i < c.size(); i++)
            if ('a' <= c[i] && c[i] <= 'z') c[i] = (char)(c[i] - ('a' - 'A'));

        mutexLock(&g_ids_mx);
        std::unordered_map<u64, std::string> &m = Ids();
        if (c.empty()) m.erase(title_id);
        else           m[title_id] = c;
        mutexUnlock(&g_ids_mx);

        // Keyed by the title id as 16 upper-case hex, the way the menu names
        // every other per-title file.
        char key[24];
        snprintf(key, sizeof(key), "%016llX", (unsigned long long)title_id);
        RewriteLine(kIdsPath, key, std::string(key) + "=" + c, c.empty(), true);
    }

    bool Enabled() {
        const Settings &s = Cfg();
        // No mappings means every title would be skipped anyway, so the source
        // reports itself off rather than being asked once per title. This is
        // also what keeps the feature inert for anyone who has not opted in.
        return Wanted() && !s.base.empty() && !s.ext.empty() &&
               !s.types.empty() && !s.regions.empty() && IdCount() > 0;
    }

    std::string CoverUrl(const std::string &game_id, const std::string &type,
                         const std::string &region) {
        const Settings &s = Cfg();
        return s.base + "/" + type + "/" + region + "/" + game_id + "." + s.ext;
    }

    Result FetchCover(u64 title_id, const char *path) {
        Result r;
        if (!path || !Enabled()) return r;

        const std::string game_id = IdFor(title_id);
        if (game_id.empty()) {          // nothing to ask for
            r.no_id = true;
            return r;
        }

        const Settings &s = Cfg();
        for (size_t t = 0; t < s.types.size(); t++) {
            for (size_t g = 0; g < s.regions.size(); g++) {
                if (r.tries >= s.max_tries) return r;
                r.tries++;
                r.url  = CoverUrl(game_id, s.types[t], s.regions[g]);
                r.http = 0;
                r.curl = 0;
                // Download rather than a HEAD first: the overwhelmingly common
                // case is that the art is there, and asking twice for it would
                // double the cost of every hit to save nothing on a miss, which
                // leaves no file behind either way.
                if (net::Download(r.url.c_str(), path, s.timeout_s,
                                  &r.http, &r.curl)) {
                    if (LooksLikeImage(path)) {
                        r.ok = true;
                        return r;
                    }
                    // Answered, but not with a picture. Clear it out so the
                    // caller cannot mistake it for art, and keep looking.
                    remove(path);
                    continue;
                }
                // A 404 is "not in that language" and the next one is worth
                // asking. A request that never got a status never reached the
                // server, and the rest of the list would each cost a full
                // timeout to report the same thing.
                if (r.http == 0) {
                    r.unreachable = true;
                    return r;
                }
            }
        }
        return r;
    }

} // namespace sl::menu::net::gametdb
