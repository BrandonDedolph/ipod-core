/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/fs/fat32.h — minimal read-only FAT32 reader.
 *
 * Portable (no hardware access): all disk I/O goes through a caller-
 * supplied block-read callback that reads 512-byte sectors, so the same
 * code host-tests against a synthetic image and drives ATA on device.
 * Scope is exactly what "play a file off the disk" needs: mount, find a
 * file in the root directory by name, read its bytes, and enumerate any
 * directory (root or a subdirectory by cluster) so a browser can descend.
 * No writes. Lookup matches the VFAT long name (reassembled from the
 * 0x0F LFN entries, ASCII, case-insensitive, up to a fixed cap) and falls
 * back to the classic 8.3 short name.
 *
 * The volume's logical sector size (BytesPerSector, e.g. 2048 on the
 * stock iPod 80 GB) is taken from the BPB and translated to the 512-byte
 * units the callback speaks.
 */
#ifndef CORE_FS_FAT32_H
#define CORE_FS_FAT32_H

#include <stdint.h>

/*
 * Read `count` 512-byte sectors starting at absolute 512-byte LBA `lba`
 * into `buf`. Returns 0 on success, negative on error. `ud` is the opaque
 * pointer passed to fat32_mount (e.g. the partition base + ATA handle).
 */
typedef int (*fat_read_fn)(void *ud, uint32_t lba, uint32_t count, void *buf);

typedef struct {
    fat_read_fn read;
    void       *ud;
    uint32_t    part_lba;      /* partition start, 512-byte LBA           */
    uint32_t    bytes_per_sec; /* FS sector size (BytesPerSec, e.g. 2048) */
    uint32_t    sec_ratio;     /* bytes_per_sec / 512                     */
    uint32_t    sec_per_clus;  /* FS sectors per cluster                  */
    uint32_t    root_clus;     /* first cluster of the root directory     */
    uint32_t    fat_start;     /* FS sector of the FAT region (rel. part) */
    uint32_t    data_start;    /* FS sector of the data region (rel.part) */
    uint32_t    clus_bytes;    /* bytes per cluster                       */
} fat32_t;

/*
 * Mount the FAT32 volume whose first sector is at 512-byte LBA
 * `part_lba`. Reads + validates the BPB. Returns 0 on success, negative
 * on a read error, a bad boot signature, or an unsupported layout.
 */
int fat32_mount(fat32_t *fs, fat_read_fn read, void *ud, uint32_t part_lba);

/*
 * Find `name` (case-insensitive ASCII) in the root directory. Matches either
 * the file's VFAT long name (e.g. "Intentions.flac", whose 4-char extension
 * won't fit 8.3) or its 8.3 short name (e.g. "TEST.WAV"). On success returns
 * 0 and sets *first_clus and *size (bytes). Returns -1 if not found, negative
 * on a read error.
 */
int fat32_open(fat32_t *fs, const char *name,
               uint32_t *first_clus, uint32_t *size);

/* One directory entry surfaced by enumeration. */
typedef struct {
    char     name[256];   /* NUL-terminated. VFAT long name if present, else 8.3 (e.g. "TEST.FLA"). */
    uint32_t first_clus;  /* first cluster of the file/dir */
    uint32_t size;        /* file size in bytes; 0 for directories */
    uint8_t  is_dir;      /* 1 if subdirectory, else 0 */
} fat32_dirent_t;

/* Callback invoked once per real entry. Return 0 to continue, nonzero to stop early. */
typedef int (*fat32_dir_cb)(void *ud, const fat32_dirent_t *ent);

/*
 * Enumerate the directory whose first cluster is `dir_clus`, invoking
 * cb(ud, &ent) for each real entry (files AND subdirectories, is_dir set
 * accordingly). Pass fs->root_clus to enumerate the root. Skips: the
 * volume-label entry (attr & 0x08), LFN staging entries (attr==0x0F), deleted
 * slots (name[0]==0xE5), the 0x00 end-of-directory terminator (stop), PLUS the
 * "." / ".." self/parent links (any 8.3 entry whose raw name[0] is '.') so a
 * browser sees only real children. Reassembles VFAT long names from the 0x0F
 * LFN entries the same way fat32_open does (ASCII; if a long name is absent use
 * the 8.3 name with the standard "NAME.EXT" formatting — trailing spaces
 * trimmed, '.' inserted only when an extension exists). Returns 0 on success
 * (including early stop when cb returns nonzero), negative on a disk read error.
 */
int fat32_readdir(fat32_t *fs, uint32_t dir_clus, fat32_dir_cb cb, void *ud);

/*
 * Enumerate the ROOT directory. Thin wrapper over fat32_readdir with
 * dir_clus == fs->root_clus; same skip rules and long-name handling.
 * Returns 0 on success (including early stop), negative on a disk read error.
 */
int fat32_readdir_root(fat32_t *fs, fat32_dir_cb cb, void *ud);

/*
 * Read up to `maxlen` bytes of the file beginning at cluster `first_clus`
 * into `buf`, following the cluster chain. Returns the number of bytes
 * read (<= maxlen), or negative on a read error. Pass the file size as
 * `maxlen` (clamped to the buffer by the caller) to read the whole file.
 */
int32_t fat32_read_file(fat32_t *fs, uint32_t first_clus,
                        void *buf, uint32_t maxlen);

/*
 * Forward-only streaming cursor over a file's cluster chain. Lets a caller
 * read a large file in bounded pieces — e.g. a playback pump refilling a
 * ring buffer — without a RAM buffer big enough for the whole file. Forward
 * only: there is no seek-back. Open it with the (first_clus, size) pair that
 * fat32_open returns.
 */
typedef struct {
    fat32_t *fs;
    uint32_t clus;      /* current cluster (>=2 while data remains, else 0) */
    uint32_t clus_off;  /* bytes already consumed within the current cluster */
    uint32_t remaining; /* file bytes not yet returned                       */
} fat32_stream_t;

/*
 * Begin streaming `size` bytes of the file that starts at `first_clus`.
 * Pure initialisation (no I/O) — always succeeds. A size of 0 yields an
 * immediately-empty stream.
 */
void fat32_stream_open(fat32_stream_t *st, fat32_t *fs,
                       uint32_t first_clus, uint32_t size);

/*
 * Read up to `len` bytes forward from the cursor into `buf`, following the
 * cluster chain as needed. Returns the number of bytes read (0 once the
 * file is exhausted), or negative on a disk read error. A short *non-zero*
 * return happens only at end-of-file; mid-file the call always fills `len`.
 * Advances the cursor by the number of bytes returned.
 */
int32_t fat32_stream_read(fat32_stream_t *st, void *buf, uint32_t len);

/*
 * Advance the cursor forward by up to `n` bytes WITHOUT returning data —
 * walking the cluster chain (reading only FAT entries, never cluster data),
 * so skipping a large region (e.g. an embedded-art metadata block) is cheap.
 * Returns the number of bytes actually skipped (< n only at end-of-file).
 */
uint32_t fat32_stream_skip(fat32_stream_t *st, uint32_t n);

#endif /* CORE_FS_FAT32_H */
