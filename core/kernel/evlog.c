/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/evlog.c — the on-disk event log. See evlog.h for the
 * contract and the rules it inherits from config.c; this is the mechanics.
 *
 * Nothing here touches hardware: the block write and the drive wake are
 * function pointers handed in at mount, reads go through the volume's own
 * block callback (the raw one, NOT fat32_read_file — the fs layer keeps a
 * data-sector cache with no write invalidation, and we want the platter's
 * truth for a sector we may just have written), and the clock arrives in
 * the caller's cfg_commit_env_t. The host test (tests/kernel/evlog_test.c)
 * drives every path against a RAM disk.
 */

#include "evlog.h"

/* ---- module state ------------------------------------------------------ */

static struct {
    fat32_t        *fs;          /* the mounted volume, or 0 = OFF           */
    evlog_write_fn  write;
    evlog_wake_fn   wake;
    uint32_t        first_clus;  /* CORELOG.BIN's first cluster (validated) */
    uint32_t        size;        /* its directory-entry size                 */
    uint32_t        count;       /* blocks in the file, per the header       */
    uint32_t        seq;         /* the NEXT block's sequence number         */
    uint16_t        boot;        /* this boot's id                           */
    int8_t          prev_final;  /* -1 none, 0 not final, 1 final            */
    uint8_t         enabled;
    int             last_rc;
    uint32_t        failures;    /* consecutive failed writes                */
    cfg_commit_t    commit;      /* the debounce / battery gate state        */
} g_ev;

/*
 * The capture ring. head counts bytes ever captured, tail bytes ever
 * consumed (both wrap at 2^32, which the unsigned difference copes with);
 * the ring holds [tail, head). Written from wherever uart_putc is called —
 * in practice the main loop, occasionally a driver — and read from the main
 * loop. The two indices are single words, so the worst a mid-flush capture
 * can do is land a byte the flush did not see, which the next block gets.
 */
static uint8_t           g_ring[EVLOG_RING_BYTES];
static volatile uint32_t g_head;
static volatile uint32_t g_tail;
static volatile uint32_t g_dropped;

/* Scratch for one block. uint32_t-typed so it is aligned for the halfword
 * data port ata_write_sectors requires (config.c does the same). */
static uint32_t g_blk[EVLOG_BLOCK_BYTES / 4u];

/* ---- little-endian helpers --------------------------------------------- */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

/* ---- CRC-32 ------------------------------------------------------------ */

/* Bitwise, no table, as config.c: one block per flush is nothing, and the
 * table would cost more RAM than a block. Split into update/finish so the
 * block CRC can skip its own field without a copy. */
static uint32_t crc_update(uint32_t crc, const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = (uint32_t)0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc;
}

uint32_t evlog_crc32(const uint8_t *p, uint32_t n)
{
    return ~crc_update(0xFFFFFFFFu, p, n);
}

/* ---- format ------------------------------------------------------------ */

#define EV_MAGIC_FILE   0x474F4C43u          /* 'C''L''O''G' LE */
#define EV_MAGIC_BLOCK  0x424F4C43u          /* 'C''L''O''B' LE */

/* header block */
#define H_OFF_MAGIC   0u
#define H_OFF_VERSION 4u
#define H_OFF_BSIZE   6u
#define H_OFF_COUNT   8u
#define H_OFF_FILEID  12u
#define H_OFF_CRC     16u
#define H_CRC_BYTES   16u

/* ring block */
#define B_OFF_MAGIC   0u
#define B_OFF_SEQ     4u
#define B_OFF_BOOT    8u
#define B_OFF_LEN     10u
#define B_OFF_CRC     12u
#define B_OFF_TEXT    EVLOG_HDR_BYTES

uint32_t evlog_block_index(uint32_t seq, uint32_t count)
{
    if (count < EVLOG_MIN_BLOCKS) {
        return 0;                       /* no ring: the caller checks */
    }
    return 1u + (seq % (count - 1u));
}

void evlog_header_encode(uint8_t *blk, uint32_t count, uint32_t file_id)
{
    for (uint32_t i = 0; i < EVLOG_BLOCK_BYTES; i++) {
        blk[i] = 0;
    }
    wr32(&blk[H_OFF_MAGIC],   EV_MAGIC_FILE);
    wr16(&blk[H_OFF_VERSION], (uint16_t)EVLOG_VERSION);
    wr16(&blk[H_OFF_BSIZE],   (uint16_t)EVLOG_BLOCK_BYTES);
    wr32(&blk[H_OFF_COUNT],   count);
    wr32(&blk[H_OFF_FILEID],  file_id);
    wr32(&blk[H_OFF_CRC],     evlog_crc32(blk, H_CRC_BYTES));
}

