/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/wallclock.c — the software clock. See wallclock.h.
 */

#include "wallclock.h"

#define WALLCLOCK_SEC_US 1000000u

/*
 * Whole seconds in `frac_us + delta`, computed without ever forming a sum that
 * could overflow: `delta` may be most of a full 32-bit wrap, so the naive
 * (frac + delta) / 1e6 is wrong for large deltas. delta % 1e6 and frac_us are
 * both < 1e6, so their sum is < 2e6 and fits.
 */
static uint32_t whole_seconds(uint32_t frac_us, uint32_t delta)
{
    return delta / WALLCLOCK_SEC_US
         + (frac_us + delta % WALLCLOCK_SEC_US) / WALLCLOCK_SEC_US;
}

void wallclock_reset(wallclock_t *w)
{
    w->valid        = 0;
    w->epoch        = 0;
    w->anchor_epoch = 0;
    w->last_us      = 0;
    w->frac_us      = 0;
}

void wallclock_anchor(wallclock_t *w, int valid, uint32_t epoch,
                      uint32_t now_us)
{
    if (!valid) {
        wallclock_reset(w);
        return;
    }
    w->valid        = 1;
    w->epoch        = epoch;
    w->anchor_epoch = epoch;
    w->last_us      = now_us;
    /* The chip's sub-second phase is unknown (the second we just read is
     * somewhere between 0 and 1 s old), so the carry starts empty: the worst
     * case is being up to one second behind, which is inside the ±1 s the
     * read-back compare in hal_rtc_set already allows. */
    w->frac_us      = 0;
}

void wallclock_tick(wallclock_t *w, uint32_t now_us)
{
    if (!w->valid) {
        return;
    }
    /* Unsigned subtraction between CONSECUTIVE samples: the counter's wrap
     * cancels out. See the feed contract in the header for the one gap this
     * cannot survive. */
    uint32_t delta = (uint32_t)(now_us - w->last_us);
    w->last_us = now_us;

    uint32_t secs = whole_seconds(w->frac_us, delta);
    w->frac_us = (w->frac_us + delta % WALLCLOCK_SEC_US) % WALLCLOCK_SEC_US;
    w->epoch  += secs;
}

int wallclock_now(const wallclock_t *w, uint32_t now_us, uint32_t *epoch)
{
    if (!w->valid || epoch == 0) {
        return 0;
    }
    uint32_t delta = (uint32_t)(now_us - w->last_us);
    *epoch = w->epoch + whole_seconds(w->frac_us, delta);
    return 1;
}

int wallclock_minute(const wallclock_t *w, uint32_t now_us, uint32_t *minute)
{
    uint32_t now;
    if (!wallclock_now(w, now_us, &now) || minute == 0) {
        return 0;
    }
    *minute = now / 60u;
    return 1;
}

wallclock_resync_t wallclock_resync(wallclock_t *w, int rtc_valid,
                                    uint32_t rtc_epoch, uint32_t now_us,
                                    int32_t *drift_s)
{
    if (drift_s != 0) {
        *drift_s = 0;
    }

    if (!rtc_valid) {
        wallclock_reset(w);
        return WALLCLOCK_RESYNC_OK;
    }

    if (!w->valid) {
        wallclock_anchor(w, 1, rtc_epoch, now_us);   /* first anchor of a boot */
        return WALLCLOCK_RESYNC_OK;
    }

    uint32_t believed = w->epoch;
    (void)wallclock_now(w, now_us, &believed);
    int32_t drift = (int32_t)(rtc_epoch - believed);
    if (drift_s != 0) {
        *drift_s = drift;
    }

    /* A chip still reading the second it read at the last anchor, while the µs
     * timer says seconds have passed, is not a clock — it is the bus handing
     * back stale bytes. Adopting that reading would freeze the displayed time
     * at a plausible-looking value, which is worse than showing none. */
    if (rtc_epoch == w->anchor_epoch &&
        believed - w->anchor_epoch >= (uint32_t)WALLCLOCK_STOPPED_S) {
        wallclock_reset(w);
        return WALLCLOCK_RESYNC_STOPPED;
    }

    wallclock_anchor(w, 1, rtc_epoch, now_us);
    if (drift > WALLCLOCK_DRIFT_S || drift < -WALLCLOCK_DRIFT_S) {
        return WALLCLOCK_RESYNC_DRIFT;
    }
    return WALLCLOCK_RESYNC_OK;
}
