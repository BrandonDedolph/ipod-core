#!/usr/bin/env python3
"""
check_hw_consistency.py — static cross-check of hal/hw/pp5022.h against
the Phase-0 hardware reference (docs/hw/) plus header-internal address
invariants.

Two independently-maintained sources describe the same silicon: the
prose/tables in docs/hw/ (the cleanroom artifact) and the C constants in
pp5022.h. This asserts they agree, and that the header's own address
arithmetic (register strides, port spacing) is internally sound. It is
the check that catches the exact bug class this project has hit twice —
a dropped zero in an address (0x200FFE00 -> 0x20FFE00) and a byte-vs-
word register stride — without needing to run anything on hardware.

It does NOT prove the values match real silicon (a typo shared by doc
and header survives); that residue is what the on-device bring-up
confirms. Pure source parsing, no build required.

Exit non-zero on any inconsistency; prints every check for transparency.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORE = os.path.normpath(os.path.join(HERE, "..", ".."))
HEADER = os.path.join(CORE, "hal", "hw", "pp5022.h")
DOCDIR = os.path.join(CORE, "docs", "hw")

# Every real MMIO address in this SoC lives at/above this line; bit
# masks, register indices and divisor values sit well below it. The
# split lets us tell an "address" token apart from a "value" token in
# free-form doc prose without hand-maintaining a list.
ADDR_FLOOR = 0x00100000

# Known MMIO windows (inclusive lo, exclusive hi). A *_ADDR outside all
# of these is almost certainly a dropped/added hex digit.
WINDOWS = [
    (0x30000000, 0x30080000),   # BCM video coprocessor ports
    (0x60000000, 0x60100000),   # system / device-control / GPIO
    (0x70000000, 0x70100000),   # GPO32 / SER0 / misc peripherals
    (0xF000F000, 0xF000F008),   # MMAP0 remap pair
]


def fail(msgs, m):
    msgs.append("FAIL: " + m)


def parse_header_addrs():
    """Return {NAME_ADDR: intvalue} from #define lines."""
    out = {}
    rx = re.compile(r"#define\s+(\w+_ADDR)\s+(0x[0-9A-Fa-f]+)")
    with open(HEADER) as f:
        for line in f:
            m = rx.search(line)
            if m:
                out[m.group(1)] = int(m.group(2), 16)
    return out


def doc_addr_tokens_by_symbol():
    """
    Scan every doc line; for each backtick-quoted `SYMBOL` on the line,
    associate every backtick-quoted `0xADDR` (>= ADDR_FLOOR) also on
    that line. Returns {symbol: set(addrs)}.
    """
    sym_rx = re.compile(r"`([A-Z][A-Z0-9_]+)`")
    hex_rx = re.compile(r"`(0x[0-9A-Fa-f]+)`")
    assoc = {}
    for name in sorted(os.listdir(DOCDIR)):
        if not name.endswith(".md"):
            continue
        with open(os.path.join(DOCDIR, name)) as f:
            for line in f:
                addrs = {int(h, 16) for h in hex_rx.findall(line)
                         if int(h, 16) >= ADDR_FLOOR}
                if not addrs:
                    continue
                for sym in sym_rx.findall(line):
                    assoc.setdefault(sym, set()).update(addrs)
    return assoc


def main():
    msgs = []
    header = parse_header_addrs()
    if not header:
        print("FAIL: no *_ADDR defines parsed from", HEADER)
        return 1

    # (1) Every address sits inside a known MMIO window.
    for name, val in sorted(header.items()):
        if not any(lo <= val < hi for lo, hi in WINDOWS):
            fail(msgs, f"{name} = 0x{val:08X} is outside every known "
                       f"MMIO window (dropped/added hex digit?)")

    # (2) Header <-> doc cross-check. Compare only where the doc tables/
    #     prose actually pin an address to the same symbol name.
    doc = doc_addr_tokens_by_symbol()
    checked = unchecked = 0
    for name, val in sorted(header.items()):
        base = name[:-len("_ADDR")]
        seen = doc.get(base)
        if not seen:
            unchecked += 1
            continue
        checked += 1
        if val not in seen:
            fail(msgs, f"{name} = 0x{val:08X} but docs/hw associates "
                       f"`{base}` with {{{', '.join(f'0x{a:08X}' for a in sorted(seen))}}}")

    # (3) Header-internal invariants: register strides / port spacing.
    def eq(name_a, name_b, delta):
        if name_a in header and name_b in header:
            if header[name_a] + delta != header[name_b]:
                fail(msgs, f"{name_b} != {name_a}+0x{delta:X} "
                           f"(0x{header[name_b]:08X} vs "
                           f"0x{header[name_a] + delta:08X})")

    # SER0 is an 8250 with every byte register widened to a 4-byte slot.
    eq("SER0_RBR_ADDR", "SER0_IER_ADDR", 0x04)
    eq("SER0_RBR_ADDR", "SER0_IIR_ADDR", 0x08)
    eq("SER0_RBR_ADDR", "SER0_LCR_ADDR", 0x0C)
    eq("SER0_RBR_ADDR", "SER0_MCR_ADDR", 0x10)
    eq("SER0_RBR_ADDR", "SER0_LSR_ADDR", 0x14)
    eq("SER0_RBR_ADDR", "SER0_MSR_ADDR", 0x18)
    # GPO32 value/enable are an adjacent pair.
    eq("GPO32_VAL_ADDR", "GPO32_ENABLE_ADDR", 0x04)
    # MMAP0 logical/physical are an adjacent pair.
    eq("MMAP0_LOGICAL_ADDR", "MMAP0_PHYSICAL_ADDR", 0x04)
    # BCM ports are spaced one decoded-bit (0x10000) apart, in order.
    bcm = ["BCM_DATA_ADDR", "BCM_WR_ADDR_ADDR", "BCM_RD_ADDR_ADDR",
           "BCM_CONTROL_ADDR", "BCM_ALT_DATA_ADDR", "BCM_ALT_WR_ADDR_ADDR",
           "BCM_ALT_RD_ADDR_ADDR", "BCM_ALT_CONTROL_ADDR"]
    for i in range(1, len(bcm)):
        eq(bcm[0], bcm[i], 0x10000 * i)

    print(f"pp5022.h: {len(header)} *_ADDR constants")
    print(f"  window check:     all inside known MMIO windows")
    print(f"  doc cross-check:  {checked} matched docs/hw, "
          f"{unchecked} prose-only/not tabulated")
    print(f"  stride invariants: SER0 x6, GPO32, MMAP0, BCM x7")
    if msgs:
        print()
        for m in msgs:
            print(m)
        print(f"\n{len(msgs)} inconsistenc" +
              ("y" if len(msgs) == 1 else "ies"))
        return 1
    print("OK: header and docs/hw are consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
