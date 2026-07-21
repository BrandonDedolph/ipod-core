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

/* Format a raw 11-byte on-disk 8.3 field into a NUL-terminated display name.
 * The stored form is space-padded and split base(8)+ext(3) with no dot
 * ("HELLO   TXT", "README     "); enumeration needs the human "NAME.EXT"
 * form. Trailing spaces are trimmed from the base and the extension, and the
 * '.' is inserted only when an extension actually survives — so a dotless
 * name stays dotless. `out` must hold at least 13 bytes (8 + 1 + 3 + NUL). */
static void fmt_83(const uint8_t raw[11], char *out)
{
    int o = 0;

    int base_len = 8;
    while (base_len > 0 && raw[base_len - 1] == ' ') {
        base_len--;
    }
    for (int i = 0; i < base_len; i++) {
        out[o++] = (char)raw[i];
    }

    int ext_len = 3;
    while (ext_len > 0 && raw[8 + ext_len - 1] == ' ') {
        ext_len--;
    }
    if (ext_len > 0) {
        out[o++] = '.';
        for (int i = 0; i < ext_len; i++) {
            out[o++] = (char)raw[8 + i];
        }
    }
    out[o] = '\0';
}

/* ---- VFAT long-filename (LFN) reassembly ----------------------------- */
/*
 * A long name is stored in one or more 0x0F-attribute entries that PRECEDE
 * the file's 8.3 entry, in reverse order (highest sequence first). Each LFN
 * entry carries 13 UTF-16LE code units at byte offsets 1..10, 14..25, 28..31,
 * a 1-based sequence number in byte 0 (bit 0x40 marks the last/first-logical
 * piece), and the 8.3 checksum in byte 13. We accumulate the pieces by their
 * sequence index into one flat ASCII buffer, then match the caller's ASCII
 * name against it. Non-ASCII code points (> 0x7F) become a sentinel that can
 * never match an ASCII request; names longer than the cap are marked unusable
 * and simply fall back to the 8.3 match.
 */
#define FAT_LFN_MAX 64   /* longest long-name we reassemble (chars); plenty
                          * for real track filenames, and the buffer stays
                          * small for a freestanding build */

/* Byte offset of each of the 13 chars inside a 32-byte LFN entry. */
static const uint8_t lfn_pos[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};

typedef struct {
    char lfn[FAT_LFN_MAX];  /* assembled ASCII long name (not NUL-terminated) */
    int  max_idx;           /* highest slot written, -1 if none               */
    int  term;              /* terminator (0x0000) position, -1 if none       */
    int  bad;               /* saw an out-of-range piece -> unusable          */
} lfn_acc_t;

static void lfn_reset(lfn_acc_t *a)
{
    a->max_idx = -1;
    a->term    = -1;
    a->bad     = 0;
}

/* Fold one 0x0F LFN entry into the accumulator. */
static void lfn_add(lfn_acc_t *a, const uint8_t *e)
{
    uint32_t seq = (uint32_t)(e[0] & 0x3Fu);   /* strip the 0x40 last-marker */
    if (seq == 0) {
        a->bad = 1;                            /* not a valid sequence index */
        return;
    }
    uint32_t base = (seq - 1u) * 13u;
    for (int k = 0; k < 13; k++) {
        uint16_t u   = rd16(&e[lfn_pos[k]]);
        uint32_t idx = base + (uint32_t)k;
        if (u == 0x0000) {                     /* name terminator */
            if (a->term < 0 || (int)idx < a->term) {
                a->term = (int)idx;
            }
        } else if (u == 0xFFFF) {
            /* padding past the terminator: nothing to store */
        } else if (idx >= FAT_LFN_MAX) {
            a->bad = 1;                         /* longer than we handle */
        } else {
            a->lfn[idx] = (u > 0x7Fu) ? (char)0x01 : (char)u;
            if ((int)idx > a->max_idx) {
                a->max_idx = (int)idx;
            }
        }
    }
}

/* Length of the assembled long name, or -1 if there isn't a usable one. */
static int lfn_length(const lfn_acc_t *a)
{
    if (a->bad) {
        return -1;
    }
    if (a->term >= 0) {
        return a->term;
    }
    if (a->max_idx >= 0) {
        return a->max_idx + 1;
    }
    return -1;
}

/* Case-insensitive ASCII compare of `name` against the first `len` chars of
 * the assembled long name; both must end at the same place. */
