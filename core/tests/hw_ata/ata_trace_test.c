/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/hw_ata/ata_trace_test.c — golden-trace tests for the PIO ATA driver
 * (hal/hw/ata.c), host-side under -DMMIO_MOCK.
 *
 * WHY THIS FILE EXISTS. Every byte of music, every FAT sector, every
 * settings save goes through ata.c, and until now its entire host coverage
 * was two happy-path cases tucked into the AUDIO trace test: an ata_init
 * grammar with RDY pre-set, and one 2-sector read with RDY|DRQ pre-set so
 * every poll passed on its first read. Nothing exercised a poll that had to
 * spin, a poll that never completes, the ERR path, the mid-transfer recovery
 * that exists to stop a retry returning shifted data as success, the >256-
 * sector split, spin-down/wake, or the write path at all — and clicky models
 * no drive, so the device is the only other place any of that runs. Those
 * two cases now live here, alongside the ones that were missing.
 *
 * WHAT EACH GROUP PROVES:
 *   init     — the reset+select+wait grammar, and that the ready poll really
 *              spins on BOTH conditions (BSY clear, then RDY set); that a
 *              drive that never comes ready is given up on by the TIMER —
 *              elapsed > the ceiling, not a poll count — and that the
 *              ceiling after a soft reset is the ATA spin-up allowance
 *              (31 s), not the plain 10 s one.
 *   read     — the moved 2-sector odd-LBA case, now as an exact trace; the
 *              spin-up-tolerant DRQ wait (BSY N times, then DRQ); the TIMED
 *              DRQ deadline (elapsed > ATA_SPINUP_US, not a poll count); the
 *              post-sector status waited for !BSY before ERR/DF is believed;
 *              the ERR path before and after data, asserting the recovery
 *              grammar — ERROR latched BEFORE the reset clears it, residual
 *              DRQ drained word by word, soft reset, reselect, and the
 *              post-reset ready wait's result actually checked — plus the
 *              IDNF upgrade; and the >256-sector split into two commands.
 *   power    — STANDBY IMMEDIATE grammar and the parked flag; wake through a
 *              spin-up (BSY) then normal reads; parked reconciled to 0 on a
 *              failed wake and on any plain read; a wedged drive can't park.
 *   write    — the full WRITE SECTORS + FLUSH CACHE grammar with every data
 *              word checked; and the recovery the write path used to skip:
 *              DRQ error, post-data error, completion timeout and flush
 *              error must each latch ERROR, drain, reset — because the next
 *              command after a settings save is the player's refill read.
 *   lba28    — an LBA range past 2^28 is rejected before a single register
 *              write, instead of having its top bits masked off and quietly
 *              aliasing a lower sector (and, on a write, overwriting it).
 *
 * Values are hand-derived from core/docs/hw/04-ata.md via pp5022.h, never
 * from Rockbox source. Private ata.c constants are MIRRORED here rather than
 * included, so a silent change to a spin limit or a command byte shows up as
 * a test failure — that is the point of a golden trace.
 */

#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "pp5022.h"
#include "ata.h"
#include "mmio_mock.h"
#include "trace_expect.h"
#include "../xfail.h"

/* ---- mirrors of ata.c's private constants ---------------------------- */

#define READY_US          10000000u    /* ATA_READY_US: !BSY / RDY ceiling   */
#define SRST_READY_US     31000000u    /* ATA_SRST_READY_US: after soft reset */
#define SPINUP_US         8000000u     /* ATA_SPINUP_US: timed DRQ deadline  */
#define CMD_STANDBY_IMM   0xE0u
#define CMD_WRITE_SECTORS 0x30u
#define CMD_FLUSH_CACHE   0xE7u
#define ERROR_ABRT        0x04u        /* any non-IDNF error bit will do     */

#define RDY   ATA_STATUS_RDY
#define BSY   ATA_STATUS_BSY
#define DRQ   ATA_STATUS_DRQ
#define ERR   ATA_STATUS_ERR

/* ---- watchdog: "bounded" is the claim, so a hang must be a FAIL -------- *
 * A driver that spins forever on a wedged status register would otherwise
 * hang the binary until meson's timeout kills it, which reports as a timeout
 * rather than as the specific assertion that failed. 5 s is far past any
 * legitimate wait here: the deadlines are on the scripted usec timer, so a
 * wait that gives up correctly does so in a handful of polls. */

static sigjmp_buf g_escape;
static volatile sig_atomic_t g_hung;

static void on_alarm(int sig)
{
    (void)sig;
    g_hung = 1;
    siglongjmp(g_escape, 1);
}

static void arm_watchdog(void)
{
    struct itimerval it = { { 0, 0 }, { 5, 0 } };
    g_hung = 0;
    signal(SIGALRM, on_alarm);
    setitimer(ITIMER_REAL, &it, 0);
}

static void disarm_watchdog(void)
{
    struct itimerval it = { { 0, 0 }, { 0, 0 } };
    setitimer(ITIMER_REAL, &it, 0);
    signal(SIGALRM, SIG_DFL);
}

/* Run `expr` under the watchdog; yields its return value, or 999 on a hang. */
#define GUARDED(rc_out, expr)                       \
    do {                                            \
        arm_watchdog();                             \
        if (sigsetjmp(g_escape, 1) == 0) {          \
            (rc_out) = (expr);                      \
        } else {                                    \
            (rc_out) = 999;                         \
        }                                           \
        disarm_watchdog();                          \
    } while (0)

/* ---- log helpers ------------------------------------------------------ */

static size_t count_writes(uint32_t addr)
{
    return mmio_mock_count(MMIO_OP_WRITE, addr);
}

/* Value of the n-th (0-based) write to `addr`; ~0u if there is none. */
static uint32_t nth_write(uint32_t addr, size_t n)
{
    const mmio_event *log = mmio_mock_log();
    size_t len = mmio_mock_log_len(), c = 0;
    for (size_t i = 0; i < len; i++) {
        if (log[i].op == MMIO_OP_WRITE && log[i].addr == addr) {
            if (c == n) {
                return log[i].value;
            }
            c++;
        }
    }
    return ~0u;
}

/* Everything the driver did, including what fell off the end of the log. */
static size_t total_events(void)
{
    return mmio_mock_log_len() + mmio_mock_dropped();
}

/* Index of the first event matching (op, addr), or the log length. */
static size_t first_index(mmio_op op, uint32_t addr)
{
    const mmio_event *log = mmio_mock_log();
    size_t len = mmio_mock_log_len();
    for (size_t i = 0; i < len; i++) {
        if (log[i].op == op && log[i].addr == addr) {
            return i;
        }
    }
    return len;
}

static int all_halfwords(const uint16_t *p, size_t n, uint16_t v)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] != v) {
            return 0;
        }
    }
    return 1;
}

/* ---- grammar fragments ------------------------------------------------ */

