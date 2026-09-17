#!/usr/bin/env python3
"""make_otg.py — create (and verify, dump) the two pre-allocated things the
On-The-Go playlist needs on the iPod's data partition:

  * COREOTG.DAT in the volume root — the LIVE list, 512 (folder_hash,
    file_hash) locator pairs in two CRC-32'd 5120-byte slots the firmware
    alternates between (core/kernel/otg_store.c).
  * Music/Playlists/On-The-Go 1.m3u8 .. On-The-Go 5.m3u8 — the five SAVED
    lists, ordinary extended M3U8 files carrying two extra comment lines,
    overwritten in place by the firmware (core/library/otg_slot.c).

WHY THIS TOOL EXISTS
--------------------
The firmware's FAT32 driver is READ-ONLY apart from one hole: it overwrites
the data sectors of a file that ALREADY EXISTS. It cannot create, grow,
shrink, move or delete anything — doing so would mean allocating clusters and
rewriting directory entries, which is precisely where filesystem corruption
lives. So both files have to exist before the device ever sees them, and that
is this tool's whole job. `core sync` (core/cli) does the same thing from Go
and is tested byte-for-byte against this script; this is the reference.

WORKFLOW
--------
  1. Mount the iPod's FAT32 data partition.

  2. Create the files ONCE:

         python3 tools/make_otg.py --create /mnt/d

     COREOTG.DAT goes in the VOLUME ROOT (not Music/); the five slot
     playlists go in Music/Playlists/, which must already exist (run
     `core sync` or tools/build_index.py first, or make it by hand).

     Re-running is safe. A COREOTG.DAT that already holds a valid slot is
     left alone unless you pass --force — a re-import must not silently throw
     away the list the user built. A slot .m3u8 that EXISTS is ALWAYS left
     alone, --force or not: it may hold a list they saved, or it may be their
     own playlist that happens to use the name. --create says which each one
     is, because an ABSENT slot is the only thing it puts back and that is the
     way out of the one state the device cannot recover from by itself: a save
     interrupted inside its very last write leaves a file with no header, and
     every writer on the device then refuses it for ever. Delete that file
     yourself and re-run this. Deleting is yours to do because nothing but a
     person looking at the file can tell a torn slot from a playlist somebody
     made — the stale bytes a tear leaves parse as a perfectly good playlist,
     and a real one is entitled to be empty.

  3. Flush and unmount properly (on WSL: `powershell.exe Write-VolumeCache D`),
     then boot the firmware. It prints, over UART:

         core: otg load <n> writable <1|0> seq <s> lba <slot0>/<slot1>

  4. BEFORE letting the device write anything, confirm those addresses:

         sudo python3 tools/make_otg.py --verify /dev/sdX

     This walks the MBR and the FAT32 BPB, finds COREOTG.DAT in the root, and
     prints the absolute 512-byte LBA of every CLUSTER RUN of both slots,
     computed the same way core/fs/fat32.c's fat32_file_lba_at() computes
     them. The two numbers on the UART line are the FIRST run of each slot and
     MUST match. It then walks Music/Playlists for the five slot files and
     prints each one's first LBA and chain — the firmware prints
     "core: otg slot N lba <first>" the first time it saves one.

     If anything differs, stop: do not add a track, power the device down. A
     mismatch there is the bug that overwrites somebody's music library.

  5. --dump reads a pulled COREOTG.DAT (both slots, the newest marked) or a
     pulled slot .m3u8 (header, trailer, count/crc/gen consistency, entries).

Both formats are defined by core/kernel/otg_store.c and core/library/otg_slot.c;
this file is the host half of them and the three implementations (C, Python,
Go) must agree byte for byte. --selftest and the meson suites hold that line.
"""

import argparse
import binascii
import os
import struct
import sys

# ---- COREOTG.DAT (must match core/kernel/otg_store.c) ---------------------

SECTOR = 512
PHYS_LOG = 2                        # logical sectors per physical sector
SLOT_BYTES = 5120                   # 5 physical sectors; 512 * 8 B of entries
SLOTS = 2
MIN_BYTES = SLOT_BYTES * SLOTS      # 10240 — the smallest file the device takes
DEFAULT_SIZE = 32 * 1024            # one stock cluster

MAGIC = 0x47544F43                  # 'C''O''T''G' little-endian
VERSION = 1
OTG_MAX = 512

OFF_MAGIC, OFF_VERSION, OFF_COUNT, OFF_SEQ = 0, 4, 6, 8
OFF_GEN, OFF_FLAGS, OFF_ENTRIES = 12, 14, 16
ENTRY_BYTES = 8
OFF_RESERVED = OFF_ENTRIES + OTG_MAX * ENTRY_BYTES      # 4112
OFF_CRC = SLOT_BYTES - 4                                # 5116

FILENAME = "COREOTG.DAT"

# ---- the slot playlists (must match core/library/otg_slot.c) --------------

SLOT_COUNT = 5                      # On-The-Go 1..5
SLOT_FILE_BYTES = 128 * 1024        # 131072 — four stock clusters; the
                                    # worst case is 512 * 193 + 80 = 98896 B
SLOT_FILE_MIN = 4096
SLOT_SIZE_GRAIN = 1024

