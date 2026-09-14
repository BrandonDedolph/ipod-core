/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/kernel/cfg_commit_test.c — host tests for the settings-save commit
 * gate (kernel/cfg_commit.c), the policy that decides when a pending change
 * to the settings becomes the firmware's one disk write, and what becomes of
 * it when that write fails.
 *
 * Until the gate was extracted it lived inside settings_commit() in
 * kernel/main.c, reading the clock, the drive, the player and the battery
 * directly, and nothing could exercise it. Two things it did wrong were found
 * by reading, not by a test: the forced commit at suspend/power-off wrote
 * straight into a PARKED drive, so the spin-up happened inside the write's
 * DRQ budget and a timeout there soft-reset the channel mid-spin-up; and the
 * dirty flag was cleared BEFORE the write, so that failure — or any other —
 * discarded the change (the resume position captured a line earlier
 * included) with nothing left to retry.
 *
 * Every case is an exact (mode, environment, clock) triple against the
 * verdict, and the pending flag is asserted after every step.
 *
 * 2026-09-14 added CFG_COMMIT_SOFT and tightened the parked rule: NO
 * non-forced mode wakes the platters any more. Leaving Settings forced a
 * commit, resume_capture() had just marked the record dirty, and the spin-up
 * landed before the pop was rendered — the stall the user felt on every
 * back-out.
 */

#include <stdio.h>
#include <string.h>

#include "../../kernel/cfg_commit.h"

static int g_fails;

static void check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    if (!cond) {
        g_fails++;
    }
}

/* A benign world: drive spinning, player idle, battery fine, file present. */
static cfg_commit_env_t env_at(uint32_t now_us)
{
    cfg_commit_env_t e;
    memset(&e, 0, sizeof e);
    e.now_us     = now_us;
    e.battery_ok = 1;
    e.writable   = 1;
    return e;
}

#define T0        1000000u
#define AFTER     (T0 + CFG_SAVE_DEBOUNCE_US)       /* the debounce has run */
#define BEFORE    (T0 + CFG_SAVE_DEBOUNCE_US - 1u)  /* one microsecond short */

/* ---- 1. nothing pending ------------------------------------------------- */

static void test_idle_nothing(void)
{
    cfg_commit_t c;
    memset(&c, 0, sizeof c);
    cfg_commit_env_t e = env_at(AFTER);
    check("nothing pending: idle says NOTHING",
          cfg_commit_gate(&c, CFG_COMMIT_IDLE, &e) == CFG_GATE_NOTHING);
    check("nothing pending: force says NOTHING",
          cfg_commit_gate(&c, CFG_COMMIT_FORCE, &e) == CFG_GATE_NOTHING);
    check("nothing pending: last says NOTHING",
          cfg_commit_gate(&c, CFG_COMMIT_LAST, &e) == CFG_GATE_NOTHING);

    cfg_commit_touch(&c, T0);
    check("touch marks the change pending", c.dirty == 1 && c.dirty_us == T0);
    cfg_commit_clear(&c);
    check("clear (a load is not a change) leaves nothing pending",
          c.dirty == 0 &&
          cfg_commit_gate(&c, CFG_COMMIT_FORCE, &e) == CFG_GATE_NOTHING);
}

/* ---- 2. the debounce ---------------------------------------------------- */

