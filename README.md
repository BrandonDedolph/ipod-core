# core — custom firmware for iPod Video 5G/5.5G

A from-scratch firmware that ships **Cabinet** as the shell and **Linen**
as the only theme. Replaces Rockbox entirely on the device.

The simulator runs the real C firmware against an SDL2 host backend, so
every screen below is rendered by the same code that will run on the
iPod.

![Main menu](docs/img/01-main.png) ![Now Playing](docs/img/09-np-default.png) ![Big art](docs/img/10-np-bigart.png)

---

## Status

The music browser is feature-complete in sim — recursive scan, four
iconic groupings (Artists / Albums / Genres / Composers), drill down,
play. Now Playing shows real metadata, embedded album art, and a
proper progress bar. Lists carry 22² thumbnails; Artists pulls real
artist photos from Deezer at tagcache-build time.

| Working today (sim) | Pending |
|---|---|
| FLAC + MP3 decoders (bit-exact via codec KAT) | Bootable ARM image (Phase 1) |
| Audio engine — SPSC ring + SDL2 HAL | On-device USB / disk / battery |
| Recursive library scan + tag parse (Vorbis, ID3v2.3/2.4) | Playlists, podcasts, audiobooks |
| Same-track dedup (FLAC > MP3) + combo-artist fold | Per-track gain (replaygain) |
| Drilldown: Artists / Albums / Genres / Composers → songs | Volume / brightness (need HAL knobs) |
| Embedded album art (FLAC PICTURE, MP3 APIC → JPEG, box-averaged) |  |
| Per-list 22² thumbnails (Songs / Albums / Artists), LRU cached |  |
| Artist photos via Deezer at tagcache-build time |  |
| Binary tagcache `.tcdb` v2 — Go encoder + C reader, artist art inline |  |
| Categorized search (songs / albums / artists) with section headers |  |
| Now Playing layout: cap-aligned format badge, 130² art, 6px progress bar, "Up next" beneath the bar |  |
| Settings — light / dark theme cycle + About screen |  |
| End-to-end audio playback test (captures real PCM, bit-compares to reference) |  |

55 commits on `main`. See [`STATUS.md`](STATUS.md) for the running list.

---

## Quick start (sim)

Requires `meson`, `ninja`, `pkg-config`, `libsdl2-dev`, and a C11
compiler. Run `tools/install_deps.sh` for a one-shot setup.

```bash
cd core
make sim                                  # builds build-sim/sim/core-sim
./build-sim/sim/core-sim                  # synthetic data — empty library
./build-sim/sim/core-sim --music ~/Music  # scan a real directory at startup
meson test -C build-sim                   # codec KAT + .tcdb parser + audio playback
```

Wheel = scroll, center = SELECT, MENU = back, SPACE = pause/resume.

---

## Screenshots

All captured headlessly via `core-sim --shot <path.bmp>` against a
six-track tagged library; the same code path runs in interactive mode.

### Cabinet shell — main menu and Music sub-menu
![Main menu](docs/img/01-main.png) ![Music sub-menu](docs/img/02-music.png)

### Search — on-screen keyboard with live results
Wheel cycles the highlighted key (or, with focus shifted via RIGHT,
the result list); SELECT types / plays. Substring match,
case-insensitive ASCII against song title, artist, or album.

![Search](docs/img/13-search.png)

### Settings — light / dark theme
Settings → **Theme** cycles between Light and Dark; everything except
album art flips. **About** shows firmware version + library counts.

| Light | Dark |
|---|---|
| ![Settings light](docs/img/14-settings-light.png) | ![Settings dark](docs/img/15-settings-dark.png) |

| Main menu, dark | About |
|---|---|
| ![Main dark](docs/img/16-main-dark.png) | ![About](docs/img/17-about.png) |

### Library views (all backed by the tagcache)
![Songs](docs/img/03-songs.png) ![Artists](docs/img/04-artists.png)

### Genres / Composers — same shape as Artists/Albums
![Genres](docs/img/05-genres.png) ![Composers](docs/img/06-composers.png)

### Drilldowns — pick a row, see its songs
![Aphex Twin → songs](docs/img/07-aphex-songs.png) ![Genre IDM → songs](docs/img/08-genre-idm.png)

