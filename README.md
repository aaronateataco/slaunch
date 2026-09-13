
# sLaunch

A fast, clean, **SDL2-based HOME Menu replacement** for the Nintendo Switch
(Atmosphere CFW). Themes with wallpapers, custom fonts, Lua widgets, icon packs,
translations, and lots of UI modes.

[Discord](https://discord.gg/dv28MgtaNn)

<img width="640" height="360" alt="2026081514233500-A082AE4E5DA891D87084ACEACFDFF4A9" src="https://github.com/user-attachments/assets/60f9b2ec-b202-4646-9ff6-8addbf3f8498" />
<img width="640" height="360" alt="2026081514223900-A082AE4E5DA891D87084ACEACFDFF4A9" src="https://github.com/user-attachments/assets/2d65d40b-23a2-40d9-921f-2de83a317440" />
<img width="640" height="360" alt="2026081701534700-A082AE4E5DA891D87084ACEACFDFF4A9" src="https://github.com/user-attachments/assets/07c247c4-70d0-45fa-86c7-d4f151f8de20" />
<img width="640" height="360" alt="2026081514234500-A082AE4E5DA891D87084ACEACFDFF4A9" src="https://github.com/user-attachments/assets/759afbcb-6e1e-4b69-ac33-3649acd45737" />

## Architecture

sLaunch follows the same split as [uLaunch](https://github.com/Xortroll/uLaunch):
a privileged daemon that *is* the HOME Menu, plus a graphical applet that
renders the UI. A system applet cannot create an SDL/GPU window, so the UI has
to live in a library-applet slot.

```
sSystem   (libstratosphere sysmodule, runs as qlaunch / program 0100000000001000)
  |- launches/suspends/terminates games (libnx applet API)
  |- ECS: registers sMenu's SD folder as external code for an applet slot and
  |       serves it over a libstratosphere fs server (ldr:shel cmd 65000)
  \- SMI: talks to the menu over the library-applet in/out-data channel

sMenu     (SDL2 library applet, served into the shop applet slot via ECS)
  |- renders the menu (SDL2 + SDL2_ttf + SDL2_image), system font via pl
  |- themes (5 built-in + custom), fonts, locales, OOBE, persisted to the SD
  \- asks sSystem (over SMI) to launch games / open system applets

hbloader  (fork of nx-hbloader, served into an applet or donor-game slot)
  |- loads a specific .nro named by slaunch/hbtarget.txt, then exits back to
  |  the menu instead of reloading itself (see projects/hbloader/README.md)
  \- can hand a chainload back to sSystem so it runs with full RAM (opt-in)
```

SD card layout produced by the build:

```
atmosphere/contents/0100000000001000/exefs/{main,main.npdm}   sSystem daemon (qlaunch)
slaunch/bin/sMenu/{main,main.npdm}                            sMenu applet (ECS exefs)
slaunch/bin/hbloader/                                         homebrew loader, applet mode
slaunch/bin/hbloader_app/                                     homebrew loader, full-RAM title mode
slaunch/fonts/                                                bundled fonts (incl. Noto Sans CJK)
slaunch/icons/                                                built-in system icons
slaunch/icon_packs/<pack>/                                    icon packs (bundled and user-made)
slaunch/lang/                                                 translations + template.txt
slaunch/music/                                                background music (mp3/ogg/flac)
slaunch/sounds/                                               UI sound effects
slaunch/widgets/                                              Lua home-screen widgets
slaunch/themes/                                               user wallpapers (.jpg/.png)
slaunch/config/                                               console settings, saved at runtime
slaunch/config/users/<account id>/                            each account's own settings
```

### Translations

The menu is fully translatable. `slaunch/lang/template.txt` lists every string
it can show; copy it to `<code>.txt` (e.g. `fr.txt`, `pt-BR.txt`) in the same
folder and translate the right-hand side of each `=`. The console's system
language picks the file, trying the regional form first and then the two-letter
one. Missing strings, and missing files, fall back to English.

Shipped: `ru`, `ja`, `de`, `es`, `zh` (Simplified).

Non-Latin scripts need a font with those glyphs. sLaunch selects the console's
own shared font from the system language, so Japanese, Korean and Chinese render
correctly with nothing installed; a full Noto Sans CJK is also bundled and
selectable under **Theming > Fonts** for reading names in other scripts.

### Widgets

Lua widgets drawn on the home screen and draggable with touch. See
[docs/WIDGETS.md](docs/WIDGETS.md).

### Per-account settings

Everything you choose belongs to the Switch account that chose it. Theme,
custom themes, font, icon pack, UI mode, layout tuning, tile sizes and colours,
favourites, manual order, renamed entries, hidden system entries, pinned
homebrew, widget placement and music all live in

```
slaunch/config/users/<account id>/
```

so two people sharing a console get two different menus. The folder is named
after the account's 32-hex-digit id, with a `name.txt` inside holding the
nickname - that is how you tell them apart from a PC.

Content is still shared, because it is content: `themes/`, `icon_packs/`,
`fonts/`, `music/`, `widgets/`, `covers/`, `lang/`. So is anything that
describes the console rather than a person - the SteamGridDB key, the homebrew
donor title, `takeover.txt` - which stays directly in `slaunch/config/`.

Updating from an earlier version loses nothing: the first account to open the
menu inherits the old `slaunch/config/` files, including its first-run setup, so
that account sees exactly what it saw before. The originals are copied rather
than moved, so downgrading still finds them. Any other account starts on the
defaults and goes through first-run setup once, as a new account should.

### Deck layout

**Theming > UI mode > Deck** is the SteamOS gamepad layout. The game you last
played gets a wide tile; the rest of the library follows it as upright tiles of
the same height, and under both is a row of cards:

| Tab | What it shows |
|---|---|
| What's new | Steam's news for the selected game, matched by name |
| Nintendo | the front page of nintendo.com/us/whatsnew |
| Widgets | your Lua widgets, one per card |

`A` on a card opens the story in a reader, `Y` opens the whole library as a grid
with tabs (all / favourites / recent / game card / homebrew), and `-` (or the
**HOME** button) opens a side menu holding everything that is not a game -
Theming, homebrew, Album, music, network, power.

Both feeds are keyless and cached on the card (`slaunch/cache/news/`), so the
menu opens on the last set of stories rather than waiting for the network -
Nintendo is refetched after six hours, a game's Steam news after a day. Box art
is the same fetch Flow uses (see [Box art](#box-art)); the wide tile's hero art
comes only from SteamGridDB, so **Theming > SteamGridDB key** is worth setting
for this layout in particular.

### Homebrew that launches homebrew

An .nro handing over to another one (hbmenu opening something, an installer
restarting you into what it just installed) works the way it always has:
`envSetNextLoad`, and hbloader loads the next .nro in the same process.

What that cannot do is change the terms it runs under. Homebrew started from
the Homebrew menu lives in an applet slot with a small heap, and no process can
promote itself to a full-RAM application - that means serving hbloader into a
donor game's slot, which only the daemon can do. So the daemon watches a drop
box:

```
sdmc:/slaunch/hb_queue/<name>.req      mode=app | nro=... | argv=... | donor=...
```

Any homebrew can write one with plain stdio (format and a ready-made writer:
`libs/sCommon/include/sl/sys/HbLaunchRequest.hpp`). The daemon takes it at the
moment the homebrew that queued it exits - never while it is still on screen -
and chains straight into the next launch instead of bouncing through the menu.
`donor=` may be left out, in which case the donor set in **Homebrew > Set
donor** is used; if none is set, it runs as an applet and says so in
`daemon.log`. Requests are one-shot and deleted as they are taken, valid or
not, so a bad one costs a launch rather than a boot loop, and the path must be
an .nro that exists on the card.

To get this for homebrew that knows nothing about sLaunch, create

```
sdmc:/slaunch/config/hb_chain_app.txt     containing: 1
```

and hbloader will hand *every* chainload out of an applet slot to the daemon,
so what hbmenu opens runs with full RAM. It is opt-in because it changes where
those launches happen: the homebrew you chainloaded from is gone (you return to
sLaunch rather than to it), and starting a donor title takes longer than
loading an .nro in place.

### Box art

The layouts that draw a case front - Flow, Deck and Cover - fetch box art for
the title the cursor is resting on, one at a time, into `slaunch/covers/`. Two
sources, asked in that order:

| Source | Addressed by | Key | Holds |
|---|---|---|---|
| [GameTDB](https://www.gametdb.com) | cartridge product code | none | scans of the retail case front |
| SteamGridDB | game name | **Theming > SteamGridDB key** | digital 600x900 grids, and Deck's wide hero art |

GameTDB goes first because it is keyless and its covers are real case scans - so
box art can work on a console with no key set at all. A miss falls straight
through to SteamGridDB, which is still what fetches Deck's hero tile and still
worth a key.

The catch is the product code: GameTDB files Switch art under the five characters
printed on the cartridge (`HAC-P-AAB6B` -> `AAB6B`), and nothing in a title's
NACP carries that, so it cannot be derived on the console. Until the menu can
consult GameTDB's own database, the mapping is a file you write -
`slaunch/config/gametdb_ids.txt`, one `<title id>=<code>` per line. Titles with
no entry are skipped without a request, and with no mappings at all GameTDB stays
dormant, so nothing changes until you opt in.

Covers are filed by language, with English split into UK, US and Australian
editions; the console's own language decides the order. That and everything else
about the paths lives in `slaunch/config/gametdb.txt`; see
[docs/GAMETDB.md](docs/GAMETDB.md).

A cover is fetched once. Drop your own `slaunch/covers/<title id>.jpg` in and
nothing overwrites it; delete one you dislike and the next visit refetches it.

### Content filter

Off unless you ask for it. With

```
sdmc:/slaunch/config/content_filter.txt    containing: enabled=1
```

the covers and news the menu fetches are checked for adult and extreme-violence
content before anything is downloaded or shown - by keyword, by ESRB/PEGI/USK
rating and by Steam store tags. It filters what sLaunch pulls off the internet;
it is not a parental control over what is installed on the console. See
[docs/CONTENT_FILTER.md](docs/CONTENT_FILTER.md).

## Building

Requires devkitPro (devkitA64 + libnx + the switch SDL2 stack) and a built
**libstratosphere** (Atmosphere 1.11.2). On Linux/WSL2:

```sh
# one-time on Arch: devkitPro ships its own repos, signed with their key
sudo pacman-key --recv-keys BC26F752D25B92CE272E0F44F7FD5492264BB9D0 --keyserver keyserver.ubuntu.com
sudo pacman-key --lsign-key BC26F752D25B92CE272E0F44F7FD5492264BB9D0
sudo pacman -U https://pkg.devkitpro.org/devkitpro-keyring.pkg.tar.xz
# add to /etc/pacman.conf:
#   [dkp-libs]
#   Server = https://pkg.devkitpro.org/packages
#   [dkp-linux]
#   Server = https://pkg.devkitpro.org/packages/linux/$arch
sudo pacman -Sy switch-dev switch-sdl2 switch-sdl2_ttf switch-sdl2_image \
               switch-sdl2_mixer switch-curl switch-mbedtls
# devkitPro's profile script exports DEVKITPRO but not the compiler's own bin
# dir, which the Makefiles need on PATH:
#   export DEVKITA64=$DEVKITPRO/devkitA64; export PATH=$DEVKITA64/bin:$PATH

export DEVKITPRO=/opt/devkitpro
# one-time: build libstratosphere into $DEVKITPRO/AtmosphereLibs
git clone --depth=1 --branch 1.11.2 https://github.com/Atmosphere-NX/Atmosphere /opt/atmosphere
make -C /opt/atmosphere/libraries/libstratosphere -j$(nproc)
mkdir -p $DEVKITPRO/AtmosphereLibs/include $DEVKITPRO/AtmosphereLibs/lib
cp -r /opt/atmosphere/libraries/libstratosphere/include/. $DEVKITPRO/AtmosphereLibs/include/
cp -r /opt/atmosphere/libraries/libvapours/include/.      $DEVKITPRO/AtmosphereLibs/include/
find /opt/atmosphere/libraries/libstratosphere -name '*.a' -exec cp {} $DEVKITPRO/AtmosphereLibs/lib/ \;

make            # builds everything into SdOut/
```

Individual targets: `make ssystem`, `make smenu`, `make sinstaller`,
`make hbloader`, `make assets`. `make package` zips `SdOut/`.

`make` produces the full SD layout under `SdOut/`; copy it to your SD card.
The daemon Makefile expects the Atmosphere checkout at `/opt/atmosphere`
(override with `ATMOSPHERE_DIR=`).

Eject the card properly before removing it. A half-written NSO on the qlaunch
slot crash-loops the console on boot.

## Status & diagnostics

The daemon and applet each write a small bring-up log to the SD card, which
makes hardware issues diagnosable without a debugger:

- `slaunch/daemon.log` - daemon boot + ECS register/launch results, power path
- `slaunch/ecs.log`    - ECS filesystem-server thread status
- `slaunch/boot.log`   - sMenu applet: `main enter` / `gfx.Init OK|FAILED`

If the menu doesn't appear, those logs (plus `atmosphere/crash_reports/` and
`atmosphere/fatal_reports/`) point at the exact stage. A crash report names the
faulting module and offset; `aarch64-none-elf-nm -SCn` on the matching
`build/*.elf` turns that offset into a function.

**Recovery:** delete `atmosphere/contents/0100000000001000/` from the SD to
return to the stock HOME Menu.

## Firmware

Developed and tested on 18.x; 9.0.0 and up is the range it tries to support.

The menu is not a program of its own - it is served into a library-applet slot,
and *which program a slot launches* is decided by a table inside `am` that is
not the same on every firmware. The shop slot (the one sLaunch takes over,
because web applets get the largest applet heap) launches `010000000000100B`
on older firmware and `0100000000001042` on newer. There is no way to read that
table, so the daemon registers its exefs for **both** ids and lets the firmware
launch whichever one it believes in.

If the menu still never appears, the daemon notices - a slot that opens and
closes without the menu ever saying a word twice in a row means the firmware
launched its own applet - and falls through to the `offlineWeb` slot, writing
every step to `slaunch/daemon.log`. To pin a slot by hand, put a program id in
`slaunch/config/takeover.txt`:

```
0x010000000000100B
```

The daemon then tries that slot first, and only drops back to the built-in
list if it never brings the menu up. Taking over a slot costs you whatever that
applet did, so prefer one the menu has no entry for.

## Credits

- The daemon's ECS content-serving, the sysmodule structure, the qlaunch/applet
  NPDMs, and the applet/daemon model are adapted from
  **[uLaunch](https://github.com/Xortroll/uLaunch)** by Xortroll & contributors
  (GPLv2). The bundled homebrew loader is a fork of
  **[nx-hbloader](https://github.com/switchbrew/nx-hbloader)** (ISC).
- The `Minimal` icon pack is by
  **[MeepCat55](https://github.com/meepcat55)**.
- Button prompt icons are from **Xelu's Free Controller and Key Prompts**
  (CC0) - see `assets/icons/buttons/ATTRIBUTION.md`.
- Bundled fonts are SIL OFL / Apache licensed - see `assets/fonts/ATTRIBUTION.md`.
