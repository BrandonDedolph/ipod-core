/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/jackwatch.h — the policy behind "pause when the headphones are
 * pulled out": one edge detector over the debounced jack level.
 *
 * WHY THIS FILE EXISTS
 *
 * The hardware half has been host-tested since it was written
 * (hal/hw/headphone.c: one GPIO read, a 200 ms debounce, -1 while the line is
 * untrusted). The POLICY half was five lines inline in kernel/main.c over a
 * file-scope `g_hp_last`, and nothing tested it — in a file that is not
 * host-built, so nothing could. It also had a hole: the suspend path never
 * updated `g_hp_last`, so a plug pulled while the device slept left the loop
 * looking at a 1 -> 0 edge it had already acted on. The only thing standing
 * between that and a pause on a player the wake path had deliberately left
 * paused was a `!player_paused()` guard two screens away.
 *
 * So the decision moved here, the way ui/keyhold.c took PLAY's tap-vs-hold
 * out of the same file: pure integer logic, no clock and no hardware of its
 * own, the SAME source the ARM build links, and a case per row of the table
 * below in tests/ui/jackwatch_test.c.
 *
 * WHAT IT DECIDES, AND WHAT IT REFUSES TO
 *
 *   pull while playing  -> PAUSE, once, on the edge.
 *   plug back in        -> NOTHING. The insertion switch closes before the
 *                          audio contacts seat, so resuming on that edge
 *                          would start playing into a half-made connection at
 *                          whatever the volume happened to be, while the user
 *                          still has hold of the plug. Every reference player
 *                          waits for Play.
 *   pull while paused   -> nothing to pause.
 *   level -1            -> no answer, not a level: never an edge, never a
 *                          prime. This is what makes the whole feature inert
 *                          while HEADPHONE_DETECT_TRUSTED is 0.
 *
 * There is no screen, Hold or charging state in here on purpose: a yank in a
 * pocket is the canonical case and the lock switch must not swallow it.
 *
 * TIMING IS NOT OURS. `now_us` is recorded (`edge_us`) and never compared:
 * the HAL owns the 200 ms debounce, and a second window here would mean two
 * pieces of code disagreeing about when a pull happened.
 */

#ifndef CORE_UI_JACKWATCH_H
#define CORE_UI_JACKWATCH_H

#include <stdint.h>

typedef struct {
    int8_t   last;        /* last believed debounced level: -1 unknown, 0/1 */
    int8_t   raw_last;    /* last raw level fed to jackwatch_note_raw, -1 none */
    uint8_t  paused_by;   /* 1 from a PAUSE until the plug goes back in     */
    uint16_t raw_edges;   /* raw transitions since reset (About + evlog)    */
    uint16_t pauses;      /* PAUSE actions emitted since reset             */
    uint32_t edge_us;     /* `now_us` of the last DEBOUNCED edge            */
} jackwatch_t;

typedef enum {
    JACKWATCH_NONE = 0,   /* nothing to do this pass                       */
    JACKWATCH_PAUSE,      /* the plug just came out under a playing track  */
} jackwatch_action_t;

/*
 * Forget everything: no sample yet, nothing believed. Zero-initialised
 * storage is NOT this state (`last`/`raw_last` have to start at -1), so a
 * file-scope jackwatch_t still has to be reset before its first feed — the
 * way kernel/main.c resets its keyhold_t beside it.
 */
void jackwatch_reset(jackwatch_t *j);

/*
 * Feed the DEBOUNCED level — hal_headphones_present(), i.e. -1 unknown /
 * 0 absent / 1 seated — once per main-loop pass, with whether the transport
 * is actually playing (player_active() && !player_paused()).
 *
 * Returns PAUSE on exactly the 1 -> 0 edge while playing, NONE otherwise.
 * The first level after a reset primes `last` and can never be an edge, so a
 * device booted with nothing in the jack cannot look like a pull-out.
 */
jackwatch_action_t jackwatch_feed(jackwatch_t *j, int level, int playing,
                                  uint32_t now_us);

/*
 * Re-prime from the live level with NO action — the wake path, which was not
 * running to see what happened during the suspend. A level of -1 leaves
 * `last` alone; a seated plug also clears `paused_by`, because whatever this
 * module paused has since been superseded by the wake's own resume decision.
 */
void jackwatch_prime(jackwatch_t *j, int level);

/*
 * Count RAW (un-debounced) transitions — headphone_raw(), available in every
 * build. Returns 1 when the caller should narrate and repaint (the first
 * sample after a reset included: that one is the bench's starting reading),
 * 0 while nothing has moved. Pure bookkeeping: never an action, and it feeds
 * nothing the pause decision reads. `raw_edges` counts transitions only, not
 * that first prime, and saturates rather than wrapping.
 */
int jackwatch_note_raw(jackwatch_t *j, int raw);

#endif /* CORE_UI_JACKWATCH_H */