static void test_debounce(void)
{
    cfg_commit_t c;
    memset(&c, 0, sizeof c);
    cfg_commit_touch(&c, T0);

    cfg_commit_env_t e = env_at(BEFORE);
    check("idle inside the debounce: NOTHING, still pending",
          cfg_commit_gate(&c, CFG_COMMIT_IDLE, &e) == CFG_GATE_NOTHING && c.dirty == 1);
    e = env_at(AFTER);
    check("idle at the debounce: WRITE",
          cfg_commit_gate(&c, CFG_COMMIT_IDLE, &e) == CFG_GATE_WRITE);
    check("a WRITE verdict does NOT clear the pending flag",
          c.dirty == 1);

    /* A second change inside the window restarts it: one write per
     * settling, not one per tick. */
    cfg_commit_touch(&c, BEFORE);
    e = env_at(AFTER);
    check("a fresh change restarts the debounce",
          cfg_commit_gate(&c, CFG_COMMIT_IDLE, &e) == CFG_GATE_NOTHING);
    e = env_at(BEFORE + CFG_SAVE_DEBOUNCE_US);
    check("...and the write comes a full debounce after IT",
          cfg_commit_gate(&c, CFG_COMMIT_IDLE, &e) == CFG_GATE_WRITE);

    /* Forced modes skip the debounce entirely. */
    cfg_commit_touch(&c, T0);
    e = env_at(T0 + 1u);
    check("force skips the debounce",
          cfg_commit_gate(&c, CFG_COMMIT_FORCE, &e) == CFG_GATE_WRITE);
    check("last skips the debounce",
          cfg_commit_gate(&c, CFG_COMMIT_LAST, &e) == CFG_GATE_WRITE);

    /* The clock wraps: a change made just before the wrap is recent just
     * after it, and old a debounce later. */
    cfg_commit_touch(&c, 0xFFFFFFFFu - 10u);
    e = env_at(5u);
    check("debounce across the clock wrap: still recent",
          cfg_commit_gate(&c, CFG_COMMIT_IDLE, &e) == CFG_GATE_NOTHING);
    e = env_at(CFG_SAVE_DEBOUNCE_US - 11u);
    check("debounce across the clock wrap: due exactly a debounce later",
          cfg_commit_gate(&c, CFG_COMMIT_IDLE, &e) == CFG_GATE_WRITE);
}

/* ---- 3. the parked drive ----------------------------------------------- */

static void test_parked(void)
{
    cfg_commit_t c;
    memset(&c, 0, sizeof c);
    cfg_commit_touch(&c, T0);

    /* Idle, parked, audio playing out of the buffer: wait. */
    cfg_commit_env_t e = env_at(AFTER);
    e.parked = 1; e.player_active = 1;
    check("idle + parked + player active: NOTHING (no spin-up for 1 KB)",
          cfg_commit_gate(&c, CFG_COMMIT_IDLE, &e) == CFG_GATE_NOTHING && c.dirty == 1);

    /* Idle, parked, NOTHING PLAYING: still no spin-up. This used to say
     * WRITE_WAKE, which is how a change deferred at a Settings exit could
     * ambush the user with a 1-3 s spin-up three seconds later, mid-browse.
     * Only a FORCE wakes the platters now. */
    e.player_active = 0;
    check("idle + parked + player idle: NOTHING, still pending",
          cfg_commit_gate(&c, CFG_COMMIT_IDLE, &e) == CFG_GATE_NOTHING && c.dirty == 1);

    /* Drive spinning, the debounce run: the deferred change lands. */
    e.parked = 0;
    check("idle + spinning after the debounce: WRITE",
          cfg_commit_gate(&c, CFG_COMMIT_IDLE, &e) == CFG_GATE_WRITE && c.dirty == 1);
    e.parked = 1;

    /* THE BUG: the forced commit at suspend/power-off skipped the parked
     * check and wrote straight into the parked drive. It must still write
     * (that is the point of force) — after a wake. */
    e.player_active = 1;
    check("force + parked + player active: WRITE_WAKE, never a bare WRITE",
          cfg_commit_gate(&c, CFG_COMMIT_FORCE, &e) == CFG_GATE_WRITE_WAKE);
    check("last + parked: WRITE_WAKE",
          cfg_commit_gate(&c, CFG_COMMIT_LAST, &e) == CFG_GATE_WRITE_WAKE);

    /* Drive spinning: a plain WRITE, no wake. */
    e.parked = 0;
    check("force + spinning: WRITE",
          cfg_commit_gate(&c, CFG_COMMIT_FORCE, &e) == CFG_GATE_WRITE);
    check("still pending after every verdict above", c.dirty == 1);
}

/* ---- 3b. CFG_COMMIT_SOFT ------------------------------------------------ */

/*
 * SOFT is "now, if it is free": the debounce is skipped like FORCE, but a
 * parked drive is never woken. Leaving the Settings root uses it — a FORCE
 * there paid a 1-3 s ata_wakeup() before the pop was even rendered, because
 * resume_capture() marks the record dirty on the way out whenever a track is
 * loaded. The battery gate and the writable check still apply.
 */
