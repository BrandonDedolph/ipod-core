/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/kernel/timesync_test.c — the boot-time clock decision
 * (kernel/timesync.c) and the software clock that carries the time between RTC
 * reads (kernel/wallclock.c). Both are the SAME sources the ARM build links.
 *
 * THE POINT OF THIS FILE. The host cannot set the device's clock over the
 * cable, so it leaves a stamp in the settings record and the firmware decides
 * at boot what to do with it. The two ways that goes wrong are both silent and
 * both live in sequences, not in single calls:
 *
 *   - applying the stamp on EVERY boot, so a device that sat on a shelf for a
 *     week sets itself back a week every time it is switched on;
 *   - a mark that failed to reach the disk making the stamp immortal.
 *
 * So the table below is one case per rule, and then there are sequences: the
 * same stamp seen at three consecutive boots, and a boot whose mark was lost.
 * None of this is reachable on the device without waiting days between boots,
 * which is the whole reason the decision is a pure function.
 *
 * The wallclock half is here for the same reason ui/sleeptimer.c's test is:
 * the µs counter wraps every ~71.6 minutes, so a clock built on it can only be
 * proved by running hours of feeds through it, which takes milliseconds here
 * and hours at the bench.
 *
 * MUTATION CHECK: making rule 3 "host > applied" instead of "!=" lets a
 * corrected host clock be ignored (corrected_stamp fails); dropping rule 1
 * fails not_writable; dropping rule 5 fails shelf_boot and lost_mark; copying
 * the offset only on SET fails dst_offset; folding the wallclock remainder
 * with (frac + delta) / 1e6 fails big_delta; adopting a frozen RTC fails
 * resync_stopped.
 */

#include <stdio.h>

#include "timesync.h"
#include "wallclock.h"
#include "datetime.h"
#include "../xfail.h"

/* 2026-09-16 10:42:00Z, the plan's moment, as the host would stamp it. */
#define HOST_T 1789555320u

static timesync_state_t stamped(uint32_t host, int off, uint32_t applied)
{
    timesync_state_t s = { host, off, applied, 0 };
    return s;
}

/* ---------- the decision table ---------------------------------------- */

static void check_table(xfail_ctx *c)
{
    timesync_state_t s;

    /* 1. Nothing may be applied that cannot be recorded. */
    s = stamped(HOST_T, 120, 0);
    xpect(c, "not_writable: a record we cannot write means no action, "
             "whatever the stamp says",
          timesync_decide(&s, 0, 1, HOST_T - 3600u) == TIMESYNC_NONE);

    /* 2. Never stamped. */
    s = stamped(0, 0, 0);
    xpect(c, "no_stamp: a device that has never met the host app does nothing",
          timesync_decide(&s, 1, 0, 0) == TIMESYNC_NONE);

    /* 3. The mark. */
    s = stamped(HOST_T, 120, HOST_T);
    xpect(c, "marked: a stamp already acted on is inert",
          timesync_decide(&s, 1, 1, HOST_T + 100u) == TIMESYNC_NONE);

    /* 3, the other way round: a host whose clock was wrong IN THE FUTURE and
     * was then corrected stamps an EARLIER epoch than the one we acted on.
     * That is a new stamp, not an old one. */
    s = stamped(HOST_T, 120, HOST_T + 86400u);
    xpect(c, "corrected_stamp: an earlier stamp than the mark is still a new "
             "stamp",
          timesync_decide(&s, 1, 0, 0) == TIMESYNC_SET);

    /* 4. Not a date this clock can hold. */
    s = stamped(DATETIME_EPOCH_2001 - 1u, 0, 0);
    xpect(c, "before_2001: a stamp the RTC cannot represent is marked, "
             "not applied",
          timesync_decide(&s, 1, 0, 0) == TIMESYNC_STALE);
    s = stamped(DATETIME_EPOCH_2100, 0, 0);
    xpect(c, "after_2099: likewise at the far end",
          timesync_decide(&s, 1, 0, 0) == TIMESYNC_STALE);

    /* 5. The running clock is already well past the stamp. */
    s = stamped(HOST_T, 0, 0);
    xpect(c, "shelf_boot: an RTC ten minutes past the stamp leaves it alone",
          timesync_decide(&s, 1, 1, HOST_T + TIMESYNC_STALE_S) ==
              TIMESYNC_STALE);
    xpect(c, "shelf_boot: one second inside the window still applies",
          timesync_decide(&s, 1, 1, HOST_T + TIMESYNC_STALE_S - 1u) ==
              TIMESYNC_SET);
    xpect(c, "shelf_boot: a week past the stamp is emphatically stale",
          timesync_decide(&s, 1, 1, HOST_T + 7u * 86400u) == TIMESYNC_STALE);

    /* 5 cannot fire without a clock to judge with: "unset" and "the bus did
     * not answer" both reach here as rtc_valid 0. */
    xpect(c, "unset_rtc: an unknown clock is always worth setting, whatever "
             "number came back with it",
          timesync_decide(&s, 1, 0, HOST_T + 7u * 86400u) == TIMESYNC_SET);

    /* 6. Behind, or ahead by less than the window. */
    xpect(c, "behind: a slow clock is pulled forward",
          timesync_decide(&s, 1, 1, HOST_T - 86400u) == TIMESYNC_SET);
    xpect(c, "slightly_fast: a clock a minute fast is pulled back",
          timesync_decide(&s, 1, 1, HOST_T + 60u) == TIMESYNC_SET);
}