M3U_TAG = b"#EXTM3U\n"
HDR_FMT = "#CORE-OTG v1 count=%05d crc=%08X gen=%05d\n"
HDR_BYTES = 48
END_FMT = "#CORE-OTG-END gen=%05d\n"
END_BYTES = 24

MUSIC_DIR = "Music"
PLAYLIST_DIR = "Playlists"


def slot_file_name(n: int) -> str:
    return "On-The-Go %d.m3u8" % n


def crc32(data: bytes) -> int:
    """The CRC-32 the firmware computes: reflected, poly 0xEDB88320,
    init/final 0xFFFFFFFF — i.e. plain zlib/binascii CRC-32."""
    return binascii.crc32(data) & 0xFFFFFFFF


# ---- COREOTG.DAT codec ----------------------------------------------------

def encode_slot(entries, seq: int, gen: int = 0) -> bytes:
    """One SLOT_BYTES slot holding `entries`, a list of (folder_hash,
    file_hash) pairs. Every byte the entries do not cover is zero, and the
    CRC covers the whole slot, so the padding is deterministic."""
    if len(entries) > OTG_MAX:
        raise ValueError("%d entries; the format holds %d" % (len(entries), OTG_MAX))
    rec = bytearray(SLOT_BYTES)
    struct.pack_into("<I", rec, OFF_MAGIC, MAGIC)
    struct.pack_into("<H", rec, OFF_VERSION, VERSION)
    struct.pack_into("<H", rec, OFF_COUNT, len(entries))
    struct.pack_into("<I", rec, OFF_SEQ, seq & 0xFFFFFFFF)
    struct.pack_into("<H", rec, OFF_GEN, gen & 0xFFFF)
    struct.pack_into("<H", rec, OFF_FLAGS, 0)
    for i, (fh, xh) in enumerate(entries):
        struct.pack_into("<II", rec, OFF_ENTRIES + i * ENTRY_BYTES,
                         fh & 0xFFFFFFFF, xh & 0xFFFFFFFF)
    struct.pack_into("<I", rec, OFF_CRC, crc32(bytes(rec[:OFF_CRC])))
    return bytes(rec)


def decode_slot(rec: bytes):
    """Validate + decode one slot, exactly as otg_slot_decode() does.
    Returns (seq, gen, entries) or None. A (0, 0) pair inside the declared
    count is DROPPED, as the firmware drops it: that pair is what the padding
    is made of, so a hand-edited file cannot inject an entry."""
    if len(rec) < SLOT_BYTES:
        return None
    if struct.unpack_from("<I", rec, OFF_MAGIC)[0] != MAGIC:
        return None
    ver = struct.unpack_from("<H", rec, OFF_VERSION)[0]
    if ver == 0 or ver > VERSION:
        return None
    count = struct.unpack_from("<H", rec, OFF_COUNT)[0]
    if count > OTG_MAX:
        return None
    if crc32(bytes(rec[:OFF_CRC])) != struct.unpack_from("<I", rec, OFF_CRC)[0]:
        return None
    seq = struct.unpack_from("<I", rec, OFF_SEQ)[0]
    gen = struct.unpack_from("<H", rec, OFF_GEN)[0]
    entries = []
    for i in range(count):
        fh, xh = struct.unpack_from("<II", rec, OFF_ENTRIES + i * ENTRY_BYTES)
        if fh == 0 and xh == 0:
            continue
        entries.append((fh, xh))
    return seq, gen, entries


def seq_newer(a: int, b: int) -> bool:
    """The firmware's wrapping comparison: the distance from b to a read as
    signed, so 0xFFFFFFFF -> 0 is +1 and not a four-billion step back."""
    d = (a - b) & 0xFFFFFFFF
    if d >= 0x80000000:
        d -= 0x100000000
    return d > 0


def newest_slot(blob: bytes):
    """(slot index, seq, gen, entries) of the newest valid slot in the
    two-slot region, or None."""
    best = None
    for s in range(SLOTS):
        d = decode_slot(blob[s * SLOT_BYTES:(s + 1) * SLOT_BYTES])
        if d is None:
            continue
        seq, gen, entries = d
        if best is None or seq_newer(seq, best[1]):
            best = (s, seq, gen, entries)
    return best


# ---- slot playlist codec --------------------------------------------------

def slot_playlist_bytes(size: int, entries=None, gen: int = 0) -> bytes:
    """The whole file, exactly as otg_slot_save() writes it: the #EXTM3U
    line, the fixed-width directive, one line per entry, the fixed-width
    trailer, newlines to the last byte."""
    if size < SLOT_FILE_MIN or size % SLOT_SIZE_GRAIN != 0:
        raise ValueError("slot file size %d must be >= %d and a multiple of %d"
                         % (size, SLOT_FILE_MIN, SLOT_SIZE_GRAIN))
    entries = entries or []
    body = b"".join((e + "\n").encode("utf-8") for e in entries)
    out = M3U_TAG
    out += (HDR_FMT % (len(entries), crc32(body), gen)).encode("ascii")
    out += body
    out += (END_FMT % gen).encode("ascii")
    if len(out) > size:
        raise ValueError("%d entries do not fit in %d bytes" % (len(entries), size))
    return out + b"\n" * (size - len(out))


