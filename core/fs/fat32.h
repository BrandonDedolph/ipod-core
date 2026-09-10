/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/fs/fat32.h — minimal read-only FAT32 reader.
 *
 * Portable (no hardware access): all disk I/O goes through a caller-
 * supplied block-read callback that reads 512-byte sectors, so the same
 * code host-tests against a synthetic image and drives ATA on device.
 * Scope is exactly what "play a file off the disk" needs: mount, find a
 * file by name in any directory (the root by default), read its bytes, and
 * enumerate any directory (root or a subdirectory by cluster) so a browser
 * can descend. No writes. Lookup matches the VFAT long name (reassembled
 * from the 0x0F LFN entries and validated against the 8.3 checksum that
 * binds them, ASCII, case-insensitive, up to a fixed cap) and falls back to
 * the classic 8.3 short name.
 *
 * Only FAT32 is accepted: a FAT12/16 BPB is rejected at mount rather than
 * mis-parsed (its offsets 36/44 hold entirely different fields).
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
    uint32_t    total_clus;    /* total data clusters (for capacity), 0 if
                                * the BPB size fields were absent          */
    uint32_t    max_clus;      /* EXCLUSIVE cluster-number ceiling: every
                                * chain step is validated against it and
                                * every chain walk is bounded by it. Derived
                                * at mount from total_clus AND from what the
                                * FAT can address, whichever is tighter (see
                                * fat32.c) — so it is a real bound even on a
                                * volume whose TotSec fields are missing.   */
    uint32_t    free_clus;     /* free clusters from FSInfo, or 0xFFFFFFFF */
} fat32_t;

/*
 * Return codes. 0 is success everywhere; the negative values are shared by
 * the lookup/enumeration calls so a caller can tell "no such file" from "the
 * disk is failing" from "this filesystem is corrupt".
 */
#define FAT32_ENOENT   (-1)   /* no such entry                              */
#define FAT32_EIO      (-2)   /* the block callback reported a read error   */
#define FAT32_ECORRUPT (-3)   /* cluster chain is cyclic / impossibly long,
                               * or a geometry that cannot be trusted       */
#define FAT32_EINVAL   (-4)   /* null/absurd argument                       */

/*
 * Mount the FAT32 volume whose first sector is at 512-byte LBA
 * `part_lba`. Reads + validates the BPB. Returns 0 on success, negative
 * on a read error, a bad boot signature, or an unsupported layout.
 */
int fat32_mount(fat32_t *fs, fat_read_fn read, void *ud, uint32_t part_lba);

/*
 * A VFAT long name is at most 255 UTF-16 code units: 20 LFN entries of 13
 * units, with the specification capping the name itself at 255 (the 260th
 * slot is padding). A unit is at most 3 bytes of UTF-8 — a surrogate pair is
 * two units for four bytes, so it never exceeds that rate — and the dirent's
 * name buffer holds the longest legal name whole, plus its terminator. That
 * is a stack local in the directory walk; nothing keeps one of these in .bss.
 */
#define FAT32_LFN_MAX_UNITS 255
#define FAT32_NAME_BYTES    (FAT32_LFN_MAX_UNITS * 3 + 1)

/* One directory entry surfaced by enumeration. */
typedef struct {
    char     name[FAT32_NAME_BYTES];
                             /* NUL-terminated. VFAT long name if present (and
                              * checksum-bound to this entry), else the 8.3
                              * name formatted "NAME.EXT". UTF-8.            */
    char     short_name[13]; /* the raw 8.3 name, always, same formatting —
                              * what a lookup by mangled short name matches  */
    uint32_t first_clus;     /* first cluster of the file/dir */
    uint32_t size;           /* file size in bytes; 0 for directories */
    uint8_t  is_dir;         /* 1 if subdirectory, else 0 */
    uint8_t  name_lossy;     /* 1 when `name` is NOT this file's full long
                              * name: a long-name run was bound to this entry
                              * by checksum but ran past FAT32_LFN_MAX_UNITS
                              * (no conforming writer emits one), so `name`
                              * holds the 8.3 short name instead. A caller
                              * that identifies files by name — the library's
                              * locator hash — must treat such an entry as
                              * unmatchable, not hash the mangled name.     */
} fat32_dirent_t;

/*
 * Callback invoked once per real entry. Return 0 to continue, nonzero to stop
 * early. `ent` is owned by the enumerator and only valid for the duration of
 * the call — copy anything you need to keep. The callback MAY read from the
 * filesystem (open/read/enumerate); the directory sector being parsed is a
 * private copy, so that no longer corrupts the enumeration in progress.
 */
typedef int (*fat32_dir_cb)(void *ud, const fat32_dirent_t *ent);

