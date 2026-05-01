# Status — picked up where we left off

Last working session ended 2026-05-01 with **16 PRs merged on main**.

## What works end-to-end today

Run `make sim` from `core/`, then `./build-sim/sim/core-sim`. The
sim opens an SDL window at 320×240 (×2 scaled) with a real Linen UI:

- **Main menu** — "iPod" upper-left, 78% battery upper-right, separator
  line, six menu items (Music / Playlists / Podcasts / Audiobooks /
  Settings / Now Playing) with the trailing chevron and ink-bg selector.
- **Music sub-menu** — drills into Music → Artists / Albums / Songs /
  Genres / Composers, each rendering ~25 real names from the synthetic
  tagcache (Aphex Twin, Drukqs, Avril 14th, etc).
- **Now Playing** — SELECT on the main menu's Now Playing plays the
  FLAC fixture and pushes the NP frame:
  - Page 1 (default): striped art, title, artist, album, 4-star rating,
    "FLAC 44 kHz" badge, "UP NEXT" row, scrubber, time labels.
  - Page 2 (big art): dark backdrop, 180×180 art, title overlay.
  - Page 3 (peak meter): two animated channel columns with
    amber/red zones, dB labels.
  - Page 4 (track info): key/value rows.
  - SELECT cycles, MENU pops back to the main menu (track keeps
    playing in the background).
- **Pause / resume** — SPACE on any frame.
- **Headless capture** — `core-sim --shot path.bmp [--press CHARS] [--frames N]`
  writes a 24-bit BMP screenshot. Used through the session for visual
  verification.

Plus underneath the UI:

- **FLAC + MP3 decoders** (dr_flac + dr_mp3) wrapped under a unified
  `decoder_t` ABI. Both bit-exact via the codec KAT (`meson test`).
- **Audio engine** — SPSC ring buffer between decoder and HAL audio,
  `__atomic_*` release/acquire ordering for dual-core PP5022 safety.
- **HAL** — LCD framebuffer, click wheel via keyboard, monotonic clock,
  audio out via SDL2, log to stdout.
- **Phase-0 hardware reference doc** at `core/docs/hw/` — 8 subsystems,
  ~2,500 lines, reverse-engineered from Rockbox source.

## What's NOT wired (pick up tomorrow)

The sim **can play one specific track** (the synthetic 1 s sine FLAC at
`core/tests/codec-vectors/sine_440hz_1s_44k_s16_stereo.flac`) via the
"Now Playing" main-menu item. **It cannot yet play music from the
library** — selecting a song in Music → Songs is a no-op.

To make it actually play library music, three things are needed
(easiest first):

1. **`core-sim --music <dir>` flag.** Scan a directory for `.flac` /
   `.mp3` files at startup, populate the Songs list with their
   filenames, wire SELECT-on-song to `audio_engine_play(...)` with
   that file's bytes. ~150 lines in `core/sim/main.c` +
   `core/apps/db/tagcache.c`. **This is the ~30-min next move.**
2. **Path-mapped tagcache.** Current `tagcache.c` only stores names;
   needs to store filesystem paths alongside so SELECT can find the
   actual bytes. Tiny refactor.
3. **Drill-down Music → Artist → Album → Song.** Cabinet's frame
   stack needs per-frame parent-context (which artist's albums are we
   listing?). Bigger but bounded.

After those, the natural next moves are the bigger pending items:

4. **Go-side `core release tagcache <music-dir>` indexer** — scan a
   real music directory, parse ID3v2 / Vorbis / MP4 tags via
   `github.com/dhowden/tag`, emit a binary tagcache file. The C
   reader replaces `apps/db/tagcache.c`'s synthetic data.
5. **Real ID3 metadata in the NP screen** — title/artist/album come
   from the played file's tags, not hardcoded "Test Sine 440 Hz".
6. **Album-art JPEG decode** — use `dr_jpg` or similar, replace the
   striped placeholder with actual cover art.
7. **Phase 1+: bootable ARM skeleton** — needs hardware in the loop.
   See `PLAN.md`.

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
│   ├── dr_flac/  vendored single-header FLAC
│   └── dr_mp3/   vendored single-header MP3
├── apps/
│   ├── audio/    decoder → ring → hal_audio engine
│   ├── db/       tagcache (synthetic data today)
│   └── ui/       Cabinet shell, list view, NP, Linen chrome
├── cli/          Go CLI scaffold (10 subcommands, mostly stubs)
├── sim/          core-sim entry point
└── tests/        codec KAT, fixtures, codec-vectors

tools/
├── atlas_gen.{py,sh}      Pillow-based glyph atlas generator
├── install_deps.sh        distro-aware dev-env installer
└── fonts-src/             Nunito TTFs (Regular/Medium/SemiBold/Bold/ExtraBold)
```

## Test gates

- `cd core && meson test -C build-sim` — codec KATs (FLAC + MP3,
  bit-exact). Should always pass.
- `cd core && ./build-sim/sim/core-sim --shot /tmp/x.bmp` — quick
  visual smoke test; produces a BMP of the main menu.

## Repository links

GitHub: https://github.com/BrandonDedolph/ipod_theme

All 16 PRs are squash-merged into `main`; commit history is linear
(`git log --oneline -16`).
