# core — firmware (device + host build)

The bare-metal firmware for the iPod 5.5G and its host test build. From
scratch, Apache-2.0, no RTOS and no libc on the device — it **replaces
Rockbox** rather than theming it. See [`../README.md`](../README.md) for the
project overview and [`../PLAN.md`](../PLAN.md) for the phased roadmap.

## Layout

```
core/
├── boot/        crt0.S, .ipod image header, linker script
├── kernel/      cooperative scheduler, IRQ, timer, clock, cache,
│                framebuffer console, PCM ring — and main.c (the UI)
├── hal/
│   ├── hal.h    hardware contract shared by both backends
│   ├── hw/      ARM drivers: lcd, ata, i2c, i2s, wm8758, dma, audio,
│   │            clickwheel, backlight, battery, power, piezo, uart
│   └── sim/     host backend (SDL2)
├── fs/          from-scratch read-only FAT32 reader (LFN → UTF-8)
├── lib/         freestanding mem.c (memcpy/memset)
├── codecs/      dr_flac + dr_mp3 (freestanding), static arena, read-ahead
│                disk source, FLAC metadata reader, unified decoder ABI
├── ui/          gamma-correct AA text renderer + Nunito atlases, the
│                runtime palette (Linen/Onyx), art thumbnail cache,
│                settings model, and per-screen renderers
├── cli/         Go host CLI (`core` — .ipod firmware pack/unpack, sim)
├── docs/hw/     hardware reference the drivers were written against
├── cross/       Meson cross file (arm-none-eabi)
└── tests/       host unit + MMIO golden-trace tests
```

The player UI lives in `kernel/main.c` (the menu / browser / now-playing
loop) on top of the `ui/` primitives; there is no separate app layer.

## Prerequisites

- `meson` ≥ 0.62, `ninja`, `pkg-config`
- `libsdl2-dev` / `sdl2` — for the host (`sim`) HAL
- `arm-none-eabi-gcc` 13+ with `binutils` and `newlib` — for the device build
- `go` — for the host CLI (`.ipod` packaging)

On Arch: `pacman -S arm-none-eabi-gcc arm-none-eabi-binutils
arm-none-eabi-newlib meson ninja pkgconf sdl2 go`.

## Quick start

```bash
make hw      # → build-hw/core.elf, build-hw/core.bin  (ARMv4T bare metal)
make ipod    # → build-hw/core.ipod  (transport-wrapped image to flash)
make sim     # configures + builds the host target (SDL2 HAL)
make help    # all targets

meson test -C build-sim     # host unit + MMIO golden-trace tests
```

The host (`sim`) target compiles the same freestanding driver, codec, and
text-renderer sources the device links, plus MMIO golden-trace tests that
assert each driver's exact register grammar against a recording mock bus —
the automated safety net for code that would otherwise need a logic
analyzer to verify.

## Audio path

`dr_flac` / `dr_mp3` are compiled freestanding (`-DCORE_FREESTANDING`) and
fed by a read-ahead disk source into an SPSC PCM ring drained by the
DMA-completion ISR. Streaming, not preload — a full-length track plays off
the iPod's own disk while the UI stays responsive. Output is always 16-bit
signed interleaved PCM (see [`codecs/README.md`](codecs/README.md)).

## Library index

The FAT volume is read-only to the firmware, so the song library is built
on the host by [`../tools/build_index.py`](../tools/build_index.py) into a
single `CORELIB.IDX` the firmware loads in one read — instant Songs / Albums
/ Genres with no per-file tag scan at boot. Records carry UTF-8 display
fields plus a normalized-name hash that binds each record to its file on
disk independent of quote/case style. If the index is absent the firmware
falls back to a per-file tag scan.

## Status

Runs on real iPod 5.5G hardware: boot + bring-up, LCD, click-wheel, backlight,
WM8758B audio, DMA streaming playback, ATA + FAT32, and streaming FLAC/MP3 off
the device's disk, with the full menu / browser / now-playing UI. See
[`../STATUS.md`](../STATUS.md) for the running list.