/*
 * Find `name` (case-insensitive ASCII) in the directory whose first cluster
 * is `dir_clus`. Matches either the entry's VFAT long name (e.g.
 * "Intentions.flac", whose 4-char extension won't fit 8.3) or its raw 8.3
 * short name (e.g. "TEST.WAV", "INTENT~1.FLA"). On success returns 0 and sets
 * *first_clus and *size (bytes). Returns FAT32_ENOENT if not found, FAT32_EIO
 * on a read error, FAT32_ECORRUPT on a cyclic directory chain.
 *
 * Implemented as a matching callback over fat32_readdir, so lookup and
 * enumeration cannot drift apart. Subdirectories match too (size 0).
 */
int fat32_open_in(fat32_t *fs, uint32_t dir_clus, const char *name,
                  uint32_t *first_clus, uint32_t *size);

/*
 * fat32_open_in with the ROOT directory as the (documented) default — the
 * common case, and the shape every existing caller uses. There is no path
 * parsing: to reach a file inside a folder, enumerate/open the folder first
 * and pass its cluster to fat32_open_in.
 */
int fat32_open(fat32_t *fs, const char *name,
               uint32_t *first_clus, uint32_t *size);

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
 * (including early stop when cb returns nonzero), FAT32_EIO on a disk read
 * error, FAT32_ECORRUPT on a cyclic chain OR when `dir_clus` itself is not a
 * cluster this volume can address — that used to return 0 with no entries,
 * indistinguishable from an empty directory.
 *
 * A NONZERO RETURN MEANS THE LISTING IS INCOMPLETE. Entries surfaced before
 * the failure were real, but the caller has no way to know how many were
 * not, so a caller building a list must treat the result as "unreadable",
 * not as "these are the files". Ignoring the return here is exactly how a
 * failed album read used to show up as an album with no tracks.
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
    uint32_t clus;      /* current cluster: the one clus_off indexes into. May
                         * be fully consumed (clus_off == cluster size) with
                         * bytes remaining — the step to the next cluster is
                         * taken lazily by the next read/skip. 0 for an empty
                         * file. Whatever fat32_stream_open was handed for a
                         * corrupt entry, so the first read can reject it.   */
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
 * file is exhausted), FAT32_EIO on a disk read error, or FAT32_ECORRUPT when
 * bytes remain but the chain cannot supply them — the first cluster is not
 * addressable, or the chain ends before the size does. A short *non-zero*
 * return happens only at end-of-file; mid-file the call always fills `len`.
 *
 * 0 MEANS END OF FILE AND NOTHING ELSE. A stream opened on a corrupt entry
 * used to return 0 on its first read, which every caller — the player in
 * particular, which was written to tell EOF from error — read as a clean,
 * empty file. It now returns FAT32_ECORRUPT. A genuinely empty file (size 0)
 * still returns 0, so the two stay distinguishable.
 *
 * On success the cursor advances by the number of bytes returned. ON ANY
 * ERROR THE CURSOR IS UNCHANGED: bytes copied before the failure are not
 * counted, the cursor is not moved past them, and the same call can simply
 * be retried. (It used to move the cursor and then report failure, leaving
 * the stream ahead of the caller's own position.)
 */
int32_t fat32_stream_read(fat32_stream_t *st, void *buf, uint32_t len);

/*
 * Advance the cursor forward by up to `n` bytes WITHOUT returning data —
 * walking the cluster chain (reading only FAT entries, never cluster data),
 * so skipping a large region (e.g. an embedded-art metadata block) is cheap.
 * Returns the number of bytes actually skipped: < n at end-of-file, or when
 * the chain could not be followed (FAT read error, or a chain that ends or
 * starts on an unaddressable cluster). This call has no error channel — it
 * stops short and the NEXT fat32_stream_read reports the FAT32_* code, so a
 * caller that skips then reads always learns what happened.
 */
uint32_t fat32_stream_skip(fat32_stream_t *st, uint32_t n);

/*
 * Resolve a file's FIRST CLUSTER to an absolute 512-byte LBA, plus how many
 * consecutive sectors from it are safely addressable.
 *
 * THIS IS THE ONLY WRITE-ENABLING CALL IN A READ-ONLY DRIVER. It exists for
 * kernel/config.c, which overwrites the data sectors of a pre-allocated file
 * in place and touches no filesystem metadata; a wrong answer here is not a
 * failed read, it is the user's music library destroyed. The implementation
 * carries the full rationale — read it before changing it.
 *
 * On success returns 0, sets *lba to the absolute 512-byte LBA of the first
 * sector of `first_clus`, and sets *max_sectors to the number of 512-byte
 * sectors that are contiguous from there (exactly ONE cluster's worth — the
 * chain is deliberately not followed, so a caller can never walk past this
 * cluster into a different file).
 *
 * On ANY failure — null args, an unmounted/absurd geometry, a cluster number
 * this volume cannot address, arithmetic that would overflow uint32, or a run
 * that would leave the partition — returns negative AND writes 0 to both
 * out-params, so a caller that ignores the return code still has no address.
 * Never returns LBA 0.
 */
int fat32_file_lba(const fat32_t *fs, uint32_t first_clus,
                   uint32_t *lba, uint32_t *max_sectors);

#endif /* CORE_FS_FAT32_H */