/* A timed wait (ready, or not-busy) satisfied after `busy` unsatisfied
 * polls: the usec timer is read once for the start time and once per
 * unsatisfied poll, then the status read that satisfies it. */
static void expect_timed_wait(trace_cursor *tc, size_t busy)
{
    expect_r(tc, 32, USEC_TIMER_ADDR);
    for (size_t i = 0; i < busy; i++) {
        expect_r(tc, 8, ATA_ALT_STATUS_ADDR);
        expect_r(tc, 32, USEC_TIMER_ADDR);
    }
    expect_r(tc, 8, ATA_ALT_STATUS_ADDR);
}

/* A timed wait that GIVES UP after `polls` unsatisfied polls: no final
 * satisfied status read — the last thing it does is read the timer and find
 * the deadline passed. */
static void expect_timed_timeout(trace_cursor *tc, size_t polls)
{
    expect_r(tc, 32, USEC_TIMER_ADDR);
    for (size_t i = 0; i < polls; i++) {
        expect_r(tc, 8, ATA_ALT_STATUS_ADDR);
        expect_r(tc, 32, USEC_TIMER_ADDR);
    }
}

/* ata_wait_ready satisfied on its first read. */
static void expect_wait_ready(trace_cursor *tc)
{
    expect_timed_wait(tc, 0);
}

/* The LBA28 task-file programming for one command. NSECTOR carries the
 * low byte only, so 256 is written as 0 — the drive reads 0 as 256. */
static void expect_command(trace_cursor *tc, uint32_t lba, uint32_t count,
                           uint32_t cmd)
{
    expect_w(tc, 8, ATA_NSECTOR_ADDR, count & 0xFFu);
    expect_w(tc, 8, ATA_SECTOR_ADDR,  lba & 0xFFu);
    expect_w(tc, 8, ATA_LCYL_ADDR,    (lba >> 8) & 0xFFu);
    expect_w(tc, 8, ATA_HCYL_ADDR,    (lba >> 16) & 0xFFu);
    expect_w(tc, 8, ATA_SELECT_ADDR,
             ATA_SELECT_OBS | ATA_SELECT_LBA | ((lba >> 24) & 0x0Fu));
    expect_w(tc, 8, ATA_COMMAND_ADDR, cmd);
}

/* One DRQ block arriving after `busy` BSY polls (same shape as any timed
 * wait). */
static void expect_drq_wait(trace_cursor *tc, size_t busy)
{
    expect_timed_wait(tc, busy);
}

static void expect_drq_timeout(trace_cursor *tc, size_t polls)
{
    expect_timed_timeout(tc, polls);
}

/* One sector streamed IN: status ack, 256 halfwords, then the post-sector
 * status — waited for !BSY (`post_busy` BSY polls) before ERR/DF is read
 * off it. */
static void expect_sector_in_post(trace_cursor *tc, size_t busy,
                                  size_t post_busy)
{
    expect_drq_wait(tc, busy);
    expect_r(tc, 8, ATA_COMMAND_ADDR);
    for (int w = 0; w < 256; w++) {
        expect_r(tc, 16, ATA_DATA_ADDR);
    }
    expect_timed_wait(tc, post_busy);
}

static void expect_sector_in(trace_cursor *tc, size_t busy)
{
    expect_sector_in_post(tc, busy, 0);
}

/* One sector streamed OUT, every data word checked against `words`. */
static void expect_sector_out(trace_cursor *tc, const uint16_t *words)
{
    expect_drq_wait(tc, 0);
    expect_r(tc, 8, ATA_COMMAND_ADDR);
    for (int w = 0; w < 256; w++) {
        expect_w(tc, 16, ATA_DATA_ADDR, words[w]);
    }
    expect_timed_wait(tc, 0);
}

/* The documented mid-transfer recovery (04-ata.md, "Per-sector error
 * handling"): ERROR first (a reset clears it), drain `drain` residual data
 * words while DRQ holds, soft reset, reselect the master, wait ready. */
static void expect_recovery(trace_cursor *tc, size_t drain)
{
    expect_r(tc, 8, ATA_ERROR_ADDR);
    for (size_t i = 0; i < drain; i++) {
        expect_r(tc, 8, ATA_ALT_STATUS_ADDR);    /* DRQ still set */
        expect_r(tc, 16, ATA_DATA_ADDR);         /* retire a word */
    }
    expect_r(tc, 8, ATA_ALT_STATUS_ADDR);        /* DRQ clear: done */
    expect_w(tc, 8, ATA_CONTROL_ADDR, ATA_CONTROL_SRST | ATA_CONTROL_NIEN);
    expect_w(tc, 8, ATA_CONTROL_ADDR, ATA_CONTROL_NIEN);
    expect_w(tc, 8, ATA_SELECT_ADDR, ATA_SELECT_OBS);
    expect_wait_ready(tc);
}

/* FLUSH CACHE: select, command, timed BSY wait, final status. */
static void expect_flush(trace_cursor *tc)
{
    expect_wait_ready(tc);
    expect_w(tc, 8, ATA_SELECT_ADDR, ATA_SELECT_OBS);
    expect_w(tc, 8, ATA_COMMAND_ADDR, CMD_FLUSH_CACHE);
    expect_r(tc, 32, USEC_TIMER_ADDR);
    expect_r(tc, 8, ATA_ALT_STATUS_ADDR);        /* BSY clear */
    expect_r(tc, 8, ATA_ALT_STATUS_ADDR);        /* ERR/DF?   */
}

static void finish(xfail_ctx *c, trace_cursor *tc)
{
    trace_expect_end(tc);
    if (trace_done(tc) != 0) {
        c->fails++;
    }
}

/* Scratch buffers. The split case needs 258 sectors; static keeps them off
 * the stack and lets a sentinel word past the end prove the copy stops. */
static uint16_t g_buf[258 * 256 + 1];
static uint16_t g_pattern[2 * 256];

/* ======================================================================= */
/*  init                                                                   */
/* ======================================================================= */