static void test_soft(void)
{
    cfg_commit_t c;
    memset(&c, 0, sizeof c);
    cfg_commit_touch(&c, T0);

    /* Parked: nothing happens, and the change is untouched. */
    cfg_commit_env_t e = env_at(T0 + 1u);
    e.parked = 1;
    check("soft + parked: NOTHING (never a WRITE_WAKE)",
          cfg_commit_gate(&c, CFG_COMMIT_SOFT, &e) == CFG_GATE_NOTHING);
    check("soft + parked: the change stays pending", c.dirty == 1);
    e.player_active = 1;
    check("soft + parked + player active: NOTHING too",
          cfg_commit_gate(&c, CFG_COMMIT_SOFT, &e) == CFG_GATE_NOTHING && c.dirty == 1);

    /* Spinning: write immediately — no debounce wait (the change was made
     * one microsecond ago). */
    e.parked = 0;
    check("soft + spinning: WRITE, without waiting out the debounce",
          cfg_commit_gate(&c, CFG_COMMIT_SOFT, &e) == CFG_GATE_WRITE);
    check("soft: a WRITE verdict does not clear the pending flag", c.dirty == 1);

    /* Still gated by the battery, like FORCE. */
    e.battery_ok = 0;
    check("soft below disk-safe: refused (DEFER_LOG), change kept",
          cfg_commit_gate(&c, CFG_COMMIT_SOFT, &e) == CFG_GATE_DEFER_LOG && c.dirty == 1);
    e.battery_ok = 1;

    /* And by the writable check, like FORCE: no file, drop it. */
    e.writable = 0;
    check("soft with no CORECFG.DAT: NOTHING, and the change is dropped",
          cfg_commit_gate(&c, CFG_COMMIT_SOFT, &e) == CFG_GATE_NOTHING && c.dirty == 0);

    /* A parked SOFT leaves the change for the idle path, which writes it as
     * soon as the platters are turning again — the deferral is not a loss. */
    memset(&c, 0, sizeof c);
    cfg_commit_touch(&c, T0);
    e = env_at(T0 + 1u);
    e.parked = 1;
    check("soft + parked, then the drive spins up: idle writes it",
          cfg_commit_gate(&c, CFG_COMMIT_SOFT, &e) == CFG_GATE_NOTHING);
    e = env_at(AFTER);
    e.parked = 0;
    check("...on the next idle pass past the debounce",
          cfg_commit_gate(&c, CFG_COMMIT_IDLE, &e) == CFG_GATE_WRITE && c.dirty == 1);
}

/* ---- 4. the battery gate ----------------------------------------------- */

static void test_battery(void)
{
    cfg_commit_t c;
    memset(&c, 0, sizeof c);
    cfg_commit_touch(&c, T0);

    cfg_commit_env_t e = env_at(AFTER);
    e.battery_ok = 0;
    check("idle below disk-safe: DEFER_LOG the first time",
          cfg_commit_gate(&c, CFG_COMMIT_IDLE, &e) == CFG_GATE_DEFER_LOG);
    check("idle below disk-safe: DEFER_QUIET after that",
          cfg_commit_gate(&c, CFG_COMMIT_IDLE, &e) == CFG_GATE_DEFER_QUIET &&
          cfg_commit_gate(&c, CFG_COMMIT_IDLE, &e) == CFG_GATE_DEFER_QUIET);
    check("force below disk-safe: refused too (quiet, already logged)",
          cfg_commit_gate(&c, CFG_COMMIT_FORCE, &e) == CFG_GATE_DEFER_QUIET);
    check("a refused change is still pending", c.dirty == 1);

    /* The exemption: the DISKSAFE flush goes through. */
    check("last below disk-safe: WRITE (the exemption)",
          cfg_commit_gate(&c, CFG_COMMIT_LAST, &e) == CFG_GATE_WRITE);

    /* The charger goes in: the pending change lands, and the log latch
     * resets so the NEXT refusal episode is reported again. */
    e.battery_ok = 1;
    check("battery recovers: the pending change is written",
          cfg_commit_gate(&c, CFG_COMMIT_IDLE, &e) == CFG_GATE_WRITE);
    e.battery_ok = 0;
    check("a new refusal episode is logged again",
          cfg_commit_gate(&c, CFG_COMMIT_IDLE, &e) == CFG_GATE_DEFER_LOG);
}

/* ---- 5. no file to write ------------------------------------------------ */

