/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/eq.c — the equaliser preset table (no hardware, no framebuffer).
 *
 * Seventeen named curves over the WM8758B's five bands, plus Off. Each curve
 * is five gains in dB and the centre/corner code each band sits at; the
 * register arithmetic is hal/hw/volume.c's (hal_eq_set) and the on-screen
 * names are ui/settings.c's. The host unit test compiles this exact source.
 *
 * WHERE THE NUMBERS COME FROM. They are ours: each curve was written against
 * the band centres the silicon actually offers (see below), not transcribed
 * from any other player's preset list. The shapes are the ordinary ones the
 * names imply — a bass booster boosts the low shelf and the band above it, a
 * vocal booster lifts the two bands the voice sits in and steps back either
 * side — and every one of them is capped at +7 dB so the pre-cut that pays
 * for it (eq_precut_db) never has to take more than 7 dB off the DAC.
 *
 * BAND CENTRES. The default set, used by every preset except the two marked
 * below, is band 0 (low shelf) 105 Hz, band 1 300 Hz, band 2 1.1 kHz, band 3
 * 3.2 kHz, band 4 (high shelf) 6.9 kHz — the two shelves on the same corners
 * the Bass/Treble tone control uses, and the three peaks spread roughly a
 * decade apart. Small Speakers moves its shelf up to 175 Hz (there is no
 * point boosting what a small driver cannot make); Spoken Word moves band 3
 * down to 2.4 kHz, onto consonants rather than sibilance.
 *
 * Freestanding: integer-only, no libc/libm/malloc.
 */

#include "eq.h"

/* Band centre/corner codes (EQxC), per band. See core/docs/hw/05-audio.md. */
#define C_B0_105HZ   1u   /* low shelf corner: 80 / 105 / 135 / 175 Hz       */
#define C_B0_175HZ   3u
#define C_B1_300HZ   1u   /* peak 1: 230 / 300 / 385 / 500 Hz                */
#define C_B2_1K1     2u   /* peak 2: 650 / 850 / 1100 / 1400 Hz              */
#define C_B3_2K4     1u   /* peak 3: 1800 / 2400 / 3200 / 4100 Hz            */
#define C_B3_3K2     2u
#define C_B4_6K9     1u   /* high shelf corner: 5.3 / 6.9 / 9 / 11.7 kHz     */

/* The centres the tone control writes at 0 dB: the shelves on their corners,
 * the three peaks on code 00. Keeping the flat curve's mid codes at 00 is
 * what makes "EQ Off, tone flat" emit byte-for-byte the register words this
 * firmware emitted before presets existed — inaudible either way at 0 dB, and
 * a regression pin the mock-bus test can hold onto. */
#define FLAT_CUTOFFS { C_B0_105HZ, 0u, 0u, 0u, C_B4_6K9 }

/* The default centres every preset uses unless its row says otherwise. */
#define STD_CUTOFFS  { C_B0_105HZ, C_B1_300HZ, C_B2_1K1, C_B3_3K2, C_B4_6K9 }

typedef struct {
    const char *name;
    int8_t      gain_db[EQ_BANDS];
    uint8_t     cutoff[EQ_BANDS];
} eq_preset_t;

/*
 * The table. Index 0 is Off and carries the flat curve; a preset's gains are
 * (low shelf, 300 Hz, 1.1 kHz, 3.2 kHz, high shelf) unless its cutoff row
 * says otherwise.
 *
 * No preset sets EQxBW: every peak here is a wide one, which is what a
 * broad-stroke tone preset wants. The bit is still modelled (eq_curve_t.narrow
 * -> hal_eq_set) because it is part of the register, and a narrow band is a
 * curve away rather than a driver change away.
 */
static const eq_preset_t PRESETS[EQ_PRESET_COUNT] = {
    { "Off",             {  0,  0,  0,  0,  0 }, FLAT_CUTOFFS },
    { "Acoustic",        {  4,  2,  0,  2,  3 }, STD_CUTOFFS  },
    { "Bass Booster",    {  6,  3,  0,  0,  0 }, STD_CUTOFFS  },
    { "Bass Reducer",    { -6, -3,  0,  0,  0 }, STD_CUTOFFS  },
    { "Classical",       {  4,  2, -2, -2,  3 }, STD_CUTOFFS  },
    { "Dance",           {  5,  2,  0,  3,  4 }, STD_CUTOFFS  },
    { "Electronic",      {  5,  1, -2,  2,  4 }, STD_CUTOFFS  },
    { "Hip-Hop",         {  6,  3,  0,  1,  3 }, STD_CUTOFFS  },
    { "Jazz",            {  3,  0, -1,  2,  3 }, STD_CUTOFFS  },
    { "Loudness",        {  7,  2, -2,  0,  5 }, STD_CUTOFFS  },
    { "Pop",             { -1,  2,  4,  2, -1 }, STD_CUTOFFS  },
    { "R&B",             {  5,  3, -1,  1,  3 }, STD_CUTOFFS  },
    { "Rock",            {  5,  2, -1,  2,  4 }, STD_CUTOFFS  },
    /* Shelf at 175 Hz: a small driver has nothing below it to lift. */
    { "Small Speakers",  {  5,  2,  0,  2,  3 },
      { C_B0_175HZ, C_B1_300HZ, C_B2_1K1, C_B3_3K2, C_B4_6K9 } },
    /* Band 3 at 2.4 kHz: consonants, not sibilance. */
    { "Spoken Word",     { -3,  1,  3,  4,  1 },
      { C_B0_105HZ, C_B1_300HZ, C_B2_1K1, C_B3_2K4, C_B4_6K9 } },
    { "Treble Booster",  {  0,  0,  0,  2,  6 }, STD_CUTOFFS  },
    { "Treble Reducer",  {  0,  0,  0, -2, -6 }, STD_CUTOFFS  },
    { "Vocal Booster",   { -2,  1,  4,  4,  0 }, STD_CUTOFFS  },
};

/* Clamp `v` into [lo, hi] (no libc). */
static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* The table row for `preset`, with anything out of range reading as Off — the
 * same rule config.c applies to a preset byte from a newer build. */
static const eq_preset_t *row(int preset)
{
    if (preset < 0 || preset >= EQ_PRESET_COUNT) {
        return &PRESETS[EQ_OFF];
    }
    return &PRESETS[preset];
}

const char *eq_preset_name(int preset)
{
    return row(preset)->name;
}

void eq_preset_curve(int preset, eq_curve_t *out)
{
    const eq_preset_t *p = row(preset);
    for (int b = 0; b < EQ_BANDS; b++) {
        out->gain_db[b] = p->gain_db[b];
        out->cutoff[b]  = p->cutoff[b];
        out->narrow[b]  = 0;
    }
}

void eq_effective_curve(int preset, int bass_db, int treble_db, eq_curve_t *out)
{
    eq_preset_curve(preset, out);
    if (preset > EQ_OFF && preset < EQ_PRESET_COUNT) {
        return;                       /* the preset owns both shelves */
    }
    /* Off (or an index that reads as Off): the user's own tone control, on
     * the flat curve's corners. */
    out->gain_db[0]            = (int8_t)clampi(bass_db,   -12, 12);
    out->gain_db[EQ_BANDS - 1] = (int8_t)clampi(treble_db, -12, 12);
}

int eq_precut_db(const eq_curve_t *c)
{
    int peak = 0;
    for (int b = 0; b < EQ_BANDS; b++) {
        if (c->gain_db[b] > peak) {
            peak = c->gain_db[b];
        }
    }
    return peak;
}
