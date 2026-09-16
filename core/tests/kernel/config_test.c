/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/kernel/config_test.c — host tests for settings persistence
 * (kernel/config.c), the ONLY code in the firmware that writes to the disk.
 *
 * Four layers, none of them touching a real drive:
 *
 *   1. LBA RESOLUTION (fat32_file_lba). The single point where a mistake
 *      becomes data loss. Run against an in-RAM FAT32 volume with
 *      BytesPerSector 2048 (sec_ratio 4) at a NON-ZERO partition base, which
 *      is the geometry that makes the two candidate formulas differ. The test
 *      pins the exact number and separately asserts it is NOT the
 *      sec_ratio-less value — the formula in the original design doc, which
 *      would have written into the FAT.
 *
 *   2. RECORD CODEC. Round-trip, and rejection of every way a record can be
 *      wrong: bad magic, version 0, version from the future, absurd length,
 *      a flipped payload byte, and — the one the doc's layout would have
 *      missed — a flipped HEADER byte, since the CRC covers the whole record
 *      and not just the payload. Plus range clamping of a CRC-valid record.
 *
 *   3. TWO-SLOT SELECTION via config_load() over that volume: newer seq wins,
 *      a torn/garbage slot is ignored in favour of the good one, sequence
 *      WRAPAROUND resolves the right way round, both-bad falls back to
 *      defaults, and a missing/too-small file disables writing entirely.
 *      Plus READ ERRORS, which are not the same thing as a bad slot: a
 *      failed read is retried, and a slot still unreadable with no good
 *      record to anchor on makes the module refuse to write for the session.
 *
 *   4. WRITE GRAMMAR. config_save() compiled against the recording mock bus
 *      (-DMMIO_MOCK), asserting the exact register sequence ata.c emits:
 *      WRITE SECTORS 0x30 with the resolved LBA in the task registers, two
 *      DRQ-out data phases of 256 halfwords each, then FLUSH CACHE 0xE7.
 *      Also that a refused save emits ZERO bus events, that consecutive
 *      saves ALTERNATE slots, and that a fresh (or both-bad) file takes its
 *      first write in slot 0, as the host tool tells the operator to expect.
 *
 * WHAT THIS CANNOT PROVE: that the drive does what the trace says. The
 * register grammar, the alignment and the addresses are checked; the physical
 * write is not. See the bring-up procedure at the top of kernel/config.c.
 */

#include <stdio.h>
#include <string.h>

#include "pp5022.h"
#include "ata.h"
#include "fat32.h"
#include "config.h"
#include "mmio_mock.h"
#include "trace_expect.h"

static int g_fails;

static void check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    if (!cond) {
        g_fails++;
    }
}

/* ---------------------------------------------------------------------------
 * In-RAM FAT32 volume.
 *
 * Geometry chosen so the LBA arithmetic is checkable by hand AND so the
 * sec_ratio factor is load-bearing:
 *
 *   BytesPerSec 2048  => sec_ratio 4   (the stock iPod 80 GB volume)
 *   SecPerClus  1     => one cluster is one FS-sector = 4 LBAs = 2048 B,
 *                        exactly CONFIG_MIN_BYTES
 *   RsvdSecCnt  1, NumFATs 1, FATSz32 1 => data_start = FS-sector 2,
 *                        so cluster N lives at FS-sector N
 *   part_lba    64    => nonzero, so a formula that forgot the base is caught
 *
 * Root is cluster 2 (FS-sector 2); CORECFG.DAT is cluster 3 (FS-sector 3).
 * Therefore CORECFG.DAT's first sector is at absolute LBA
 *     64 + 3 * 4 = 76
 * and the doc's sec_ratio-less formula would have said 64 + 3 = 67 — which on
 * a real volume lands inside the FAT.
 * ------------------------------------------------------------------------- */
#define IMG_BPS       2048u
#define IMG_PART_LBA  64u
#define IMG_FS_SECS   16u        /* FS-sectors in the volume */
#define IMG_TOT_LBA   (IMG_PART_LBA + IMG_FS_SECS * (IMG_BPS / 512u))

#define CFG_CLUS      3u
#define CFG_FS_SEC    3u
#define CFG_LBA0      (IMG_PART_LBA + CFG_FS_SEC * (IMG_BPS / 512u))   /* 76 */
#define CFG_LBA1      (CFG_LBA0 + CONFIG_SLOT_SECTORS)                 /* 78 */

static uint8_t g_disk[IMG_TOT_LBA * 512u];

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* One 32-byte 8.3 directory entry. `raw` is exactly 11 bytes, space-padded. */
static void put_dirent(uint8_t *e, const char *raw, uint8_t attr,
                       uint32_t clus, uint32_t size)
{
    memset(e, 0, 32);
    memcpy(e, raw, 11);
    e[11] = attr;
    put16(&e[20], (uint16_t)(clus >> 16));
    put16(&e[26], (uint16_t)(clus & 0xFFFFu));
    put32(&e[28], size);
}

/* Byte offset of FS-sector `s` inside g_disk (i.e. past the partition base). */
static uint8_t *fs_sec(uint32_t s)
{
    return &g_disk[(IMG_PART_LBA * 512u) + s * IMG_BPS];
}

/*
 * Build the volume. `cfg_size` is the size recorded in CORECFG.DAT's directory
 * entry (0 = omit the file entirely), which is how the "absent" and "too
 * small" cases are set up.
 */
static void build_image(uint32_t cfg_size)
{
    memset(g_disk, 0, sizeof g_disk);

    uint8_t *bs = fs_sec(0);
    bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90;
    memcpy(&bs[3], "MSDOS5.0", 8);
    put16(&bs[11], IMG_BPS);        /* BytesPerSec  */
    bs[13] = 1;                     /* SecPerClus   */
    put16(&bs[14], 1);              /* RsvdSecCnt   */
    bs[16] = 1;                     /* NumFATs      */
    /* bs[17] RootEntCnt = 0 and bs[22] FATSz16 = 0: this is FAT32.           */
    bs[21] = 0xF8;                  /* media        */
    put32(&bs[32], IMG_FS_SECS);    /* TotSec32 — gives a real total_clus     */
    put32(&bs[36], 1);              /* FATSz32      */
    put32(&bs[44], 2);              /* RootClus     */
    bs[510] = 0x55; bs[511] = 0xAA;

    /* FAT (FS-sector 1): every used cluster is a one-cluster chain. */
    uint8_t *fat = fs_sec(1);
    put32(&fat[0 * 4], 0x0FFFFFF8u);
    put32(&fat[1 * 4], 0x0FFFFFFFu);
    put32(&fat[2 * 4], 0x0FFFFFFFu);   /* root      (clus 2) */
    put32(&fat[3 * 4], 0x0FFFFFFFu);   /* CORECFG   (clus 3) */

    /* Root directory (cluster 2 == FS-sector 2). */
    uint8_t *root = fs_sec(2);
    if (cfg_size != 0) {
        put_dirent(&root[0], "CORECFG DAT", 0x20, CFG_CLUS, cfg_size);
    } else {
        put_dirent(&root[0], "OTHER   TXT", 0x20, CFG_CLUS, 4096);
    }
    /* root[32..] stays 0x00 => end of directory */
}

/*
 * Read-fault injection. Any read touching [g_fail_lba, g_fail_lba + g_fail_n)
 * fails while g_fail_left is nonzero — each failure consumes one, negative
 * means forever. This stages the boot failure config_load actually meets: the
 * drive still settling from spin-up, so the first read (or every read) of a
 * sector errors while the bytes underneath are perfectly good.
 */
#define NO_FAIL 0xFFFFFFFFu
static uint32_t g_fail_lba  = NO_FAIL;
static uint32_t g_fail_n    = 0;
static int      g_fail_left = 0;
static int      g_fail_hits;        /* how many reads the injector refused */

static void fail_sectors(uint32_t lba, uint32_t n, int times)  /* times < 0: forever */
{
    g_fail_lba  = lba;
    g_fail_n    = n;
    g_fail_left = times;
    g_fail_hits = 0;
}

static void fail_none(void)
{
    g_fail_lba  = NO_FAIL;
    g_fail_n    = 0;
    g_fail_left = 0;
}

/* 512-byte block callback over the RAM disk. Absolute LBAs, exactly like the
 * device's player_disk_read. */
static int mem_read(void *ud, uint32_t lba, uint32_t count, void *buf)
{
    (void)ud;
    if ((uint64_t)lba + count > IMG_TOT_LBA) {
        return -1;
    }
    if (g_fail_lba != NO_FAIL && g_fail_left != 0 &&
        lba < g_fail_lba + g_fail_n && g_fail_lba < lba + count) {
        if (g_fail_left > 0) {
            g_fail_left--;
        }
        g_fail_hits++;
        return -1;                  /* the injected fault */
    }
    memcpy(buf, &g_disk[(size_t)lba * 512u], (size_t)count * 512u);
    return 0;
}

