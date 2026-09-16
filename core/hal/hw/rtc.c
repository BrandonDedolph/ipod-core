/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/rtc.c — PCF50605 real-time clock over I²C. See rtc.h for the
 * map's provenance and docs/hw/06-power.md for the table and the bench.
 *
 * Freestanding: <stdint.h>, the i2c.c seam and kernel/datetime.c's integer
 * calendar maths. No libc, no hardware access except through i2c.c, so the
 * whole file host-compiles for the golden-trace test the way battery.c does.
 */

#include <stdint.h>

#include "i2c.h"
#include "rtc.h"
#include "hal.h"                       /* hal_rtc_get / hal_rtc_set contract */
#include "../../kernel/datetime.h"

/* ---------- PCF50605 RTC registers ----------------------------------
 *
 * Driver-local #defines, not pp5022.h symbols: these are I²C register indices
 * inside another chip, not SoC addresses, and tests/scripts/
 * check_hw_consistency.py scans pp5022.h's *_ADDR symbols against docs/hw/.
 * battery.c's PMU_* block is the convention being followed.
 *
 * Confidence (see 06-power.md for the full table):
 *   SC/MN/DT/MT/YR  high    — datasheet map, cross-checked block placement
 *   HR              high, but 24-hour format is medium: to confirm that no
 *                             12-hour/AM-PM select bit is in play
 *   WD              medium  — the base day is unknown, so we write Sunday=0
 *                             and never read it
 *   YR == 00        to confirm — taken as the reset value, i.e. "unset"
 */
#define PMU_ADDR   0x08      /* the PMIC's 7-bit bus address (06-power.md) */
#define PMU_RTCSC  0x0A      /* seconds 00..59, BCD                        */
#define PMU_RTCMN  0x0B      /* minutes 00..59, BCD                        */
#define PMU_RTCHR  0x0C      /* hours   00..23, BCD, 24-hour               */
#define PMU_RTCWD  0x0D      /* weekday 0..6, plain binary                 */
#define PMU_RTCDT  0x0E      /* day     01..31, BCD                        */
#define PMU_RTCMT  0x0F      /* month   01..12, BCD                        */
#define PMU_RTCYR  0x10      /* year    00..99 = 2000..2099, BCD           */

/* The chunking the 4-byte controller forces (09-i2c.md): SC MN HR WD in one
 * transaction, DT MT YR in the next. */
#define RTC_HEAD_LEN 4       /* SC MN HR WD */
#define RTC_TAIL_LEN 3       /* DT MT YR    */

/* Indices into the raw byte array handed around here and by rtc_read_raw. */
enum { R_SC = 0, R_MN, R_HR, R_WD, R_DT, R_MT, R_YR };

/* ---------- BCD ------------------------------------------------------ */

/* Two packed decimal digits -> 0..99, or -1 when either nibble is not a digit.
 * The strict check is load-bearing: it is what turns a wrong register map, an
 * absent PMU or a drained cell into "no time known" instead of a wrong date. */
static int bcd_to_int(uint8_t b)
{
    int hi = (b >> 4) & 0x0F;
    int lo = b & 0x0F;
    if (hi > 9 || lo > 9) {
        return -1;
    }
    return hi * 10 + lo;
}

static uint8_t int_to_bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

/* ---------- Read ----------------------------------------------------- */

/*
 * One pass over the calendar: SC MN HR WD, then DT MT YR, then SC again.
 *
 * The third read is the tear check. The two chunks are separate bus
 * transactions, so a second boundary between them can hand back a time from
 * before the carry and a date from after it — at 23:59:59 that is a whole day
 * wrong. If the seconds moved BACKWARDS between the first read and the last,
 * a carry happened in the middle and the sample is discarded.
 *
 * Returns 0 for a clean pass, 1 for a torn one (the caller re-reads once), -1
 * when the bus did not answer.
 */
static int rtc_read_once(uint8_t regs[RTC_REG_COUNT])
{
    uint8_t head[RTC_HEAD_LEN];
    uint8_t tail[RTC_TAIL_LEN];
    uint8_t again[1];

    if (i2c_read(PMU_ADDR, PMU_RTCSC, head, RTC_HEAD_LEN) != 0) {
        return -1;
    }
    if (i2c_read(PMU_ADDR, PMU_RTCDT, tail, RTC_TAIL_LEN) != 0) {
        return -1;
    }
    if (i2c_read(PMU_ADDR, PMU_RTCSC, again, 1) != 0) {
        return -1;
    }

    for (int i = 0; i < RTC_HEAD_LEN; i++) {
        regs[i] = head[i];
    }
    for (int i = 0; i < RTC_TAIL_LEN; i++) {
        regs[RTC_HEAD_LEN + i] = tail[i];
    }

    int first = bcd_to_int(head[0]);
    int last  = bcd_to_int(again[0]);
    if (first >= 0 && last >= 0 && last < first) {
        return 1;                              /* the seconds carried: torn */
    }
    /* Bytes that are not BCD are NOT a tear — there is nothing to re-read our
     * way out of, and rtc_read()'s validity gate will call it unset. */
    return 0;
}

