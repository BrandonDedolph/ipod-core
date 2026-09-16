/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/datetime.c — civil date <-> Unix epoch. See datetime.h.
 */

#include "datetime.h"

/* Days elapsed before the 1st of each month in a NON-leap year. The leap day
 * is added separately (for March onwards) so this table serves both kinds of
 * year. */
static const uint16_t CUM_DAYS[12] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
};

static const uint8_t MDAYS[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

static const char *const MON_ABBR[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static const char *const MON_NAME[12] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December"
};

static const char *const WDAY_NAME[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
    "Saturday"
};

int datetime_leap(int year)
{
    /* Exact for 2000..2099 ONLY: 2000 is a leap year by the 400 rule and 2100
     * — the first year the & 3 test would get wrong — is outside the range
     * this module answers for. datetime_from_epoch enforces that. */
    return (year & 3) == 0;
}

int datetime_mdays(int year, int month)
{
    if (month < 1 || month > 12) {
        return 0;
    }
    return (int)MDAYS[month - 1] + ((month == 2) && datetime_leap(year) ? 1 : 0);
}

/* Whole days from 2000-01-01 to the given civil date. The caller has already
 * range-checked the date. */
static uint32_t days_since_2000(int year, int month, int day)
{
    uint32_t y = (uint32_t)(year - 2000);           /* 0..99 */
    /* (y + 3) / 4 = leap days in the years STRICTLY BEFORE y: y=1 -> 1 (2000's
     * own), y=4 -> 1 (2004's leap day is the CUM_DAYS/leap term below, not
     * this one), y=5 -> 2. */
    return y * 365u + (y + 3u) / 4u
         + (uint32_t)CUM_DAYS[month - 1]
         + (uint32_t)((month > 2 && datetime_leap(year)) ? 1 : 0)
         + (uint32_t)(day - 1);
}

int datetime_wday(int year, int month, int day)
{
    if (year < 2000 || year > 2099 || month < 1 || month > 12 ||
        day < 1 || day > datetime_mdays(year, month)) {
        return -1;
    }
    /* 2000-01-01 was a Saturday (wday 6), so day 0 of the count is 6. */
    return (int)((days_since_2000(year, month, day) + 6u) % 7u);
}

int datetime_valid(const datetime_t *d)
{
    if (d == 0) {
        return 0;
    }
    if (d->year < 2000 || d->year > 2099) {
        return 0;
    }
    if (d->month < 1 || d->month > 12) {
        return 0;
    }
    if (d->day < 1 || d->day > datetime_mdays(d->year, d->month)) {
        return 0;
    }
    if (d->hour < 0 || d->hour > 23) {
        return 0;
    }
    if (d->min < 0 || d->min > 59) {
        return 0;
    }
    if (d->sec < 0 || d->sec > 59) {
        return 0;
    }
    return 1;
}

uint32_t datetime_to_epoch(const datetime_t *d)
{
    if (!datetime_valid(d)) {
        /* 0 is 1970, which this range cannot produce, so it is unambiguous. */
        return 0;
    }
    return DATETIME_EPOCH_2000
         + days_since_2000(d->year, d->month, d->day) * 86400u
         + (uint32_t)d->hour * 3600u
         + (uint32_t)d->min * 60u
         + (uint32_t)d->sec;
}

int datetime_from_epoch(uint32_t t, datetime_t *d)
{
    if (d == 0 || t < DATETIME_EPOCH_2000 || t >= DATETIME_EPOCH_2100) {
        return 0;
    }

    uint32_t s    = t - DATETIME_EPOCH_2000;
    uint32_t days = s / 86400u;
    uint32_t rem  = s % 86400u;

    d->hour = (int)(rem / 3600u);
    d->min  = (int)((rem / 60u) % 60u);
    d->sec  = (int)(rem % 60u);
    d->wday = (int)((days + 6u) % 7u);       /* 2000-01-01 = Saturday */

    /* Four-year cycles of 1461 days. Within the range there is no century
     * exception, and each cycle STARTS with its leap year (2000, 2004, ...),
     * so the first 366 days of a cycle are that leap year and the remaining
     * 1095 split evenly into three 365-day years. */
    uint32_t cycle = days / 1461u;
    uint32_t r     = days % 1461u;
    uint32_t y     = cycle * 4u;
    if (r >= 366u) {
        r -= 366u;
        y += 1u + r / 365u;
        r %= 365u;
    }
    d->year = 2000 + (int)y;

    int m = 1;
    while (m < 12 && r >= (uint32_t)datetime_mdays(d->year, m)) {
        r -= (uint32_t)datetime_mdays(d->year, m);
        m++;
    }
    d->month = m;
    d->day   = (int)r + 1;
    return 1;
}

int datetime_local(uint32_t utc, int off_min, datetime_t *d)
{
    if (off_min < DATETIME_OFF_MIN || off_min > DATETIME_OFF_MAX) {
        return 0;
    }
    uint32_t local;
    if (off_min >= 0) {
        local = utc + (uint32_t)off_min * 60u;
    } else {
        uint32_t back = (uint32_t)(-off_min) * 60u;
        if (back > utc) {
            return 0;                        /* would underflow below 1970 */
        }
        local = utc - back;
    }
    return datetime_from_epoch(local, d);
}

int datetime_utc_from_local(uint32_t local, int off_min, uint32_t *utc)
{
    if (utc == 0 || off_min < DATETIME_OFF_MIN || off_min > DATETIME_OFF_MAX) {
        return 0;
    }
    uint32_t got;
    if (off_min >= 0) {
        uint32_t back = (uint32_t)off_min * 60u;
        if (back > local) {
            return 0;                        /* would underflow below 1970 */
        }
        got = local - back;
    } else {
        got = local + (uint32_t)(-off_min) * 60u;
    }
    /* The chip's range, not this module's: a UTC epoch in the year 2000 is the
     * register file's "unset" value and must not be written as a time. */
    if (got < DATETIME_EPOCH_2001 || got >= DATETIME_EPOCH_2100) {
        return 0;
    }
    *utc = got;
    return 1;
}

/* Write `v` (0..99) as exactly two zero-padded digits. Returns the number of
 * bytes written. */
static int put_pad2(char *buf, int v)
{
    buf[0] = (char)('0' + (v / 10) % 10);
    buf[1] = (char)('0' + v % 10);
    return 2;
}

/* Write `v` (0..9999) with no leading zeros. */
static int put_dec(char *buf, int v)
{
    char tmp[5];
    int  n = 0;
    unsigned u = (unsigned)v;
    do {
        tmp[n++] = (char)('0' + u % 10u);
        u /= 10u;
    } while (u != 0u && n < 5);
    for (int i = 0; i < n; i++) {
        buf[i] = tmp[n - 1 - i];
    }
    return n;
}

static int put_str(char *buf, const char *s)
{
    int n = 0;
    while (s[n] != '\0') {
        buf[n] = s[n];
        n++;
    }
    return n;
}

int datetime_fmt_time(char *buf, int buf_sz, const datetime_t *d, int use_24h)
{
    if (buf == 0 || buf_sz <= 0) {
        return 0;
    }
    buf[0] = '\0';
    if (d == 0 || d->hour < 0 || d->hour > 23 || d->min < 0 || d->min > 59) {
        return 0;
    }

    /* Longest output is "12:05 PM" (8) or "22:42" (5), NUL included below.
     * A short buffer draws nothing rather than half a clock. */
    if (buf_sz < (use_24h ? 6 : 9)) {
        return 0;
    }

    int n = 0;
    if (use_24h) {
        n += put_pad2(buf + n, d->hour);
        buf[n++] = ':';
        n += put_pad2(buf + n, d->min);
    } else {
        int h12 = d->hour % 12;
        if (h12 == 0) {
            h12 = 12;                        /* 00:xx and 12:xx both read 12 */
        }
        n += put_dec(buf + n, h12);
        buf[n++] = ':';
        n += put_pad2(buf + n, d->min);
        buf[n++] = ' ';
        buf[n++] = (d->hour < 12) ? 'A' : 'P';
        buf[n++] = 'M';
    }
    buf[n] = '\0';
    return n;
}

int datetime_fmt_date(char *buf, int buf_sz, const datetime_t *d)
{
    if (buf == 0 || buf_sz <= 0) {
        return 0;
    }
    buf[0] = '\0';
    if (!datetime_valid(d) || buf_sz < DATETIME_DATE_MAX) {
        return 0;
    }

    int wd = datetime_wday(d->year, d->month, d->day);
    int n  = 0;
    n += put_str(buf + n, datetime_wday_name(wd));
    buf[n++] = ' ';
    n += put_dec(buf + n, d->day);
    buf[n++] = ' ';
    n += put_str(buf + n, datetime_month_name(d->month));
    buf[n++] = ' ';
    n += put_dec(buf + n, d->year);
    buf[n] = '\0';
    return n;
}

const char *datetime_month_abbr(int month)
{
    return (month < 1 || month > 12) ? "" : MON_ABBR[month - 1];
}

const char *datetime_month_name(int month)
{
    return (month < 1 || month > 12) ? "" : MON_NAME[month - 1];
}

const char *datetime_wday_name(int wday)
{
    return (wday < 0 || wday > 6) ? "" : WDAY_NAME[wday];
}
