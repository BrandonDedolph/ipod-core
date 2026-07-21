/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/fs/fat32.c — minimal read-only FAT32 reader (see fat32.h).
 *
 * Portable: no hardware access, no libc. All disk I/O is the caller's
 * 512-byte block-read callback; the volume's BytesPerSector (e.g. 2048 on
 * the stock iPod 80 GB) is translated to 512-byte units here.
 */

#include "fat32.h"

/* End-of-cluster-chain marker (FAT32 entries are 28-bit). */
#define FAT_EOC 0x0FFFFFF8u

/* One FS-sector of scratch (BytesPerSector is at most 4096). Used for FAT
 * lookups and directory scanning; never live at the same time as the
 * bulk data path (which reads straight into the caller's buffer). */
static uint8_t fat_scratch[4096];

/* ---- little helpers -------------------------------------------------- */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static char upcase(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

/* Read one FS-sector (relative to the partition) into `buf` (which must
 * hold bytes_per_sec bytes), via the caller's 512-byte-sector callback. */
static int read_fs_sector(fat32_t *fs, uint32_t fs_sec, void *buf)
{
    return fs->read(fs->ud, fs->part_lba + fs_sec * fs->sec_ratio,
                    fs->sec_ratio, buf);
}

/* First FS-sector of a data cluster (clusters are numbered from 2). */
static uint32_t cluster_fs_sector(fat32_t *fs, uint32_t clus)
{
    return fs->data_start + (clus - 2u) * fs->sec_per_clus;
}

/* Follow the FAT: next cluster after `clus`. Returns 0 on read error,
 * >= FAT_EOC at the end of the chain. FAT entries never cross an FS-sector
 * boundary (bytes_per_sec is a multiple of 4). */
static uint32_t next_cluster(fat32_t *fs, uint32_t clus)
{
    uint32_t byte_off = clus * 4u;
    uint32_t fs_sec   = fs->fat_start + byte_off / fs->bytes_per_sec;
    uint32_t in_off   = byte_off % fs->bytes_per_sec;

    if (read_fs_sector(fs, fs_sec, fat_scratch) != 0) {
        return 0;
    }
    return rd32(&fat_scratch[in_off]) & 0x0FFFFFFFu;
}

/* Build the on-disk 11-byte 8.3 name ("TEST.WAV" -> "TEST    WAV"). */
static void make_83(const char *name, uint8_t out[11])
{
    for (int i = 0; i < 11; i++) {
        out[i] = ' ';
    }
    int i = 0, o = 0;
    while (name[i] != '\0' && name[i] != '.' && o < 8) {
        out[o++] = (uint8_t)upcase(name[i++]);
    }
    while (name[i] != '\0' && name[i] != '.') {
        i++;
    }
    if (name[i] == '.') {
        i++;
    }
    o = 8;
    while (name[i] != '\0' && o < 11) {
        out[o++] = (uint8_t)upcase(name[i++]);
    }
}

/* ---- public API ------------------------------------------------------ */

int fat32_mount(fat32_t *fs, fat_read_fn read, void *ud, uint32_t part_lba)
{
    fs->read     = read;
    fs->ud       = ud;
    fs->part_lba = part_lba;

    uint8_t bs[512];
    if (read(ud, part_lba, 1, bs) != 0) {
        return -1;
    }
    if (bs[510] != 0x55 || bs[511] != 0xAA) {
        return -2;   /* no boot signature */
    }

    uint32_t byts = rd16(&bs[11]);
    if (byts != 512 && byts != 1024 && byts != 2048 && byts != 4096) {
        return -3;
    }
    uint32_t rsvd     = rd16(&bs[14]);
    uint32_t num_fats = bs[16];
    uint32_t fatsz    = rd32(&bs[36]);   /* FATSz32 (FS sectors)         */
    uint32_t rootclus = rd32(&bs[44]);   /* BPB_RootClus                 */

    fs->bytes_per_sec = byts;
    fs->sec_ratio     = byts / 512u;
    fs->sec_per_clus  = bs[13];
    fs->root_clus     = rootclus;
    if (fs->sec_per_clus == 0 || fatsz == 0 || num_fats == 0 || rootclus < 2) {
        return -4;
    }
    fs->fat_start   = rsvd;
    fs->data_start  = rsvd + num_fats * fatsz;
    fs->clus_bytes  = fs->sec_per_clus * byts;
    return 0;
}

int fat32_open(fat32_t *fs, const char *name,
               uint32_t *first_clus, uint32_t *size)
{
    uint8_t want[11];
    make_83(name, want);

    uint32_t clus = fs->root_clus;
    while (clus >= 2 && clus < FAT_EOC) {
        uint32_t csec = cluster_fs_sector(fs, clus);
        for (uint32_t s = 0; s < fs->sec_per_clus; s++) {
            if (read_fs_sector(fs, csec + s, fat_scratch) != 0) {
                return -2;
            }
            for (uint32_t o = 0; o + 32u <= fs->bytes_per_sec; o += 32u) {
                const uint8_t *e = &fat_scratch[o];
                if (e[0] == 0x00) {
                    return -1;              /* end of directory */
                }
                if (e[0] == 0xE5) {
                    continue;               /* deleted */
                }
                if ((e[11] & 0x0F) == 0x0F || (e[11] & 0x08) != 0) {
                    continue;               /* LFN fragment or volume label */
                }
                int hit = 1;
                for (int i = 0; i < 11; i++) {
                    if (e[i] != want[i]) {
                        hit = 0;
                        break;
                    }
                }
                if (hit) {
                    *first_clus = ((uint32_t)rd16(&e[20]) << 16) | rd16(&e[26]);
                    *size = rd32(&e[28]);
                    return 0;
                }
            }
        }
        clus = next_cluster(fs, clus);
        if (clus == 0) {
            return -2;
        }
    }
    return -1;   /* not found */
}

int32_t fat32_read_file(fat32_t *fs, uint32_t clus, void *buf, uint32_t maxlen)
{
    uint8_t *out   = (uint8_t *)buf;
    uint32_t total = 0;

    while (clus >= 2 && clus < FAT_EOC && total < maxlen) {
        uint32_t csec      = cluster_fs_sector(fs, clus);
        uint32_t remaining = maxlen - total;

        if (remaining >= fs->clus_bytes) {
            /* Whole cluster straight into the caller's buffer, one call. */
            if (fs->read(fs->ud, fs->part_lba + csec * fs->sec_ratio,
                         fs->sec_per_clus * fs->sec_ratio, out) != 0) {
                return -1;
            }
            out   += fs->clus_bytes;
            total += fs->clus_bytes;
        } else {
            /* Partial final cluster: copy FS-sector by FS-sector, stopping
             * at maxlen (via the scratch so we never overrun the buffer). */
            for (uint32_t s = 0; s < fs->sec_per_clus && total < maxlen; s++) {
                if (read_fs_sector(fs, csec + s, fat_scratch) != 0) {
                    return -1;
                }
                uint32_t take = maxlen - total;
                if (take > fs->bytes_per_sec) {
                    take = fs->bytes_per_sec;
                }
                for (uint32_t i = 0; i < take; i++) {
                    out[i] = fat_scratch[i];
                }
                out   += take;
                total += take;
            }
        }

        clus = next_cluster(fs, clus);
        if (clus == 0) {
            return -1;
        }
    }
    return (int32_t)total;
}

void fat32_stream_open(fat32_stream_t *st, fat32_t *fs,
                       uint32_t first_clus, uint32_t size)
{
    st->fs        = fs;
    st->clus      = (size == 0) ? 0 : first_clus;
    st->clus_off  = 0;
    st->remaining = size;
}

int32_t fat32_stream_read(fat32_stream_t *st, void *buf, uint32_t len)
{
    fat32_t *fs    = st->fs;
    uint8_t *out   = (uint8_t *)buf;
    uint32_t total = 0;

    while (total < len && st->remaining > 0 &&
           st->clus >= 2 && st->clus < FAT_EOC) {
        uint32_t sec_in_clus = st->clus_off / fs->bytes_per_sec;
        uint32_t off_in_sec  = st->clus_off % fs->bytes_per_sec;
        uint32_t base_sec    = cluster_fs_sector(fs, st->clus) + sec_in_clus;

        /* How much this iteration can copy: bounded by the request and by
         * what's left of the file. */
        uint32_t want = len - total;
        if (want > st->remaining) {
            want = st->remaining;
        }

        uint32_t take;
        if (off_in_sec != 0) {
            /* Unaligned head: copy the tail of one FS-sector via scratch. */
            uint32_t sec_avail = fs->bytes_per_sec - off_in_sec;
            take = want < sec_avail ? want : sec_avail;
            if (read_fs_sector(fs, base_sec, fat_scratch) != 0) {
                return -1;
            }
            for (uint32_t i = 0; i < take; i++) {
                out[i] = fat_scratch[off_in_sec + i];
            }
        } else {
            /* Sector-aligned: read as many WHOLE contiguous FS-sectors as
             * fit — bounded by the request and the cluster boundary — in ONE
             * bulk fs->read straight into the caller buffer. This is what
             * keeps throughput up: one large aligned block read instead of a
             * per-sector read that ata_read_sectors would inflate to a full
             * physical sector each (4x amplification when the FS sector is
             * smaller than the drive's physical sector). Matches the
             * whole-cluster path in fat32_read_file. */
            uint32_t secs_left = fs->sec_per_clus - sec_in_clus;
            uint32_t whole     = want / fs->bytes_per_sec;
            if (whole > secs_left) {
                whole = secs_left;
            }
            if (whole > 0) {
                take = whole * fs->bytes_per_sec;
                if (fs->read(fs->ud, fs->part_lba + base_sec * fs->sec_ratio,
                             whole * fs->sec_ratio, out) != 0) {
                    return -1;
                }
            } else {
                /* Less than one FS-sector left to satisfy (partial tail at
                 * end of file): copy via scratch. */
                take = want;
                if (read_fs_sector(fs, base_sec, fat_scratch) != 0) {
                    return -1;
                }
                for (uint32_t i = 0; i < take; i++) {
                    out[i] = fat_scratch[i];
                }
            }
        }

        out           += take;
        total         += take;
        st->clus_off  += take;
        st->remaining -= take;

        /* Advance to the next cluster only when the current one is fully
         * consumed AND more file remains — so we never walk the FAT (and
         * risk a spurious read error) once we've returned the last byte. */
        if (st->clus_off == fs->clus_bytes && st->remaining > 0) {
            st->clus     = next_cluster(fs, st->clus);
            st->clus_off = 0;
            if (st->clus == 0) {
                return -1;   /* read error walking the FAT */
            }
        }
    }
    return (int32_t)total;
}