/* ---------- sequences: the two failures that only show over boots ------ */

static void check_sequences(xfail_ctx *c)
{
    /* (a) + (b): one stamp, three boots. The RTC runs on between them and must
     * never be written after the first. */
    timesync_state_t s = stamped(HOST_T, 120, 0);
    uint32_t rtc = 0;
    int actions[3];
    int applied_ok = 1;

    for (int boot = 0; boot < 3; boot++) {
        int rtc_valid = (boot > 0);            /* boot 0: the cell was flat */
        timesync_action_t a = timesync_decide(&s, 1, rtc_valid, rtc);
        actions[boot] = (int)a;
        if (a == TIMESYNC_SET) {
            rtc = s.host_epoch;                /* hal_rtc_set() */
        }
        (void)timesync_apply(&s, a);
        if (s.applied_epoch != HOST_T) {
            applied_ok = 0;                    /* the mark must stick */
        }
        rtc += 3600u;                          /* an hour between boots */
    }

    xpect(c, "one_stamp_three_boots: set once, then nothing, ever",
          actions[0] == TIMESYNC_SET && actions[1] == TIMESYNC_NONE &&
          actions[2] == TIMESYNC_NONE && applied_ok);
    xpect(c, "one_stamp_three_boots: the clock is two hours past the stamp "
             "and was never pulled back", rtc == HOST_T + 3u * 3600u);

    /* (c) The mark failed to reach the disk (a refused save). The stamp is
     * still there and applied_epoch is still 0 — but the RTC has now run 11
     * minutes past it, so rule 5 makes it inert anyway. THIS is what bounds a
     * lost mark to one backwards step of under ten minutes. */
    timesync_state_t lost = stamped(HOST_T, 120, 0);
    xpect(c, "lost_mark: a stamp whose mark was lost cannot pull the clock "
             "back once the RTC has run past it",
          timesync_decide(&lost, 1, 1, HOST_T + 11u * 60u) == TIMESYNC_STALE);
    xpect(c, "lost_mark: inside the window it retries, which is the point of "
             "not marking a failed set",
          timesync_decide(&lost, 1, 1, HOST_T + 9u * 60u) == TIMESYNC_SET);

    /* (d) Nothing is written when nothing may be written. */
    timesync_state_t ro = stamped(HOST_T, 120, 0);
    timesync_action_t a = timesync_decide(&ro, 0, 1, HOST_T);
    xpect(c, "not_writable: the record is untouched, so the next writable "
             "boot still sees the stamp",
          a == TIMESYNC_NONE && timesync_apply(&ro, a) == 0 &&
          ro.applied_epoch == 0 && ro.utc_off_min == 0);

    /* (e) DST. The offset is the host's to know, and it is copied on a STALE
     * action too — the epoch beside it may be one we decline to use. */
    timesync_state_t dst = stamped(HOST_T, 60, 0);
    dst.utc_off_min = 0;
    timesync_action_t sa = timesync_decide(&dst, 1, 1, HOST_T + 86400u);
    int moved = timesync_apply(&dst, sa);
    xpect(c, "dst_offset: a stale stamp still hands over the host's offset",
          sa == TIMESYNC_STALE && moved == 1 && dst.utc_off_min == 60 &&
          dst.applied_epoch == HOST_T);
    xpect(c, "dst_offset: and the marked stamp is then inert",
          timesync_decide(&dst, 1, 1, HOST_T + 86400u) == TIMESYNC_NONE);

    /* apply() is a no-op for NONE and idempotent for the rest. */
    timesync_state_t idem = stamped(HOST_T, 30, 0);
    xpect(c, "apply: NONE moves nothing; a repeat move reports nothing to save",
          timesync_apply(&idem, TIMESYNC_NONE) == 0 && idem.applied_epoch == 0 &&
          timesync_apply(&idem, TIMESYNC_SET) == 1 &&
          timesync_apply(&idem, TIMESYNC_SET) == 0 &&
          idem.applied_epoch == HOST_T && idem.utc_off_min == 30);
}

