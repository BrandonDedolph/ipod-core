/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/sleeptimer.c — the sleep timer's countdown. See sleeptimer.h.
 */

#include "sleeptimer.h"

#define SLEEPTIMER_MIN_US 60000000u        /* one minute of the 1 MHz clock */

void sleeptimer_reset(sleeptimer_t *t)
{
    t->armed       = 0;
    t->total_min   = 0;
    t->elapsed_min = 0;
    t->last_us     = 0;
    t->acc_us      = 0;
}

void sleeptimer_arm(sleeptimer_t *t, int minutes, uint32_t now_us)
{
    sleeptimer_reset(t);
    /* total_min is a byte on the device; a duration that would not fit is a
     * caller bug, and disarming is the safe reading of it (the row's value
     * comes from a fixed table that tops out at 120). */
    if (minutes <= 0 || minutes > 255) {
        return;
    }
    t->armed     = 1;
    t->total_min = (uint8_t)minutes;
    t->last_us   = now_us;
}

int sleeptimer_armed(const sleeptimer_t *t)
{
    return t->armed ? 1 : 0;
}

int sleeptimer_total_min(const sleeptimer_t *t)
{
    return t->armed ? (int)t->total_min : 0;
}

int sleeptimer_remaining_min(const sleeptimer_t *t)
{
    if (!t->armed) {
        return 0;
    }
    /* feed() disarms the moment elapsed reaches total, so this is >= 1 for
     * every state an armed timer can be observed in. The guard is belt and
     * braces against a caller that hand-built the struct. */
    if (t->elapsed_min >= t->total_min) {
        return 0;
    }
    return (int)t->total_min - (int)t->elapsed_min;
}

sleeptimer_event_t sleeptimer_feed(sleeptimer_t *t, uint32_t now_us)
{
    if (!t->armed) {
        return SLEEPTIMER_NONE;
    }

    /* Unsigned subtraction between CONSECUTIVE feeds: the 1 MHz counter's
     * wrap cancels, so the accumulated total is exact however many times it
     * wraps during a 120-minute countdown. See the feed contract in the
     * header — a gap longer than one wrap (~71.6 min) is unrepresentable. */
    uint32_t delta = (uint32_t)(now_us - t->last_us);
    t->last_us = now_us;
    t->acc_us += delta;

    if (t->acc_us < SLEEPTIMER_MIN_US) {
        return SLEEPTIMER_NONE;            /* the common pass: nothing moved */
    }

    /* Carry the remainder rather than truncating it, so coarse or irregular
     * feeds accumulate to the exact minute instead of drifting late. */
    while (t->acc_us >= SLEEPTIMER_MIN_US) {
        t->acc_us -= SLEEPTIMER_MIN_US;
        if (t->elapsed_min < 0xFFFFu) {
            t->elapsed_min++;
        }
    }

    if (t->elapsed_min >= t->total_min) {
        /* One-shot: disarm in the same call that reports it, so no later feed
         * — after a wrap, or after the caller was away asleep — can fire it a
         * second time (ui/screen_battery.c's toast disarms for the same
         * reason). The caller is about to suspend; it will not be back. */
        sleeptimer_reset(t);
        return SLEEPTIMER_FIRE;
    }
    return SLEEPTIMER_TICK;
}

int sleeptimer_token(const sleeptimer_t *t, char *buf, int buf_sz)
{
    if (buf == 0 || buf_sz <= 0) {
        return 0;
    }
    buf[0] = '\0';

    int rem = sleeptimer_remaining_min(t);
    if (rem <= 0) {
        return 0;
    }

    char digits[4];
    int  nd = 0;
    unsigned v = (unsigned)rem;
    do {
        digits[nd++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v && nd < 4);

    /* "SLEEP " + digits + NUL. A buffer that cannot hold the whole token
     * draws nothing: a truncated "SLEEP 1" for 120 minutes left would be a
     * worse readout than none. */
    int need = 6 + nd + 1;
    if (buf_sz < need) {
        return 0;
    }

    const char *p = "SLEEP ";
    int i = 0;
    while (*p) {
        buf[i++] = *p++;
    }
    while (nd > 0) {
        buf[i++] = digits[--nd];
    }
    buf[i] = '\0';
    return i;
}
