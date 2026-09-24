/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/kernel/chargestat_test.c — the charge trend record on the host.
 *
 * chargestat.c is pure (the caller passes the clock), so every scenario here
 * is a script of samples at exact microsecond stamps and the assertions are
 * the exact numbers the Battery page would print. Pinned:
 *   1. An empty record answers "nothing yet" everywhere, not zeros dressed
 *      up as readings.
 *   2. A plug edge opens a session: t0 moves, the delta restarts from the
 *      first sample AFTER the edge, the CHRG count restarts.
 *   3. The history keeps one reading per minute, oldest first, the LATEST
 *      sample of a minute winning, and is NOT reset by a plug edge.
 *   4. A feed that arrives minutes late leaves gaps, and gaps make
 *      recent_delta decline rather than invent a slope.
 *   5. The ring wraps at sixty and keeps the newest sixty.
 *   6. Everything survives the 32-bit microsecond wrap.
 *   7. A failed read (-1) moves the clock and nothing else.
 */

#include <stdio.h>
#include <string.h>

#include "chargestat.h"

static int g_fail;
static int check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    if (!cond) g_fail = 1;
    return cond;
}

#define S(n)  ((uint32_t)(n) * 1000000u)      /* seconds -> us */
#define MIN(n) ((uint32_t)(n) * 60000000u)    /* minutes -> us */