int evlog_header_decode(const uint8_t *blk, uint32_t *count, uint32_t *file_id)
{
    if (blk == 0 || count == 0 || file_id == 0) {
        return 0;
    }
    if (rd32(&blk[H_OFF_MAGIC]) != EV_MAGIC_FILE) {
        return 0;
    }
    if (rd16(&blk[H_OFF_VERSION]) != EVLOG_VERSION) {
        return 0;                       /* a layout we do not understand */
    }
    if (rd16(&blk[H_OFF_BSIZE]) != EVLOG_BLOCK_BYTES) {
        return 0;
    }
    uint32_t n = rd32(&blk[H_OFF_COUNT]);
    if (n < EVLOG_MIN_BLOCKS || n > EVLOG_MAX_BLOCKS) {
        return 0;
    }
    if (evlog_crc32(blk, H_CRC_BYTES) != rd32(&blk[H_OFF_CRC])) {
        return 0;
    }
    *count   = n;
    *file_id = rd32(&blk[H_OFF_FILEID]);
    return 1;
}

/* CRC over the block minus its own CRC field: [0, 12) ++ [16, 2048). */
static uint32_t block_crc(const uint8_t *blk)
{
    uint32_t c = crc_update(0xFFFFFFFFu, blk, B_OFF_CRC);
    c = crc_update(c, &blk[B_OFF_TEXT], EVLOG_BLOCK_BYTES - B_OFF_TEXT);
    return ~c;
}

/* Stamp the header + CRC onto a block whose text is ALREADY in place at
 * blk + EVLOG_HDR_BYTES (and whose padding is already zero). The flush
 * builds its block in the static buffer this way rather than through a
 * 2 KB stack copy — the hw build guards frames at 1 KB. */
static void block_finish(uint8_t *blk, uint32_t seq, uint16_t boot,
                         uint32_t len, int final)
{
    wr32(&blk[B_OFF_MAGIC], EV_MAGIC_BLOCK);
    wr32(&blk[B_OFF_SEQ],   seq);
    wr16(&blk[B_OFF_BOOT],  boot);
    wr16(&blk[B_OFF_LEN],   (uint16_t)(len | (final ? EVLOG_LEN_FINAL : 0u)));
    wr32(&blk[B_OFF_CRC],   block_crc(blk));
}

void evlog_block_encode(uint8_t *blk, uint32_t seq, uint16_t boot,
                        const uint8_t *text, uint32_t len, int final)
{
    if (len > EVLOG_TEXT_BYTES) {
        len = EVLOG_TEXT_BYTES;
    }
    for (uint32_t i = 0; i < EVLOG_BLOCK_BYTES; i++) {
        blk[i] = 0;                     /* deterministic padding: the CRC covers it */
    }
    for (uint32_t i = 0; i < len; i++) {
        blk[B_OFF_TEXT + i] = text[i];
    }
    block_finish(blk, seq, boot, len, final);
}

int evlog_block_decode(const uint8_t *blk, uint32_t *seq, uint16_t *boot,
                       uint32_t *len, int *final)
{
    if (blk == 0 || seq == 0 || boot == 0 || len == 0 || final == 0) {
        return 0;
    }
    if (rd32(&blk[B_OFF_MAGIC]) != EV_MAGIC_BLOCK) {
        return 0;
    }
    uint32_t l = rd16(&blk[B_OFF_LEN]);
    if ((l & EVLOG_LEN_MASK) > EVLOG_TEXT_BYTES) {
        return 0;
    }
    /* CRC last: the expensive check, and the one that decides whether any
     * of the above was real. */
    if (block_crc(blk) != rd32(&blk[B_OFF_CRC])) {
        return 0;
    }
    *seq   = rd32(&blk[B_OFF_SEQ]);
    *boot  = rd16(&blk[B_OFF_BOOT]);
    *len   = l & EVLOG_LEN_MASK;
    *final = (l & EVLOG_LEN_FINAL) ? 1 : 0;
    return 1;
}

/* ---- the tap ----------------------------------------------------------- */

