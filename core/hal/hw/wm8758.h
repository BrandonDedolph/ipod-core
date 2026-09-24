/*
 * core/hal/hw/wm8758.h — Wolfson WM8758B codec register + bit constants.
 *
 * The WM8758B is reached over the SoC I2C controller (see
 * core/hal/hw/i2c.h and core/docs/hw/09-i2c.md), NOT via MMIO — so this
 * header carries no memory addresses, only 7-bit register numbers and
 * 9-bit data bit values. The register map and bit fields below are the
 * WM8758B datasheet's own — register numbers are the datasheet's R0..R61
 * (R0 Software Reset, R1..R3 Power Management, R4 Audio Interface, R6/R7
 * Clock/Sample control, R10..R12 DAC, R36..R39 PLL, R18..R22 EQ, R49..R57
 * Output mixers/volumes, R61 Bias control), and the mnemonics are Wolfson's
 * (VMIDSEL, BIASEN, POBCTRL, …). That datasheet is the primary source; the
 * concrete init values are cross-referenced in core/docs/hw/05-audio.md,
 * "Numeric register reference".
 *
 * Register data is 9 bits: several bits (the volume-update VU latches,
 * some enable bits, PLLK2 MSBs) live at bit 8 and ride the I2C framing's
 * first-byte LSB — see wm8758_write() in wm8758.c.
 */

#ifndef CORE_HAL_HW_WM8758_H
#define CORE_HAL_HW_WM8758_H

/* 7-bit I2C device address of the iPod audio codec (05-audio.md). */
#define WM8758_I2C_ADDR   0x1A

/* ---- Register numbers (7-bit) --------------------------------------- */
#define WM_RESET      0x00
#define WM_PWRMGMT1   0x01
#define WM_PWRMGMT2   0x02
#define WM_PWRMGMT3   0x03
#define WM_AINTFCE    0x04
#define WM_CLKCTRL    0x06
#define WM_ADDCTRL    0x07
#define WM_DACCTRL    0x0A
#define WM_LDACVOL    0x0B
#define WM_RDACVOL    0x0C
#define WM_PLLN       0x24
#define WM_PLLK1      0x25
#define WM_PLLK2      0x26
#define WM_PLLK3      0x27
#define WM_EQ1        0x12  /* low shelf  (Bass)  */
#define WM_EQ2        0x13  /* peaking band 2     */
#define WM_EQ3        0x14  /* peaking band 3     */
#define WM_EQ4        0x15  /* peaking band 4     */
#define WM_EQ5        0x16  /* high shelf (Treble) */
#define WM_OUT4TOADC  0x2A
#define WM_OUTCTRL    0x31
#define WM_LOUTMIX    0x32
#define WM_ROUTMIX    0x33
#define WM_LOUT1VOL   0x34
#define WM_ROUT1VOL   0x35
#define WM_LOUT2VOL   0x36
#define WM_ROUT2VOL   0x37
#define WM_OUT3MIX    0x38
#define WM_OUT4MIX    0x39
#define WM_BIASCTRL   0x3D

/* ---- PWRMGMT1 (0x01) ------------------------------------------------ */
#define PWRMGMT1_VMIDSEL_OFF   0x000
#define PWRMGMT1_VMIDSEL_75K   0x001  /* NORMAL OPERATION — the playback setting */
#define PWRMGMT1_VMIDSEL_500K  0x002  /* low-power STANDBY hold — not for playback:
                                        * the high-impedance divider has the worst
                                        * supply rejection, and on the device it put
                                        * the HDD motor, the piezo burst and a hiss
                                        * on the headphone out (2026-09-13) */
#define PWRMGMT1_VMIDSEL_10K   0x003  /* fast startup   */
#define PWRMGMT1_BUFIOEN       0x004
#define PWRMGMT1_BIASEN        0x008
#define PWRMGMT1_PLLEN         0x020

/* ---- PWRMGMT2 (0x02) ------------------------------------------------ */
#define PWRMGMT2_LOUT1EN       0x080
#define PWRMGMT2_ROUT1EN       0x100

