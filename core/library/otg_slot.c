/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/library/otg_slot.c — the saved On-The-Go playlists. See otg_slot.h for
 * the format, the tear-detection argument and the write order.
 *
 * Freestanding: no libc beyond lib/mem.c's memcpy/memset, no statics, no
 * recursion. Every loop is bounded by the caller's row count, by the slot
 * file's own size, or by a fixed cap.
 */

#include "otg_slot.h"

#include "../lib/mem.h"

/* ---- tiny string helpers ------------------------------------------------ */

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static uint32_t slen_n(const char *s, uint32_t cap)
{
    uint32_t i = 0;
    while (i < cap && s[i] != '\0') {
        i++;
    }
    return i;
}

/* Five zero-padded decimal digits. Everything this writes (a count <= 512, a
 * 16-bit gen) fits; a value that does not is written modulo 100000, which
 * only a corrupt caller could produce and which still parses. */
static void put_dec5(char *dst, uint32_t v)
{
    v %= 100000u;
    for (int i = 4; i >= 0; i--) {
        dst[i] = (char)('0' + (v % 10u));
        v /= 10u;
    }
}

/* Eight UPPER-case hex digits — what tools/make_otg.py and internal/devicefs
 * both print, so the three implementations produce identical bytes. */
static void put_hex8(char *dst, uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) {
        dst[i] = hex[v & 0xFu];
        v >>= 4;
    }
}

/* Exactly `n` decimal digits, nothing else. 1 on success. */
static int parse_dec(const char *s, uint32_t n, uint32_t *out)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return 0;
        }
        v = v * 10u + (uint32_t)(s[i] - '0');
    }
    *out = v;
    return 1;
}

/* Exactly eight hex digits, either case. 1 on success. */
static int parse_hex8(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < 8; i++) {
        char c = s[i];
        uint32_t d;
        if (c >= '0' && c <= '9')      d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else return 0;
        v = (v << 4) | d;
    }
    *out = v;
    return 1;
}

static int lit_eq(const char *s, const char *lit, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (s[i] != lit[i]) {
            return 0;
        }
    }
    return 1;
}

/* ---- the directive lines ------------------------------------------------ */

/*
 * Field offsets inside the 48-byte header line. Fixed width on purpose: the
 * writer patches count and crc into stage 0 AFTER the rest of the file has
 * been formatted, which is only possible if doing so cannot change any
 * byte's position.
 *
 *   0  "#CORE-OTG v1 "   13
 *   13 "count="           6   -> 19
 *   19 NNNNN              5   -> 24
 *   24 ' '                1   -> 25
 *   25 "crc="             4   -> 29
 *   29 HHHHHHHH           8   -> 37
 *   37 ' '                1   -> 38
 *   38 "gen="             4   -> 42
 *   42 GGGGG              5   -> 47
 *   47 '\n'               1   -> 48
 */
#define HDR_TAG      "#CORE-OTG v1 count="
#define HDR_TAG_LEN  19u
#define HDR_OFF_CNT  19u
#define HDR_OFF_CRC  29u
#define HDR_OFF_GEN  42u

/*
 *   0  "#CORE-OTG-END gen="  18
 *   18 GGGGG                  5  -> 23
 *   23 '\n'                   1  -> 24
 */
#define END_TAG      "#CORE-OTG-END gen="
#define END_TAG_LEN  18u
#define END_OFF_GEN  18u

#define M3U_TAG      "#EXTM3U\n"
#define M3U_TAG_LEN  8u

static void format_header(char *dst, uint32_t count, uint32_t crc, uint16_t gen)
{
    memcpy(dst, HDR_TAG, HDR_TAG_LEN);
    put_dec5(&dst[HDR_OFF_CNT], count);
    dst[24] = ' ';
    memcpy(&dst[25], "crc=", 4);
    put_hex8(&dst[HDR_OFF_CRC], crc);
    dst[37] = ' ';
    memcpy(&dst[38], "gen=", 4);
    put_dec5(&dst[HDR_OFF_GEN], gen);
    dst[47] = '\n';
}

static void format_trailer(char *dst, uint16_t gen)
{
    memcpy(dst, END_TAG, END_TAG_LEN);
    put_dec5(&dst[END_OFF_GEN], gen);
    dst[23] = '\n';
}