def parse_slot_playlist(data: bytes):
    """Read a slot .m3u8 back. Returns a dict with `present`, and when it is
    a slot file: count, crc, gen, trailer_gen, entries, damaged. `damaged`
    applies exactly the firmware's two tests — the trailer must be there
    carrying the header's gen, and the entry lines must number what the
    header claims."""
    out = {"present": False, "count": 0, "crc": 0, "gen": 0,
           "trailer_gen": None, "entries": [], "damaged": False}
    text = data
    if text[:3] == b"\xef\xbb\xbf":
        text = text[3:]
    lines = text.split(b"\n")
    entries = []
    for raw in lines:
        line = raw.rstrip(b"\r")
        if not line:
            continue
        if line.startswith(b"#CORE-OTG v1 "):
            full = line + b"\n"
            if len(full) != HDR_BYTES:
                continue
            try:
                s = full.decode("ascii")
            except UnicodeDecodeError:
                continue
            if (s[13:19] != "count=" or s[24] != " " or s[25:29] != "crc="
                    or s[37] != " " or s[38:42] != "gen="):
                continue
            # Digits only, exactly as the device's parse_dec() and Go's
            # parseDecN() accept them. int() would take "+0012" and " 0012",
            # which would make this oracle laxer than the two implementations
            # it exists to be the oracle for.
            if not (s[19:24].isdigit() and s[42:47].isdigit()
                    and all(c in "0123456789abcdefABCDEF" for c in s[29:37])):
                continue
            out["count"] = int(s[19:24], 10)
            out["crc"] = int(s[29:37], 16)
            out["gen"] = int(s[42:47], 10)
            out["present"] = True
            continue
        if line.startswith(b"#CORE-OTG-END "):
            full = line + b"\n"
            if len(full) != END_BYTES:
                continue
            s = full.decode("ascii", "replace")
            if s[14:18] != "gen=" or not s[18:23].isdigit():
                continue
            out["trailer_gen"] = int(s[18:23], 10)
            continue
        if line.startswith(b"#"):
            continue
        entries.append(line.decode("utf-8", "replace"))
    out["entries"] = entries
    if out["present"]:
        body = b"".join((e + "\n").encode("utf-8") for e in entries)
        out["body_crc"] = crc32(body)
        out["damaged"] = (out["trailer_gen"] != out["gen"] or
                          len(entries) != out["count"])
    return out


# ---- create ---------------------------------------------------------------

def do_create(volume: str, size: int, slot_size: int, force: bool) -> int:
    if size < MIN_BYTES:
        print("error: --size must be at least %d (%d slots of %d B)"
              % (MIN_BYTES, SLOTS, SLOT_BYTES), file=sys.stderr)
        return 2
    if slot_size < SLOT_FILE_MIN or slot_size % SLOT_SIZE_GRAIN != 0:
        print("error: --slot-size must be at least %d and a multiple of %d"
              % (SLOT_FILE_MIN, SLOT_SIZE_GRAIN), file=sys.stderr)
        return 2
    if not os.path.isdir(volume):
        print("error: %s is not a directory" % volume, file=sys.stderr)
        return 2

    rc = 0
    path = os.path.join(volume, FILENAME)
    write_it = True
    if os.path.exists(path) and not force:
        with open(path, "rb") as f:
            head = f.read(MIN_BYTES)
        best = newest_slot(head) if len(head) >= MIN_BYTES else None
        if best:
            print("%s already exists and holds a valid slot (slot %d, seq %d, "
                  "%d entries) — leaving it alone. Use --force to reset."
                  % (path, best[0], best[1], len(best[3])))
            write_it = False
        elif os.path.getsize(path) >= MIN_BYTES:
            print("%s exists but holds no valid slot; rewriting it." % path)
        else:
            print("%s exists but is too small (%d B < %d); rewriting it."
                  % (path, os.path.getsize(path), MIN_BYTES))

    if write_it:
        # Slot 0: a valid EMPTY list at seq 1. Slot 1: zeroed, so the device's
        # first save lands there and the alternation starts cleanly.
        blob = bytearray(size)
        blob[0:SLOT_BYTES] = encode_slot([], 1)
        _write_through(path, bytes(blob))
        print("wrote %s: %d B, slot 0 = empty list (seq 1), slot 1 = empty"
              % (path, size))

    pldir = os.path.join(volume, MUSIC_DIR, PLAYLIST_DIR)
    if not os.path.isdir(pldir):
        print("error: %s does not exist — create the library first "
              "(`core sync`, or tools/build_index.py's layout)" % pldir,
              file=sys.stderr)
        return 1
    for n in range(1, SLOT_COUNT + 1):
        p = os.path.join(pldir, slot_file_name(n))
        if os.path.exists(p):
            # ALWAYS left alone, --force or not: it may hold a list the user
            # saved, or it may be their own playlist under that name. Say
            # WHICH, because a slot with no header is one the device will
            # never write to again, and deleting it here is the only way back.
            try:
                with open(p, "rb") as f:
                    info = parse_slot_playlist(f.read())
            except OSError as e:
                print("  %-20s exists but will not read (%s)"
                      % (slot_file_name(n), e))
                print("  %-20s   the device will never write to it; if it is "
                      "not yours, delete it and re-run" % "")
                continue
            if not info["present"]:
                print("  %-20s exists with NO On-The-Go header — a playlist of "
                      "your own, or a save" % slot_file_name(n))
                print("  %-20s   interrupted at its last write. The device will "
                      "never write to it; if it" % "")
                print("  %-20s   is not yours, delete it and re-run this." % "")
            elif info["damaged"]:
                print("  %-20s exists, torn save — left alone (Delete Playlist "
                      "on the device frees it)" % slot_file_name(n))
            elif info["count"]:
                print("  %-20s exists, %d track(s) — left alone"
                      % (slot_file_name(n), info["count"]))
            else:
                print("  %-20s exists, empty — left alone" % slot_file_name(n))
            continue
        _write_through(p, slot_playlist_bytes(slot_size))
        print("  %-20s created, %d B, empty" % (slot_file_name(n), slot_size))

    print("Now unmount/flush the volume before unplugging "
          "(WSL: powershell.exe Write-VolumeCache D).")
    print("Then boot the firmware and compare its 'otg ... lba' line against")
    print("  sudo python3 %s --verify <device-or-image>"
          % os.path.basename(__file__))
    return rc


