# core — firmware (device + host build)

The bare-metal firmware for the iPod 5.5G and its host test build. From
scratch, Apache-2.0, no RTOS and no libc on the device — an independent,
modern player experience on the original hardware (not a Rockbox theme or
plugin; no copied Rockbox code). See [`../README.md`](../README.md) for the
project overview and [`../PLAN.md`](../PLAN.md) for the phased roadmap.

## Layout

```
core/
├── boot/        crt0.S, .ipod image header, linker script
├── kernel/      cooperative scheduler, IRQ, timer, clock, cache,
│                framebuffer console, PCM ring, panic/fault handlers,
│                CORECFG.DAT settings store, the CORELOG.BIN event log
│                — and main.c (the UI)
├── hal/
│   ├── hal.h    hardware contract shared by both backends
│   ├── hw/      ARM drivers: lcd, ata, i2c, i2s, wm8758, dma, audio,
│   │            clickwheel, backlight, battery, power, volume, piezo, uart
│   └── sim/     host backend (SDL2)
├── fs/          from-scratch read-only FAT32 reader (LFN → UTF-8) with a
│                bounded path walk, plus an M3U8 playlist reader
├── lib/         freestanding mem.c (memcpy/memset)
├── codecs/      dr_flac + dr_mp3 (freestanding), static arena, read-ahead
│                disk source, FLAC metadata reader, unified decoder ABI
├── library/     CORELIB.IDX loader, artists/genres, playlist rows
├── player/      queue + transport engine (open/next/prev/seek/end-of-queue)
├── ui/          gamma-correct AA text renderer + Nunito atlases, the
│                runtime palette (seven themes), art thumbnail cache,
│                settings model, and per-screen renderers
├── cli/         Go host CLI (`core` — .ipod firmware pack/unpack, sim)
├── docs/hw/     hardware reference the drivers were written against
├── docs/design/ design notes (settings persistence, …)
├── cross/       Meson cross file (arm-none-eabi)
├── tools/       icon_gen.sh + source icons (UI glyph art)
└── tests/       host unit + MMIO golden-trace tests
```

The player UI lives in `kernel/main.c` (the menu / browser / now-playing
loop) on top of the `ui/` primitives and `player/`; there is no separate
app layer.

## Prerequisites

- `meson` ≥ 0.62, `ninja`, `pkg-config`
- `libsdl2-dev` / `sdl2` — for the host (`sim`) HAL
- `arm-none-eabi-gcc` **14+** with `binutils` and `newlib` — for the device
  build. Floor is 14, deliberately no ceiling: `meson.build` probes
  newer-compiler flags with `cc.get_supported_arguments()` rather than passing
  them blind, so the rolling Arch toolchain (16.x today) and the pinned CI
  reference build (Arm GNU Toolchain 14.2.rel1) both compile under `-Werror`.
  The pin and the reasoning live in `.tool-versions` and
  `.github/workflows/ci.yml`, not in a comment that can go stale.
- `go` — for the host CLI (`.ipod` packaging)

On Arch: `pacman -S arm-none-eabi-gcc arm-none-eabi-binutils
arm-none-eabi-newlib meson ninja pkgconf sdl2 go`.

## Quick start

```bash
make hw         # → build-hw/core.elf, build-hw/core.bin  (ARMv4T bare metal)
make ipod       # → build-hw/core.ipod  (transport-wrapped image to flash)
make sim        # configures + builds the HOST TEST SUITE (see note below)
make verify-hw  # static checks against a fresh `make hw` (see below)
make help       # all targets

meson test -C build-sim     # 63 host unit + MMIO golden-trace suites
```

`make verify-hw` is the static half of the safety net — the checks that
compare the built image against the docs and against the host tools rather
than executing anything: image layout (`check_hw_layout.sh`), size
(`check_size.sh`), `hal/hw/pp5022.h` against `docs/hw/*`
(`check_hw_consistency.py`), and the two host↔device parity checks for the
library name hash and the resume record.

> **`make sim` is the test build, not a simulator.** The target name predates
> what it does: past `hal/`, `meson.build`'s `sim` branch descends into
> `codecs/` and `tests/` and nothing else. `tests/` reaches back into
> individual device sources it wants to exercise (`ui/text.c`, `ui/thumb.c`,
> `ui/settings.c`, the drivers), but `kernel/main.c` — the UI — is never
> built for the host, and `hal/sim/sim_hal.c` compiles into a static library
> that **no executable links**. There is nothing to run: no SDL2 window, no
> host player. The device is still the only place the UI can be seen.

