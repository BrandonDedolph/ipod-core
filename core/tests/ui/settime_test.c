/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ui/settime_test.c — the Set Date & Time field editor's model
 * (core/ui/settime.c) on the host. The SAME source the ARM build links.
 *
 * THE POINT OF THIS FILE. A field editor is nothing but edge cases, and the
 * only way to find them at the bench is to stand there turning a wheel: 31
 * January stepped into February, the year that must clamp where every other
 * field wraps, midnight and noon both reading 12, an AM/PM plate that has to
 * move the hour by exactly twelve without dragging the hour plate with it.
 * Each of those is a case below, fed the way kernel/main.c will feed it: one
 * clamped wheel delta or one Select per event.
 *
 * MUTATION CHECK: wrapping the year instead of clamping fails year_clamps;
 * dropping the day re-clamp after a month move fails jan31_into_february;
 * letting the hour field run 0..23 in 12-hour mode fails hour_cycles_in_half_day;
 * flipping AM/PM on an even delta fails ampm_flips; printing hour 0 as "00" in
 * 12-hour mode fails twelve_hour_text; a settime_next that reports done twice
 * fails select_walk.
 */

#include <stdio.h>

#include "settime.h"
#include "../xfail.h"

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* The text on the currently selected plate. */
static int sel_text_is(const settime_t *t, const char *want)
{
    char buf[SETTIME_FIELD_MAX];
    settime_field_text(t, t->field, buf, (int)sizeof(buf));
    return str_eq(buf, want);
}

static void check_seed(xfail_ctx *c)
{
    settime_t t;
    datetime_t now = { 2026, 9, 16, 22, 42, 37, 3 };

    settime_begin(&t, 1, &now, 1);
    xpect(c, "seed: a known time seeds every field and drops the seconds",
          t.year == 2026 && t.month == 9 && t.day == 16 && t.hour == 22 &&
          t.min == 42 && t.field == ST_YEAR && t.use_12h == 1);

    settime_begin(&t, 0, &now, 0);
    xpect(c, "seed: with no clock the editor starts at a plainly artificial "
             "2026-01-01 12:00",
          t.year == 2026 && t.month == 1 && t.day == 1 && t.hour == 12 &&
          t.min == 0 && t.use_12h == 0);

    /* A date the RTC cannot hold is not a seed, even if the caller offers it. */
    datetime_t y2000 = { 2000, 6, 1, 8, 0, 0, 0 };
    settime_begin(&t, 1, &y2000, 1);
    xpect(c, "seed: the year 2000 (the RTC's 'unset' reading) is not seeded "
             "from", t.year == 2026 && t.month == 1);

    xpect(c, "fields: six plates in 12-hour mode, five in 24-hour",
          (settime_begin(&t, 1, &now, 1), settime_field_count(&t)) == 6 &&
          (settime_begin(&t, 1, &now, 0), settime_field_count(&t)) == 5);
}