def _write_through(path: str, data: bytes) -> None:
    """Write and fsync — on removable media the bytes have to leave the page
    cache before the volume is unmounted, or the device sees a file full of
    nothing."""
    with open(path, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())


# ---- emit (test fixtures) -------------------------------------------------

# The three pairs core/tests/kernel/otg_store_test.c expects to decode out of
# --emit's slot 0. Arbitrary, but FIXED: the point of the fixture is that the
# host encoder and the device decoder agree byte for byte.
FIXTURE_ENTRIES = [(0x11111111, 0x22222222),
                   (0xAABBCCDD, 0x01020304),
                   (0xFFFFFFFF, 0x00000001)]
FIXTURE_SEQ = 1
FIXTURE_GEN = 3


def do_emit(path: str) -> int:
    """The two-slot region of a COREOTG.DAT: slot 0 valid at seq 1 holding
    FIXTURE_ENTRIES, slot 1 zeroed. core/tests/kernel/otg_store_test.c decodes
    it with the FIRMWARE's otg_slot_decode() — if the two encoders ever drift,
    a freshly created COREOTG.DAT would be silently rejected on the device and
    the On-The-Go list would never persist."""
    blob = bytearray(MIN_BYTES)
    blob[0:SLOT_BYTES] = encode_slot(FIXTURE_ENTRIES, FIXTURE_SEQ, FIXTURE_GEN)
    with open(path, "wb") as f:
        f.write(bytes(blob))
    return 0


def do_emit_slot(path: str) -> int:
    """An EMPTY slot playlist at the smallest legal size, for
    core/tests/library/otg_slot_test.c to probe with the firmware's parser."""
    with open(path, "wb") as f:
        f.write(slot_playlist_bytes(SLOT_FILE_MIN * 2))
    return 0


# ---- dump -----------------------------------------------------------------

def do_dump(path: str) -> int:
    with open(path, "rb") as f:
        data = f.read()

    if data[:len(M3U_TAG)] == M3U_TAG or b"#CORE-OTG" in data[:HEAD_SCAN]:
        return _dump_slot_playlist(path, data)
    return _dump_store(path, data)


HEAD_SCAN = 512


def _dump_store(path: str, data: bytes) -> int:
    print("%s: %d B" % (path, len(data)))
    if len(data) < MIN_BYTES:
        print("  too small: %d < %d — the device would refuse it"
              % (len(data), MIN_BYTES))
        return 1
    best = newest_slot(data)
    for s in range(SLOTS):
        raw = data[s * SLOT_BYTES:(s + 1) * SLOT_BYTES]
        d = decode_slot(raw)
        if d is None:
            kind = "empty" if not any(raw) else "invalid (magic/version/count/CRC)"
            print("  slot %d: %s" % (s, kind))
            continue
        seq, gen, entries = d
        mark = "  <- newest" if best and best[0] == s else ""
        print("  slot %d: VALID  seq=%d gen=%d  %d entr%s%s"
              % (s, seq, gen, len(entries),
                 "y" if len(entries) == 1 else "ies", mark))
        for i, (fh, xh) in enumerate(entries):
            print("      %3d  folder %08X  file %08X" % (i, fh, xh))
    if best is None:
        print("  -> no valid slot; the firmware keeps an empty list and "
              "writes slot 0 on the first save")
    else:
        print("  -> the firmware loads slot %d (seq %d) and writes slot %d next"
              % (best[0], best[1], 1 - best[0]))
    return 0