/* Drop a record straight onto the RAM disk at an absolute LBA. */
static void place_record(uint32_t lba, const uint8_t *rec)
{
    memcpy(&g_disk[(size_t)lba * 512u], rec, CONFIG_SLOT_BYTES);
}

/* ---- settings helpers -------------------------------------------------- */

static void defaults(settings_t *s)
{
    memset(s, 0, sizeof *s);
    s->shuffle = 0;  s->repeat = REPEAT_OFF; s->volume = 70;
    s->volume_limit = 100;   /* settings_defaults()'s "no limit"; a zeroed  */
                             /* struct would encode as the 10% floor        */
    s->backlight_secs = 15; s->backlight_bright = 32;
}

static int settings_eq(const settings_t *a, const settings_t *b)
{
    return a->shuffle == b->shuffle && a->repeat == b->repeat &&
           a->resume_on_startup == b->resume_on_startup &&
           a->crossfade == b->crossfade && a->volume == b->volume &&
           a->bass == b->bass && a->treble == b->treble &&
           a->balance == b->balance &&
           a->backlight_secs == b->backlight_secs &&
           a->backlight_bright == b->backlight_bright &&
           a->theme == b->theme && a->clicker == b->clicker &&
           a->volume_limit == b->volume_limit && a->eq == b->eq &&
           a->resume_hash == b->resume_hash &&
           a->resume_secs == b->resume_secs &&
           a->resume_total == b->resume_total &&
           a->resume_kind == b->resume_kind &&
           a->resume_flags == b->resume_flags &&
           a->resume_qidx == b->resume_qidx &&
           a->resume_seed == b->resume_seed &&
           a->resume_order_seed == b->resume_order_seed &&
           a->resume_order_keep == b->resume_order_keep &&
           a->resume_ctx_hash == b->resume_ctx_hash;
    /* sleep_timer_min is deliberately NOT compared: it is runtime-only and
     * never rides the record (settings.h), so decode always writes 0 into it
     * whatever the encoded struct held. test_codec pins that directly. */
}

/* A settings_t with every field distinct from the defaults and at an extreme
 * of its range, so a codec that drops or truncates a field is caught. */
static void spicy(settings_t *s)
{
    s->shuffle = 1;  s->repeat = REPEAT_ONE; s->resume_on_startup = 1;
    s->crossfade = 1; s->volume = 100;
    s->bass = -12;   s->treble = 12;  s->balance = -100;
    s->backlight_secs = 60; s->backlight_bright = 1;
    s->theme = 3;    s->clicker = 2;
    s->resume_hash = 0xDEADBEEFu; s->resume_secs = 1234; s->resume_total = 5678;
    /* The queue context: the largest kind (a playlist), a queue index past
     * any album, a negative keep (PLAYER_KEEP_QUEUE), seeds and a context
     * hash with every byte distinct. The reserved words stay 0 — the codec
     * writes them as 0 whatever the struct holds, which test_resume_context
     * pins separately. */
    s->resume_kind = 6;  s->resume_flags = 0; s->resume_qidx = 5999;
    s->resume_seed = 0xC0FFEE01u; s->resume_order_seed = 0xBADC0DE5u;
    s->resume_order_keep = -2; s->resume_ctx_hash = 0x5EEDF00Du;
    /* The sound tail at its far end: the last preset this build knows, and a
     * limit that leaves the spicy volume (100) alone so the round trip is a
     * round trip. The limit-vs-volume coupling gets its own test. */
    s->volume_limit = 100; s->eq = 17;
}

/* ---- mock-bus programming ---------------------------------------------- */

/*
 * Status the drive reports throughout: BSY clear, RDY set, DRQ set, no
 * ERR/DF. That satisfies every poll in ata.c on the first read, which makes
 * the trace deterministic and lets the test assert it event for event.
 */
#define ST_OK  (ATA_STATUS_RDY | ATA_STATUS_DRQ)

static void arm_drive_ready(void)
{
    mmio_mock_reset();
    mmio_mock_set_read(ATA_ALT_STATUS_ADDR, ST_OK);
    mmio_mock_set_read(ATA_COMMAND_ADDR,    ST_OK);
    mmio_mock_set_read(USEC_TIMER_ADDR,     0);   /* time never advances */
}

/* ---- 1. LBA resolution ------------------------------------------------- */

static void test_file_lba(void)
{
    build_image(CONFIG_MIN_BYTES);

    fat32_t fs;
    check("mount", fat32_mount(&fs, mem_read, 0, IMG_PART_LBA) == 0);
    check("mount/sec_ratio", fs.sec_ratio == IMG_BPS / 512u);

    uint32_t clus = 0, size = 0;
    check("open CORECFG.DAT",
          fat32_open(&fs, "CORECFG.DAT", &clus, &size) == 0 &&
          clus == CFG_CLUS && size == CONFIG_MIN_BYTES);

    uint32_t lba = 0, run = 0;
    check("file_lba ok", fat32_file_lba(&fs, CFG_CLUS, &lba, &run) == 0);

    /* THE assertion this whole file exists for. */
    check("file_lba == part_lba + fs_sec*sec_ratio", lba == CFG_LBA0);
    check("file_lba is NOT the sec_ratio-less value (would hit the FAT)",
          lba != IMG_PART_LBA + CFG_FS_SEC);
    check("file_lba run == one cluster in 512B sectors",
          run == (IMG_BPS / 512u) * 1u && run >= CONFIG_SECTORS);

    /* Fail-closed behaviour: an error must zero BOTH out-params, so a caller
     * that ignores the return code has no address to write to. */
    lba = 0xDEADBEEFu; run = 0xDEADBEEFu;
    check("file_lba rejects cluster 0",
          fat32_file_lba(&fs, 0, &lba, &run) != 0 && lba == 0 && run == 0);
    lba = 0xDEADBEEFu; run = 0xDEADBEEFu;
    check("file_lba rejects cluster 1",
          fat32_file_lba(&fs, 1, &lba, &run) != 0 && lba == 0 && run == 0);
    lba = 0xDEADBEEFu; run = 0xDEADBEEFu;
    check("file_lba rejects a cluster past the volume",
          fat32_file_lba(&fs, 0x0FFFFFF0u, &lba, &run) != 0 &&
          lba == 0 && run == 0);
    check("file_lba rejects null args",
          fat32_file_lba(&fs, CFG_CLUS, 0, &run) != 0 &&
          fat32_file_lba(0, CFG_CLUS, &lba, &run) != 0);

    /* A zeroed fat32_t is the "somebody passed an unmounted struct" case: the
     * arithmetic is perfectly happy with it and yields LBA 0. Must refuse. */
    fat32_t zero;
    memset(&zero, 0, sizeof zero);
    lba = 0xDEADBEEFu;
    check("file_lba rejects an unmounted fs",
          fat32_file_lba(&zero, 2, &lba, &run) != 0 && lba == 0);
}

/* ---- 2. record codec --------------------------------------------------- */

