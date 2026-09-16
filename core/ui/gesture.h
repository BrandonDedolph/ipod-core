/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/gesture.h — the three press-length gestures the click wheel owes
 * the user, as pure logic the host can test.
 *
 * WHY THIS FILE EXISTS
 *
 * kernel/main.c is 7000+ lines and every UI decision that lands in it is a
 * decision nothing can check until the firmware is on the device. The button
 * model already went this way once (ui/keyhold.c, the tap-vs-hold arbiter);
 * this is the rest of it:
 *
 *   1. PLAY on a list. On a real iPod, Play on a highlighted album, artist,
 *      genre, playlist or song STARTS it; it only means pause/resume where
 *      there is no list title under the cursor. Which screens those are is a
 *      policy, not a mechanism — gesture_play_tap() is that policy, one
 *      table-shaped function over a context enum, so every screen's answer is
 *      pinned by a test instead of by reading a 400-line switch.
 *
 *   2. RIGHT / LEFT held. A tap skips a track, a hold seeks inside it, at a
 *      rate that ramps the longer you hold. seekhold_t is that machine: a
 *      keyhold_t for the press length plus an AIM, because a seek per tick
 *      would stop the DAC, re-prime the ring and rewind the file twenty times
 *      a second. It aims silently while held and hands the caller one target
 *      on release — the same trade ui/wheel.c's scrubber makes.
 *
 *   3. MENU held. Plain keyhold_t in the caller (GESTURE_MENU_HOLD_US lives
 *      here so all three thresholds are in one place); the jump home is a
 *      stack operation with nothing to arbitrate.
 *
 * Freestanding C11, integer only, no clock of its own: the caller passes
 * `now_us` (the free-running 1 MHz USEC_TIMER on the device) and every
 * elapsed time is an unsigned 32-bit subtraction, so a press that straddles
 * the counter wrap is timed correctly. Same file the ARM build links into
 * core.elf and the host suite compiles.
 */

#ifndef CORE_UI_GESTURE_H
#define CORE_UI_GESTURE_H

#include <stdint.h>

#include "keyhold.h"

/*
 * Press-length thresholds.
 *
 * SELECT's 450 ms (SEL_HOLD_US in kernel/main.c) is the neighbour the seek
 * threshold was chosen against: a hold long enough to be deliberate, short
 * enough that the seek confirms itself under the thumb. MENU's is longer
 * because its tap already fired at the down-edge — a hesitant back-press must
 * not also walk the user home — but clearly shorter than PLAY's 2 s sleep, so
 * the two long presses cannot be confused for one another.
 */
#define GESTURE_SEEK_HOLD_US  500000u   /* RIGHT/LEFT: under this is a skip   */
#define GESTURE_SEEK_TICK_US  250000u   /* one aim step per tick while held   */
#define GESTURE_MENU_HOLD_US 1000000u   /* MENU held this long jumps home     */

/*
 * Seconds of audio one GESTURE_SEEK_TICK_US step covers, by how long the hold
 * has lasted: 5 s a quarter-second to start (20x real time), then 15 (60x),
 * 30 (120x) and 60 (240x). The first tier is fine enough to land on a verse;
 * the last crosses a 70-minute recording in 17 s of holding. Exported so the
 * test pins the exact numbers rather than re-deriving them.
 */
uint32_t gesture_seek_step(uint32_t held_us);

/*
 * One direction of "tap skips, hold seeks". Two instances live in
 * kernel/main.c, one per button; `dir` is the only thing that differs and it
 * is set at the definition (seekhold_reset deliberately keeps it, because the
 * Hold switch resets these mid-press).
 */
typedef struct {
    keyhold_t key;             /* the press length: tap vs hold               */
    int8_t    dir;             /* +1 fast forward, -1 rewind                  */
    uint8_t   was_down;        /* live state last feed, for the press edges   */
    uint8_t   allowed_at_down; /* the press BEGAN somewhere it may seek       */
    uint8_t   active;          /* a hold fired and is aiming right now        */
    uint32_t  hold_origin_us;  /* when it fired: the ramp's and the ticks' 0  */
    uint32_t  ticks;           /* aim steps already applied since the origin  */
    uint32_t  target_s;        /* where the aim points                        */
} seekhold_t;