/* Parse a header line out of `line` (at least OTG_HDR_LINE_BYTES readable).
 * 1 when it is one. */
static int parse_header(const char *line, otg_slot_info_t *out)
{
    uint32_t count = 0, crc = 0, gen = 0;
    if (!lit_eq(line, HDR_TAG, HDR_TAG_LEN)) {
        return 0;
    }
    if (!parse_dec(&line[HDR_OFF_CNT], 5, &count) || count > OTG_MAX) {
        return 0;
    }
    if (line[24] != ' ' || !lit_eq(&line[25], "crc=", 4)) {
        return 0;
    }
    if (!parse_hex8(&line[HDR_OFF_CRC], &crc)) {
        return 0;
    }
    if (line[37] != ' ' || !lit_eq(&line[38], "gen=", 4)) {
        return 0;
    }
    if (!parse_dec(&line[HDR_OFF_GEN], 5, &gen) || gen > 0xFFFFu) {
        return 0;
    }
    if (line[47] != '\n' && line[47] != '\r') {
        return 0;
    }
    out->count   = (uint16_t)count;
    out->crc     = crc;
    out->gen     = (uint16_t)gen;
    out->present = 1;
    return 1;
}

static int parse_trailer(const char *line, uint16_t *gen_out)
{
    uint32_t gen = 0;
    if (!lit_eq(line, END_TAG, END_TAG_LEN)) {
        return 0;
    }
    if (!parse_dec(&line[END_OFF_GEN], 5, &gen) || gen > 0xFFFFu) {
        return 0;
    }
    if (line[23] != '\n' && line[23] != '\r') {
        return 0;
    }
    *gen_out = (uint16_t)gen;
    return 1;
}

/* ---- naming ------------------------------------------------------------- */

int otg_slot_index(const char *name)
{
    static const char want[] = "on-the-go ";
    if (name == 0) {
        return 0;
    }
    uint32_t i = 0;
    for (; want[i] != '\0'; i++) {
        if (lower(name[i]) != want[i]) {
            return 0;               /* also catches a name that ends early */
        }
    }
    char d = name[i];
    if (d < '1' || d > (char)('0' + (int)OTG_SLOTS)) {
        return 0;
    }
    if (name[i + 1] != '\0') {
        return 0;                   /* "On-The-Go 1.m3u8", "On-The-Go 12"  */
    }
    return d - '0';
}

void otg_slot_name(char *dst, int n)
{
    if (dst == 0) {
        return;
    }
    if (n < 1 || n > (int)OTG_SLOTS) {
        dst[0] = '\0';
        return;
    }
    memcpy(dst, "On-The-Go ", 10);
    dst[10] = (char)('0' + n);
    dst[11] = '\0';
}

/* ---- probe / verify ----------------------------------------------------- */

/*
 * The head read both probe and verify start from. One FS-sector's worth of
 * bytes is plenty: the directive is on line 1 or line 2, and a file whose
 * first 512 bytes do not carry it is not a slot file (which is exactly the
 * verdict we want for a user's own playlist at that name).
 */
#define HEAD_BYTES 512u

static int read_head(fat32_t *fs, uint32_t clus, uint32_t size,
                     char *buf, uint32_t *got)
{
    uint32_t want = size < HEAD_BYTES ? size : HEAD_BYTES;
    *got = 0;
    if (want == 0) {
        return 0;
    }
    int32_t rc = fat32_read_file(fs, clus, buf, want);
    if (rc < 0) {
        return -1;
    }
    *got = (uint32_t)rc;
    return 0;
}

/* Find the header directive in `buf` (line-oriented, '\n' or '\r' ends a
 * line). 1 when found. */
static int scan_head(const char *buf, uint32_t n, otg_slot_info_t *out)
{
    uint32_t start = 0;
    /* A UTF-8 BOM would sit before line 1; m3u.c skips it, so must we. */
    if (n >= 3 && (uint8_t)buf[0] == 0xEF && (uint8_t)buf[1] == 0xBB &&
        (uint8_t)buf[2] == 0xBF) {
        start = 3;
    }
    for (uint32_t i = start; i < n; ) {
        uint32_t j = i;
        while (j < n && buf[j] != '\n' && buf[j] != '\r') {
            j++;
        }
        /* The line is buf[i..j); the terminator has to BE there, or the line
         * runs past what we read and we have no business parsing it. */
        if (j < n && (j - i) + 1u == OTG_HDR_LINE_BYTES &&
            parse_header(&buf[i], out)) {
            return 1;
        }
        i = j;
        while (i < n && (buf[i] == '\n' || buf[i] == '\r')) {
            i++;
        }
    }
    return 0;
}