static void test_codec(void)
{
    uint8_t rec[CONFIG_SLOT_BYTES];
    settings_t in, out;
    uint32_t seq = 0;

    defaults(&in);
    spicy(&in);
    config_encode(rec, &in, 0x12345678u);
    memset(&out, 0xA5, sizeof out);
    check("codec round-trip",
          config_decode(rec, &out, &seq) == 1 && seq == 0x12345678u &&
          settings_eq(&in, &out));

    /*
     * THE SLEEP TIMER IS NOT ON DISK. An armed timer means nothing after a
     * power cut, so settings_t.sleep_timer_min is runtime-only: encode must
     * ignore it (two records differing only in it are byte-identical, which
     * is what keeps arming the timer off the user's disk), and decode must
     * WRITE 0 into it — config_load() copies the whole decoded struct over
     * the caller's record, so a field decode skips arrives holding stack
     * garbage and would arm an arbitrary timer at boot.
     */
    {
        uint8_t rec_off[CONFIG_SLOT_BYTES], rec_on[CONFIG_SLOT_BYTES];
        settings_t s2 = in;

        s2.sleep_timer_min = 0;
        config_encode(rec_off, &s2, 0x0BADF00Du);
        s2.sleep_timer_min = 90;
        config_encode(rec_on, &s2, 0x0BADF00Du);
        check("codec: the sleep timer never reaches the record",
              memcmp(rec_off, rec_on, sizeof rec_off) == 0);

        memset(&out, 0x5A, sizeof out);      /* the `cand = tmp` hazard */
        check("codec: decode zeroes the sleep timer over a dirty struct",
              config_decode(rec_on, &out, &seq) == 1 &&
              out.sleep_timer_min == 0);
    }

    /* Bad magic. */
    config_encode(rec, &in, 1);
    rec[0] ^= 0xFF;
    check("codec rejects bad magic", config_decode(rec, &out, &seq) == 0);

    /* Version 0 and a version from the future. Both must be refused rather
     * than half-interpreted — and note the CRC is recomputed for the future
     * case, so this proves the VERSION check bites, not the CRC. */
    config_encode(rec, &in, 1);
    rec[4] = 0; rec[5] = 0;
    check("codec rejects version 0", config_decode(rec, &out, &seq) == 0);

    config_encode(rec, &in, 1);
    rec[4] = (uint8_t)(CONFIG_VERSION + 1u);
    check("codec rejects a future version",
          config_decode(rec, &out, &seq) == 0);

    /* Absurd declared length. */
    config_encode(rec, &in, 1);
    rec[6] = 0; rec[7] = 0;
    check("codec rejects length 0", config_decode(rec, &out, &seq) == 0);
    config_encode(rec, &in, 1);
    rec[6] = 0xFF; rec[7] = 0xFF;
    check("codec rejects an oversized length",
          config_decode(rec, &out, &seq) == 0);

    /* A flipped PAYLOAD bit. */
    config_encode(rec, &in, 1);
    rec[CONFIG_SLOT_BYTES / 2] ^= 0x01;
    check("codec rejects a flipped payload bit",
          config_decode(rec, &out, &seq) == 0);

    /* A flipped HEADER bit — specifically inside `seq`, which is what decides
     * which slot wins. The design doc's CRC covered only the payload, so this
     * exact corruption would have been accepted and could have let a stale or
     * garbage record beat a good one. */
    config_encode(rec, &in, 1);
    rec[8] ^= 0x01;
    check("codec rejects a flipped seq bit (CRC covers the header)",
          config_decode(rec, &out, &seq) == 0);

    /* A flipped bit in the ZERO PADDING, which the CRC also covers. */
    config_encode(rec, &in, 1);
    rec[CONFIG_SLOT_BYTES - 8] ^= 0x01;
    check("codec rejects a flipped padding bit",
          config_decode(rec, &out, &seq) == 0);

    /* An all-zero slot (a never-written or erased sector) must not decode. */
    memset(rec, 0, sizeof rec);
    check("codec rejects an all-zero slot",
          config_decode(rec, &out, &seq) == 0);

    /* Garbage. */
    for (unsigned i = 0; i < sizeof rec; i++) {
        rec[i] = (uint8_t)(i * 7u + 3u);
    }
    check("codec rejects garbage", config_decode(rec, &out, &seq) == 0);

    /*
     * A CRC-VALID record carrying out-of-range values. Encode clamps, so to
     * build one we have to poke the payload and re-CRC it — which is exactly
     * what an older/newer build or a buggy host tool would produce. Decode
     * must clamp rather than hand the hardware a brightness of 255.
     */
    config_encode(rec, &in, 1);
    rec[12 + 4] = 200;    /* volume            */
    rec[12 + 9] = 255;    /* backlight_bright  */
    rec[12 + 5] = 0x80;   /* bass = -128       */
    rec[12 + 10] = 99;    /* theme — no such id: must land on Linen (0) */
    rec[12 + 8] = 7;      /* backlight_secs — not a legal step */
    {
        /* Recompute the CRC the same way config.c does, so this record is
         * genuinely valid and only its VALUES are wrong. */
        uint32_t crc = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < CONFIG_SLOT_BYTES - 4u; i++) {
            crc ^= rec[i];
            for (int b = 0; b < 8; b++) {
                uint32_t m = (uint32_t)0u - (crc & 1u);
                crc = (crc >> 1) ^ (0xEDB88320u & m);
            }
        }
        crc = ~crc;
        rec[CONFIG_SLOT_BYTES - 4] = (uint8_t)crc;
        rec[CONFIG_SLOT_BYTES - 3] = (uint8_t)(crc >> 8);
        rec[CONFIG_SLOT_BYTES - 2] = (uint8_t)(crc >> 16);
        rec[CONFIG_SLOT_BYTES - 1] = (uint8_t)(crc >> 24);
    }
    check("codec clamps an in-range-CRC but out-of-range record",
          config_decode(rec, &out, &seq) == 1 &&
          out.volume == 100 && out.backlight_bright == 32 &&
          out.bass == -12 && out.theme == 0 &&
          out.backlight_secs == 5);
}

/* ---- 2b. the v2 resume tail, and v1 compatibility ---------------------- */

/*
 * Re-stamp a record's CRC after poking its bytes, so what we hand the decoder
 * is genuinely valid and only its CONTENT is under test. Spelled out here
 * rather than imported from config.c for the same reason the write trace
 * spells out its opcodes: a test that reuses the implementation's CRC agrees
 * with whatever the implementation happens to compute.
 */
static void recrc(uint8_t *rec)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < CONFIG_SLOT_BYTES - 4u; i++) {
        crc ^= rec[i];
        for (int b = 0; b < 8; b++) {
            uint32_t m = (uint32_t)0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & m);
        }
    }
    crc = ~crc;
    rec[CONFIG_SLOT_BYTES - 4] = (uint8_t)crc;
    rec[CONFIG_SLOT_BYTES - 3] = (uint8_t)(crc >> 8);
    rec[CONFIG_SLOT_BYTES - 2] = (uint8_t)(crc >> 16);
    rec[CONFIG_SLOT_BYTES - 1] = (uint8_t)(crc >> 24);
}

/* Offsets are restated as literals, not imported: this file is the
 * independent statement of the on-disk contract that config.c has to meet. */
#define T_OFF_VERSION 4u
#define T_OFF_LENGTH  6u
#define T_OFF_SEQ     8u
#define T_OFF_PAYLOAD 12u
#define T_LEN_V1      12u          /* settings only                        */
#define T_LEN_V2      24u          /* + resume hash/secs/total, 4 bytes ea. */
#define T_LEN_V2Q     44u          /* + the resume queue context (same ver) */
#define T_LEN_V2S     48u          /* + the sound tail (limit, EQ; same ver) */
#define T_OFF_RES     (T_OFF_PAYLOAD + T_LEN_V1)   /* 24: resume_hash      */
#define T_OFF_CTX     (T_OFF_PAYLOAD + T_LEN_V2)   /* 36: resume_kind      */
#define T_OFF_SND     (T_OFF_PAYLOAD + T_LEN_V2Q)  /* 56: volume_limit     */
#define T_RESUME_MAX  86400u       /* the decoder's ceiling on both counts  */

static void put32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * The resume locator is the only field in this record that can make the device
 * DO something on its own at boot, so every way it can be wrong is a way to
 * wake up playing the wrong thing. The tests below pin, in order:
 *
 *   - the record this build writes really is v2 with a v2 length;
 *   - the V1 RECORD SITTING ON THE USER'S IPOD RIGHT NOW still decodes, and
 *     decodes to "no resume" rather than reading three 32-bit fields out of
 *     the zero padding. This is the live-format constraint, and it is the
 *     stale-resume fallback for every already-deployed device;
 *   - `length`, not `version`, gates the tail;
 *   - hash 0 means no resume, enforced on both sides of the codec;
 *   - a CRC-valid but insane position is clamped, not forwarded to the seek.
 */
