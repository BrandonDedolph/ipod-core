/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/eq.h — the equaliser preset table: Off plus 17 named curves.
 *
 * A pure, host-built lookup table. It knows nothing about the codec: a curve
 * is five band gains in dB plus, per band, which centre/cutoff the band sits
 * at and whether a peaking band is narrow. Turning that into WM8758B register
 * words is hal/hw/volume.c's job (hal_eq_set), and turning it into a row of
 * text is ui/settings.c's. Splitting it this way is what lets the preset
 * numbers be unit-tested with plain cc and keeps the ui -> hal direction
 * one-way: settings.c includes this header, hal/ never does.
 *
 * Band order is the silicon's: band 0 is the LOW SHELF (what the Bass slider
 * drives), bands 1..3 are parametric peaks, band 4 is the HIGH SHELF (Treble).
 * `cutoff` is the 2-bit EQxC code for that band, so its meaning depends on
 * which band it belongs to (see core/docs/hw/05-audio.md, "WM8758 tone
 * controls"); `narrow` is the EQxBW bit and is only meaningful on the three
 * peaking bands — the shelves have no bandwidth control, and EQ1's bit 8 is
 * the EQ path select, so a shelf must never carry it.
 *
 * The gains are this project's own tuning. Nothing here is transcribed from
 * another firmware or from any player's preset list.
 *
 * Freestanding: integer-only, no libc/libm/malloc.
 */

#ifndef CORE_UI_EQ_H
#define CORE_UI_EQ_H

#include <stdint.h>

#define EQ_BANDS        5
#define EQ_OFF          0     /* preset 0: no curve — the tone sliders rule  */
#define EQ_PRESET_COUNT 18    /* Off + 17 named presets                      */

/* One resolved curve. Every array is indexed by band 0..4 as described above. */
typedef struct {
    int8_t  gain_db[EQ_BANDS];  /* -12..+12 dB, 0 = flat                     */
    uint8_t cutoff[EQ_BANDS];   /* 0..3 — the band's EQxC centre/corner code */
    uint8_t narrow[EQ_BANDS];   /* 0/1 — EQxBW; always 0 on bands 0 and 4    */
} eq_curve_t;

/* The display name of preset `preset` ("Off", "Acoustic", …). An index
 * outside 0..EQ_PRESET_COUNT-1 names "Off", which is also the curve
 * eq_preset_curve() hands back for it. */
const char *eq_preset_name(int preset);

/* Write preset `preset`'s curve into *out. Preset 0 (and any index out of
 * range) is the FLAT curve: all gains 0, with the two shelves parked on the
 * corners the Bass/Treble tone control uses so the caller's register words
 * are the tone control's own. */
void eq_preset_curve(int preset, eq_curve_t *out);

/*
 * The curve actually in force: preset `preset`'s if one is selected, else the
 * flat curve with the user's own tone dialled into the two shelves (both
 * clamped to +/-12 dB). This is the single place the "a preset owns the
 * shelves" rule lives — it is why the Bass and Treble rows lock while a
 * preset is on, rather than fighting it.
 */
void eq_effective_curve(int preset, int bass_db, int treble_db,
                        eq_curve_t *out);

/*
 * How much digital headroom a curve needs, in dB: its largest positive band
 * gain, or 0 if it only cuts. The caller pre-attenuates the DAC by this much
 * before boosting, so a full-scale track cannot clip the digital path.
 */
int eq_precut_db(const eq_curve_t *c);

#endif /* CORE_UI_EQ_H */
