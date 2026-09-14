/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/cfg_commit.c — the settings-save commit gate. See cfg_commit.h
 * for the policy; this is its mechanics, kept free of hardware so the host
 * test can drive every branch with a hand-advanced clock.
 */

#include "cfg_commit.h"

void cfg_commit_touch(cfg_commit_t *c, uint32_t now_us)
{
    c->dirty    = 1;
    c->dirty_us = now_us;
    c->failures = 0;          /* a new change gets its own attempts */
}

void cfg_commit_clear(cfg_commit_t *c)
{
    c->dirty    = 0;
    c->failures = 0;
}

int cfg_commit_gate(cfg_commit_t *c, int mode, const cfg_commit_env_t *env)
{
    if (!c->dirty) {
        return CFG_GATE_NOTHING;
    }
    if (mode == CFG_COMMIT_IDLE) {
        /* Unsigned difference: the clock wraps, and a change made just
         * before the wrap must still be "recent" just after it. */
        if ((uint32_t)(env->now_us - c->dirty_us) < CFG_SAVE_DEBOUNCE_US) {
            return CFG_GATE_NOTHING;
        }
    }
    if (mode == CFG_COMMIT_IDLE || mode == CFG_COMMIT_SOFT) {
        /*
         * NEVER spin the platters up just to save 1 KB. A spin-up is 1-3
         * seconds of audible, blocking wait for something with no deadline —
         * whether or not a player is running (it used to be deferred only
         * under a live player, which is how leaving Settings with the drive
         * parked and nothing playing became a multi-second stall, and how a
         * change deferred at a Settings exit could ambush the user with a
         * spin-up three seconds later, mid-browse, on the idle debounce).
         *
         * The change stays pending — dirty is untouched — and lands the next
         * time the platters turn for any other reason (this idle path runs
         * every pass and writes as soon as !parked), or at the next forced
         * commit: suspend, power-off, disk mode, the DISKSAFE flush. The
         * trade-off: a change made while the drive is parked is lost only on
         * a HARD power cut before any of those happens, and the suspend
         * timeout forces a commit within 30 minutes of going idle.
         */
        if (env->parked) {
            return CFG_GATE_NOTHING;
        }
    }
    /*
     * Refuse the write when the cell is too low to guarantee finishing it —
     * a cut mid-sector is how a config record gets torn. Deliberately BEFORE
     * anything clears the pending flag: the change lands on the next commit
     * if the charger goes in and the policy recovers. Refusing to write is
     * not the same as discarding the edit. The DISKSAFE flush (CFG_COMMIT_LAST)
     * is exempt: see the header for why that is not optional.
     *
     * Reported ONCE per refusal, not once per pass: the caller's log line is
     * milliseconds of blocking UART, and this runs from a main loop that
     * spins thousands of times a second while playing.
     */
    if (mode != CFG_COMMIT_LAST && !env->battery_ok) {
        if (c->deferred_logged) {
            return CFG_GATE_DEFER_QUIET;
        }
        c->deferred_logged = 1;
        return CFG_GATE_DEFER_LOG;
    }
    c->deferred_logged = 0;

    if (!env->writable) {
        /* No CORECFG.DAT: nothing can persist the change, ever. Drop it so
         * the main loop does not re-decide this every pass. */
        c->dirty    = 0;
        c->failures = 0;
        return CFG_GATE_NOTHING;
    }
    /* The pending flag stays SET across the write: only a reported success
     * clears it (cfg_commit_result). A parked drive is woken first, so the
     * spin-up is paid on the read path built to wait for it, not inside the
     * write's DRQ budget. */
    return env->parked ? CFG_GATE_WRITE_WAKE : CFG_GATE_WRITE;
}

int cfg_commit_result(cfg_commit_t *c, int rc, uint32_t now_us)
{
    if (rc == 0) {
        c->dirty    = 0;
        c->failures = 0;
        return 0;
    }
    c->failures++;
    if (c->failures >= CFG_SAVE_MAX_FAILURES) {
        /* Enough. The previous good record is still intact on disk; this
         * session's change is what is lost, and that beats hammering a
         * drive that has refused three times in a row. */
        c->dirty    = 0;
        c->failures = 0;
        return 0;
    }
    /* Still pending — but not for the very next pass: re-stamp so an idle
     * retry waits a debounce, the same spacing a fresh change gets. */
    c->dirty    = 1;
    c->dirty_us = now_us;
    return 1;
}
