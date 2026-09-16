/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ui/jackwatch_test.c — the pause-on-unplug policy (core/ui/jackwatch.c)
 * on the host.
 *
 * THE POINT OF THIS FILE. This decision used to be five lines inline in
 * kernel/main.c, which is not host-built, so the only way to find out what it
 * did with an unplug during a suspend was to pull a plug on the device. Each
 * case below is a row of the table in the plan / jackwatch.h, fed the way the
 * main loop feeds it: one debounced level per pass, plus whether the
 * transport is playing.
 *
 * The three that matter most, because they are the ones a regression would
 * get wrong silently:
 *   - a -1 (the shipping build, where the detect line is not yet trusted)
 *     is NOT a level: it never primes and never pauses;
 *   - re-inserting the plug never resumes;
 *   - the wake re-prime moves the believed level with no action, which is
 *     what keeps a pull-during-sleep from pausing a player that the wake
 *     path already decided to leave paused.
 *
 * MUTATION CHECK: each of these breaks the suite —
 *   - priming `last` from a -1 level: unknown_never_primes and
 *     minus_one_mid_stream fail;
 *   - dropping the `playing` guard: pull_while_paused fails;
 *   - returning PAUSE on the 0 -> 1 edge: replug_never_resumes fails;
 *   - making jackwatch_prime act instead of prime: prime_on_wake fails;
 *   - counting the first raw sample as an edge: note_raw_counts_transitions
 *     fails.
 */

#include <stdio.h>

#include "jackwatch.h"
#include "../xfail.h"

/* A pass of the main loop, at the ~10 ms idle cadence. */
#define PASS_US 10000u

static int feed_n(jackwatch_t *j, int level, int playing, uint32_t *now, int n,
                  jackwatch_action_t want, xfail_ctx *c, const char *what)
{
    int ok = 1;
    for (int i = 0; i < n; i++) {
        *now += PASS_US;
        if (jackwatch_feed(j, level, playing, *now) != want) {
            ok = 0;
        }
    }
    xpect(c, what, ok);
    return ok;
}

/* The pull table, run from a known-seated state: 1 (prime) then 0 while
 * playing must be exactly one PAUSE and nothing after it. Used twice, from
 * two independently reset machines, to state that the answer depends on
 * NOTHING but the level and `playing` — there is no screen, Hold or charging
 * field in jackwatch_t for anything else to have got at. */
static int pull_table(jackwatch_t *j, uint32_t *now)
{
    int ok = 1;
    jackwatch_reset(j);
    *now += PASS_US;
    ok &= jackwatch_feed(j, 1, 1, *now) == JACKWATCH_NONE;
    *now += PASS_US;
    ok &= jackwatch_feed(j, 0, 1, *now) == JACKWATCH_PAUSE;
    for (int i = 0; i < 20; i++) {
        *now += PASS_US;
        ok &= jackwatch_feed(j, 0, 0, *now) == JACKWATCH_NONE;
    }
    ok &= j->pauses == 1;
    return ok;
}