/* ---------- the software clock ---------------------------------------- */

#define SEC_US 1000000u

static void check_wallclock(xfail_ctx *c)
{
    wallclock_t w;
    uint32_t got = 0, minute = 0;

    wallclock_reset(&w);
    xpect(c, "wallclock: a reset clock knows nothing",
          wallclock_now(&w, 12345u, &got) == 0 &&
          wallclock_minute(&w, 12345u, &minute) == 0);

    wallclock_anchor(&w, 0, HOST_T, 1000u);
    xpect(c, "wallclock: anchoring from an RTC that had no time keeps it "
             "unknown", wallclock_now(&w, 2000u, &got) == 0);

    /* Sub-second reads do not move the second; a second and a half does. */
    wallclock_anchor(&w, 1, HOST_T, 1000u);
    int ok = wallclock_now(&w, 1000u + 999999u, &got) && got == HOST_T;
    ok &= wallclock_now(&w, 1000u + SEC_US + SEC_US / 2u, &got) &&
          got == HOST_T + 1u;
    xpect(c, "wallclock: the time advances with the µs counter without a tick",
          ok);

    /* Two hours of 5 s feeds — the main loop's battery cadence — across TWO
     * wraps of the 32-bit µs counter (~71.6 min each). The anchor is placed
     * just before the first wrap so the run crosses one almost immediately. */
    wallclock_reset(&w);
    uint32_t now_us = 0xFFFF0000u;
    wallclock_anchor(&w, 1, HOST_T, now_us);
    for (int i = 0; i < 1440; i++) {           /* 1440 * 5 s = 2 h */
        now_us += 5u * SEC_US;
        wallclock_tick(&w, now_us);
    }
    xpect(c, "wallclock: two hours of 5 s feeds across two counter wraps is "
             "exactly two hours",
          wallclock_now(&w, now_us, &got) && got == HOST_T + 7200u);

    /* Irregular feeds must CARRY the remainder, not truncate it: 3 000 feeds
     * of 333 333 µs is 999.999 s, so the clock must read exactly +999 with
     * 999 000 µs still in hand — a truncating fold would have lost 999 s. */
    wallclock_reset(&w);
    now_us = 0;
    wallclock_anchor(&w, 1, HOST_T, now_us);
    for (int i = 0; i < 3000; i++) {
        now_us += 333333u;
        wallclock_tick(&w, now_us);
    }
    xpect(c, "wallclock: an irregular feed carries its remainder instead of "
             "drifting late",
          wallclock_now(&w, now_us, &got) && got == HOST_T + 999u);

    /* One enormous delta: the fold must not form frac + delta in 32 bits. */
    wallclock_reset(&w);
    wallclock_anchor(&w, 1, HOST_T, 0u);
    wallclock_tick(&w, 4000u * SEC_US);        /* 4 000 s, ~93 % of a wrap */
    xpect(c, "wallclock: a delta near a full wrap folds without overflowing",
          wallclock_now(&w, 4000u * SEC_US, &got) && got == HOST_T + 4000u);

    /* The repaint edge: the minute counter changes exactly once per minute. */
    wallclock_reset(&w);
    /* HOST_T is 10:42:00; back it up to 10:41:59 so the edge is one second
     * away and the one after it is a minute away. */
    wallclock_anchor(&w, 1, HOST_T - 1u, 0u);
    uint32_t m0 = 0, m1 = 0, m2 = 0;
    wallclock_minute(&w, 0u, &m0);
    wallclock_minute(&w, SEC_US, &m1);
    wallclock_minute(&w, 59u * SEC_US, &m2);
    xpect(c, "wallclock: the minute counter steps on the minute edge and "
             "nowhere else",
          m1 == m0 + 1u && m2 == m1 && m0 == (HOST_T - 1u) / 60u);
}