/* ---- PWRMGMT3 (0x03) ------------------------------------------------ */
#define PWRMGMT3_DACENL        0x001
#define PWRMGMT3_DACENR        0x002
#define PWRMGMT3_LMIXEN        0x004
#define PWRMGMT3_RMIXEN        0x008
#define PWRMGMT3_ROUT2EN       0x020
#define PWRMGMT3_LOUT2EN       0x040

/* ---- AINTFCE (0x04) ------------------------------------------------- */
#define AINTFCE_FORMAT_I2S     0x010  /* 2<<3 */
#define AINTFCE_IWL_16BIT      0x000

/* ---- CLKCTRL (0x06) ------------------------------------------------- */
#define CLKCTRL_MS             0x001  /* codec is I2S clock master */
#define CLKCTRL_BCLKDIV_2      0x004
/* MCLKDIV occupies bits 7:5; the datasheet's R6 MCLKDIV[2:0] encoding is
 * 000=/1 001=/1.5 010=/2 011=/3 100=/4 101=/6 110=/8 111=/12. Only the
 * dividers our supported rates need are spelled out (05-audio.md gives the
 * /2 case as CLKCTRL_MCLKDIV_2 = 0x040, which pins the field position). */
#define CLKCTRL_MCLKDIV_2      0x040  /* 2<<5: SYSCLK = fPLLOUT/2 */
#define CLKCTRL_MCLKDIV_3      0x060  /* 3<<5: SYSCLK = fPLLOUT/3 */
#define CLKCTRL_MCLKDIV_4      0x080  /* 4<<5: SYSCLK = fPLLOUT/4 */
#define CLKCTRL_CLKSEL         0x100  /* clock source = PLL */

/* ---- ADDCTRL (0x07) — SR is a filter-class hint, not the real rate --
 * SR[2:0] sits in bits 3:1; datasheet R7 encoding 000=48k 001=32k 010=24k
 * 011=16k 100=12k 101=8k. The true rate comes from the PLL + MCLKDIV, so
 * the 44.1 kHz family legitimately reuses its nearest class (05-audio.md,
 * "ADDCTRL_SR is a filter-class hint, not the real rate"). */
#define ADDCTRL_SLOWCLKEN      0x001
#define ADDCTRL_SR_48kHz       0x000
#define ADDCTRL_SR_32kHz       0x002
#define ADDCTRL_SR_24kHz       0x004

/* ---- DACCTRL (0x0A) ------------------------------------------------- */
#define DACCTRL_DACOSR128      0x008  /* 128x oversample (the "unmuted" state) */
#define DACCTRL_SOFTMUTE       0x040

/* ---- LDACVOL / RDACVOL (0x0B / 0x0C) --------------------------------
 * 8-bit digital attenuator, 0.5 dB per step: 0xFF = 0 dB (full scale),
 * 0x00 = mute. hal_eq_set() pre-attenuates here by the EQ curve's largest
 * boost so a full-scale track cannot clip the digital path. */
#define DACVOL_MASK            0x0FF
#define DACVOL_0DB             0x0FF  /* full scale — the codec's own default */
#define DACVOL_DACVU           0x100  /* latch L+R DAC volume now */

/* ---- PLLN (0x24) ---------------------------------------------------- */
#define PLLN_PLLPRESCALE       0x010

/* ---- OUT4TOADC (0x2A) ----------------------------------------------- */
#define OUT4TOADC_POBCTRL      0x004  /* VMID-independent bias / pop control */
#define OUT4TOADC_VMIDTOG      0x010  /* VMID discharge toggle (close)       */

/* ---- OUTCTRL (0x31) ------------------------------------------------- */
#define OUTCTRL_VROI           0x001
#define OUTCTRL_TSDEN          0x002
#define OUTCTRL_TSOPCTRL       0x004
#define OUTCTRL_LINE_COM       0x080
#define OUTCTRL_HP_COM         0x100

/* ---- LOUTMIX / ROUTMIX (0x32 / 0x33) -------------------------------- */
#define LOUTMIX_DACL2LMIX      0x001
#define ROUTMIX_DACR2RMIX      0x001

/* ---- OUT1/OUT2 volume (0x34-0x37) — shared bit layout --------------- */
#define OUTVOL_GAIN_MASK       0x03F  /* 6-bit amp gain */
#define OUTVOL_MUTE            0x040
#define OUTVOL_ZC              0x080  /* zero-cross gain change */
#define OUTVOL_VU              0x100  /* latch L+R gain now */

