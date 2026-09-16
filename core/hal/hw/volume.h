/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/volume.h — output-volume control for the WM8758B codec.
 *
 * Thin policy layer on top of the codec control bus (i2c.c) and the
 * register/bit facts in wm8758.h. It drives the HEADPHONE amp gain
 * (LOUT1VOL/ROUT1VOL, regs 0x34/0x35) — the analog attenuator on the
 * OUT1 path — as the user's volume knob, and owns the codec's 5-band EQ.
 * The global DAC digital volume stays at 0 dB except for the headroom
 * hal_eq_set() takes off ahead of a boosting curve.
 *
 * See core/docs/hw/05-audio.md, "Volume control" and "WM8758 tone
 * controls". Does NOT touch the playback/DMA path in audio.c; it only
 * pushes register writes. Freestanding (no libc/libm), integer-only, and asm-free so it
 * host-compiles for the mock-bus mapping test.
 *
 * Ordering: the codec (i2c_init + wm8758_init) must already be up. A UI
 * calls hal_volume_init() once for a safe default, then hal_volume_set()
 * on user input.
 */
#ifndef CORE_HAL_HW_VOLUME_H
#define CORE_HAL_HW_VOLUME_H

#include <stdint.h>

#include "wm8758.h"                   /* EQ_BAND_COUNT + the register facts */

/* Set a safe default output level (~70%) and latch it into the codec. */
void hal_volume_init(void);

/*
 * Set output volume as a percentage. `percent` is clamped to 0..100.
 * 0 mutes the headphone amp; 100 is 0 dB (unity, never the +6 dB top, so
 * a full-scale track cannot clip the amp). Writes left then right with
 * the OUT1 volume-update (VU) bit on the right write so both channels
 * change together at a zero-crossing.
 */
void hal_volume_set(int percent);

/* Last percent handed to hal_volume_set()/hal_volume_init() (0..100). */
int hal_volume_get(void);

/*
 * Set stereo balance, -100 (full left) .. +100 (full right), 0 = center.
 * Pans by attenuating the far channel's OUT1 amp gain (muting it at the
 * extreme); at 0 both channels are identical to hal_volume_set() alone.
 * Clamped to [-100, 100]. Re-latches immediately at the current volume.
 */
void hal_balance_set(int balance);
int  hal_balance_get(void);

/*
 * Set the whole 5-band EQ curve. Per band (0 = low shelf, 1..3 = the peaking
 * bands, 4 = high shelf):
 *   gain_db[]  -12..+12 dB, clamped here; 0 = flat.
 *   cutoff[]   the 2-bit EQxC centre/corner code for THAT band (05-audio.md).
 *   narrow[]   0/1 EQxBW, honoured on bands 1..3 only — EQ1's bit 8 is the
 *              EQ path select and EQ5 has none, so a shelf's flag is dropped.
 *
 * The DAC digital volume is pre-attenuated by the curve's largest boost, so a
 * full-scale track cannot clip the digital path; a curve that only cuts leaves
 * it at 0 dB. The attenuation is never smaller than the boost that is live,
 * including mid-call: going UP the DAC is cut before the gains rise, going
 * DOWN the gains come off before the DAC goes back up. An all-zero curve
 * leaves the EQ on the codec's inert ADC path, which is bit-identical to no EQ
 * at all.
 *
 * This is the ONLY way tone reaches the codec: the Bass/Treble sliders are the
 * flat curve with the two shelves moved (ui/eq.c eq_effective_curve), not a
 * second entry point here. See core/docs/hw/05-audio.md, "tone controls".
 *
 * The curve is cached, so hal_codec_restore() can put it back after the
 * per-track codec reset. Requires the codec to be up (wm8758_init).
 */
void hal_eq_set(const int8_t gain_db[EQ_BAND_COUNT],
                const uint8_t cutoff[EQ_BAND_COUNT],
                const uint8_t narrow[EQ_BAND_COUNT]);

/*
 * Re-apply everything this driver owns — volume, balance and the EQ curve —
 * from its cached state, in one shot.
 *
 * WHY: hal_audio_init() runs once per TRACK, and its first act is a full
 * codec WM_RESET, which returns every register here to a datasheet default
 * (0 dB headphone gain, full-scale DAC volume, flat EQ on the inert ADC
 * path). Volume used to be papered over by an external re-apply from the
 * player; the EQ had nothing at all, so the user's tone settings were
 * silently wiped on every track change and every auto-advance. Worse, a track boundary could hand the
 * listener 0 dB when they had dialled the volume down -- a hearing-safety
 * problem, not a preferences one.
 *
 * wm8758_init() now calls this itself (via wm8758_set_restore), so the codec
 * can never come up at a level the user did not choose. An external re-apply
 * after hal_audio_init is therefore redundant but harmless -- it writes the
 * same values.
 */
void hal_codec_restore(void);

/*
 * Pure percent -> OUT1VOL data-word mapping (no side effects, no I2C).
 * Returns the 9-bit LOUT1VOL/ROUT1VOL data word WITHOUT the VU latch bit
 * (the caller ORs OUTVOL_VU onto the right-channel write). Exposed so the
 * host mapping test can assert monotonicity, clamping, and the 0%/100%
 * endpoints without touching MMIO. `percent` is clamped to 0..100.
 */
uint16_t hal_volume_out1_word(int percent);

#endif /* CORE_HAL_HW_VOLUME_H */