static int lfn_match(const char *name, const char *lfn, int len)
{
    for (int i = 0; i < len; i++) {
        if (name[i] == '\0' || upcase(name[i]) != upcase(lfn[i])) {
            return 0;
        }
    }
    return name[len] == '\0';
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

    /* Long-name pieces accumulate here until their 8.3 entry is reached. */
    lfn_acc_t acc;
    lfn_reset(&acc);

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
                    lfn_reset(&acc);        /* deleted (drop any LFN run) */
                    continue;
                }
                if ((e[11] & 0x0F) == 0x0F) {
                    lfn_add(&acc, e);       /* LFN fragment for the next 8.3 */
                    continue;
                }
                if ((e[11] & 0x08) != 0) {
                    lfn_reset(&acc);        /* volume label */
                    continue;
                }
                /* Real 8.3 entry: try the reassembled long name first, then
                 * fall back to the classic 8.3 short-name match. */
                int hit  = 0;
                int llen = lfn_length(&acc);
                if (llen >= 0) {
                    hit = lfn_match(name, acc.lfn, llen);
                }
                if (!hit) {
                    hit = 1;
                    for (int i = 0; i < 11; i++) {
                        if (e[i] != want[i]) {
                            hit = 0;
                            break;
                        }
                    }
                }
                if (hit) {
                    *first_clus = ((uint32_t)rd16(&e[20]) << 16) | rd16(&e[26]);
                    *size = rd32(&e[28]);
                    return 0;
                }
                lfn_reset(&acc);            /* LFN run belongs only to this 8.3 */
            }
        }
        clus = next_cluster(fs, clus);
        if (clus == 0) {
            return -2;
        }
    }
    return -1;   /* not found */
}

int fat32_readdir_root(fat32_t *fs, fat32_dir_cb cb, void *ud)
{
    /* Same root-directory walk as fat32_open — same cluster-chain follow, the
     * same FS-sector reads, the same LFN reassembly — but instead of matching
     * a target name we surface every real entry through the callback. The LFN
     * run accumulates across the 0x0F fragments that precede each 8.3 entry;
     * we reset it on anything that breaks a run (deleted slot, volume label)
     * exactly as the lookup path does, so long names bind to the right file. */
    lfn_acc_t acc;
    lfn_reset(&acc);

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
                    return 0;               /* end of directory: done */
                }
                if (e[0] == 0xE5) {
                    lfn_reset(&acc);        /* deleted (drop any LFN run) */
                    continue;
                }
                if ((e[11] & 0x0F) == 0x0F) {
                    lfn_add(&acc, e);       /* LFN fragment for the next 8.3 */
                    continue;
                }
                if ((e[11] & 0x08) != 0) {
                    lfn_reset(&acc);        /* volume label: not a real entry */
                    continue;
                }

                /* Real 8.3 entry: build the dirent. Prefer the reassembled
                 * long name; fall back to the formatted 8.3 short name. The
                 * name buffer lives in the caller's fat32_dirent_t (their
                 * stack), and the long name is capped at FAT_LFN_MAX (< 256),
                 * so it always fits with room for the terminator. */
                fat32_dirent_t ent;
                int llen = lfn_length(&acc);
                if (llen >= 0) {
                    int i;
                    for (i = 0; i < llen; i++) {
                        ent.name[i] = acc.lfn[i];
                    }
                    ent.name[i] = '\0';
                } else {
                    fmt_83(e, ent.name);
                }
                ent.is_dir     = (e[11] & 0x10) ? 1 : 0;
                ent.first_clus = ((uint32_t)rd16(&e[20]) << 16) | rd16(&e[26]);
                ent.size       = ent.is_dir ? 0u : rd32(&e[28]);

                lfn_reset(&acc);            /* LFN run belonged to this entry */

                if (cb(ud, &ent) != 0) {
                    return 0;               /* caller asked to stop early */
                }
            }
        }
        clus = next_cluster(fs, clus);
        if (clus == 0) {
            return -2;
        }
    }
    return 0;
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

uint32_t fat32_stream_skip(fat32_stream_t *st, uint32_t n)
{
    fat32_t *fs   = st->fs;
    uint32_t done = 0;

    while (n > 0 && st->remaining > 0 &&
           st->clus >= 2 && st->clus < FAT_EOC) {
        /* Skip within the current cluster by just moving the cursor — no data
         * read. Only the FAT is touched, when we step to the next cluster. */
        uint32_t clus_left = fs->clus_bytes - st->clus_off;
        uint32_t take      = n < clus_left ? n : clus_left;
        if (take > st->remaining) {
            take = st->remaining;
        }

        st->clus_off  += take;
        st->remaining -= take;
        done          += take;
        n             -= take;

        if (st->clus_off == fs->clus_bytes && st->remaining > 0) {
            st->clus     = next_cluster(fs, st->clus);
            st->clus_off = 0;
            if (st->clus == 0) {
                break;      /* FAT read error — stop, report what we skipped */
            }
        }
    }
    return done;
}