int main(void)
{
    xfail_ctx c = { "jackwatch", 0, 0, 0 };
    jackwatch_t j;
    uint32_t now = 1000u;

    /* --- reset is the "nothing known yet" state ------------------------- */
    jackwatch_reset(&j);
    xpect(&c, "reset: no level believed, no raw sample, no counts",
          j.last == -1 && j.raw_last == -1 && j.paused_by == 0 &&
          j.raw_edges == 0 && j.pauses == 0 && j.edge_us == 0);

    /* --- -1 is not a level: the shipping (untrusted) build --------------- */
    jackwatch_reset(&j);
    feed_n(&j, -1, 1, &now, 50, JACKWATCH_NONE, &c,
           "unknown: 50 passes of -1 while playing do nothing");
    xpect(&c, "unknown: -1 never primes the believed level", j.last == -1);
    xpect(&c, "unknown: nothing was paused", j.pauses == 0);

    /* --- boot with the jack empty: a prime, never an edge ---------------- */
    jackwatch_reset(&j);
    now += PASS_US;
    xpect(&c, "boot empty: the first level is silent",
          jackwatch_feed(&j, 0, 1, now) == JACKWATCH_NONE && j.last == 0);
    feed_n(&j, 0, 1, &now, 100, JACKWATCH_NONE, &c,
           "boot empty: staying empty while playing never pauses");
    xpect(&c, "boot empty: no pause counted", j.pauses == 0);

    /* --- boot with the plug in, then pulled: the whole point ------------- */
    jackwatch_reset(&j);
    now += PASS_US;
    xpect(&c, "boot seated: the first level is silent",
          jackwatch_feed(&j, 1, 1, now) == JACKWATCH_NONE && j.last == 1);
    now += PASS_US;
    {
        uint32_t t_pull = now;
        xpect(&c, "pull while playing: PAUSE, once, on the edge",
              jackwatch_feed(&j, 0, 1, now) == JACKWATCH_PAUSE);
        xpect(&c, "pull while playing: counted, claimed, and timestamped",
              j.pauses == 1 && j.paused_by == 1 && j.edge_us == t_pull &&
              j.last == 0);
    }
    feed_n(&j, 0, 1, &now, 100, JACKWATCH_NONE, &c,
           "pull while playing: the level staying 0 is not a second pull");
    xpect(&c, "pull while playing: still exactly one pause", j.pauses == 1);

    /* --- pull while paused / nothing loaded: nothing to pause ------------ */
    jackwatch_reset(&j);
    now += PASS_US;
    jackwatch_feed(&j, 1, 0, now);
    now += PASS_US;
    xpect(&c, "pull while paused: silent",
          jackwatch_feed(&j, 0, 0, now) == JACKWATCH_NONE);
    xpect(&c, "pull while paused: nothing counted, nothing claimed",
          j.pauses == 0 && j.paused_by == 0);
    xpect(&c, "pull while paused: the edge still moved the believed level",
          j.last == 0 && j.edge_us == now);

    /* --- re-inserting never resumes -------------------------------------- */
    jackwatch_reset(&j);
    now += PASS_US;
    jackwatch_feed(&j, 1, 1, now);
    now += PASS_US;
    jackwatch_feed(&j, 0, 1, now);              /* PAUSE */
    now += PASS_US;
    xpect(&c, "replug: the plug going back in is silent",
          jackwatch_feed(&j, 1, 0, now) == JACKWATCH_NONE);
    xpect(&c, "replug: our claim on the pause is retired", j.paused_by == 0);
    feed_n(&j, 1, 0, &now, 50, JACKWATCH_NONE, &c,
           "replug: and stays silent while it is seated");
    now += PASS_US;
    xpect(&c, "replug: a SECOND genuine pull pauses again",
          jackwatch_feed(&j, 0, 1, now) == JACKWATCH_PAUSE && j.pauses == 2);

    /* --- -1 mid-stream: the HAL losing its answer is not an unplug ------- */
    jackwatch_reset(&j);
    now += PASS_US;
    jackwatch_feed(&j, 1, 1, now);
    feed_n(&j, -1, 1, &now, 5, JACKWATCH_NONE, &c,
           "gap: -1 between two real levels does nothing");
    xpect(&c, "gap: the believed level is still seated", j.last == 1);
    now += PASS_US;
    xpect(&c, "gap: the 0 after it is still an edge from 1",
          jackwatch_feed(&j, 0, 1, now) == JACKWATCH_PAUSE);

    /* --- the wake re-prime ------------------------------------------------ */
    jackwatch_reset(&j);
    now += PASS_US;
    jackwatch_feed(&j, 1, 1, now);              /* asleep with the plug in   */
    jackwatch_prime(&j, 0);                     /* woken: it is out now      */
    xpect(&c, "wake: priming moves the believed level with no action",
          j.last == 0 && j.pauses == 0);
    feed_n(&j, 0, 0, &now, 10, JACKWATCH_NONE, &c,
           "wake: no phantom pause on the passes after the prime");
    jackwatch_prime(&j, -1);
    xpect(&c, "wake: priming with -1 leaves the believed level alone",
          j.last == 0);
    /* Pulled AND re-inserted during the sleep: the wake sees 1 again. The
     * player stays paused (main.c's resume gate, not ours), and the claim
     * goes with the plug. */
    jackwatch_reset(&j);
    now += PASS_US;
    jackwatch_feed(&j, 1, 1, now);
    now += PASS_US;
    jackwatch_feed(&j, 0, 1, now);              /* PAUSE, paused_by = 1      */
    jackwatch_prime(&j, 1);
    xpect(&c, "wake: a seated prime clears the claim and never resumes",
          j.last == 1 && j.paused_by == 0 && j.pauses == 1);

    /* --- raw bookkeeping (the About token and the log lines) ------------- */
    jackwatch_reset(&j);
    {
        int raws[5]  = { 1, 1, 0, 0, 1 };
        int want[5]  = { 1, 0, 1, 0, 1 };   /* first sample narrates too     */
        int edges[5] = { 0, 0, 1, 1, 2 };
        int ok = 1;
        for (int i = 0; i < 5; i++) {
            if (jackwatch_note_raw(&j, raws[i]) != want[i] ||
                j.raw_edges != edges[i]) {
                ok = 0;
            }
        }
        xpect(&c, "raw: the first sample narrates but is not an edge; "
                  "transitions counted, repeats silent", ok);
    }
    xpect(&c, "raw: bookkeeping never pauses anything and never touches "
              "the believed level", j.pauses == 0 && j.last == -1);

    /* --- the module does no timing: a clock wrap across a pull ----------- */
    jackwatch_reset(&j);
    jackwatch_feed(&j, 1, 1, 0xFFFFFFF0u);
    xpect(&c, "wrap: a pull whose timestamp wrapped is still a PAUSE",
          jackwatch_feed(&j, 0, 1, 0x10u) == JACKWATCH_PAUSE);
    xpect(&c, "wrap: the wrapped timestamp is recorded as given",
          j.edge_us == 0x10u);

    /* --- nothing but (level, playing) decides ---------------------------- */
    {
        jackwatch_t a, b;
        int ok = pull_table(&a, &now);
        ok &= pull_table(&b, &now);
        xpect(&c, "no hidden state: two fresh machines answer the pull table "
                  "identically", ok && a.pauses == b.pauses &&
                  a.last == b.last && a.paused_by == b.paused_by);
    }

    return xfail_done(&c);
}