static void test_init(xfail_ctx *c)
{
    /* --- the moved case: RDY pre-set, every poll passes first time ------ */
    mmio_mock_reset();
    mmio_mock_set_read(ATA_ALT_STATUS_ADDR, RDY);
    int rc = ata_init();
    xpect(c, "init: returns 0 on a ready drive", rc == 0);

    trace_cursor tc = trace_begin("init-grammar");
    expect_w(&tc, 8, ATA_CONTROL_ADDR, ATA_CONTROL_SRST | ATA_CONTROL_NIEN);
    expect_w(&tc, 8, ATA_CONTROL_ADDR, ATA_CONTROL_NIEN);
    expect_w(&tc, 8, ATA_SELECT_ADDR, ATA_SELECT_OBS);
    expect_wait_ready(&tc);
    finish(c, &tc);

    /* --- BSY three times, then ready: the poll must spin on BSY --------- *
     * The timer reads 0 throughout, so the deadline never trips; the driver
     * must keep polling, consulting the timer after each miss. */
    {
        static const uint32_t seq[] = { BSY, BSY, BSY, RDY };
        mmio_mock_reset();
        mmio_mock_queue_read(ATA_ALT_STATUS_ADDR, seq, 4);
        rc = ata_init();
        xpect(c, "init: accepts a drive that is BSY for 3 polls", rc == 0);

        tc = trace_begin("init-busy-then-ready");
        expect_w(&tc, 8, ATA_CONTROL_ADDR, ATA_CONTROL_SRST | ATA_CONTROL_NIEN);
        expect_w(&tc, 8, ATA_CONTROL_ADDR, ATA_CONTROL_NIEN);
        expect_w(&tc, 8, ATA_SELECT_ADDR, ATA_SELECT_OBS);
        expect_timed_wait(&tc, 3);                   /* BSY x3, then RDY  */
        finish(c, &tc);
    }

    /* --- BSY clear at once but RDY late: the poll must spin on RDY too --- *
     * A driver that only waited for !BSY would return after one read and
     * issue commands to a drive that is not ready. */
    {
        static const uint32_t seq[] = { 0, 0, 0, 0, RDY };
        mmio_mock_reset();
        mmio_mock_queue_read(ATA_ALT_STATUS_ADDR, seq, 5);
        rc = ata_init();
        xpect(c, "init: accepts a drive that is !BSY but not RDY for 4 polls",
              rc == 0);

        tc = trace_begin("init-ready-late");
        expect_w(&tc, 8, ATA_CONTROL_ADDR, ATA_CONTROL_SRST | ATA_CONTROL_NIEN);
        expect_w(&tc, 8, ATA_CONTROL_ADDR, ATA_CONTROL_NIEN);
        expect_w(&tc, 8, ATA_SELECT_ADDR, ATA_SELECT_OBS);
        expect_timed_wait(&tc, 4);                   /* 0 x4, then RDY    */
        finish(c, &tc);
    }

    /* --- permanently BSY: the TIMER gives up, at the post-reset ceiling -- *
     * The wait after the SRST carries the ATA spin-up allowance (31 s), not
     * the plain 10 s ceiling. The timer is scripted: 0 at the start, then 0,
     * 5 s, exactly the deadline (which must NOT trip: the test is `>`), then
     * past it — four unsatisfied polls, then -1. Under the old iteration-
     * bounded wait this took 1<<20 polls regardless of time; under a 10 s
     * ceiling it would have given up on the second poll. */
    {
        static const uint32_t usec[] = { 0, 0, 5000000u, SRST_READY_US,
                                         SRST_READY_US + 1u };
        mmio_mock_reset();
        mmio_mock_set_read(ATA_ALT_STATUS_ADDR, BSY);
        mmio_mock_queue_read(USEC_TIMER_ADDR, usec, 5);
        GUARDED(rc, ata_init());
        xpect(c, "init: a permanently-BSY drive does not hang", rc != 999);
        xpect(c, "init: a permanently-BSY drive returns -1", rc == -1);

        tc = trace_begin("init-busy-forever");
        expect_w(&tc, 8, ATA_CONTROL_ADDR, ATA_CONTROL_SRST | ATA_CONTROL_NIEN);
        expect_w(&tc, 8, ATA_CONTROL_ADDR, ATA_CONTROL_NIEN);
        expect_w(&tc, 8, ATA_SELECT_ADDR, ATA_SELECT_OBS);
        expect_timed_timeout(&tc, 4);   /* 0, 5 s, ==deadline, >deadline */
        finish(c, &tc);
    }

    /* --- a drive that takes 20 s to come back from the reset is FINE ----- *
     * The spec allows 31 s of spin-up after SRST; with a 10 s ceiling this
     * init would fail and the boot with it. Timer: start 0, then 10 s + 1 us
     * (past the plain ceiling — must keep polling), then 20 s, then RDY. */
    {
        static const uint32_t seq[]  = { BSY, BSY, RDY };
        static const uint32_t usec[] = { 0, READY_US + 1u, 20000000u };
        mmio_mock_reset();
        mmio_mock_queue_read(ATA_ALT_STATUS_ADDR, seq, 3);
        mmio_mock_queue_read(USEC_TIMER_ADDR, usec, 3);
        GUARDED(rc, ata_init());
        xpect(c, "init: a 20 s post-reset spin-up is accepted (31 s ceiling)",
              rc == 0);
        tc = trace_begin("init-slow-spinup");
        expect_w(&tc, 8, ATA_CONTROL_ADDR, ATA_CONTROL_SRST | ATA_CONTROL_NIEN);
        expect_w(&tc, 8, ATA_CONTROL_ADDR, ATA_CONTROL_NIEN);
        expect_w(&tc, 8, ATA_SELECT_ADDR, ATA_SELECT_OBS);
        expect_timed_wait(&tc, 2);
        finish(c, &tc);
    }
}

/* ======================================================================= */
/*  read                                                                   */
/* ======================================================================= */

