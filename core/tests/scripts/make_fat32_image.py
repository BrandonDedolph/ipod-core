#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# Generate a minimal FAT32 volume image for the fat32 reader host test.
# Deliberately uses BytesPerSector = 2048 (like the stock iPod 80 GB drive)
# so the test exercises the sector-size translation (sec_ratio = 4), and a
# file that spans two clusters so cluster-chain following is covered.
#
# Layout (FS-sectors, SecPerClus = 1 so cluster == FS-sector):
#   0        boot sector (BPB)
#   1        (reserved)
#   2        FAT #1
#   3        FAT #2
#   4        cluster 2  = root directory
#   5        cluster 3  = HELLO.TXT part 1
#   6        cluster 4  = HELLO.TXT part 2
#   7        cluster 5  = "Intentions.flac" (long-name file)
#   8..11    spare data clusters
#
# HELLO.TXT is 3000 bytes of the pattern byte[i] = i & 0xFF (spans clusters
# 3 and 4). "Intentions.flac" exercises the VFAT long-name lookup: its 4-char
# extension can't fit an 8.3 name, so it is written with two 0x0F LFN entries
# (correct checksum) preceding a mangled short entry (INTENT~1.FLA), and can
# only be found by its long name. Usage: make_fat32_image.py <output-path>

import struct
import sys

BPS = 2048          # BytesPerSector
SPC = 1             # SectorsPerCluster
RSVD = 2            # reserved sectors
NFATS = 2
FATSZ = 1           # FS-sectors per FAT
ROOTCLUS = 2
NUM_DATA_CLUS = 8

FILE_CLUS = 3
FILE_SIZE = 3000

# Long-name file: "Intentions.flac" (single cluster 5).
LFN_NAME = "Intentions.flac"
LFN_SHORT = b'INTENT~1FLA'       # mangled 8.3 short name (11 bytes)
LFN_CLUS = 5
LFN_SIZE = 500

DATA_START = RSVD + NFATS * FATSZ          # FS-sector of the data region
TOTAL_SEC = DATA_START + NUM_DATA_CLUS


def set_fat(img, base, entry, val):
    struct.pack_into('<I', img, base + entry * 4, val & 0x0FFFFFFF)


# Standard 8.3 short-name checksum used to bind LFN entries to their 8.3 entry.
def lfn_checksum(short11):
    s = 0
    for c in short11:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
    return s


# Build the on-disk LFN entries for `longname`, in physical (reverse) order:
# highest sequence first (with the 0x40 last-marker), sequence 1 last, i.e.
# the run that immediately precedes the 8.3 entry.
def lfn_entries(longname, short11):
    chksum = lfn_checksum(short11)
    pos = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]
    units = [ord(ch) for ch in longname]
    units.append(0x0000)                       # name terminator
    while len(units) % 13 != 0:                 # pad the last piece with 0xFFFF
        units.append(0xFFFF)
    nent = len(units) // 13
    entries = []
    for seq in range(1, nent + 1):
        ent = bytearray(32)
        ent[0] = seq | (0x40 if seq == nent else 0x00)
        ent[11] = 0x0F                          # LFN attribute
        ent[13] = chksum
        chunk = units[(seq - 1) * 13: seq * 13]
        for k in range(13):
            struct.pack_into('<H', ent, pos[k], chunk[k])
        entries.append(ent)
    entries.reverse()                           # on disk: highest seq first
    return entries


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: make_fat32_image.py <output-path>")
    img = bytearray(TOTAL_SEC * BPS)

    # --- boot sector / BPB (FS-sector 0) ---
    img[0:3] = b'\xEB\x58\x90'
    img[3:11] = b'MSDOS5.0'
    struct.pack_into('<H', img, 11, BPS)        # BytesPerSec
    img[13] = SPC                               # SecPerClus
    struct.pack_into('<H', img, 14, RSVD)       # RsvdSecCnt
    img[16] = NFATS                             # NumFATs
    img[21] = 0xF8                              # media descriptor
    struct.pack_into('<I', img, 36, FATSZ)      # FATSz32
    struct.pack_into('<I', img, 44, ROOTCLUS)   # RootClus
    img[510] = 0x55
    img[511] = 0xAA                             # boot signature

    # --- FATs (both copies identical) ---
    for base in (RSVD * BPS, (RSVD + FATSZ) * BPS):
        set_fat(img, base, 0, 0x0FFFFFF8)       # media
        set_fat(img, base, 1, 0x0FFFFFFF)       # reserved / EOC
        set_fat(img, base, 2, 0x0FFFFFFF)       # root dir: single cluster
        set_fat(img, base, 3, 4)                # HELLO.TXT: clus 3 -> 4
        set_fat(img, base, 4, 0x0FFFFFFF)       # HELLO.TXT: clus 4 -> EOC
        set_fat(img, base, LFN_CLUS, 0x0FFFFFFF)  # long-name file: single clus

    # --- root directory (cluster 2 == FS-sector DATA_START) ---
    e = DATA_START * BPS
    img[e:e + 11] = b'HELLO   TXT'              # 8.3 name
    img[e + 11] = 0x20                          # attr: archive
    struct.pack_into('<H', img, e + 20, (FILE_CLUS >> 16) & 0xFFFF)
    struct.pack_into('<H', img, e + 26, FILE_CLUS & 0xFFFF)
    struct.pack_into('<I', img, e + 28, FILE_SIZE)

    # "Intentions.flac": its LFN run, then the mangled 8.3 entry.
    e += 32
    for ent in lfn_entries(LFN_NAME, LFN_SHORT):
        img[e:e + 32] = ent
        e += 32
    img[e:e + 11] = LFN_SHORT                   # 8.3 short name
    img[e + 11] = 0x20                          # attr: archive
    struct.pack_into('<H', img, e + 20, (LFN_CLUS >> 16) & 0xFFFF)
    struct.pack_into('<H', img, e + 26, LFN_CLUS & 0xFFFF)
    struct.pack_into('<I', img, e + 28, LFN_SIZE)
    # the following entry is left 0x00 => end of directory

    # --- file content (clusters 3,4, contiguous from FS-sector DATA_START+1) ---
    f = (DATA_START + (FILE_CLUS - 2)) * BPS
    for i in range(FILE_SIZE):
        img[f + i] = i & 0xFF

    # --- long-name file content (cluster 5), a distinct pattern ---
    f = (DATA_START + (LFN_CLUS - 2)) * BPS
    for i in range(LFN_SIZE):
        img[f + i] = (i ^ 0x5A) & 0xFF

    with open(sys.argv[1], 'wb') as fh:
        fh.write(img)


if __name__ == '__main__':
    main()