int rtc_read_raw(uint8_t regs[RTC_REG_COUNT])
{
    int rc = rtc_read_once(regs);
    if (rc == 1) {
        /* Exactly one retry. A carry cannot happen twice in the microseconds
         * two passes take, and an unbounded retry on a bus that is lying to us
         * would spin. The second pass's bytes are what we return. */
        rc = rtc_read_once(regs);
        if (rc == 1) {
            rc = 0;
        }
    }
    return rc;
}

int rtc_read(datetime_t *d)
{
    uint8_t regs[RTC_REG_COUNT];
    if (rtc_read_raw(regs) != 0) {
        return -1;
    }
    return rtc_decode(regs, d);
}

int rtc_decode(const uint8_t regs[RTC_REG_COUNT], datetime_t *d)
{
    int sec   = bcd_to_int(regs[R_SC]);
    int min   = bcd_to_int(regs[R_MN]);
    int hour  = bcd_to_int(regs[R_HR]);
    int day   = bcd_to_int(regs[R_DT]);
    int month = bcd_to_int(regs[R_MT]);
    int year  = bcd_to_int(regs[R_YR]);

    if (sec < 0 || min < 0 || hour < 0 || day < 0 || month < 0 || year < 0) {
        return 0;                              /* not BCD: nothing is known */
    }
    /* Year 00 is the register file's reset value, so a clock that has lost its
     * cell reads as the year 2000 — which is exactly why 2000 is not a year we
     * accept. The representable range on this chip is 2001..2099. */
    if (year == 0) {
        return 0;
    }

    datetime_t got;
    got.year  = 2000 + year;
    got.month = month;
    got.day   = day;
    got.hour  = hour;
    got.min   = min;
    got.sec   = sec;
    got.wday  = 0;
    if (!datetime_valid(&got)) {
        return 0;
    }
    /* Recomputed, never read: see rtc.h on the weekday register. */
    got.wday = datetime_wday(got.year, got.month, got.day);

    *d = got;
    return 1;
}

/* ---------- Write ---------------------------------------------------- */

static int rtc_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t msg[2] = { reg, value };
    return i2c_send(PMU_ADDR, msg, 2) == 0 ? 0 : -1;
}

int rtc_write(const datetime_t *d)
{
    if (!datetime_valid(d) || d->year < 2001) {
        return -1;
    }

    int wday = datetime_wday(d->year, d->month, d->day);

    /* Seconds first — see rtc.h. The order is the correctness argument, so it
     * is spelled out as a table rather than hidden in seven calls. */
    const uint8_t writes[RTC_REG_COUNT][2] = {
        { PMU_RTCSC, int_to_bcd(d->sec) },
        { PMU_RTCMN, int_to_bcd(d->min) },
        { PMU_RTCHR, int_to_bcd(d->hour) },
        { PMU_RTCWD, (uint8_t)wday },          /* plain binary, not BCD */
        { PMU_RTCDT, int_to_bcd(d->day) },
        { PMU_RTCMT, int_to_bcd(d->month) },
        { PMU_RTCYR, int_to_bcd(d->year - 2000) },
    };
    for (int i = 0; i < RTC_REG_COUNT; i++) {
        if (rtc_write_reg(writes[i][0], writes[i][1]) != 0) {
            return -1;
        }
    }

    /* i2c_send returns before the transaction is on the wire (09-i2c.md: the
     * write path is completed lazily by the next transaction's leading
     * BUSY-wait). A one-byte read of YR is the cheapest way to make the last
     * write land before the caller compares anything. */
    uint8_t back[1];
    if (i2c_read(PMU_ADDR, PMU_RTCYR, back, 1) != 0) {
        return -1;
    }
    return 0;
}

/* ---------- The HAL contract ----------------------------------------- */

int hal_rtc_get(uint32_t *epoch)
{
    if (epoch == 0) {
        return 0;
    }
    datetime_t d;
    int rc = rtc_read(&d);
    if (rc != 1) {
        return rc;                             /* 0 unset, -1 no answer */
    }
    *epoch = datetime_to_epoch(&d);
    return 1;
}

int hal_rtc_set(uint32_t epoch)
{
    datetime_t d;
    if (epoch < DATETIME_EPOCH_2001 || epoch >= DATETIME_EPOCH_2100 ||
        !datetime_from_epoch(epoch, &d)) {
        return -3;
    }

    if (rtc_write(&d) != 0) {
        return -1;
    }

    /* Read back and compare. The write takes a few hundred microseconds, so
     * the seconds may have ticked once by the time we look: ±1 s is agreement,
     * anything else means the write did not take (a write-enable we do not
     * know about, or a map that is wrong in the write direction). */
    datetime_t back;
    int rc = rtc_read(&back);
    if (rc < 0) {
        return -1;
    }
    if (rc == 0) {
        return -2;
    }
    uint32_t got = datetime_to_epoch(&back);
    uint32_t diff = (got > epoch) ? (got - epoch) : (epoch - got);
    return (diff <= 1u) ? 0 : -2;
}