int main(void)
{
    chargestat_t cs;
    uint16_t h[CHARGESTAT_BINS];
    int d = 12345;

    /* --- 1: empty --- */
    chargestat_reset(&cs);
    check("empty session is 0 s",     chargestat_session_s(&cs, S(500)) == 0);
    check("empty mv0 is -1",          chargestat_session_mv0(&cs) == -1);
    check("empty delta is 0",         chargestat_session_delta_mv(&cs) == 0);
    check("empty chg pct is 0",       chargestat_session_chg_pct(&cs) == 0);
    check("empty history is 0 bins",  chargestat_history(&cs, h) == 0);
    check("empty recent_delta says no", !chargestat_recent_delta(&cs, 10, &d) && d == 12345);

    /* --- 2: on battery, then a plug edge --- */
    uint32_t t = S(100);
    check("first feed opens a session", chargestat_feed(&cs, 3900, 0, 0, t) == 1);
    check("second feed does not",       chargestat_feed(&cs, 3890, 0, 0, t + S(5)) == 0);
    chargestat_feed(&cs, 3880, 0, 0, t + S(10));
    check("battery session mv0",   chargestat_session_mv0(&cs) == 3900);
    check("battery session delta", chargestat_session_delta_mv(&cs) == -20);
    check("battery session s",     chargestat_session_s(&cs, t + S(12)) == 12);
    check("battery chg pct 0",     chargestat_session_chg_pct(&cs) == 0);

    /* Plug in at t+15: the edge opens a session with NO reading yet, so the
     * first sample after it is the baseline — not the pre-plug 3880. */
    check("plug edge opens a session", chargestat_feed(&cs, 3905, 1, 1, t + S(15)) == 1);
    check("plug session mv0 is the post-edge sample", chargestat_session_mv0(&cs) == 3905);
    check("plug session starts at the edge", chargestat_session_s(&cs, t + S(20)) == 5);
    chargestat_feed(&cs, 3920, 1, 1, t + S(20));
    chargestat_feed(&cs, 3930, 1, 0, t + S(25));      /* CHRG dropped once */
    chargestat_feed(&cs, 3945, 1, 1, t + S(30));
    check("plug delta = last - first after edge", chargestat_session_delta_mv(&cs) == 40);
    check("chg pct = 3 of 4 = 75", chargestat_session_chg_pct(&cs) == 75);

    /* --- 3: the history keeps the latest reading per minute, oldest first,
     *        and the plug edge above did not cut it --- */
    check("still one bin inside the first minute", chargestat_history(&cs, h) == 1);
    check("that bin holds the latest sample", h[0] == 3945);
    chargestat_feed(&cs, 3950, 1, 1, t + MIN(1));      /* minute 2 opens */
    chargestat_feed(&cs, 3960, 1, 1, t + MIN(1) + S(30));
    chargestat_feed(&cs, 3975, 1, 1, t + MIN(2));      /* minute 3 */
    check("three bins after two minutes", chargestat_history(&cs, h) == 3);
    check("oldest first, latest-of-minute wins",
          h[0] == 3945 && h[1] == 3960 && h[2] == 3975);
    check("recent_delta over 2 min = 30",
          chargestat_recent_delta(&cs, 2, &d) == 1 && d == 30);
    check("recent_delta over 3 min needs 4 bins",
          chargestat_recent_delta(&cs, 3, &d) == 0);

    /* --- 4: a late feed leaves gaps; a gap at either end declines --- */
    chargestat_feed(&cs, 3990, 1, 1, t + MIN(5));      /* minutes 4,5 skipped */
    int n = chargestat_history(&cs, h);
    check("late feed advanced three bins", n == 6);
    check("the skipped minutes are gaps", h[3] == 0 && h[4] == 0 && h[5] == 3990);
    check("recent_delta across a gap end declines",
          chargestat_recent_delta(&cs, 2, &d) == 0);
    check("recent_delta over the whole span works",
          chargestat_recent_delta(&cs, 5, &d) == 1 && d == 3990 - 3945);

    /* --- 5: the ring wraps at sixty and keeps the newest sixty --- */
    chargestat_reset(&cs);
    t = S(1000);
    for (int m = 0; m < 75; m++) {
        chargestat_feed(&cs, 3800 + m, 1, 1, t + MIN(m));
    }
    n = chargestat_history(&cs, h);
    check("ring holds sixty bins", n == CHARGESTAT_BINS);
    check("ring keeps the newest sixty, oldest first",
          h[0] == 3800 + 15 && h[59] == 3800 + 74);
    check("recent_delta over 59 min", chargestat_recent_delta(&cs, 59, &d) == 1 && d == 59);
    check("recent_delta over 60 min declines (past the ring)",
          chargestat_recent_delta(&cs, 60, &d) == 0);
    /* More than an hour of silence: the whole ring is gap. (65 minutes, not
     * more: the timer wraps at 71.6, and a longer gap is not representable
     * to the caller either — that is the suspend loop's problem, not this
     * module's.) */
    chargestat_feed(&cs, 4000, 1, 1, t + MIN(75) + MIN(65));
    n = chargestat_history(&cs, h);
    check("an hour+ of silence leaves only the newest reading",
          n == CHARGESTAT_BINS && h[59] == 4000 && h[58] == 0 && h[0] == 0);

    /* --- 6: the microsecond wrap --- */
    chargestat_reset(&cs);
    t = 0xFFFFFFF0u - MIN(1);                   /* one minute before the wrap */
    chargestat_feed(&cs, 3700, 0, 0, t);
    chargestat_feed(&cs, 3710, 0, 0, t + MIN(1));        /* wraps to 0xFFFFFFF0 */
    chargestat_feed(&cs, 3720, 0, 0, t + MIN(2));        /* past zero */
    n = chargestat_history(&cs, h);
    check("history walks through the wrap", n == 3 && h[0] == 3700 && h[2] == 3720);
    check("session seconds through the wrap",
          chargestat_session_s(&cs, t + MIN(2) + S(7)) == 127);
    check("delta through the wrap", chargestat_session_delta_mv(&cs) == 20);

    /* --- 7: a failed read moves only the clock --- */
    chargestat_reset(&cs);
    t = S(50);
    chargestat_feed(&cs, -1, 1, 1, t);                   /* boot: bus hiccup */
    check("failed first read: no mv0", chargestat_session_mv0(&cs) == -1);
    check("failed first read: no samples", chargestat_session_chg_pct(&cs) == 0);
    check("failed first read still opens the bin", chargestat_history(&cs, h) == 1 && h[0] == 0);
    chargestat_feed(&cs, 4010, 1, 1, t + S(5));
    check("first good read is the baseline", chargestat_session_mv0(&cs) == 4010);
    chargestat_feed(&cs, -1, 1, 1, t + MIN(1));          /* a failed read opens minute 2 */
    n = chargestat_history(&cs, h);
    check("failed read in a new minute leaves that bin a gap",
          n == 2 && h[0] == 4010 && h[1] == 0);
    check("failed read does not move the latest", chargestat_session_delta_mv(&cs) == 0);
    chargestat_feed(&cs, 4030, 1, 1, t + MIN(1) + S(5));
    check("the next good read fills the bin",
          chargestat_history(&cs, h) == 2 && h[1] == 4030 &&
          chargestat_session_delta_mv(&cs) == 20);

    return g_fail;
}
