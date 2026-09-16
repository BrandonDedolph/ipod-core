/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/kernel/datetime_test.c — the civil<->epoch calendar (kernel/datetime.c)
 * on the host. The SAME source the ARM build links into core.elf.
 *
 * THE POINT OF THIS FILE. Date arithmetic is the one part of the clock feature
 * that can be settled completely off the device, and it is also the part where
 * a wrong answer is invisible: a leap-year rule that is off by one day does
 * not crash, it shows the wrong date on a Tuesday in March four years from
 * now. So this suite does not sample — it walks EVERY day from 2000-01-01 to
 * 2099-12-31 (36 525 of them) against an independently accumulated epoch and
 * an independently stepped weekday, in both directions, at both ends of the
 * day.
 *
 * The hand table above the loop is not redundant with it: the loop proves the
 * conversion is SELF-consistent with the month-length table, and the table
 * proves the month lengths, the epoch origin and the weekday phase are right
 * in the first place, against values derived outside this codebase.
 *
 * MUTATION CHECK (performed while writing): `leap()` as (y % 4) == 0 with 2100
 * included, the (y + 3) / 4 leap-day term as y / 4, the weekday origin as
 * Sunday instead of Saturday, and dropping the `month > 2` guard on the leap
 * term each fail the loop within the first year they affect; a 12-hour
 * formatter that prints hour 0 as "0:00 AM" fails the format cases.
 */

#include <stdio.h>

#include "datetime.h"
#include "../xfail.h"

/* Hand-derived vectors: civil date, its epoch, its weekday (0 = Sunday). */
typedef struct {
    int      y, mo, d, h, mi, s;
    uint32_t epoch;
    int      wday;
    const char *what;
} vector_t;

static const vector_t VECTORS[] = {
    { 2000,  1,  1,  0,  0,  0,  946684800u, 6, "the origin, a Saturday" },
    { 2000,  2, 29, 12,  0,  0,  951825600u, 2, "29 Feb 2000 (the 400 rule)" },
    { 2000,  3,  1,  0,  0,  0,  951868800u, 3, "1 Mar 2000, just after it" },
    { 2001,  1,  1,  0,  0,  0,  978307200u, 1, "the first year the RTC can hold" },
    { 2004,  2, 29, 23, 59, 59, 1078099199u, 0, "29 Feb 2004, last second" },
    { 2004,  3,  1,  0,  0,  0, 1078099200u, 1, "1 Mar 2004, the next second" },
    { 2026,  9, 16, 10, 42,  0, 1789555320u, 3, "the plan's Wednesday" },
    { 2038,  1, 19,  3, 14,  7, 2147483647u, 2, "the signed 32-bit roll" },
    { 2038,  1, 19,  3, 14,  8, 2147483648u, 2, "one second past it" },
    { 2099, 12, 31, 23, 59, 59, 4102444799u, 4, "the last second in range" },
};

static void check_vectors(xfail_ctx *c)
{
    int fwd = 1, rev = 1, wd = 1;
    for (unsigned i = 0; i < sizeof(VECTORS) / sizeof(VECTORS[0]); i++) {
        const vector_t *v = &VECTORS[i];
        datetime_t d = { v->y, v->mo, v->d, v->h, v->mi, v->s, 0 };

        uint32_t got = datetime_to_epoch(&d);
        if (got != v->epoch) {
            fprintf(stderr, "  %s: to_epoch %u, expected %u\n",
                    v->what, (unsigned)got, (unsigned)v->epoch);
            fwd = 0;
        }

        datetime_t back;
        if (!datetime_from_epoch(v->epoch, &back) ||
            back.year != v->y || back.month != v->mo || back.day != v->d ||
            back.hour != v->h || back.min != v->mi || back.sec != v->s ||
            back.wday != v->wday) {
            fprintf(stderr, "  %s: from_epoch %04d-%02d-%02d %02d:%02d:%02d "
                            "wday %d\n", v->what, back.year, back.month,
                    back.day, back.hour, back.min, back.sec, back.wday);
            rev = 0;
        }

        if (datetime_wday(v->y, v->mo, v->d) != v->wday) {
            fprintf(stderr, "  %s: wday %d, expected %d\n", v->what,
                    datetime_wday(v->y, v->mo, v->d), v->wday);
            wd = 0;
        }
    }
    xpect(c, "table: every hand-derived date converts to its epoch", fwd);
    xpect(c, "table: every epoch converts back to its date, weekday included",
          rev);
    xpect(c, "table: the weekday is computed, not stored", wd);
}

/* Every day of the century, both directions, both ends of the day. The epoch
 * and the weekday are accumulated here rather than asked of the module; the
 * month lengths the loop steps by are pinned separately, just below. */
