/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/volume.h — output-volume control for the WM8758B codec.
 *
 * Thin policy layer on top of the codec control bus (i2c.c) and the
 * register/bit facts in wm8758.h. It drives the HEADPHONE amp gain
 * (LOUT1VOL/ROUT1VOL, regs 0x34/0x35) — the analog attenuator on the
 * OUT1 path — leaving the global DAC digital volume at full scale (as
 * wm8758_init() sets it) so this is a single, well-behaved knob.
 *
 * See core/docs/hw/05-audio.md, "Volume control". Does NOT touch the
 * playback/DMA path in audio.c; it only pushes two register writes per
 * change. Freestanding (no libc/libm), integer-only, and asm-free so it
 * host-compiles for the mock-bus mapping test.
 *
 * Ordering: the codec (i2c_init + wm8758_init) must already be up. A UI
 * calls hal_volume_init() once for a safe default, then hal_volume_set()
 * on user input.
 */
#ifndef CORE_HAL_HW_VOLUME_H
#define CORE_HAL_HW_VOLUME_H

#include <stdint.h>

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
 * Pure percent -> OUT1VOL data-word mapping (no side effects, no I2C).
 * Returns the 9-bit LOUT1VOL/ROUT1VOL data word WITHOUT the VU latch bit
 * (the caller ORs OUTVOL_VU onto the right-channel write). Exposed so the
 * host mapping test can assert monotonicity, clamping, and the 0%/100%
 * endpoints without touching MMIO. `percent` is clamped to 0..100.
 */
uint16_t hal_volume_out1_word(int percent);

#endif /* CORE_HAL_HW_VOLUME_H */
