/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/settime.c — the Set Date & Time field editor's model. See settime.h.
 * The painter is in ui/screen_settings.c.
 */

#include "settime.h"

/* The seed when no clock is known. Deliberately a round, plainly artificial
 * moment rather than "now-ish": the user is about to set every field anyway,
 * and a default that looked like a real time would be mistaken for one. */
#define SETTIME_DEF_YEAR  2026
#define SETTIME_DEF_MONTH 1
#define SETTIME_DEF_DAY   1
#define SETTIME_DEF_HOUR  12
#define SETTIME_DEF_MIN   0

static const char *const FIELD_LABEL[ST_FIELDS] = {
    "YEAR", "MONTH", "DAY", "HOUR", "MINUTE", ""
};

/* Wrap `v` into [0, n) for any delta within ±SETTIME_DELTA_MAX. */
static int wrap(int v, int n)
{
    v %= n;
    if (v < 0) {
        v += n;
    }
    return v;
}

static void clamp_day(settime_t *t)
{
    int mx = datetime_mdays(t->year, t->month);
    if (t->day > mx) {
        t->day = mx;
    }
    if (t->day < 1) {
        t->day = 1;
    }
}

void settime_begin(settime_t *t, int valid, const datetime_t *now,
                   int use_12h)
{
    t->field   = ST_YEAR;
    t->use_12h = use_12h ? 1 : 0;

    if (valid && now != 0 && datetime_valid(now) &&
        now->year >= SETTIME_YEAR_MIN) {
        t->year  = now->year;
        t->month = now->month;
        t->day   = now->day;
        t->hour  = now->hour;
        t->min   = now->min;       /* seconds dropped: we set a whole minute */
    } else {
        t->year  = SETTIME_DEF_YEAR;
        t->month = SETTIME_DEF_MONTH;
        t->day   = SETTIME_DEF_DAY;
        t->hour  = SETTIME_DEF_HOUR;
        t->min   = SETTIME_DEF_MIN;
    }
    clamp_day(t);
}

int settime_field_count(const settime_t *t)
{
    return t->use_12h ? ST_FIELDS : ST_FIELDS - 1;
}

int settime_adjust(settime_t *t, int delta)
{
    if (delta > SETTIME_DELTA_MAX) {
        delta = SETTIME_DELTA_MAX;
    } else if (delta < -SETTIME_DELTA_MAX) {
        delta = -SETTIME_DELTA_MAX;
    }
    if (delta == 0) {
        return 0;
    }

    settime_t was = *t;

    switch (t->field) {
    case ST_YEAR:
        /* Clamps, never wraps — see settime.h. */
        t->year += delta;
        if (t->year < SETTIME_YEAR_MIN) {
            t->year = SETTIME_YEAR_MIN;
        } else if (t->year > SETTIME_YEAR_MAX) {
            t->year = SETTIME_YEAR_MAX;
        }
        clamp_day(t);                    /* 29 Feb in a year that is not */
        break;

    case ST_MONTH:
        t->month = wrap(t->month - 1 + delta, 12) + 1;
        clamp_day(t);                    /* 31 Jan -> 28/29 Feb */
        break;

    case ST_DAY:
        t->day = wrap(t->day - 1 + delta, datetime_mdays(t->year, t->month)) + 1;
        break;

    case ST_HOUR:
        if (t->use_12h) {
            /* Cycle within the half-day the AM/PM plate shows, so the two
             * plates never move each other. */
            int pm  = (t->hour >= 12);
            int h12 = wrap(t->hour % 12 + delta, 12);
            t->hour = h12 + (pm ? 12 : 0);
        } else {
            t->hour = wrap(t->hour + delta, 24);
        }
        break;

    case ST_MIN:
        t->min = wrap(t->min + delta, 60);
        break;

    case ST_AMPM:
        /* A two-value field: an even number of detents lands where it started,
         * which is what wrapping means here. */
        if (delta & 1) {
            t->hour = (t->hour >= 12) ? t->hour - 12 : t->hour + 12;
        }
        break;

    default:
        return 0;
    }

    return (t->year != was.year || t->month != was.month ||
            t->day != was.day || t->hour != was.hour || t->min != was.min);
}

int settime_next(settime_t *t)
{
    int last = settime_field_count(t) - 1;
    if (t->field >= last) {
        t->field = last;
        return 1;
    }
    t->field++;
    return 0;
}

void settime_civil(const settime_t *t, datetime_t *out)
{
    out->year  = t->year;
    out->month = t->month;
    out->day   = t->day;
    out->hour  = t->hour;
    out->min   = t->min;
    out->sec   = 0;                      /* the editor sets a whole minute */
    out->wday  = datetime_wday(t->year, t->month, t->day);
}

/* Two zero-padded digits. */
static int put_pad2(char *buf, int v)
{
    buf[0] = (char)('0' + (v / 10) % 10);
    buf[1] = (char)('0' + v % 10);
    buf[2] = '\0';
    return 2;
}

int settime_field_text(const settime_t *t, int f, char *buf, int buf_sz)
{
    if (buf == 0 || buf_sz < SETTIME_FIELD_MAX) {
        return 0;
    }
    buf[0] = '\0';
    if (f < 0 || f >= settime_field_count(t)) {
        return 0;                        /* ST_AMPM in 24-hour mode */
    }

    switch (f) {
    case ST_YEAR: {
        int y = t->year;
        buf[0] = (char)('0' + (y / 1000) % 10);
        buf[1] = (char)('0' + (y / 100) % 10);
        buf[2] = (char)('0' + (y / 10) % 10);
        buf[3] = (char)('0' + y % 10);
        buf[4] = '\0';
        return 4;
    }
    case ST_MONTH: {
        const char *m = datetime_month_abbr(t->month);
        int n = 0;
        while (m[n] != '\0' && n < buf_sz - 1) {
            buf[n] = m[n];
            n++;
        }
        buf[n] = '\0';
        return n;
    }
    case ST_DAY:
        return put_pad2(buf, t->day);
    case ST_HOUR: {
        if (!t->use_12h) {
            return put_pad2(buf, t->hour);
        }
        int h12 = t->hour % 12;
        if (h12 == 0) {
            h12 = 12;                    /* midnight and noon both read 12 */
        }
        return put_pad2(buf, h12);
    }
    case ST_MIN:
        return put_pad2(buf, t->min);
    case ST_AMPM:
        buf[0] = (t->hour < 12) ? 'A' : 'P';
        buf[1] = 'M';
        buf[2] = '\0';
        return 2;
    default:
        return 0;
    }
}

const char *settime_field_label(int f)
{
    return (f < 0 || f >= ST_FIELDS) ? "" : FIELD_LABEL[f];
}