int otg_slot_probe(fat32_t *fs, uint32_t clus, uint32_t size,
                   otg_slot_info_t *out)
{
    if (fs == 0 || out == 0) {
        return -1;
    }
    memset(out, 0, sizeof *out);
    if (size < M3U_TAG_LEN + OTG_HDR_LINE_BYTES) {
        return 0;                   /* too small to be a slot file */
    }

    char head[HEAD_BYTES];
    uint32_t got = 0;
    if (read_head(fs, clus, size, head, &got) != 0) {
        return -1;
    }
    (void)scan_head(head, got, out);
    return 0;
}

/*
 * Walk the file forward looking for the trailer line. Bounded by the file's
 * own size; stops at the padding (the first line that is empty AND follows
 * the trailer is the end of anything worth reading, but we do not rely on
 * that — the bound is the size).
 *
 * The line accumulator is 32 bytes: a line longer than the trailer cannot BE
 * the trailer, so a long line only needs to be recognised as long.
 */
static int find_trailer(fat32_t *fs, uint32_t clus, uint32_t size,
                        uint16_t *gen_out, int *found)
{
    fat32_stream_t st;
    char     chunk[HEAD_BYTES];
    char     line[OTG_END_LINE_BYTES];
    uint32_t len  = 0;
    int      over = 0;

    *found = 0;
    fat32_stream_open(&st, fs, clus, size);
    for (;;) {
        int32_t got = fat32_stream_read(&st, chunk, HEAD_BYTES);
        if (got < 0) {
            return -1;
        }
        if (got == 0) {
            break;
        }
        for (int32_t i = 0; i < got; i++) {
            char c = chunk[i];
            if (c == '\n' || c == '\r') {
                if (!over && len + 1u == OTG_END_LINE_BYTES) {
                    /* The stored line has no terminator; parse_trailer wants
                     * one, and the byte we are looking at IS it. */
                    line[len] = c;
                    if (parse_trailer(line, gen_out)) {
                        *found = 1;
                        return 0;
                    }
                }
                len  = 0;
                over = 0;
                continue;
            }
            if (len + 1u < OTG_END_LINE_BYTES) {
                line[len++] = c;
            } else {
                over = 1;           /* longer than the trailer: not it */
            }
        }
    }
    return 0;
}

int otg_slot_verify(fat32_t *fs, uint32_t clus, uint32_t size,
                    uint32_t listed, otg_slot_info_t *out)
{
    if (otg_slot_probe(fs, clus, size, out) != 0) {
        return -1;
    }
    if (!out->present) {
        return 0;                   /* not a slot file: nothing to damage */
    }

    uint16_t tgen  = 0;
    int      found = 0;
    if (find_trailer(fs, clus, size, &tgen, &found) != 0) {
        return -1;
    }
    /*
     * The two tests the write order makes sufficient: the trailer has to be
     * there carrying the header's gen (a cut before the last write leaves an
     * old header over a new trailer, or the reverse), and the entry lines the
     * parser actually produced have to be the number the header claims (a cut
     * mid-tail leaves fewer, or leaves an old tail's extra ones).
     */
    if (!found || tgen != out->gen || listed != out->count) {
        out->damaged = 1;
    }
    return 0;
}

/* ---- the staged writer -------------------------------------------------- */

typedef struct {
    fat32_t            *fs;
    uint32_t            clus;
    uint32_t            size;
    otg_write_fn        write;
    otg_save_scratch_t *scr;
    uint32_t            pos;     /* byte offset of the next byte to emit    */
    int                 rc;      /* the first error; sticky                 */
} sink_t;

/*
 * Put `bytes` of `buf` on the platter at byte offset `off`, run by run.
 *
 * `off` and `bytes` are both multiples of ATA_SECTOR_SZ by construction (the
 * stage size and the file-size grain see to that). Every run is re-resolved
 * immediately before its write: nothing is cached, and the address is checked
 * for physical-sector alignment, for LBA 0, for the partition bound and for
 * staying inside the cluster it resolved to.
 */