static void check_wrap(xfail_ctx *c)
{
    settime_t t;
    datetime_t now = { 2026, 1, 31, 23, 59, 0, 0 };

    /* Year: clamps at both ends, and the clamp is not a wrap. */
    settime_begin(&t, 1, &now, 0);
    t.field = ST_YEAR;
    t.year  = SETTIME_YEAR_MAX;
    settime_adjust(&t, 3);
    int hi = (t.year == SETTIME_YEAR_MAX);
    t.year = SETTIME_YEAR_MIN;
    settime_adjust(&t, -3);
    xpect(c, "year_clamps: the year stops at 2001 and 2099 instead of "
             "wrapping forty years under a flick",
          hi && t.year == SETTIME_YEAR_MIN);

    /* Month, day, hour, minute all wrap. */
    settime_begin(&t, 1, &now, 0);
    t.field = ST_MONTH;
    t.month = 12;
    settime_adjust(&t, 1);
    int mo_up = (t.month == 1);
    t.month = 1;
    settime_adjust(&t, -1);
    xpect(c, "month_wraps: December steps to January and back", mo_up &&
          t.month == 12);

    settime_begin(&t, 1, &now, 0);
    t.field = ST_MIN;
    t.min   = 59;
    settime_adjust(&t, 1);
    int mi_up = (t.min == 0);
    settime_adjust(&t, -1);
    xpect(c, "minute_wraps: 59 steps to 0 and back to 59", mi_up &&
          t.min == 59);

    settime_begin(&t, 1, &now, 0);
    t.field = ST_HOUR;
    t.hour  = 23;
    settime_adjust(&t, 1);
    int h_up = (t.hour == 0);
    settime_adjust(&t, -1);
    xpect(c, "hour_wraps: 23:00 steps to 00:00 and back in 24-hour mode",
          h_up && t.hour == 23);

    settime_begin(&t, 1, &now, 0);
    t.field = ST_DAY;
    t.day   = 31;                              /* January */
    settime_adjust(&t, 1);
    int d_up = (t.day == 1);
    settime_adjust(&t, -1);
    xpect(c, "day_wraps: the last of the month steps to the first and back",
          d_up && t.day == 31);

    /* One wheel event cannot carry more than SETTIME_DELTA_MAX detents. */
    settime_begin(&t, 1, &now, 0);
    t.field = ST_MIN;
    t.min   = 0;
    settime_adjust(&t, 40);
    xpect(c, "delta_clamp: a flick moves at most four steps, like the sliders",
          t.min == SETTIME_DELTA_MAX);
}

static void check_day_clamp(xfail_ctx *c)
{
    settime_t t;
    datetime_t jan31 = { 2026, 1, 31, 12, 0, 0, 0 };

    settime_begin(&t, 1, &jan31, 0);
    t.field = ST_MONTH;
    settime_adjust(&t, 1);
    xpect(c, "jan31_into_february: 31 January becomes 28 February in 2026",
          t.month == 2 && t.day == 28);

    datetime_t jan31_leap = { 2028, 1, 31, 12, 0, 0, 0 };
    settime_begin(&t, 1, &jan31_leap, 0);
    t.field = ST_MONTH;
    settime_adjust(&t, 1);
    xpect(c, "jan31_into_february: and 29 February in 2028",
          t.month == 2 && t.day == 29);

    /* The year field can shorten the month too: 29 Feb in a leap year, stepped
     * to the year after it. */
    datetime_t feb29 = { 2028, 2, 29, 12, 0, 0, 0 };
    settime_begin(&t, 1, &feb29, 0);
    t.field = ST_YEAR;
    settime_adjust(&t, 1);
    xpect(c, "leap_day_out_of_a_leap_year: 29 Feb 2028 becomes 28 Feb 2029",
          t.year == 2029 && t.month == 2 && t.day == 28);

    /* Whatever the walk, the date is always a real one. */
    settime_begin(&t, 1, &jan31, 0);
    int always_valid = 1;
    for (int i = 0; i < 40; i++) {
        t.field = i % 3;                       /* year, month, day */
        settime_adjust(&t, (i & 1) ? 3 : -2);
        if (t.day < 1 || t.day > datetime_mdays(t.year, t.month)) {
            always_valid = 0;
        }
    }
    xpect(c, "the day never exceeds the month, whatever order the fields are "
             "turned in", always_valid);
}

