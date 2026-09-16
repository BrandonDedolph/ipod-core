/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/rtc.h — the PCF50605 PMIC's real-time clock, over the same I²C
 * bus the battery gauge and the codec use.
 *
 * The chip keeps a BCD calendar in its always-on domain, so it counts through
 * a suspend and through a PMU standby (docs/hw/06-power.md, "State across
 * sleep"). This is the only time-of-day source on the device: there is no
 * network, and the cable is Apple's ROM disk mode, so the host cannot talk to
 * us while it is plugged in.
 *
 * THE REGISTER MAP IS NOT CONFIRMED ON HARDWARE. Every address here is derived
 * from the public NXP PCF50606 datasheet (same family) and cross-checked
 * against the six PCF50605 registers docs/hw/06-power.md already documents,
 * which all sit at their datasheet addresses. That cross-check is evidence,
 * not proof. What protects us is the validity gate in rtc_read(): a wrong map
 * reads as "unset" (no time known), never as a plausible wrong time, and the
 * only writes this driver makes land inside 0x0A..0x10 whose neighbours are
 * OOCC2 (0x09) and the alarm block (0x11+). See 06-power.md, "Real-time
 * clock", for the table with per-register confidence levels and the bench
 * procedure that settles it.
 *
 * Freestanding: <stdint.h> plus the i2c.c seam, so this host-compiles under
 * -DMMIO_MOCK for tests/hw_mmio/rtc_trace_test.c exactly as battery.c does.
 */

#ifndef CORE_HAL_HW_RTC_H
#define CORE_HAL_HW_RTC_H

#include <stdint.h>

#include "../../kernel/datetime.h"

/* The seven calendar registers, in address order — SC MN HR WD DT MT YR. */
#define RTC_REG_COUNT 7

/*
 * Read the raw BCD calendar bytes as they come off the chip, SC..YR, with the
 * torn-read retry applied but NO validation: this is what the boot line prints
 * on the first flash, before anything trusts the map.
 *
 * Returns 0 when the bus answered, -1 when it did not. Note that "the bus
 * answered" is weaker than it sounds — i2c_read() cannot see a NACK
 * (docs/hw/09-i2c.md), so an absent PMU yields the DATA registers' last
 * written bytes. Those decode as invalid BCD, which is why the caller must use
 * rtc_read() rather than trusting these bytes.
 */
int rtc_read_raw(uint8_t regs[RTC_REG_COUNT]);

/*
 * Decode bytes that rtc_read_raw() returned, with no bus traffic of its own.
 * Returns 1 and fills *d for a plausible date, 0 for "unset" — see rtc_read()
 * for what makes a reading unset, since this is the half that decides it.
 *
 * Exposed so the boot path can LOG the raw bytes and decide from exactly those
 * bytes rather than reading the chip a second time: two passes could disagree,
 * and the log line is the evidence the register map is right at all.
 */
int rtc_decode(const uint8_t regs[RTC_REG_COUNT], datetime_t *d);

/*
 * The calendar as a civil date. Returns:
 *   1  — a running clock holding a plausible date (2001..2099);
 *   0  — UNSET: year register 00 (the reset value a drained cell leaves), or
 *        bytes that are not a date at all (bad BCD, hour 25, 31 February);
 *  -1  — the bus did not answer. *d is untouched for 0 and -1.
 *
 * The weekday register is IGNORED on read and `d->wday` is recomputed from the
 * date: we know what we write into it, but not what Apple's firmware or a
 * drained cell left there, and nothing needs it to be right.
 */
int rtc_read(datetime_t *d);

/*
 * Set the calendar. Seven one-register writes in the order SC, MN, HR, WD, DT,
 * MT, YR — seconds FIRST so the second counter restarts and the next minute
 * carry is a whole second away while the remaining six writes go out in
 * microseconds, which is what makes the date safe to write without a
 * stop-the-clock bit (the datasheet has none). Then a one-byte read-back of YR
 * to flush the lazily-completed write path (docs/hw/09-i2c.md).
 *
 * `d` must be a valid 2001..2099 date; its `wday` is ignored and Sunday=0 is
 * written from the date. Returns 0, or -1 on a bus error / an invalid date.
 * The read-back COMPARE is hal_rtc_set()'s job, not this one's.
 */
int rtc_write(const datetime_t *d);

/*
 * ALARM — tabled, not implemented, and two of its bits are NOT KNOWN.
 *
 * The chip carries a second calendar at 0x11..0x17 (RTCSCA..RTCYRA, same order
 * and encoding), raises an alarm bit in INT1 (masked by INT1M) and can wake the
 * PMU from standby through a bit in OOCC1. Two functions would be the whole of
 * it — and neither bit is settled:
 *
 *   INT1's alarm bit: 0x40 and 0x80 are both on the table. The bit positions
 *   recalled from the datasheet's INT1 map (ONKEYR/ONKEYF/ONKEY1S/EXTONR/
 *   EXTONF/SECOND/ALARM, bit 7 unused) put it at 0x40; an earlier reading of
 *   the same map put it at 0x80. Nothing here picks one, because nothing here
 *   needs to yet — a define would only be a number the next person trusts.
 *
 *   OOCC1's RTCWAK: docs/hw/06-power.md's standby table says 0x80, the
 *   datasheet map says 0x10, and 0x80 is very likely EXTONWAK's high bit.
 *   Writing the wrong bit into the register that TRIGGERS STANDBY is the one
 *   mistake on this chip that can leave an iPod that will not wake.
 *
 * Both get settled on the bench (06-power.md, "Bench procedure") before a line
 * of alarm code is written. Only the register addresses are defined here.
 */
#define PMU_RTCSCA 0x11
#define PMU_INT1   0x02
#define PMU_INT1M  0x05

#endif /* CORE_HAL_HW_RTC_H */
