/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/volume.c — WM8758B output-volume HAL.
 *
 * Maps a 0..100% UI level onto the codec's HEADPHONE amp gain field
 * (LOUT1VOL/ROUT1VOL, regs 0x34/0x35), reusing the same I2C control-word
 * grammar as wm8758.c. The DAC digital volume is left at full scale by
 * wm8758_init(), so the analog amp gain is the single audible knob here.
 *
 * REGISTER CHOICE (why OUT1 amp gain, not DAC digital volume):
 *   - LOUT1VOL/ROUT1VOL are a 6-bit field (OUTVOL_GAIN_MASK = 0x3F),
 *     ~-57..+6 dB in 1 dB steps, with 0x39 = 0 dB and 0x3F = +6 dB
 *     (core/docs/hw/05-audio.md, "Volume control"; the 0x39=0 dB point is
 *     also the codec reset default used by wm8758_init()).
 *   - The MUTE bit (OUTVOL_MUTE = 0x40) is separate from the gain field
 *     (0x00 is -57 dB, NOT silence), so 0% asserts MUTE explicitly.
 *   - The ZC bit (OUTVOL_ZC = 0x80) makes gain changes wait for a signal
 *     zero-crossing (no zipper noise); we set it on every write.
 *   - The VU bit (OUTVOL_VU = 0x100) on the RIGHT write latches both
 *     channels together — the classic Wolfson "write L, then write R with
 *     update" pattern.
 * The DAC digital volume (LDACVOL/RDACVOL, 0x0B/0x0C) is a global 8-bit
 * attenuator shared by every output path; using it for the user knob
 * would also scale the line-out and is left full-scale.
 *
 * CLEANROOM NOTE: register numbers, bit positions and the 0x39=0 dB
 * headphone-gain fact are transcribed from core/docs/hw/05-audio.md
 * (which resolves the Wolfson/Rockbox mnemonics to concrete numbers).
 * No Rockbox code body is copied — the percent curve and write sequence
 * below are this project's own.
 */

#include "volume.h"
#include "wm8758.h"
#include "i2c.h"

/*
 * WM8758 control word (same framing as wm8758.c's private helper): 7-bit
 * register in bits 15:9, 9-bit data in 8:0 -> two I2C payload bytes,
 * byte0 = (reg<<1) | data bit 8, byte1 = data low 8 bits. wm8758.c keeps
 * its writer file-static, and this driver must not edit that file, so we
 * mirror the exact grammar here.
 */
static void codec_write(uint8_t reg, uint16_t data)
{
    uint8_t frame[2];
    frame[0] = (uint8_t)((reg << 1) | ((data >> 8) & 0x1));
    frame[1] = (uint8_t)(data & 0xFF);
    (void)i2c_send(WM8758_I2C_ADDR, frame, 2);
}

/*
 * Usable amp-gain range for the 1..100% span.
 *   VOL_GAIN_0DB  = 0x39  -> 0 dB at 100% (unity; NOT the +6 dB top, so a
 *                            full-scale sample cannot clip the amp).
 *   VOL_GAIN_FLOOR= 0x06  -> ~-51 dB at 1% (quiet but audible; the very
 *                            bottom of the field is effectively inaudible,
 *                            so we do not map the UI onto it). 0% is a
 *                            hard mute via OUTVOL_MUTE instead.
 */
#define VOL_GAIN_0DB    0x39
#define VOL_GAIN_FLOOR  0x06

/* Default output level at bring-up: moderate, headroom below unity. */
#define VOL_DEFAULT_PCT 70

static int g_percent = VOL_DEFAULT_PCT;

uint16_t hal_volume_out1_word(int percent)
{
    if (percent <= 0) {
        /* Hard mute: the gain field is don't-care under MUTE; keep ZC so
         * the un-mute later still lands on a zero-crossing. */
        return (uint16_t)(OUTVOL_MUTE | OUTVOL_ZC);
    }
    if (percent > 100) {
        percent = 100;
    }

    /* Linear 1..100% -> [FLOOR .. 0 dB] gain codes. Rounded so 100 lands
     * exactly on VOL_GAIN_0DB and the map is monotonic non-decreasing. */
    int span = VOL_GAIN_0DB - VOL_GAIN_FLOOR;               /* 0x33 = 51 */
    int code = VOL_GAIN_FLOOR + (span * percent + 50) / 100;
    if (code > VOL_GAIN_0DB) {
        code = VOL_GAIN_0DB;                                /* belt + braces */
    }

    return (uint16_t)((code & OUTVOL_GAIN_MASK) | OUTVOL_ZC);
}

void hal_volume_set(int percent)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    g_percent = percent;

    uint16_t word = hal_volume_out1_word(percent);

    /* Left first, then right with the update bit so both latch together. */
    codec_write(WM_LOUT1VOL, word);
    codec_write(WM_ROUT1VOL, (uint16_t)(word | OUTVOL_VU));
}

void hal_volume_init(void)
{
    hal_volume_set(VOL_DEFAULT_PCT);
}

int hal_volume_get(void)
{
    return g_percent;
}
