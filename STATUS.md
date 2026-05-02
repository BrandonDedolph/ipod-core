# Status — picked up where we left off

Last working session ended **2026-05-01** with **23 PRs merged on
main** (16 from the prior session, 7 from this one).

## What works end-to-end today

Run `make sim` from `core/`, then `./build-sim/sim/core-sim
--music ~/Music` (any directory of `.flac` / `.mp3` files). The sim
opens an SDL window at 320×240 (×2 scaled) with a real Linen UI.

### Without `--music` (synthetic data)

- **Main menu** — "iPod" header, 78% battery, six items (Music,
  Playlists, Podcasts, Audiobooks, Settings, Now Playing).
- **Music sub-menu** — drills into Artists / Albums / Songs / Genres /
  Composers populated from a synthetic tagcache (~26 real-looking
  artist names: Aphex Twin, Beach House, Boards of Canada, etc).
  Selecting any leaf row logs a stub.
- **Now Playing** — SELECT on the main-menu "Now Playing" item plays
  the FLAC fixture, pushes the NP frame with four cycle-able pages
  (default / big art / peak meter / track info). SELECT cycles,
  MENU pops, SPACE pauses.

### With `--music <dir>` (real library)

Same UI, but:
- **Songs list** shows the discovered files sorted by TITLE tag (or
  filename for untagged files).
- **Artists / Albums menus** show only the unique values present in
  the loaded library (sorted, NULL tags skipped).
- **Drilldown**: SELECT on an Artist row → that artist's songs;
  SELECT on an Album row → that album's songs. SELECT on any leaf
  song row plays it through the audio engine, populating NP with
  real title / artist / album from the file's tags.
- **Tag readers** ship for FLAC (Vorbis comments via dr_flac's
  metadata API) and MP3 (custom ID3v2.3/2.4 parser at
  `core/codecs/dr_mp3/tag_mp3.c`). UTF-16 ID3v2 frames are
  best-effort downconverted to ASCII.

### Underneath the UI

- **FLAC + MP3 decoders** (dr_flac + dr_mp3) under a unified
  `decoder_t` ABI. Both bit-exact via the codec KAT (`meson test`).
- **Audio engine** — SPSC ring buffer between decoder and HAL audio,
  `__atomic_*` release/acquire ordering for dual-core PP5022 safety.
- **HAL** — LCD framebuffer, click wheel via keyboard, monotonic
  clock, audio out via SDL2.
- **Phase-0 hardware reference doc** at `core/docs/hw/` — 8
  subsystems, ~2,500 lines.
- **Headless capture** — `core-sim --shot path.bmp` runs SDL with
  `SDL_VIDEODRIVER=dummy` so screenshots don't pop a window or play
  audio. Used heavily for visual verification.

## What's NOT done (pick up next)

The music browser is feature-complete for sim use. Remaining items
from the original PLAN.md:

1. **Album-art JPEG decode** — replace the diagonal-stripe placeholder
   with the actual cover art. Vendor `dr_jpg` (or similar single-
   header JPEG decoder), call it from the FLAC PICTURE block /
   ID3v2 APIC frame, render into the NP screen. Probably 200-300
   lines.

2. **Go-side `core release tagcache <music-dir>` indexer** — scan a
   real music directory, parse tags via `github.com/dhowden/tag`,
   emit a binary tagcache file. The C reader replaces the in-memory
   scan-at-startup path with mmap'd binary. Needed before this
   firmware ships on real hardware (scan-at-startup over USB-disk
   speeds is too slow).

3. **Genres / Composers drilldown** — same shape as Artists/Albums,
   trivial extension once the patterns are in.

4. **Phase 1: bootable ARM skeleton** — needs hardware in the loop.
   See `PLAN.md`. Major chunk.

5. **Search / on-screen keyboard** — iPod-style alphabetical jump
   into long lists.

## Recent PRs (this session)

- #17 `Library playback: --music dir scans + SELECT-on-song plays`
- #18 `NP metadata: real FLAC Vorbis tags in Now Playing`
- #19 `NP metadata: ID3v2 reader for MP3 + shared audio_tags_t`
- #20 `Tagcache: parse tags during library_load, real titles in Songs`
- #21 `Tagcache: derive unique-artist + unique-album indexes`
- #22 `Tagcache: per-song filter indexes + drilldown query APIs`
- #23 `Cabinet drilldown: Music → Artists/Albums → Songs → play`

## Repo layout reminder

```
core/
├── docs/hw/      Phase-0 hardware reference (8 subsystems)
├── boot/         (empty — phase 1)
├── kernel/       (empty — phase 1)
├── hal/
│   ├── hal.h     contract
│   ├── hw/       (empty — needs hardware)
│   └── sim/      SDL2-backed implementation
├── codecs/
│   ├── tags.h           shared audio_tags_t
│   ├── dr_flac/         vendored single-header FLAC + tag_flac
│   └── dr_mp3/          vendored single-header MP3 + tag_mp3 (ID3v2)
├── apps/
│   ├── audio/           decoder → ring → hal_audio engine
│   ├── db/              tagcache: scan, parse tags, build indexes
│   └── ui/              Cabinet shell, list view, NP, Linen chrome
├── cli/                 Go CLI scaffold (10 subcommands, mostly stubs)
├── sim/                 core-sim entry point
└── tests/               codec KAT, fixtures, codec-vectors

tools/
├── atlas_gen.{py,sh}      Pillow-based glyph atlas generator
├── install_deps.sh        distro-aware dev-env installer
└── fonts-src/             Nunito TTFs (Regular/Medium/SemiBold/Bold/ExtraBold)
```

## Test gates

- `cd core && meson test -C build-sim` — codec KATs (FLAC + MP3,
  bit-exact). Should always pass.
- `cd core && ./build-sim/sim/core-sim --shot /tmp/x.bmp` — quick
  visual smoke test; produces a BMP of the main menu (headless, no
  window pop-up).

## Repository links

GitHub: https://github.com/BrandonDedolph/ipod_theme

All 23 PRs are squash-merged into `main`; commit history is linear
(`git log --oneline -23`).
