/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/jackwatch.c — pause-on-unplug policy. See jackwatch.h.
 */

#include "jackwatch.h"

/* Counters are for a human reading the About footer or a log dump. Saturating
 * beats wrapping: "65535" is obviously "a lot", "3" after a pin flapped all
 * afternoon is a lie. */
#define JACKWATCH_COUNT_MAX 0xFFFFu

void jackwatch_reset(jackwatch_t *j)
{
    j->last      = -1;
    j->raw_last  = -1;
    j->raw_edges = 0;
    j->pauses    = 0;
    j->edge_us   = 0;
}

jackwatch_action_t jackwatch_feed(jackwatch_t *j, int level, int playing,
                                  uint32_t now_us)
{
    if (level < 0) {
        /* "No answer" — the line is not trusted on this device, or the HAL
         * has nothing yet. Not a level: it must not prime `last`, or the
         * first real 0 afterwards would read as a pull-out. */
        return JACKWATCH_NONE;
    }

    int8_t lvl = level ? (int8_t)1 : (int8_t)0;

    if (j->last < 0) {
        /* First answer since the reset. Whatever it is, it is the starting
         * state, never a transition — this is the boot-with-an-empty-jack
         * case, and the boot-with-headphones-in case. */
        j->last = lvl;
        return JACKWATCH_NONE;
    }
    if (lvl == j->last) {
        return JACKWATCH_NONE;
    }

    j->last    = lvl;
    j->edge_us = now_us;

    if (lvl == 0) {
        if (!playing) {
            return JACKWATCH_OUT;       /* already paused, or nothing loaded */
        }
        if (j->pauses < JACKWATCH_COUNT_MAX) {
            j->pauses++;
        }
        return JACKWATCH_PAUSE;
    }
    /* Plug back in: something for the caller to say, never anything to do. */
    return JACKWATCH_IN;
}

void jackwatch_prime(jackwatch_t *j, int level)
{
    if (level < 0) {
        return;
    }
    j->last = level ? (int8_t)1 : (int8_t)0;
}

int jackwatch_note_raw(jackwatch_t *j, int raw)
{
    int8_t lvl = raw ? (int8_t)1 : (int8_t)0;

    if (j->raw_last < 0) {
        j->raw_last = lvl;
        return 1;                       /* the starting reading, not an edge */
    }
    if (lvl == j->raw_last) {
        return 0;
    }
    j->raw_last = lvl;
    if (j->raw_edges < JACKWATCH_COUNT_MAX) {
        j->raw_edges++;
    }
    return 1;
}