static void test_resume_record(void)
{
    uint8_t rec[CONFIG_SLOT_BYTES];
    settings_t in, out;
    uint32_t seq = 0;

    /* What this build emits. */
    defaults(&in);
    spicy(&in);
    config_encode(rec, &in, 7);
    check("record is version 2", rec[T_OFF_VERSION] == 2 &&
                                 rec[T_OFF_VERSION + 1] == 0);
    check("record declares the v2 payload length, sound tail included",
          rec[T_OFF_LENGTH] == T_LEN_V2S && rec[T_OFF_LENGTH + 1] == 0);
    check("resume fields land at the documented offsets",
          get32le(&rec[T_OFF_RES])     == 0xDEADBEEFu &&
          get32le(&rec[T_OFF_RES + 4]) == 1234u &&
          get32le(&rec[T_OFF_RES + 8]) == 5678u);

    /* Round-trip through decode (settings_eq covers all three fields). */
    memset(&out, 0xA5, sizeof out);
    check("resume round-trips",
          config_decode(rec, &out, &seq) == 1 && settings_eq(&in, &out));

    /*
     * A REAL V1 RECORD, built by hand exactly as the shipped v1 encoder did:
     * version 1, length 12, nothing after the settings bytes. This is the
     * record already on the device, and mis-parsing it is the one failure the
     * version bump exists to prevent.
     */
    {
        settings_t v1;
        defaults(&v1);
        v1.volume = 42; v1.theme = 1; v1.clicker = 3;
        config_encode(rec, &v1, 99);
        rec[T_OFF_VERSION] = 1;
        put32le(&rec[T_OFF_RES],     0);      /* v1 wrote only zero padding */
        put32le(&rec[T_OFF_RES + 4], 0);
        put32le(&rec[T_OFF_RES + 8], 0);
        rec[T_OFF_LENGTH] = (uint8_t)T_LEN_V1;
        recrc(rec);

        memset(&out, 0x5A, sizeof out);       /* caller's struct is garbage */
        check("v1 record still decodes under a v2 build",
              config_decode(rec, &out, &seq) == 1 && seq == 99u &&
              out.volume == 42 && out.theme == 1 && out.clicker == 3);
        check("v1 record decodes to NO resume",
              out.resume_hash == 0 && out.resume_secs == 0 &&
              out.resume_total == 0);
    }

    /*
     * length, not version, gates the tail: a record that says v2 but declares
     * the v1 length must NOT have its resume read out of the bytes that
     * follow — those are padding as far as `length` is concerned, and
     * believing them is how a truncated write becomes a seek target.
     */
    config_encode(rec, &in, 5);               /* real resume bytes present */
    rec[T_OFF_LENGTH] = (uint8_t)T_LEN_V1;
    recrc(rec);
    memset(&out, 0x5A, sizeof out);
    check("a v1 length suppresses the resume tail even at version 2",
          config_decode(rec, &out, &seq) == 1 &&
          out.resume_hash == 0 && out.resume_secs == 0 &&
          out.resume_total == 0);

    /* A length between v1 and v2 is still short of the tail. */
    config_encode(rec, &in, 5);
    rec[T_OFF_LENGTH] = (uint8_t)(T_LEN_V2 - 1u);
    recrc(rec);
    check("a length one byte short of v2 suppresses the tail",
          config_decode(rec, &out, &seq) == 1 && out.resume_hash == 0);

    /* Hash 0 = nothing to resume. ENCODE must drop the position with it, so a
     * record can never carry a position belonging to no track. */
    defaults(&in);
    in.resume_hash = 0; in.resume_secs = 900; in.resume_total = 1000;
    config_encode(rec, &in, 3);
    check("encode drops a position with no track",
          get32le(&rec[T_OFF_RES + 4]) == 0 &&
          get32le(&rec[T_OFF_RES + 8]) == 0);

    /* …and DECODE re-asserts it, for a hand-edited or third-party record. */
    defaults(&in);
    in.resume_hash = 1; in.resume_secs = 900; in.resume_total = 1000;
    config_encode(rec, &in, 3);
    put32le(&rec[T_OFF_RES], 0);              /* hash cleared, position left */
    recrc(rec);
    check("decode drops a position with no track",
          config_decode(rec, &out, &seq) == 1 && out.resume_hash == 0 &&
          out.resume_secs == 0 && out.resume_total == 0);

    /* An absurd position in an otherwise valid record is clamped, not passed
     * through. player_seek_to clamps to the real track length too; this is the
     * layer that stops a garbage record getting that far. */
    defaults(&in);
    in.resume_hash = 0x11223344u;
    config_encode(rec, &in, 4);
    put32le(&rec[T_OFF_RES + 4], 0xFFFFFFFFu);
    put32le(&rec[T_OFF_RES + 8], 0xFFFFFFFFu);
    recrc(rec);
    check("decode clamps an insane resume position",
          config_decode(rec, &out, &seq) == 1 &&
          out.resume_secs == T_RESUME_MAX && out.resume_total == T_RESUME_MAX);

    /* Encode clamps the same way, so a runaway elapsed counter cannot be
     * written in the first place. */
    defaults(&in);
    in.resume_hash = 0x11223344u;
    in.resume_secs = 0xFFFFFFFFu; in.resume_total = 0xFFFFFFFFu;
    config_encode(rec, &in, 4);
    check("encode clamps an insane resume position",
          get32le(&rec[T_OFF_RES + 4]) == T_RESUME_MAX &&
          get32le(&rec[T_OFF_RES + 8]) == T_RESUME_MAX);

    /* A corrupt resume tail must take the WHOLE record down, not silently
     * yield a half-trusted locator — the CRC covers the payload, so this is
     * really a check that the tail is inside the covered range. */
    config_encode(rec, &in, 4);
    rec[T_OFF_RES + 2] ^= 0x01;
    check("a flipped resume byte fails the CRC",
          config_decode(rec, &out, &seq) == 0);
}

/*
 * The resume QUEUE CONTEXT: appended after the locator under the SAME record
 * version, with `length` 24 -> 44. What it must get right:
 *
 *   - the 24-byte record every device in the field holds right now (written
 *     by the build before this one) still decodes, and decodes to kind NONE
 *     — the album fallback, which is exactly what that build did;
 *   - `length` gates the context on its own, independently of the locator:
 *     a record one byte short of 44 keeps its locator and drops its context;
 *   - no track, no context — on both sides, like the position;
 *   - a kind from a future build is declined (NONE), never acted on;
 *   - the reserved words are written as 0 whatever the struct held;
 *   - the negative keep values the player uses survive the round trip.
 */
static void test_resume_context(void)
{
    uint8_t rec[CONFIG_SLOT_BYTES];
    settings_t in, out;
    uint32_t seq = 0;

    defaults(&in);
    spicy(&in);
    config_encode(rec, &in, 11);
    check("context fields land at the documented offsets",
          rec[T_OFF_CTX] == 6 && rec[T_OFF_CTX + 1] == 0 &&
          rec[T_OFF_CTX + 2] == (5999 & 0xFF) && rec[T_OFF_CTX + 3] == (5999 >> 8) &&
          get32le(&rec[T_OFF_CTX + 4])  == 0xC0FFEE01u &&
          get32le(&rec[T_OFF_CTX + 8])  == 0xBADC0DE5u &&
          get32le(&rec[T_OFF_CTX + 12]) == 0x5EEDF00Du &&
          rec[T_OFF_CTX + 16] == 0xFE && rec[T_OFF_CTX + 17] == 0xFF &&
          rec[T_OFF_CTX + 18] == 0 && rec[T_OFF_CTX + 19] == 0);
    memset(&out, 0xA5, sizeof out);
    check("context round-trips (negative keep included)",
          config_decode(rec, &out, &seq) == 1 && settings_eq(&in, &out) &&
          out.resume_order_keep == -2);

    /* The record the PREVIOUS build wrote: version 2, length 24, zero
     * padding where the context now lives. Built exactly as that encoder
     * did, and it is the record on every deployed device. */
    config_encode(rec, &in, 12);
    rec[T_OFF_LENGTH] = (uint8_t)T_LEN_V2;
    memset(&rec[T_OFF_CTX], 0, T_LEN_V2Q - T_LEN_V2);
    recrc(rec);
    memset(&out, 0x5A, sizeof out);
    check("a 24-byte v2 record still decodes under this build",
          config_decode(rec, &out, &seq) == 1 && seq == 12u &&
          out.resume_hash == 0xDEADBEEFu && out.resume_secs == 1234u);
    check("...with kind NONE and an empty context, not the padding",
          out.resume_kind == 0 && out.resume_flags == 0 &&
          out.resume_qidx == 0 && out.resume_seed == 0 &&
          out.resume_order_seed == 0 && out.resume_order_keep == 0 &&
          out.resume_ctx_hash == 0);

    /* One byte short of the context: real context bytes are present in the
     * record but `length` says they are padding, and padding they are. */
    config_encode(rec, &in, 13);
    rec[T_OFF_LENGTH] = (uint8_t)(T_LEN_V2Q - 1u);
    recrc(rec);
    memset(&out, 0x5A, sizeof out);
    check("a length one byte short of the context keeps the locator",
          config_decode(rec, &out, &seq) == 1 &&
          out.resume_hash == 0xDEADBEEFu && out.resume_total == 5678u);
    check("...and drops the context",
          out.resume_kind == 0 && out.resume_seed == 0 &&
          out.resume_order_seed == 0 && out.resume_qidx == 0);

    /* No track => no context, on encode... */
    defaults(&in);
    spicy(&in);
    in.resume_hash = 0;
    config_encode(rec, &in, 14);
    check("encode drops the context with the locator",
          rec[T_OFF_CTX] == 0 && get32le(&rec[T_OFF_CTX + 4]) == 0 &&
          get32le(&rec[T_OFF_CTX + 8]) == 0 &&
          rec[T_OFF_CTX + 16] == 0 && rec[T_OFF_CTX + 17] == 0);

    /* ...and on decode, against a hand-edited record. */
    defaults(&in);
    spicy(&in);
    config_encode(rec, &in, 15);
    put32le(&rec[T_OFF_RES], 0);
    recrc(rec);
    memset(&out, 0x5A, sizeof out);
    check("decode drops the context with the locator",
          config_decode(rec, &out, &seq) == 1 && out.resume_hash == 0 &&
          out.resume_kind == 0 && out.resume_qidx == 0 &&
          out.resume_seed == 0 && out.resume_order_seed == 0 &&
          out.resume_order_keep == 0);

    /* A kind this build has no builder for: read as NONE (album fallback),
     * with the rest of the context still there for a build that does. */
    config_encode(rec, &in, 16);
    rec[T_OFF_CTX] = 200;
    recrc(rec);
    check("an unknown kind decodes as NONE",
          config_decode(rec, &out, &seq) == 1 && out.resume_kind == 0 &&
          out.resume_seed == 0xC0FFEE01u);

    /* Encode clamps an out-of-range kind the same way, so a stray value in
     * RAM never reaches the disk as a number a future reader might act on. */
    in.resume_kind = 200;
    config_encode(rec, &in, 17);
    check("encode writes an out-of-range kind as NONE", rec[T_OFF_CTX] == 0);

    /* Reserved words: always 0 on disk, always 0 in RAM after a decode. The
     * context hash beside them is real — a playlist's name hash — and goes
     * through verbatim, gated on the locator like the rest of the context. */
    defaults(&in);
    spicy(&in);
    in.resume_flags = 0xAB; in.resume_ctx_hash = 0x12345678u;
    config_encode(rec, &in, 18);
    check("reserved context words are written as 0, the context hash as is",
          rec[T_OFF_CTX + 1] == 0 && get32le(&rec[T_OFF_CTX + 12]) == 0x12345678u &&
          rec[T_OFF_CTX + 18] == 0 && rec[T_OFF_CTX + 19] == 0);
    rec[T_OFF_CTX + 1] = 0xCD;
    put32le(&rec[T_OFF_CTX + 12], 0xFEEDFACEu);
    recrc(rec);
    check("reserved words read back as 0, the context hash as written",
          config_decode(rec, &out, &seq) == 1 &&
          out.resume_flags == 0 && out.resume_ctx_hash == 0xFEEDFACEu);

    /* No locator, no context: the hash is not written, and a stray one in
     * a hand-edited record is not read. */
    in.resume_hash = 0;
    config_encode(rec, &in, 19);
    check("no track: the context hash is written as 0",
          get32le(&rec[T_OFF_CTX + 12]) == 0);
    put32le(&rec[T_OFF_CTX + 12], 0xFEEDFACEu);
    recrc(rec);
    check("no track: a context hash on disk reads back as 0",
          config_decode(rec, &out, &seq) == 1 && out.resume_ctx_hash == 0);
}