static void test_not_writable(void)
{
    cfg_commit_t c;
    memset(&c, 0, sizeof c);
    cfg_commit_touch(&c, T0);

    cfg_commit_env_t e = env_at(AFTER);
    e.writable = 0;
    check("no CORECFG.DAT: NOTHING, and the change is dropped",
          cfg_commit_gate(&c, CFG_COMMIT_FORCE, &e) == CFG_GATE_NOTHING && c.dirty == 0);
    e.writable = 1;
    check("...so a later pass has nothing to do",
          cfg_commit_gate(&c, CFG_COMMIT_FORCE, &e) == CFG_GATE_NOTHING);
}

/* ---- 6. the write's result --------------------------------------------- */

static void test_result(void)
{
    cfg_commit_t c;
    memset(&c, 0, sizeof c);
    cfg_commit_touch(&c, T0);
    cfg_commit_env_t e = env_at(AFTER);
    check("result setup: WRITE",
          cfg_commit_gate(&c, CFG_COMMIT_FORCE, &e) == CFG_GATE_WRITE);

    /* Success: nothing pending. */
    check("rc 0: nothing pending",
          cfg_commit_result(&c, 0, AFTER + 1u) == 0 && c.dirty == 0);

    /* THE BUG: a failed write (a timeout, -2, is what a spin-up overrun
     * returns) used to discard the change. It must stay pending. */
    cfg_commit_touch(&c, T0);
    (void)cfg_commit_gate(&c, CFG_COMMIT_FORCE, &e);
    uint32_t t_fail = AFTER + 1u;
    check("rc -2: the change stays pending",
          cfg_commit_result(&c, -2, t_fail) == 1 && c.dirty == 1);
    /* ...but not for the very next pass: the retry waits a debounce from
     * the failure, so a sick drive is not hammered thousands of times a
     * second. */
    e = env_at(t_fail + 1u);
    check("retry: not on the next pass",
          cfg_commit_gate(&c, CFG_COMMIT_IDLE, &e) == CFG_GATE_NOTHING);
    e = env_at(t_fail + CFG_SAVE_DEBOUNCE_US);
    check("retry: one debounce after the failure",
          cfg_commit_gate(&c, CFG_COMMIT_IDLE, &e) == CFG_GATE_WRITE);
    check("retry: force retries immediately",
          cfg_commit_gate(&c, CFG_COMMIT_FORCE, &e) == CFG_GATE_WRITE);

    /* A success after a failure clears everything, including the count. */
    check("rc 0 after a failure: nothing pending",
          cfg_commit_result(&c, 0, e.now_us) == 0 && c.dirty == 0 && c.failures == 0);

    /* CFG_SAVE_MAX_FAILURES consecutive failures: give up on the change. */
    cfg_commit_touch(&c, T0);
    uint32_t now = AFTER;
    int pending = 1;
    for (uint32_t i = 1; i <= CFG_SAVE_MAX_FAILURES; i++) {
        e = env_at(now);
        int g = cfg_commit_gate(&c, CFG_COMMIT_IDLE, &e);
        if (g != CFG_GATE_WRITE) {
            pending = -1;
            break;
        }
        pending = cfg_commit_result(&c, -3, now);
        if (i < CFG_SAVE_MAX_FAILURES && pending != 1) {
            pending = -2;
            break;
        }
        now += CFG_SAVE_DEBOUNCE_US;
    }
    check("every failure short of the limit keeps the change pending, the last drops it",
          pending == 0 && c.dirty == 0);
    e = env_at(now);
    check("a dropped change is not retried",
          cfg_commit_gate(&c, CFG_COMMIT_FORCE, &e) == CFG_GATE_NOTHING);

    /* A NEW change after failures starts its own count. */
    cfg_commit_touch(&c, T0);
    (void)cfg_commit_gate(&c, CFG_COMMIT_FORCE, &e);
    (void)cfg_commit_result(&c, -2, now);
    (void)cfg_commit_gate(&c, CFG_COMMIT_FORCE, &e);
    (void)cfg_commit_result(&c, -2, now);
    cfg_commit_touch(&c, now);
    check("a new change resets the failure count", c.failures == 0 && c.dirty == 1);
    (void)cfg_commit_gate(&c, CFG_COMMIT_FORCE, &e);
    check("...so its first failure does not drop it",
          cfg_commit_result(&c, -2, now) == 1 && c.dirty == 1);
}

int main(void)
{
    test_idle_nothing();
    test_debounce();
    test_parked();
    test_soft();
    test_battery();
    test_not_writable();
    test_result();

    printf("cfg_commit_test: %s (%d failure%s)\n",
           g_fails == 0 ? "PASS" : "FAIL", g_fails, g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
