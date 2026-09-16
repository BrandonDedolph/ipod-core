/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/timesync.h — what the firmware does at boot with the clock the
 * host left in CORECFG.DAT.
 *
 * WHY THERE IS A STATE MACHINE AT ALL
 *
 * The iPod cannot be told the time over the cable: on USB it is Apple's ROM
 * disk-mode stack answering, not ours (docs/hw/07-usb.md), so `core sync` /
 * `core eject` / `core install` write a STAMP — the host's UTC epoch and its
 * UTC offset — into the settings record instead, and the firmware picks it up
 * the next time it boots. That makes the stamp a message from the past whose
 * age is unknown, and the two ways of getting it wrong are both bad:
 *
 *   - apply it on every boot and a device that has sat on a shelf for a week
 *     silently sets its clock back a week, every time, for ever;
 *   - never re-apply it and a stamp whose "I acted on this" mark failed to
 *     reach the disk is lost.
 *
 * The rules below thread between the two: a stamp is acted on AT MOST ONCE per
 * host_epoch value (the mark), and once the RTC has run more than
 * TIMESYNC_STALE_S past the stamp the stamp is inert whatever the mark says.
 * So the worst case of a lost mark is one backwards step of under ten minutes,
 * once.
 *
 * PURE. No hardware, no record: the caller fills a timesync_state_t from
 * wherever the four numbers live (the settings record, once its time block
 * lands) and writes the decision back. That is what lets every row of the
 * table, and the "cannot loop" / "cannot walk backwards" sequences, be a case
 * in tests/kernel/timesync_test.c.
 */

#ifndef CORE_KERNEL_TIMESYNC_H
#define CORE_KERNEL_TIMESYNC_H

#include <stdint.h>

/*
 * How far past the stamp the RTC may be read and the stamp still applied.
 *
 * Ten minutes is "longer than the normal unplug-to-boot delay, shorter than a
 * clock error a user would tolerate". `core eject` stamps immediately before
 * the OS ejects, the ROM reboots the iPod on disconnect and our boot is ~8 s,
 * so the normal flow is well under a minute. The cost of the rule is that a
 * clock running more than ten minutes FAST cannot be corrected by a host
 * stamp — the user sees it and sets it by hand.
 */
#define TIMESYNC_STALE_S 600u

typedef enum {
    TIMESYNC_NONE = 0,   /* leave the clock and the record alone           */
    TIMESYNC_SET,        /* set the RTC to host_epoch, then mark the stamp */
    TIMESYNC_STALE       /* do not set anything, but mark the stamp        */
} timesync_action_t;

/*
 * The four numbers the decision is made from. `host_*` are written by the host
 * tools and never by the firmware; `applied_epoch` / `utc_off_min` are the
 * firmware's own and are what timesync_apply() moves.
 *
 * utc_off_min is the DISPLAY offset: the RTC holds UTC, and local time is
 * RTC + utc_off_min minutes. It is copied from the host's offset on every
 * action — including a STALE one — because a DST change arrives as a new
 * offset next to an epoch we may well decide not to use, and the host knows
 * the zone better than we do.
 */
typedef struct {
    uint32_t host_epoch;      /* UTC seconds the host stamped; 0 = never   */
    int      host_off_min;    /* the host's UTC offset then, minutes       */
    uint32_t applied_epoch;   /* the host_epoch last acted on; 0 = never   */
    int      utc_off_min;     /* the device's display offset               */
} timesync_state_t;

/*
 * The decision, first rule that fires:
 *
 *  1. !writable                                   -> NONE   nothing may be
 *     applied that cannot be recorded, or it is applied again every boot.
 *  2. host_epoch == 0                             -> NONE   never stamped.
 *  3. host_epoch == applied_epoch                 -> NONE   already acted on.
 *     EQUALITY, not "newer": a host whose clock was wrong in the future and
 *     was then corrected still counts as a new stamp.
 *  4. host_epoch outside 2001..2099               -> STALE  not a date this
 *     clock can hold; mark it so it is never looked at again.
 *  5. rtc_valid && rtc_epoch >= host + STALE_S    -> STALE  the clock is
 *     running and is already past the stamp by more than the stamp could
 *     plausibly be old.
 *  6. otherwise                                   -> SET.
 *
 * `rtc_valid` is hal_rtc_get()'s 1; both its 0 (unset) and its -1 (no answer)
 * are rtc_valid = 0, and then rule 5 cannot fire — an unknown clock is always
 * worth setting.
 */
timesync_action_t timesync_decide(const timesync_state_t *st, int writable,
                                  int rtc_valid, uint32_t rtc_epoch);

/*
 * Record `action` in the state: on SET and on STALE alike, applied_epoch takes
 * host_epoch (the mark) and utc_off_min takes host_off_min. NONE changes
 * nothing. Returns 1 when a field actually moved, i.e. when the caller has
 * something to save.
 *
 * Call it ONLY after the action has been carried out — on a SET whose
 * hal_rtc_set() failed the mark must NOT move, so the next boot retries the
 * same stamp (rule 5 bounds how far back that can ever pull the clock).
 */
int timesync_apply(timesync_state_t *st, timesync_action_t action);

#endif /* CORE_KERNEL_TIMESYNC_H */