static void test_read(xfail_ctx *c)
{
    /* --- the moved case: 1 logical sector at an ODD LBA ----------------- *
     * The drive IDNFs sub-physical-sector reads, so the wrapper rounds down
     * to the physical boundary (0x01234567 -> 0x01234566), reads both
     * logical sectors of it into the bounce, and copies out only the one
     * asked for. The data port serves a constant so the copy can be checked:
     * exactly 256 halfwords land in the caller's buffer and not one more. */
    mmio_mock_reset();
    mmio_mock_set_read(ATA_ALT_STATUS_ADDR, RDY | DRQ);
    mmio_mock_set_read(ATA_DATA_ADDR, 0xA55A);
    memset(g_buf, 0, sizeof g_buf);
    int rc = ata_read_sectors(0x01234567u, 1, g_buf);
    xpect(c, "read: odd-LBA single sector returns 0", rc == 0);
    xpect(c, "read: NSECTOR=2 (a whole physical sector)",
          nth_write(ATA_NSECTOR_ADDR, 0) == 2);
    xpect(c, "read: SECTOR=0x66 (rounded down to the physical boundary)",
          nth_write(ATA_SECTOR_ADDR, 0) == 0x66);
    xpect(c, "read: SELECT=0xE1 (obs|LBA|high nibble)",
          nth_write(ATA_SELECT_ADDR, 0) ==
              (ATA_SELECT_OBS | ATA_SELECT_LBA | 0x1u));
    xpect(c, "read: 512 data-port reads (both logical sectors)",
          mmio_mock_count(MMIO_OP_READ, ATA_DATA_ADDR) == 512);
    xpect(c, "read: the requested sector was copied out of the bounce",
          all_halfwords(g_buf, 256, 0xA55A));
    xpect(c, "read: and nothing past it", g_buf[256] == 0);

    trace_cursor tc = trace_begin("read-odd-lba-grammar");
    expect_wait_ready(&tc);
    expect_command(&tc, 0x01234566u, 2, ATA_CMD_READ_SECTORS);
    expect_sector_in(&tc, 0);
    expect_sector_in(&tc, 0);
    finish(c, &tc);

    /* --- DRQ arrives after 3 BSY polls (a spun-down drive spinning up) --- *
     * The usec timer reads 0 throughout, so the deadline never trips; the
     * driver must keep polling, consulting the timer each time, and then
     * transfer normally. */
    {
        static const uint32_t seq[] = { RDY, BSY, BSY, BSY, RDY | DRQ };
        mmio_mock_reset();
        mmio_mock_queue_read(ATA_ALT_STATUS_ADDR, seq, 5);
        rc = ata_read_sectors(0x1000u, 2, g_buf);
        xpect(c, "read: DRQ after 3 BSY polls returns 0", rc == 0);

        tc = trace_begin("read-drq-after-busy");
        expect_wait_ready(&tc);
        expect_command(&tc, 0x1000u, 2, ATA_CMD_READ_SECTORS);
        expect_sector_in(&tc, 3);
        expect_sector_in(&tc, 0);
        finish(c, &tc);
    }

    /* --- DRQ never arrives: the TIMED deadline, then recovery ----------- *
     * Status is !BSY, RDY, no DRQ, no ERR — a drive that accepted the command
     * and then said nothing. The timer is scripted (after the ready wait's
     * own start read) so that the elapsed time reads 0, 1 s, exactly the
     * deadline (which must NOT trip: the test is `>`), then past it — four
     * unsatisfied polls. The driver must give up with -2 and, because the
     * drive is still mid-command from our point of view, run the recovery. */
    {
        static const uint32_t usec[] = { 0, 0, 0, 1000000u, SPINUP_US,
                                         SPINUP_US + 1u };
        mmio_mock_reset();
        mmio_mock_set_read(ATA_ALT_STATUS_ADDR, RDY);
        mmio_mock_queue_read(USEC_TIMER_ADDR, usec, 6);
        GUARDED(rc, ata_read_sectors(0x1000u, 2, g_buf));
        xpect(c, "read: a DRQ that never comes does not hang", rc != 999);
        xpect(c, "read: a DRQ that never comes returns -2 (timeout)", rc == -2);

        tc = trace_begin("read-drq-timeout");
        expect_wait_ready(&tc);
        expect_command(&tc, 0x1000u, 2, ATA_CMD_READ_SECTORS);
        expect_drq_timeout(&tc, 4);    /* 0, 1 s, ==deadline, >deadline */
        expect_recovery(&tc, 0);
        finish(c, &tc);
    }

    /* --- ERR before any data (the drive rejected the command) ----------- */
    mmio_mock_reset();
    mmio_mock_set_read(ATA_ALT_STATUS_ADDR, RDY | ERR);
    mmio_mock_set_read(ATA_ERROR_ADDR, ERROR_ABRT);
    rc = ata_read_sectors(0x1000u, 2, g_buf);
    xpect(c, "read: ERR at the DRQ wait returns -3 (retryable)", rc == -3);
    tc = trace_begin("read-err-before-data");
    expect_wait_ready(&tc);
    expect_command(&tc, 0x1000u, 2, ATA_CMD_READ_SECTORS);
    expect_drq_wait(&tc, 0);
    expect_recovery(&tc, 0);
    finish(c, &tc);

    /* --- the same, with IDNF latched: NOT retryable ---------------------- */
    mmio_mock_reset();
    mmio_mock_set_read(ATA_ALT_STATUS_ADDR, RDY | ERR);
    mmio_mock_set_read(ATA_ERROR_ADDR, ATA_ERROR_IDNF);
    rc = ata_read_sectors(0x1000u, 2, g_buf);
    xpect(c, "read: ERR with IDNF latched returns ATA_ERR_IDNF",
          rc == ATA_ERR_IDNF);
    xpect(c, "read: ERROR is latched BEFORE the reset that would clear it",
          first_index(MMIO_OP_READ, ATA_ERROR_ADDR) <
              first_index(MMIO_OP_WRITE, ATA_CONTROL_ADDR));

    /* --- ERR after a sector, with data still queued: the DRAIN ---------- *
     * The scenario the recovery banner describes. Sector 0 transfers, then
     * the post-sector status carries ERR while DRQ is STILL set: the drive
     * has more of the (now failed) command buffered. Three status reads show
     * DRQ, then it clears. The driver must retire exactly those three words
     * through the data port before resetting — leave them and the next READ
     * SECTORS returns data shifted by three halfwords, reported as success. */
    {
        static const uint32_t seq[] = {
            RDY,                    /* wait ready                       */
            RDY | DRQ,              /* sector 0 DRQ                     */
            RDY | DRQ | ERR,        /* post-sector status: ERR, DRQ up  */
            RDY | DRQ, RDY | DRQ, RDY | DRQ,   /* drain: 3 words        */
            RDY,                    /* drained; also serves wait ready  */
        };
        mmio_mock_reset();
        mmio_mock_queue_read(ATA_ALT_STATUS_ADDR, seq, 7);
        mmio_mock_set_read(ATA_ERROR_ADDR, ERROR_ABRT);
        rc = ata_read_sectors(0x1000u, 2, g_buf);
        xpect(c, "read: ERR after sector 0 returns -3", rc == -3);

        tc = trace_begin("read-err-after-data-drain");
        expect_wait_ready(&tc);
        expect_command(&tc, 0x1000u, 2, ATA_CMD_READ_SECTORS);
        expect_sector_in(&tc, 0);
        expect_recovery(&tc, 3);
        finish(c, &tc);
    }

    /* --- >256 sectors must split into two READ SECTORS commands --------- *
     * NSECTOR is one byte, so a 258-sector request has to go out as 256 + 2.
     * The catch for a host test: one 256-sector command alone is ~66k bus
     * events and the mock log holds 65536, so the second command's task-file
     * writes fall off the end and cannot be matched directly. What CAN be
     * seen: the first command's header (NSECTOR=0, i.e. 256 — NOT 258&0xFF=2,
     * which is what a driver without the cap would program); the total bus
     * traffic including dropped events, which is calibrated here against a
     * lone 256-sector and a lone 2-sector read and must equal their sum (one
     * 258-sector command would be 8 header events short); and the data —
     * every one of the 258*256 halfwords lands, and the sentinel after them
     * does not. */
    {
        mmio_mock_reset();
        mmio_mock_set_read(ATA_ALT_STATUS_ADDR, RDY | DRQ);
        rc = ata_read_sectors(0x2000u, 2, g_buf);
        size_t t2 = total_events();
        xpect(c, "split: calibration 2-sector read returns 0", rc == 0);

        mmio_mock_reset();
        mmio_mock_set_read(ATA_ALT_STATUS_ADDR, RDY | DRQ);
        rc = ata_read_sectors(0x2000u, 256, g_buf);
        size_t t256 = total_events();
        xpect(c, "split: calibration 256-sector read returns 0", rc == 0);
        xpect(c, "split: a 256-sector read programs NSECTOR=0 (wraps)",
              nth_write(ATA_NSECTOR_ADDR, 0) == 0);

        mmio_mock_reset();
        mmio_mock_set_read(ATA_ALT_STATUS_ADDR, RDY | DRQ);
        mmio_mock_set_read(ATA_DATA_ADDR, 0xBEEF);
        memset(g_buf, 0, sizeof g_buf);
        rc = ata_read_sectors(0x2000u, 258, g_buf);
        xpect(c, "split: a 258-sector read returns 0", rc == 0);
        xpect(c, "split: the first command is capped at 256 (NSECTOR=0)",
              nth_write(ATA_NSECTOR_ADDR, 0) == 0);
        xpect(c, "split: the first command starts at the requested LBA",
              nth_write(ATA_SECTOR_ADDR, 0) == 0x00 &&
              nth_write(ATA_LCYL_ADDR, 0) == 0x20);
        xpect(c, "split: the log saturated, as expected for this size",
              mmio_mock_dropped() > 0);
        xpect(c, "split: total traffic == one 256-sector + one 2-sector command",
              total_events() == t256 + t2);
        xpect(c, "split: all 258 sectors of data landed",
              all_halfwords(g_buf, 258 * 256, 0xBEEF));
        xpect(c, "split: and the copy stopped there", g_buf[258 * 256] == 0);
    }

    /* --- a drive that never comes ready: timed out, no command issued --- *
     * The plain (no reset) ready wait has the 10 s ceiling. Timer: start 0,
     * then 0, past the OLD 4 s data deadline and the new 8 s one (must keep
     * polling — this is the ready wait, not the DRQ wait), exactly 10 s
     * (must not trip), then past it. */
    {
        static const uint32_t usec[] = { 0, 0, SPINUP_US + 1u, READY_US,
                                         READY_US + 1u };
        mmio_mock_reset();
        mmio_mock_set_read(ATA_ALT_STATUS_ADDR, BSY);
        mmio_mock_queue_read(USEC_TIMER_ADDR, usec, 5);
        GUARDED(rc, ata_read_sectors(0x1000u, 2, g_buf));
        xpect(c, "read: a permanently-BSY drive does not hang", rc != 999);
        xpect(c, "read: a permanently-BSY drive returns -1", rc == -1);
        xpect(c, "read: no command was issued to it",
              count_writes(ATA_COMMAND_ADDR) == 0);
        tc = trace_begin("read-busy-forever");
        expect_timed_timeout(&tc, 4);   /* 0, 8 s+, ==10 s, >10 s */
        finish(c, &tc);
    }

    /* --- the post-sector status is waited for !BSY before it is believed - *
     * After the 256th word the drive raises BSY while it fetches the next
     * block, and every other status bit is undefined while it does. Sector
     * 0's post-sector status reads BSY|ERR — a stale ERR bit under BSY —
     * then BSY, then clear: the driver must poll through it and NOT run a
     * recovery. Under the old code the first read after the data was taken
     * at face value and this transfer failed with -3. */
    {
        static const uint32_t seq[] = {
            RDY,                    /* wait ready                       */
            RDY | DRQ,              /* sector 0 DRQ                     */
            BSY | ERR, BSY,         /* post-sector: BSY (bits invalid)  */
            RDY | DRQ,              /* !BSY: clean, and sector 1's DRQ  */
            RDY | DRQ,              /* sector 1 DRQ wait                */
            RDY,                    /* post-sector: clean               */
        };
        mmio_mock_reset();
        mmio_mock_queue_read(ATA_ALT_STATUS_ADDR, seq, 7);
        mmio_mock_set_read(ATA_ERROR_ADDR, ERROR_ABRT);
        GUARDED(rc, ata_read_sectors(0x1000u, 2, g_buf));
        xpect(c, "read: ERR under BSY after a sector is not an error", rc == 0);
        xpect(c, "read: and no recovery ran", count_writes(ATA_CONTROL_ADDR) == 0
              && mmio_mock_count(MMIO_OP_READ, ATA_ERROR_ADDR) == 0);
        tc = trace_begin("read-post-sector-busy");
        expect_wait_ready(&tc);
        expect_command(&tc, 0x1000u, 2, ATA_CMD_READ_SECTORS);
        expect_sector_in_post(&tc, 0, 2);
        expect_sector_in(&tc, 0);
        finish(c, &tc);
    }

    /* --- the post-reset ready wait in the recovery is CHECKED ----------- *
     * ERR before data, then the drive comes back from the SRST only after
     * 20 s (BSY past the plain 10 s ceiling): the 31 s allowance applies and
     * the recovery completes, reporting the original -3. Then a drive that
     * NEVER comes back: the recovery must not report -3 as though the drive
     * were reset and ready for a retry — it reports -1 (never came ready).
     * Under the old code the wait's result was discarded. */
    {
        static const uint32_t seq[]  = { RDY, RDY | ERR, RDY | ERR, BSY, BSY, RDY };
        static const uint32_t usec[] = { 0, 0, 0, READY_US + 1u, 20000000u };
        mmio_mock_reset();
        mmio_mock_queue_read(ATA_ALT_STATUS_ADDR, seq, 6);
        mmio_mock_queue_read(USEC_TIMER_ADDR, usec, 5);
        mmio_mock_set_read(ATA_ERROR_ADDR, ERROR_ABRT);
        GUARDED(rc, ata_read_sectors(0x1000u, 2, g_buf));
        xpect(c, "recover: a 20 s post-SRST spin-up completes the recovery (-3)",
              rc == -3);
        tc = trace_begin("recover-slow-spinup");
        expect_wait_ready(&tc);
        expect_command(&tc, 0x1000u, 2, ATA_CMD_READ_SECTORS);
        expect_drq_wait(&tc, 0);                     /* sees ERR at once  */
        expect_r(&tc, 8, ATA_ERROR_ADDR);
        expect_r(&tc, 8, ATA_ALT_STATUS_ADDR);       /* drain: no DRQ     */
        expect_w(&tc, 8, ATA_CONTROL_ADDR, ATA_CONTROL_SRST | ATA_CONTROL_NIEN);
        expect_w(&tc, 8, ATA_CONTROL_ADDR, ATA_CONTROL_NIEN);
        expect_w(&tc, 8, ATA_SELECT_ADDR, ATA_SELECT_OBS);
        expect_timed_wait(&tc, 2);                   /* BSY 10 s+, 20 s, RDY */
        finish(c, &tc);
    }
    {
        static const uint32_t seq[]  = { RDY, RDY | ERR, RDY | ERR, BSY };
        static const uint32_t usec[] = { 0, 0, 0, SRST_READY_US + 1u };
        mmio_mock_reset();
        mmio_mock_queue_read(ATA_ALT_STATUS_ADDR, seq, 4);
        mmio_mock_queue_read(USEC_TIMER_ADDR, usec, 4);
        mmio_mock_set_read(ATA_ERROR_ADDR, ERROR_ABRT);
        GUARDED(rc, ata_read_sectors(0x1000u, 2, g_buf));
        xpect(c, "recover: a drive that never returns from SRST does not hang",
              rc != 999);
        xpect(c, "recover: and reports -1 (never came ready), not the cause",
              rc == -1);
        tc = trace_begin("recover-srst-timeout");
        expect_wait_ready(&tc);
        expect_command(&tc, 0x1000u, 2, ATA_CMD_READ_SECTORS);
        expect_drq_wait(&tc, 0);
        expect_r(&tc, 8, ATA_ERROR_ADDR);
        expect_r(&tc, 8, ATA_ALT_STATUS_ADDR);
        expect_w(&tc, 8, ATA_CONTROL_ADDR, ATA_CONTROL_SRST | ATA_CONTROL_NIEN);
        expect_w(&tc, 8, ATA_CONTROL_ADDR, ATA_CONTROL_NIEN);
        expect_w(&tc, 8, ATA_SELECT_ADDR, ATA_SELECT_OBS);
        expect_timed_timeout(&tc, 1);                /* BSY, then >31 s   */
        finish(c, &tc);
    }
}

