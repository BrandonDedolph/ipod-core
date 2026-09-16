/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/timesync.c — the boot-time host-stamp decision. See timesync.h.
 */

#include "timesync.h"
#include "datetime.h"

timesync_action_t timesync_decide(const timesync_state_t *st, int writable,
                                  int rtc_valid, uint32_t rtc_epoch)
{
    if (st == 0) {
        return TIMESYNC_NONE;
    }

    /* 1. A decision we cannot record is a decision we would take again on
     *    every boot. Refusing is the only stable answer. */
    if (!writable) {
        return TIMESYNC_NONE;
    }

    /* 2. No stamp: this device has never been plugged into the host app. */
    if (st->host_epoch == 0u) {
        return TIMESYNC_NONE;
    }

    /* 3. The mark. One action per stamp value, which is what stops the loop. */
    if (st->host_epoch == st->applied_epoch) {
        return TIMESYNC_NONE;
    }

    /* 4. Not a date the RTC can hold (the chip counts years 00..99 from 2000,
     *    and year 00 is its "unset" reset value). Mark it and move on rather
     *    than re-examining the same impossible stamp at every boot. */
    if (st->host_epoch < DATETIME_EPOCH_2001 ||
        st->host_epoch >= DATETIME_EPOCH_2100) {
        return TIMESYNC_STALE;
    }

    /* 5. The running clock is already past the stamp by more than the stamp
     *    could plausibly be old: the device sat unbooted. Applying would set
     *    it BACK by that much. No overflow here — host_epoch is below
     *    EPOCH_2100 and EPOCH_2100 + 600 still fits a uint32. */
    if (rtc_valid && rtc_epoch >= st->host_epoch + TIMESYNC_STALE_S) {
        return TIMESYNC_STALE;
    }

    /* 6. The clock is unset, or behind, or under ten minutes ahead. */
    return TIMESYNC_SET;
}

int timesync_apply(timesync_state_t *st, timesync_action_t action)
{
    if (st == 0 || action == TIMESYNC_NONE) {
        return 0;
    }

    int moved = 0;
    if (st->applied_epoch != st->host_epoch) {
        st->applied_epoch = st->host_epoch;
        moved = 1;
    }
    /* The offset is copied even when the epoch was judged STALE: a DST change
     * reaches the device as a new offset beside an epoch we may not use. */
    if (st->utc_off_min != st->host_off_min) {
        st->utc_off_min = st->host_off_min;
        moved = 1;
    }
    return moved;
}
