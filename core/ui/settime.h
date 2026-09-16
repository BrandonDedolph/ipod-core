/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/settime.h — the Set Date & Time field editor: model and painter.
 *
 * Settings > Date & Time > Set Date & Time walks five plates (year, month,
 * day, hour, minute) or six (a trailing AM/PM plate in 12-hour mode). The
 * wheel changes the selected field, Select confirms it and moves on, Select on
 * the last field commits the whole date, and Menu cancels without writing
 * anything.
 *
 * The MODEL below is pure — no framebuffer, no hardware, no settings_t — for
 * the reason ui/keyhold.c and ui/jackwatch.c are: a field editor is all edge
 * cases (31 January stepped into February, midnight in 12-hour mode, the year
 * that must clamp where every other field wraps) and none of them are worth
 * discovering by standing at the bench turning a wheel. The PAINTER lives
 * beside the other Settings painters in ui/screen_settings.c and draws only
 * from this struct.
 *
 * WHAT IT DOES NOT DO: it never touches the clock. The caller reads the fields
 * out with settime_civil() and writes them to the RTC itself, because the
 * conversion from the local civil time shown here to the UTC the chip holds
 * needs the device's stored UTC offset, which is settings state.
 *
 * DISPLAY: day, hour and minute are two digits, zero-padded, because the
 * plates are a fixed-width row and a field that changes width under the wheel
 * reads as the layout twitching. Month is the three-letter abbreviation (the
 * widget has no room for "September" and a number would be ambiguous), year is
 * four digits.
 */

#ifndef CORE_UI_SETTIME_H
#define CORE_UI_SETTIME_H

#include "../kernel/datetime.h"

/* Field order, left to right. ST_AMPM exists only in 12-hour mode — see
 * settime_field_count(). */
enum { ST_YEAR = 0, ST_MONTH, ST_DAY, ST_HOUR, ST_MIN, ST_AMPM, ST_FIELDS };

/* The editable year range: what the RTC can hold (year register 00 is its
 * "unset" reset value, so 2000 is not offered). */
#define SETTIME_YEAR_MIN 2001
#define SETTIME_YEAR_MAX 2099

/* One wheel event may carry several detents; the sliders clamp the same way
 * (kernel/main.c), so a flick cannot spin the year by fifty. */
#define SETTIME_DELTA_MAX 4

/* Longest field text is the year: "2026" + NUL. */
#define SETTIME_FIELD_MAX 8

typedef struct {
    int year;      /* 2001..2099                                          */
    int month;     /* 1..12                                               */
    int day;       /* 1..mdays(year, month) — re-clamped after every move */
    int hour;      /* 0..23 ALWAYS, whatever use_12h says. The AM/PM plate
                    * is a view of this, not a second field of state.     */
    int min;       /* 0..59                                               */
    int field;     /* the selected ST_* field                             */
    int use_12h;   /* 1 when the AM/PM plate is shown                     */
} settime_t;

/*
 * Seed the editor. With `valid` the fields come from `now` (seconds dropped:
 * the editor always sets a whole minute); otherwise from a fixed, plainly
 * wrong-looking default that the user has to walk anyway.
 *
 * The CALLER decides what `now` is — the running clock if there is one, else
 * the host's stamp if there is one. That choice needs the settings record, so
 * it is not made here.
 */
void settime_begin(settime_t *t, int valid, const datetime_t *now,
                   int use_12h);

/*
 * A wheel event on the selected field. `delta` is clamped to
 * ±SETTIME_DELTA_MAX. Every field WRAPS except the year, which CLAMPS at
 * SETTIME_YEAR_MIN / _MAX — a year that wrapped from 2099 to 2001 under a
 * flick would be a silent forty-year error, while a month that wraps is
 * obviously a month. The day is re-clamped to the month's length after a month
 * or year move, so 31 January stepped to February becomes the 28th (or the
 * 29th in a leap year) rather than a date that does not exist.
 *
 * In 12-hour mode the hour field cycles within the half-day the AM/PM plate
 * shows (11 AM steps to 12 AM), so the two plates stay independent; in 24-hour
 * mode it wraps 0..23. Any non-zero delta on the AM/PM plate flips it an odd
 * number of times, i.e. flips it.
 *
 * Returns 1 when something changed.
 */
int settime_adjust(settime_t *t, int delta);

/* Select: confirm this field and move to the next. Returns 1 when the LAST
 * field was confirmed — the caller's cue to commit and leave. The selection
 * stays on the last field so a re-paint on the way out is sane. */
int settime_next(settime_t *t);

/* 6 with the AM/PM plate, 5 without. */
int settime_field_count(const settime_t *t);

/* The edited value as a civil date, seconds zeroed and the weekday computed. */
void settime_civil(const settime_t *t, datetime_t *out);

/*
 * The text on field `f`'s plate: "2026", "Sep", "16", "10", "42", "AM".
 * Writes "" and returns 0 for a field the current mode does not show or a
 * buffer under SETTIME_FIELD_MAX. Returns the length.
 */
int settime_field_text(const settime_t *t, int f, char *buf, int buf_sz);

/* The caption under a plate: "YEAR", "MONTH", "DAY", "HOUR", "MINUTE"; "" for
 * ST_AMPM, which is captioned by its own value. */
const char *settime_field_label(int f);

/*
 * Paint the whole 320x240 editor. Implemented in ui/screen_settings.c beside
 * the other Settings painters (same palette tokens, same header, same
 * selection inversion); declared here because the model is what it draws.
 *
 * Leaves the status band clear, like every other Settings screen: the firmware
 * paints the strip over it afterwards.
 */
void settime_render(const settime_t *t);

#endif /* CORE_UI_SETTIME_H */
