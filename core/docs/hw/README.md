# iPod Video 5G/5.5G — Hardware Reference

A consolidated hardware reference for the iPod Video 5G and 5.5G,
written for someone who needs to drive every register and replicate
every sequence from scratch. Reverse-engineered from the Rockbox source
tree at the time of writing, plus the iPodLinux wiki and the leaked
PortalPlayer documentation; cross-referenced where possible.

> **Status:** Phase 0 deliverable. This document stands on its own —
> even if no firmware is ever built on top of it, the iPod 5G modding
> community now has the cleanest single reference covering this device.

## Table of contents

| File | Subsystem |
|---|---|
| [01-soc-pp5022.md](01-soc-pp5022.md) | PortalPlayer PP5022 SoC: memory map, clocks, dual-core, IRQs, cache |
| [02-lcd.md](02-lcd.md)               | Broadcom BCM video coprocessor + LCD panel (5G / 5.5G) |
| [03-clickwheel.md](03-clickwheel.md) | Touch wheel + buttons + hold switch |
| [04-ata.md](04-ata.md)               | Integrated PIO/UDMA ATA controller, HDD power management |
| [05-audio.md](05-audio.md)           | I²S transport, DMA chain, Wolfson WM8758 DAC |
| [06-power.md](06-power.md)           | PCF50605 PMIC, battery curve, charge detection, sleep |
| [07-usb.md](07-usb.md)               | ARC USBOTG controller, MSC stack, exclusive-storage handoff |
| [08-boot-dock.md](08-boot-dock.md)   | Firmware partition format, bootloader handoff, dock UART, recovery |
| [09-i2c.md](09-i2c.md)               | On-SoC I²C controller (WM8758 codec control bus) |

## Conventions

- Register addresses are in MMIO space at `0x60000000`–`0x700FFFFF`
  unless otherwise noted.
- Bit and byte numbering follows the Rockbox source convention: bit 0
  is the LSB, byte 0 is the lowest-address byte.
- "PP5022" and "PP502x" are used somewhat interchangeably — the iPod
  Video uses the PP5022 specifically, but most of the platform-shared
  code lives in `firmware/target/arm/pp/` and applies to PP5020/5022/5024
  uniformly.
- 5G and 5.5G differ in panel gamma, max storage size, and a few
  GPIO assignments. They share one Rockbox build with runtime branches.

## Source map

These files in the Rockbox tree (commit current as of phase-0 research)
are the load-bearing references. Citations in each subsystem doc point
back to specific functions and line ranges.

```
firmware/target/arm/pp/                 Shared PortalPlayer platform code
firmware/target/arm/ipod/               Shared iPod-specific code
firmware/target/arm/ipod/video/         iPod Video 5G/5.5G specific
firmware/export/pp5020.h                PP5020/5022 register definitions
firmware/export/config/ipodvideo.h      Video target compile config
firmware/drivers/ata.c                  Generic ATA driver layer
firmware/drivers/audio/wm8758.c         Wolfson DAC driver
firmware/drivers/pcf50605.c             PMIC driver
firmware/usbstack/                      USB device stack + MSC class
firmware/target/arm/usb-drv-arc.c       ARC USBOTG controller driver
bootloader/ipod.c                       Bootloader entry & dispatch
utils/ipodpatcher/ipodpatcher.c         Firmware-partition installer
```

## What's intentionally out of scope here

- **FireWire** — the 5G/5.5G has a FireWire dock pinout but Rockbox
  doesn't drive it. Not researched.
- **Video encoder / TV out** — the BCM video coprocessor can produce
  PAL/NTSC composite, but we won't use it. Documented at a surface
  level in 02-lcd.md only.
- **Apple firmware ("OF") internals** — covered only where the boot
  ROM's expectations affect us (image format, partition signature).

## Caveats

- Some registers below are documented only in source comments. Where
  the Rockbox source says `/* unknown */`, this doc says so explicitly.
- Magic constants for the BCM video coprocessor's bootstrap (in 02-lcd.md)
  came from iPodLinux reverse-engineering and have never been validated
  against an Apple datasheet. Treat them as "known-working incantations,"
  not "specified values."
- Battery curves (in 06-power.md) are calibrated to the original 2005
  cell chemistry. Users with replacement cells from 2024+ may see
  drift at the bottom 10% of the curve.
