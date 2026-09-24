/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/chargestat.h — what the cell has been doing lately.
 *
 * The device has no current sense. The only thing it can measure about a
 * charge is the terminal voltage, every 5 s, and the only honest answer to
 * "is it charging?" is therefore a TREND: what the filtered millivolts have
 * done since the cable went in, and over the last hour. The charger's own
 * CHRG pin says only that the LTC4066 is trying — the 2026-09-19 event log
 * has it asserted for three hours of playback at the 100 mA cap while the
 * cell moved 11 mV — so a Battery page that showed the pin alone would have
 * said "charging" through every one of those hours.
 *
 * This module keeps that record. It is fed once per battery sample by
 * kernel/main.c (battery_refresh) and read by the Battery page and the
 * charging screen's caption. Pure: no hardware, no clock of its own, no
 * allocation — the caller passes the 1 MHz USEC_TIMER and every difference
 * is unsigned, so the 71-minute wrap is a non-event. Host-tested in
 * tests/kernel/chargestat_test.c.
 *
 * Two things are tracked, on purpose separately:
 *
 *   THE SESSION: since the cable last went in (or out). Its first filtered
 *   sample, the latest, the extremes, how many samples the charger pin was
 *   asserted for. Reset on every plug/unplug edge, because "+40 mV since
 *   you plugged in" is the sentence the user wants.
 *
 *   THE HISTORY: one millivolt reading per minute for the last hour, NOT
 *   reset on an edge — the dip when the cable came out and the climb after
 *   it went back in are the picture, and cutting the line at the edge
 *   would remove exactly the part that explains it.
 */

#ifndef CORE_KERNEL_CHARGESTAT_H
#define CORE_KERNEL_CHARGESTAT_H

#include <stdint.h>

#define CHARGESTAT_BINS    60             /* one per minute: the last hour   */
#define CHARGESTAT_BIN_US  60000000u      /* a bin is a minute of samples    */

typedef struct {
    /* session */
    uint8_t  primed;       /* a feed has happened                            */
    uint8_t  ext;          /* external power as of the last feed             */
    uint32_t t0_us;        /* when the session began (the last edge, or the
                            * first feed)                                    */
    int16_t  mv0;          /* first filtered sample of the session; -1 none  */
    int16_t  mv_last;      /* latest filtered sample; -1 none                */
    int16_t  mv_min;       /* extremes over the session                      */
    int16_t  mv_max;
    uint16_t samples;      /* good samples this session (saturates)          */
    uint16_t chg_samples;  /* ...of which the CHRG pin was asserted          */
    /* history */
    uint32_t bin_t_us;     /* when the current bin opened                    */
    uint16_t hist[CHARGESTAT_BINS];  /* ring of minute readings, mV; 0 = gap */
    uint8_t  hist_n;       /* bins in use (<= CHARGESTAT_BINS)               */
    uint8_t  hist_head;    /* index of the CURRENT (newest) bin              */
} chargestat_t;

/* Empty: no session, no history. .bss gives the same thing. */
void chargestat_reset(chargestat_t *cs);

/*
 * One battery sample. `filt_mv` is the FILTERED millivolts (the policy's
 * median), or -1 for a failed read, which advances nothing but the clock;
 * `ext` is power_is_external(); `charging` the CHRG pin; `now_us` the 1 MHz
 * timer. Returns 1 when this feed opened a new session (a plug or unplug),
 * so the caller can narrate it once.
 */
int chargestat_feed(chargestat_t *cs, int filt_mv, int ext, int charging,
                    uint32_t now_us);

/* Seconds since the session began. 0 before the first feed. */
uint32_t chargestat_session_s(const chargestat_t *cs, uint32_t now_us);

/* The session's first filtered reading, or -1 before one arrived. */
int chargestat_session_mv0(const chargestat_t *cs);

/* Latest reading minus the session's first. 0 until both exist. */
int chargestat_session_delta_mv(const chargestat_t *cs);

/* Per cent of the session's samples on which the charger pin was asserted,
 * 0..100; 0 with no samples. */
int chargestat_session_chg_pct(const chargestat_t *cs);

/*
 * The last `minutes` minutes as one number: the newest bin minus the bin
 * `minutes` back. Returns 1 and writes *delta_mv when the history is deep
 * enough and both ends hold a reading, else 0 — "not yet" is a real answer
 * and the page shows it as such rather than as a zero.
 */
int chargestat_recent_delta(const chargestat_t *cs, int minutes, int *delta_mv);

/*
 * Copy the history into `out` (CHARGESTAT_BINS entries), OLDEST FIRST, 0 for
 * a bin with no reading. Returns the number of bins in use; the copied
 * entries are out[0 .. n-1].
 */
int chargestat_history(const chargestat_t *cs, uint16_t *out);

#endif /* CORE_KERNEL_CHARGESTAT_H */