/* ---- OUT3MIX / OUT4MIX (0x38 / 0x39) -------------------------------- */
#define OUTMIX_MUTE            0x040  /* preinit mute value for OUT3/4 mixers */

/* ---- BIASCTRL (0x3D) ------------------------------------------------ */
#define BIASCTRL_BIASCUT       0x100  /* low-power bias cut */

/* ---- EQ1..EQ5 (0x12-0x16): 5-band EQ tone control (05-audio.md) -----
 * Gain field EQxG[4:0] uses code = 12 - gain_dB (0x0C = 0 dB flat, 0x00 =
 * +12 dB, 0x18 = -12 dB). EQ3DMODE (EQ1 bit 8) routes the EQ to the DAC
 * (playback) when set; cleared it sits on the ADC path (inert here). */
#define EQ_GAIN_MASK           0x01F  /* EQxG[4:0]                        */
#define EQ_GAIN_0DB            0x00C  /* flat: code = 12 - 0 dB           */
#define EQ_DAC_MODE            0x100  /* EQ1 bit 8: apply EQ to the DAC   */
#define EQ1_CUTOFF_105HZ       0x020  /* EQ1C=01: bass shelf @ 105 Hz     */
#define EQ5_CUTOFF_6K9         0x020  /* EQ5C=01: treble shelf @ 6.9 kHz  */
/*
 * EQxC[1:0] (bits 6:5) selects the band's corner/centre. The code means a
 * different frequency per band — the tables are in 05-audio.md; here is only
 * the field. EQxBW (bit 8) widens/narrows a PEAKING band (EQ2..EQ4): on EQ1
 * that bit is EQ3DMODE and on EQ5 it is unused, so neither shelf may carry
 * it.
 *
 * UNVERIFIED — FROM DATASHEET MEMORY: the per-band centre-frequency tables for
 * EQ2..EQ4 and the polarity of EQxBW (this header assumes 0 = wide, 1 =
 * narrow) were written down from memory of the WM8758B datasheet, not read out
 * of it or off the device. Check both against the PDF before relying on them.
 * Being wrong costs a band centred elsewhere, or five presets whose three
 * peaks are narrow instead of wide — a differently shaped preset, never a
 * fault. The two SHELF corners are not in doubt: the shipped tone control has
 * used them. See 05-audio.md.
 */
#define EQ_CUTOFF_SHIFT        5      /* EQxC[1:0] position               */
#define EQ_CUTOFF_MASK         0x060  /* EQxC[1:0] field                  */
#define EQ_BW_NARROW           0x100  /* EQ2..EQ4 bit 8: narrow bandwidth */
#define EQ_BAND_COUNT          5      /* EQ1..EQ5 — the silicon's bands   */

/* ---- Sample-rate program: 44.1 kHz (05-audio.md, resolved) ---------
 * PLL preset 0 -> fPLLOUT 22.5792 MHz; MCLKDIV/2 -> SYSCLK = 256*44.1kHz.
 * The SR field intentionally uses the 48 kHz class value; the true rate
 * comes from the PLL, not the SR hint. */
#define WM_PLLN_44     (PLLN_PLLPRESCALE | 0x7)                             /* 0x17 */
#define WM_PLLK1_44    0x21
#define WM_PLLK2_44    0x161
#define WM_PLLK3_44    0x26
#define WM_CLKCTRL_44  (CLKCTRL_CLKSEL | CLKCTRL_MCLKDIV_2 | \
                        CLKCTRL_BCLKDIV_2 | CLKCTRL_MS)                     /* 0x145 */
#define WM_ADDCTRL_44  (ADDCTRL_SR_48kHz | ADDCTRL_SLOWCLKEN)              /* 0x001 */

/* ---- PLL presets: the two operating points (05-audio.md, "DAC sample-rate
 * setup" + the numeric appendix). Both run from a 12 MHz reference (the SoC
 * feeds 24 MHz; PLLPRESCALE halves it). SYSCLK = fPLLOUT / MCLKDIV must land
 * on exactly 256 x the sample rate.
 *
 *   preset 0: fPLLOUT = 22.5792 MHz  -> the 44.1 kHz family (44.1 / 22.05)
 *   preset 1: fPLLOUT = 24.576  MHz  -> the 48 kHz family (48 / 32 / 24)
 *
 * Preset 0's coefficients are the resolved 44.1 kHz sequence; preset 1's are
 * the 48 kHz preset recorded alongside it (PLLN=0x18, K1=0x0C, K2=0x93,
 * K3=0xE9). */