The host (`sim`) target compiles the same freestanding driver, codec, and
text-renderer sources the device links, plus MMIO golden-trace tests that
assert each driver's exact register grammar against a recording mock bus —
the automated safety net for code that would otherwise need a logic
analyzer to verify.

## Boot — direct, no chainloader

**Our firmware IS the OSOS image.** It is written into the iPod's firmware
partition, in place of Apple's OS, and the Apple boot ROM enters `crt0.S`
directly. There is no chainloader on the device, no ipodloader2, and no boot
menu — power on and you are in our UI.

That makes `crt0.S` responsible for state a chainloader used to hand us:
the boot ROM enters us with SDRAM still at its native `0x10000000` and the
linker linking everything at `0x0`, so crt0 performs the MMAP0 remap itself,
from an IRAM-resident stub (the code doing the remap must not be affected by
it), and everything before that point is position-independent. The same
change removed the display's free ride: `lcd.c` can no longer assume a
loader left the BCM powered, bootstrapped and idle at frame one, so it
probes and, on a wedged BCM, power-cycles via `bcm_init()`.

```bash
make ipod                       # → build-hw/core.ipod
ipodpatcher <n> -wf build-hw/core.ipod    # write it as the firmware image
ipodpatcher <n> -rfb read-back.bin        # read back and compare — always
```

**Recovery is the boot ROM's disk mode: hold Select + Play at power-on.**
It lives in ROM and runs *before* any firmware image is loaded, so it works
no matter how badly the image we wrote is broken. That unconditional escape
hatch is what makes flashing safe; nothing we ship may write the boot ROM or
disturb the partition signature. See
[`docs/hw/08-boot-dock.md`](docs/hw/08-boot-dock.md).

## Settings and resume

Settings persist across reboots in `CORECFG.DAT`, a pre-allocated file in the
volume root. The firmware's FAT driver is read-only and cannot *create*
files, so the file is created once on the host by
[`../tools/make_config.py`](../tools/make_config.py) and thereafter the
firmware overwrites its sectors in place, by absolute LBA, alternating
between two slots so a power loss mid-write always leaves one good record.
`--verify` prints the LBA the firmware must agree on and is the mandatory
pre-flight before the first write on a given device.

The record is v2 and 48 payload bytes: v1 held settings only, v2 appends a
resume locator (name hash + elapsed seconds + track length), the kind of queue
the track was playing in with the shuffle seeds, and the sound tail (volume
limit + EQ preset). Each tail was appended under the same version with
`length` gating it, so a record written by any earlier build still loads. The
shuffle byte itself widened in place to carry the three-way mode (off / songs
/ albums) since 2026-09-16 — same offset, same version: an older build reads
an albums record as Songs, and this one reads a value it does not know as Off.
With **Resume** enabled, a cold boot reopens the track you were on, in that
queue, and seeks to where you left off, **paused** — never surprising you
with audio at boot.

## Event log

`CORELOG.BIN` is a 4 MiB ring of 2048-byte blocks, created on the host by
[`../tools/make_log.py`](../tools/make_log.py) (`--create`), that captures
every `core:` line the UART would carry: a 16 KiB RAM ring in
`kernel/evlog.c` is flushed one block at a time through the same write gate
as the settings file — a full block at idle, a forced FINAL block at sleep,
power-off and Disk Mode entry, the last one at the disk-safe battery edge.
Block 0 is a validated header the device never writes; every other block
carries a sequence number, boot number, length and CRC-32, and the cursor
is found at boot by a binary search over the ring. `--dump` reads a pulled
copy back per boot; About shows `LOG <seq> on`, Boot Details the header and
next-flush LBAs.

## Sleep and power-off

Hold PLAY for two seconds: the player pauses, settings and position are
force-committed, the log flushes, the drive parks, the panel blanks white
and the backlight drops, the codec powers down, and the main loop halts
between wheel samples until a button is pressed; wake re-boosts the CPU,
spins the drive,
repaints while dark and only then lights the backlight. Hold past five
seconds for PMU standby (a true off; any button cold-boots). Asleep on
battery the device keeps sampling the cell and escalates to standby after
thirty minutes or at the shut-off edge.