void evlog_capture(uint8_t b)
{
    uint32_t h = g_head;
    if ((uint32_t)(h - g_tail) >= EVLOG_RING_BYTES) {
        g_tail    = g_tail + 1u;        /* drop the oldest byte */
        g_dropped = g_dropped + 1u;
    }
    g_ring[h & (EVLOG_RING_BYTES - 1u)] = b;
    g_head = h + 1u;
}

/* ---- LBA resolution ---------------------------------------------------- */

/*
 * The block for `seq` -> an absolute LBA, fully validated. Runs at mount,
 * before every write, and for the diagnostic probe — never cached. Every
 * line is a reason to refuse. `enabled` is deliberately NOT checked here:
 * mount needs it before enabling, and the gate above checks it.
 */
static int block_lba(uint32_t block, uint32_t *out)
{
    *out = 0;
    if (g_ev.fs == 0 || g_ev.first_clus == 0 || g_ev.count < EVLOG_MIN_BLOCKS) {
        return -1;
    }
    if (block >= g_ev.count) {
        return -1;
    }
    /* The block must lie inside the size the directory entry declared —
     * the count came from the header, and mount checked the two agree, but
     * this is the last line before an address. */
    if (block > (0xFFFFFFFFu / EVLOG_BLOCK_BYTES) ||
        (block + 1u) * EVLOG_BLOCK_BYTES > g_ev.size) {
        return -1;
    }

    uint32_t base = 0, run = 0;
    if (fat32_file_lba_at(g_ev.fs, g_ev.first_clus, block * EVLOG_BLOCK_BYTES,
                          &base, &run) != 0) {
        return -1;
    }
    /* The whole block must sit inside the cluster it resolved to: a
     * cluster smaller than a block (a 1 KB-cluster volume) refuses here,
     * because the next cluster is one we have not resolved. */
    if (run < EVLOG_BLOCK_SECTORS) {
        return -1;
    }
    /* Physical-sector alignment: the drive IDNFs a misaligned access, and
     * bouncing would mean rewriting bytes nobody asked to touch. */
    if ((base % ATA_PHYS_LOG) != 0) {
        return -1;
    }
    /* Belt and braces on top of fat32_file_lba_at: never LBA 0, never at
     * or below the partition's own first sector. */
    if (base == 0 || base <= g_ev.fs->part_lba) {
        return -1;
    }
    *out = base;
    return 0;
}

static int seq_lba(uint32_t seq, uint32_t *out)
{
    return block_lba(evlog_block_index(seq, g_ev.count), out);
}

int evlog_probe_lba(uint32_t seq, uint32_t *lba)
{
    if (lba == 0) {
        return -1;
    }
    return seq_lba(seq, lba);
}

int evlog_probe_header_lba(uint32_t *lba)
{
    if (lba == 0) {
        return -1;
    }
    return block_lba(0, lba);
}

/* ---- mount ------------------------------------------------------------- */

/* Weak, as in config.c: the host test links this file with no timer and
 * retries without waiting. */
__attribute__((weak)) void sleep_ms(uint32_t ms);

static void ev_settle(void)
{
    if (sleep_ms) {
        sleep_ms(EVLOG_READ_RETRY_MS);
    }
}

/* One block, straight off the platter, with the settle-and-retry. */
static int read_block_raw(uint32_t lba)
{
    int rc = g_ev.fs->read(g_ev.fs->ud, lba, EVLOG_BLOCK_SECTORS, g_blk);
    for (uint32_t attempt = 0; rc != 0 && attempt < EVLOG_READ_RETRIES; attempt++) {
        ev_settle();
        rc = g_ev.fs->read(g_ev.fs->ud, lba, EVLOG_BLOCK_SECTORS, g_blk);
    }
    return rc;
}

/* Root-directory scan for the 8.3 name, as config.c finds its file: by
 * enumeration, never by a hardcoded address. */
static int ev_root_cb(void *ud, const fat32_dirent_t *e)
{
    (void)ud;
    if (e->is_dir) {
        return 0;
    }
    const char *want = EVLOG_FILE_NAME;
    const char *have = e->short_name;
    int i = 0;
    for (; want[i] != '\0' && have[i] != '\0'; i++) {
        char a = want[i], b = have[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) {
            return 0;
        }
    }
    if (want[i] != have[i]) {
        return 0;
    }
    g_ev.first_clus = e->first_clus;
    g_ev.size       = e->size;
    return 1;
}

