/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ui/gesture_test.c — the press-length gestures (core/ui/gesture.c) on
 * the host.
 *
 * THE POINT OF THIS FILE. Two of the three gestures are decisions nobody can
 * check by looking at the screen: the fast-forward ramp is a rate, and the
 * PLAY-tap policy is "which of eleven screens starts a queue". Both used to
 * be the kind of thing that only a device and a stopwatch could falsify. So
 * the rate is pinned to the microsecond here, and the policy is pinned as a
 * table — including the two races the main loop's ordering creates:
 *
 *   - a RIGHT press BORN on a list (where the down-edge jumps to Now Playing)
 *     must never skip or seek, although `allowed` turns 1 one pass later;
 *   - a press the event drain voided before the 100 Hz sampler showed it must
 *     stay silent.
 *
 * Every case is a sequence of (is_down, clock, allowed, elapsed, total)
 * samples at 10 ms, the way run_ui() feeds it once per pass, and the
 * assertions are the action returned and the exact aim target in seconds.
 */

#include <stdio.h>

#include "gesture.h"
#include "../xfail.h"

#define PASS_US 10000u      /* one main-loop pass at 100 Hz */

/* A feed rig that carries the sample context, so a case reads as the gesture
 * it describes rather than as five arguments repeated. */
typedef struct {
    seekhold_t s;
    uint32_t   now;
    int        allowed;
    uint32_t   elapsed;
    uint32_t   total;
} rig_t;

static void rig_init(rig_t *r, int dir, uint32_t elapsed, uint32_t total)
{
    r->s = (seekhold_t){ .dir = (int8_t)dir };
    r->now     = 1000u;
    r->allowed = 1;
    r->elapsed = elapsed;
    r->total   = total;
}

/* One pass. */
static seekhold_action_t step(rig_t *r, int down)
{
    r->now += PASS_US;
    return seekhold_feed(&r->s, down, r->now, r->allowed, r->elapsed, r->total);
}

/* `n` passes, asserting every one of them returns `want`. */
static int step_n(rig_t *r, int down, int n, seekhold_action_t want,
                  xfail_ctx *c, const char *what)
{
    int ok = 1;
    for (int i = 0; i < n; i++) {
        if (step(r, down) != want) ok = 0;
    }
    xpect(c, what, ok);
    return ok;
}

/* Hold `down` until the next aim step lands, returning what it did. Used so a
 * case asserts the TARGET sequence without counting passes. */
static seekhold_action_t step_to_aim(rig_t *r, int max_passes)
{
    for (int i = 0; i < max_passes; i++) {
        seekhold_action_t a = step(r, 1);
        if (a != SEEKHOLD_NONE) return a;
    }
    return SEEKHOLD_NONE;
}

/* Take a press from its down-edge through to the first AIM (the pass the hold
 * fires), leaving the finger down. */
static int hold_until_aim(rig_t *r, xfail_ctx *c, const char *what)
{
    int ok = (step(r, 1) == SEEKHOLD_NONE);              /* the down-edge */
    /* GESTURE_SEEK_HOLD_US is 500 ms: 49 more silent passes, then the aim. */
    for (int i = 0; i < 49; i++) {
        if (step(r, 1) != SEEKHOLD_NONE) ok = 0;
    }
    ok = ok && (step(r, 1) == SEEKHOLD_AIM);
    ok = ok && (seekhold_active(&r->s) == 1);
    ok = ok && (seekhold_target(&r->s) == r->elapsed);
    xpect(c, what, ok);
    return ok;
}

