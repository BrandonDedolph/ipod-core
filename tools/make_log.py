#!/usr/bin/env python3
"""make_log.py — create, verify and dump CORELOG.BIN, the pre-allocated
on-disk event log the firmware writes its UART narration into.

WHY THIS TOOL EXISTS
--------------------
The firmware narrates everything it does over the dock UART (`core: ...`
lines: boot phases, the 5 s battery sample, suspend/standby steps, disk park
and wake, player opens and errors). On the bench that is the whole debug
story; in a pocket nobody is listening. kernel/evlog.c keeps the last 8 KiB
of that narration in RAM and, one 2048-byte block at a time, writes it into
a ring inside this file — so the disk holds what the UART would have said,
and a device that came back from a night in a bag can explain itself.

The firmware's FAT32 driver is READ-ONLY apart from overwriting bytes inside
files that already exist (kernel/config.c for settings; kernel/evlog.c for
this log). It cannot create, grow, move or delete anything, so the file has
to be pre-allocated here, through the host's real filesystem, exactly as
CORECFG.DAT is by make_config.py. The device then only ever rewrites whole
2048-byte blocks inside the file's own clusters: no FAT entry, no directory
entry, no free-count, no FSInfo is ever touched.

FILE FORMAT (must match core/kernel/evlog.c byte for byte)
----------------------------------------------------------
The file is `block_count` blocks of BLOCK bytes (2048 = one FAT sector on the
stock 80 GB volume = two of the drive's 1024-byte physical sectors, so every
block is a whole, physical-sector-aligned write). Block 0 is the header and
is written ONLY by this tool; blocks 1 .. block_count-1 are the ring.

  Header block (block 0), little-endian:
    off  size  field
    0    4     magic        'C' 'L' 'O' 'G'
    4    2     version      1
    6    2     block_size   2048
    8    4     block_count  blocks in the file INCLUDING this one
    12   4     file_id      random; tells two logs apart in a pile of dumps
    16   4     crc32        CRC-32 over bytes [0, 16)
    (rest zero)

  Ring block (block 1 + (seq % (block_count - 1))):
    0    4     magic        'C' 'L' 'O' 'B'
    4    4     seq          monotonic, 0 for the first block ever written
    8    2     boot         boot id: the previous newest block's boot + 1
    10   2     len          bits 0..14: text bytes that follow (<= 2032)
                            bit 15: FINAL — this block was a forced flush
                            (suspend, standby, the last write before a
                            low-battery power-off), so the session it
                            closes ended on purpose
    12   4     crc32        CRC-32 over bytes [0, 12) ++ [16, 2048) — the
                            whole block except this field
    16   2032  text         UART bytes, zero-padded

The CRC is the plain zlib / binascii one, as in config.c. A block that fails
magic or CRC is treated as unwritten by both the device and --dump.

WORKFLOW
--------
  1. Mount the iPod's FAT32 data partition and create the file ONCE, in the
     VOLUME ROOT, next to CORECFG.DAT:

         python3 tools/make_log.py --create /mnt/d

     4 MiB by default (2048 blocks: a header and 2047 ring blocks, ~4 MB of
     text — weeks of the 5 s battery line). An existing file with a valid
     header is left alone unless you pass --force; the ring's contents are
     kept either way.

     ON WINDOWS / WSL: writes through /mnt/d (drvfs) do NOT reliably reach
     the volume — the file can look written from Linux and be absent or
     empty on the device. Create it somewhere local and copy it with a
     Windows-native tool, then flush the volume before unplugging:

         python3 tools/make_log.py --create /tmp/logdir
         powershell.exe Copy-Item /tmp/logdir/CORELOG.BIN D:\\CORELOG.BIN
         powershell.exe Write-VolumeCache D

     (make_config.py takes the same route for CORECFG.DAT.)

  2. Before the device's FIRST write, cross-check the address it resolved.
     The firmware prints, once the volume is mounted:

         core: evlog on seq <n> boot <b> lba <block0>/<next>

     and `sudo python3 tools/make_log.py --verify /dev/sdX` (or an image)
     prints the same two absolute 512-byte LBAs computed on the host through
     the SAME formula core/fs/fat32.c uses — following the file's cluster
     chain, because unlike CORECFG.DAT this file spans many clusters. They
     MUST match. If they do not, do not let the device run to a flush; power
     it off. That is the qualification kernel/config.c's banner demands of
     every caller of the write path, and this file is the second one.

  3. To read the log: put the device in disk mode, copy the file out with a
     Windows-native copy (a drvfs read of a file the device just wrote can
     serve a stale cache) and dump it:

         powershell.exe Copy-Item D:\\CORELOG.BIN C:\\Users\\you\\CORELOG.BIN
         python3 tools/make_log.py --dump /mnt/c/Users/you/CORELOG.BIN

     --dump orders the ring blocks by sequence number and prints the text,
     marking boot boundaries and any block the ring has already overwritten.
     --blocks lists every block's header instead (index, seq, boot, length,
     final, CRC state) for when the text itself is in doubt.
"""

