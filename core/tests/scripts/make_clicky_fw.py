#!/usr/bin/env python3
"""
make_clicky_fw.py — wrap core.bin in an iPod firmware-partition image for
the clicky emulator's HLE bootloader.

WHY THIS EXISTS. On the device our firmware IS the OSOS image: ipodpatcher
writes core.bin into the existing firmware partition, whose Apple preamble
and directory are already there. The clicky emulator has no such partition
to write into — its `--hle` flag takes a whole firmware-partition image,
parses the directory, and loads the `osos` entry into SDRAM at the entry's
load address (the native 0x10000000 window, remap NOT done — exactly the
state core/docs/hw/08-boot-dock.md, "Boot ROM -> our code: handoff state"
describes, and exactly what boot/crt0.S is written to inherit). So this
script builds the minimal partition image that directory needs around
core.bin: a preamble, the `]ih[` marker + locator at 0x100, one 40-byte
`soso` entry, a terminating all-zero entry, and the image body.

The layout is taken from OUR doc (08-boot-dock.md, "The firmware
partition") — it is not a port of make_fw or ipodpatcher, and no third-
party code was consulted for the byte layout. Two deliberate deviations
from what a real partition carries:

  - The preamble is our own banner, not Apple's copyright string. The
    boot ROM insists on Apple's; the emulator's HLE loader skips the
    first 256 bytes without looking. We cannot ship Apple's text, and we
    do not need to.

  - The image body sits at devOffset + 0x200. 08-boot-dock.md records
    (measured on the device, 2026-07-27) that the REAL partition places
    it at devOffset + 0x800. The emulator's HLE loader uses the older
    +0x200 convention, and this file only ever feeds the emulator, so it
    follows the emulator. Do not reuse this script to build something for
    ipodpatcher — that is `core firmware pack` (core/cli), a different
    format (.ipod: checksum + model name + raw image).

Usage: make_clicky_fw.py <core.bin> <out.fw>
"""

import struct
import sys

SDRAM_NATIVE = 0x10000000          # 01-soc-pp5022.md: SDRAM before the MMAP0 remap
DIR_MARKER   = b"]ih["             # "[hi]" byte-reversed on disk (08-boot-dock.md)
DIR_VERSION  = 3                   # directory format version at 0x10A; 5G sees 2 or 3
DIR_AT       = 0x0400              # where the first 40-byte entry lives in the file
BODY_AT      = 0x5000              # where the image bytes start in the file
ENTRY_LEN    = 40


def additive_checksum(data: bytes) -> int:
    # 08-boot-dock.md, "Checksum algorithm": plain byte sum, 32-bit wrap,
    # NOT seeded with the model number (measured on device 2026-07-27).
    return sum(data) & 0xFFFFFFFF


def build(image: bytes) -> bytes:
    out = bytearray(BODY_AT)

    banner = b"core firmware - clicky emulator smoke image (not for ipodpatcher)\n"
    out[0:len(banner)] = banner

    # Directory locator at 0x100: marker, then LE32 (directory - 0x200),
    # then a u16 we leave zero, then the u16 format version at 0x10A.
    out[0x100:0x104] = DIR_MARKER
    struct.pack_into("<IHH", out, 0x104, DIR_AT - 0x200, 0, DIR_VERSION)

    # One image directory entry — the 40-byte row from 08-boot-dock.md,
    # "Image directory entry". Tags are stored byte-reversed: the row for
    # the OSOS image spells "soso" and the ATA container spells "!ATA".
    entry = struct.pack(
        "<4s4sIIIIIIII",
        b"!ATA",                # container ID
        b"soso",                # image type: OSOS
        0,                      # image ID
        BODY_AT - 0x200,        # devOffset (emulator adds 0x200 back)
        len(image),             # len
        SDRAM_NATIVE,           # addr: load at native SDRAM, pre-remap
        0,                      # entryOffset: 0 = entry at image start (_start)
        additive_checksum(image),
        0,                      # vers
        SDRAM_NATIVE,           # loadAddr (secondary)
    )
    assert len(entry) == ENTRY_LEN
    out[DIR_AT:DIR_AT + ENTRY_LEN] = entry
    # The all-zero entry that follows terminates the directory.

    out += image
    return bytes(out)


def main(argv):
    if len(argv) != 3:
        sys.exit(__doc__)
    with open(argv[1], "rb") as f:
        image = f.read()
    if len(image) < 32:
        sys.exit(f"{argv[1]}: too small to hold a vector table")
    with open(argv[2], "wb") as f:
        f.write(build(image))
    print(f"{argv[2]}: osos {len(image)} bytes @ file+0x{BODY_AT:x}, "
          f"load 0x{SDRAM_NATIVE:08x}, checksum 0x{additive_checksum(image):08x}")


if __name__ == "__main__":
    main(sys.argv)