int main(void)
{
    xfail_ctx c = { "gesture", 0, 0, 0 };
    rig_t r;

    /* ---- the ramp table ------------------------------------------------- *
     * These four numbers are the whole feel of the gesture; a test that said
     * "it gets faster" would not notice one of them changing. */
    xpect(&c, "ramp: 5 s a tick from the start",
          gesture_seek_step(0u) == 5u && gesture_seek_step(1999999u) == 5u);
    xpect(&c, "ramp: 15 s a tick from two seconds of holding",
          gesture_seek_step(2000000u) == 15u &&
          gesture_seek_step(4999999u) == 15u);
    xpect(&c, "ramp: 30 s a tick from five seconds",
          gesture_seek_step(5000000u) == 30u &&
          gesture_seek_step(9999999u) == 30u);
    xpect(&c, "ramp: 60 s a tick from ten seconds, and it stops there",
          gesture_seek_step(10000000u) == 60u &&
          gesture_seek_step(0xFFFFFFFFu) == 60u);

    /* ---- tap: a short press is a skip, decided on the RELEASE ------------ */
    rig_init(&r, +1, 60u, 600u);
    xpect(&c, "tap: the down-edge decides nothing", step(&r, 1) == SEEKHOLD_NONE);
    step_n(&r, 1, 48, SEEKHOLD_NONE, &c,
           "tap: held under the threshold, still nothing");
    /* 49 passes down; the release lands at 490 ms, a pass short of the
     * GESTURE_SEEK_HOLD_US threshold. */
    xpect(&c, "tap: a release under the threshold is the skip",
          step(&r, 0) == SEEKHOLD_SKIP);
    step_n(&r, 0, 5, SEEKHOLD_NONE, &c, "tap: exactly once");
    xpect(&c, "tap: a skip leaves no aim behind", seekhold_active(&r.s) == 0);

    /* ---- hold: the aim flips on, then steps every quarter second --------- */
    rig_init(&r, +1, 60u, 600u);
    hold_until_aim(&r, &c, "hold: the pass at 500 ms aims at the live position");
    {
        /* 5 s a tick for the first two seconds of holding. */
        uint32_t want[] = { 65u, 70u, 75u, 80u, 85u, 90u, 95u };
        int ok = 1;
        for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++) {
            if (step_to_aim(&r, 40) != SEEKHOLD_AIM) ok = 0;
            if (seekhold_target(&r.s) != want[i]) ok = 0;
        }
        xpect(&c, "hold: fast forward walks 5 s a quarter-second", ok);
    }

    rig_init(&r, -1, 60u, 600u);
    hold_until_aim(&r, &c, "rewind: the pass at 500 ms aims at the live position");
    {
        uint32_t want[] = { 55u, 50u, 45u, 40u };
        int ok = 1;
        for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++) {
            if (step_to_aim(&r, 40) != SEEKHOLD_AIM) ok = 0;
            if (seekhold_target(&r.s) != want[i]) ok = 0;
        }
        xpect(&c, "rewind: the same steps, backwards", ok);
    }

    /* ---- the ramp under a real hold -------------------------------------- *
     * Held from 0:00 of an hour-long recording, so nothing clamps and the
     * target IS the sum of every tick the ramp has paid out so far. */
    rig_init(&r, +1, 0u, 3600u);
    hold_until_aim(&r, &c, "ramp: a hold from 0:00 starts the aim at 0:00");
    {
        /* Ticks 1-7 land under 2 s of holding and are worth 5 s each,
         * 8-19 under 5 s and worth 15, 20-39 under 10 s and worth 30, the
         * rest 60. Three seconds of holding is twelve ticks, six seconds
         * twenty-four, twelve seconds forty-eight. */
        struct { uint32_t held_us; uint32_t target; } mark[] = {
            {  3000000u, 7u*5u + 5u*15u },                            /*  110 */
            {  6000000u, 7u*5u + 12u*15u + 5u*30u },                  /*  365 */
            { 12000000u, 7u*5u + 12u*15u + 20u*30u + 9u*60u },        /* 1355 */
        };
        uint32_t origin = r.now;
        int ok = 1;
        for (unsigned m = 0; m < sizeof mark / sizeof mark[0]; m++) {
            while ((uint32_t)(r.now - origin) < mark[m].held_us) {
                (void)step(&r, 1);
            }
            if (seekhold_target(&r.s) != mark[m].target) {
                printf("  [gesture] at %u us: target %u, wanted %u\n",
                       mark[m].held_us, seekhold_target(&r.s), mark[m].target);
                ok = 0;
            }
        }
        xpect(&c, "ramp: the tiers change at 2 s, 5 s and 10 s of holding", ok);
    }

    /* ---- the aim never crosses a track boundary -------------------------- */
    rig_init(&r, +1, 55u, 60u);
    hold_until_aim(&r, &c, "clamp: fast forward near the end aims at the end");
    xpect(&c, "clamp: the first step pins at the track length",
          step_to_aim(&r, 40) == SEEKHOLD_AIM && seekhold_target(&r.s) == 60u);
    step_n(&r, 1, 200, SEEKHOLD_NONE, &c,
           "clamp: pinned at the end it reports NONE, not a repaint a tick");
    xpect(&c, "clamp: and the release still commits the pinned target",
          step(&r, 0) == SEEKHOLD_COMMIT && seekhold_target(&r.s) == 60u);

    rig_init(&r, -1, 3u, 600u);
    hold_until_aim(&r, &c, "clamp: rewind near the start aims at the position");
    xpect(&c, "clamp: rewind pins at zero",
          step_to_aim(&r, 40) == SEEKHOLD_AIM && seekhold_target(&r.s) == 0u);
    step_n(&r, 1, 200, SEEKHOLD_NONE, &c, "clamp: pinned at zero it is quiet");

    /* total 0 is "length unknown": the player clamps at the last frame. */
    rig_init(&r, +1, 10u, 0u);
    hold_until_aim(&r, &c, "unknown length: the aim still starts at the position");
    {
        int ok = 1;
        for (int i = 0; i < 4; i++) {
            if (step_to_aim(&r, 40) != SEEKHOLD_AIM) ok = 0;
        }
        xpect(&c, "unknown length: no upper clamp here",
              ok && seekhold_target(&r.s) == 30u);
    }

    /* ---- commit: one seek for the whole hold ----------------------------- */
    rig_init(&r, +1, 60u, 600u);
    hold_until_aim(&r, &c, "commit: the hold aims first");
    (void)step_to_aim(&r, 40);
    (void)step_to_aim(&r, 40);
    xpect(&c, "commit: the release carries the last aim",
          step(&r, 0) == SEEKHOLD_COMMIT && seekhold_target(&r.s) == 70u);
    xpect(&c, "commit: the aim is over", seekhold_active(&r.s) == 0);
    step_n(&r, 0, 10, SEEKHOLD_NONE, &c, "commit: exactly once");

    /* ---- a pass that went away for a second ------------------------------ *
     * A disk read blocks the loop. The ticks that elapsed must all land, or a
     * hold across a track open would silently seek a quarter as far. */
    rig_init(&r, +1, 60u, 600u);
    hold_until_aim(&r, &c, "slow pass: the hold aims first");
    r.now += 900000u;                    /* the loop was away 0.9 s */
    xpect(&c, "slow pass: the ticks that elapsed all land, not one",
          seekhold_feed(&r.s, 1, r.now, r.allowed, r.elapsed, r.total)
              == SEEKHOLD_AIM &&
          seekhold_target(&r.s) == 75u);        /* three 5 s ticks */

    /* ---- a press born where a seek means nothing ------------------------- *
     * RIGHT on a list: the down-edge jumps to Now Playing, so one pass later
     * `allowed` is 1 — but the press is still the jump, not a transport. */
    rig_init(&r, +1, 60u, 600u);
    r.allowed = 0;
    xpect(&c, "not allowed: the down-edge on a list is silent",
          step(&r, 1) == SEEKHOLD_NONE);
    r.allowed = 1;                        /* the jump pushed Now Playing */
    step_n(&r, 1, 400, SEEKHOLD_NONE, &c,
           "not allowed: four seconds of holding it still does nothing");
    xpect(&c, "not allowed: and the release is not a skip",
          step(&r, 0) == SEEKHOLD_NONE);
    xpect(&c, "not allowed: the press after it, born on the player, skips",
          step(&r, 1) == SEEKHOLD_NONE && step(&r, 0) == SEEKHOLD_SKIP);

    /* ---- void: the other half of the same race --------------------------- *
     * The event drain can run BEFORE the live sampler shows the press, so the
     * void lands while the machine still thinks nothing is down. */
    rig_init(&r, +1, 60u, 600u);
    seekhold_void(&r.s);
    xpect(&c, "void before the press: the down-edge is silent",
          step(&r, 1) == SEEKHOLD_NONE);
    step_n(&r, 1, 400, SEEKHOLD_NONE, &c, "void before the press: no aim");
    xpect(&c, "void before the press: no skip and no commit on release",
          step(&r, 0) == SEEKHOLD_NONE);

    rig_init(&r, +1, 60u, 600u);
    (void)step(&r, 1);
    seekhold_void(&r.s);
    step_n(&r, 1, 400, SEEKHOLD_NONE, &c, "void during the press: no aim");
    xpect(&c, "void during the press: the release is silent",
          step(&r, 0) == SEEKHOLD_NONE);

    /* ---- cancel: the aim loses the screen or the track ------------------- */
    rig_init(&r, +1, 60u, 600u);
    hold_until_aim(&r, &c, "cancel: an aim in flight");
    (void)step_to_aim(&r, 40);
    r.allowed = 0;                        /* the track ended / MENU popped */
    xpect(&c, "cancel: reported once so the band repaints",
          step(&r, 1) == SEEKHOLD_CANCEL && seekhold_active(&r.s) == 0);
    step_n(&r, 1, 50, SEEKHOLD_NONE, &c, "cancel: and only once");
    xpect(&c, "cancel: the release seeks nothing",
          step(&r, 0) == SEEKHOLD_NONE);

    /* ---- the Hold switch went on under the finger ------------------------ */
    rig_init(&r, +1, 60u, 600u);
    hold_until_aim(&r, &c, "reset: an aim in flight");
    seekhold_reset(&r.s);
    xpect(&c, "reset: the aim is gone and the direction is not",
          seekhold_active(&r.s) == 0 && r.s.dir == 1);
    xpect(&c, "reset: the release is silent", step(&r, 0) == SEEKHOLD_NONE);

    /* ---- a hold across the 32-bit microsecond wrap ------------------------ */
    {
        rig_t w;
        rig_init(&w, +1, 100u, 600u);
        w.now = 0xFFFFFFFFu - 400000u;    /* 0.4 s before the wrap */
        int ok = (seekhold_feed(&w.s, 1, w.now, 1, w.elapsed, w.total)
                  == SEEKHOLD_NONE);
        w.now += 500000u;                 /* 0.1 s past it: 0.5 s into the press */
        ok = ok && (seekhold_feed(&w.s, 1, w.now, 1, w.elapsed, w.total)
                    == SEEKHOLD_AIM);
        ok = ok && (seekhold_target(&w.s) == 100u);
        w.now += 250000u;
        ok = ok && (seekhold_feed(&w.s, 1, w.now, 1, w.elapsed, w.total)
                    == SEEKHOLD_AIM);
        ok = ok && (seekhold_target(&w.s) == 105u);
        w.now += 10000u;
        ok = ok && (seekhold_feed(&w.s, 0, w.now, 1, w.elapsed, w.total)
                    == SEEKHOLD_COMMIT);
        xpect(&c, "wrap: a hold across 2^32 us aims and commits correctly", ok);
    }

    /* ---- a hold that moved nothing does not seek ------------------------- *
     * Released between the 500 ms fire and the first 250 ms tick. Committing
     * there costs a DAC stop, a re-prime and an audible rewind to land
     * exactly where playback already was — and 500-750 ms is what a slightly
     * slow "tap" measures, so it is the likeliest mis-press on the device. */
    rig_init(&r, +1, 60u, 600u);
    hold_until_aim(&r, &c, "zero-tick: the hold fires and the band flips");
    step_n(&r, 1, 10, SEEKHOLD_NONE, &c, "zero-tick: still before the first tick");
    xpect(&c, "zero-tick: the release is a cancel, not a seek to where we are",
          step(&r, 0) == SEEKHOLD_CANCEL && seekhold_active(&r.s) == 0);
    step_n(&r, 0, 5, SEEKHOLD_NONE, &c, "zero-tick: and only once");

    /* The same rule for a hold that spent its whole life pinned: fast forward
     * begun with the playhead already at the end never moves. */
    rig_init(&r, +1, 600u, 600u);
    hold_until_aim(&r, &c, "pinned from the start: the hold fires");
    step_n(&r, 1, 200, SEEKHOLD_NONE, &c, "pinned from the start: no tick moves it");
    xpect(&c, "pinned from the start: the release seeks nothing",
          step(&r, 0) == SEEKHOLD_CANCEL);

    /* ---- seekhold_cancel: the track changed under the aim ----------------- *
     * An auto-advance keeps the player active and the screen put, so the
     * machine sees only a new elapsed and a new total — indistinguishable
     * from the old track's. The caller owns that edge; this is what it buys.
     * Without it, a rewind held through the end of a 4:00 track committed
     * ~3:23 into the next one, which the listener has heard none of. */
    rig_init(&r, -1, 238u, 240u);
    hold_until_aim(&r, &c, "track change: a rewind aim in flight");
    (void)step_to_aim(&r, 40);
    xpect(&c, "track change: the aim had moved before the track ended",
          seekhold_target(&r.s) == 233u);
    xpect(&c, "track change: cancelling an aim reports it once",
          seekhold_cancel(&r.s) == SEEKHOLD_CANCEL && seekhold_active(&r.s) == 0);
    /* The next track is playing now: a new elapsed, a new (longer) total. */
    r.elapsed = 0u;
    r.total   = 300u;
    step_n(&r, 1, 300, SEEKHOLD_NONE, &c,
           "track change: three more seconds of holding aim at nothing");
    xpect(&c, "track change: and the release commits nothing",
          step(&r, 0) == SEEKHOLD_NONE && seekhold_active(&r.s) == 0);
    xpect(&c, "track change: the press after it is a normal skip",
          step(&r, 1) == SEEKHOLD_NONE && step(&r, 0) == SEEKHOLD_SKIP);

    /* Cancelling with nothing down must be a true no-op — in particular it
     * must not arm keyhold's pre-press grace and eat the next press, which is
     * why it clears the latch rather than voiding the key. */
    rig_init(&r, +1, 60u, 600u);
    xpect(&c, "cancel when idle: nothing to report",
          seekhold_cancel(&r.s) == SEEKHOLD_NONE);
    xpect(&c, "cancel when idle: the next press still skips",
          step(&r, 1) == SEEKHOLD_NONE && step(&r, 0) == SEEKHOLD_SKIP);
    hold_until_aim(&r, &c, "cancel when idle: and the press after that holds");
    (void)step_to_aim(&r, 40);
    xpect(&c, "cancel when idle: which still commits",
          step(&r, 0) == SEEKHOLD_COMMIT);

    /* ---- seekhold_missed_tap: a press that fell inside a blocked pass ----- *
     * The tick latched the down-edge, so the event survives a two-second disk
     * read; the live state the machine is fed does not. Handing the edge over
     * keeps the latch's "no tap is ever lost" promise for the transport. */
    rig_init(&r, +1, 60u, 600u);
    seekhold_missed_tap(&r.s, 0);
    xpect(&c, "missed tap: the next feed reports it as a skip",
          step(&r, 0) == SEEKHOLD_SKIP);
    step_n(&r, 0, 5, SEEKHOLD_NONE, &c, "missed tap: exactly once");

    /* A button still down at the drain is judged at the next feed, not at the
     * drain. Still down there: the machine has the press and times it, and
     * claiming it a second time would skip twice. */
    rig_init(&r, +1, 60u, 600u);
    seekhold_missed_tap(&r.s, 1);
    xpect(&c, "missed tap: a press still down is left to the machine",
          step(&r, 1) == SEEKHOLD_NONE);
    step_n(&r, 1, 10, SEEKHOLD_NONE, &c, "missed tap: nor claimed once it is timed");
    xpect(&c, "missed tap: the press reports its own skip and no second one",
          step(&r, 0) == SEEKHOLD_SKIP);
    step_n(&r, 0, 5, SEEKHOLD_NONE, &c, "missed tap: and nothing follows it");

    /* ...but already up there, the whole press fell into the gap between the
     * drain and the feed — the per-screen switch, then a render and a present
     * of the skip that came before it, tens of milliseconds. Nothing else will
     * ever report it, so it was a tap. This is the narrow half of the same
     * hole: the second tap of a double-skip beginning in the last moments of
     * the first skip's blocking file open. */
    rig_init(&r, +1, 60u, 600u);
    seekhold_missed_tap(&r.s, 1);
    r.now += 60000u;                      /* a repaint went by */
    xpect(&c, "missed tap: a press that ended in the gap is still a skip",
          step(&r, 0) == SEEKHOLD_SKIP);
    step_n(&r, 0, 5, SEEKHOLD_NONE, &c, "missed tap: exactly once, again");

    /* An edge still in the air belongs to a press, so a cancel takes it — the
     * same rule as any other press in flight. A tap already settled does not
     * go with it; that one is finished. */
    rig_init(&r, +1, 60u, 600u);
    seekhold_missed_tap(&r.s, 1);
    (void)seekhold_cancel(&r.s);
    step_n(&r, 0, 5, SEEKHOLD_NONE, &c,
           "missed tap: an edge still in the air is cancelled with the press");
    rig_init(&r, +1, 60u, 600u);
    seekhold_missed_tap(&r.s, 0);
    (void)seekhold_cancel(&r.s);
    xpect(&c, "missed tap: one already settled survives a cancel",
          step(&r, 0) == SEEKHOLD_SKIP);

    /* Claimed, then the screen went away before it could be reported: drop it
     * rather than skipping a track on a screen the user has already left. */
    rig_init(&r, +1, 60u, 600u);
    seekhold_missed_tap(&r.s, 0);
    r.allowed = 0;
    xpect(&c, "missed tap: dropped if the player screen went away first",
          step(&r, 0) == SEEKHOLD_NONE);
    r.allowed = 1;
    step_n(&r, 0, 5, SEEKHOLD_NONE, &c, "missed tap: and it does not come back");

    /* A real action landing in the same pass wins; the missed one follows on
     * the next, so a genuine double-tap produces two skips. */
    rig_init(&r, +1, 60u, 600u);
    (void)step(&r, 1);
    seekhold_missed_tap(&r.s, 0);         /* ignored: a press is in flight */
    xpect(&c, "missed tap: not claimed while a press is being timed",
          step(&r, 0) == SEEKHOLD_SKIP);
    step_n(&r, 0, 5, SEEKHOLD_NONE, &c, "missed tap: so there is no second skip");

    /* ---- allowed lost and regained under one finger ----------------------- *
     * The header says a press that loses `allowed` is dead for the rest of
     * its life. Unreachable on the device (the player cannot come back under
     * a held RIGHT), but the sentence has to be true or it is not a contract. */
    rig_init(&r, +1, 60u, 600u);
    step_n(&r, 1, 20, SEEKHOLD_NONE, &c, "regain: 200 ms down, allowed");
    r.allowed = 0;
    step_n(&r, 1, 10, SEEKHOLD_NONE, &c, "regain: allowed drops before the fire");
    r.allowed = 1;
    r.elapsed = 5u;
    step_n(&r, 1, 100, SEEKHOLD_NONE, &c,
           "regain: a second of holding after it comes back aims at nothing");
    xpect(&c, "regain: and the release is silent", step(&r, 0) == SEEKHOLD_NONE);
    xpect(&c, "regain: the next press is a fresh one",
          step(&r, 1) == SEEKHOLD_NONE && step(&r, 0) == SEEKHOLD_SKIP);

    /* ---- the drain's void, and the pass ordering it exists for ----------- *
     *
     * kernel/main.c feeds these machines from the LIVE button state at the top
     * of a pass and drains the TICK-LATCHED event further down, so a press can
     * begin BETWEEN the two. On a list screen the drain spends that press on
     * the jump to Now Playing — and by the next pass the screen IS Now Playing,
     * so `allowed` is 1 and the machine would see a perfectly ordinary
     * down-edge. seekhold_void() is what stops it owning the press.
     *
     * Both halves are asserted, because a test that only ran the voided path
     * would pass with the call deleted — which is exactly how it went missing.
     */
    {
        rig_t v;

        /* Voided: the drain got there first. */
        rig_init(&v, +1, 100, 300);
        v.allowed = 0;                       /* a list screen */
        step(&v, 0);                         /* the feed samples: still up   */
        seekhold_void(&v.s);                 /* ...the press arrives and the */
        v.allowed = 1;                       /* drain pushes Now Playing     */
        int quiet = (step(&v, 1) == SEEKHOLD_NONE) &&
                    (step(&v, 0) == SEEKHOLD_NONE);
        xpect(&c, "drain: a press voided by the jump is not a skip on release",
              quiet);

        rig_init(&v, +1, 100, 300);
        v.allowed = 0;
        step(&v, 0);
        seekhold_void(&v.s);
        v.allowed = 1;
        int held_quiet = 1;
        for (int i = 0; i < 200; i++) {      /* two seconds of holding it */
            if (step(&v, 1) != SEEKHOLD_NONE) held_quiet = 0;
        }
        xpect(&c, "drain: ...and holding it never aims, however long",
              held_quiet && !seekhold_active(&v.s));
        xpect(&c, "drain: the release after that hold is silent too",
              step(&v, 0) == SEEKHOLD_NONE);
        xpect(&c, "drain: and the NEXT press is a fresh, ordinary one",
              step(&v, 1) == SEEKHOLD_NONE && step(&v, 0) == SEEKHOLD_SKIP);

        /* Not voided: the same timeline is a skip. This is the regression the
         * assertions above are guarding, stated as the behaviour it would be. */
        rig_init(&v, +1, 100, 300);
        v.allowed = 0;
        step(&v, 0);
        v.allowed = 1;                       /* no void */
        xpect(&c, "drain: WITHOUT the void that same press skips the track",
              step(&v, 1) == SEEKHOLD_NONE && step(&v, 0) == SEEKHOLD_SKIP);
    }

    /* ---- the PLAY-tap policy, screen by screen --------------------------- *
     * The manual's rule: Play on a highlighted list title plays that list.
     * Everywhere there is no title under the cursor it stays pause/resume —
     * including a list that is empty, so "Play works everywhere" holds. */
    {
        static const struct {
            gesture_ctx_t ctx;
            const char   *name;
            gesture_play_t want_full;    /* with rows */
        } cases[] = {
            { GESTURE_CTX_MENU,          "the main menu",     GESTURE_PLAY_PAUSE },
            { GESTURE_CTX_MUSIC_SHUFFLE, "Shuffle Songs",     GESTURE_PLAY_START },
            { GESTURE_CTX_MUSIC_OTHER,   "a Music menu row",  GESTURE_PLAY_PAUSE },
            { GESTURE_CTX_LIST_TITLE,    "a list title",      GESTURE_PLAY_START },
            { GESTURE_CTX_LIST_TRACK,    "a track row",       GESTURE_PLAY_START },
            { GESTURE_CTX_PLAYER,        "Now Playing",       GESTURE_PLAY_PAUSE },
            { GESTURE_CTX_SETTINGS,      "Settings",          GESTURE_PLAY_PAUSE },
            { GESTURE_CTX_MODAL,         "a modal",           GESTURE_PLAY_PAUSE },
        };
        int ok_full = 1, ok_empty = 1;
        for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            if (gesture_play_tap(cases[i].ctx, 1) != cases[i].want_full ||
                gesture_play_tap(cases[i].ctx, 5000) != cases[i].want_full) {
                printf("  [gesture] %s with rows: wrong answer\n", cases[i].name);
                ok_full = 0;
            }
            if (gesture_play_tap(cases[i].ctx, 0) != GESTURE_PLAY_PAUSE) {
                printf("  [gesture] %s when empty: should pause\n", cases[i].name);
                ok_empty = 0;
            }
        }
        xpect(&c, "play tap: only a row that names or is a track starts one",
              ok_full);
        xpect(&c, "play tap: an empty list pauses or resumes instead", ok_empty);

        /* Music > Search rides on these two rules rather than a context of its
         * own. Its PICKER reports a count of 0 — a text field has no row under
         * the cursor, so PLAY stays the transport it is everywhere else — and a
         * RESULT row reports LIST_TRACK for a song and LIST_TITLE for an
         * artist, album or playlist. ui/search.c's search_play_rows() is what
         * kernel/main.c asks; these are the answers it gets back. */
        xpect(&c, "play tap: Search's picker (no row, count 0) is pause/resume",
              gesture_play_tap(GESTURE_CTX_LIST_TITLE, 0) == GESTURE_PLAY_PAUSE &&
              gesture_play_tap(GESTURE_CTX_LIST_TRACK, 0) == GESTURE_PLAY_PAUSE);
        xpect(&c, "play tap: a Search result starts its queue, song row or not",
              gesture_play_tap(GESTURE_CTX_LIST_TRACK, 12) == GESTURE_PLAY_START &&
              gesture_play_tap(GESTURE_CTX_LIST_TITLE, 12) == GESTURE_PLAY_START);
    }

    return xfail_done(&c);
}