/*
 * Read ring slot `slot` and say whether it belongs to the CURRENT lap:
 * valid and seq >= s0 (signed difference, so a wrapped seq space still
 * compares the right way round). Returns 1 / 0, or -1 when the slot could
 * not be read at all — which is not a 0.
 */
static int slot_is_current(uint32_t slot, uint32_t s0, uint32_t *seq_out,
                           uint16_t *boot_out, int *final_out)
{
    uint32_t lba = 0;
    if (block_lba(1u + slot, &lba) != 0) {
        return -1;
    }
    if (read_block_raw(lba) != 0) {
        return -1;
    }
    uint32_t seq = 0, len = 0;
    uint16_t boot = 0;
    int      final = 0;
    if (!evlog_block_decode((const uint8_t *)g_blk, &seq, &boot, &len, &final)) {
        return 0;
    }
    if ((int32_t)(seq - s0) < 0) {
        return 0;
    }
    *seq_out   = seq;
    *boot_out  = boot;
    *final_out = final;
    return 1;
}

/*
 * Find the write cursor: binary search for the last slot of the current
 * lap (see evlog.h). Sets seq/boot/prev_final. Returns 0, or -1 when a
 * slot would not read (the cursor is then unknowable: stay OFF).
 *
 * The anchor is slot 0 — the first slot of every lap. If slot 0 is not a
 * valid block, slot 1 is tried: a wrapped ring whose slot-0 write was torn
 * still holds the previous lap intact from slot 1 on, and anchoring there
 * finds its newest block, so the next seq is the torn one's and it is
 * simply written again. Only when neither holds a block is the ring fresh.
 *
 * A torn block ELSEWHERE in the lap breaks the predicate's monotonicity at
 * one slot: if the search happens to probe it the search stops early and
 * the blocks after it are re-written; if not, it is invisible. Either way
 * the cursor is a slot of this lap, inside the file — the ordering of the
 * dump can suffer, the disk cannot.
 */
static int find_cursor(void)
{
    uint32_t slots = g_ev.count - 1u;
    uint32_t seq0 = 0, seqk = 0;
    uint16_t boot0 = 0, bootk = 0;
    int      fin0 = 0, fink = 0;
    uint32_t anchor = 0;

    int r = slot_is_current(0, 0, &seq0, &boot0, &fin0);
    if (r < 0) {
        return -1;
    }
    if (r == 0 && slots > 1u) {
        anchor = 1;
        r = slot_is_current(1, 0, &seq0, &boot0, &fin0);
        if (r < 0) {
            return -1;
        }
    }
    if (r == 0) {
        /* Neither slot holds a block: nothing has been written. Fresh. */
        g_ev.seq        = 0;
        g_ev.boot       = 1;
        g_ev.prev_final = -1;
        return 0;
    }
    /* Invariant: slot lo is current, slot hi (virtual at `slots`) is not. */
    uint32_t lo = anchor, hi = slots;
    seqk = seq0; bootk = boot0; fink = fin0;
    while (hi - lo > 1u) {
        uint32_t mid = lo + (hi - lo) / 2u;
        uint32_t s = 0; uint16_t b = 0; int f = 0;
        r = slot_is_current(mid, seq0, &s, &b, &f);
        if (r < 0) {
            return -1;
        }
        if (r) {
            lo = mid; seqk = s; bootk = b; fink = f;
        } else {
            hi = mid;
        }
    }
    g_ev.seq        = seqk + 1u;
    g_ev.boot       = (uint16_t)(bootk + 1u);
    g_ev.prev_final = (int8_t)(fink ? 1 : 0);
    return 0;
}