static int write_region(sink_t *s, uint32_t off, const uint8_t *buf,
                        uint32_t bytes)
{
    while (bytes > 0) {
        if (off >= s->size || bytes > s->size - off) {
            return -2;              /* never a byte past the file's own end */
        }
        uint32_t lba = 0, run = 0;
        if (fat32_file_lba_at(s->fs, s->clus, off, &lba, &run) != 0) {
            return -2;
        }
        if (lba == 0 || lba <= s->fs->part_lba) {
            return -2;
        }
        if ((lba % ATA_PHYS_LOG) != 0 || run < ATA_PHYS_LOG) {
            return -2;
        }
        run -= run % ATA_PHYS_LOG;

        uint32_t sectors = bytes / ATA_SECTOR_SZ;
        if (sectors > run) {
            sectors = run;
        }
        if (sectors == 0 || (sectors % ATA_PHYS_LOG) != 0) {
            return -2;
        }
        int rc = s->write(lba, sectors, buf);
        if (rc != 0) {
            return rc;
        }
        uint32_t n = sectors * ATA_SECTOR_SZ;
        off   += n;
        buf   += n;
        bytes -= n;
    }
    return 0;
}

/* The stage `pos` currently lands in: stage 0 is held back, everything else
 * shares one buffer that is written as it fills. */
static uint8_t *stage_buf(sink_t *s, uint32_t base)
{
    return base == 0 ? s->scr->stage0 : s->scr->stage;
}

/*
 * Append `n` bytes. `p` may be null, in which case `fill` is repeated — the
 * padding run, which would otherwise need a kilobyte of source.
 */
static void sink_put(sink_t *s, const uint8_t *p, uint32_t n, uint8_t fill)
{
    while (n > 0 && s->rc == 0) {
        uint32_t base = (s->pos / OTG_SAVE_STAGE) * OTG_SAVE_STAGE;
        uint32_t idx  = s->pos - base;
        uint32_t take = OTG_SAVE_STAGE - idx;
        if (take > n) {
            take = n;
        }
        if (s->pos + take > s->size) {
            s->rc = -2;             /* a caller that overran its own bound */
            return;
        }
        uint8_t *buf = stage_buf(s, base);
        if (p) {
            memcpy(buf + idx, p, take);
            p += take;
        } else {
            memset(buf + idx, fill, take);
        }
        s->pos += take;
        n      -= take;

        if (s->pos - base == OTG_SAVE_STAGE && base != 0) {
            s->rc = write_region(s, base, buf, OTG_SAVE_STAGE);
        }
    }
}

/*
 * Flush what is left: a partial final stage (only when the file size is not a
 * multiple of the stage), and then STAGE 0, last of all. Returns the sink's
 * sticky error.
 */
static int sink_finish(sink_t *s)
{
    if (s->rc != 0) {
        return s->rc;
    }
    uint32_t base = (s->pos / OTG_SAVE_STAGE) * OTG_SAVE_STAGE;
    uint32_t tail = s->pos - base;
    if (tail > 0 && base != 0) {
        s->rc = write_region(s, base, s->scr->stage, tail);
        if (s->rc != 0) {
            return s->rc;
        }
    }
    uint32_t head = s->size < OTG_SAVE_STAGE ? s->size : OTG_SAVE_STAGE;
    s->rc = write_region(s, 0, s->scr->stage0, head);
    return s->rc;
}

/* ---- naming a row ------------------------------------------------------- */

typedef struct {
    uint32_t want_clus;
    int      want_dir;
    int      found;
    uint8_t  lossy;
    char    *name;          /* where to copy the exact on-disk name */
} find_t;

static int find_cb(void *ud, const fat32_dirent_t *e)
{
    find_t *f = (find_t *)ud;
    if (e->first_clus != f->want_clus) {
        return 0;
    }
    if ((e->is_dir ? 1 : 0) != f->want_dir) {
        return 0;
    }
    f->found = 1;
    f->lossy = e->name_lossy;
    uint32_t n = slen_n(e->name, FAT32_NAME_BYTES - 1u);
    memcpy(f->name, e->name, n);
    f->name[n] = '\0';
    return 1;
}

