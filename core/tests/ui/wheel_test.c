/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ui/wheel_test.c — the wheel acceleration state machine and the A-Z
 * locator (core/ui/wheel.c) on the host.
 *
 * THE POINT OF THIS FILE. Until wheel.c existed all of this was static in
 * kernel/main.c, so the only way to check the velocity curve, the idle reset,
 * the letter-mode latch, its hold window, the tick accumulator or the guard
 * that lets letterless screens fall through to row scrolling was to spin the
 * wheel and watch. Several of those had already been wrong once (the
 * double-step from a remainder every detent; the fast spin that did nothing
 * on menus; the letter that flickered mid-spin) and were fixed by feel.
 *
 * The three things wheel.c cannot do portably are injected: a clock this
 * test advances by hand, a row-initial source backed by a string of letters,
 * and a click counter. Every "event" below is therefore a (delta, gap)
 * pair, and the assertions are exact selection indices — the numbers a real
 * spin would land on — rather than "it moved".
 */

#include <stdio.h>
#include <string.h>

#include "wheel.h"
#include "../xfail.h"

/* ---- the injected environment ------------------------------------------ */

static uint32_t g_now;                   /* the wheel's clock, in µs          */
static uint32_t clock_fn(void) { return g_now; }

static const char *g_letters;            /* row -> initial; NULL = no letters */
static int         g_count;
static char initial_fn(int idx)
{
    if (!g_letters || idx < 0 || idx >= g_count) return 0;
    return g_letters[idx];
}

static int  g_clicks;
static void click_fn(void) { g_clicks++; }

/* The letter-step seam (§9): what kernel_main registers over the run index. */
static int g_step_calls, g_step_sel, g_step_count, g_step_dir, g_step_target;
static int step_stub(int sel, int count, int dir)
{
    g_step_calls++;
    g_step_sel   = sel;
    g_step_count = count;
    g_step_dir   = dir;
    return g_step_target;
}

/* A list under the wheel: selection, size, sub-detent remainder. */
typedef struct { int sel, count, accum; } list_t;

/* One wheel event: advance the clock by `gap_us`, apply `delta` ticks. */
static int ev(list_t *l, int delta, uint32_t gap_us)
{
    g_now += gap_us;
    l->sel = wheel_move(l->sel, l->count, (int8_t)delta, &l->accum);
    return l->sel;
}

/* A gesture that is over: longer than WHEEL_IDLE_US since the last event.
 * It resets the SPEED; in letter mode it leaves the latch alone, because it
 * is shorter than WHEEL_AZ_HOLD_LETTER (the plate is still up). */
#define IDLE   (WHEEL_IDLE_US + 50000u)
/* A gesture that is over for good: past the plate's hold, so letter mode has
 * ended too and the next detent is one row. */
#define GONE   (WHEEL_AZ_HOLD_LETTER + 50000u)
/* A deliberate detent-by-detent pace: same gesture, but far too slow to
 * accelerate (4 ticks / 150 ms = 26 ticks/s, under WHEEL_TPS_ACCEL). */
#define SLOW   150000u
/* A real spin: 4 ticks every 10 ms = 400 ticks/s, ~4 turns a second. */
#define FAST   10000u

static void fresh(list_t *l, int count, const char *letters)
{
    wheel_accel_reset();
    l->sel = 0; l->count = count; l->accum = 0;
    g_letters = letters; g_count = count;
    g_clicks = 0;
    g_now += IDLE;
}

/* 26 letters x 4 rows: "AAAABBBB...ZZZZ". */
static char g_az[26 * 4 + 1];