int evlog_mount(fat32_t *fs, evlog_write_fn write, evlog_wake_fn wake)
{
    /* Full reset: a second call must not inherit the first one's state.
     * The capture ring is NOT reset — bytes narrated before the mount are
     * exactly the ones a boot log wants. */
    g_ev.fs         = 0;
    g_ev.write      = 0;
    g_ev.wake       = 0;
    g_ev.first_clus = 0;
    g_ev.size       = 0;
    g_ev.count      = 0;
    g_ev.seq        = 0;
    g_ev.boot       = 0;
    g_ev.prev_final = -1;
    g_ev.enabled    = 0;
    g_ev.last_rc    = 0;
    g_ev.failures   = 0;
    cfg_commit_clear(&g_ev.commit);
    g_ev.commit.deferred_logged = 0;

    if (fs == 0 || write == 0) {
        return 0;
    }

    /* Only a read error is retried: ECORRUPT is structural. */
    int rc = fat32_readdir(fs, fs->root_clus, ev_root_cb, 0);
    for (uint32_t attempt = 0;
         rc == FAT32_EIO && attempt < EVLOG_READ_RETRIES; attempt++) {
        ev_settle();
        g_ev.first_clus = 0;
        g_ev.size       = 0;
        rc = fat32_readdir(fs, fs->root_clus, ev_root_cb, 0);
    }
    if (rc != 0 || g_ev.first_clus == 0) {
        g_ev.first_clus = 0;
        g_ev.size       = 0;
        return 0;                       /* absent, or the root would not read */
    }
    if (g_ev.size < EVLOG_MIN_BLOCKS * EVLOG_BLOCK_BYTES ||
        (g_ev.size % EVLOG_BLOCK_BYTES) != 0) {
        g_ev.first_clus = 0;
        g_ev.size       = 0;
        return 0;                       /* not a whole number of blocks */
    }

    /* Provisionally bind so block_lba() can run; everything below makes
     * that conditional on the header actually validating. */
    g_ev.fs    = fs;
    g_ev.count = g_ev.size / EVLOG_BLOCK_BYTES;   /* claimed; the header must agree */

    uint32_t lba0 = 0;
    if (block_lba(0, &lba0) != 0 || read_block_raw(lba0) != 0) {
        g_ev.fs = 0; g_ev.count = 0;
        return 0;
    }
    uint32_t hcount = 0, fid = 0;
    if (!evlog_header_decode((const uint8_t *)g_blk, &hcount, &fid) ||
        hcount != g_ev.count) {
        g_ev.fs = 0; g_ev.count = 0;
        return 0;                       /* wrong magic/size/CRC: OFF, silently */
    }

    if (find_cursor() != 0) {
        g_ev.fs = 0; g_ev.count = 0;
        return 0;                       /* a slot would not read: cursor unknown */
    }

    /* The next block's address must resolve NOW, or we would only learn
     * that at the first flush. */
    uint32_t next = 0;
    if (seq_lba(g_ev.seq, &next) != 0) {
        g_ev.fs = 0; g_ev.count = 0;
        return 0;
    }

    g_ev.write   = write;
    g_ev.wake    = wake;
    g_ev.enabled = 1;
    return 1;
}

int evlog_enabled(void)      { return g_ev.enabled ? 1 : 0; }
uint32_t evlog_seq(void)     { return g_ev.seq; }
uint16_t evlog_boot_id(void) { return g_ev.boot; }
int evlog_prev_final(void)   { return g_ev.prev_final; }
uint32_t evlog_dropped(void) { return g_dropped; }
int evlog_last_rc(void)      { return g_ev.last_rc; }
uint32_t evlog_failures(void){ return g_ev.failures; }

uint32_t evlog_pending(void)
{
    return (uint32_t)(g_head - g_tail);
}

/* ---- flush ------------------------------------------------------------- */

/* "<evlog: N B dropped>\n" into `d`; returns the length. Decimal, no libc. */
static uint32_t put_drop_marker(uint8_t *d, uint32_t n)
{
    static const char pre[] = "<evlog: ";
    static const char post[] = " B dropped>\n";
    uint32_t i = 0;
    for (const char *s = pre; *s; s++) d[i++] = (uint8_t)*s;
    char tmp[10];
    int t = 0;
    do { tmp[t++] = (char)('0' + n % 10u); n /= 10u; } while (n && t < 10);
    while (t > 0) d[i++] = (uint8_t)tmp[--t];
    for (const char *s = post; *s; s++) d[i++] = (uint8_t)*s;
    return i;
}

