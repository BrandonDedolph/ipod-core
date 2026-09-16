/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/gesture.c — the press-length gestures. See gesture.h.
 */

#include "gesture.h"

/*
 * The fast-forward / rewind ramp, as (up to, seconds per tick) pairs walked
 * in order; a zero `until_us` is the last tier and matches everything left.
 * The tiers are the hold's own age, so the rate a user feels is "5 s a
 * quarter-second, faster after two seconds, faster again after five and ten".
 */
typedef struct {
    uint32_t until_us;   /* held for less than this...                       */
    uint32_t step_s;     /* ...and one tick moves the aim this far           */
} gesture_ramp_t;

static const gesture_ramp_t GESTURE_SEEK_RAMP[] = {
    {  2000000u,  5 },
    {  5000000u, 15 },
    { 10000000u, 30 },
    {         0u, 60 },
};

uint32_t gesture_seek_step(uint32_t held_us)
{
    unsigned n = (unsigned)(sizeof GESTURE_SEEK_RAMP / sizeof GESTURE_SEEK_RAMP[0]);
    for (unsigned i = 0; i + 1u < n; i++) {
        if (held_us < GESTURE_SEEK_RAMP[i].until_us) {
            return GESTURE_SEEK_RAMP[i].step_s;
        }
    }
    return GESTURE_SEEK_RAMP[n - 1u].step_s;
}

void seekhold_reset(seekhold_t *s)
{
    int8_t dir = s->dir;                  /* which button this is, not state */
    keyhold_reset(&s->key);
    s->dir             = dir;
    s->was_down        = 0;
    s->allowed_at_down = 0;
    s->active          = 0;
    s->moved           = 0;
    s->edge_seen       = 0;
    s->pending_skip    = 0;
    s->hold_origin_us  = 0;
    s->ticks           = 0;
    s->target_s        = 0;
}

void seekhold_void(seekhold_t *s)
{
    keyhold_void(&s->key);
    s->active       = 0;                  /* nothing left to commit          */
    s->moved        = 0;
    s->edge_seen    = 0;
    s->pending_skip = 0;
}

seekhold_action_t seekhold_cancel(seekhold_t *s)
{
    int aiming = s->active;
    s->active = 0;
    s->moved  = 0;
    /* A handed-over edge still in the air belongs to the press being killed,
     * so it goes with it. A tap already settled (pending_skip) does not: that
     * one is finished and the user did ask for it. */
    s->edge_seen = 0;
    /* Clearing the latch is what kills the press: seekhold_feed only ever
     * sets it on a down-edge, so while the finger stays down the arbiter is
     * fed a released button and the press can produce nothing. Resetting the
     * arbiter with it stops that fed release reading as a TAP — and, unlike
     * keyhold_void(), arms no pre-press grace, so a cancel with nothing down
     * cannot eat the next press. */
    s->allowed_at_down = 0;
    keyhold_reset(&s->key);
    return aiming ? SEEKHOLD_CANCEL : SEEKHOLD_NONE;
}

void seekhold_missed_tap(seekhold_t *s, int is_down)
{
    if (s->was_down) {
        return;                  /* the machine already has this press */
    }
    if (is_down) {
        s->edge_seen = 1;        /* still in the air: the next feed settles it */
    } else {
        s->pending_skip = 1;     /* over before the drain: it was a tap */
    }
}

/*
 * Move the aim to where the ticks that have elapsed since the hold fired put
 * it. Computed from the ELAPSED time, not counted one per pass: a pass that
 * blocked in a disk read for a second must produce the four steps it was away
 * for, not one. The step a tick is worth is read at the moment that tick
 * fires, so the ramp changes tier exactly at 2 s / 5 s / 10 s of holding.
 */
static seekhold_action_t seek_aim(seekhold_t *s, uint32_t now_us,
                                  uint32_t total_s)
{
    uint32_t held = now_us - s->hold_origin_us;     /* wrap-safe             */
    uint32_t want = held / GESTURE_SEEK_TICK_US;
    if (want <= s->ticks) {
        return SEEKHOLD_NONE;
    }

    uint32_t t = s->target_s;
    while (s->ticks < want) {
        s->ticks++;
        /* The tick's own age. Saturated rather than wrapped: an hour-long
         * hold must not fall back to the slowest tier. */
        uint32_t at = (s->ticks <= 0xFFFFFFFFu / GESTURE_SEEK_TICK_US)
                      ? s->ticks * GESTURE_SEEK_TICK_US : 0xFFFFFFFFu;
        uint32_t step = gesture_seek_step(at);
        if (s->dir > 0) {
            t += step;
            if (total_s > 0 && t > total_s) t = total_s;
        } else {
            t = (t > step) ? t - step : 0;
        }
    }

    if (t == s->target_s) {
        return SEEKHOLD_NONE;             /* pinned at an end of the track   */
    }
    s->target_s = t;
    s->moved    = 1;                      /* ...so the release is worth a seek */
    return SEEKHOLD_AIM;
}

