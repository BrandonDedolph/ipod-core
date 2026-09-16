/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/volume.c — WM8758B output-volume HAL.
 *
 * Maps a 0..100% UI level onto the codec's HEADPHONE amp gain field
 * (LOUT1VOL/ROUT1VOL, regs 0x34/0x35), reusing the same I2C control-word
 * grammar as wm8758.c, and owns the 5-band EQ (EQ1..EQ5) that the Bass /
 * Treble sliders and the EQ presets drive. The DAC digital volume is NOT the
 * user's knob: it stays at 0 dB except for the headroom hal_eq_set takes off
 * ahead of a boosting curve.
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
 * attenuator shared by every output path, so it is no use as the user knob
 * — it would scale the line-out with it. It is used for exactly one thing
 * here: the pre-cut that buys an EQ boost its headroom (hal_eq_set).
 *
 * CLEANROOM NOTE: register numbers, bit positions and the 0x39=0 dB
 * headphone-gain fact are transcribed from core/docs/hw/05-audio.md
 * (which resolves the Wolfson/Rockbox mnemonics to concrete numbers).
 * No Rockbox code body is copied — the percent curve, the EQ write order
 * and the pre-cut policy below are this project's own.
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

/*
 * Cached codec state. This driver is the single owner of everything the user
 * can change on the codec, and it must be able to reconstruct all of it from
 * RAM: hal_audio_init() issues a full WM_RESET once per track, so any setting
 * that lives only in a codec register is gone at every track boundary. See
 * hal_codec_restore() at the bottom of this file.
 */
static int g_percent = VOL_DEFAULT_PCT;
static int g_balance = 0;                /* -100 (full left) .. +100 (full right) */

/*
 * The EQ curve currently in force: five band gains in dB plus, per band, the
 * EQxC centre code and the EQxBW bandwidth bit. Initialised to the FLAT
 * curve the codec comes up on — gains 0, the two shelves on their corners
 * (105 Hz / 6.9 kHz), the three peaks on code 00 — so a restore that happens
 * before anything has been set writes exactly the words wm8758_init's own
 * sequence would have left.
 */
static int8_t  g_eq_gain[EQ_BAND_COUNT];
static uint8_t g_eq_cutoff[EQ_BAND_COUNT] = { 1, 0, 0, 0, 1 };
static uint8_t g_eq_narrow[EQ_BAND_COUNT];

/*
 * The pre-cut (in dB) the codec's LDACVOL/RDACVOL are believed to hold right
 * now — NOT what the cached curve implies. eq_latch() orders its writes by
 * the sign of (new pre-cut - this), and a codec reset puts the registers back
 * to 0 dB behind our back, which is why hal_codec_restore() zeroes it. 0 at
 * bring-up for the same reason: the codec comes up at full scale.
 */
static int g_dac_precut;

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

/* Build an OUT1VOL data word (ZC set, no VU) for an absolute gain code, muting
 * if the code has been panned below the audible floor. */
static uint16_t out1_word_for_code(int code)
{
    if (code < VOL_GAIN_FLOOR) {
        return (uint16_t)(OUTVOL_MUTE | OUTVOL_ZC);
    }
    if (code > VOL_GAIN_0DB) {
        code = VOL_GAIN_0DB;
    }
    return (uint16_t)((code & OUTVOL_GAIN_MASK) | OUTVOL_ZC);
}

/*
 * Latch the current (percent, balance) into LOUT1VOL/ROUT1VOL. Balance pans by
 * attenuating one channel's amp gain: at 0 both channels get the identical
 * center word (byte-for-byte the old behaviour), so nothing changes when
 * balance is untouched; toward an end, the FAR channel's gain code is walked
 * down by up to the full span and hard-muted at the extreme.
 */