def _dump_slot_playlist(path: str, data: bytes) -> int:
    print("%s: %d B" % (path, len(data)))
    info = parse_slot_playlist(data)
    if not info["present"]:
        print("  no #CORE-OTG directive — this is a FOREIGN playlist at a slot "
              "name. The device lists and plays it and never writes to it.")
        for e in info["entries"]:
            print("      %s" % e)
        return 0
    print("  header : count=%d crc=%08X gen=%d"
          % (info["count"], info["crc"], info["gen"]))
    print("  trailer: gen=%s"
          % ("(missing)" if info["trailer_gen"] is None else info["trailer_gen"]))
    print("  entries: %d, crc of their bytes %08X"
          % (len(info["entries"]), info["body_crc"]))
    if info["damaged"]:
        print("  -> DAMAGED: the device lists it and opens it to "
              "'Playlist damaged — save again' with Delete Playlist under it.")
        print("     The slot is NOT free until that Delete rewrites it: Save "
              "only ever writes into an")
        print("     EMPTY slot, which is what makes the one tear gen+count "
              "cannot see unreachable.")
    elif info["crc"] != info["body_crc"]:
        print("  -> the header's CRC does not match the entry bytes. The DEVICE "
              "does not check this (gen + count are its test), but the bytes "
              "were changed by something that did not update the header.")
    else:
        print("  -> intact")
    for e in info["entries"]:
        print("      %s" % e)
    return 0


# ---- verify ---------------------------------------------------------------

def _u16(b, o):
    return b[o] | (b[o + 1] << 8)


def _u32(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)