/* ======================================================================= */
/*  power: standby / wakeup / parked                                       */
/* ======================================================================= */

static void test_power(xfail_ctx *c)
{
    xpect(c, "power: drive starts out not parked", ata_is_parked() == 0);

    /* --- STANDBY IMMEDIATE: grammar and the parked flag ----------------- */
    mmio_mock_reset();
    mmio_mock_set_read(ATA_ALT_STATUS_ADDR, RDY);
    int rc = ata_standby();
    xpect(c, "standby: returns 0", rc == 0);
    xpect(c, "standby: reports the drive parked", ata_is_parked() == 1);
    trace_cursor tc = trace_begin("standby-grammar");
    expect_wait_ready(&tc);
    expect_w(&tc, 8, ATA_SELECT_ADDR, ATA_SELECT_OBS);
    expect_w(&tc, 8, ATA_COMMAND_ADDR, CMD_STANDBY_IMM);
    expect_timed_wait(&tc, 0);                    /* accepted (BSY clear) */
    finish(c, &tc);

    /* --- STANDBY holds BSY for seconds while the drive flushes and parks -- *
     * The real failure: the drive keeps BSY up for 2-3 s after accepting
     * STANDBY IMMEDIATE, the old 1<<20-poll wait (well under a second)
     * returned -1, g_ata_parked stayed 0, and the main loop re-issued
     * STANDBY every pass. Timer: 0, then past the 8 s DATA-phase deadline
     * (this is the 10 s ready ceiling, so it must keep polling), 9 s, then
     * BSY clears: 0 and parked. */
    {
        static const uint32_t seq[]  = { RDY, BSY, BSY, RDY };
        static const uint32_t usec[] = { 0, 0, SPINUP_US + 1u, 9000000u };
        mmio_mock_reset();
        mmio_mock_queue_read(ATA_ALT_STATUS_ADDR, seq, 4);
        mmio_mock_queue_read(USEC_TIMER_ADDR, usec, 4);
        GUARDED(rc, ata_standby());
        xpect(c, "standby: BSY for 9 s while parking still returns 0", rc == 0);
        xpect(c, "standby: and reports the drive parked", ata_is_parked() == 1);
        tc = trace_begin("standby-slow-park");
        expect_wait_ready(&tc);
        expect_w(&tc, 8, ATA_SELECT_ADDR, ATA_SELECT_OBS);
        expect_w(&tc, 8, ATA_COMMAND_ADDR, CMD_STANDBY_IMM);
        expect_timed_wait(&tc, 2);
        finish(c, &tc);
    }

    /* --- wake: a whole-physical-sector read at LBA 0 through a spin-up -- *
     * The drive reports ready while spun down; the READ is what spins it up,
     * and the DRQ wait sees BSY for the duration. Three BSY polls model that;
     * the timer reads 0 so the (4 s) deadline stays out of the picture. The
     * probe must be count=2 — a count=1 probe IDNFs on this drive and left
     * the parked flag stuck at 1 for the rest of the session. */
    {
        static const uint32_t seq[] = { RDY, BSY, BSY, BSY, RDY | DRQ };
        mmio_mock_reset();
        mmio_mock_queue_read(ATA_ALT_STATUS_ADDR, seq, 5);
        rc = ata_wakeup();
        xpect(c, "wakeup: returns 0 through a spin-up", rc == 0);
        xpect(c, "wakeup: reports the drive no longer parked",
              ata_is_parked() == 0);
        tc = trace_begin("wakeup-grammar");
        expect_wait_ready(&tc);
        expect_command(&tc, 0, ATA_PHYS_LOG, ATA_CMD_READ_SECTORS);
        expect_sector_in(&tc, 3);
        expect_sector_in(&tc, 0);
        finish(c, &tc);
    }

    /* --- and a normal read afterwards works ------------------------------ */
    mmio_mock_reset();
    mmio_mock_set_read(ATA_ALT_STATUS_ADDR, RDY | DRQ);
    rc = ata_read_sectors(0x1000u, 2, g_buf);
    xpect(c, "wakeup: a normal read after wake returns 0", rc == 0);
    xpect(c, "wakeup: still not parked after it", ata_is_parked() == 0);

    /* --- a plain read also clears parked (no explicit wakeup needed) ---- */
    mmio_mock_reset();
    mmio_mock_set_read(ATA_ALT_STATUS_ADDR, RDY | DRQ);
    xpect(c, "parked: standby parks", ata_standby() == 0 && ata_is_parked());
    rc = ata_read_sectors(0x1000u, 2, g_buf);
    xpect(c, "parked: any READ clears it", rc == 0 && ata_is_parked() == 0);

    /* --- a FAILED wake still reconciles parked to 0 --------------------- *
     * The read either spun the platters up or left the drive in a state we
     * cannot characterise; either way "parked" would be a lie that suppresses
     * every later spin-up pre-payment. */
    mmio_mock_reset();
    mmio_mock_set_read(ATA_ALT_STATUS_ADDR, RDY);
    xpect(c, "parked: standby parks again", ata_standby() == 0 && ata_is_parked());
    mmio_mock_reset();
    mmio_mock_set_read(ATA_ALT_STATUS_ADDR, RDY | ERR);
    mmio_mock_set_read(ATA_ERROR_ADDR, ERROR_ABRT);
    rc = ata_wakeup();
    xpect(c, "wakeup: a drive error surfaces as -3", rc == -3);
    xpect(c, "wakeup: parked is 0 even though the wake failed",
          ata_is_parked() == 0);
    xpect(c, "wakeup: the failed probe ran the recovery",
          mmio_mock_count(MMIO_OP_READ, ATA_ERROR_ADDR) == 1 &&
          count_writes(ATA_CONTROL_ADDR) == 2);

    /* --- a wedged drive cannot be parked, and does not hang ------------- */
    {
        static const uint32_t usec[] = { 0, 0, READY_US, READY_US + 1u };
        mmio_mock_reset();
        mmio_mock_set_read(ATA_ALT_STATUS_ADDR, BSY);
        mmio_mock_queue_read(USEC_TIMER_ADDR, usec, 4);
        GUARDED(rc, ata_standby());
        xpect(c, "standby: a permanently-BSY drive does not hang", rc != 999);
        xpect(c, "standby: a permanently-BSY drive returns -1", rc == -1);
        xpect(c, "standby: and is not reported parked", ata_is_parked() == 0);
        xpect(c, "standby: no STANDBY was issued to it",
              count_writes(ATA_COMMAND_ADDR) == 0);
        tc = trace_begin("standby-busy-forever");
        expect_timed_timeout(&tc, 3);   /* 0, ==10 s, >10 s */
        finish(c, &tc);
    }
}