import argparse
import binascii
import os
import struct
import sys

# ---- format (must match core/kernel/evlog.h) ------------------------------

SECTOR = 512
PHYS_LOG = 2                        # logical sectors per physical sector
BLOCK = 2048                        # bytes per block = 4 LBAs = 2 physical sectors
HDR = 16                            # ring-block header bytes
TEXT = BLOCK - HDR                  # 2032 text bytes per block

MAGIC_FILE = 0x474F4C43             # 'C''L''O''G' little-endian
MAGIC_BLOCK = 0x424F4C43            # 'C''L''O''B'
VERSION = 1

LEN_MASK = 0x7FFF
LEN_FINAL = 0x8000

DEFAULT_SIZE = 4 * 1024 * 1024      # 4 MiB = 2048 blocks
MIN_BLOCKS = 2                      # header + one ring block
MAX_BLOCKS = 65536                  # the firmware's ceiling on a header's count

FILENAME = "CORELOG.BIN"


def crc32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


# ---- codec -----------------------------------------------------------------

def encode_header(block_count: int, file_id: int) -> bytes:
    h = bytearray(BLOCK)
    struct.pack_into("<IHHII", h, 0, MAGIC_FILE, VERSION, BLOCK,
                     block_count, file_id & 0xFFFFFFFF)
    struct.pack_into("<I", h, 16, crc32(bytes(h[:16])))
    return bytes(h)


def decode_header(blk: bytes):
    """Returns (block_count, file_id) or None. Same checks as evlog_mount."""
    if len(blk) < 20:
        return None
    magic, ver, bsz, count, fid = struct.unpack_from("<IHHII", blk, 0)
    if magic != MAGIC_FILE or ver != VERSION or bsz != BLOCK:
        return None
    if count < MIN_BLOCKS or count > MAX_BLOCKS:
        return None
    if crc32(bytes(blk[:16])) != struct.unpack_from("<I", blk, 16)[0]:
        return None
    return count, fid


def encode_block(seq: int, boot: int, text: bytes, final: bool = False) -> bytes:
    if len(text) > TEXT:
        raise ValueError("text longer than a block")
    b = bytearray(BLOCK)
    length = len(text) | (LEN_FINAL if final else 0)
    struct.pack_into("<IIHH", b, 0, MAGIC_BLOCK, seq & 0xFFFFFFFF,
                     boot & 0xFFFF, length)
    b[HDR:HDR + len(text)] = text
    struct.pack_into("<I", b, 12, crc32(bytes(b[:12]) + bytes(b[HDR:])))
    return bytes(b)


