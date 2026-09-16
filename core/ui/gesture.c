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
    s->hold_origin_us  = 0;
    s->ticks           = 0;
    s->target_s        = 0;
}

void seekhold_void(seekhold_t *s)
{
    keyhold_void(&s->key);
    s->active = 0;                        /* nothing left to commit          */
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
    return SEEKHOLD_AIM;
}

seekhold_action_t seekhold_feed(seekhold_t *s, int is_down, uint32_t now_us,
                                int allowed, uint32_t elapsed_s,
                                uint32_t total_s)
{
    int down     = is_down ? 1 : 0;
    int was_held = keyhold_held(&s->key);   /* the hold is in force right now */
    if (down && !s->was_down) {
        s->allowed_at_down = (uint8_t)(allowed ? 1 : 0);
    }
    s->was_down = (uint8_t)down;

    /* A press that began where a seek means nothing is never shown to the
     * arbiter at all, so it can produce neither a skip nor a hold however
     * long it lasts or wherever it ends up. */
    keyhold_action_t a = keyhold_feed(&s->key, down && s->allowed_at_down,
                                      now_us, GESTURE_SEEK_HOLD_US);

    if (!allowed) {
        /* The track ended or the screen went away under the press. An aim in
         * flight is dropped unseeked; the caller repaints the band so the
         * live position comes back. The release is silent either way: the
         * arbiter has already fired for this press, or never saw it. */
        if (s->active) {
            s->active = 0;
            return SEEKHOLD_CANCEL;
        }
        return SEEKHOLD_NONE;
    }

    if (a == KEYHOLD_TAP) {
        return SEEKHOLD_SKIP;
    }
    if (a == KEYHOLD_HOLD) {
        /* The band flips to the aim display at once, before any step: what
         * confirms the gesture is the readout changing under the thumb. */
        s->active         = 1;
        s->hold_origin_us = now_us;
        s->ticks          = 0;
        s->target_s       = elapsed_s;
        return SEEKHOLD_AIM;
    }

    if (s->active) {
        if (was_held && !down) {          /* the hold's own release          */
            s->active = 0;
            return SEEKHOLD_COMMIT;       /* one seek for the whole hold     */
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