/* ======================================================================= */
/*  write                                                                  */
/* ======================================================================= */

static void fill_pattern(void)
{
    for (size_t i = 0; i < 2 * 256; i++) {
        g_pattern[i] = (uint16_t)(0x1234u + i * 7u);
    }
}

static void test_write(xfail_ctx *c)
{
    fill_pattern();

    /* --- argument rejection happens before any bus access --------------- */
    mmio_mock_reset();
    mmio_mock_set_read(ATA_ALT_STATUS_ADDR, RDY | DRQ);
    xpect(c, "write: odd LBA rejected", ata_write_sectors(0x1001u, 2, g_pattern) == -1);
    xpect(c, "write: odd count rejected", ata_write_sectors(0x1000u, 1, g_pattern) == -1);
    xpect(c, "write: zero count rejected", ata_write_sectors(0x1000u, 0, g_pattern) == -1);
    xpect(c, "write: odd buffer rejected",
          ata_write_sectors(0x1000u, 2, (const uint8_t *)g_pattern + 1) == -1);
    xpect(c, "write: none of those touched the bus", mmio_mock_log_len() == 0);

    /* --- the full happy path, every data word checked ------------------- */
    mmio_mock_reset();
    mmio_mock_set_read(ATA_ALT_STATUS_ADDR, RDY | DRQ);
    int rc = ata_write_sectors(0x01234566u, 2, g_pattern);
    xpect(c, "write: 2 aligned sectors return 0", rc == 0);
    trace_cursor tc = trace_begin("write-grammar");
    expect_wait_ready(&tc);
    expect_command(&tc, 0x01234566u, 2, CMD_WRITE_SECTORS);
    expect_sector_out(&tc, g_pattern);
    expect_sector_out(&tc, g_pattern + 256);
    expect_r(&tc, 32, USEC_TIMER_ADDR);           /* completion: BSY wait */
    expect_r(&tc, 8, ATA_ALT_STATUS_ADDR);
    expect_r(&tc, 8, ATA_ALT_STATUS_ADDR);        /* final ERR/DF check   */
    expect_flush(&tc);
    finish(c, &tc);

    /* --- ERR at the DRQ wait: must recover, must not flush -------------- *
     * The drive rejected WRITE SECTORS. The first version of ata_write_raw
     * returned -3 right here with the drive still mid-command; the player's
     * next refill read would then have been issued on top of it. */
    mmio_mock_reset();
    mmio_mock_set_read(ATA_ALT_STATUS_ADDR, RDY | ERR);
    mmio_mock_set_read(ATA_ERROR_ADDR, ERROR_ABRT);
    rc = ata_write_sectors(0x1000u, 2, g_pattern);
    xpect(c, "write: ERR at the DRQ wait returns -3", rc == -3);
    xpect(c, "write: no FLUSH CACHE after a failed transfer",
          count_writes(ATA_COMMAND_ADDR) == 1);
    tc = trace_begin("write-err-before-data");
    expect_wait_ready(&tc);
    expect_command(&tc, 0x1000u, 2, CMD_WRITE_SECTORS);
    expect_drq_wait(&tc, 0);
    expect_recovery(&tc, 0);
    finish(c, &tc);

    /* --- IDNF on a write surfaces as IDNF, exactly like a read ---------- */
    mmio_mock_reset();
    mmio_mock_set_read(ATA_ALT_STATUS_ADDR, RDY | ERR);
    mmio_mock_set_read(ATA_ERROR_ADDR, ATA_ERROR_IDNF);
    rc = ata_write_sectors(0x1000u, 2, g_pattern);
    xpect(c, "write: IDNF latched returns ATA_ERR_IDNF", rc == ATA_ERR_IDNF);

    /* --- ERR after sector 0's data --------------------------------------- */
    {
        static const uint32_t seq[] = {
            RDY,                /* wait ready              */
            RDY | DRQ,          /* sector 0 DRQ            */
            RDY | ERR,          /* post-sector status: ERR */
            RDY,                /* drain check, wait ready */
        };
        mmio_mock_reset();
        mmio_mock_queue_read(ATA_ALT_STATUS_ADDR, seq, 4);
        mmio_mock_set_read(ATA_ERROR_ADDR, ERROR_ABRT);
        rc = ata_write_sectors(0x1000u, 2, g_pattern);
        xpect(c, "write: ERR after sector 0 returns -3", rc == -3);
        tc = trace_begin("write-err-after-data");
        expect_wait_ready(&tc);
        expect_command(&tc, 0x1000u, 2, CMD_WRITE_SECTORS);
        expect_sector_out(&tc, g_pattern);
        expect_recovery(&tc, 0);
        finish(c, &tc);
    }

    /* --- completion BSY that never clears: timed out, then recovered ---- *
     * Both sectors go out, then the drive holds BSY while "committing" and
     * never lets go. Timer: the ready wait's start, two per-sector DRQ starts
     * and two post-sector starts, the completion start, one in-deadline poll,
     * one past it. */
    {
        static const uint32_t seq[] = {
            RDY,
            RDY | DRQ, RDY | DRQ,       /* sector 0: DRQ, post-status */
            RDY | DRQ, RDY | DRQ,       /* sector 1                   */
            BSY, BSY,                   /* committing... forever      */
            RDY,                        /* recovery's drain check +   *
                                         * wait ready after the reset */
        };
        static const uint32_t usec[] = { 0, 0, 0, 0, 0, 0, 0, SPINUP_US + 1u };
        mmio_mock_reset();
        mmio_mock_queue_read(ATA_ALT_STATUS_ADDR, seq, 8);
        mmio_mock_queue_read(USEC_TIMER_ADDR, usec, 8);
        GUARDED(rc, ata_write_sectors(0x1000u, 2, g_pattern));
        xpect(c, "write: a completion that never comes does not hang", rc != 999);
        xpect(c, "write: a completion that never comes returns -2", rc == -2);
        tc = trace_begin("write-completion-timeout");
        expect_wait_ready(&tc);
        expect_command(&tc, 0x1000u, 2, CMD_WRITE_SECTORS);
        expect_sector_out(&tc, g_pattern);
        expect_sector_out(&tc, g_pattern + 256);
        expect_r(&tc, 32, USEC_TIMER_ADDR);       /* t0                 */
        expect_r(&tc, 8, ATA_ALT_STATUS_ADDR);    /* BSY                */
        expect_r(&tc, 32, USEC_TIMER_ADDR);       /* 0: keep waiting    */
        expect_r(&tc, 8, ATA_ALT_STATUS_ADDR);    /* BSY                */
        expect_r(&tc, 32, USEC_TIMER_ADDR);       /* past the deadline  */
        expect_recovery(&tc, 0);
        finish(c, &tc);
    }

    /* --- the transfer lands but FLUSH CACHE reports an error ------------ *
     * The data may or may not be on the platter; what is certain is that the
     * drive is not in a state to take the next command unreset. */
    {
        static const uint32_t seq[] = {
            RDY,
            RDY | DRQ, RDY,             /* sector 0                    */
            RDY | DRQ, RDY,             /* sector 1                    */
            RDY, RDY,                   /* completion: !BSY, no ERR    */
            RDY,                        /* flush: wait ready           */
            RDY,                        /* flush: !BSY                 */
            RDY | ERR,                  /* flush: status carries ERR   */
            RDY,                        /* drain check, wait ready     */
        };
        mmio_mock_reset();
        mmio_mock_queue_read(ATA_ALT_STATUS_ADDR, seq, 11);
        mmio_mock_set_read(ATA_ERROR_ADDR, ERROR_ABRT);
        rc = ata_write_sectors(0x1000u, 2, g_pattern);
        xpect(c, "write: a failed FLUSH CACHE returns -3", rc == -3);
        tc = trace_begin("write-flush-error");
        expect_wait_ready(&tc);
        expect_command(&tc, 0x1000u, 2, CMD_WRITE_SECTORS);
        expect_sector_out(&tc, g_pattern);
        expect_sector_out(&tc, g_pattern + 256);
        expect_r(&tc, 32, USEC_TIMER_ADDR);
        expect_r(&tc, 8, ATA_ALT_STATUS_ADDR);
        expect_r(&tc, 8, ATA_ALT_STATUS_ADDR);
        expect_flush(&tc);
        expect_recovery(&tc, 0);
        finish(c, &tc);
    }
}

