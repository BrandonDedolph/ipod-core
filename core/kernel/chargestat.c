/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/chargestat.c — the charge trend record. See chargestat.h.
 *
 * Freestanding, integer-only, no libc. Every time comparison is an unsigned
 * difference on the 32-bit microsecond timer.
 */

#include "chargestat.h"

static void session_open(chargestat_t *cs, int ext, uint32_t now_us)
{
    cs->ext         = (uint8_t)(ext ? 1 : 0);
    cs->t0_us       = now_us;
    cs->mv0         = -1;
    cs->mv_last     = -1;
    cs->mv_min      = 0;
    cs->mv_max      = 0;
    cs->samples     = 0;
    cs->chg_samples = 0;
}

void chargestat_reset(chargestat_t *cs)
{
    cs->primed = 0;
    session_open(cs, 0, 0);
    cs->bin_t_us  = 0;
    cs->hist_n    = 0;
    cs->hist_head = 0;
    for (int i = 0; i < CHARGESTAT_BINS; i++) {
        cs->hist[i] = 0;
    }
}

/* Clamp a reading into the uint16 the history stores; -1 (no reading) and
 * anything the ADC could not have produced become the 0 gap marker. */
static uint16_t to_bin(int mv)
{
    if (mv <= 0 || mv > 65535) {
        return 0;
    }
    return (uint16_t)mv;
}

int chargestat_feed(chargestat_t *cs, int filt_mv, int ext, int charging,
                    uint32_t now_us)
{
    int opened = 0;
    ext = ext ? 1 : 0;

    if (!cs->primed) {
        cs->primed = 1;
        session_open(cs, ext, now_us);
        /* The first bin opens with the first feed, reading or not, so the
         * hour's clock starts at power-on rather than at the first good
         * sample. */
        cs->bin_t_us  = now_us;
        cs->hist_n    = 1;
        cs->hist_head = 0;
        cs->hist[0]   = 0;
        opened = 1;
    } else if (ext != cs->ext) {
        session_open(cs, ext, now_us);
        opened = 1;
    }

    /* A new minute: advance the ring. A feed that arrives several minutes
     * late (the suspend loop samples at the same 5 s, but a failed bus can
     * skip) leaves the skipped minutes as gaps rather than smearing one
     * reading across them. Bounded: at most one full lap. */
    uint32_t late = now_us - cs->bin_t_us;
    int steps = 0;
    while (late >= CHARGESTAT_BIN_US && steps < CHARGESTAT_BINS) {
        cs->hist_head = (uint8_t)((cs->hist_head + 1) % CHARGESTAT_BINS);
        cs->hist[cs->hist_head] = 0;
        if (cs->hist_n < CHARGESTAT_BINS) {
            cs->hist_n++;
        }
        cs->bin_t_us += CHARGESTAT_BIN_US;
        late -= CHARGESTAT_BIN_US;
        steps++;
    }
    if (steps == CHARGESTAT_BINS) {
        cs->bin_t_us = now_us;        /* more than an hour: the ring is all gap */
    }

    if (filt_mv < 0) {
        return opened;                /* a failed read moves only the clock */
    }

    /* The session. */
    if (cs->mv0 < 0) {
        cs->mv0    = (int16_t)filt_mv;
        cs->mv_min = (int16_t)filt_mv;
        cs->mv_max = (int16_t)filt_mv;
    }
    cs->mv_last = (int16_t)filt_mv;
    if (filt_mv < cs->mv_min) cs->mv_min = (int16_t)filt_mv;
    if (filt_mv > cs->mv_max) cs->mv_max = (int16_t)filt_mv;
    if (cs->samples < 0xFFFFu) {
        cs->samples++;
        if (charging) {
            cs->chg_samples++;
        }
    }

    /* The history: the bin holds the LATEST reading of its minute. The
     * caller's value is already a median over the last five samples, so
     * "latest" is not a spike. */
    cs->hist[cs->hist_head] = to_bin(filt_mv);
    return opened;
}

uint32_t chargestat_session_s(const chargestat_t *cs, uint32_t now_us)
{
    if (!cs->primed) {
        return 0;
    }
    return (now_us - cs->t0_us) / 1000000u;
}

int chargestat_session_mv0(const chargestat_t *cs)
{
    return cs->mv0;
}

int chargestat_session_delta_mv(const chargestat_t *cs)
{
    if (cs->mv0 < 0 || cs->mv_last < 0) {
        return 0;
    }
    return (int)cs->mv_last - (int)cs->mv0;
}

int chargestat_session_chg_pct(const chargestat_t *cs)
{
    if (cs->samples == 0) {
        return 0;
    }
    return (int)(((uint32_t)cs->chg_samples * 100u + cs->samples / 2u)
                 / cs->samples);
}

int chargestat_recent_delta(const chargestat_t *cs, int minutes, int *delta_mv)
{
    if (minutes <= 0 || minutes >= CHARGESTAT_BINS || cs->hist_n <= minutes) {
        return 0;
    }
    int newest = cs->hist_head;
    int oldest = (newest + CHARGESTAT_BINS - minutes) % CHARGESTAT_BINS;
    if (cs->hist[newest] == 0 || cs->hist[oldest] == 0) {
        return 0;
    }
    *delta_mv = (int)cs->hist[newest] - (int)cs->hist[oldest];
    return 1;
}

int chargestat_history(const chargestat_t *cs, uint16_t *out)
{
    int n = cs->hist_n;
    /* Oldest bin is head - (n - 1), walking forward. */
    int idx = (cs->hist_head + CHARGESTAT_BINS - (n - 1)) % CHARGESTAT_BINS;
    for (int i = 0; i < n; i++) {
        out[i] = cs->hist[idx];
        idx = (idx + 1) % CHARGESTAT_BINS;
    }
    return n;
}
