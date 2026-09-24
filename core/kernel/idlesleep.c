/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/idlesleep.c — see idlesleep.h.
 */

#include "idlesleep.h"

void idlesleep_reset(idlesleep_t *s, uint32_t now_us)
{
    s->last_busy_us   = now_us;
    s->seen_input_us  = now_us;
    s->off            = 0;
}

void idlesleep_off(idlesleep_t *s)
{
    s->off = 1;
}

int idlesleep_is_off(const idlesleep_t *s)
{
    return s->off;
}

int idlesleep_feed(idlesleep_t *s, uint32_t now_us, uint32_t input_us, int busy)
{
    /* An input is a change of the stamp, and the countdown restarts from the
     * stamp itself (the moment of the press), not from this pass. */
    if (input_us != s->seen_input_us) {
        s->seen_input_us = input_us;
        s->last_busy_us  = input_us;
    }
    if (busy) {
        s->last_busy_us = now_us;
    }
    if (s->off) {
        return 0;
    }
    if ((uint32_t)(now_us - s->last_busy_us) >= IDLESLEEP_TIMEOUT_US) {
        s->last_busy_us = now_us;       /* one answer per timeout */
        return 1;
    }
    return 0;
}

uint32_t idlesleep_idle_us(const idlesleep_t *s, uint32_t now_us)
{
    if (s->off) {
        return 0;
    }
    return (uint32_t)(now_us - s->last_busy_us);
}