class _Vol:
    """Just enough FAT32 to walk to a file and list its cluster chain, with
    the same arithmetic core/fs/fat32.c uses."""

    def __init__(self, f):
        self.f = f
        mbr = self.rd(0)
        if _u16(mbr, 510) != 0xAA55:
            raise ValueError("no MBR signature — is this the whole disk?")
        part_lba = None
        for p in range(4):
            e = 0x1BE + 16 * p
            if mbr[e + 4] in (0x0B, 0x0C):
                part_lba = _u32(mbr, e + 8)
                break
        if part_lba is None:
            raise ValueError("no FAT32 (0x0B/0x0C) partition in the MBR")
        # The stock 80 GB iPod records the partition start in 2048-byte units,
        # not 512 — the ambiguity kernel/main.c resolves by trying both.
        for cand in (part_lba, part_lba * 4):
            bs = self.rd(cand)
            if (_u16(bs, 510) == 0xAA55 and _u16(bs, 11) in (512, 1024, 2048, 4096)
                    and _u16(bs, 22) == 0 and _u16(bs, 17) == 0):
                part_lba = cand
                break
        else:
            raise ValueError("no valid FAT32 BPB at the partition start")

        self.part_lba = part_lba
        self.bps = _u16(bs, 11)
        self.sec_ratio = self.bps // SECTOR
        self.spc = bs[13]
        self.rsvd = _u16(bs, 14)
        self.nfats = bs[16]
        self.fatsz = _u32(bs, 36)
        self.root_clus = _u32(bs, 44)
        self.data_start = self.rsvd + self.nfats * self.fatsz

    def rd(self, lba, count=1):
        self.f.seek(lba * SECTOR)
        b = self.f.read(count * SECTOR)
        if len(b) != count * SECTOR:
            raise IOError("short read at LBA %d" % lba)
        return b

    def clus_lba(self, clus):
        """fat32_file_lba()'s formula. The sec_ratio factor is the whole
        point: without it the address is 4x too small on a 2048-byte volume
        and lands inside the FAT."""
        return self.part_lba + (self.data_start + (clus - 2) * self.spc) * self.sec_ratio

    def read_clus(self, clus):
        return self.rd(self.clus_lba(clus), self.spc * self.sec_ratio)

    def fat_next(self, clus):
        off = clus * 4
        fs_sec = self.rsvd + off // self.bps
        sec = self.rd(self.part_lba + fs_sec * self.sec_ratio, self.sec_ratio)
        return _u32(sec, off % self.bps) & 0x0FFFFFFF

    def chain(self, clus, cap=4096):
        out = []
        while 2 <= clus < 0x0FFFFFF8 and len(out) < cap:
            out.append(clus)
            clus = self.fat_next(clus)
        return out

    def entries(self, dir_clus):
        """(name, short_name, first_clus, size, is_dir) for every real entry,
        with the long name reassembled from the 0x0F runs."""
        out = []
        lfn = {}
        for clus in self.chain(dir_clus):
            data = self.read_clus(clus)
            for o in range(0, len(data), 32):
                ent = data[o:o + 32]
                if ent[0] == 0x00:
                    return out
                if ent[0] == 0xE5:
                    lfn = {}
                    continue
                if ent[11] == 0x0F:
                    seq = ent[0] & 0x3F
                    units = b""
                    for lo in (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30):
                        units += ent[lo:lo + 2]
                    lfn[seq] = units
                    continue
                if ent[11] & 0x08:
                    lfn = {}
                    continue
                short = ent[0:8].decode("latin-1").rstrip()
                ext = ent[8:11].decode("latin-1").rstrip()
                sname = short + ("." + ext if ext else "")
                name = sname
                if lfn:
                    raw = b"".join(lfn[k] for k in sorted(lfn))
                    try:
                        s = raw.decode("utf-16-le")
                    except UnicodeDecodeError:
                        s = ""
                    for stop in ("\x00", "￿"):
                        if stop in s:
                            s = s[:s.index(stop)]
                    if s:
                        name = s
                lfn = {}
                first = (_u16(ent, 20) << 16) | _u16(ent, 26)
                out.append((name, sname, first, _u32(ent, 28),
                            bool(ent[11] & 0x10)))
        return out

    def find(self, dir_clus, want, is_dir=None):
        for name, sname, first, size, isd in self.entries(dir_clus):
            if is_dir is not None and isd != is_dir:
                continue
            if name.upper() == want.upper() or sname.upper() == want.upper():
                return first, size, isd
        return None

    def file_runs(self, first_clus, byte_len):
        """Every (byte offset, absolute LBA, sectors) run of the first
        `byte_len` bytes, following the chain — what fat32_file_lba_at()
        resolves, one cluster at a time."""
        runs = []
        off = 0
        for clus in self.chain(first_clus):
            if off >= byte_len:
                break
            n = min(self.spc * self.bps, byte_len - off)
            runs.append((off, self.clus_lba(clus), n // SECTOR))
            off += n
        return runs


def do_verify(dev: str) -> int:
    try:
        f = open(dev, "rb")
    except OSError as e:
        print("error: cannot open %s: %s" % (dev, e), file=sys.stderr)
        print("(reading a raw block device usually needs sudo)", file=sys.stderr)
        return 2

    with f:
        try:
            v = _Vol(f)
        except (ValueError, IOError) as e:
            print("error: %s" % e, file=sys.stderr)
            return 1

        print("partition LBA   : %d" % v.part_lba)
        print("BytesPerSec     : %d  (sec_ratio %d)" % (v.bps, v.sec_ratio))
        print("SecPerClus      : %d  (cluster %d B)" % (v.spc, v.spc * v.bps))
        print("data_start      : FS-sector %d" % v.data_start)
        print("root cluster    : %d" % v.root_clus)

        rc = 0
        hit = v.find(v.root_clus, FILENAME, is_dir=False)
        if not hit:
            print("\n%s NOT FOUND in the volume root." % FILENAME)
            print("The On-The-Go list will work for the session and persist "
                  "nothing; the firmware says 'otg load 0 writable 0'.")
            print("Run --create against the mounted volume first.")
            rc = 1
        else:
            first, size, _ = hit
            print("\n%s" % FILENAME)
            print("  first cluster : %d" % first)
            print("  size          : %d B (%s)"
                  % (size, "OK" if size >= MIN_BYTES
                     else "TOO SMALL, need %d" % MIN_BYTES))
            if size < MIN_BYTES:
                rc = 1
            else:
                runs = v.file_runs(first, MIN_BYTES)
                slot_lbas = []
                for s in range(SLOTS):
                    base = s * SLOT_BYTES
                    print("  slot %d runs   :" % s)
                    for off, lba, secs in runs:
                        end = off + secs * SECTOR
                        if end <= base or off >= base + SLOT_BYTES:
                            continue
                        run_off = max(off, base)
                        run_lba = lba + (run_off - off) // SECTOR
                        run_end = min(end, base + SLOT_BYTES)
                        if not slot_lbas or len(slot_lbas) == s:
                            slot_lbas.append(run_lba)
                        print("      +%-5d  LBA %-10d (0x%08X)  %d sectors%s"
                              % (run_off - base, run_lba, run_lba,
                                 (run_end - run_off) // SECTOR,
                                 "" if run_lba % PHYS_LOG == 0
                                 else "  MISALIGNED — the device refuses"))
                if len(slot_lbas) == SLOTS:
                    print()
                    print("  >>> These MUST match the firmware's UART line:")
                    print("  >>>   core: otg load .. writable .. seq .. "
                          "lba %08X/%08X" % (slot_lbas[0], slot_lbas[1]))
                    print("  >>> If they differ, DO NOT add a track. Power off.")

                print()
                blob = b""
                for off, lba, secs in runs:
                    blob += v.rd(lba, secs)
                _dump_store("(on the volume)", blob[:MIN_BYTES])

        # --- the five slot playlists ---
        print("\n%s/%s/" % (MUSIC_DIR, PLAYLIST_DIR))
        music = v.find(v.root_clus, MUSIC_DIR, is_dir=True)
        pld = v.find(music[0], PLAYLIST_DIR, is_dir=True) if music else None
        if not pld:
            print("  not found — the saved On-The-Go lists have nowhere to live")
            return 1
        for n in range(1, SLOT_COUNT + 1):
            name = slot_file_name(n)
            e = v.find(pld[0], name, is_dir=False)
            if not e:
                print("  %-20s MISSING — Save will have no free slot here"
                      % name)
                rc = 1
                continue
            first, size, _ = e
            chain = v.chain(first)
            note = ""
            if size < SLOT_FILE_MIN or size % SLOT_SIZE_GRAIN != 0:
                note = "  UNUSABLE (size must be >= %d and a multiple of %d)" \
                       % (SLOT_FILE_MIN, SLOT_SIZE_GRAIN)
                rc = 1
            print("  %-20s %d B, first cluster %d -> LBA %d (0x%08X), "
                  "%d cluster(s)%s"
                  % (name, size, first, v.clus_lba(first), v.clus_lba(first),
                     len(chain), note))
            body = b""
            for _off, lba, secs in v.file_runs(first, size):
                body += v.rd(lba, secs)
            info = parse_slot_playlist(body[:size])
            if not info["present"]:
                print("      foreign: no #CORE-OTG directive (a playlist of "
                      "your own, never written to)")
            elif info["damaged"]:
                print("      DAMAGED (header gen %d, trailer gen %s, %d entry "
                      "lines vs count %d) — not free until Delete"
                      % (info["gen"], info["trailer_gen"],
                         len(info["entries"]), info["count"]))
            elif info["count"] == 0:
                print("      empty (hidden from the Playlists list, free for Save)")
            else:
                print("      %d track(s), gen %d" % (info["count"], info["gen"]))
        print("\n  >>> The first Save of a session prints, over UART:")
        print("  >>>   core: otg slot N lba <the first-cluster LBA above>")
    return rc


# ---- selftest -------------------------------------------------------------

def do_selftest() -> int:
    fails = 0

    def check(label, cond):
        nonlocal fails
        print("[%s] %s" % (label, "PASS" if cond else "FAIL"))
        if not cond:
            fails += 1

    # --- the COREOTG.DAT slot codec ---
    rec = encode_slot(FIXTURE_ENTRIES, FIXTURE_SEQ, FIXTURE_GEN)
    check("a slot is exactly %d bytes" % SLOT_BYTES, len(rec) == SLOT_BYTES)
    d = decode_slot(rec)
    check("round trip", d == (FIXTURE_SEQ, FIXTURE_GEN, FIXTURE_ENTRIES))
    check("the reserved word and the padding are zero",
          rec[OFF_RESERVED:OFF_CRC] == b"\x00" * (OFF_CRC - OFF_RESERVED))
    check("an empty list encodes and decodes",
          decode_slot(encode_slot([], 7)) == (7, 0, []))
    check("a full list encodes and decodes",
          decode_slot(encode_slot([(i + 1, i + 2) for i in range(OTG_MAX)], 2))
          == (2, 0, [(i + 1, i + 2) for i in range(OTG_MAX)]))

    for label, off, xor in (("magic", OFF_MAGIC, 0xFF),
                            ("version", OFF_VERSION, 0xFF),
                            ("seq", OFF_SEQ, 0x01),
                            ("an entry", OFF_ENTRIES, 0x01),
                            ("the padding", OFF_CRC - 1, 0x01),
                            ("the CRC itself", OFF_CRC, 0x01)):
        bad = bytearray(rec)
        bad[off] ^= xor
        check("a flipped bit in %s is refused" % label, decode_slot(bytes(bad)) is None)

    bad = bytearray(rec)
    struct.pack_into("<H", bad, OFF_COUNT, OTG_MAX + 1)
    struct.pack_into("<I", bad, OFF_CRC, crc32(bytes(bad[:OFF_CRC])))
    check("count > OTG_MAX is refused, even with a good CRC",
          decode_slot(bytes(bad)) is None)

    bad = bytearray(encode_slot([(1, 2), (3, 4)], 1))
    struct.pack_into("<II", bad, OFF_ENTRIES + ENTRY_BYTES, 0, 0)
    struct.pack_into("<I", bad, OFF_CRC, crc32(bytes(bad[:OFF_CRC])))
    check("a null pair inside the count is dropped",
          decode_slot(bytes(bad)) == (1, 0, [(1, 2)]))

    check("all-zero is not a slot", decode_slot(b"\x00" * SLOT_BYTES) is None)
    check("a short buffer is not a slot", decode_slot(rec[:-1]) is None)

    check("seq wraps: 0 is newer than 0xFFFFFFFF",
          seq_newer(0, 0xFFFFFFFF) and not seq_newer(0xFFFFFFFF, 0))
    check("equal is not newer", not seq_newer(5, 5))

    two = bytearray(MIN_BYTES)
    two[0:SLOT_BYTES] = encode_slot([(1, 1)], 9)
    two[SLOT_BYTES:2 * SLOT_BYTES] = encode_slot([(2, 2), (3, 3)], 10)
    check("the newer slot wins", newest_slot(bytes(two))[0] == 1)
    two[SLOT_BYTES + 40] ^= 0x01
    check("a torn newer slot loses to the intact older one",
          newest_slot(bytes(two))[0] == 0)

    # --- the CRC itself, against a known vector ---
    check("CRC-32 of \"123456789\" is 0xCBF43926",
          crc32(b"123456789") == 0xCBF43926)

    # --- the slot playlist ---
    empty = slot_playlist_bytes(SLOT_FILE_MIN)
    check("an empty slot playlist is the file's whole size",
          len(empty) == SLOT_FILE_MIN)
    check("...begins #EXTM3U + the fixed-width directive",
          empty[:8] == M3U_TAG and
          empty[8:8 + HDR_BYTES] ==
          b"#CORE-OTG v1 count=00000 crc=00000000 gen=00000\n")
    check("...then the trailer, then newlines to the end",
          empty[56:56 + END_BYTES] == b"#CORE-OTG-END gen=00000\n" and
          set(empty[80:]) == {0x0A})
    info = parse_slot_playlist(empty)
    check("an empty slot parses as present, count 0, undamaged",
          info["present"] and info["count"] == 0 and not info["damaged"])

    ent = ["/Music/Artist - Album/01 Song.flac",
           "/Music/Artist - Album/02 Other.flac",
           "/Root.flac"]
    used = slot_playlist_bytes(8192, ent, gen=42)
    info = parse_slot_playlist(used)
    check("a used slot round-trips its entries", info["entries"] == ent)
    check("...its count, gen and CRC",
          info["count"] == 3 and info["gen"] == 42 and
          info["crc"] == info["body_crc"] and info["trailer_gen"] == 42)
    check("...and is not damaged", not info["damaged"])
    check("the header line is 48 bytes and the trailer 24",
          used[8:8 + HDR_BYTES].endswith(b"\n") and HDR_BYTES == 48 and
          END_BYTES == 24)

    # A tear: the trailer carries the PREVIOUS gen (stage 0 was written last
    # and never landed), which is the failure the write order manufactures.
    torn = bytearray(used)
    torn[8:8 + HDR_BYTES] = (HDR_FMT % (3, info["crc"], 43)).encode("ascii")
    info = parse_slot_playlist(bytes(torn))
    check("a gen mismatch reads as damaged", info["damaged"])

    # A tear mid-tail: the header claims more entries than are there.
    torn = bytearray(slot_playlist_bytes(8192, ent, gen=7))
    torn[8:8 + HDR_BYTES] = (HDR_FMT % (5, 0, 7)).encode("ascii")
    check("a count mismatch reads as damaged",
          parse_slot_playlist(bytes(torn))["damaged"])

    for bad in (b"#CORE-OTG v1 count=+0012 crc=00000000 gen=00000\n",
                b"#CORE-OTG v1 count= 0012 crc=00000000 gen=00000\n",
                b"#CORE-OTG v1 count=00012 crc=0000000G gen=00000\n",
                b"#CORE-OTG v1 count=00012 crc=00000000 gen=+0000\n"):
        assert len(bad) == HDR_BYTES
        check("a non-digit field is not a directive (%s)"
              % bad[13:47].decode(),
              not parse_slot_playlist(b"#EXTM3U\n" + bad +
                                      b"#CORE-OTG-END gen=00000\n")["present"])

    foreign = b"#EXTM3U\n/Music/Something/else.flac\n"
    info = parse_slot_playlist(foreign)
    check("a playlist with no directive is foreign, not damaged",
          not info["present"] and not info["damaged"] and
          info["entries"] == ["/Music/Something/else.flac"])

    for bad_size in (SLOT_FILE_MIN - 1024, 5000):
        try:
            slot_playlist_bytes(bad_size)
            check("size %d is refused" % bad_size, False)
        except ValueError:
            check("size %d is refused" % bad_size, True)
    try:
        slot_playlist_bytes(SLOT_FILE_MIN, ["/x" * 200] * 200)
        check("entries that do not fit are refused", False)
    except ValueError:
        check("entries that do not fit are refused", True)

    check("512 entries at the worst-case path length fit in %d B"
          % SLOT_FILE_BYTES,
          len(slot_playlist_bytes(SLOT_FILE_BYTES,
                                  ["/" + "x" * 191] * OTG_MAX)) == SLOT_FILE_BYTES)

    print("%s: %d failure(s)" % ("OK" if fails == 0 else "FAILED", fails))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(
        description="Create/verify/dump COREOTG.DAT and the five On-The-Go "
                    "slot playlists, the pre-allocated files the firmware "
                    "overwrites in place.")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--create", metavar="MOUNTPOINT",
                   help="create COREOTG.DAT in the root of a MOUNTED volume "
                        "and the five On-The-Go slot playlists under "
                        "Music/Playlists/")
    g.add_argument("--verify", metavar="DEVICE",
                   help="read-only: resolve every cluster run of both "
                        "COREOTG.DAT slots and of the five slot playlists "
                        "from a raw disk/image, and decode what is there")
    g.add_argument("--dump", metavar="FILE",
                   help="decode a pulled COREOTG.DAT or a pulled "
                        "On-The-Go N.m3u8")
    g.add_argument("--emit", metavar="FILE",
                   help="write the %d-byte two-slot region to FILE (test "
                        "fixture: the firmware's decoder must accept it)"
                        % MIN_BYTES)
    g.add_argument("--emit-slot", metavar="FILE",
                   help="write an empty slot playlist to FILE (test fixture)")
    g.add_argument("--selftest", action="store_true",
                   help="run the format round-trip checks (meson runs this)")
    ap.add_argument("--size", type=int, default=DEFAULT_SIZE,
                    help="COREOTG.DAT size in bytes (default %d; minimum %d)"
                         % (DEFAULT_SIZE, MIN_BYTES))
    ap.add_argument("--slot-size", type=int, default=SLOT_FILE_BYTES,
                    help="each slot playlist's size in bytes (default %d; "
                         "minimum %d, a multiple of %d)"
                         % (SLOT_FILE_BYTES, SLOT_FILE_MIN, SLOT_SIZE_GRAIN))
    ap.add_argument("--force", action="store_true",
                    help="overwrite an existing COREOTG.DAT even if it holds a "
                         "valid slot (throws away the live list). Never "
                         "touches an existing slot playlist.")
    args = ap.parse_args()

    if args.create:
        return do_create(args.create, args.size, args.slot_size, args.force)
    if args.emit:
        return do_emit(args.emit)
    if args.emit_slot:
        return do_emit_slot(args.emit_slot)
    if args.dump:
        return do_dump(args.dump)
    if args.selftest:
        return do_selftest()
    return do_verify(args.verify)


if __name__ == "__main__":
    sys.exit(main())