typedef enum {
    SEEKHOLD_NONE = 0,  /* nothing to do this pass                            */
    SEEKHOLD_SKIP,      /* released under the threshold: next/previous track  */
    SEEKHOLD_AIM,       /* the target moved: repaint the transport band       */
    SEEKHOLD_COMMIT,    /* released after a hold: seek to seekhold_target()   */
    SEEKHOLD_CANCEL,    /* the aim went away unseeked: repaint the band       */
} seekhold_action_t;

/* Forget any press in flight, keeping `dir`. The Hold switch going on under
 * the finger is the case this exists for. */
void seekhold_reset(seekhold_t *s);

/*
 * This press does nothing at all — no skip, no seek. The press that jumps to
 * Now Playing from a list, that dismisses a modal, or that lights a dark
 * screen is spent on that. Valid before the press has been fed (the event
 * drain runs before the live sampler shows it), with keyhold_void's grace.
 */
void seekhold_void(seekhold_t *s);

/*
 * Feed one sample, once per main-loop pass.
 *
 *   is_down    the button's LIVE state (clickwheel_buttons()).
 *   allowed    1 where a seek means something: the player screens, with a
 *              track loaded. LATCHED at the down-edge — a press that began on
 *              a list (where RIGHT jumps to Now Playing instead) stays void
 *              for its whole life, even though `allowed` becomes 1 one pass
 *              later. A press that began allowed and LOSES it (the track
 *              ended, MENU popped the screen) is cancelled.
 *   elapsed_s  the live position; where a new aim starts from.
 *   total_s    the track length, the aim's upper clamp. 0 = unknown: no upper
 *              clamp here, player_seek_to() clamps to the last frame.
 *
 * Returns at most one action per pass. The aim never crosses a track
 * boundary: fast forward pins at total_s and rewind at 0, and reports NONE
 * once pinned.
 */
seekhold_action_t seekhold_feed(seekhold_t *s, int is_down, uint32_t now_us,
                                int allowed, uint32_t elapsed_s,
                                uint32_t total_s);

/* Where the aim points, in seconds. Only meaningful while seekhold_active()
 * or on the pass that returned SEEKHOLD_COMMIT. */
uint32_t seekhold_target(const seekhold_t *s);

/* 1 while a hold is aiming: the transport band shows the target instead of
 * the live position. */
int seekhold_active(const seekhold_t *s);

/*
 * Where the cursor is, as far as "what does a PLAY tap mean here" is
 * concerned. LIST_TITLE is a row that NAMES a queue (an album, an artist, a
 * genre, a playlist); LIST_TRACK is a row that IS a track inside one.
 */
typedef enum {
    GESTURE_CTX_MENU = 0,       /* the main menu                              */
    GESTURE_CTX_MUSIC_SHUFFLE,  /* the Music menu's Shuffle Songs row         */
    GESTURE_CTX_MUSIC_OTHER,    /* any other Music menu row                   */
    GESTURE_CTX_LIST_TITLE,     /* albums / artists / genres / playlists      */
    GESTURE_CTX_LIST_TRACK,     /* songs, an album's or a playlist's tracks   */
    GESTURE_CTX_PLAYER,         /* Now Playing, the queue view                */
    GESTURE_CTX_SETTINGS,       /* Settings and its sub-screens               */
    GESTURE_CTX_MODAL,          /* charging / low battery                     */
} gesture_ctx_t;

typedef enum {
    GESTURE_PLAY_PAUSE = 0,     /* PLAY means pause/resume, as it always did  */
    GESTURE_PLAY_START,         /* PLAY means "play what is highlighted"      */
} gesture_play_t;

/*
 * What a PLAY *tap* means at `ctx`, where `count` is the number of rows the
 * list holds (0 elsewhere). An empty list has no title under the cursor, so
 * it falls back to pause/resume — "Play works everywhere" stays true.
 */
gesture_play_t gesture_play_tap(gesture_ctx_t ctx, int count);

#endif /* CORE_UI_GESTURE_H */