int main(void)
{
    xfail_ctx c = { "wheel", 0, 0, 0 };
    list_t l;

    for (int i = 0; i < 26 * 4; i++) g_az[i] = (char)('A' + i / 4);
    g_az[26 * 4] = '\0';

    /* ---- 1. nothing registered: still safe ------------------------------ */
    {
        int acc = 0;
        /* No clock: every event reads as t=0, so no gap is ever "idle" and
         * the estimate runs straight to full tilt — WHEEL_VEL_MAX rows a
         * detent from the first event. Safe, not sensible: the device
         * registers its clock before the UI loop runs. */
        int s = wheel_move(0, 100, 4, &acc);
        xpect(&c, "no hooks: does not fault; no clock means full tilt at once",
              s == WHEEL_VEL_MAX && acc == 0);
        s = wheel_move(s, 100, 4, &acc);
        xpect(&c, "no letter source: never letter-steps, keeps row scrolling",
              s == 2 * WHEEL_VEL_MAX);
    }
    wheel_set_clock(clock_fn);
    wheel_set_initial_at(initial_fn);
    wheel_set_click(click_fn);
    g_now = 1000000u;

    /* ---- 2. the detent arithmetic --------------------------------------- */
    fresh(&l, 100, NULL);
    xpect(&c, "one detent (4 ticks) = one row, one click", ev(&l, 4, SLOW) == 1 && g_clicks == 1);
    xpect(&c, "half a detent moves nothing and is not a click",
          ev(&l, 2, SLOW) == 1 && l.accum == 2 && g_clicks == 1);
    xpect(&c, "...the other half completes the row",
          ev(&l, 2, SLOW) == 2 && l.accum == 0 && g_clicks == 2);
    xpect(&c, "a detent back", ev(&l, -4, SLOW) == 1);
    xpect(&c, "halves back", ev(&l, -2, SLOW) == 1 && ev(&l, -2, SLOW) == 0);
    /* Divisor == sensitivity: 3 detents are exactly 3 rows, never 1,1,2. */
    fresh(&l, 100, NULL);
    ev(&l, 4, SLOW); ev(&l, 4, SLOW); ev(&l, 4, SLOW);
    xpect(&c, "three slow detents are exactly three rows", l.sel == 3 && l.accum == 0);

    /* ---- 3. clamps and the flick cap ------------------------------------ */
    fresh(&l, 5, NULL);
    l.sel = 4;
    xpect(&c, "at the end: stays, no click", ev(&l, 4, SLOW) == 4 && g_clicks == 0);
    l.sel = 0;
    xpect(&c, "at the top: stays, no click", ev(&l, -4, SLOW) == 0 && g_clicks == 0);
    fresh(&l, 1, NULL);
    xpect(&c, "a one-row list never moves", ev(&l, 4, SLOW) == 0 && ev(&l, -4, SLOW) == 0);
    fresh(&l, 100, NULL);
    xpect(&c, "a flick is capped at WHEEL_MAX_DELTA ticks (2 rows) per event",
          ev(&l, 100, IDLE) == 2 && l.accum == 0);
    xpect(&c, "...and backwards", ev(&l, -100, IDLE) == 0);
    /* The contract is "a selection index in [0, count)"; for count == 0 the
     * clamp yields count - 1. No caller passes an empty list today (every
     * list view guards its count), so this pins the edge rather than a fault. */
    fresh(&l, 0, NULL);
    xpect(&c, "an empty list: the clamp answers count - 1", ev(&l, 4, SLOW) == -1);

    /* ---- 4. acceleration, exactly --------------------------------------- */
    /* 400 ticks/s smoothed 3:1 per event: tps 0,100,175,231,273,... so the
     * velocity curve 1 + (tps-50)*7/200 gives 1,2,5,7,8,8 rows per detent.
     * These are the numbers, not "it got faster". */
    fresh(&l, 1000, NULL);
    int path[7], ok = 1;
    static const int want[7] = { 1, 3, 8, 15, 23, 31, 39 };
    for (int i = 0; i < 7; i++) {
        path[i] = ev(&l, 4, i == 0 ? IDLE : FAST);
        if (path[i] != want[i]) ok = 0;
    }
    xpect(&c, "fast spin lands on 1,3,8,15,23,31,39 (vel 1,2,5,7,8,8,8)", ok);
    if (!ok) {
        fprintf(stderr, "  got:");
        for (int i = 0; i < 7; i++) fprintf(stderr, " %d", path[i]);
        fprintf(stderr, "\n");
    }
    xpect(&c, "at full tilt letter mode is latched (vel >= WHEEL_AZ_VEL)",
          wheel_letter_mode() == 1);
    /* The documented bug: letter mode is latched, this screen has no letters,
     * and the wheel must fall through to row scrolling — not do nothing. */
    xpect(&c, "a letterless screen falls through to row scrolling in letter mode",
          ev(&l, 4, FAST) == 47 && g_clicks == 8);
    /* A pause is not the end of the gesture while the PLATE is still up: the
     * thing on screen and the thing the wheel does share one clock. Only the
     * speed resets, so the next detent is one unit rather than eight rows. */
    xpect(&c, "a pause under the plate's hold keeps letter mode, at velocity 1",
          ev(&l, 4, IDLE) == 48 && wheel_letter_mode() == 1 && wheel_accelerating());
    xpect(&c, "a pause past the plate's hold is a new gesture, one row a detent",
          ev(&l, 4, GONE) == 49 && wheel_letter_mode() == 0 && !wheel_accelerating());
    /* The same gap with no latch to keep is the plain reset it always was. */
    fresh(&l, 1000, NULL);
    ev(&l, 4, IDLE); ev(&l, 4, FAST);      /* vel 2: fast, but not letters   */
    xpect(&c, "a pause with nothing latched is still a plain reset to velocity 1",
          !wheel_letter_mode() && l.sel == 3 && ev(&l, 4, IDLE) == 4);

    fresh(&l, 200, NULL);
    l.sel = 100;
    int back[6], okb = 1;
    static const int wantb[6] = { 99, 97, 92, 85, 77, 69 };
    for (int i = 0; i < 6; i++) {
        back[i] = ev(&l, -4, i == 0 ? IDLE : FAST);
        if (back[i] != wantb[i]) okb = 0;
    }
    xpect(&c, "backwards spin is the mirror image: 99,97,92,85,77,69", okb);

    /* ---- 5. the clock wraps --------------------------------------------- */
    wheel_accel_reset();
    g_now = 0xFFFFFFF0u;
    xpect(&c, "first event after reset is velocity 1", wheel_accel_step(4) == 1);
    g_now = 0x00000010u;                  /* 32 µs later, across the wrap  */
    xpect(&c, "a gap across the 32-bit wrap is a short gap, not an idle one",
          wheel_accel_step(4) == WHEEL_VEL_MAX);
    xpect(&c, "wheel_last_us is the clock at the last event",
          wheel_last_us() == 0x00000010u);
    wheel_accel_reset();
    xpect(&c, "...and 0 after a reset", wheel_last_us() == 0);

    /* ---- 6. letter stepping on a sorted list ----------------------------- */
    fresh(&l, 10, "AAABBCDDDD");
    xpect(&c, "step: mid-A forward -> first B", list_letter_step(1, 10, 1) == 3);
    xpect(&c, "step: first B forward -> C", list_letter_step(3, 10, 1) == 5);
    xpect(&c, "step: C forward -> first D", list_letter_step(5, 10, 1) == 6);
    xpect(&c, "step: in the last letter forward stays",
          list_letter_step(6, 10, 1) == 6 && list_letter_step(9, 10, 1) == 9);
    xpect(&c, "step: mid-D back -> head of D", list_letter_step(8, 10, -1) == 6);
    xpect(&c, "step: head of D back -> head of C", list_letter_step(6, 10, -1) == 5);
    xpect(&c, "step: C back -> head of B", list_letter_step(5, 10, -1) == 3);
    xpect(&c, "step: head of B back -> head of A", list_letter_step(3, 10, -1) == 0);
    xpect(&c, "step: mid-A back -> head of A, then stays",
          list_letter_step(2, 10, -1) == 0 && list_letter_step(0, 10, -1) == 0);
    xpect(&c, "step: empty list stays", list_letter_step(3, 0, 1) == 3);
    g_letters = NULL;
    xpect(&c, "step: letterless screen stays", list_letter_step(3, 10, 1) == 3 &&
                                              list_letter_step(3, 10, -1) == 3);

    /* ---- 7. letter mode through the wheel -------------------------------- */
    fresh(&l, 26 * 4, g_az);
    ev(&l, 4, IDLE);                      /* vel 1: row  -> 1 (A)          */
    ev(&l, 4, FAST);                      /* vel 2: rows -> 3 (last A)     */
    xpect(&c, "under WHEEL_AZ_VEL the wheel still moves rows", l.sel == 3 && !wheel_letter_mode());
    xpect(&c, "at WHEEL_AZ_VEL a detent is one letter: last A -> first B",
          ev(&l, 4, FAST) == 4 && wheel_letter_mode());
    xpect(&c, "...and each detent is the next letter's first row",
          ev(&l, 4, FAST) == 8 && ev(&l, 4, FAST) == 12);
    xpect(&c, "letter back: first D -> first C", ev(&l, -4, FAST) == 8);
    /* The latch: five slow detents bring the smoothed estimate under the
     * threshold (vel 2), yet the gesture is still in letter mode, so a
     * detent is still a letter — the unit must not flip under the thumb. */
    for (int i = 0; i < 5; i++) ev(&l, 4, SLOW);
    xpect(&c, "letter mode is latched for the gesture even as speed drops",
          wheel_letter_mode() == 1 && l.sel == 8 + 5 * 4);
    /* Lifting a thumb to read the plate is not "I am done": while the letter
     * is on screen the next detent is still a letter. */
    xpect(&c, "a pause the plate outlives still steps letters",
          ev(&l, 4, IDLE) == 8 + 6 * 4 && wheel_letter_mode() == 1);
    xpect(&c, "...until the plate goes down: then one detent is one row again",
          ev(&l, 4, GONE) == 8 + 6 * 4 + 1 && wheel_letter_mode() == 0);
    /* At the end of the alphabet a letter step has nowhere to go: stays. */
    fresh(&l, 26 * 4, g_az);
    l.sel = 26 * 4 - 2;                   /* second-to-last Z             */
    ev(&l, 4, IDLE); ev(&l, 4, FAST); ev(&l, 4, FAST);
    int before = l.sel;
    g_clicks = 0;
    xpect(&c, "in the last letter a letter step stays and does not click",
          wheel_letter_mode() && ev(&l, 4, FAST) == before && g_clicks == 0);
    /* Clicks: one per event that moved, whatever the unit. */
    fresh(&l, 26 * 4, g_az);
    ev(&l, 4, IDLE); ev(&l, 4, FAST); ev(&l, 4, FAST); ev(&l, 4, FAST);
    xpect(&c, "one click per event that moved the cursor", g_clicks == 4);

    /* ---- 8. the A-Z plate's hold window ---------------------------------- */
    fresh(&l, 26 * 4, g_az);
    xpect(&c, "a slow detent shows no letter", ev(&l, 4, IDLE) == 1 && !wheel_accelerating());
    ev(&l, 4, FAST); ev(&l, 4, FAST);     /* latched                        */
    uint32_t last = wheel_last_us();
    xpect(&c, "in letter mode the plate is up", wheel_accelerating());
    g_now = last + WHEEL_AZ_HOLD_LETTER - 1;
    xpect(&c, "...and stays up just short of WHEEL_AZ_HOLD_LETTER", wheel_accelerating());
    g_now = last + WHEEL_AZ_HOLD_LETTER;
    xpect(&c, "...and is down at WHEEL_AZ_HOLD_LETTER", !wheel_accelerating());
    /* The plate follows letter mode and nothing else. Speed falling under
     * WHEEL_AZ_VEL mid-gesture (five slow detents) does not shorten the
     * hold: letter mode is latched, and there is no second, shorter hold for
     * a "fast but not letters" state — no such state exists, because the
     * step that reaches WHEEL_AZ_VEL is the step that latches letter mode. */
    fresh(&l, 26 * 4, g_az);
    ev(&l, 4, IDLE); ev(&l, 4, FAST); ev(&l, 4, FAST);
    for (int i = 0; i < 5; i++) ev(&l, 4, SLOW);
    last = wheel_last_us();
    g_now = last + WHEEL_AZ_HOLD_LETTER - 1;
    xpect(&c, "speed dropping under WHEEL_AZ_VEL keeps the letter hold",
          wheel_letter_mode() && wheel_accelerating());
    fresh(&l, 26 * 4, g_az);
    ev(&l, 4, IDLE); ev(&l, 4, FAST);     /* vel 2: fast-ish, no letters   */
    xpect(&c, "under WHEEL_AZ_VEL there is no plate at all, not a shorter hold",
          !wheel_letter_mode() && !wheel_accelerating());
    ev(&l, 4, IDLE); ev(&l, 4, FAST); ev(&l, 4, FAST);
    wheel_accel_reset();
    xpect(&c, "a reset takes the plate down at once", !wheel_accelerating());

    /* ---- 9. the letter-step seam ----------------------------------------- */
    /* On the device the detent is answered by ui/letterindex.c's run index,
     * registered through wheel_set_letter_step; the walk is the fallback for
     * anything that does not register one. The stub proves WHICH one runs,
     * with what arguments, and that its answer is the one honoured. */
    fresh(&l, 26 * 4, g_az);
    ev(&l, 4, IDLE); ev(&l, 4, FAST); ev(&l, 4, FAST);   /* latched, at 4 */
    wheel_set_letter_step(step_stub);
    g_step_calls  = 0;
    g_step_target = 40;
    int at9 = l.sel;
    xpect(&c, "in letter mode the registered step decides where a detent lands",
          ev(&l, 4, FAST) == 40 && g_step_calls == 1 && g_step_sel == at9 &&
          g_step_count == 26 * 4 && g_step_dir == 1);
    xpect(&c, "...and backwards it is asked for the other direction",
          ev(&l, -4, FAST) == 40 && g_step_dir == -1);
    /* An answer equal to `sel` means "no further letter": stop, and no click. */
    g_step_calls = 0;
    g_clicks     = 0;
    xpect(&c, "a step that stays is the end of the list: no move, no click",
          ev(&l, 4, FAST) == 40 && g_step_calls == 1 && g_clicks == 0);
    /* Unset and the walk is back — the same wheel, its own answers. */
    wheel_set_letter_step(0);
    g_step_calls = 0;
    xpect(&c, "unset falls back to the walk (row 40 is mid-K: next is L at 44)",
          ev(&l, 4, FAST) == 44 && g_step_calls == 0);
    /* The guard still comes first: a screen with no letters never asks. */
    fresh(&l, 1000, NULL);
    wheel_set_letter_step(step_stub);
    g_step_calls  = 0;
    g_step_target = 0;
    ev(&l, 4, IDLE); ev(&l, 4, FAST); ev(&l, 4, FAST); ev(&l, 4, FAST);
    xpect(&c, "a letterless screen never reaches the step at all",
          wheel_letter_mode() && g_step_calls == 0 && l.sel == 15);
    wheel_set_letter_step(0);

    return xfail_done(&c);
}
