/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/keyhold.c — press-length arbitration for one button. See keyhold.h.
 */

#include "keyhold.h"

void keyhold_reset(keyhold_t *k)
{
    k->down    = 0;
    k->no_tap  = 0;
    k->fired   = 0;
    k->down_us = 0;
}

keyhold_action_t keyhold_feed(keyhold_t *k, int is_down, uint32_t now_us,
                              uint32_t hold_us)
{
    if (!k->down) {
        if (is_down) {
            /* no_tap is NOT cleared here: a swallow that arrived before the
             * sampler showed the press (the tick landed between this feed
             * and the event drain) must still claim this press's tap. It is
             * cleared on release, so it can only ever cost one press. */
            k->down    = 1;
            k->fired   = 0;
            k->down_us = now_us;
        }
        return KEYHOLD_NONE;
    }

    if (!is_down) {
        /* Released. The short action, unless the press already produced the
         * long one or its tap was claimed by whoever consumed the down-edge. */
        keyhold_action_t a = (k->fired || k->no_tap) ? KEYHOLD_NONE
                                                     : KEYHOLD_TAP;
        k->down   = 0;
        k->no_tap = 0;
        return a;
    }

    if (!k->fired && (uint32_t)(now_us - k->down_us) >= hold_us) {
        k->fired = 1;
        return KEYHOLD_HOLD;
    }
    return KEYHOLD_NONE;
}

void keyhold_swallow_tap(keyhold_t *k)
{
    /* Unconditional: the consumer of the down-edge (an event) can run before
     * the live-state sampler has shown the press to keyhold_feed. */
    k->no_tap = 1;
}

uint32_t keyhold_down_us(const keyhold_t *k)
{
    return k->down_us;
}