def decode_block(blk: bytes):
    """Returns (seq, boot, text, final) or None for an unwritten/torn block."""
    if len(blk) < BLOCK:
        return None
    magic, seq, boot, length = struct.unpack_from("<IIHH", blk, 0)
    if magic != MAGIC_BLOCK:
        return None
    n = length & LEN_MASK
    if n > TEXT:
        return None
    if crc32(bytes(blk[:12]) + bytes(blk[HDR:BLOCK])) != struct.unpack_from("<I", blk, 12)[0]:
        return None
    return seq, boot, bytes(blk[HDR:HDR + n]), bool(length & LEN_FINAL)


def ring_index(seq: int, block_count: int) -> int:
    """The block a sequence number lands in. Block 0 is the header, so the
    ring is block_count - 1 slots starting at block 1."""
    return 1 + (seq % (block_count - 1))


# ---- create ----------------------------------------------------------------

def do_create(volume: str, size: int, force: bool) -> int:
    if size % BLOCK != 0 or size // BLOCK < MIN_BLOCKS:
        print(f"error: --size must be a multiple of {BLOCK} and at least "
              f"{MIN_BLOCKS * BLOCK}", file=sys.stderr)
        return 2
    if size // BLOCK > MAX_BLOCKS:
        print(f"error: --size exceeds {MAX_BLOCKS} blocks", file=sys.stderr)
        return 2
    if not os.path.isdir(volume):
        print(f"error: {volume} is not a directory", file=sys.stderr)
        return 2

    path = os.path.join(volume, FILENAME)
    count = size // BLOCK

    if os.path.exists(path) and not force:
        with open(path, "rb") as f:
            head = f.read(BLOCK)
        h = decode_header(head)
        if h and h[0] * BLOCK == os.path.getsize(path):
            print(f"{path} already exists with a valid header "
                  f"({h[0]} blocks, id {h[1]:08X}) — leaving it alone. "
                  f"Use --force to recreate (this discards the log).")
            return 0
        print(f"{path} exists but its header does not validate; rewriting it.")

    file_id = struct.unpack("<I", os.urandom(4))[0]
    with open(path, "wb") as f:
        f.write(encode_header(count, file_id))
        # Zero-filled ring: every block fails the magic check, which is what
        # "unwritten" means to the device and to --dump.
        zero = bytes(BLOCK)
        for _ in range(count - 1):
            f.write(zero)
        f.flush()
        os.fsync(f.fileno())

    print(f"wrote {path}: {size} B = {count} blocks (1 header + {count - 1} "
          f"ring), id {file_id:08X}")
    print("Now flush the volume before unplugging (WSL: powershell.exe "
          "Write-VolumeCache D; and copy Windows-natively, not via /mnt/d).")
    print("Then boot the firmware and compare its 'evlog ... lba' line against")
    print(f"  sudo python3 {os.path.basename(__file__)} --verify <device-or-image>")
    return 0


# ---- dump ------------------------------------------------------------------

def read_blocks(data: bytes):
    """Split a whole file into (header, [ring blocks]). Raises on a bad header."""
    h = decode_header(data[:BLOCK])
    if h is None:
        raise ValueError("no valid CORELOG header in block 0")
    count, fid = h
    if len(data) < count * BLOCK:
        raise ValueError(f"header says {count} blocks but the file holds "
                         f"{len(data) // BLOCK}")
    blocks = []
    for i in range(1, count):
        blocks.append(data[i * BLOCK:(i + 1) * BLOCK])
    return (count, fid), blocks


def ordered_entries(data: bytes):
    """Every valid ring block as (seq, boot, text, final, index), by seq.

    Numerically by seq: a u32 that wraps needs 2^32 blocks (8 TB of log),
    so the firmware does not handle the wrap either and this stays simple.
    """
    (count, fid), blocks = read_blocks(data)
    ents = []
    for i, blk in enumerate(blocks, start=1):
        d = decode_block(blk)
        if d is None:
            continue
        seq, boot, text, final = d
        ents.append((seq, boot, text, final, i))
    ents.sort(key=lambda e: e[0])
    return (count, fid), ents