#define WM_PLLN_48     (PLLN_PLLPRESCALE | 0x8)                             /* 0x18 */
#define WM_PLLK1_48    0x0C
#define WM_PLLK2_48    0x93
#define WM_PLLK3_48    0xE9

/* ---- Codec driver API (wm8758.c) ------------------------------------
 * Requires the SoC I2C controller to be initialised first (i2c_init).
 * wm8758_init brings the codec fully up for I2S playback at the rate chosen
 * by wm8758_set_rate (44.1 kHz by default): power rails, I2S 16-bit format,
 * DAC PLL, DAC->output routing, and unmute. It now supplies the datasheet's
 * VMID settle itself (a USEC_TIMER wait between the 10k fast-charge and the
 * 500k hold) rather than leaving it to a caller that never did it.
 */
#ifndef __ASSEMBLER__
#include <stdbool.h>
#include <stdint.h>

/*
 * Select the sample rate the NEXT wm8758_init() will program. Accepts the
 * rates in the PLL preset table (44100, 48000, 32000, 24000, 22050); anything
 * else is rejected and the previous selection stands. Returns 0 on success,
 * -1 if the rate is unsupported. Defaults to 44100 until called.
 *
 * Split from wm8758_init (rather than made a parameter) so the codec bring-up
 * keeps its zero-argument shape.
 */
int wm8758_set_rate(uint32_t sample_rate);

/*
 * Register a callback invoked near the end of wm8758_init(), at the
 * datasheet's "unmute outputs and set desired volume" step (the DAC is
 * still soft-muted; only POBCTRL-off follows it). hal/hw/audio.c points this
 * at hal_codec_restore() so the user's volume/balance/bass/treble survive
 * the full WM_RESET that starts every cold bring-up. NULL (the default)
 * disables it.
 */
void wm8758_set_restore(void (*fn)(void));

/*
 * Bring a COLD codec up: the datasheet's power-up sequence, WM_RESET first,
 * a 100 ms VMID rise inside, DAC left soft-muted. Boot, the Play after a
 * persistent pause, the first play after a close — never a track change
 * (that is wm8758_retune). Returns the number of control writes that FAILED
 * (0 = the whole sequence was accepted). See wm8758.c for what that number
 * can actually detect: the PP502x controller has no per-byte NAK status, so
 * the only observable failure is a wedged bus (BUSY never clearing) — but
 * that is precisely the case where playback would otherwise be silent with
 * no indication anywhere.
 */
int  wm8758_init(void);

/*
 * Re-clock a WARM codec to the rate last given to wm8758_set_rate. No writes
 * at all when the codec is already programmed for it; otherwise the PLL and
 * dividers are re-programmed with PLLEN toggled around the write and a
 * bounded lock wait after. The caller guarantees the DAC is soft-muted and
 * the DMA stopped (hal/hw/audio.c does), and resets the I2S FIFO afterwards.
 * Same return as wm8758_init. Meaningless on a cold codec — use wm8758_init.
 */
int  wm8758_retune(void);
/*
 * DAC soft-mute. The bring-up leaves the DAC muted; the stream unmutes once
 * PCM is actually flowing. A mute does not return until the ramp is over
 * (bounded USEC_TIMER wait, ~23 ms at 44.1 kHz), so the caller may cut the
 * data the moment it returns. Neither direction touches the oversampling
 * ratio. Callable in any codec state; a mute of a cold codec is a harmless
 * write plus the wait.
 */
void wm8758_mute(bool mute);

/* Pop-suppressed power-down: mute, discharge VMID, WAIT for it to drain
 * (~300 ms, bounded), then drop all power rails. The codec is left cold —
 * wm8758_init() brings it back. Call while MCLK is still running; gate
 * clocks afterwards. Blocks for the drain, so not on a button's path. */
void wm8758_powerdown(void);
#endif

#endif /* CORE_HAL_HW_WM8758_H */
