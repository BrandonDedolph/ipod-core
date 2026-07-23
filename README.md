# core — cleanroom firmware for the iPod 5.5G

`core` is a **from-scratch, Apache-2.0 firmware** for the 5th-generation
iPod ("iPod Video", PortalPlayer PP5022). It boots on real hardware,
reads music off the iPod's own disk, and plays it back through a custom
Nunito/Linen interface. It **replaces Rockbox entirely** — it is not a
Rockbox theme, patch, or plugin, and shares no Rockbox code.

> ### ▶ This runs on a real iPod. Not an emulator, not a simulator.
> It boots and plays on **actual 2006 Apple hardware** — a physical iPod
> 5.5G. Written from scratch in C and ARM assembly, flashed to the device,
> coming up from a cold start and streaming music straight off the iPod's
> own hard drive. Bare metal: no OS, no libc, nothing between this code and
> the silicon. *(The screenshots and GIF below are faithful renders of the
> on-device UI — the panel is hard to photograph cleanly.)*

The whole bare-metal stack is proven end-to-end on an actual iPod 5.5G:
boot + memory remap → clock/PLL → timer/IRQ → LCD (BCM framebuffer
present) → I²C/WM8758B/I²S first sound → DMA playback → ATA PIO reader →
FAT32 → streaming FLAC/MP3 decode → audio out the headphone jack.

<p align="center">
  <img src="docs/screens/demo.gif" alt="core UI in motion" width="360">
</p>

<table>
  <tr>
    <td><img src="docs/screens/nowplaying.png" width="320" alt="Now Playing"></td>
    <td><img src="docs/screens/albums.png" width="320" alt="Album list"></td>
  </tr>
  <tr>
    <td><img src="docs/screens/detail.png" width="320" alt="Album detail"></td>
    <td><img src="docs/screens/genres.png" width="320" alt="Genres"></td>
  </tr>
  <tr>
    <td><img src="docs/screens/about.png" width="320" alt="About"></td>
    <td><img src="docs/screens/volume.png" width="320" alt="Volume overlay"></td>
  </tr>
</table>

---

## What it is

- **Cleanroom.** Every driver and the freestanding codec/UI glue are
  written from hardware facts — PortalPlayer register maps, the WM8758B
  datasheet, the FAT32 spec — not from reading GPL sources. See
  [`core/docs/hw/`](core/docs/hw/) for the subsystem-by-subsystem
  hardware reference the drivers were written against.
- **Bare metal.** No RTOS, no libc on the device. A small cooperative
  kernel, a static-arena allocator, and our own `mem.c` back the
  freestanding decoders; integer division and soft-float come from the
  compiler runtime (`libgcc`), never libc.
- **Real audio.** `dr_flac` and `dr_mp3` are compiled freestanding
  (`-DCORE_FREESTANDING`) and fed by a read-ahead disk source into an
  SPSC PCM ring drained by the DMA-completion ISR. Streaming, not
  preload — a full-length track plays off the disk.
- **Real type.** `core/ui/text.c` is a libc-free, gamma-correct
  antialiased text renderer that draws pre-rasterized Nunito glyph
  atlases straight into the RGB565 framebuffer — no FreeType, no malloc,
  all `.rodata`. It decodes UTF-8 and covers Latin-1 + smart punctuation,
  so accented names and curly quotes render true.
- **A real library UI.** Browse by Album / Artist / Genre / Song off a
  host-built index (`CORELIB.IDX`) that loads in one read — album-art
  chips, a 120×120 now-playing cover, a scrolling marquee for long
  titles, a warm-light **Linen** theme and a warm-dark **Onyx** one,
  plus settings (tone/balance, backlight, click profiles), volume and
  lock overlays, and a battery gauge that warns red when low.

## Hardware target