def render_dump(data: bytes, out) -> int:
    (count, fid), ents = ordered_entries(data)
    print(f"# {FILENAME}: {count} blocks, id {fid:08X}, "
          f"{len(ents)} written", file=out)
    if not ents:
        print("# (empty: nothing has been flushed yet)", file=out)
        return 0
    prev_seq = None
    prev_boot = None
    for seq, boot, text, final, idx in ents:
        if prev_seq is not None and seq != prev_seq + 1:
            print(f"# --- {seq - prev_seq - 1} block(s) missing "
                  f"(overwritten by the ring, or torn) ---", file=out)
        if boot != prev_boot:
            print(f"# === boot {boot} ===", file=out)
        s = text.decode("utf-8", errors="replace").replace("\r\n", "\n")
        out.write(s)
        if s and not s.endswith("\n"):
            out.write("\n")
        if final:
            print(f"# --- final flush (seq {seq}) ---", file=out)
        prev_seq, prev_boot = seq, boot
    return 0


def render_blocks(data: bytes, out) -> int:
    (count, fid), blocks = read_blocks(data)
    print(f"# {FILENAME}: {count} blocks, id {fid:08X}", file=out)
    print("# idx    seq    boot  len   final  state", file=out)
    for i, blk in enumerate(blocks, start=1):
        d = decode_block(blk)
        if d is None:
            state = "empty" if not any(blk) else "INVALID (magic/len/crc)"
            print(f"  {i:5d}  {'-':>8}  {'-':>4}  {'-':>4}  {'-':>5}  {state}",
                  file=out)
        else:
            seq, boot, text, final = d
            print(f"  {i:5d}  {seq:8d}  {boot:4d}  {len(text):4d}  "
                  f"{'yes' if final else 'no':>5}  ok", file=out)
    return 0


def do_dump(path: str, blocks: bool) -> int:
    with open(path, "rb") as f:
        data = f.read()
    try:
        if blocks:
            return render_blocks(data, sys.stdout)
        return render_dump(data, sys.stdout)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


# ---- verify ----------------------------------------------------------------

def _u16(b, o):
    return b[o] | (b[o + 1] << 8)


def _u32(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)