seekhold_action_t seekhold_feed(seekhold_t *s, int is_down, uint32_t now_us,
                                int allowed, uint32_t elapsed_s,
                                uint32_t total_s)
{
    int down     = is_down ? 1 : 0;
    int was_held = keyhold_held(&s->key);   /* the hold is in force right now */
    if (down) {
        if (!s->was_down) {
            s->allowed_at_down = (uint8_t)(allowed ? 1 : 0);
        }
        /* A handed-over edge and a live press: they are the same press, and
         * the machine has it now. */
        s->edge_seen = 0;
    } else if (s->edge_seen) {
        /* The drain saw its down-edge with the button still down, and by this
         * feed it is up again: the whole press fell into the gap — the
         * per-screen switch, a render and a present of the skip before it —
         * so nothing else will ever report it. It was a tap. */
        s->edge_seen    = 0;
        s->pending_skip = 1;
    }
    s->was_down = (uint8_t)down;

    /* A press that began where a seek means nothing is never shown to the
     * arbiter at all, so it can produce neither a skip nor a hold however
     * long it lasts or wherever it ends up. */
    keyhold_action_t a = keyhold_feed(&s->key, down && s->allowed_at_down,
                                      now_us, GESTURE_SEEK_HOLD_US);

    if (!allowed) {
        /* The queue ended or the screen went away under the press. Kill it:
         * an aim in flight is dropped unseeked and the press is dead for the
         * rest of its life, which is what the header promises. (`a` is
         * discarded deliberately — the arbiter may have read this pass's
         * withheld sample as a release and offered a TAP.) */
        s->pending_skip = 0;
        return seekhold_cancel(s);
    }

    /* A tap the sampler missed entirely, handed over by the drain. Reported
     * only on a pass with nothing else to say, so a real action landing in
     * the same pass is not displaced — the missed one follows 10 ms later. */
    if (s->pending_skip && a == KEYHOLD_NONE && !s->active) {
        s->pending_skip = 0;
        return SEEKHOLD_SKIP;
    }

    if (a == KEYHOLD_TAP) {
        return SEEKHOLD_SKIP;
    }
    if (a == KEYHOLD_HOLD) {
        /* The band flips to the aim display at once, before any step: what
         * confirms the gesture is the readout changing under the thumb. */
        s->active         = 1;
        s->moved          = 0;
        s->hold_origin_us = now_us;
        s->ticks          = 0;
        s->target_s       = elapsed_s;
        return SEEKHOLD_AIM;
    }

    if (s->active) {
        if (was_held && !down) {          /* the hold's own release          */
            int moved = s->moved;
            s->active = 0;
            s->moved  = 0;
            /* One seek for the whole hold — but only if the aim actually
             * went somewhere. A hold let go between the 500 ms fire and the
             * first 250 ms tick, or one that spent its whole life pinned at
             * an end, would otherwise seek to where playback already is: a
             * DAC stop and a re-prime, heard as a hiccup and a small jump
             * backwards, for nothing. */
            return moved ? SEEKHOLD_COMMIT : SEEKHOLD_CANCEL;
        }
        return seek_aim(s, now_us, total_s);
    }
    return SEEKHOLD_NONE;
}

uint32_t seekhold_target(const seekhold_t *s)
{
    return s->target_s;
}

int seekhold_active(const seekhold_t *s)
{
    return s->active != 0;
}

gesture_play_t gesture_play_tap(gesture_ctx_t ctx, int count)
{
    switch (ctx) {
    case GESTURE_CTX_MUSIC_SHUFFLE:   /* the one menu row that names a queue */
    case GESTURE_CTX_LIST_TITLE:      /* an album / artist / genre / playlist */
    case GESTURE_CTX_LIST_TRACK:      /* a song inside one                    */
        return (count > 0) ? GESTURE_PLAY_START : GESTURE_PLAY_PAUSE;
    case GESTURE_CTX_MENU:
    case GESTURE_CTX_MUSIC_OTHER:
    case GESTURE_CTX_PLAYER:
    case GESTURE_CTX_SETTINGS:
    case GESTURE_CTX_MODAL:
    default:
        return GESTURE_PLAY_PAUSE;
    }
}