/* ---- seq ordering ------------------------------------------------------ */

/*
 * The SOUND TAIL: the Volume Limit and the EQ preset, appended after the
 * queue context under the SAME record version, `length` 44 -> 48. What it
 * must get right:
 *
 *   - the 44-byte record every device in the field holds right now still
 *     decodes, and decodes to "no limit, EQ off" with its VOLUME UNTOUCHED —
 *     the state those devices are already in;
 *   - a volume_limit byte of 0 means UNSET, not 10%: a 48-byte record whose
 *     new bytes were never written must not pin the user at the floor;
 *   - both fields are clamped/declined on the way in AND on the way out;
 *   - decode enforces the invariant the two Sound rows share, volume <=
 *     volume_limit, whatever the bytes say.
 */
static void test_sound_tail(void)
{
    uint8_t rec[CONFIG_SLOT_BYTES];
    settings_t in, out;
    uint32_t seq = 0;

    defaults(&in);
    spicy(&in);
    config_encode(rec, &in, 21);
    check("sound tail lands at the documented offsets",
          rec[T_OFF_SND] == 100 && rec[T_OFF_SND + 1] == 17 &&
          rec[T_OFF_SND + 2] == 0 && rec[T_OFF_SND + 3] == 0);
    memset(&out, 0xA5, sizeof out);
    check("sound tail round-trips",
          config_decode(rec, &out, &seq) == 1 && settings_eq(&in, &out) &&
          out.volume_limit == 100 && out.eq == 17);

    /* The record the PREVIOUS build wrote: version 2, length 44, zero padding
     * where the sound tail now lives. This is the live-format constraint —
     * it is the record on every deployed device. */
    defaults(&in);
    in.volume = 90;
    config_encode(rec, &in, 22);
    rec[T_OFF_LENGTH] = (uint8_t)T_LEN_V2Q;
    memset(&rec[T_OFF_SND], 0, T_LEN_V2S - T_LEN_V2Q);
    recrc(rec);
    memset(&out, 0x5A, sizeof out);
    check("a 44-byte v2 record still decodes under this build",
          config_decode(rec, &out, &seq) == 1 && seq == 22u && out.volume == 90);
    check("...as no limit and EQ off, with the volume untouched",
          out.volume_limit == 100 && out.eq == 0 && out.volume == 90);

    /* One byte short of the tail: the bytes are there, `length` says padding. */
    defaults(&in);
    in.volume_limit = 40; in.eq = 3;
    config_encode(rec, &in, 23);
    rec[T_OFF_LENGTH] = (uint8_t)(T_LEN_V2S - 1u);
    recrc(rec);
    memset(&out, 0x5A, sizeof out);
    check("a length one byte short of the sound tail drops it",
          config_decode(rec, &out, &seq) == 1 &&
          out.volume_limit == 100 && out.eq == 0);

    /* A limit below the volume pulls the volume down on the way in, so the
     * device can never come up louder than the ceiling the user set. */
    defaults(&in);
    in.volume = 90; in.volume_limit = 60;
    config_encode(rec, &in, 24);
    check("encode writes the limit verbatim", rec[T_OFF_SND] == 60);
    memset(&out, 0x5A, sizeof out);
    check("decode clamps the volume to the limit",
          config_decode(rec, &out, &seq) == 1 &&
          out.volume_limit == 60 && out.volume == 60);

    /* The limit byte's edges, hand-edited into an otherwise valid record. */
    defaults(&in);
    config_encode(rec, &in, 25);
    rec[T_OFF_SND] = 0;                    /* unset */
    recrc(rec);
    check("limit byte 0 reads as no limit",
          config_decode(rec, &out, &seq) == 1 && out.volume_limit == 100);

    config_encode(rec, &in, 26);
    rec[T_OFF_SND] = 5;                    /* below the floor */
    recrc(rec);
    check("limit byte 5 clamps up to the 10% floor",
          config_decode(rec, &out, &seq) == 1 && out.volume_limit == 10 &&
          out.volume == 10);

    config_encode(rec, &in, 27);
    rec[T_OFF_SND] = 250;                  /* above the ceiling */
    recrc(rec);
    check("limit byte 250 clamps down to 100",
          config_decode(rec, &out, &seq) == 1 && out.volume_limit == 100);

    /* Encode clamps the same way, so a stray value in RAM never reaches the
     * disk as a limit outside the field's range. */
    defaults(&in);
    in.volume_limit = 250;
    config_encode(rec, &in, 28);
    check("encode clamps a limit above the ceiling", rec[T_OFF_SND] == 100);
    in.volume_limit = 1;
    config_encode(rec, &in, 29);
    check("encode clamps a limit below the floor", rec[T_OFF_SND] == 10);

    /* The EQ byte: the top preset this build knows survives, anything past it
     * is Off — the theme byte's rule, for the same reason (declining to
     * interpret beats acting on a curve we have no table for). */
    defaults(&in);
    config_encode(rec, &in, 30);
    rec[T_OFF_SND + 1] = 17;
    recrc(rec);
    check("EQ byte 17 is the last preset this build knows",
          config_decode(rec, &out, &seq) == 1 && out.eq == 17);

    config_encode(rec, &in, 31);
    rec[T_OFF_SND + 1] = 18;
    recrc(rec);
    check("EQ byte 18 decodes as Off",
          config_decode(rec, &out, &seq) == 1 && out.eq == 0);

    config_encode(rec, &in, 32);
    rec[T_OFF_SND + 1] = 200;
    recrc(rec);
    check("EQ byte 200 decodes as Off",
          config_decode(rec, &out, &seq) == 1 && out.eq == 0);

    defaults(&in);
    in.eq = 200;
    config_encode(rec, &in, 33);
    check("encode writes an unknown preset as Off", rec[T_OFF_SND + 1] == 0);

    /* The tail is inside the CRC's range, like every other field. */
    defaults(&in);
    in.volume_limit = 40;
    config_encode(rec, &in, 34);
    rec[T_OFF_SND] ^= 0x01;
    check("a flipped sound-tail byte fails the CRC",
          config_decode(rec, &out, &seq) == 0);
}

