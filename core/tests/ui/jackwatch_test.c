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
 *   - re-inserting the plug is a notification (IN), never an instruction;
 *   - a pull is PAUSE only while something is playing, and OUT otherwise —
 *     the distinction kernel/main.c's suspend loop turns into "do not resume
 *     at wake".
 *
 * MUTATION CHECK: each of these breaks the suite —
 *   - priming `last` from a -1 level: unknown_never_primes and
 *     minus_one_mid_stream fail;
 *   - dropping the `playing` guard: pull_while_paused fails;
 *   - returning PAUSE on the 0 -> 1 edge: replug_never_resumes fails;
 *   - collapsing OUT into PAUSE: pull_while_paused fails;
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
          j.last == -1 && j.raw_last == -1 && j.raw_edges == 0 &&
          j.pauses == 0 && j.edge_us == 0);

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
        xpect(&c, "pull while playing: counted and timestamped",
              j.pauses == 1 && j.edge_us == t_pull && j.last == 0);
    }
    feed_n(&j, 0, 1, &now, 100, JACKWATCH_NONE, &c,
           "pull while playing: the level staying 0 is not a second pull");
    xpect(&c, "pull while playing: still exactly one pause", j.pauses == 1);

    /* --- pull while paused / nothing loaded: nothing to pause ------------ */
    jackwatch_reset(&j);
    now += PASS_US;
    jackwatch_feed(&j, 1, 0, now);
    now += PASS_US;
    xpect(&c, "pull while paused: OUT, which is a notification, not a pause",
          jackwatch_feed(&j, 0, 0, now) == JACKWATCH_OUT);
    xpect(&c, "pull while paused: nothing counted", j.pauses == 0);
    xpect(&c, "pull while paused: the edge still moved the believed level",
          j.last == 0 && j.edge_us == now);

    /* --- re-inserting never resumes -------------------------------------- */
    jackwatch_reset(&j);
    now += PASS_US;
    jackwatch_feed(&j, 1, 1, now);
    now += PASS_US;
    jackwatch_feed(&j, 0, 1, now);              /* PAUSE */
    now += PASS_US;
    xpect(&c, "replug: the plug going back in is IN, never an instruction",
          jackwatch_feed(&j, 1, 0, now) == JACKWATCH_IN);
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

    /* --- the suspend loop, which feeds this module the same way ----------
     *
     * kernel/main.c keeps feeding the watcher through a suspend, with
     * `playing` = the transport state the sleep interrupted, and answers a
     * PAUSE by declining to resume at wake. These two sequences are the
     * reason that works: a pull is seen WHILE it happens, so a plug that
     * comes back before the wake cannot hide it. */
    jackwatch_reset(&j);
    now += PASS_US;
    jackwatch_feed(&j, 1, 1, now);              /* asleep with the plug in   */
    now += PASS_US;
    xpect(&c, "suspend: a pull during the sleep is a PAUSE (do not resume)",
          jackwatch_feed(&j, 0, 1, now) == JACKWATCH_PAUSE);
    now += PASS_US;
    xpect(&c, "suspend: pulled and RE-INSERTED before the wake is only an IN "
              "— the PAUSE already dropped main.c's was_playing, so the wake "
              "still declines",
          jackwatch_feed(&j, 1, 1, now) == JACKWATCH_IN && j.pauses == 1);
    feed_n(&j, 1, 1, &now, 10, JACKWATCH_NONE, &c,
           "suspend: and a seated plug stays silent afterwards");

    /* --- the wake re-prime (the backstop) --------------------------------- */
    jackwatch_reset(&j);
    now += PASS_US;
    jackwatch_feed(&j, 1, 1, now);
    jackwatch_prime(&j, 0);                     /* woken: it is out now      */
    xpect(&c, "wake: priming moves the believed level with no action",
          j.last == 0 && j.pauses == 0);
    feed_n(&j, 0, 0, &now, 10, JACKWATCH_NONE, &c,
           "wake: no phantom pause on the passes after the prime");
    jackwatch_prime(&j, -1);
    xpect(&c, "wake: priming with -1 leaves the believed level alone",
          j.last == 0);
    jackwatch_prime(&j, 1);
    xpect(&c, "wake: priming never counts a pause and never acts",
          j.last == 1 && j.pauses == 0);

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
                  a.last == b.last && a.raw_last == b.raw_last &&
                  a.raw_edges == b.raw_edges);
    }

    return xfail_done(&c);
}