static void check_twelve_hour(xfail_ctx *c)
{
    settime_t t;
    datetime_t midnight = { 2026, 9, 16, 0, 5, 0, 0 };

    settime_begin(&t, 1, &midnight, 1);
    t.field = ST_HOUR;
    int h0 = sel_text_is(&t, "12");
    t.field = ST_AMPM;
    int am = sel_text_is(&t, "AM");

    t.hour  = 12;
    t.field = ST_HOUR;
    int h12 = sel_text_is(&t, "12");
    t.field = ST_AMPM;
    int pm = sel_text_is(&t, "PM");

    t.hour  = 13;
    t.field = ST_HOUR;
    int h13 = sel_text_is(&t, "01");
    xpect(c, "twelve_hour_text: 00:xx and 12:xx both read 12, 13:xx reads 01, "
             "and the plate says which half of the day it is",
          h0 && am && h12 && pm && h13);

    /* 24-hour mode shows the raw hour and no AM/PM plate at all. */
    settime_begin(&t, 1, &midnight, 0);
    t.field = ST_HOUR;
    char buf[SETTIME_FIELD_MAX];
    buf[0] = 'x';
    xpect(c, "twenty_four_hour_text: the hour is two digits and the AM/PM "
             "plate does not exist",
          sel_text_is(&t, "00") &&
          settime_field_text(&t, ST_AMPM, buf, (int)sizeof(buf)) == 0 &&
          buf[0] == '\0');

    /* The AM/PM plate moves the hour by twelve and nothing else. */
    settime_begin(&t, 1, &midnight, 1);
    t.field = ST_AMPM;
    int flipped = settime_adjust(&t, 1) && t.hour == 12 && t.min == 5;
    int back    = settime_adjust(&t, -1) && t.hour == 0;
    int even    = (settime_adjust(&t, 2) == 0) && t.hour == 0;
    xpect(c, "ampm_flips: one detent flips the half-day, an even number of "
             "them lands where it started", flipped && back && even);

    /* The hour plate cycles WITHIN the half-day, so the two plates are
     * independent — 11 AM steps to 12 AM, not to noon. */
    settime_begin(&t, 1, &midnight, 1);
    t.field = ST_HOUR;
    t.hour  = 11;
    settime_adjust(&t, 1);
    int am_kept = (t.hour == 0);
    t.hour = 23;
    settime_adjust(&t, 1);
    xpect(c, "hour_cycles_in_half_day: the hour plate never changes AM to PM",
          am_kept && t.hour == 12);
}

static void check_select_walk(xfail_ctx *c)
{
    settime_t t;
    datetime_t now = { 2026, 9, 16, 22, 42, 0, 0 };

    for (int mode = 0; mode < 2; mode++) {
        settime_begin(&t, 1, &now, mode);
        int n     = settime_field_count(&t);
        int dones = 0, order_ok = 1;
        for (int i = 0; i < n; i++) {
            if (t.field != i) {
                order_ok = 0;
            }
            dones += settime_next(&t);
        }
        /* Select on the last field is the commit; pressing it again cannot
         * commit a second time in a different state. */
        int stays = (t.field == n - 1) && settime_next(&t) == 1;
        xpect(c, mode ? "select_walk: 12-hour — left to right, done exactly "
                        "once, on the last plate"
                      : "select_walk: 24-hour — left to right, done exactly "
                        "once, on the last plate",
              order_ok && dones == 1 && stays);
    }

    /* What the caller writes to the clock. */
    settime_begin(&t, 1, &now, 1);
    datetime_t out;
    settime_civil(&t, &out);
    xpect(c, "civil: the editor always sets a whole minute, with the weekday "
             "computed from the date",
          out.year == 2026 && out.month == 9 && out.day == 16 &&
          out.hour == 22 && out.min == 42 && out.sec == 0 && out.wday == 3 &&
          datetime_valid(&out));

    xpect(c, "labels: every plate but AM/PM is captioned",
          str_eq(settime_field_label(ST_YEAR), "YEAR") &&
          str_eq(settime_field_label(ST_MONTH), "MONTH") &&
          str_eq(settime_field_label(ST_DAY), "DAY") &&
          str_eq(settime_field_label(ST_HOUR), "HOUR") &&
          str_eq(settime_field_label(ST_MIN), "MINUTE") &&
          str_eq(settime_field_label(ST_AMPM), "") &&
          str_eq(settime_field_label(ST_FIELDS), ""));

    /* The month plate is the abbreviation, not a number. */
    settime_begin(&t, 1, &now, 1);
    t.field = ST_MONTH;
    xpect(c, "month_text: the plate spells the month so 9 cannot be read as a "
             "day", sel_text_is(&t, "Sep"));
}

int main(void)
{
    xfail_ctx c = { "settime", 0, 0, 0 };

    check_seed(&c);
    check_wrap(&c);
    check_day_clamp(&c);
    check_twelve_hour(&c);
    check_select_walk(&c);

    return xfail_done(&c);
}