def do_verify(dev: str) -> int:
    """Resolve CORELOG.BIN's block LBAs the way core/fs/fat32.c does — the
    first block through fat32_file_lba's formula, later ones by following
    the cluster chain as fat32_file_lba_at does — and report what the ring
    holds. READ-ONLY: never writes to `dev`."""
    try:
        f = open(dev, "rb")
    except OSError as e:
        print(f"error: cannot open {dev}: {e}", file=sys.stderr)
        print("(reading a raw block device usually needs sudo)", file=sys.stderr)
        return 2

    with f:
        def rd(lba, count=1):
            f.seek(lba * SECTOR)
            b = f.read(count * SECTOR)
            if len(b) != count * SECTOR:
                raise IOError(f"short read at LBA {lba}")
            return b

        mbr = rd(0)
        if _u16(mbr, 510) != 0xAA55:
            print("error: no MBR signature — is this the whole disk?",
                  file=sys.stderr)
            return 1
        part_lba = None
        for p in range(4):
            e = 0x1BE + 16 * p
            if mbr[e + 4] in (0x0B, 0x0C):
                part_lba = _u32(mbr, e + 8)
                break
        if part_lba is None:
            print("error: no FAT32 (0x0B/0x0C) partition in the MBR",
                  file=sys.stderr)
            return 1

        # As make_config.py: the stock 80 GB records the partition start in
        # 2048-byte units. Try as-is, then x4.
        for cand in (part_lba, part_lba * 4):
            bs = rd(cand)
            if (_u16(bs, 510) == 0xAA55 and _u16(bs, 11) in (512, 1024, 2048, 4096)
                    and _u16(bs, 22) == 0 and _u16(bs, 17) == 0):
                part_lba = cand
                break
        else:
            print("error: no valid FAT32 BPB at the partition start",
                  file=sys.stderr)
            return 1

        bps = _u16(bs, 11)
        sec_ratio = bps // SECTOR
        spc = bs[13]
        rsvd = _u16(bs, 14)
        nfats = bs[16]
        fatsz = _u32(bs, 36)
        root_clus = _u32(bs, 44)
        data_start = rsvd + nfats * fatsz
        clus_bytes = spc * bps

        print(f"partition LBA   : {part_lba}")
        print(f"BytesPerSec     : {bps}  (sec_ratio {sec_ratio})")
        print(f"SecPerClus      : {spc}  (cluster {clus_bytes} B)")
        print(f"data_start      : FS-sector {data_start}")

        def clus_lba(clus):
            return part_lba + (data_start + (clus - 2) * spc) * sec_ratio

        def read_clus(clus):
            return rd(clus_lba(clus), spc * sec_ratio)

        def fat_next(clus):
            off = clus * 4
            fs_sec = rsvd + off // bps
            sec = rd(part_lba + fs_sec * sec_ratio, sec_ratio)
            return _u32(sec, off % bps) & 0x0FFFFFFF

        want = b"CORELOG BIN"
        found = None
        clus = root_clus
        for _ in range(4096):
            data = read_clus(clus)
            done = False
            for o in range(0, len(data), 32):
                ent = data[o:o + 32]
                if ent[0] == 0x00:
                    done = True
                    break
                if ent[0] == 0xE5 or ent[11] == 0x0F or (ent[11] & 0x08):
                    continue
                if ent[0:11].upper() == want:
                    first = (_u16(ent, 20) << 16) | _u16(ent, 26)
                    found = (first, _u32(ent, 28))
                    done = True
                    break
            if found or done:
                break
            clus = fat_next(clus)
            if clus < 2 or clus >= 0x0FFFFFF8:
                break

        if not found:
            print(f"\n{FILENAME} NOT FOUND in the volume root.")
            print("The firmware will run with the log OFF (About: 'LOG off').")
            return 1

        first_clus, size = found
        print(f"\n{FILENAME}")
        print(f"  first cluster : {first_clus}")
        print(f"  size          : {size} B ({size // BLOCK} blocks)")

        # The chain, as fat32_file_lba_at walks it.
        chain = [first_clus]
        c = first_clus
        need = (size + clus_bytes - 1) // clus_bytes
        while len(chain) < need:
            c = fat_next(c)
            if c < 2 or c >= 0x0FFFFFF8:
                break
            chain.append(c)
        contiguous = all(chain[i] + 1 == chain[i + 1] for i in range(len(chain) - 1))
        print(f"  chain         : {len(chain)} cluster(s), "
              f"{'contiguous' if contiguous else 'FRAGMENTED'}"
              + ("" if len(chain) >= need else
                 f"  — SHORT: {need} needed; the device will refuse blocks past it"))

        def block_lba(idx):
            off = idx * BLOCK
            ci = off // clus_bytes
            if ci >= len(chain):
                return None
            return clus_lba(chain[ci]) + (off % clus_bytes) // SECTOR

        lba0 = block_lba(0)
        print(f"  block 0 LBA   : {lba0}   (0x{lba0:08X})   "
              + ("aligned" if lba0 % PHYS_LOG == 0 else "MISALIGNED — device refuses"))

        # Read the whole file through the chain and scan the ring the way the
        # device does, to name the block it will write next.
        blob = bytearray()
        for ci in chain:
            blob += read_clus(ci)
        blob = bytes(blob[:size])
        h = decode_header(blob[:BLOCK])
        if h is None:
            print("  header        : INVALID — the device will run with the log OFF")
            return 1
        count, fid = h
        print(f"  header        : OK, {count} blocks, id {fid:08X}"
              + ("" if count * BLOCK == size else
                 f"  — SIZE MISMATCH (file is {size // BLOCK}): device refuses"))
        _, ents = ordered_entries(blob)
        if ents:
            top = ents[-1]
            nxt = top[0] + 1
            print(f"  newest block  : seq {top[0]} boot {top[1]} "
                  f"({'final' if top[3] else 'not final'}) at index {top[4]}")
        else:
            nxt = 0
            print("  newest block  : none (ring empty)")
        nidx = ring_index(nxt, count)
        nlba = block_lba(nidx)
        print(f"  next write    : seq {nxt} -> block {nidx} at LBA {nlba} "
              f"(0x{nlba:08X})")
        print()
        print("  >>> These MUST match the firmware's UART line:")
        print(f"  >>>   core: evlog on seq {nxt:08X} boot .. lba {lba0:08X}/{nlba:08X}")
        print("  >>> If they differ, power off before the first flush.")
    return 0


