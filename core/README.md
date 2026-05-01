# core — custom firmware for iPod Video 5G/5.5G

A from-scratch firmware that ships **Cabinet** as the shell and **Linen**
as the only theme. Replaces Rockbox entirely on the device.

See [`../PLAN.md`](../PLAN.md) for the full plan: goals, performance
budget, quality bar, design principles, codec stack, install/update flow,
and phased timeline.

## Layout

```
core/
├── boot/        crt0.S, image header, linker script
├── kernel/      cooperative tasks, IPC, static memory, IRQs
├── hal/
│   ├── hal.h    contract
│   ├── hw/      ARM target — touches real registers
│   └── sim/     host target — backed by SDL + a disk-image file
├── fs/          FAT32 (vendored FatFs + adapter)
├── codecs/      Helix MP3/AAC, dr_flac, ALAC, Tremor, libopus, WAV
├── apps/
│   ├── audio/   engine, DSP, replaygain
│   ├── db/      tagcache build/query
│   └── ui/      Cabinet shell + Linen renderer
├── cli/         the `core` Go binary (host tooling)
├── sim/         SDL-based simulator main + glue
├── docs/hw/     hardware reference (phase-0 deliverable)
├── tests/       golden frames, codec vectors, scripted scenarios
├── cross/       Meson cross files (arm-none-eabi)
└── scripts/     build helpers
```

## Quick start

```bash
make sim    # builds the simulator (host)
make hw     # builds the firmware image (ARM)
make help   # see all targets
```

The Go CLI (`core`) and the C build are separate: Go talks to the host;
C is what ends up on the iPod (or the host sim).

## Status

Phase 0 — hardware reference doc (in progress).

See `../PLAN.md` for the phased roadmap.