static void volume_latch(void)
{
    uint16_t lword, rword;
    if (g_percent <= 0) {
        lword = rword = (uint16_t)(OUTVOL_MUTE | OUTVOL_ZC);
    } else if (g_balance == 0) {
        lword = rword = hal_volume_out1_word(g_percent);   /* exact center path */
    } else {
        int span = VOL_GAIN_0DB - VOL_GAIN_FLOOR;
        int code = VOL_GAIN_FLOOR + (span * g_percent + 50) / 100;
        if (code > VOL_GAIN_0DB) {
            code = VOL_GAIN_0DB;
        }
        int mag   = g_balance < 0 ? -g_balance : g_balance;
        int atten = span * mag / 100;              /* full pan -> down to floor */
        int lcode = code, rcode = code;
        if (g_balance > 0) {
            lcode -= atten;                        /* pan right: cut left  */
        } else {
            rcode -= atten;                        /* pan left:  cut right */
        }
        lword = (g_balance > 0 && mag >= 100) ? (uint16_t)(OUTVOL_MUTE | OUTVOL_ZC)
                                              : out1_word_for_code(lcode);
        rword = (g_balance < 0 && mag >= 100) ? (uint16_t)(OUTVOL_MUTE | OUTVOL_ZC)
                                              : out1_word_for_code(rcode);
    }

    /* Left first, then right with the update bit so both latch together. */
    codec_write(WM_LOUT1VOL, lword);
    codec_write(WM_ROUT1VOL, (uint16_t)(rword | OUTVOL_VU));
}

void hal_volume_set(int percent)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    g_percent = percent;
    volume_latch();
}

void hal_balance_set(int balance)
{
    if (balance < -100) {
        balance = -100;
    } else if (balance > 100) {
        balance = 100;
    }
    g_balance = balance;
    volume_latch();
}

int hal_balance_get(void)
{
    return g_balance;
}

/* EQxG gain code for a signed dB (05-audio.md: code = 12 - dB), clamped to the
 * ±12 dB field. */
static uint16_t eq_gain_code(int db)
{
    if (db >  12) db =  12;
    if (db < -12) db = -12;
    return (uint16_t)((12 - db) & EQ_GAIN_MASK);
}

/* Clamp a signed dB into the EQxG field's range. */
static int eq_clamp_db(int db)
{
    if (db >  12) return  12;
    if (db < -12) return -12;
    return db;
}

/* Write the DAC digital volume for a pre-cut of `db`, left then right with
 * DACVU so both channels latch together. 0.5 dB per step from 0xFF = 0 dB. */
static void dacvol_write(int db)
{
    uint16_t w = (uint16_t)((DACVOL_0DB - 2 * db) & DACVOL_MASK);
    codec_write(WM_LDACVOL, w);
    codec_write(WM_RDACVOL, (uint16_t)(w | DACVOL_DACVU));
}

/* Write the cached curve into EQ1..EQ5, low shelf first. */
static void eq_bands_write(void)
{
    /*
     * Only route the EQ onto the DAC when the curve actually does something;
     * a flat curve stays on the (silent) ADC path, so "EQ Off with the tone
     * flat" is bit-identical to no EQ at all — the state the device ships in.
     */
    int flat = 1;
    for (int b = 0; b < EQ_BAND_COUNT; b++) {
        if (g_eq_gain[b] != 0) {
            flat = 0;
        }
    }

    static const uint8_t EQ_REG[EQ_BAND_COUNT] = {
        WM_EQ1, WM_EQ2, WM_EQ3, WM_EQ4, WM_EQ5
    };
    for (int b = 0; b < EQ_BAND_COUNT; b++) {
        uint16_t w = (uint16_t)(((g_eq_cutoff[b] << EQ_CUTOFF_SHIFT) &
                                 EQ_CUTOFF_MASK) |
                                eq_gain_code(g_eq_gain[b]));
        if (b == 0) {
            /* EQ1 bit 8 is the path select, not a bandwidth bit. */
            w |= flat ? 0u : (uint16_t)EQ_DAC_MODE;
        } else if (b < EQ_BAND_COUNT - 1 && g_eq_narrow[b]) {
            w |= EQ_BW_NARROW;              /* peaking bands only */
        }
        codec_write(EQ_REG[b], w);
    }
}