# ---- emit (test fixture) / selftest -----------------------------------------

def synth(count: int, entries):
    """A whole file: header + ring, `entries` = [(seq, boot, text, final)]."""
    blocks = [bytes(BLOCK)] * (count - 1)
    for seq, boot, text, final in entries:
        blocks[ring_index(seq, count) - 1] = encode_block(seq, boot, text, final)
    return encode_header(count, 0x0BADCAFE) + b"".join(blocks)


FIXTURE_COUNT = 6
FIXTURE_ENTRIES = [
    # (seq, boot, text, final). Six blocks = header + FIVE ring slots. Nine
    # writes, so seqs 0..3 have been overwritten by 5..8 and the ring holds
    # 4..8; boot 2 starts at seq 6; seq 5 was a forced flush.
    (0, 1, b"core: boot 1 first\n", False),
    (1, 1, b"core: boot 1 second\n", False),
    (2, 1, b"core: boot 1 third\n", False),
    (3, 1, b"core: boot 1 fourth\n", False),
    (4, 1, b"core: boot 1 fifth\n", False),
    (5, 1, b"core: suspend: entering\n", True),
    (6, 2, b"core: boot 2 first\n", False),
    (7, 2, b"core: boot 2 second\n", False),
    (8, 2, b"core: boot 2 third\n", False),
]


def do_emit(path: str) -> int:
    """The fixture core/tests/kernel/evlog_test.c decodes with the FIRMWARE's
    decoder: the host encoder and the device decoder must agree byte for
    byte, or a freshly created CORELOG.BIN is silently rejected on device
    and the log never turns on."""
    with open(path, "wb") as f:
        f.write(synth(FIXTURE_COUNT, FIXTURE_ENTRIES))
    return 0