static void check_exhaustive(xfail_ctx *c)
{
    uint32_t epoch = DATETIME_EPOCH_2000;
    int      wday  = 6;                 /* 2000-01-01 was a Saturday */
    long     days  = 0;
    int      ok    = 1;

    for (int y = 2000; y <= 2099 && ok; y++) {
        for (int m = 1; m <= 12 && ok; m++) {
            for (int d = 1; d <= datetime_mdays(y, m) && ok; d++) {
                datetime_t midnight = { y, m, d, 0, 0, 0, 0 };
                datetime_t last     = { y, m, d, 23, 59, 59, 0 };
                datetime_t back;

                if (datetime_to_epoch(&midnight) != epoch ||
                    datetime_to_epoch(&last) != epoch + 86399u) {
                    fprintf(stderr, "  %04d-%02d-%02d: to_epoch %u/%u, "
                                    "expected %u/%u\n", y, m, d,
                            (unsigned)datetime_to_epoch(&midnight),
                            (unsigned)datetime_to_epoch(&last),
                            (unsigned)epoch, (unsigned)(epoch + 86399u));
                    ok = 0;
                    break;
                }
                if (!datetime_from_epoch(epoch, &back) ||
                    back.year != y || back.month != m || back.day != d ||
                    back.hour != 0 || back.min != 0 || back.sec != 0 ||
                    back.wday != wday) {
                    fprintf(stderr, "  %04d-%02d-%02d: from_epoch %04d-%02d-%02d "
                                    "%02d:%02d:%02d wday %d (want wday %d)\n",
                            y, m, d, back.year, back.month, back.day,
                            back.hour, back.min, back.sec, back.wday, wday);
                    ok = 0;
                    break;
                }
                if (!datetime_from_epoch(epoch + 86399u, &back) ||
                    back.year != y || back.month != m || back.day != d ||
                    back.hour != 23 || back.min != 59 || back.sec != 59) {
                    fprintf(stderr, "  %04d-%02d-%02d 23:59:59 round-trip\n",
                            y, m, d);
                    ok = 0;
                    break;
                }
                if (datetime_wday(y, m, d) != wday) {
                    fprintf(stderr, "  %04d-%02d-%02d: wday %d, expected %d\n",
                            y, m, d, datetime_wday(y, m, d), wday);
                    ok = 0;
                    break;
                }

                wday   = (wday + 1) % 7;
                epoch += 86400u;
                days++;
            }
        }
    }

    xpect(c, "exhaustive: every day 2000-01-01..2099-12-31 converts both ways "
             "and its weekday follows the one before", ok);
    xpect(c, "exhaustive: the century is 36525 days and ends at EPOCH_2100",
          days == 36525 && epoch == DATETIME_EPOCH_2100);
}

