# Status — picking up where we left off

Last working session ended **2026-05-11** on `main`. The README is the
canonical public story; this doc is the running list of what works,
what doesn't, and what to pick up next.

## What works end-to-end today

Run `make sim` from `core/`, then `./build-sim/sim/core-sim
--music ~/Music` (any directory of `.flac` / `.mp3` files). The sim
opens an SDL window at 320×240 (×2 scaled) with the real Linen UI.

- **Music browser** — recursive scan, all four iconic groupings
  (Artists / Albums / Genres / Composers), drilldown to songs, SELECT
  plays through the audio engine with real metadata + embedded art.
  Same-track copies are deduped (FLAC > MP3) and combo artists
  ("A & B feat. C") fold to their primary.
- **Library load paths**: `--music <dir>` scans + parses tags at
  startup; `--tagcache <file.tcdb>` loads the precomputed binary index.
- **Album art** — embedded JPEG (FLAC PICTURE / ID3v2 APIC) decoded
  via stb_image, box-average downscaled, served through a 32-slot LRU
  for list thumbnails (22²) and a separate 4-slot LRU for NP art
  (130² / 180²). Untagged tracks get a diagonal-stripe placeholder.
- **Artist photos** — fetched from Deezer at `core tagcache build
  --fetch-art` time, cached under `~/.cache/core/artist-art/`,
  inlined into `.tcdb` v2 so the firmware needs no network.
- **Now Playing** — four cycle-able pages (default / big art / peak
  meter / track info). 130² art with cap-aligned format badge, 6 px
  full-width progress bar with elapsed / remaining labels, "Up next"
  beneath. MP3 progress now reports total_frames correctly so the
  bar + remaining time actually move.
- **Search** — on-screen keyboard (4×7 grid), case-insensitive ASCII
  substring match across title + artist + album, results grouped
  into Songs / Albums / Artists sections with headers.
- **Settings** — 4 rows. Theme cycles Light ↔ Dark via SELECT;
  Volume + Brightness via LEFT/RIGHT (±10, clamped 0..100, sim-state
  only — hw needs WM8758B I²C + LCD backlight PWM). About drills
  into firmware version + library counts.
- **Chrome / typography** — gamma-correct AA glyph blending (4
  precomputed sRGB↔linear LUTs, integer hot path). Pre-rasterized
  shuffle / repeat icons landed but are flagged "preliminary, needs
  polish" in their commit.

## What's NOT done (pick up next)

Roughly in order of payoff × strategic weight. **Sim work is the easy
half** — Phase 1 is where ~90% of from-scratch iPod firmware projects
die. Don't stack another sim PR without weighing that tradeoff.

