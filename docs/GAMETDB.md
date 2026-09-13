# GameTDB box art

sLaunch fetches box art for the layouts that draw a case front - Flow, Deck and
Cover - and [GameTDB](https://www.gametdb.com) is the first place it asks.

## What it adds

GameTDB's covers are **scans of the actual retail case front**, which is
literally what a Flow shelf is drawing, and it needs **no API key** - so it is
the only source of box art on a console that has never been near
**Theming > SteamGridDB key**.

It is asked *first*, not instead. A miss falls straight through to the
SteamGridDB fetch exactly as before, and SteamGridDB remains the only source of
Deck's wide hero art.

## The catch: art is not addressed by title id

GameTDB files Switch art under the **five-character product code printed on the
cartridge** - `HAC-P-AAB6B` becomes `AAB6B`:

```
https://art.gametdb.com/switch/coverHQ/US/AAB6B.jpg
```

Nothing on the console carries that code. `NsApplicationControlData` hands the
menu the NACP and the icon, and there is no product number anywhere in the NACP;
a digital-only title has no cartridge, so it has no code at all.

So the code has to be looked up, and until the menu carries GameTDB's own
database to look it up in, the mapping is a file you write:

```
sdmc:/slaunch/config/gametdb_ids.txt

# title id        = GameTDB code
0100152120B54000  = AAB6B        # Title Name
0100000000010000  : AAAAA
0100ABCDEF012345    AZZZZ
```

`=`, `:`, or plain whitespace all separate; `#` and `;` start a comment. A line
whose left side is not a 16-hex title id is dropped rather than quietly mapped,
and so is one whose right side has a space in it.

**A title with no entry is skipped without a request.** Guessing - asking for
`/0100152120B54000.jpg` - would 404 on every title on the card. With no mappings
at all the whole source reports itself off, so this changes nothing whatsoever
for anyone who has not written that file.

## Languages, and the three Englishes

The third path segment is a **language**, not a territory - except for English,
which GameTDB splits three ways:

| Code | |
|---|---|
| `EN` | UK English |
| `US` | American English |
| `AU` | Australian English |
| `JA` `FR` `DE` `ES` `IT` `NL` `PT` `RU` `KO` | the console's other languages |
| `ZHCN` `ZHTW` | Chinese, by script |

Art is not uploaded in every language, so each configured one is tried in turn.
Types are the outer loop, so a `coverHQ` in *any* language is preferred over a
small `cover` in the first language that has one.

By default the list follows the console:

| Console language | Tried, in order |
|---|---|
| `en-GB` | `EN`, `US`, `AU` |
| `en-US`, or no answer | `US`, `EN`, `AU` |
| `fr-FR` | `FR`, `US`, `EN` |
| `zh-Hant` | `ZHTW`, `US`, `EN` |
| `sv-SE` (no GameTDB folder) | `US`, `EN`, `AU` |

A cover in the wrong English is still the right game in the right language, so
all three are tried - just in the order the console's own setting implies, since
`en-GB` and `en-US` are distinguishable and the local edition is the one whose
cover text and ratings flash match. The system language is read straight from the
console rather than through the menu's locale code, which is truncated to two
letters and so cannot tell `ZHCN` from `ZHTW`.

Three entries and not all fourteen on purpose: a title with no scan anywhere is
the ordinary case, and probing the rest would spend ten more requests per title
establishing it.

## A note on `coverfullHQ`

`coverfullHQ` is the **whole wrap** - back, spine and front in one landscape
image. It is deliberately not a default, because `DecodeCoverSurface` scales a
cover straight to 480x720 with `SDL_BlitScaled` and no aspect preservation, so a
wrap arrives on the shelf visibly squashed. Putting one to use needs a crop step
that pulls the front panel out of the wrap, which does not exist yet.

Until it does, `coverHQ` is the front panel already cropped for you, in whichever
language you asked for - which is the same picture, without needing the geometry.
The plain `box` art and the bulk cover packs on GameTDB's downloads page are not
used either: this fetches one cover for one product code, on demand.

## Configuration

Everything about the path is configurable, and deliberately so: GameTDB is
partway through a ground-up site rewrite, and a change to the art layout must not
need a new build of the menu. Create

```
sdmc:/slaunch/config/gametdb.txt
```

with any of the following, one `key=value` per line. Lines starting with `#` or
`;` are ignored, as is a key with an empty value - so a typo leaves the default
in place instead of silently turning the feature off.

| Key | Default | What it does |
|---|---|---|
| `enabled` | `1` | `0` turns GameTDB off; the SteamGridDB path is untouched |
| `base` | `https://art.gametdb.com/switch` | everything before the type; a trailing `/` is ignored |
| `types` | `coverHQ,cover` | art folders to try, best first |
| `regions` | `auto` | language folders to try, in order. `auto` is the table above |
| `ext` | `jpg` | file extension; a leading `.` is ignored |
| `timeout` | `10` | seconds per request |
| `max_tries` | `9` | hard cap on requests for one title, whatever `types` x `regions` comes to |

`type` and `region` are accepted as spellings of `types` and `regions`, and
`regions=auto` is the default written out - handy for reverting a list without
deleting the line.

Both files are read once when the menu starts, so an edit takes effect on the
next launch.

### Example: only high-resolution UK covers

```
types=coverHQ
regions=EN
max_tries=1
```

### Example: try every language GameTDB has

One request per language per type, so raise the cap to match or it stops early.

```
regions=EN,US,AU,JA,FR,DE,ES,IT,NL,PT,RU,KO,ZHCN,ZHTW
max_tries=28
```

### Example: following a layout change without a new build

```
base=https://art.gametdb.com/switch/v2
ext=png
```

## Where covers land

The same place every other cover does:

```
sdmc:/slaunch/covers/<title id>.jpg
```

Note that the *file* is named by title id even though the *URL* was not - the
product code is only how GameTDB addresses art, not how the menu stores it.

The extension stays `.jpg` whatever GameTDB actually served, because that is the
name the menu looks for and SDL_image sniffs the real format anyway. A cover you
dislike is an ordinary file: delete it and the next fetch replaces it, or drop
your own in and nothing will overwrite it - a title that already has a cover is
never refetched.

Requests are logged to `sdmc:/slaunch/covers.log`:

| Line | Meaning |
|---|---|
| `gametdb-ok` | a cover landed |
| `gametdb-no-id` | no product code for this title, nothing was requested |
| `gametdb-miss` | every candidate was tried and none answered |

each with the HTTP status and CURLcode of the last request, which is the only way
to tell a missing scan from a blocked connection on a console.

A `200` that is not actually an image - an HTML "no such cover" page - is
detected by its magic bytes, deleted, and treated as an ordinary miss, so it
cannot be cached as a permanently art-less title. A request that comes back with
no HTTP status at all never reached the server, and the remaining candidates are
abandoned rather than each spending a full timeout to say so.

## Content filter

The [content filter](CONTENT_FILTER.md), when enabled, checks the game's name
*before* any GameTDB request is made, so filtered titles cost no traffic at all.
GameTDB art is addressed directly by product code and carries no ratings or tags
alongside the image, so name filtering is the whole of what applies here - there
is no metadata response to inspect the way there is for SteamGridDB and Steam.

## Credit

GameTDB is a community database; its covers are uploaded and maintained by
volunteers. Support for it here was asked for by a GameTDB moderator in
[issue #5](https://github.com/etonedemid/slaunch/issues/5).