static void test_seq_order(void)
{
    check("seq 2 newer than 1",       config_seq_newer(2, 1) == 1);
    check("seq 1 not newer than 2",   config_seq_newer(1, 2) == 0);
    check("seq equal is not newer",   config_seq_newer(7, 7) == 0);
    check("seq wraps: 0 newer than 0xFFFFFFFF",
          config_seq_newer(0, 0xFFFFFFFFu) == 1);
    check("seq wraps: 0xFFFFFFFF not newer than 0",
          config_seq_newer(0xFFFFFFFFu, 0) == 0);
    check("seq wraps: 3 newer than 0xFFFFFFFE",
          config_seq_newer(3, 0xFFFFFFFEu) == 1);
}

/* ---- 3. two-slot selection --------------------------------------------- */

/* Set up the volume with the given records in slot 0 / slot 1 (null = leave
 * the sector zeroed), mount, and run config_load. */
static int load_with(const uint8_t *s0, const uint8_t *s1,
                     fat32_t *fs, settings_t *s)
{
    build_image(CONFIG_MIN_BYTES);
    if (s0) place_record(CFG_LBA0, s0);
    if (s1) place_record(CFG_LBA1, s1);
    if (fat32_mount(fs, mem_read, 0, IMG_PART_LBA) != 0) {
        return -1;
    }
    defaults(s);
    return config_load(fs, s);
}

static void test_two_slot(void)
{
    uint8_t a[CONFIG_SLOT_BYTES], b[CONFIG_SLOT_BYTES];
    settings_t want_a, want_b, got;
    fat32_t fs;

    defaults(&want_a); want_a.volume = 11; want_a.theme = 1;
    defaults(&want_b); spicy(&want_b);

    /* Both valid, slot 1 newer. */
    config_encode(a, &want_a, 5);
    config_encode(b, &want_b, 6);
    check("load picks the higher seq (slot 1)",
          load_with(a, b, &fs, &got) == 1 && settings_eq(&got, &want_b));

    /* Both valid, slot 0 newer. */
    config_encode(a, &want_a, 9);
    config_encode(b, &want_b, 8);
    check("load picks the higher seq (slot 0)",
          load_with(a, b, &fs, &got) == 1 && settings_eq(&got, &want_a));

    /*
     * Slot 1 TORN, modelled exactly as the hardware would tear it: the drive
     * committed the FIRST of the two logical sectors of the new record and
     * lost power before the second, so slot 1 now holds the new header (magic
     * fine, seq 6 — NEWER than the good slot) glued to the previous record's
     * tail, including its CRC. This is the failure the two-slot scheme exists
     * for, and the one a naive "highest seq wins" loader gets wrong.
     */
    {
        uint8_t prev[CONFIG_SLOT_BYTES];
        config_encode(a, &want_a, 5);       /* slot 0: the good record       */
        config_encode(b, &want_b, 6);       /* slot 1: the write in progress */
        config_encode(prev, &want_a, 4);    /* what slot 1 held before it    */
        memcpy(&b[512], &prev[512], 512);   /* …second sector never landed   */
    }
    check("load ignores a torn newer slot and takes the older good one",
          load_with(a, b, &fs, &got) == 1 && settings_eq(&got, &want_a));

    /* Slot 0 never written (all zeros), slot 1 good. */
    config_encode(b, &want_b, 1);
    check("load tolerates a virgin slot 0",
          load_with(0, b, &fs, &got) == 1 && settings_eq(&got, &want_b));

    /* Slot 1 pure garbage, slot 0 good. */
    config_encode(a, &want_a, 4);
    for (unsigned i = 0; i < sizeof b; i++) b[i] = (uint8_t)(i ^ 0x5Au);
    check("load ignores a garbage slot",
          load_with(a, b, &fs, &got) == 1 && settings_eq(&got, &want_a));

    /* WRAPAROUND: slot 0 at 0xFFFFFFFF, slot 1 at 0 — slot 1 is the newer
     * one. A naive `a > b` comparison picks slot 0 and silently reverts the
     * user's settings by one save, forever. */
    config_encode(a, &want_a, 0xFFFFFFFFu);
    config_encode(b, &want_b, 0u);
    check("load resolves seq wraparound in favour of the wrapped slot",
          load_with(a, b, &fs, &got) == 1 && settings_eq(&got, &want_b));

    /* Both slots bad: defaults survive untouched, but the file is still
     * writable — writing slot 0 is how we recover. */
    for (unsigned i = 0; i < sizeof a; i++) a[i] = 0xFF;
    for (unsigned i = 0; i < sizeof b; i++) b[i] = 0x5A;
    settings_t def; defaults(&def);
    check("load with both slots bad keeps defaults but stays writable",
          load_with(a, b, &fs, &got) == 0 && settings_eq(&got, &def) &&
          config_writable() == 1);

    /* File absent entirely. */
    build_image(0);
    check("mount (no CORECFG.DAT)",
          fat32_mount(&fs, mem_read, 0, IMG_PART_LBA) == 0);
    defaults(&got);
    check("load with no CORECFG.DAT disables writing",
          config_load(&fs, &got) == 0 && config_writable() == 0);

    /* File present but too small to hold two slots. */
    build_image(CONFIG_MIN_BYTES - 1u);
    check("mount (short CORECFG.DAT)",
          fat32_mount(&fs, mem_read, 0, IMG_PART_LBA) == 0);
    defaults(&got);
    check("load with a too-small CORECFG.DAT disables writing",
          config_load(&fs, &got) == 0 && config_writable() == 0);

    /* Null arguments. */
    check("load(null fs) disables writing",
          config_load(0, &got) == 0 && config_writable() == 0);
}

/* ---- 4. write grammar -------------------------------------------------- */

/*
 * The exact register sequence ata.c emits for ata_write_sectors(lba, 2, buf),
 * asserted event for event. `data` is the 1024-byte record we expect on the
 * wire, checked as 512 little-endian halfwords.
 */
static void expect_write_trace(trace_cursor *tc, uint32_t lba,
                               const uint8_t *data)
{
    /* ata_wait_ready: timed (USEC_TIMER start), !BSY && RDY on first read. */
    expect_r(tc, 32, USEC_TIMER_ADDR);
    expect_r(tc, 8,  ATA_ALT_STATUS_ADDR);

    /* LBA28 task-file programming. This is where a wrong address would show
     * up, so the values are checked, not just the addresses. */
    expect_w(tc, 8, ATA_NSECTOR_ADDR, CONFIG_SLOT_SECTORS);
    expect_w(tc, 8, ATA_SECTOR_ADDR,  lba & 0xFFu);
    expect_w(tc, 8, ATA_LCYL_ADDR,    (lba >> 8)  & 0xFFu);
    expect_w(tc, 8, ATA_HCYL_ADDR,    (lba >> 16) & 0xFFu);
    expect_w(tc, 8, ATA_SELECT_ADDR,
             ATA_SELECT_OBS | ATA_SELECT_LBA | ((lba >> 24) & 0x0Fu));
    /* Command opcodes are spelled out rather than imported from ata.c: a
     * golden trace has to state independently what the driver should emit
     * (04-ata.md — WRITE SECTORS 0x30, FLUSH CACHE 0xE7), or it just agrees
     * with whatever the driver happens to do today. */
    expect_w(tc, 8, ATA_COMMAND_ADDR, 0x30u);        /* WRITE SECTORS      */

    /* One DRQ-out data phase per logical sector. */
    for (uint32_t s = 0; s < CONFIG_SLOT_SECTORS; s++) {
        expect_r(tc, 32, USEC_TIMER_ADDR);           /* ata_wait_drq t0    */
        expect_r(tc, 8,  ATA_ALT_STATUS_ADDR);       /* BSY clear + DRQ    */
        expect_r(tc, 8,  ATA_COMMAND_ADDR);          /* status ack         */
        for (uint32_t w = 0; w < 256u; w++) {
            uint32_t off = s * 512u + w * 2u;
            uint32_t hw  = (uint32_t)data[off] | ((uint32_t)data[off + 1] << 8);
            expect_w(tc, 16, ATA_DATA_ADDR, hw);
        }
        expect_r(tc, 32, USEC_TIMER_ADDR);           /* post-sector: wait  */
        expect_r(tc, 8,  ATA_ALT_STATUS_ADDR);       /* !BSY, ERR/DF check */
    }

    /* Post-data completion wait (the drive holds BSY while it commits). */
    expect_r(tc, 32, USEC_TIMER_ADDR);
    expect_r(tc, 8,  ATA_ALT_STATUS_ADDR);
    expect_r(tc, 8,  ATA_ALT_STATUS_ADDR);           /* ERR/DF check       */

    /* FLUSH CACHE — load-bearing: the drive's write cache is on by default,
     * so without this the record is only in the drive's DRAM. */
    expect_r(tc, 32, USEC_TIMER_ADDR);               /* wait_ready: start  */
    expect_r(tc, 8,  ATA_ALT_STATUS_ADDR);           /* !BSY && RDY        */
    expect_w(tc, 8, ATA_SELECT_ADDR,  ATA_SELECT_OBS);
    expect_w(tc, 8, ATA_COMMAND_ADDR, 0xE7u);        /* FLUSH CACHE        */
    expect_r(tc, 32, USEC_TIMER_ADDR);
    expect_r(tc, 8,  ATA_ALT_STATUS_ADDR);
    expect_r(tc, 8,  ATA_ALT_STATUS_ADDR);           /* ERR/DF check       */
}