1. **Phase 1: bootable ARM skeleton** — in progress.
   - **PR #1 (#40):** `arm-none-eabi-gcc` cross-build, minimal
     `boot/crt0.S` + `boot/linker.ld`, `kernel/main.c` spin loop.
     `make hw` produces a 60-byte ARM ELF entered at 0x0; `make sim`
     and the four test suites are unaffected.
   - **PR #2 (`phase1/image-packaging`):** `.ipod` transport-format
     packaging. `make hw` now also emits `core.bin` (objcopy `-O
     binary`); `make ipod` wraps it as `core.ipod` (BE32 additive
     checksum + `ipvd` model name + image). `core firmware pack` /
     `unpack` subcommands expose the format; round-trip verified
     against the ARM ELF. Go tests cover empty / wrap / mismatch /
     truncated. Review fixes on the branch: crt0 now performs the
     MMAP0 SDRAM remap itself (the doc-documented handoff has SDRAM
     at native 0x10000000, not 0x0 as crt0 first assumed — would have
     crashed on hardware); unpack derives the checksum seed from the
     embedded model name; pack/unpack got O_EXCL + `--force`.
   - **PR #3 (`phase1/uart-debug`, stacked on PR #2):** UART debug @
     115200 via SER0 — first freestanding driver. `hal/hw/pp5022.h`
     is the canonical platform header (memory map, MMAP0, SER0; every
     constant cites its doc section). Polled TX with bounded spin;
     kernel banner + hex self-test + PROCESSOR_ID dump. The four hw
     questions raised during review were **resolved 2026-06-10**
     against Rockbox `crt0-pp.S` / `pp5020.h` / `uart-pp.c` and
     ipodloader2 source; facts folded into `core/docs/hw/` + code.
     The real bug was elsewhere: `MMAP0_LOGICAL` needs the window
     mask (`0x3C00`, the size mask lives in the *logical* register),
     not `0x00000000`; `0x0F84` was correct all along (`0x3F84` is
     PP5002's). `DEV_SER0` = bit 6, `GPO32_ENABLE` = `0x70000084`
     (uart_init now does the full routing/enable/reset sequence);
     SER0 registers are word-strided (the doc's byte-strided IER/FCR
     were a transcription slip, corrected). ipodloader2's handoff
     state confirmed from its source: it restores the Apple-ROM MMAP
     and jumps at native `0x10000000` — crt0's remap-it-ourselves
     approach is right.
   - **PR #4:** LCD init + solid-color present (5G + 5.5G variants).
   - **PR #5:** Kernel scheduler skeleton + idle task.
   See `PLAN.md` § Phase 1.
2. **Playlists** — M3U8 reader → tagcache resolver → read-only
   browse is the right opening PR. Sim-only, ships visible value.
   Design in the `project_playlists_design` memory: M3U8 on disk,
   separate dynamic queue (RAM) from saved playlists (disk).
3. **Polish the new chrome icons** — c2766f7 explicitly says
   "preliminary, needs polish". Low effort, finishes an in-flight
   thread.
4. **mmap path for `.tcdb` v2** — `tcdb_parse` already takes
   `(bytes, len)`; just the slurp wrapper changes. Small, but only
   meaningful with hardware.
5. **HAL knob hw effects** — `hal_volume_set` / `hal_backlight_set`
   are sim-state only; on hw they need WM8758B I²C + LCD backlight
   PWM. Hardware.
6. **Polish odds & ends** — UTF-8 mid-codepoint truncation in tag
   fields; v2.4 compressed APIC frames misparse silently; compressed
   art format support (PNG via APIC) if a real-world MP3 ever needs it.

## Test gates

`meson test -C build-sim` from `core/` runs all four suites; should
always be green.

- **codec-kat** (unit) — FLAC + MP3 decoders bit-exact against
  reference PCM.
- **tcdb-reader** (unit) — C-side parser exercised against a hand-
  rolled in-memory `.tcdb` (5 cases incl. truncation, bad magic).
- **tag-mp3** (unit) — ID3v2.3/2.4 parser cases including TCON
  numeric → genre name mapping.
- **sim-audio-playback** (integration) — spawns `core-sim` with
  SDL's disk audio driver, drives Music → Songs → SELECT, bit-
  compares 176 400 captured bytes against the codec KAT reference.

Quick visual smoke: `./build-sim/sim/core-sim --shot /tmp/x.bmp`
(headless, no window pop).

### Emulator smoke for the hw image (clicky)

The [clicky](https://github.com/daniel5151/clicky) PortalPlayer
emulator (evaluated 2026-06-11) boots our unmodified `core.bin`: it
models the MMAP0 remap (functionally — our 0x3C00/0x10000F84 pair
produces a real 0→0x10000000 mapping), the SER0 UART (TX → stdout),
GPO32, and stubs DEV_EN/DEV_RS harmlessly. It already caught one real
bug (unparked COP racing the CPU through crt0 — its HLE boot enters
both cores). Caveats: it's the 4G/PP5020 machine model (PROCESSOR_ID
reads full-word 0x55555555, SER0 DLAB unmodeled, no BCM2722 LCD, no
2048-byte-sector ATA), and the GUI needs a display (WSLg ok; xvfb for
CI). Expected output: the three `core:` banner lines, single-stream.

```bash
# one-time setup (Rust via asdf; clicky clone + build)
asdf plugin add rust && asdf install rust 1.96.0
git clone https://github.com/daniel5151/clicky && cd clicky
echo 'rust 1.96.0' > .tool-versions && cargo build --release -p clicky-desktop
cc -o make_fw resources/ipodloader/make_fw.c
printf '\x01\x02\xa0\xe3\x10\xff\x2f\xe1' > jumpstub.bin  # mov r0,#0x10000000; bx r0

# per run (from the clicky dir; core.bin from `make hw`)
./make_fw -g 4g -o core_fw.bin -l <repo>/core/build-hw/core.bin jumpstub.bin
timeout 15 ./target/release/clicky-desktop --hle=core_fw.bin --hdd=null:len=64MiB \
  2>&1 | grep '^core:'
```

(GDB works too: `--gdb` + `arm-none-eabi`-aware gdb against
`core.elf` gives source-level breakpoints — verified at
`kernel_main`. Current checkout lives in `/tmp/clicky-eval/` and
will not survive a reboot; re-clone per the recipe.)

## Recent PRs (since #31 — most recent first)

- pre-rasterized shuffle / repeat icons (preliminary)
- artist photos + categorized search (`.tcdb` v2)
- tagcache scan: dedup same-track copies + fold combo artists
- album-art thumbnails on the Albums list
- box-average downscale for decoded album art
- album-art LRU cache + Now Playing layout pass
- MP3 total_frames fix (progress bar + remaining time work)
- recursive tagcache scan
- prune Rockbox-era theme assets, scripts, stale build output
- refresh sim screenshots against a real 311-track library
- #39 README screenshots regenerate
- #38 HAL volume + brightness + Settings rows
- #37 gamma-correct AA blending in atlas_render
- #36 search matches title + artist + album
- #35 settings frame with light/dark theme + about
- #34 search frame with on-screen keyboard
- #33 ID3v2 TCON numeric → genre name mapping
- #32 build_filter_indexes refactor

Older PRs (#1–#31) are summarized at the bottom of `git log --oneline`
and in commit history. `main` is linear; each commit was a squash-
merge of one PR.

## Repository links

GitHub: https://github.com/BrandonDedolph/ipod_theme
