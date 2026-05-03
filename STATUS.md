# Status — picked up where we left off

Last working session ended **2026-05-03** with **31 PRs merged on
main**: 16 from the original arc, 7 library/drilldown, 3 album art,
1 Genres/Composers, 1 Go-side `.tcdb` encoder, 1 sim audio-playback
test, 1 C-side `.tcdb` reader, 1 README + screenshots refresh.

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
  SELECT on an Album row → that album's songs; same for Genres and
  Composers. SELECT on any leaf song row plays it through the audio
  engine, populating NP with real title / artist / album from the
  file's tags.
- **Tag readers** ship for FLAC (Vorbis comments via dr_flac's
  metadata API: TITLE / ARTIST / ALBUM / GENRE / COMPOSER) and MP3
  (custom ID3v2.3/2.4 parser at `core/codecs/dr_mp3/tag_mp3.c`:
  TIT2 / TPE1 / TALB / TCON / TCOM / APIC). UTF-16 ID3v2 frames are
  best-effort downconverted to ASCII.
- **Album art** — embedded JPEG cover art (FLAC PICTURE block /
  ID3v2 APIC frame) is decoded via stb_image (JPEG-only build) and
  rendered on both the default page (84×84) and big-art page
  (180×180). Untagged tracks fall back to diagonal-stripe placeholder.

### With `--tagcache <file.tcdb>` (precomputed binary index)

Same UI end-state as `--music`, but loads from a precomputed binary
file instead of scanning + reparsing tags:

- **Build the file** with the host CLI:
  `core tagcache build ~/Music --out ~/Music/tagcache.tcdb`
- **Load it** with `core-sim --tagcache ~/Music/tagcache.tcdb`.
- This is what the firmware will use on real hardware (scan-at-startup
  over USB-disk speeds is too slow). The Go encoder
  (`core/cli/internal/tagcache/`) and C reader
  (`core/apps/db/tagcache.c::tcdb_parse`) share their format spec via
  parallel headers (`format.go` ↔ `tagcache_format.h`); both round-
  trip themselves and cross-validate via the audio-playback test.

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
- **Audio capture for tests** — `core-sim --shot ... --capture-audio
  out.raw` switches the SDL audio backend to its disk driver, writing
  the engine's raw S16LE samples to a file. The `sim-audio-playback`
  meson test uses this to bit-compare engine output against the codec
  KAT reference end-to-end (catches regressions in tagcache → cabinet
  → engine → ring → HAL audio source callback that the codec KAT
  alone wouldn't see).

## What's NOT done (pick up next)

The music browser feels real end-to-end in sim — load, browse all
four iconic groupings, drill, play. Both the in-memory scan path
and the binary `.tcdb` path produce bit-identical audio output
(verified by the integration test). Remaining items, roughly in
order of payoff vs. effort:

1. **Phase 1: bootable ARM skeleton** — needs hardware in the loop.
   See `PLAN.md`. Major chunk; not trivially mockable in sim.

2. **mmap path for `.tcdb`** — the C reader currently slurps the
   file with fread+heap-copy of strings/art. On real hardware we'd
   mmap and let pointers reference into the mapped region. The
   `tcdb_parse` shape already takes `(bytes, len)`; just the slurp
   wrapper changes. Small PR, but needs hardware to actually exercise.

3. **Search / on-screen keyboard** — iPod-style alphabetical jump
   into long lists.

4. **Settings UI** — currently a stub leaf menu.

5. **Polish odds & ends** — UI quirks (silent UTF-8 truncation
   mid-codepoint in tag fields; v2.4 compressed APIC frames misparse
   silently); ID3v2 TCON `(N)` numeric → genre-name mapping;
   compressed-art format support if a real-world MP3 ever embeds
   PNG via APIC; refactor the four near-identical per-group blocks
   in `tagcache.c::build_filter_indexes` into a helper.

## Recent PRs (this session)

- #17 `Library playback: --music dir scans + SELECT-on-song plays`
- #18 `NP metadata: real FLAC Vorbis tags in Now Playing`
- #19 `NP metadata: ID3v2 reader for MP3 + shared audio_tags_t`
- #20 `Tagcache: parse tags during library_load, real titles in Songs`
- #21 `Tagcache: derive unique-artist + unique-album indexes`
- #22 `Tagcache: per-song filter indexes + drilldown query APIs`
- #23 `Cabinet drilldown: Music → Artists/Albums → Songs → play`
- #24 `Album art: FLAC PICTURE → JPEG decode → render in NP`
- #25 `MP3 album art: ID3v2 APIC extraction in tag_mp3`
- #26 `NP big-art page: render real album art at 180×180`
- #27 `Genres/Composers drilldown: tag readers + tagcache + cabinet`
- #28 `core tagcache build: Go-side .tcdb binary index`
- #29 `sim: --capture-audio + end-to-end audio-playback test`
- #30 `core: C-side .tcdb reader (mmap-style binary tagcache loader)`
- #31 `docs: rewrite README + refresh screenshots, fix NP track-info path`

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
│   ├── tags.h           shared audio_tags_t (incl. owned art bytes)
│   ├── dr_flac/         vendored single-header FLAC + tag_flac
│   ├── dr_mp3/          vendored single-header MP3 + tag_mp3 (ID3v2)
│   └── stb_image/       JPEG-only build for embedded album art
├── apps/
│   ├── audio/           decoder → ring → hal_audio engine
│   ├── db/              tagcache: scan/parse + .tcdb binary reader
│   │                     (tagcache_format.h is the C-side layout spec)
│   └── ui/              Cabinet shell, list view, NP, Linen chrome
├── cli/                 Go CLI — `tagcache build/dump` works; the
│                         other 10 subcommands are stubs
│   └── internal/tagcache/  Go-side encoder + decoder for the .tcdb
│                            (format.go is the Go-side layout spec)
├── sim/                 core-sim entry point (--shot, --music,
│                         --tagcache, --capture-audio)
└── tests/               codec KAT + .tcdb parser + audio playback

tools/
├── atlas_gen.{py,sh}      Pillow-based glyph atlas generator
├── install_deps.sh        distro-aware dev-env installer
└── fonts-src/             Nunito TTFs (Regular/Medium/SemiBold/Bold/ExtraBold)
```

## Test gates

- `cd core && meson test -C build-sim` runs all three suites and
  should always be green:
  - **codec-kat** (unit) — FLAC + MP3 decoders bit-exact against
    reference PCM.
  - **tcdb-reader** (unit) — C-side parser exercised against a
    hand-rolled in-memory `.tcdb` (5 cases: happy, bad magic, bad
    version, truncated header, invalid group song-idx).
  - **sim-audio-playback** (integration) — spawns `core-sim` with
    SDL's disk audio driver, plays the FLAC fixture, bit-compares
    176 400 captured bytes against the codec KAT reference.
- `cd core && ./build-sim/sim/core-sim --shot /tmp/x.bmp` — quick
  visual smoke test; produces a BMP of the main menu (headless, no
  window pop-up).

## Repository links

GitHub: https://github.com/BrandonDedolph/ipod_theme

All 31 PRs are squash-merged into `main`; commit history is linear
(`git log --oneline -31`).