static void test_write_trace(void)
{
    uint8_t rec0[CONFIG_SLOT_BYTES], expect[CONFIG_SLOT_BYTES];
    settings_t saved, now;
    fat32_t fs;

    /* Load a good record from SLOT 0 with seq 41, so the next save must go to
     * SLOT 1 with seq 42. */
    defaults(&saved); saved.volume = 33;
    config_encode(rec0, &saved, 41);
    check("trace setup: load slot 0",
          load_with(rec0, 0, &fs, &now) == 1 && config_seq() == 41u);

    now.volume = 88;
    now.shuffle = 1;
    config_encode(expect, &now, 42);      /* what config_save must emit */

    arm_drive_ready();
    check("save returns 0", config_save(&now) == 0);
    check("save bumped seq", config_seq() == 42u);

    trace_cursor tc = trace_begin("cfg-write-slot1");
    expect_write_trace(&tc, CFG_LBA1, expect);
    trace_expect_end(&tc);
    if (trace_done(&tc) != 0) {
        g_fails++;
    }

    /* The SECOND save must alternate back to slot 0 — that alternation is the
     * entire torn-write defence, so it is asserted, not assumed. */
    now.theme = 1;
    config_encode(expect, &now, 43);
    arm_drive_ready();
    check("second save returns 0", config_save(&now) == 0);
    check("second save bumped seq", config_seq() == 43u);

    trace_cursor tc2 = trace_begin("cfg-write-slot0-alternate");
    expect_write_trace(&tc2, CFG_LBA0, expect);
    trace_expect_end(&tc2);
    if (trace_done(&tc2) != 0) {
        g_fails++;
    }
}

/*
 * A FRESH file (both slots zero) — the state make_config.py --create leaves
 * behind, and what --verify describes as "will write slot 0 on the first
 * save". The comment in config_load said the same. The code disagreed: with
 * no record loaded g_cfg.slot stayed 0, config_save alternated to the OTHER
 * slot, and the first write went to slot 1. Pinned on the wire, both saves.
 */
static void test_fresh_file_slot(void)
{
    uint8_t expect[CONFIG_SLOT_BYTES];
    settings_t s;
    fat32_t fs;

    check("fresh setup: load finds no record but the file is writable",
          load_with(0, 0, &fs, &s) == 0 && config_writable() == 1 &&
          config_seq() == 0u);

    s.volume = 55;
    config_encode(expect, &s, 1);
    arm_drive_ready();
    check("fresh file: first save returns 0", config_save(&s) == 0);
    check("fresh file: first save is seq 1", config_seq() == 1u);
    trace_cursor tc = trace_begin("cfg-fresh-first-write-slot0");
    expect_write_trace(&tc, CFG_LBA0, expect);
    trace_expect_end(&tc);
    if (trace_done(&tc) != 0) {
        g_fails++;
    }

    s.theme = 2;
    config_encode(expect, &s, 2);
    arm_drive_ready();
    check("fresh file: second save returns 0", config_save(&s) == 0);
    trace_cursor tc2 = trace_begin("cfg-fresh-second-write-slot1");
    expect_write_trace(&tc2, CFG_LBA1, expect);
    trace_expect_end(&tc2);
    if (trace_done(&tc2) != 0) {
        g_fails++;
    }

    /* Both slots damaged is the same "no record" state and takes the same
     * path: recovery starts in slot 0. */
    uint8_t a[CONFIG_SLOT_BYTES], b[CONFIG_SLOT_BYTES];
    for (unsigned i = 0; i < sizeof a; i++) a[i] = 0xFF;
    for (unsigned i = 0; i < sizeof b; i++) b[i] = 0x5A;
    check("both-bad setup: load finds no record but stays writable",
          load_with(a, b, &fs, &s) == 0 && config_writable() == 1);
    config_encode(expect, &s, 1);
    arm_drive_ready();
    check("both slots bad: first save returns 0", config_save(&s) == 0);
    trace_cursor tc3 = trace_begin("cfg-bothbad-first-write-slot0");
    expect_write_trace(&tc3, CFG_LBA0, expect);
    trace_expect_end(&tc3);
    if (trace_done(&tc3) != 0) {
        g_fails++;
    }
}

static void test_save_refusals(void)
{
    settings_t s;
    fat32_t fs;

    /* No CORECFG.DAT => config_save must refuse AND must not touch the bus.
     * "Returns an error" is not good enough here: the requirement is that no
     * command reaches the drive at all. */
    build_image(0);
    check("refusal setup: mount", fat32_mount(&fs, mem_read, 0, IMG_PART_LBA) == 0);
    defaults(&s);
    check("refusal setup: load fails", config_load(&fs, &s) == 0);

    arm_drive_ready();
    check("save refuses with no config file", config_save(&s) < 0);
    check("refused save emitted ZERO bus events", mmio_mock_log_len() == 0);

    /* Too small a file: same deal. */
    build_image(CONFIG_MIN_BYTES - 1u);
    check("refusal setup: mount short",
          fat32_mount(&fs, mem_read, 0, IMG_PART_LBA) == 0);
    defaults(&s);
    (void)config_load(&fs, &s);
    arm_drive_ready();
    check("save refuses with a too-small config file", config_save(&s) < 0);
    check("refused short-file save emitted ZERO bus events",
          mmio_mock_log_len() == 0);

    /* Null settings. */
    arm_drive_ready();
    check("save refuses a null settings pointer", config_save(0) < 0);
    check("null save emitted ZERO bus events", mmio_mock_log_len() == 0);

    /* And after a load that never ran at all (module reset by the null-fs
     * path above), a save still refuses. */
    (void)config_load(0, &s);
    arm_drive_ready();
    check("save refuses after a failed load", config_save(&s) < 0);
    check("post-failed-load save emitted ZERO bus events",
          mmio_mock_log_len() == 0);
}

/* ---- probe ------------------------------------------------------------- */

/* ---- 4b. read errors at load ------------------------------------------- */

/*
 * A read that FAILS is not a slot that decoded invalid, and config_load used
 * to conflate them: with both slot reads erroring (the drive still settling
 * at boot) `have` stayed 0, the module stayed writable at seq 0, and the
 * first save wrote seq 1 into a slot while the other still held seq 57 — so
 * the next boot preferred the old slot and the whole session's changes,
 * resume position included, were silently discarded. Now a failed read is
 * retried after a settle, and a slot still unreadable with no good record
 * to fall back on makes the module refuse to write for the session.
 */
#define ROOT_LBA  (IMG_PART_LBA + 2u * (IMG_BPS / 512u))              /* 72 */