static void check_mdays(xfail_ctx *c)
{
    static const int LEN[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int ok = 1;
    for (int m = 1; m <= 12; m++) {
        if (datetime_mdays(2001, m) != LEN[m - 1]) {
            ok = 0;
        }
    }
    xpect(c, "mdays: every month of a non-leap year", ok);
    xpect(c, "mdays: February is 29 days in 2000, 2004 and 2096, 28 in 2001",
          datetime_mdays(2000, 2) == 29 && datetime_mdays(2004, 2) == 29 &&
          datetime_mdays(2096, 2) == 29 && datetime_mdays(2001, 2) == 28);
    xpect(c, "mdays: a month out of range is 0, not a guess",
          datetime_mdays(2026, 0) == 0 && datetime_mdays(2026, 13) == 0);
}

static void check_range(xfail_ctx *c)
{
    datetime_t d;
    xpect(c, "range: the second before 2000 and the first of 2100 are refused",
          datetime_from_epoch(DATETIME_EPOCH_2000 - 1u, &d) == 0 &&
          datetime_from_epoch(DATETIME_EPOCH_2100, &d) == 0);
    xpect(c, "range: the two seconds on either side of them are accepted",
          datetime_from_epoch(DATETIME_EPOCH_2000, &d) == 1 &&
          datetime_from_epoch(DATETIME_EPOCH_2100 - 1u, &d) == 1);

    /* A date that does not exist must not convert to a plausible epoch. */
    datetime_t feb30 = { 2026, 2, 30, 0, 0, 0, 0 };
    datetime_t feb29 = { 2001, 2, 29, 0, 0, 0, 0 };
    datetime_t h24   = { 2026, 9, 16, 24, 0, 0, 0 };
    datetime_t y1999 = { 1999, 12, 31, 0, 0, 0, 0 };
    xpect(c, "range: 30 Feb, 29 Feb 2001, hour 24 and the year 1999 are all "
             "invalid and convert to 0",
          !datetime_valid(&feb30) && datetime_to_epoch(&feb30) == 0 &&
          !datetime_valid(&feb29) && datetime_to_epoch(&feb29) == 0 &&
          !datetime_valid(&h24)   && datetime_to_epoch(&h24) == 0 &&
          !datetime_valid(&y1999) && datetime_to_epoch(&y1999) == 0);
    xpect(c, "range: datetime_wday refuses what datetime_valid refuses",
          datetime_wday(2001, 2, 29) == -1 && datetime_wday(1999, 1, 1) == -1 &&
          datetime_wday(2100, 1, 1) == -1);
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void check_format(xfail_ctx *c)
{
    static const struct {
        int hour, min;
        const char *h12, *h24;
    } CASES[] = {
        {  0,  0, "12:00 AM", "00:00" },
        {  0,  5, "12:05 AM", "00:05" },
        {  9,  7,  "9:07 AM", "09:07" },
        { 10, 42, "10:42 AM", "10:42" },
        { 11, 59, "11:59 AM", "11:59" },
        { 12,  5, "12:05 PM", "12:05" },
        { 13,  0,  "1:00 PM", "13:00" },
        { 22, 42, "10:42 PM", "22:42" },
        { 23, 59, "11:59 PM", "23:59" },
    };
    char buf[DATETIME_TIME_MAX];
    int ok = 1;
    for (unsigned i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        datetime_t d = { 2026, 9, 16, CASES[i].hour, CASES[i].min, 0, 0 };
        datetime_fmt_time(buf, (int)sizeof(buf), &d, 0);
        if (!str_eq(buf, CASES[i].h12)) {
            fprintf(stderr, "  12h %02d:%02d -> \"%s\", expected \"%s\"\n",
                    CASES[i].hour, CASES[i].min, buf, CASES[i].h12);
            ok = 0;
        }
        datetime_fmt_time(buf, (int)sizeof(buf), &d, 1);
        if (!str_eq(buf, CASES[i].h24)) {
            fprintf(stderr, "  24h %02d:%02d -> \"%s\", expected \"%s\"\n",
                    CASES[i].hour, CASES[i].min, buf, CASES[i].h24);
            ok = 0;
        }
    }
    xpect(c, "format: midnight and noon both read 12 in 12-hour, and the "
             "24-hour form is always two digits", ok);

    /* A buffer that cannot hold the whole string draws NOTHING. */
    datetime_t noon = { 2026, 9, 16, 12, 5, 0, 0 };
    char tiny[6];
    tiny[0] = 'x';
    xpect(c, "format: a short buffer writes an empty string, not half a clock",
          datetime_fmt_time(tiny, (int)sizeof(tiny), &noon, 0) == 0 &&
          tiny[0] == '\0');
    xpect(c, "format: six bytes is enough for the 24-hour form",
          datetime_fmt_time(tiny, (int)sizeof(tiny), &noon, 1) == 5 &&
          str_eq(tiny, "12:05"));

    char date[DATETIME_DATE_MAX];
    datetime_t wed = { 2026, 9, 16, 10, 42, 0, 0 };
    datetime_t new_year = { 2026, 1, 1, 0, 0, 0, 0 };
    int n = datetime_fmt_date(date, (int)sizeof(date), &wed);
    xpect(c, "format: the summary line spells the day and month out",
          n == 27 && str_eq(date, "Wednesday 16 September 2026"));
    datetime_fmt_date(date, (int)sizeof(date), &new_year);
    xpect(c, "format: a single-digit day has no leading zero",
          str_eq(date, "Thursday 1 January 2026"));
    xpect(c, "format: month and weekday names out of range are empty strings",
          str_eq(datetime_month_abbr(0), "") &&
          str_eq(datetime_month_abbr(13), "") &&
          str_eq(datetime_month_name(0), "") &&
          str_eq(datetime_wday_name(7), "") &&
          str_eq(datetime_month_abbr(9), "Sep"));
}

static void check_local(xfail_ctx *c)
{
    /* 2026-09-16 10:42:00 UTC, the plan's moment, seen from three zones. */
    const uint32_t utc = 1789555320u;
    datetime_t d;

    xpect(c, "local: UTC+14:00 (+840) is the next day, 00:42",
          datetime_local(utc, 840, &d) && d.year == 2026 && d.month == 9 &&
          d.day == 17 && d.hour == 0 && d.min == 42 && d.wday == 4);
    xpect(c, "local: UTC-12:00 (-720) is the previous evening, 22:42",
          datetime_local(utc, -720, &d) && d.day == 15 && d.hour == 22 &&
          d.min == 42 && d.wday == 2);
    xpect(c, "local: a half-hour zone (+05:30) moves the minutes",
          datetime_local(utc, 330, &d) && d.day == 16 && d.hour == 16 &&
          d.min == 12);
    xpect(c, "local: offset 0 is the UTC date",
          datetime_local(utc, 0, &d) && d.hour == 10 && d.min == 42);
    xpect(c, "local: an offset no zone has is refused, not clamped",
          !datetime_local(utc, 841, &d) && !datetime_local(utc, -721, &d));
    xpect(c, "local: a shift that would leave the supported range is refused",
          !datetime_local(DATETIME_EPOCH_2100 - 60u, 840, &d) &&
          !datetime_local(DATETIME_EPOCH_2000 + 60u, -720, &d));
}

int main(void)
{
    xfail_ctx c = { "datetime", 0, 0, 0 };

    check_vectors(&c);
    check_mdays(&c);
    check_exhaustive(&c);
    check_range(&c);
    check_format(&c);
    check_local(&c);

    return xfail_done(&c);
}