### Now Playing — four cycle-able pages
The iconic iPod NP screen. Embedded album art renders at 130² on the
default page and 180² on the big-art page; SELECT cycles pages, MENU
pops. Format badge sits cap-aligned with the album line; the progress
bar runs the full width with elapsed / remaining labels above it,
"Up next" tucked beneath. Both art sizes go through a 4-slot LRU so
scrubbing doesn't re-decode the JPEG.

| Default | Big art |
|---|---|
| ![NP default](docs/img/09-np-default.png) | ![NP big art](docs/img/10-np-bigart.png) |

| Peak meter (synthesized) | Track info |
|---|---|
| ![NP peak meter](docs/img/12-np-peakmeter.png) | ![NP track info](docs/img/11-np-trackinfo.png) |

---

## Binary tagcache

Real iPod hardware can't afford a scan-at-startup pass — even a small
library on a USB-disk takes seconds-to-minutes to walk and re-parse
every tag. The `core` host CLI builds a precomputed binary index the
firmware mmaps at boot.

```bash
cd core/cli
go build -o /tmp/core ./cmd/core
/tmp/core tagcache build ~/Music
# ~/Music/tagcache.tcdb: 1834 songs, 142 artists, 218 albums, 17 genres, 89 composers (412 KB)

# --fetch-art pulls real artist photos from Deezer (no auth, ~150ms
# per artist, cached under ~/.cache/core/artist-art/). Re-runs reuse
# the cache; a `.missing` sentinel keeps unknown artists from
# re-fetching every build.
/tmp/core tagcache build --fetch-art ~/Music

cd ..
./build-sim/sim/core-sim --tagcache ~/Music/tagcache.tcdb
```

Format spec: [`core/apps/db/tagcache_format.h`](core/apps/db/tagcache_format.h)
(C side) and [`core/cli/internal/tagcache/format.go`](core/cli/internal/tagcache/format.go)
(Go side). Both sides have round-trip tests; a layout drift fails both
simultaneously.

---

## Repo layout

```
core/                     C firmware + simulator
├── boot/                 (empty — Phase 1)
├── kernel/               (empty — Phase 1)
├── hal/
│   ├── hal.h             contract
│   ├── hw/               (empty — needs hardware)
│   └── sim/              SDL2-backed implementation
├── codecs/
│   ├── tags.h            shared audio_tags_t (incl. owned art bytes)
│   ├── decoder.h         unified decoder ABI
│   ├── dr_flac/          vendored single-header FLAC + Vorbis comments
│   ├── dr_mp3/           vendored single-header MP3 + custom ID3v2 parser
│   └── stb_image/        JPEG-only build for embedded album art
├── apps/
│   ├── audio/            decoder → SPSC ring → hal_audio engine
│   ├── db/               tagcache: scan, parse, build indexes, .tcdb reader
│   └── ui/               Cabinet shell, list view, Now Playing, Linen chrome
├── cli/                  Go host CLI (build / install / update / tagcache build)
├── sim/                  core-sim entry point
├── docs/hw/              Phase-0 hardware reference (8 subsystems, ~2,500 lines)
└── tests/                codec KAT + .tcdb reader + end-to-end audio playback

design_reference/              JSX/HTML design source — themes, palette, icon paths
docs/img/                      Sim screenshots (this README)
tools/                         Top-level build helpers (atlas generator, deps installer, font sources)
                               (firmware-side tools live in core/tools/, e.g. icon_gen.sh)
```

See [`core/README.md`](core/README.md) for the firmware-side build details
and [`PLAN.md`](PLAN.md) for the phased roadmap.

---

## Verifying audio actually plays

The `sim-audio-playback` integration test spawns `core-sim` with the
SDL2 `disk` audio driver, drives Music → Songs → SELECT, and bit-
compares the captured S16LE samples against the codec KAT reference.
This catches regressions anywhere in the chain — tagcache, cabinet's
play path, audio engine, ring buffer, or HAL audio source callback.

```bash
cd core
meson test -C build-sim --suite integration
# 2/2 integration - core:sim-audio-playback OK     6.74s
# stdout: ok: 176400 bytes of audio matched (capture started at byte 0)
```

To listen interactively, drop `--shot` and use `--music`:

```bash
./build-sim/sim/core-sim --music ~/Music
```

---

## License

Apache-2.0 first-party; vendored codecs (`dr_flac`, `dr_mp3`,
`stb_image`) keep their upstream licenses (zlib / public domain / MIT).
Nunito font: SIL Open Font License 1.1 (`tools/fonts-src/`).
