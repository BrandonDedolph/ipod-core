/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/fs/fat32.h — minimal read-only FAT32 reader.
 *
 * Portable (no hardware access): all disk I/O goes through a caller-
 * supplied block-read callback that reads 512-byte sectors, so the same
 * code host-tests against a synthetic image and drives ATA on device.
 * Scope is exactly what "play a file off the disk" needs: mount, find a
 * file by 8.3 name in the root directory, read its bytes. No writes, no
 * subdirectories, no long-filename decoding (LFN entries are skipped and
 * the short 8.3 name is matched).
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
 * Find `name` (an 8.3 name like "TEST.WAV", case-insensitive) in the root
 * directory. On success returns 0 and sets *first_clus and *size (bytes).
 * Returns -1 if not found, negative on a read error.
 */
int fat32_open(fat32_t *fs, const char *name,
               uint32_t *first_clus, uint32_t *size);

/*
 * Read up to `maxlen` bytes of the file beginning at cluster `first_clus`
 * into `buf`, following the cluster chain. Returns the number of bytes
 * read (<= maxlen), or negative on a read error. Pass the file size as
 * `maxlen` (clamped to the buffer by the caller) to read the whole file.
 */
int32_t fat32_read_file(fat32_t *fs, uint32_t first_clus,
                        void *buf, uint32_t maxlen);

#endif /* CORE_FS_FAT32_H */