/*
 * The exact on-disk name of the entry with first cluster `clus` inside
 * directory `dir`. Returns 1 when it was found and its long name is whole,
 * 0 when it was not found or the name is lossy (unmatchable — a caller that
 * identifies files by name must not hash a mangled 8.3 name), and negative
 * on a read error.
 */
static int name_of(fat32_t *fs, uint32_t dir, uint32_t clus, int is_dir,
                   char *dst)
{
    find_t f;
    f.want_clus = clus;
    f.want_dir  = is_dir;
    f.found     = 0;
    f.lossy     = 0;
    f.name      = dst;
    dst[0] = '\0';
    if (clus == 0) {
        return 0;               /* an empty file has no cluster to match on */
    }
    int rc = fat32_readdir(fs, dir, find_cb, &f);
    if (rc != 0) {
        return rc;
    }
    if (!f.found || f.lossy || dst[0] == '\0') {
        return 0;
    }
    return 1;
}

/* Would fs/m3u.c refuse this line? Any byte below 0x20 rejects it, and so
 * does a ':' anywhere but index 1 (that is what makes "http://" not a path).
 * FAT forbids both in a name, so this is a belt to that pair of braces —
 * and the failure it prevents is a track that silently vanishes. */
static int line_is_clean(const char *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c < 0x20u || c == 0x7Fu) {
            return 0;
        }
        if (c == ':') {
            /* m3u.c allows one at index 1 (a drive letter). These lines are
             * always volume-root-absolute, so index 1 is a folder character
             * and a colon there is no more legitimate than one anywhere
             * else. FAT forbids it in a name either way. */
            return 0;
        }
    }
    return 1;
}

/* ---- save --------------------------------------------------------------- */

