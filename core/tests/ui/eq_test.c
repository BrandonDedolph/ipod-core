/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ui/eq_test.c — host test for the equaliser preset table (ui/eq.c).
 *
 * eq.c is pure: a name, five gains and five centre codes per preset. The ARM
 * build links this exact source, so what is asserted here is what the codec
 * is asked for on the device.
 *
 * The fixture below restates every preset's gains INDEPENDENTLY of the table
 * (it is the table from the plan that specified them, typed out again) — the
 * point being that a slip in eq.c's numbers has to show up as a mismatch here
 * rather than being copied into the expectation. Everything else is a
 * structural invariant the register encoder depends on:
 *   - gains inside the +/-12 dB the EQxG field can express;
 *   - centre codes inside the 2 bits EQxC has;
 *   - the bandwidth bit never set on a shelf (EQ1 bit 8 is the EQ path
 *     select, so a shelf carrying it would silently move the EQ onto the
 *     playback path);
 *   - the pre-cut equals the largest boost, and is 0 for a curve that only
 *     cuts;
 *   - Off is flat and hands the shelves to the tone sliders.
 */

#include "eq.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;
static int check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    if (!cond) {
        g_fail = 1;
    }
    return cond;
}

/* Independently typed expectation: name + the five band gains, in preset
 * order. Centre codes are asserted structurally (and, for the two presets
 * that move one, by name below). */
static const struct {
    const char *name;
    int         gain[EQ_BANDS];
} WANT[EQ_PRESET_COUNT] = {
    { "Off",            {  0,  0,  0,  0,  0 } },
    { "Acoustic",       {  4,  2,  0,  2,  3 } },
    { "Bass Booster",   {  6,  3,  0,  0,  0 } },
    { "Bass Reducer",   { -6, -3,  0,  0,  0 } },
    { "Classical",      {  4,  2, -2, -2,  3 } },
    { "Dance",          {  5,  2,  0,  3,  4 } },
    { "Electronic",     {  5,  1, -2,  2,  4 } },
    { "Hip-Hop",        {  6,  3,  0,  1,  3 } },
    { "Jazz",           {  3,  0, -1,  2,  3 } },
    { "Loudness",       {  7,  2, -2,  0,  5 } },
    { "Pop",            { -1,  2,  4,  2, -1 } },
    { "R&B",            {  5,  3, -1,  1,  3 } },
    { "Rock",           {  5,  2, -1,  2,  4 } },
    { "Small Speakers", {  5,  2,  0,  2,  3 } },
    { "Spoken Word",    { -3,  1,  3,  4,  1 } },
    { "Treble Booster", {  0,  0,  0,  2,  6 } },
    { "Treble Reducer", {  0,  0,  0, -2, -6 } },
    { "Vocal Booster",  { -2,  1,  4,  4,  0 } },
};

