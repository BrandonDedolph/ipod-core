/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/datetime.h — civil date <-> Unix epoch, integer only, 2000..2099.
 *
 * WHY THIS FILE EXISTS
 *
 * The device's only time-of-day source is the PMIC's RTC (hal/hw/rtc.c), which
 * hands over a BCD calendar; everything else in the firmware — the settings
 * record's host stamp, the timesync decision, the software clock, the Date &
 * Time editor — wants seconds. Converting between the two is the one piece of
 * this feature that is pure arithmetic, so it lives on its own and is the only
 * part that can be proved exhaustively: tests/kernel/datetime_test.c walks
 * every day from 2000-01-01 to 2099-12-31 against an independently accumulated
 * day counter.
 *
 * FREESTANDING. No libc, no time_t, no 64-bit division, no floating point. An
 * epoch here is a uint32_t of UTC seconds since 1970-01-01, which covers
 * 1970..2106; this module deliberately answers only for 2000..2099:
 *
 *   - below 2000 the RTC cannot represent the date at all (year register 00 is
 *     its reset value, which we read as "unset" — see hal/hw/rtc.h), and
 *   - 2100 is the century's non-leap year, which would make the four-year
 *     cycle below wrong. Excluding it is what lets `leap()` be `(y & 3) == 0`.
 *
 * So the supported civil range is 2000-01-01T00:00:00Z .. 2099-12-31T23:59:59Z
 * (946684800 .. 4102444799), and a value outside it is rejected rather than
 * wrapped or clamped: a date this code cannot represent must never be handed
 * back as a plausible-looking wrong one.
 *
 * TIME ZONES are not this module's business beyond datetime_local(): the RTC
 * holds UTC and the device stores a whole-minute offset next to it, so local
 * time is one addition performed on the epoch before conversion.
 */

#ifndef CORE_KERNEL_DATETIME_H
#define CORE_KERNEL_DATETIME_H

#include <stdint.h>

/* A civil date/time. `wday` is 0 = Sunday .. 6 = Saturday and is an OUTPUT of
 * this module — nothing reads it as an input (the RTC's own weekday register
 * is never trusted; see hal/hw/rtc.h). */
typedef struct {
    int year;    /* 2000..2099            */
    int month;   /* 1..12                 */
    int day;     /* 1..31                 */
    int hour;    /* 0..23                 */
    int min;     /* 0..59                 */
    int sec;     /* 0..59                 */
    int wday;    /* 0 = Sunday .. 6 = Sat */
} datetime_t;

/* 2000-01-01T00:00:00Z — a Saturday, which is where datetime_wday() counts
 * from. 2100-01-01T00:00:00Z is the exclusive end of the supported range and
 * still fits a uint32_t (4102444800 < 4294967296). */
#define DATETIME_EPOCH_2000  946684800u
#define DATETIME_EPOCH_2001  978307200u
#define DATETIME_EPOCH_2100  4102444800u

/* Buffer sizes for the formatters, NUL included:
 *   time  "12:05 PM"                  ->  9
 *   date  "Wednesday 16 September 2026" -> 27, rounded up */
#define DATETIME_TIME_MAX 9
#define DATETIME_DATE_MAX 32

/* 1 when `year` is a leap year. Exact within 2000..2099 only — see the header
 * comment on why 2100 is out of range. */
int datetime_leap(int year);

/* Days in `month` (1..12) of `year`; 0 for a month out of range. */
int datetime_mdays(int year, int month);

/* Day of week for a civil date, 0 = Sunday; -1 when the date is not in range.
 * Computed, never read off the chip. */
int datetime_wday(int year, int month, int day);

/* 1 when every field of `d` is in range for the others (a real calendar date
 * in 2000..2099 with a real time of day). `wday` is not checked: it is an
 * output field. */
int datetime_valid(const datetime_t *d);

/* Civil -> epoch. `d` must be valid (datetime_valid); an invalid date returns
 * 0, which is not a representable value in this range and so cannot be
 * mistaken for an answer. */
uint32_t datetime_to_epoch(const datetime_t *d);

/* Epoch -> civil, filling `wday` too. Returns 1, or 0 (leaving *d untouched)
 * when the epoch is outside 2000..2099. */
int datetime_from_epoch(uint32_t t, datetime_t *d);

/*
 * Local civil time from a UTC epoch and a whole-minute UTC offset
 * (-720..+840 — UTC-12:00 to UTC+14:00, the real-world range). Returns 0 when
 * the offset is out of range or the shifted epoch leaves 2000..2099.
 *
 * The addition is done in uint32 with no wrap risk: offsets are under 15 h and
 * the callers' epochs are 2001 or later, so neither end can cross a uint32
 * boundary before the range check catches it.
 */
#define DATETIME_OFF_MIN (-720)
#define DATETIME_OFF_MAX 840
int datetime_local(uint32_t utc, int off_min, datetime_t *d);

/*
 * "10:42 AM" / "12:05 PM" / "12:00 AM" (hour 0 reads 12), or "22:42" with
 * use_24h. Writes at most DATETIME_TIME_MAX bytes and returns the length; a
 * buffer too small writes "" and returns 0 — a truncated clock would be a
 * worse readout than none (ui/sleeptimer.c's token rule).
 */
int datetime_fmt_time(char *buf, int buf_sz, const datetime_t *d, int use_24h);

/* "Wednesday 16 September 2026" — full names, the editor's summary line.
 * Same buffer rule; needs DATETIME_DATE_MAX. */
int datetime_fmt_date(char *buf, int buf_sz, const datetime_t *d);

/* "Jan".."Dec" / "January".."December" / "Sunday".."Saturday"; "" when the
 * index is out of range, so a caller can always draw the result. */
const char *datetime_month_abbr(int month);
const char *datetime_month_name(int month);
const char *datetime_wday_name(int wday);

#endif /* CORE_KERNEL_DATETIME_H */
