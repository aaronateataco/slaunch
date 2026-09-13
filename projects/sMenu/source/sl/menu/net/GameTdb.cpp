#include <sl/menu/net/GameTdb.hpp>
#include <sl/menu/net/Http.hpp>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

                // The separator search stops at the first of several characters,
                // so "0100...=  AAB6B" leaves leading spaces on rhs; Trim above
                // has already taken them off. Anything still carrying a space is
                // two fields, not one id.
                if (rhs.find_first_of(" \t") != std::string::npos) continue;

                m[tid] = rhs;
            }
            fclose(fp);
            return m;
        }

        const std::unordered_map<u64, std::string> &Ids() {
            static const std::unordered_map<u64, std::string> m = LoadIds();
            return m;
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
        // the same thing. Editing the file takes effect on the next launch.
        static const Settings s = Load();
        return s;
    }

    size_t IdCount() { return Ids().size(); }

    const std::string &IdFor(u64 title_id) {
        static const std::string none;
        const std::unordered_map<u64, std::string> &m = Ids();
        const std::unordered_map<u64, std::string>::const_iterator it = m.find(title_id);
        return it == m.end() ? none : it->second;
    }

    bool Enabled() {
        const Settings &s = Cfg();
        // No mappings means every title would be skipped anyway, so the source
        // reports itself off rather than being asked once per title. This is
        // also what keeps the feature inert for anyone who has not opted in by
        // writing the ids file.
        return s.enabled && !s.base.empty() && !s.ext.empty() &&
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

        const std::string &game_id = IdFor(title_id);
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