static void test_load_read_errors(void)
{
    uint8_t rec0[CONFIG_SLOT_BYTES], rec1[CONFIG_SLOT_BYTES];
    uint8_t expect[CONFIG_SLOT_BYTES];
    settings_t want0, want1, got;
    fat32_t fs;

    defaults(&want0); want0.volume = 40;  want0.theme = 2;    /* seq 57 */
    defaults(&want1); want1.volume = 70;  want1.theme = 0;    /* seq 56 */
    config_encode(rec0, &want0, 57);
    config_encode(rec1, &want1, 56);

    /* TRANSIENT: the newest slot's first read errors, then the drive settles.
     * Without the retry the load would take slot 1 (seq 56) and the very next
     * save would write seq 57 over slot 0 — the user's newest settings. */
    fail_sectors(CFG_LBA0, CONFIG_SLOT_SECTORS, 1);
    int rc = load_with(rec0, rec1, &fs, &got);
    check("transient slot-0 read error: the retry gets the newest record",
          rc == 1 && config_seq() == 57u && settings_eq(&got, &want0));
    check("transient slot-0 read error: exactly one read was refused",
          g_fail_hits == 1);
    check("transient slot-0 read error: still writable", config_writable() == 1);
    fail_none();

    /* BOTH slots unreadable, for good. The record we cannot see may be the
     * newest one; a save from seq 0 would lose to it on the next boot. The
     * module must not write this session — and must not touch the bus. */
    fail_sectors(CFG_LBA0, CONFIG_SECTORS, -1);
    rc = load_with(rec0, rec1, &fs, &got);
    settings_t def; defaults(&def);
    check("both slots unreadable: load fails and keeps defaults",
          rc == 0 && settings_eq(&got, &def));
    check("both slots unreadable: each slot was retried",
          g_fail_hits == 2 * (1 + (int)CONFIG_READ_RETRIES));
    check("both slots unreadable: NOT writable this session",
          config_writable() == 0 && config_seq() == 0u);
    arm_drive_ready();
    check("both slots unreadable: save refuses", config_save(&got) < 0);
    check("both slots unreadable: refused save emitted ZERO bus events",
          mmio_mock_log_len() == 0);
    fail_none();

    /* One slot unreadable, the other garbage: same thing — there is no record
     * to anchor a sequence number on. */
    for (unsigned i = 0; i < sizeof rec1; i++) rec1[i] = (uint8_t)(i * 3u);
    fail_sectors(CFG_LBA0, CONFIG_SLOT_SECTORS, -1);
    rc = load_with(rec0, rec1, &fs, &got);
    check("unreadable slot + garbage slot: NOT writable",
          rc == 0 && config_writable() == 0);
    fail_none();
    config_encode(rec1, &want1, 56);

    /* One slot unreadable, the other VALID: writable. The next write must go
     * to the unreadable slot with a seq that beats anything it could hold —
     * that is both how a power-cut-spoiled slot is recovered and why nothing
     * can invert. Asserted on the wire, not assumed. */
    fail_sectors(CFG_LBA0, CONFIG_SLOT_SECTORS, -1);
    rc = load_with(rec0, rec1, &fs, &got);
    check("unreadable slot 0 + valid slot 1: loads slot 1 and stays writable",
          rc == 1 && config_seq() == 56u && settings_eq(&got, &want1) &&
          config_writable() == 1);
    got.volume = 41;
    config_encode(expect, &got, 57);
    arm_drive_ready();
    check("unreadable slot 0: save succeeds", config_save(&got) == 0);
    check("unreadable slot 0: save bumped seq past the good slot's",
          config_seq() == 57u);
    trace_cursor tc = trace_begin("cfg-write-into-unreadable-slot0");
    expect_write_trace(&tc, CFG_LBA0, expect);
    trace_expect_end(&tc);
    if (trace_done(&tc) != 0) {
        g_fails++;
    }
    fail_none();

    /* The ROOT walk that locates the file goes through the same retry: one
     * failed read of the root directory must not disable settings for the
     * session, and a root that never reads must. */
    fail_sectors(ROOT_LBA, IMG_BPS / 512u, 1);
    rc = load_with(rec0, rec1, &fs, &got);
    check("transient root-directory read error: the retry finds the file",
          rc == 1 && config_seq() == 57u && config_writable() == 1);
    fail_none();

    fail_sectors(ROOT_LBA, IMG_BPS / 512u, -1);
    rc = load_with(rec0, rec1, &fs, &got);
    check("root directory unreadable: load fails, NOT writable",
          rc == 0 && config_writable() == 0);
    check("root directory unreadable: the walk was retried",
          g_fail_hits == 1 + (int)CONFIG_READ_RETRIES);
    fail_none();
}

static void test_probe(void)
{
    uint8_t rec[CONFIG_SLOT_BYTES];
    settings_t s;
    fat32_t fs;

    defaults(&s);
    config_encode(rec, &s, 1);
    check("probe setup", load_with(rec, 0, &fs, &s) == 1);

    uint32_t l0 = 0, l1 = 0;
    check("probe slot 0 LBA",
          config_probe_lba(0, &l0) == 0 && l0 == CFG_LBA0);
    check("probe slot 1 LBA",
          config_probe_lba(1, &l1) == 0 && l1 == CFG_LBA1);
    check("probe slots are one physical sector apart",
          l1 - l0 == CONFIG_SLOT_SECTORS);
    check("probe LBAs are physical-sector aligned",
          (l0 % ATA_PHYS_LOG) == 0 && (l1 % ATA_PHYS_LOG) == 0);

    uint32_t bad = 0xDEADBEEFu;
    check("probe rejects an out-of-range slot",
          config_probe_lba(CONFIG_SLOTS, &bad) != 0 && bad == 0);
}

/* ---- 5. host/device format agreement ----------------------------------- */

/*
 * The record tools/make_config.py writes must be one this firmware ACCEPTS.
 * The two encoders are independent implementations of the same spec — offsets,
 * field order, signedness, CRC polynomial and coverage — and if they ever
 * drift, a freshly-created CORECFG.DAT is silently rejected on device and
 * settings simply never persist. That is a bug you would otherwise find on a
 * user's iPod, so it is pinned here instead.
 *
 * `path` is the fixture meson generates by running the real host tool.
 */
static void test_host_fixture(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == 0) {
        check("host fixture opens", 0);
        return;
    }
    uint8_t blob[CONFIG_MIN_BYTES];
    size_t got = fread(blob, 1, sizeof blob, f);
    fclose(f);
    check("host fixture is the two-slot region", got == sizeof blob);
    if (got != sizeof blob) {
        return;
    }

    settings_t s;
    uint32_t   seq = 0;
    memset(&s, 0xA5, sizeof s);
    check("firmware accepts the host tool's default record",
          config_decode(blob, &s, &seq) == 1 && seq == 1u);
    check("host default record decodes to the expected values",
          s.volume == 70 && s.backlight_secs == 15 &&
          s.backlight_bright == 32 && s.shuffle == 0 &&
          s.repeat == REPEAT_OFF && s.theme == 0 && s.clicker == 1);
    /*
     * The host tool's record must agree with settings_defaults(), field for
     * field — it is not "the tool's own defaults". config_load() prefers ANY
     * valid record over settings_defaults(), so whatever this file says is what
     * the device believes forever after.
     *
     * This exact drift shipped: the tool wrote resume_on_startup = 0 while the
     * firmware defaulted it to 1, so Resume was silently off on a device whose
     * CORECFG.DAT the tool had created — while every other setting persisted
     * correctly, which made it look like a resume bug rather than a defaults
     * bug (diagnosed on hardware 2026-07-27).
     */
    settings_t def;
    settings_defaults(&def);
    check("host defaults match settings_defaults() for resume_on_startup",
          s.resume_on_startup == def.resume_on_startup);
    check("host defaults match settings_defaults() across the board",
          s.shuffle == def.shuffle && s.repeat == def.repeat &&
          s.crossfade == def.crossfade && s.volume == def.volume &&
          s.bass == def.bass && s.treble == def.treble &&
          s.balance == def.balance &&
          s.backlight_secs == def.backlight_secs &&
          s.backlight_bright == def.backlight_bright &&
          s.theme == def.theme && s.clicker == def.clicker &&
          s.volume_limit == def.volume_limit && s.eq == def.eq);
    /* The host tool writes the v2 layout too — if it kept emitting v1 the
     * device would still boot, but a freshly imported iPod would silently be
     * unable to remember a position until its first save. */
    check("host record is v2 with an empty resume locator",
          blob[4] == 2 && blob[5] == 0 && blob[6] == 48 && blob[7] == 0 &&
          s.resume_hash == 0 && s.resume_secs == 0 && s.resume_total == 0 &&
          s.resume_kind == 0 && s.resume_qidx == 0 && s.resume_seed == 0 &&
          s.resume_order_seed == 0 && s.resume_order_keep == 0);
    check("host tool leaves slot 1 empty for the first device write",
          config_decode(&blob[CONFIG_SLOT_BYTES], &s, &seq) == 0);
}

int main(int argc, char **argv)
{
    test_file_lba();
    test_codec();
    test_resume_record();
    test_resume_context();
    test_sound_tail();
    test_seq_order();
    test_two_slot();
    test_write_trace();
    test_fresh_file_slot();
    test_save_refusals();
    test_load_read_errors();
    test_probe();
    if (argc > 1) {
        test_host_fixture(argv[1]);
    } else {
        printf("[host fixture] SKIPPED (no fixture path given)\n");
    }

    printf("config_test: %s (%d failure%s)\n",
           g_fails == 0 ? "PASS" : "FAIL", g_fails, g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