/*
 * Push the cached curve into the codec: the DACVOL pre-cut pair and EQ1..EQ5,
 * in whichever order keeps the boost covered throughout.
 *
 * WRITE ORDER, and why. A boosting band is digital gain ahead of the DAC, so
 * a full-scale sample through, say, Bass Booster's +6 dB would clip. Every
 * curve is therefore played with the DAC digital volume set to
 * 0 dB - (largest boost): transparent for a curve that only cuts, and costing
 * exactly the boost otherwise.
 *
 * The rule is that the attenuation must never be smaller than the boost that
 * is live, at any instant, INCLUDING the handful of I2C transactions in the
 * middle of this function. So the order follows the sign of the change:
 *
 *   MORE attenuation needed (new pre-cut > what the codec currently holds):
 *       cut the DAC first, then raise the gains.
 *   LESS attenuation needed (new pre-cut < it): take the gains off first,
 *       then let the DAC back up.
 *   Equal: nothing to sequence — no intermediate state exceeds the headroom
 *       already in place — so it takes the first branch for determinism.
 *
 * g_dac_precut is what the codec is believed to hold, not what the cached
 * curve implies: hal_codec_restore() resets it to 0 because the per-track
 * wm8758_init() has just written 0xFF (0 dB) over whatever was there, and a
 * replay must therefore re-cut before it re-boosts.
 *
 * None of these registers has a zero-cross latch (unlike OUT1VOL), so a large
 * step can be audible; the DACVU bit on the right DAC write at least latches
 * both channels together.
 */
static void eq_latch(void)
{
    int precut = 0;
    for (int b = 0; b < EQ_BAND_COUNT; b++) {
        if (g_eq_gain[b] > precut) {
            precut = g_eq_gain[b];
        }
    }

    if (precut >= g_dac_precut) {
        dacvol_write(precut);               /* attenuate, then boost */
        eq_bands_write();
    } else {
        eq_bands_write();                   /* unboost, then restore level */
        dacvol_write(precut);
    }
    g_dac_precut = precut;
}

void hal_eq_set(const int8_t gain_db[EQ_BAND_COUNT],
                const uint8_t cutoff[EQ_BAND_COUNT],
                const uint8_t narrow[EQ_BAND_COUNT])
{
    for (int b = 0; b < EQ_BAND_COUNT; b++) {
        g_eq_gain[b]   = (int8_t)eq_clamp_db(gain_db[b]);
        g_eq_cutoff[b] = (uint8_t)(cutoff[b] & 0x3u);
        /* The shelves have no bandwidth control and EQ1's bit 8 is the path
         * select: refuse it here rather than trusting every caller. */
        g_eq_narrow[b] = (b > 0 && b < EQ_BAND_COUNT - 1 && narrow[b]) ? 1u : 0u;
    }
    eq_latch();
}

void hal_volume_init(void)
{
    hal_volume_set(VOL_DEFAULT_PCT);
}

int hal_volume_get(void)
{
    return g_percent;
}

/*
 * Push the whole cached state back into a freshly reset codec. Called from
 * wm8758_init() through the wm8758_set_restore() hook (registered by
 * hal/hw/audio.c), so it runs at the tail of every per-track bring-up.
 *
 * Order: output gains first, then the EQ curve (whose own DACVOL pre-cut
 * leads, see eq_latch). Both are ordinary latched writes — the OUT1 pair
 * carries the VU bit on the right write, the EQ bands are independent — so
 * there is no interaction to sequence beyond "after the rails are up", which
 * the caller guarantees by construction.
 *
 * Deliberately unconditional: writing the defaults back when nothing was
 * changed costs nine I2C transactions (~a millisecond) once per track and
 * removes an entire class of "was it dirty?" reasoning.
 */
void hal_codec_restore(void)
{
    volume_latch();          /* volume + balance -> OUT1VOL pair               */
    /* wm8758_init's init_seq_c has just put DACVOL back to full scale, so the
     * codec holds no pre-cut whatever the curve says. Belt and braces: with
     * the cached curve's pre-cut equal to the one eq_latch believes is held,
     * its tie-break already writes DACVOL first, so this line changes no
     * write order today; it keeps the driver's belief true if that tie-break
     * ever changes. */
    g_dac_precut = 0;
    eq_latch();              /* the cached curve -> DACVOL pair + EQ1..EQ5     */
}