int main(void)
{
    eq_curve_t c;

    /* --- Test 1: the table is the one the UI and the HAL were built for --- */
    check("preset-count-18", EQ_PRESET_COUNT == 18);
    check("off-is-zero",     EQ_OFF == 0);
    check("bands-5",         EQ_BANDS == 5);

    /* --- Test 2: names — present, unique, and the documented ones --- */
    {
        int names_ok = 1, unique = 1;
        for (int i = 0; i < EQ_PRESET_COUNT; i++) {
            const char *n = eq_preset_name(i);
            if (n == 0 || n[0] == '\0' || strcmp(n, WANT[i].name) != 0) {
                names_ok = 0;
            }
            for (int j = 0; j < i; j++) {
                if (strcmp(eq_preset_name(i), eq_preset_name(j)) == 0) {
                    unique = 0;
                }
            }
        }
        check("names-match", names_ok);
        check("names-unique", unique);
        /* A preset byte from a newer build names Off, the same way it decodes
         * as Off in kernel/config.c. */
        check("name-out-of-range-off",
              strcmp(eq_preset_name(EQ_PRESET_COUNT), "Off") == 0 &&
              strcmp(eq_preset_name(-1), "Off") == 0);
    }

    /* --- Test 3: every preset's gains, against the independent fixture --- */
    {
        int gains_ok = 1;
        for (int i = 0; i < EQ_PRESET_COUNT; i++) {
            eq_preset_curve(i, &c);
            for (int b = 0; b < EQ_BANDS; b++) {
                if (c.gain_db[b] != WANT[i].gain[b]) {
                    printf("  preset %d (%s) band %d: got %d want %d\n",
                           i, WANT[i].name, b, c.gain_db[b], WANT[i].gain[b]);
                    gains_ok = 0;
                }
            }
        }
        check("preset-gains", gains_ok);
    }

    /* --- Test 4: structural limits the register encoder relies on --- */
    {
        int in_range = 1, cutoffs_ok = 1, shelves_wide = 1;
        for (int i = 0; i < EQ_PRESET_COUNT; i++) {
            eq_preset_curve(i, &c);
            for (int b = 0; b < EQ_BANDS; b++) {
                if (c.gain_db[b] < -12 || c.gain_db[b] > 12) in_range = 0;
                if (c.cutoff[b] > 3) cutoffs_ok = 0;
            }
            /* EQ1 bit 8 is EQ3DMODE and EQ5 bit 8 is unused: neither shelf
             * may ever carry a bandwidth bit. */
            if (c.narrow[0] || c.narrow[EQ_BANDS - 1]) shelves_wide = 0;
            for (int b = 0; b < EQ_BANDS; b++) {
                if (c.narrow[b] > 1) shelves_wide = 0;
            }
        }
        check("gains-within-+-12dB", in_range);
        check("cutoff-codes-2-bit", cutoffs_ok);
        check("bandwidth-bit-off-the-shelves", shelves_wide);
    }

    /* --- Test 5: Off is flat, on the tone control's corners --- */
    eq_preset_curve(EQ_OFF, &c);
    check("off-flat",
          c.gain_db[0] == 0 && c.gain_db[1] == 0 && c.gain_db[2] == 0 &&
          c.gain_db[3] == 0 && c.gain_db[4] == 0);
    /* The mid codes stay 00 so the flat words are the ones the tone control has
     * always emitted; the shelves sit on 105 Hz / 6.9 kHz (code 01). */
    check("off-cutoffs-1-0-0-0-1",
          c.cutoff[0] == 1 && c.cutoff[1] == 0 && c.cutoff[2] == 0 &&
          c.cutoff[3] == 0 && c.cutoff[4] == 1);

    /* --- Test 6: the two presets that move a centre --- */
    eq_preset_curve(13, &c);                       /* Small Speakers */
    check("small-speakers-shelf-175hz", c.cutoff[0] == 3);
    eq_preset_curve(14, &c);                       /* Spoken Word */
    check("spoken-word-band3-2k4", c.cutoff[3] == 1);
    eq_preset_curve(12, &c);                       /* Rock: the default set */
    check("default-centres",
          c.cutoff[0] == 1 && c.cutoff[1] == 1 && c.cutoff[2] == 2 &&
          c.cutoff[3] == 2 && c.cutoff[4] == 1);

    /* --- Test 7: the pre-cut is the largest boost, 0 for a pure cut --- */
    {
        int precut_ok = 1;
        for (int i = 0; i < EQ_PRESET_COUNT; i++) {
            eq_preset_curve(i, &c);
            int peak = 0;
            for (int b = 0; b < EQ_BANDS; b++) {
                if (WANT[i].gain[b] > peak) peak = WANT[i].gain[b];
            }
            if (eq_precut_db(&c) != peak) {
                printf("  preset %d (%s): precut %d want %d\n",
                       i, WANT[i].name, eq_precut_db(&c), peak);
                precut_ok = 0;
            }
        }
        check("precut-is-max-boost", precut_ok);
    }
    {
        /* The three curves that only cut (or do nothing) need no headroom. */
        int zero_ok = 1;
        const int cuts[3] = { EQ_OFF, 3 /*Bass Reducer*/, 16 /*Treble Reducer*/ };
        for (int i = 0; i < 3; i++) {
            eq_preset_curve(cuts[i], &c);
            if (eq_precut_db(&c) != 0) zero_ok = 0;
        }
        check("precut-zero-for-cuts", zero_ok);
    }
    /* The pre-cut has to fit in the DAC volume field: 12 dB is 24 of its 255
     * half-dB steps, so the attenuated code can never underflow. */
    {
        int cap_ok = 1;
        for (int i = 0; i < EQ_PRESET_COUNT; i++) {
            eq_preset_curve(i, &c);
            if (eq_precut_db(&c) > 12) cap_ok = 0;
        }
        check("precut-within-dac-range", cap_ok);
    }

    /* --- Test 8: eq_effective_curve — who owns the shelves --- */
    eq_effective_curve(EQ_OFF, +3, -2, &c);
    check("effective-off-uses-tone",
          c.gain_db[0] == 3 && c.gain_db[1] == 0 && c.gain_db[2] == 0 &&
          c.gain_db[3] == 0 && c.gain_db[4] == -2);
    check("effective-off-keeps-flat-corners",
          c.cutoff[0] == 1 && c.cutoff[1] == 0 && c.cutoff[4] == 1);
    eq_effective_curve(EQ_OFF, +99, -99, &c);
    check("effective-off-clamps-tone",
          c.gain_db[0] == 12 && c.gain_db[4] == -12);

    eq_effective_curve(12, +3, -2, &c);            /* Rock */
    {
        eq_curve_t want;
        eq_preset_curve(12, &want);
        check("effective-preset-ignores-tone",
              memcmp(&c, &want, sizeof c) == 0);
    }
    /* A preset index this build does not know behaves as Off, tone and all —
     * so a record from a newer build cannot leave the shelves stuck. */
    eq_effective_curve(EQ_PRESET_COUNT, +5, +1, &c);
    check("effective-out-of-range-is-off",
          c.gain_db[0] == 5 && c.gain_db[4] == 1 &&
          c.gain_db[1] == 0 && c.gain_db[2] == 0 && c.gain_db[3] == 0);

    printf("eq_test: %s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