int otg_slot_save(fat32_t *fs, uint32_t clus, uint32_t size,
                  uint32_t lib_root_clus, const char *root_prefix,
                  const otg_save_row_t *rows, int n, uint16_t gen,
                  otg_write_fn write, otg_save_scratch_t *scr,
                  otg_save_stats_t *st)
{
    if (st) {
        memset(st, 0, sizeof *st);
    }
    if (fs == 0 || write == 0 || scr == 0 || st == 0 || root_prefix == 0) {
        return -1;
    }
    if (n < 0 || (n > 0 && rows == 0)) {
        return -1;
    }
    /* The format's own preconditions. A slot file the host made at the wrong
     * size is never written to — half a physical sector is not a write this
     * drive will take, and bouncing it would rewrite bytes nobody asked to
     * touch. */
    if (size < OTG_SLOT_FILE_MIN || (size % OTG_SLOT_SIZE_GRAIN) != 0) {
        return -1;
    }
    uint32_t prefix_len = slen_n(root_prefix, M3U_PATH_MAX);
    if (prefix_len == 0 || root_prefix[0] != '/' ||
        root_prefix[prefix_len - 1u] != '/') {
        return -1;
    }

    sink_t s;
    s.fs    = fs;
    s.clus  = clus;
    s.size  = size;
    s.write = write;
    s.scr   = scr;
    s.pos   = 0;
    s.rc    = 0;

    /* 1. The two header lines. The directive is emitted as a placeholder and
     *    patched in stage 0 once count and crc are known — which is only
     *    sound because every field in it is fixed width. */
    char hdr[OTG_HDR_LINE_BYTES];
    format_header(hdr, 0, 0, gen);
    sink_put(&s, (const uint8_t *)M3U_TAG, M3U_TAG_LEN, 0);
    sink_put(&s, (const uint8_t *)hdr, OTG_HDR_LINE_BYTES, 0);
    if (s.rc != 0) {
        return s.rc;
    }

    /* 2. The entries. The CRC covers exactly these bytes: from here to the
     *    newline that ends the last one. */
    uint32_t crc     = OTG_CRC32_INIT;   /* running, inverted below */
    uint32_t written = 0;
    scr->folder_state = 0;
    scr->folder_clus  = 0;

    for (int i = 0; i < n && s.rc == 0; i++) {
        /* The folder, cached: an album-grouped run costs one root walk, not
         * one per track. */
        if (scr->folder_state == 0 || scr->folder_clus != rows[i].dir_clus) {
            scr->folder_clus  = rows[i].dir_clus;
            scr->folder_state = 2;
            scr->folder[0]    = '\0';
            if (rows[i].dir_clus == lib_root_clus) {
                scr->folder_state = 1;   /* a track loose in the library root */
            } else {
                int fr = name_of(fs, lib_root_clus, rows[i].dir_clus, 1,
                                 scr->folder);
                if (fr < 0) {
                    /* A directory that will not read is unknown, not gone.
                     * Remembered as unusable so the rest of the album costs
                     * one counter each rather than one ATA timeout each. */
                    scr->folder_state = 3;
                } else if (fr == 1) {
                    scr->folder_state = 1;
                }
            }
        }
        if (scr->folder_state == 3) {
            st->io_err++;
            continue;
        }
        if (scr->folder_state != 1) {
            st->unsaveable++;
            continue;
        }

        int xr = name_of(fs, rows[i].dir_clus, rows[i].file_clus, 0, scr->file);
        if (xr < 0) {
            st->io_err++;
            continue;
        }
        if (xr != 1) {
            st->unsaveable++;
            continue;
        }

        /* prefix + folder + '/' + file, or prefix + file for a loose track. */
        uint32_t fl = slen_n(scr->folder, FAT32_NAME_BYTES - 1u);
        uint32_t xl = slen_n(scr->file,   FAT32_NAME_BYTES - 1u);
        uint32_t want = prefix_len + fl + (fl ? 1u : 0u) + xl;
        /*
         * M3U_PATH_MAX bounds the CANONICAL path the parser hands back, which
         * is this line without its single leading '/'. A longer one would be
         * counted skipped_long and the track would simply not be in the saved
         * playlist — so it is refused here, visibly, instead.
         */
        if (want - 1u > M3U_PATH_MAX) {
            st->unsaveable++;
            continue;
        }
        char *p = scr->path;
        memcpy(p, root_prefix, prefix_len);
        uint32_t len = prefix_len;
        if (fl) {
            memcpy(p + len, scr->folder, fl);
            len += fl;
            p[len++] = '/';
        }
        memcpy(p + len, scr->file, xl);
        len += xl;
        if (!line_is_clean(p, len)) {
            st->unsaveable++;
            continue;
        }

        /* Room for this line AND the trailer, or the list has outgrown the
         * file: stop, say so, and still write a well-formed file. */
        if (s.pos + len + 1u + OTG_END_LINE_BYTES > size) {
            st->truncated = 1;
            break;
        }
        p[len] = '\n';
        sink_put(&s, (const uint8_t *)p, len + 1u, 0);
        crc = otg_crc32_update(crc, (const uint8_t *)p, len + 1u);
        written++;
    }
    if (s.rc != 0) {
        return s.rc;
    }

    /* 3. The trailer, wherever the last entry left off. */
    char end[OTG_END_LINE_BYTES];
    format_trailer(end, gen);
    sink_put(&s, (const uint8_t *)end, OTG_END_LINE_BYTES, 0);
    if (s.rc != 0) {
        return s.rc;
    }

    /* 4. Newlines to the last byte: the old tail must not survive under the
     *    new trailer, and a blank line is what every reader ignores. */
    if (s.pos > size) {
        return -2;              /* unreachable: the checks above bound it */
    }
    sink_put(&s, 0, size - s.pos, (uint8_t)'\n');
    if (s.rc != 0) {
        return s.rc;
    }

    /* 5. Patch stage 0's directive with the real count and CRC, then flush —
     *    the partial tail stage first, STAGE 0 LAST. */
    crc = ~crc;
    st->crc     = crc;
    st->written = written;
    format_header((char *)&scr->stage0[M3U_TAG_LEN], written, crc, gen);

    int rc = sink_finish(&s);

    /*
     * Whatever happened, the bytes under this file have changed and fat32.c's
     * data-sector cache does not know it. The very next thing that happens is
     * a parse of this file, so dropping the cache is not housekeeping — it is
     * the difference between reading the playlist that was saved and reading
     * the one that was there before.
     */
    fat32_cache_drop(fs);
    return rc;
}

int otg_slot_erase(fat32_t *fs, uint32_t clus, uint32_t size, uint16_t gen,
                   otg_write_fn write, otg_save_scratch_t *scr,
                   otg_save_stats_t *st)
{
    /* The empty form IS a save of no rows: same writer, same order, same
     * tear rules — so there is one place where this format is produced. */
    return otg_slot_save(fs, clus, size, 0, "/", 0, 0, gen, write, scr, st);
}