Settings > Playback > Sleep Timer takes the SAME path on a countdown
(`ui/sleeptimer.c`, a wrap-safe minute accumulator over the 1 MHz
USEC_TIMER — a 120-minute timer crosses the 32-bit wrap twice, which is
why it is a host-tested unit). The only difference is that the expiry
pauses the player BEFORE calling `suspend_to_ram`, so `was_playing` is 0
and the wake comes back paused rather than resuming. The chosen duration
is runtime-only: it never reaches `CORECFG.DAT`, so arming it costs no
disk write and it reads Off after every boot. The deeper savings (`SUSPEND_*`
switches at the top of `kernel/main.c`: PLL park, peripheral clock gates,
ATA SLEEP instead of STANDBY, panel sleep) are compiled out until each is
proven alone on the device.

## Audio path

`dr_flac` / `dr_mp3` are compiled freestanding (`-DCORE_FREESTANDING`) and
fed by a read-ahead disk source into an SPSC PCM ring drained by the
DMA-completion ISR. Streaming, not preload — a full-length track plays off
the iPod's own disk while the UI stays responsive.

Only FLAC is reachable on the device today: `kernel/main.c` sets
`CORE_ENABLE_MP3 0` and `classify_ext()` does not surface `.mp3` at all, so
MP3 files are invisible in the browser. `dr_mp3` is still built and linked
(and its decode path is covered by the host KATs) — it is parked, not
removed, because its float synthesis filter cannot keep up on this FPU-less
CPU and the ring starves. Output is always 16-bit
signed interleaved PCM (see [`codecs/README.md`](codecs/README.md)).

## Library index

The FAT volume is read-only to the firmware, so the song library is built
on the host by [`../tools/build_index.py`](../tools/build_index.py) into a
single `CORELIB.IDX` the firmware loads in one read — instant Songs / Albums
/ Genres with no per-file tag scan at boot. Records carry UTF-8 display
fields plus a normalized-name hash that binds each record to its file on
disk independent of quote/case style. If the index is absent the firmware
falls back to a per-file tag scan.

## Playlists — read only

Put `.m3u8` (or `.m3u`) files in `Music/Playlists/` — `Playlists/` at the
volume root on a disk with no `Music/` folder. Entries may be absolute from
the volume root (`/Music/Artist - Album/01 Song.flac`) or relative to that
folder (`../Artist - Album/01 Song.flac`); `\` separators and a drive
letter are tolerated. Music → Playlists lists them by filename (extension
trimmed), A–Z, re-read on every entry; open one for its tracklist, SELECT
plays the whole playlist from that row; a playlist queue resumes at boot.

`fs/m3u.c` parses (unit-tested against the malformed files real libraries
contain), `fat32_resolve_path()` walks each entry to its directory entry,
and `library/playlist.c` turns the result into rows named and located
exactly as an album's tracklist rows are, so they bind to the same index
records. Entries that are missing, not audio, or unreadable are skipped and
counted, never fatal. Caps: 64 playlists, 128 tracks per playlist — the
first that many, with the overflow reported. Host-tested end to end on an
in-RAM volume (`tests/library/playlist_test.c`); **not yet flashed**.

Playlist *writing* does not exist and cannot until the FAT driver can
allocate clusters, which it cannot — it is read-only by design today.

## Status

Runs on real iPod 5.5G hardware, booting directly from the firmware
partition: boot + MMAP0 remap, LCD, click-wheel, backlight, WM8758B audio,
DMA streaming playback, ATA + FAT32, and streaming FLAC off the device's disk
(MP3 is parked — see "Audio path"), with the full menu / browser /
now-playing UI, seven themes, persistent settings, resume-on-boot, sleep /
power-off from the Play button, and the on-disk event log.

Not yet confirmed on hardware: gapless playback, playlists with real
`.m3u8` files, and the post-fix FLAC seek timing (the fix is in — see
[`codecs/README.md`](codecs/README.md) — but the improvement has not been
re-measured on the device). The charger is held at 100 mA. See
[`../STATUS.md`](../STATUS.md) for the running list.