| | |
|---|---|
| Device | iPod 5.5G (Video), 80 GB |
| SoC | PortalPlayer PP5022 (dual ARM7TDMI, ARMv4T) |
| Audio DAC | Wolfson WM8758B over I²C control + I²S data |
| Display | 320×240 LCD driven through the BCM framebuffer path |
| Storage | ATA disk (PIO), read-only FAT32 reader |
| Input | Apple click-wheel + buttons + hold switch (polled) |
| Chainload | [ipodloader2](https://github.com/crozone/ipodloader2) loads our `.ipod` image |

## Status

Working on real hardware today: boot and bring-up, LCD present, click-wheel
input, backlight, WM8758B first sound, DMA continuous playback, ATA + FAT32
read, and **streaming FLAC/MP3 playback off the iPod's own disk**. The
menu UI and Now Playing screens (embedded album art, progress) render on
device via the freestanding text renderer. There is no serial cable in the
loop — on-device state is confirmed through an on-screen framebuffer console.

See [`STATUS.md`](STATUS.md) for the running list of what works, what's
pending, and what to pick up next, and [`PLAN.md`](PLAN.md) for the phased
roadmap.

---

## Building

Requires `meson`, `ninja`, `pkg-config`, a C11 host compiler, and (for
the device build) `arm-none-eabi-gcc` with binutils + newlib. `libsdl2-dev`
backs the host HAL build. On Arch: `pacman -S arm-none-eabi-gcc
arm-none-eabi-binutils arm-none-eabi-newlib meson ninja pkgconf sdl2`.

```bash
cd core

# Device firmware — ARMv4T bare-metal ELF + flat binary
make hw            # → build-hw/core.elf, build-hw/core.bin
make ipod          # → build-hw/core.ipod (transport-wrapped image)

# Host build + unit tests (freestanding drivers/codecs, MMIO golden traces)
make sim           # configures + builds the host target
meson test -C build-sim
```

The host (`sim`) target compiles the same freestanding driver, codec, and
text-renderer sources the device links, plus the MMIO golden-trace tests
that assert each hardware driver's exact register grammar against a
recording mock bus — the automated safety net for code that otherwise
needs a logic analyzer to verify.

## Flashing

`core` is chainloaded, not installed over Apple's firmware. Install
[ipodloader2](https://github.com/crozone/ipodloader2) once, then copy the
built image to the FAT32 data partition and reboot:

```bash
make ipod
cp build-hw/core.ipod /path/to/ipod/          # FAT32 root
# eject, then boot — ipodloader2 chainloads core.ipod
```

---

## Repo layout

```
core/                     bare-metal firmware + host test build
├── boot/                 crt0, image header, linker script
├── kernel/               cooperative scheduler, IRQ, timer, clock, PCM ring
├── hal/
│   ├── hal.h             hardware contract
│   ├── hw/               ARM drivers — LCD, ATA, I²C, I²S, WM8758B, DMA,
│   │                     click-wheel, backlight, UART
│   └── sim/              host HAL backend (SDL2)
├── fs/                   from-scratch read-only FAT32 reader (LFN → UTF-8)
├── lib/                  freestanding mem.c (memcpy/memset)
├── codecs/               dr_flac + dr_mp3 (freestanding), static arena,
│                         read-ahead disk source, FLAC metadata reader
├── ui/                   AA text renderer + Nunito atlases, palette, art cache
├── cli/                  Go host CLI (.ipod firmware pack/unpack)
├── docs/hw/              hardware reference the drivers were written against
├── cross/                Meson cross file (arm-none-eabi)
└── tests/                host unit + MMIO golden-trace tests

design_reference/         UI design source — palette, chrome, icon paths
docs/screens/             interface screenshots + demo GIF (this README)
tools/                    host tooling — atlas + glyphmap generator, album-art
                          converter, library-index builder, font sources
```

See [`core/README.md`](core/README.md) for firmware-side build detail and
[`tools/README.md`](tools/README.md) for the host toolchain.

---

## License

Apache-2.0, first-party. The firmware is a cleanroom implementation: its
drivers and decoders are derived from hardware documentation, not from
GPL sources. Vendored codecs keep their permissive upstream licenses —
`dr_flac` / `dr_mp3` (public domain / MIT), `stb_image` (public domain / MIT).
The Nunito font is under the SIL Open Font License 1.1 (`tools/fonts-src/`).