def do_selftest() -> int:
    """--dump round trip on a synthesized ring: the blocks come back in
    sequence order regardless of where the ring put them, the overwritten
    ones are reported as missing, boot boundaries and the final flag show."""
    import io
    data = synth(FIXTURE_COUNT, FIXTURE_ENTRIES)
    (count, fid), ents = ordered_entries(data)
    fails = 0

    def check(label, cond):
        nonlocal fails
        print(f"[{label}] {'PASS' if cond else 'FAIL'}")
        if not cond:
            fails += 1

    check("header round-trips", count == FIXTURE_COUNT and fid == 0x0BADCAFE)
    check("ring holds the newest five", [e[0] for e in ents] == [4, 5, 6, 7, 8])
    check("seq 4 sits in slot 1+(4%5)=5",
          next(e[4] for e in ents if e[0] == 4) == 5)
    check("seq 5 wrapped onto slot 1", next(e[4] for e in ents if e[0] == 5) == 1)
    check("final flag survives", [e[3] for e in ents] == [False, True, False, False, False])
    check("boot ids survive", [e[1] for e in ents] == [1, 1, 2, 2, 2])

    out = io.StringIO()
    render_dump(data, out)
    text = out.getvalue()
    lines = text.splitlines()
    body = [l for l in lines if not l.startswith("#")]
    check("dump text is in sequence order",
          body == ["core: boot 1 fifth", "core: suspend: entering",
                   "core: boot 2 first", "core: boot 2 second", "core: boot 2 third"])
    check("dump marks the boot boundary",
          "# === boot 1 ===" in lines and "# === boot 2 ===" in lines)
    check("dump marks the final flush", "# --- final flush (seq 5) ---" in lines)
    check("dump reports nothing missing in a contiguous ring",
          not any("missing" in l for l in lines))

    # Tear one block (flip a text byte after the CRC was computed): it must
    # vanish from the dump, and the gap must be reported.
    torn = bytearray(data)
    torn[ring_index(6, FIXTURE_COUNT) * BLOCK + HDR + 3] ^= 0x01
    out = io.StringIO()
    render_dump(bytes(torn), out)
    lines = out.getvalue().splitlines()
    check("a torn block is dropped", not any("boot 2 first" in l for l in lines))
    check("and reported as a gap", "# --- 1 block(s) missing (overwritten by the ring, or torn) ---" in lines)

    # A bad header is refused outright.
    bad = bytearray(data)
    bad[0] ^= 0xFF
    try:
        render_dump(bytes(bad), io.StringIO())
        check("a bad header is refused", False)
    except ValueError:
        check("a bad header is refused", True)

    # An empty ring dumps as empty.
    out = io.StringIO()
    render_dump(synth(FIXTURE_COUNT, []), out)
    check("an empty ring dumps as empty", "# (empty: nothing has been flushed yet)" in out.getvalue())

    # The text cap and CRC coverage: a block whose header says more text than
    # fits is invalid, a block with the FINAL bit is still 2032 max.
    try:
        encode_block(0, 0, b"x" * (TEXT + 1))
        check("oversize text is refused by the encoder", False)
    except ValueError:
        check("oversize text is refused by the encoder", True)
    big = encode_block(9, 3, b"y" * TEXT, True)
    d = decode_block(big)
    check("a full final block decodes", d is not None and len(d[2]) == TEXT and d[3])

    print(f"{'OK' if fails == 0 else 'FAILED'}: {fails} failure(s)")
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(
        description="Create/verify/dump CORELOG.BIN, the pre-allocated "
                    "on-disk event log the firmware writes in place.")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--create", metavar="MOUNTPOINT",
                   help="create CORELOG.BIN in the root of a MOUNTED volume "
                        "(or any directory, to copy Windows-natively)")
    g.add_argument("--dump", metavar="FILE",
                   help="print the log's text in sequence order")
    g.add_argument("--verify", metavar="DEVICE",
                   help="read-only: resolve CORELOG.BIN's block LBAs from a "
                        "raw disk/image and report what the ring holds")
    g.add_argument("--emit", metavar="FILE",
                   help="write a small synthesized log (test fixture for "
                        "core/tests/kernel/evlog_test.c)")
    g.add_argument("--selftest", action="store_true",
                   help="round-trip a synthesized ring through --dump")
    ap.add_argument("--blocks", action="store_true",
                    help="with --dump: list every block's header instead of the text")
    ap.add_argument("--size", type=int, default=DEFAULT_SIZE,
                    help=f"file size in bytes (default {DEFAULT_SIZE} = "
                         f"{DEFAULT_SIZE // BLOCK} blocks; multiple of {BLOCK})")
    ap.add_argument("--force", action="store_true",
                    help="recreate an existing CORELOG.BIN (discards the log)")
    args = ap.parse_args()

    if args.create:
        return do_create(args.create, args.size, args.force)
    if args.dump:
        return do_dump(args.dump, args.blocks)
    if args.emit:
        return do_emit(args.emit)
    if args.selftest:
        return do_selftest()
    return do_verify(args.verify)


if __name__ == "__main__":
    sys.exit(main())