static void check_resync(xfail_ctx *c)
{
    wallclock_t w;
    int32_t drift = 99;
    uint32_t got = 0;

    /* The first anchor of a boot has nothing to compare against. */
    wallclock_reset(&w);
    xpect(c, "resync: the first reading of a boot just anchors",
          wallclock_resync(&w, 1, HOST_T, 1000u, &drift) ==
              WALLCLOCK_RESYNC_OK && drift == 0);

    /* Half an hour later the chip agrees to the second. */
    uint32_t later = 1000u + 1800u * SEC_US;
    xpect(c, "resync: a chip that agrees is not news",
          wallclock_resync(&w, 1, HOST_T + 1800u, later, &drift) ==
              WALLCLOCK_RESYNC_OK && drift == 0);

    /* ...and half an hour after that it is half a minute out. The chip wins:
     * it is the half that survives a suspend. */
    uint32_t later2 = later + 1800u * SEC_US;
    xpect(c, "resync: a disagreement is reported with its sign and the chip's "
             "reading is adopted",
          wallclock_resync(&w, 1, HOST_T + 3630u, later2, &drift) ==
              WALLCLOCK_RESYNC_DRIFT && drift == 30 &&
          wallclock_now(&w, later2, &got) && got == HOST_T + 3630u);

    /* A chip still reading the second it read at the anchor is not a clock —
     * that is the shape an absent PMU produces (stale I2C DATA bytes). */
    wallclock_reset(&w);
    wallclock_anchor(&w, 1, HOST_T, 0u);
    xpect(c, "resync_stopped: a frozen reading is refused, not adopted, and "
             "the clock goes back to unknown",
          wallclock_resync(&w, 1, HOST_T, 10u * SEC_US, &drift) ==
              WALLCLOCK_RESYNC_STOPPED &&
          wallclock_now(&w, 10u * SEC_US, &got) == 0);

    /* The same reading a moment later is NOT frozen — the second has simply
     * not ticked yet. */
    wallclock_reset(&w);
    wallclock_anchor(&w, 1, HOST_T, 0u);
    xpect(c, "resync_stopped: the same second read a moment later is just the "
             "same second",
          wallclock_resync(&w, 1, HOST_T, SEC_US, &drift) ==
              WALLCLOCK_RESYNC_OK &&
          wallclock_now(&w, SEC_US, &got) == 1);

    /* An RTC that reports "no time" clears the software clock rather than
     * leaving a stale one running. */
    wallclock_reset(&w);
    wallclock_anchor(&w, 1, HOST_T, 0u);
    xpect(c, "resync: an RTC with no time leaves the clock unknown, and that "
             "is not a disagreement",
          wallclock_resync(&w, 0, 0, SEC_US, &drift) == WALLCLOCK_RESYNC_OK &&
          drift == 0 && wallclock_now(&w, SEC_US, &got) == 0);

    /* drift_s is optional. */
    wallclock_reset(&w);
    xpect(c, "resync: the drift is optional",
          wallclock_resync(&w, 1, HOST_T, 0u, 0) == WALLCLOCK_RESYNC_OK);
}

int main(void)
{
    xfail_ctx c = { "timesync", 0, 0, 0 };

    check_table(&c);
    check_sequences(&c);
    check_wallclock(&c);
    check_resync(&c);

    return xfail_done(&c);
}
