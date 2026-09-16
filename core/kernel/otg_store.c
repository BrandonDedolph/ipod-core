/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/otg_store.c — the On-The-Go live list in COREOTG.DAT.
 * See otg_store.h for the policy; this is its mechanics.
 *
 * =========================================================================
 * *** THIS WRITE PATH HAS NOT BEEN QUALIFIED ON HARDWARE. ***
 *
 * kernel/config.c's banner records the procedure every writer to the user's
 * disk owes, and it is not optional for a third one. Before the FIRST add is
 * allowed to reach the platter on a real iPod, in this order:
 *
 *   1. Use a SCRATCH disk, or one restored from a backup you have actually
 *      verified restores.
 *   2. On the HOST, before flashing:
 *          sudo python3 tools/make_otg.py --verify /dev/sdX
 *      which walks the root for COREOTG.DAT and prints the absolute 512-byte
 *      LBA of every cluster run of both slots, computed the same way
 *      fat32_file_lba_at() computes them. Write the numbers down.
 *   3. Boot and read the UART line
 *          core: otg load <n> writable <w> seq <s> lba <slot0>/<slot1>
 *      Those two addresses MUST equal the FIRST run of each slot from step 2.
 *      If they do not, stop: do not add anything, power the device off.
 *   4. Only then hold Select on a song, wait for "core: otg save rc 00000000",
 *      power-cycle, and confirm the list came back AND that the music is
 *      still there and `fsck.vfat -n` / `chkdsk` reports the volume clean.
 *   5. Repeat a few times and confirm the two slots ALTERNATE (--verify
 *      prints both slots' seq).
 *
 * If any step fails the correct response is to make otg_store_writable()
 * return 0 unconditionally, not to "fix it quickly".
 * =========================================================================
 *
 * WHY THIS IS SAFE (when the addresses are right). The host pre-creates
 * COREOTG.DAT at a fixed size. The device only ever overwrites bytes inside
 * the clusters that file already owns, so no FAT entry, no directory entry,
 * no free-cluster search and no FSInfo is ever written; the file's own data
 * is the only thing on the disk that changes. The whole residual risk is
 * "is the address right?", and everything below is in service of answering
 * that conservatively and refusing when it cannot.
 *
 * WHAT THIS MODULE REFUSES TO DO
 *   - Create, extend, truncate, move or delete the file. (Host tool's job.)
 *   - Address a byte outside the two slots, or outside the cluster that a
 *     run resolved to.
 *   - Cache an LBA. Every read and every write re-resolves.
 *   - Write to a misaligned LBA, LBA 0, or anything at or below the
 *     partition's first sector.
 *   - Write anything at all if the file was not found at mount time.
 */

#include "otg_store.h"

#include "hw/ata.h"
#include "../fs/fat32.h"
#include "../library/otg.h"

/* ---- module state ------------------------------------------------------ */

/*
 * Everything learned at mount. Note what is NOT here: any target LBA. Every
 * one is recomputed inside the read and write paths, deliberately — a cached
 * address is an address nobody re-checks.
 */
static struct {
    fat32_t     *fs;          /* the mounted volume, or 0 = module disabled */
    otg_write_fn write;
    otg_wake_fn  wake;
    uint32_t     first_clus;  /* COREOTG.DAT's first cluster                */
    uint32_t     size;        /* its directory-entry size                   */
    uint32_t     seq;         /* sequence of the newest valid slot seen     */
    uint8_t      slot;        /* which slot that was; the next write goes to
                               * the OTHER one                              */
    uint8_t      found;       /* 1 once the file resolved to a usable run   */
    int          last_rc;
    cfg_commit_t commit;
} g_otg;

/* Scratch for one slot. uint32_t-typed so it is aligned for the halfword
 * data port the write path requires. 5120 B of .bss. */
static uint32_t g_slot_buf[OTG_SLOT_BYTES / 4u];

/* ---- slot format -------------------------------------------------------- */

/*
 * On-disk slot (OTG_SLOT_BYTES = 5120 bytes, little-endian):
 *
 *   off    size  field
 *   0      4     magic     'C''O''T''G'
 *   4      2     version   OTG_VERSION
 *   6      2     count     entries that follow, 0..OTG_MAX
 *   8      4     seq       monotonic write counter (wrapping)
 *   12     2     gen       the list's mutation counter when it was saved
 *   14     2     flags     reserved, written 0
 *   16     4096  entries   count * { u32 folder_hash, u32 file_hash },
 *                          the rest zero
 *   4112   4     reserved  written 0
 *   4116   1000  padding   zero
 *   5116   4     crc32     CRC-32 over bytes [0, 5116) — EVERYTHING above
 *
 * The CRC covers the HEADER, not just the entries: `seq` is what decides
 * which slot wins, so a corrupt seq under a valid payload CRC would let a
 * stale or garbage slot beat a good one. Covering the zero padding too costs
 * nothing and removes a "which bytes are covered" question from the format.
 */
#define O_OFF_MAGIC     0u
#define O_OFF_VERSION   4u
#define O_OFF_COUNT     6u
#define O_OFF_SEQ       8u
#define O_OFF_GEN       12u
#define O_OFF_FLAGS     14u
#define O_OFF_ENTRIES   16u
#define O_ENTRY_BYTES   8u
#define O_OFF_RESERVED  (O_OFF_ENTRIES + OTG_MAX * O_ENTRY_BYTES)  /* 4112 */
#define O_OFF_CRC       (OTG_SLOT_BYTES - 4u)                      /* 5116 */

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

/* ---- codec (host-testable, no hardware) -------------------------------- */

/*
 * Is `a` strictly newer than `b` in the wrapping sequence space? Signed
 * difference, so 0xFFFFFFFF -> 0 reads as "newer" rather than as a
 * four-billion step backwards. config_seq_newer()'s rule, restated rather
 * than included: pulling in kernel/config.h would drag ui/settings.h and the
 * palette into this module and into its host test for one line of arithmetic.
 * tests/kernel/otg_store_test.c pins the wrap.
 */
static int seq_newer(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

void otg_slot_encode(uint8_t *slot, const otg_list_t *l, uint32_t seq)
{
    if (slot == 0 || l == 0) {
        return;
    }
    for (uint32_t i = 0; i < OTG_SLOT_BYTES; i++) {
        slot[i] = 0;               /* deterministic padding; the CRC covers it */
    }

    uint32_t n = l->n;
    if (n > OTG_MAX) {
        n = OTG_MAX;               /* a caller cannot make us write more */
    }

    wr32(&slot[O_OFF_MAGIC],   OTG_MAGIC);
    wr16(&slot[O_OFF_VERSION], (uint16_t)OTG_VERSION);
    wr16(&slot[O_OFF_COUNT],   (uint16_t)n);
    wr32(&slot[O_OFF_SEQ],     seq);
    wr16(&slot[O_OFF_GEN],     l->gen);
    wr16(&slot[O_OFF_FLAGS],   0);

    for (uint32_t i = 0; i < n; i++) {
        uint8_t *e = &slot[O_OFF_ENTRIES + i * O_ENTRY_BYTES];
        wr32(&e[0], l->e[i].folder_hash);
        wr32(&e[4], l->e[i].file_hash);
    }

    wr32(&slot[O_OFF_CRC], otg_crc32(slot, O_OFF_CRC));
}

/*
 * The acceptance test, without the entry copy: magic, version, count and CRC.
 * Split out so mount() can decide WHICH slot wins before it commits the
 * caller's list to one slot's contents — otg_slot_decode() otg_init()s the
 * list first, so calling it on the loser would blank a list the winner had
 * already filled, and a second 4 KB buffer to decode into is 4 KB of .bss for
 * an arithmetic comparison. One acceptance test, two callers.
 */
static int slot_peek(const uint8_t *slot, uint32_t *count, uint32_t *seq)
{
    if (rd32(&slot[O_OFF_MAGIC]) != OTG_MAGIC) {
        return 0;
    }
    /* Version 0 is not a version; a version we do not know is a slot written
     * by a future build whose entries we cannot interpret, so we decline
     * rather than guess. There is no `length` escape hatch here because there
     * is nothing optional in the payload: an entry is eight bytes or it is
     * not an entry. */
    uint32_t ver = rd16(&slot[O_OFF_VERSION]);
    if (ver == 0 || ver > OTG_VERSION) {
        return 0;
    }
    uint32_t n = rd16(&slot[O_OFF_COUNT]);
    if (n > OTG_MAX) {
        return 0;
    }
    /* CRC last: it is the expensive check, and it is the one that decides
     * whether any of the above was real. */
    if (otg_crc32(slot, O_OFF_CRC) != rd32(&slot[O_OFF_CRC])) {
        return 0;
    }
    *count = n;
    *seq   = rd32(&slot[O_OFF_SEQ]);
    return 1;
}

int otg_slot_decode(const uint8_t *slot, otg_list_t *l, uint32_t *seq)
{
    if (slot == 0 || l == 0 || seq == 0) {
        return 0;
    }
    uint32_t count = 0;
    if (!slot_peek(slot, &count, seq)) {
        return 0;
    }

    otg_init(l);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = &slot[O_OFF_ENTRIES + i * O_ENTRY_BYTES];
        uint32_t fh = rd32(&e[0]);
        uint32_t xh = rd32(&e[4]);
        if (fh == 0 && xh == 0) {
            /* The padding's own value. A CRC-valid slot is proof the bytes
             * are the bytes that were written — not proof they are sane (a
             * hand-edited file, a host tool bug). A null locator binds to
             * nothing and would render as a permanent "Not on this iPod"
             * row nobody added, so it is dropped rather than stored. */
            continue;
        }
        l->e[l->n].folder_hash = fh;
        l->e[l->n].file_hash   = xh;
        l->n = (uint16_t)(l->n + 1u);
    }
    /* gen travels with the list: a saved slot playlist written later in the
     * session must carry the gen the list actually has, not a fresh 0. */
    l->gen = rd16(&slot[O_OFF_GEN]);
    return 1;
}

/* ---- addressing --------------------------------------------------------- */

/*
 * Resolve the run that starts at `byte_off` inside COREOTG.DAT and say how
 * many 512-byte sectors of it are safely addressable. Returns 0 and sets
 * *lba / *sectors on success; negative otherwise, with both zeroed.
 *
 * Runs at mount, before every read and before every write — never cached.
 * Every line is a reason to refuse.
 */
static int run_at(uint32_t byte_off, uint32_t *lba, uint32_t *sectors)
{
    *lba     = 0;
    *sectors = 0;

    if (!g_otg.found || g_otg.fs == 0) {
        return -1;
    }
    if (g_otg.size < OTG_STORE_MIN_BYTES) {
        return -1;
    }
    /* Never address a byte the directory entry does not cover. */
    if (byte_off >= g_otg.size || byte_off >= OTG_STORE_MIN_BYTES) {
        return -1;
    }
    /* fat32_file_lba_at wants a 512-byte multiple; every offset this module
     * forms is a multiple of 512 by construction, so a failure here is a bug,
     * not an input. */
    if ((byte_off % ATA_SECTOR_SZ) != 0) {
        return -1;
    }

    uint32_t base = 0, run = 0;
    if (fat32_file_lba_at(g_otg.fs, g_otg.first_clus, byte_off, &base, &run) != 0) {
        return -1;
    }
    if (run == 0) {
        return -1;
    }
    /* Physical-sector alignment. The drive IDNFs a misaligned or
     * sub-physical-sector access, so an unaligned base is not a slow path
     * here, it is a refusal: bouncing it would mean read-modify-writing bytes
     * the caller never asked to touch. */
    if ((base % ATA_PHYS_LOG) != 0) {
        return -1;
    }
    /* A run shorter than one physical sector, or a ragged one, cannot be the
     * unit of a write on this drive. A volume with 512-byte clusters lands
     * here and the store simply stays off. */
    if (run < ATA_PHYS_LOG) {
        return -1;
    }
    run -= run % ATA_PHYS_LOG;

    /* Belt and braces on top of fat32_file_lba_at: never LBA 0, never at or
     * below the partition's own first sector. */
    if (base == 0 || base <= g_otg.fs->part_lba) {
        return -1;
    }

    /* Never hand back more than the two-slot region has left. */
    uint32_t left = (OTG_STORE_MIN_BYTES - byte_off) / ATA_SECTOR_SZ;
    if (run > left) {
        run = left;
    }
    if (run == 0) {
        return -1;
    }

    *lba     = base;
    *sectors = run;
    return 0;
}

int otg_store_probe_lba(uint32_t slot, uint32_t *lba)
{
    if (lba == 0) {
        return -1;
    }
    *lba = 0;
    if (slot >= OTG_SLOTS_ON_DISK) {
        return -1;
    }
    uint32_t sectors = 0;
    /* The same resolver a write takes, so what this prints IS what a save
     * would target. Read-only: it cannot itself put a byte on the disk. */
    return run_at(slot * OTG_SLOT_BYTES, lba, &sectors);
}

/*
 * Prove that EVERY run of slot `slot` resolves, before anything is read from
 * or written to any of them. A slot that straddles a cluster boundary is two
 * or more runs, and a partial write — the first run landed, the second
 * refused — would leave a slot that is neither the old one nor the new one.
 * Both slots' CRCs make that survivable, but refusing up front is better than
 * surviving.
 */
static int slot_resolves(uint32_t slot)
{
    uint32_t off = slot * OTG_SLOT_BYTES;
    uint32_t end = off + OTG_SLOT_BYTES;
    while (off < end) {
        uint32_t lba = 0, sectors = 0;
        if (run_at(off, &lba, &sectors) != 0) {
            return 0;
        }
        uint32_t bytes = sectors * ATA_SECTOR_SZ;
        if (bytes > end - off) {
            bytes = end - off;
        }
        if (bytes == 0) {
            return 0;
        }
        off += bytes;
    }
    return 1;
}

/* ---- reading ------------------------------------------------------------ */

/* Weak, as in config.c and evlog.c: the host test links this file with no
 * timer and retries without waiting. */
__attribute__((weak)) void sleep_ms(uint32_t ms);

static void otg_settle(void)
{
    if (sleep_ms) {
        sleep_ms(OTG_READ_RETRY_MS);
    }
}

/* One run, straight off the platter, with the settle-and-retry. */
static int read_run_raw(uint32_t lba, uint32_t sectors, uint8_t *dst)
{
    fat32_t *fs = g_otg.fs;
    int rc = fs->read(fs->ud, lba, sectors, dst);
    for (uint32_t attempt = 0; rc != 0 && attempt < OTG_READ_RETRIES; attempt++) {
        otg_settle();
        rc = fs->read(fs->ud, lba, sectors, dst);
    }
    return rc;
}

/*
 * Read a whole slot into g_slot_buf, run by run. Reads go through the
 * volume's RAW block callback at the SAME absolute LBAs a save writes — one
 * address, one resolver, so load and save cannot drift apart. (Never through
 * the cached file path: that would serve a stale copy of a sector we just
 * wrote, and we want the platter's truth here.)
 *
 * Returns 0 when the whole slot is in the buffer, negative otherwise.
 */
static int read_slot(uint32_t slot)
{
    uint8_t *dst  = (uint8_t *)g_slot_buf;
    uint32_t off  = slot * OTG_SLOT_BYTES;
    uint32_t end  = off + OTG_SLOT_BYTES;
    uint32_t done = 0;

    while (off < end) {
        uint32_t lba = 0, sectors = 0;
        if (run_at(off, &lba, &sectors) != 0) {
            return -2;
        }
        uint32_t bytes = sectors * ATA_SECTOR_SZ;
        if (bytes > end - off) {
            bytes    = end - off;
            sectors  = bytes / ATA_SECTOR_SZ;
        }
        if (sectors == 0 || (sectors % ATA_PHYS_LOG) != 0) {
            return -2;
        }
        if (read_run_raw(lba, sectors, dst + done) != 0) {
            return -1;
        }
        off  += bytes;
        done += bytes;
    }
    return 0;
}

/* ---- mount -------------------------------------------------------------- */

/* Root-directory scan for the 8.3 short name, as config.c and evlog.c find
 * their files: by ENUMERATION, never by a hardcoded address. */
static int otg_root_cb(void *ud, const fat32_dirent_t *e)
{
    (void)ud;
    if (e->is_dir) {
        return 0;
    }
    const char *want = OTG_FILE_NAME;
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
    g_otg.first_clus = e->first_clus;
    g_otg.size       = e->size;
    return 1;                       /* stop the enumeration — found it */
}

int otg_store_mount(fat32_t *fs, otg_write_fn write, otg_wake_fn wake,
                    otg_list_t *out)
{
    /* Full reset: a second call must not inherit the first one's state. */
    g_otg.fs         = 0;
    g_otg.write      = 0;
    g_otg.wake       = 0;
    g_otg.first_clus = 0;
    g_otg.size       = 0;
    g_otg.seq        = 0;
    g_otg.slot       = 0;
    g_otg.found      = 0;
    g_otg.last_rc    = 0;
    cfg_commit_clear(&g_otg.commit);
    g_otg.commit.deferred_logged = 0;

    if (out) {
        otg_init(out);              /* no file == an empty list, not garbage */
    }
    if (fs == 0 || write == 0 || out == 0) {
        return 0;
    }

    /* Only a read error is retried: ECORRUPT is structural (the bytes are
     * wrong, not late). The walk restarts from the top, so what the callback
     * captured on a failed pass is rewound first. */
    int rc = fat32_readdir(fs, fs->root_clus, otg_root_cb, 0);
    for (uint32_t attempt = 0;
         rc == FAT32_EIO && attempt < OTG_READ_RETRIES; attempt++) {
        otg_settle();
        g_otg.first_clus = 0;
        g_otg.size       = 0;
        rc = fat32_readdir(fs, fs->root_clus, otg_root_cb, 0);
    }
    if (rc != 0 || g_otg.first_clus == 0 || g_otg.size < OTG_STORE_MIN_BYTES) {
        g_otg.first_clus = 0;
        g_otg.size       = 0;
        return 0;                   /* absent, unreadable, or too small */
    }

    /* Provisionally enable so the resolver will run, then make that
     * conditional on EVERY run of BOTH slots actually resolving. */
    g_otg.fs    = fs;
    g_otg.found = 1;
    if (!slot_resolves(0) || !slot_resolves(1)) {
        g_otg.fs    = 0;
        g_otg.found = 0;
        return 0;
    }

    int      have       = 0;
    int      unreadable = 0;
    uint32_t best       = 0;
    uint8_t  best_i     = 0;

    for (uint32_t slot = 0; slot < OTG_SLOTS_ON_DISK; slot++) {
        if (read_slot(slot) != 0) {
            unreadable++;           /* still unreadable after the retries: NOT
                                     * "invalid" — we do not know what it holds */
            continue;
        }
        uint32_t count = 0, seq = 0;
        if (!slot_peek((const uint8_t *)g_slot_buf, &count, &seq)) {
            continue;               /* bad magic/version/count/CRC */
        }
        if (!have || seq_newer(seq, best)) {
            /* Pre-validated, so this decode cannot fail and cannot leave the
             * caller's list half-filled from a slot that loses. */
            (void)otg_slot_decode((const uint8_t *)g_slot_buf, out, &seq);
            have   = 1;
            best   = seq;
            best_i = (uint8_t)slot;
        }
    }

    g_otg.write = write;
    g_otg.wake  = wake;

    if (!have) {
        otg_init(out);
        if (unreadable) {
            /* No list we can trust AND a slot we could not read: it may hold
             * the newest record, and a save from seq 0 would write seq 1 into
             * the other slot — losing, on the next boot, to the one we never
             * saw. Fail closed; the boot log's "writable 0" says so. */
            g_otg.fs    = 0;
            g_otg.found = 0;
            g_otg.write = 0;
            g_otg.wake  = 0;
            return 0;
        }
        /* Usable file, no valid slot (fresh, or both damaged). Saving stays
         * ENABLED — writing slot 0 is exactly how it recovers — and `slot` is
         * set so the alternation below lands the first write in slot 0, which
         * is what tools/make_otg.py --verify tells the person watching the
         * bring-up to expect. */
        g_otg.slot = 1;
        return 0;
    }

    g_otg.seq  = best;
    g_otg.slot = best_i;
    return 1;
}

int otg_store_writable(void) { return g_otg.found ? 1 : 0; }
uint32_t otg_store_seq(void) { return g_otg.seq; }
int otg_store_last_rc(void)  { return g_otg.last_rc; }

/* ---- save --------------------------------------------------------------- */

int otg_store_save(const otg_list_t *l)
{
    if (l == 0) {
        return -1;
    }
    /* Hard gate: no file found at mount => this module does nothing, ever. */
    if (!g_otg.found || g_otg.fs == 0 || g_otg.write == 0) {
        return -1;
    }

    /* Alternate: never overwrite the slot we last read a good list from, so a
     * failed or torn write can only damage the copy nobody needs. */
    uint32_t slot = (g_otg.slot ^ 1u) & 1u;

    /* RE-RESOLVE every run BEFORE a byte is encoded, so a slot that cannot be
     * fully addressed is refused rather than half-written. */
    if (!slot_resolves(slot)) {
        return -2;
    }

    uint32_t seq = g_otg.seq + 1u;      /* wraps; config_seq_newer copes */
    otg_slot_encode((uint8_t *)g_slot_buf, l, seq);

    const uint8_t *src = (const uint8_t *)g_slot_buf;
    uint32_t off  = slot * OTG_SLOT_BYTES;
    uint32_t end  = off + OTG_SLOT_BYTES;
    uint32_t done = 0;
    while (off < end) {
        uint32_t lba = 0, sectors = 0;
        /* Re-resolved a second time, per run, immediately before the write —
         * nothing above this line is a cached address. */
        if (run_at(off, &lba, &sectors) != 0) {
            return -2;
        }
        uint32_t bytes = sectors * ATA_SECTOR_SZ;
        if (bytes > end - off) {
            bytes   = end - off;
            sectors = bytes / ATA_SECTOR_SZ;
        }
        if (sectors == 0 || (sectors % ATA_PHYS_LOG) != 0) {
            return -2;
        }
        /*
         * The injected write independently re-checks alignment and issues
         * FLUSH CACHE after the data — load-bearing, because the drive's
         * write cache is on by default and a battery pull would otherwise
         * silently lose the slot it just acknowledged.
         */
        int rc = g_otg.write(lba, sectors, src + done);
        if (rc != 0) {
            /* Leave seq/slot alone: the previous slot is still the newest
             * good one, and the next attempt targets THIS slot again rather
             * than eating the good one. */
            return rc;
        }
        off  += bytes;
        done += bytes;
    }

    g_otg.seq  = seq;
    g_otg.slot = (uint8_t)slot;
    return 0;
}

/* ---- the commit gate ---------------------------------------------------- */

void otg_store_touch(uint32_t now_us)
{
    cfg_commit_touch(&g_otg.commit, now_us);
}

void otg_store_commit_clear(void)
{
    cfg_commit_clear(&g_otg.commit);
    g_otg.commit.deferred_logged = 0;
}

int otg_store_pending(void)
{
    return g_otg.commit.dirty ? 1 : 0;
}

int otg_store_commit(int mode, const cfg_commit_env_t *env_in, const otg_list_t *l)
{
    if (env_in == 0 || l == 0) {
        return OTG_COMMIT_NONE;
    }

    cfg_commit_env_t env = *env_in;
    env.writable = otg_store_writable();   /* this module's own writability */

    int gate = cfg_commit_gate(&g_otg.commit, mode, &env);
    if (gate == CFG_GATE_DEFER_LOG) {
        return OTG_COMMIT_DEFERRED;
    }
    if (gate == CFG_GATE_DEFER_QUIET) {
        return OTG_COMMIT_DEFERRED_QUIET;
    }
    if (gate != CFG_GATE_WRITE && gate != CFG_GATE_WRITE_WAKE) {
        return OTG_COMMIT_NONE;
    }
    if (gate == CFG_GATE_WRITE_WAKE && g_otg.wake) {
        /* Pay the spin-up on the read path, which is built for a multi-second
         * wait, instead of inside the write's DRQ budget. */
        (void)g_otg.wake();
    }

    int rc = otg_store_save(l);
    g_otg.last_rc = rc;
    (void)cfg_commit_result(&g_otg.commit, rc, env_in->now_us);
    return rc == 0 ? OTG_COMMIT_WROTE : OTG_COMMIT_FAILED;
}