int evlog_flush(int mode, const cfg_commit_env_t *env_in)
{
    if (!g_ev.enabled || env_in == 0) {
        return EVLOG_FLUSH_NONE;
    }

    uint32_t tail0 = g_tail;
    uint32_t avail = (uint32_t)(g_head - tail0);
    if (avail == 0) {
        return EVLOG_FLUSH_NONE;        /* nothing to say, even when forced */
    }
    if (mode == CFG_COMMIT_IDLE) {
        if (avail < EVLOG_TEXT_BYTES) {
            return EVLOG_FLUSH_NONE;    /* wait for a whole block */
        }
        if (env_in->parked) {
            return EVLOG_FLUSH_NONE;    /* never spin the platters up for a log */
        }
    }

    /* The settings save's gate, with its debounce and battery rules. The
     * pending flag is raised when a block first becomes writable, so an
     * idle block waits CFG_SAVE_DEBOUNCE_US from then; a forced flush
     * ignores the debounce by construction. */
    if (!g_ev.commit.dirty) {
        cfg_commit_touch(&g_ev.commit, env_in->now_us);
    }
    cfg_commit_env_t env = *env_in;
    env.writable = 1;                   /* this module's own writability */
    int gate = cfg_commit_gate(&g_ev.commit, mode, &env);
    if (gate == CFG_GATE_DEFER_LOG) {
        return EVLOG_FLUSH_DEFERRED;
    }
    if (gate == CFG_GATE_DEFER_QUIET) {
        return EVLOG_FLUSH_DEFERRED_QUIET;
    }
    if (gate != CFG_GATE_WRITE && gate != CFG_GATE_WRITE_WAKE) {
        return EVLOG_FLUSH_NONE;
    }
    if (gate == CFG_GATE_WRITE_WAKE && g_ev.wake) {
        (void)g_ev.wake();              /* spin-up on the read path, not in the DRQ budget */
    }

    /* One block per pass at idle; a forced or last flush drains the ring
     * (bounded), so the newest lines — the ones that say why the device is
     * going down — reach the platter before it stops (evlog.h). */
    uint32_t limit = (mode == CFG_COMMIT_IDLE) ? 1u : EVLOG_FORCE_MAX_BLOCKS;
    uint32_t wrote = 0;
    while (wrote < limit && avail != 0) {
        /* RE-RESOLVE the address. Nothing above this line is a cached LBA. */
        uint32_t lba = 0;
        if (seq_lba(g_ev.seq, &lba) != 0) {
            g_ev.last_rc = -2;
            goto failed;
        }

        /* Build the block IN the static buffer: zero it, a drop marker first if
         * the ring overflowed since the last block, then as much of the ring
         * as fits, then the header. */
        uint8_t *blk  = (uint8_t *)g_blk;
        uint8_t *text = blk + B_OFF_TEXT;
        for (uint32_t i = 0; i < EVLOG_BLOCK_BYTES; i++) {
            blk[i] = 0;
        }
        uint32_t n    = 0;
        uint32_t drop = g_dropped;
        if (drop) {
            n = put_drop_marker(text, drop);
        }
        uint32_t take = avail;
        if (take > EVLOG_TEXT_BYTES - n) {
            take = EVLOG_TEXT_BYTES - n;
        }
        for (uint32_t i = 0; i < take; i++) {
            text[n + i] = g_ring[(tail0 + i) & (EVLOG_RING_BYTES - 1u)];
        }
        n += take;
        block_finish(blk, g_ev.seq, g_ev.boot, n, mode != CFG_COMMIT_IDLE);

        int rc = g_ev.write(lba, EVLOG_BLOCK_SECTORS, g_blk);
        g_ev.last_rc = rc;
        (void)cfg_commit_result(&g_ev.commit, rc, env_in->now_us);
        if (rc != 0) {
            goto failed;
        }

        /* On the platter. Consume what was written (a capture that overflowed
         * mid-copy advanced tail itself; never move it backwards), clear the
         * drop count the marker reported, advance the cursor. */
        {
            uint32_t want = tail0 + take;
            if ((int32_t)(want - g_tail) > 0) {
                g_tail = want;
            }
        }
        g_dropped = g_dropped - drop;
        g_ev.seq++;
        g_ev.failures = 0;
        wrote++;

        /* The next block starts where this one stopped consuming. */
        tail0 = g_tail;
        avail = (uint32_t)(g_head - tail0);
    }
    return EVLOG_FLUSH_WROTE;

failed:
    if (wrote != 0) {
        /* A drain that landed something and then failed: what landed is on
         * the platter with its seq advanced; the rest waits for the next
         * flush like any other failure, but the call did write. */
        g_ev.failures++;
        cfg_commit_clear(&g_ev.commit);
        return EVLOG_FLUSH_WROTE;
    }
    cfg_commit_clear(&g_ev.commit);     /* a fresh attempt gets its own debounce */
    g_ev.failures++;
    if (g_ev.failures >= EVLOG_MAX_FAILURES) {
        /* A drive that refused this many times in a row is not going to
         * take it. Off for the session; About shows the state. */
        g_ev.enabled = 0;
    }
    return EVLOG_FLUSH_FAILED;
}
