# Status — picking up where we left off

The README is the canonical public story; this doc is the running list of
what works, what doesn't, and what to pick up next.

## Where we are right now (2026-07-22)

**A full music player on real hardware.** The whole bare-metal stack is
proven end to end on an actual iPod 5.5G 80GB — boot + MMAP0 remap →
clock/PLL → timer/IRQ → LCD (BCM present) → I²C/WM8758/I²S → DMA →
ATA PIO → FAT32 — and on top of it a real player: **streaming FLAC/MP3
off the iPod's own disk** (read-ahead ring, not preload, so full-length
tracks play), a host-built library index (`CORELIB.IDX`) for instant
Songs / Albums / Artists / Genres, and the full Linen/Onyx UI —
album-art chips, 120×120 now-playing cover, scrolling marquee, settings,
volume/lock overlays, battery gauge. On-screen framebuffer console is the
cable-free debug channel; there is NO serial cable (confirm hw state on
screen instead).

**Dev-environment note:** the clean flash environment is **native Linux**
(the iPod is a real `/dev/sdX`, so `ipodpatcher` writes the firmware
partition and data copies persist). The WSL path used in recent sessions
copies over Windows interop (`/mnt` writes don't persist — must go
Windows-native + `Write-VolumeCache`), and the device drops out of disk
mode frequently, so flashing retries until `D:` reappears. Toolchain on
Arch is all official `extra`: `pacman -S arm-none-eabi-gcc
arm-none-eabi-binutils arm-none-eabi-newlib meson ninja pkgconf`, then
`make hw` / `make ipod` / `make sim` from `core/`. The bootloader
(ipodloader2) stays installed and chainloads our `.ipod`.

## What works on the device today

- **Bring-up** — cold boot + MMAP0 SDRAM remap, clock/PLL (30 MHz, refcounted
  80 MHz boost), IRQ + 100 Hz timer, unified-cache management.
- **Display** — BCM framebuffer present path; on-screen console for cable-free
  debug; the full RGB565 UI.
- **Audio** — WM8758B bring-up over I²C, I²S transport, DMA-driven continuous
  playback fed by an SPSC PCM ring drained by the DMA-completion ISR.
- **Storage** — PIO ATA reader (aligned bulk reads straight into the caller
  buffer) + from-scratch read-only FAT32 (long names decoded to UTF-8).
- **Streaming decode** — `dr_flac` / `dr_mp3` freestanding, fed by a read-ahead
  disk source; a full-length track streams off the disk while the UI stays live.
- **Library** — host-built `CORELIB.IDX` loads in one read → instant Songs /
  Albums / Artists / Genres; records carry UTF-8 fields + a normalized-name hash
  that binds each to its file independent of quote/case style (falls back to a
  per-file tag scan if the index is absent).
- **Full UTF-8 names** — atlas covers Latin-1 + smart punctuation, the text
  renderer decodes UTF-8, and display sources tag text so FAT-illegal characters
  (`?,*,:,/`) show correctly.
- **Browsing UI** — main menu, Music submenu, Artists / Albums / Songs / Genres,
  album detail (hero art + tracklist with per-disc sections + durations), with a
  scrolling marquee for long titles and taller two-line rows carrying 28px
  album-art chips.
- **Now Playing** — 120×120 cover, title/artist/album, TRACK N OF M, elapsed /
  −remaining, a rounded progress bar, shuffle/repeat tokens, battery.
- **Overlays** — volume (skinny-wave speaker icon + fill bar + %), lock/unlock
  padlock modals, charging screen, boot splash. Anti-aliased modal/progress
  corners.
- **Settings** — Playback (shuffle / repeat), Sound (volume / bass / treble /
  balance via the WM8758 EQ), Theme (Linen ↔ Onyx live palette swap), Display
  (backlight timeout), Clicker (7 piezo click profiles), About (dashboard:
  song/album/artist counts, storage, battery), Reset.
- **Battery gauge** — proportional fill, turns red at ≤20%.

## What's NOT done (pick up next)

1. **Settings persistence** — designed
   (`core/docs/design/settings-persistence.md`), not built. The firmware's FAT
   is read-only, so persisting settings needs an ATA/FAT write path. Highest-value
   next feature.
2. **Playlists / Podcasts / Audiobooks / Composers** — greyed placeholders in the
   menus; no backing implementation yet.
3. **More codecs** — AAC / ALAC / Vorbis / Opus / WAV are stubbed in
   `codecs/README.md`; only FLAC + MP3 are wired.
4. **Search** — not implemented.
5. **On-device screenshot capture** — the README shots are faithful renders
   (`docs/screens/render.py`); the SDL sim's ATA is a stub, so it can't load a
   library to capture real screens. A sim disk backing (or a device capture path)
   would enable true screenshots.
6. **Library sync is manual** — build the index on the host (`tools/build_index.py`),
   convert art (`tools/coreart.py`), and copy to the device.

## Testing

`meson test -C build-sim` from `core/` (currently **25/25** green):

- **Codec KAT** — FLAC + MP3 decoders bit-exact against reference PCM.
- **MMIO golden traces** — each freestanding hw driver is host-compiled against a
  recording mock bus (`-DMMIO_MOCK`) and asserted to emit its exact ordered
  register grammar (I²C, WM8758 bring-up, I²S, DMA, LCD, UART, clock, timer).
  This is the automated safety net for code that would otherwise need a logic
  analyzer.
- **FAT32 / readahead / scheduler / console / settings / clickwheel** — unit tests.

The device build (`make hw` / `make ipod`) is the authoritative compile for the
firmware; the clang lints about `hw/pp5022.h` / `LCD_WIDTH` / `mmio_*` are
include-path noise from editor tooling, not build errors.

## Repository

GitHub: https://github.com/BrandonDedolph/ipod-core — `main` is current
(all of Phase 1 + Phase 2 merged). See the README for the overview,
`core/README.md` for the firmware build, and `PLAN.md` for the roadmap.