/* ======================================================================= */
/*  lba28: the address-space guard                                         */
/* ======================================================================= */

static void test_lba28(xfail_ctx *c)
{
    const uint32_t top = 1u << 28;

    /* Past the end: rejected before a single register is touched. Without
     * the guard SELECT gets 0xE0 | ((1<<28 >> 24) & 0xF) == 0xE0 — sector 0,
     * reported as success. */
    mmio_mock_reset();
    mmio_mock_set_read(ATA_ALT_STATUS_ADDR, RDY | DRQ);
    int rc = ata_read_sectors(top, 2, g_buf);
    xpect(c, "lba28: read at 2^28 is rejected", rc == -1);
    xpect(c, "lba28: and issued nothing", mmio_mock_log_len() == 0);

    rc = ata_read_sectors(top - 2, 4, g_buf);
    xpect(c, "lba28: read crossing 2^28 is rejected", rc == -1);
    xpect(c, "lba28: and issued nothing", mmio_mock_log_len() == 0);

    rc = ata_write_sectors(top, 2, g_pattern);
    xpect(c, "lba28: write at 2^28 is rejected", rc == -1);
    xpect(c, "lba28: and issued nothing", mmio_mock_log_len() == 0);

    rc = ata_write_sectors(top - 2, 4, g_pattern);
    xpect(c, "lba28: write crossing 2^28 is rejected", rc == -1);
    xpect(c, "lba28: and issued nothing", mmio_mock_log_len() == 0);

    rc = ata_read_sectors(0xFFFFFFFFu, 2, g_buf);
    xpect(c, "lba28: read at UINT32_MAX (would wrap lba+count) is rejected",
          rc == -1);
    xpect(c, "lba28: and issued nothing", mmio_mock_log_len() == 0);

    /* The last addressable physical sector is fine, and carries the full
     * high nibble — the guard must not be off by one in the strict direction
     * either. */
    mmio_mock_reset();
    mmio_mock_set_read(ATA_ALT_STATUS_ADDR, RDY | DRQ);
    rc = ata_read_sectors(top - 2, 2, g_buf);
    xpect(c, "lba28: the last physical sector reads", rc == 0);
    xpect(c, "lba28: SELECT carries the full high nibble",
          nth_write(ATA_SELECT_ADDR, 0) == (ATA_SELECT_OBS | ATA_SELECT_LBA | 0xFu));
    xpect(c, "lba28: HCYL/LCYL/SECTOR are all 0xFF/0xFF/0xFE",
          nth_write(ATA_HCYL_ADDR, 0) == 0xFF &&
          nth_write(ATA_LCYL_ADDR, 0) == 0xFF &&
          nth_write(ATA_SECTOR_ADDR, 0) == 0xFE);

    /* The very last LOGICAL sector goes through the bounce at top-2. */
    mmio_mock_reset();
    mmio_mock_set_read(ATA_ALT_STATUS_ADDR, RDY | DRQ);
    rc = ata_read_sectors(top - 1, 1, g_buf);
    xpect(c, "lba28: the last logical sector reads via the bounce", rc == 0);
    xpect(c, "lba28: bounced to the last physical boundary",
          nth_write(ATA_SECTOR_ADDR, 0) == 0xFE);

    mmio_mock_reset();
    mmio_mock_set_read(ATA_ALT_STATUS_ADDR, RDY | DRQ);
    rc = ata_write_sectors(top - 2, 2, g_pattern);
    xpect(c, "lba28: the last physical sector writes", rc == 0);
}

int main(void)
{
    xfail_ctx c = { "hw-ata", 0, 0, 0 };
    test_init(&c);
    test_read(&c);
    test_power(&c);
    test_write(&c);
    test_lba28(&c);
    return xfail_done(&c);
}
