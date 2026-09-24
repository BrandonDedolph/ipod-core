/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/wm8758.c — Wolfson WM8758B codec bring-up for 44.1 kHz
 * I2S playback.
 *
 * Reaches the codec over the SoC I2C controller (i2c.c). Register/bit
 * numbers are the WM8758B datasheet's own register map (see wm8758.h).
 * wm8758_init follows the datasheet's "Recommended Power Up Sequence" step
 * for step: low bias, outputs muted and ENABLED, POBCTRL (the VMID-
 * independent bias) on, DACs and mixers on, then VMID on its normal 75k
 * divider with BIASEN + BUFIOEN, the interface and PLL, a 100 ms wait for
 * VMID to rise, the output unmute + volume, and POBCTRL off LAST. It is
 * expressed as data tables so the exact bus grammar is easy to assert in
 * the trace tests. wm8758_powerdown is the datasheet's "Recommended Power
 * Down Sequence", drain wait included.
 *
 * THE BRING-UP IS NOT PER TRACK. It runs when the codec is COLD: at boot,
 * after the persistent-pause power-down, after a close. A track change on a
 * warm codec is wm8758_retune() — nothing at all when the rate is unchanged,
 * and a muted PLL re-program when it is not. The reset + VMID cycle used to
 * run on every Next, every row play and every rate change, and each cycle
 * was a thump in the headphones (device, 2026-09-22); the datasheet
 * sequence is a power-up procedure, not a track-change one.
 *
 * Asm-free so it host-compiles for the mock-bus tests. NOT delay-free: the
 * VMID rise, the VMID drain, the PLL lock and the DAC soft-mute ramp are all
 * bounded USEC_TIMER waits here, where the sequence needs them.
 *
 * The bring-up ends with the DAC SOFT-MUTED. Unmuting is the stream's job,
 * after the DMA is feeding the serializer (hal/hw/audio.c).
 */

#include "wm8758.h"
#include "i2c.h"
#include "pp5022.h"
#include "mmio.h"

/*
 * WM8758 control word: 7-bit register in bits 15:9, 9-bit data in 8:0,
 * packed into two I2C payload bytes — byte0 = (reg<<1) | data bit 8,
 * byte1 = data low 8 bits (05-audio.md, "DAC I²C interface").
 *
 * Returns i2c_send's status. NOTE what that status can and cannot tell you:
 * the PP502x controller exposes NO per-byte ACK/NAK bit (09-i2c.md lists only
 * BUSY in I2C_STATUS), so a codec that is absent, unpowered or holding SDA
 * cannot be distinguished from a healthy one by looking at the bus. The one
 * real signal is the BUSY-clear timeout: if the controller never finishes a
 * transaction the bus is wedged. That is the only failure this can report, and
 * it is worth reporting — the alternative failure mode is a UI with a moving
 * progress bar and total silence.
 */
static int wm8758_write(uint8_t reg, uint16_t data)
{
    uint8_t frame[2];
    frame[0] = (uint8_t)((reg << 1) | ((data >> 8) & 0x1));
    frame[1] = (uint8_t)(data & 0xFF);
    return i2c_send(WM8758_I2C_ADDR, frame, 2);
}

struct wm_write {
    uint8_t  reg;
    uint16_t data;
};

/*
 * Moderate output level for first sound. Headphone amp gain 0x39 = 0 dB
 * (the codec reset default); the DAC digital volume is full-scale. The
 * source tone is generated at ~-8.7 dBFS, which lands at a reasonable
 * listening level. Tune down here if it is too loud on sensitive IEMs.
 */
#define WM_HP_GAIN_0DB 0x39

/*
 * PWRMGMT1 in normal operation: PLL, bias and the I/O buffer on, VMID on the
 * 75k divider. The datasheet's power-up sequence puts VMID on THIS divider
 * from the start (VMIDSEL=01) and waits for it to rise, rather than the 10k
 * fast-charge this driver used while the bring-up ran once per track: the
 * headphone amps are enabled before VMID charges, so their outputs (and the
 * coupling caps into the headphones) follow the VMID ramp, and a 7x faster
 * ramp is a 7x bigger thump. Now that the bring-up is a power-up and not a
 * track change, the slower charge costs nothing that matters. Also the value
 * wm8758_retune restores after it has toggled PLLEN.
 */
#define WM_PWRMGMT1_RUN  (PWRMGMT1_PLLEN | PWRMGMT1_BIASEN \
                          | PWRMGMT1_BUFIOEN | PWRMGMT1_VMIDSEL_75K)

/*
 * The VMID rise: the datasheet's "Wait 100ms to allow VMID to rise
 * sufficiently before unmuting outputs", between the PWRMGMT1 write that
 * starts the charge and the output unmute. It used to be 40 ms, chosen to
 * hide inside a per-track bring-up; that bring-up no longer exists (see the
 * file header), so the datasheet's own number stands. Paid at boot, on the
 * Play after a persistent pause, and on the first play after a close.
 */
#define WM_VMID_SETTLE_US   100000u

/*
 * The VMID drain: the datasheet's "Wait for VMID to discharge" between
 * cutting the VMID divider (VMIDTOG set, VMIDSEL off) and switching the
 * output amps and bias off. Powering the amps off with VMID still at midrail
 * steps their outputs to ground through the coupling caps — the DC pop on
 * every power-down, which 05-audio.md always said needs ~300 ms and which the
 * table below never waited for. DEVICE-GATED: the datasheet gives no figure
 * (it depends on the VMID capacitor); 300 ms is the doc's. Paid only where
 * nobody is waiting on a button: 5 s into a pause, at close, at standby, and
 * once at boot (hal_audio_boot_quiet cannot tell a live codec from a cold
 * one — the control port is write-only).
 */
#define WM_VMID_DRAIN_US    300000u

/*
 * PLL re-lock after wm8758_retune re-programs it with PLLEN toggled. The DAC
 * is soft-muted and the DMA is off for the whole retune, so an unlocked clock
 * reaches nothing audible; this only keeps the I2S block from being reset
 * (i2s_init follows) under a still-wandering BCLK. Not a datasheet figure.
 */
#define WM_PLL_LOCK_US      5000u

/*
 * Guard on the settle loop. USEC_TIMER is a free-running 1 MHz counter so on
 * silicon the elapsed test always terminates; the trip cap is the same
 * "no unbounded loops, ever" discipline the rest of the HAL follows, and it is
 * what lets this run under the host mock bus (whose fake counter never
 * advances). Same MMIO_MOCK split kernel/clock.c uses for its PLL-lock spin,
 * and for the same reason: a full-size never-advances spin would flood the
 * recording bus's fixed-capacity event log.
 */
#ifdef MMIO_MOCK
#define WM_SETTLE_GUARD_TRIPS  4u
#else
#define WM_SETTLE_GUARD_TRIPS  (1u << 24)
#endif

static void wm8758_settle_us(uint32_t us)
{
    uint32_t t0    = mmio_read32(USEC_TIMER_ADDR);
    uint32_t guard = WM_SETTLE_GUARD_TRIPS;
    while ((uint32_t)(mmio_read32(USEC_TIMER_ADDR) - t0) < us && --guard != 0) {
        /* wait */
    }
}

/*
 * Per-rate PLL + divider program (05-audio.md, "DAC sample-rate setup" and the
 * resolved 44.1 kHz appendix). Six writes, always in this order. SYSCLK =
 * fPLLOUT / MCLKDIV and must equal exactly 256 x fs:
 *
 *   44100 : preset 0 (22.5792 MHz) / 2 = 11.2896 MHz = 256 x 44100
 *   22050 : preset 0 (22.5792 MHz) / 4 =  5.6448 MHz = 256 x 22050
 *   48000 : preset 1 (24.576  MHz) / 2 = 12.288  MHz = 256 x 48000
 *   32000 : preset 1 (24.576  MHz) / 3 =  8.192  MHz = 256 x 32000
 *   24000 : preset 1 (24.576  MHz) / 4 =  6.144  MHz = 256 x 24000
 *
 * The ADDCTRL SR field is only a filter-class hint, so each rate carries its
 * nearest class rather than an exact match (44.1 kHz uses the 48 kHz class —
 * that is the documented, intended behaviour, not an oversight).
 */
struct wm_rate {
    uint32_t rate;
    uint16_t plln, pllk1, pllk2, pllk3;
    uint16_t clkctrl;
    uint16_t addctrl;
};

#define WM_CLKCTRL_BASE  (CLKCTRL_CLKSEL | CLKCTRL_BCLKDIV_2 | CLKCTRL_MS)

static const struct wm_rate rate_table[] = {
    { 44100u, WM_PLLN_44, WM_PLLK1_44, WM_PLLK2_44, WM_PLLK3_44,
      WM_CLKCTRL_BASE | CLKCTRL_MCLKDIV_2, ADDCTRL_SR_48kHz | ADDCTRL_SLOWCLKEN },
    { 48000u, WM_PLLN_48, WM_PLLK1_48, WM_PLLK2_48, WM_PLLK3_48,
      WM_CLKCTRL_BASE | CLKCTRL_MCLKDIV_2, ADDCTRL_SR_48kHz | ADDCTRL_SLOWCLKEN },
    { 32000u, WM_PLLN_48, WM_PLLK1_48, WM_PLLK2_48, WM_PLLK3_48,
      WM_CLKCTRL_BASE | CLKCTRL_MCLKDIV_3, ADDCTRL_SR_32kHz | ADDCTRL_SLOWCLKEN },
    { 24000u, WM_PLLN_48, WM_PLLK1_48, WM_PLLK2_48, WM_PLLK3_48,
      WM_CLKCTRL_BASE | CLKCTRL_MCLKDIV_4, ADDCTRL_SR_24kHz | ADDCTRL_SLOWCLKEN },
    { 22050u, WM_PLLN_44, WM_PLLK1_44, WM_PLLK2_44, WM_PLLK3_44,
      WM_CLKCTRL_BASE | CLKCTRL_MCLKDIV_4, ADDCTRL_SR_24kHz | ADDCTRL_SLOWCLKEN },
};

/* Index into rate_table; 0 (44.1 kHz) until wm8758_set_rate says otherwise. */
static unsigned g_rate_idx;

/* The rate_table entry the codec's PLL/dividers are ACTUALLY programmed
 * with: set by wm8758_init and wm8758_retune, -1 while the codec is cold
 * (nothing is programmed after a reset or a power-down). What lets a retune
 * at an unchanged rate be zero writes. */
static int g_prog_idx = -1;

int wm8758_set_rate(uint32_t sample_rate)
{
    for (unsigned i = 0; i < sizeof rate_table / sizeof rate_table[0]; i++) {
        if (rate_table[i].rate == sample_rate) {
            g_rate_idx = i;
            return 0;
        }
    }
    return -1;      /* unsupported: leave the previous selection in place */
}

/* Callback run at the tail of wm8758_init (see wm8758.h). */
static void (*g_restore)(void);

void wm8758_set_restore(void (*fn)(void))
{
    g_restore = fn;
}

/*
 * Bring-up, part A: soft reset through the start of the VMID charge, then
 * the fixed interface config. Everything before the rate program. The order
 * is the datasheet's: outputs muted and enabled, POBCTRL on, DACs/mixers on,
 * THEN VMID + bias — the amps come up on the VMID-independent bias and their
 * outputs follow VMID up from zero instead of snapping to it later.
 */
static const struct wm_write init_seq_a[] = {
    /* --- soft reset to known power-on defaults ---------------------- *
     * We chainload after Apple's flash ROM (and possibly disk mode),
     * which may have left codec state in registers this sequence does
     * not touch (ADC path, EQ, limiter, ALC, input mux). A reset (write
     * any value to reg 0x00) guarantees we start from datasheet defaults
     * rather than inherited state. The I2C control port works with or
     * without MCLK, so this is safe as step 0. */
    { WM_RESET,     0 },

    /* --- preinit: bias + protection, everything muted --------------- */
    { WM_BIASCTRL,  BIASCTRL_BIASCUT },
    { WM_OUTCTRL,   OUTCTRL_HP_COM | OUTCTRL_LINE_COM | OUTCTRL_TSOPCTRL
                    | OUTCTRL_TSDEN | OUTCTRL_VROI },
    { WM_LOUT1VOL,  OUTVOL_VU | OUTVOL_MUTE },   /* 0x140 */
    { WM_ROUT1VOL,  OUTVOL_VU | OUTVOL_MUTE },
    { WM_LOUT2VOL,  OUTVOL_VU | OUTVOL_MUTE },
    { WM_ROUT2VOL,  OUTVOL_VU | OUTVOL_MUTE },
    { WM_OUT3MIX,   OUTMIX_MUTE },               /* 0x40 */
    { WM_OUT4MIX,   OUTMIX_MUTE },

    /* --- power rails up in order ------------------------------------ */
    { WM_PWRMGMT2,  PWRMGMT2_LOUT1EN | PWRMGMT2_ROUT1EN },
    { WM_OUT4TOADC, OUT4TOADC_POBCTRL },         /* VMID-independent bias */
    { WM_PWRMGMT3,  PWRMGMT3_DACENL | PWRMGMT3_DACENR
                    | PWRMGMT3_LMIXEN | PWRMGMT3_RMIXEN },
    { WM_PWRMGMT1,  WM_PWRMGMT1_RUN },           /* VMID starts charging */

    /* --- interface + clocking: I2S 16-bit, codec is master ---------- */
    { WM_AINTFCE,   AINTFCE_FORMAT_I2S | AINTFCE_IWL_16BIT },
    { WM_CLKCTRL,   CLKCTRL_MS },
};

/*
 * Bring-up, part B: the DAC route, issued after the rate program. Ends just
 * before the VMID settle. POBCTRL is NOT dropped here: it used to be, ~3 ms
 * after VMID began charging and 40 ms before the settle ended, which handed
 * the output amps to a VMID-derived bias that did not exist yet. The
 * datasheet clears it as the very last step, after the outputs are unmuted;
 * wm8758_init does.
 */
static const struct wm_write init_seq_b[] = {
    /* --- route DAC to the output mixers (without this: silence) ----- */
    { WM_LOUTMIX,   LOUTMIX_DACL2LMIX },
    { WM_ROUTMIX,   ROUTMIX_DACR2RMIX },
};

/*
 * Bring-up, part C: everything AFTER the VMID settle — drop the low-bias,
 * then volume and the output unmute. VMID is already on the 75k divider it
 * plays through (WM_PWRMGMT1_RUN), so there is no hand-over write here.
 *
 * Never the 500k "low-power hold". That is the datasheet's standby divider:
 * the reference sits behind the highest impedance the part offers, so every
 * supply disturbance walks straight onto the outputs. On the device it did:
 * the drive's spin-up current, the piezo's PWM burst and a steady hiss were
 * all audible on the headphone jack while playing (2026-09-13).
 */
static const struct wm_write init_seq_c[] = {
    /* --- postinit: clear low-bias ------------------------------------ */
    { WM_BIASCTRL,  0 },

    /* --- volume; the bring-up ENDS SOFT-MUTED ------------------------ */
    { WM_LDACVOL,   DACVOL_0DB },                /* full-scale, no VU yet */
    { WM_RDACVOL,   DACVOL_0DB | DACVOL_DACVU },  /* VU latches L+R */
    { WM_LOUT1VOL,  WM_HP_GAIN_0DB | OUTVOL_ZC },
    { WM_ROUT1VOL,  WM_HP_GAIN_0DB | OUTVOL_ZC | OUTVOL_VU },
    /*
     * 128x OSR, DAC still soft-muted. This used to be the unmute, "last so
     * no partially-configured state is audible" — but at this point NO PCM
     * is flowing: the I2S TX FIFO was just reset and the DMA has not been
     * kicked, and after this come the user's volume/EQ writes over I2C
     * (milliseconds) before hal_audio_start ever runs. An unmuted DAC fed by
     * an idle serializer plays whatever that serializer emits on underrun,
     * and the soft-unmute ramp makes it audible. The stream unmutes itself
     * (wm8758_mute(false)) once the DMA is actually streaming — audio.c.
     */
    { WM_DACCTRL,   DACCTRL_DACOSR128 | DACCTRL_SOFTMUTE },
};

/* Run one table, accumulating the failed-write count. */
static int wm8758_run(const struct wm_write *seq, unsigned n)
{
    int bad = 0;
    for (unsigned i = 0; i < n; i++) {
        if (wm8758_write(seq[i].reg, seq[i].data) != 0) {
            bad++;
        }
    }
    return bad;
}

#define WM_RUN(tbl) wm8758_run((tbl), sizeof (tbl) / sizeof (tbl)[0])

int wm8758_init(void)
{
    const struct wm_rate *r = &rate_table[g_rate_idx];
    int bad = 0;

    bad += WM_RUN(init_seq_a);

    /* Rate program: six writes, fixed order (05-audio.md's resolved
     * sequence), values from the per-rate table. */
    bad += (wm8758_write(WM_PLLN,    r->plln)    != 0);
    bad += (wm8758_write(WM_PLLK1,   r->pllk1)   != 0);
    bad += (wm8758_write(WM_PLLK2,   r->pllk2)   != 0);
    bad += (wm8758_write(WM_PLLK3,   r->pllk3)   != 0);
    bad += (wm8758_write(WM_CLKCTRL, r->clkctrl) != 0);
    bad += (wm8758_write(WM_ADDCTRL, r->addctrl) != 0);

    bad += WM_RUN(init_seq_b);

    /* THE VMID RISE. VMID has been charging on the 75k divider since the
     * PWRMGMT1 write in init_seq_a, with the output amps enabled on the
     * VMID-independent bias; hold here so the rail is up before the outputs
     * are unmuted. Skipping it, or shortening it, is the pop. */
    wm8758_settle_us(WM_VMID_SETTLE_US);

    bad += WM_RUN(init_seq_c);

    /* Put the user's settings back. The WM_RESET at the top of this function
     * wiped volume, balance and the EQ back to datasheet defaults. This is
     * the datasheet's "unmute L/ROUT1 and set desired volume" step: the
     * DAC is still soft-muted (init_seq_c leaves it so) and the OUT1VOL
     * writes carry ZC, so nothing in this restore can be heard. */
    if (g_restore) {
        g_restore();
    }

    /* And LAST, the VMID-independent bias off: the amps now run from the
     * VMID that has risen underneath them. The datasheet's final step. */
    bad += (wm8758_write(WM_OUT4TOADC, 0) != 0);

    g_prog_idx = (int)g_rate_idx;
    return bad;
}

/*
 * A track change on a WARM codec. The rate the caller last set
 * (wm8758_set_rate) is compared with what the PLL and dividers are actually
 * programmed with: equal, and this touches nothing — the codec is already
 * clocked right, muted (every stop leaves it so), and warm. Different, and
 * the six rate registers are re-programmed with the PLL disabled across
 * the write and re-enabled after, then a bounded lock wait.
 *
 * PRECONDITIONS the caller owns (hal/hw/audio.c): the DAC is soft-muted and
 * the DMA is stopped, so the codec, which masters BCLK/LRCLK from this PLL,
 * can lose and re-find its clocks with nothing audible on the output and
 * nothing in flight on the link. The caller resets the I2S FIFO AFTER this
 * returns, so whatever the serializer did under a re-locking clock is
 * flushed before the next kick.
 *
 * This is what replaced the per-track wm8758_init. Returns the number of
 * writes that failed, as wm8758_init does; 0 when nothing was written.
 */
int wm8758_retune(void)
{
    if (g_prog_idx == (int)g_rate_idx) {
        return 0;
    }
    const struct wm_rate *r = &rate_table[g_rate_idx];
    int bad = 0;

    bad += (wm8758_write(WM_PWRMGMT1, WM_PWRMGMT1_RUN & ~PWRMGMT1_PLLEN) != 0);
    bad += (wm8758_write(WM_PLLN,    r->plln)    != 0);
    bad += (wm8758_write(WM_PLLK1,   r->pllk1)   != 0);
    bad += (wm8758_write(WM_PLLK2,   r->pllk2)   != 0);
    bad += (wm8758_write(WM_PLLK3,   r->pllk3)   != 0);
    bad += (wm8758_write(WM_CLKCTRL, r->clkctrl) != 0);
    bad += (wm8758_write(WM_ADDCTRL, r->addctrl) != 0);
    bad += (wm8758_write(WM_PWRMGMT1, WM_PWRMGMT1_RUN) != 0);
    wm8758_settle_us(WM_PLL_LOCK_US);

    g_prog_idx = (int)g_rate_idx;
    return bad;
}

/*
 * DACMU is a SOFT mute: the DAC ramps its output to zero over many sample
 * periods rather than cutting it. Two consequences the caller cannot see
 * from a register write:
 *
 *  - The write returns before the ramp has even begun. i2c_send() returns
 *    with the transaction still on the wire (09-i2c.md; i2c.c drains it
 *    lazily at the NEXT call), so a caller that cuts the DMA straight after
 *    "muting" cuts it with the DAC still live, and the remainder of the ramp
 *    then rides whatever the starved serializer emits — the click on pause.
 *    So a mute waits here until the ramp is over, and the PCM keeps flowing
 *    underneath it: the ramp fades real audio, not underrun garbage. The wait
 *    is bounded and mock-guarded exactly like the VMID settle.
 *
 *  - DACOSR128 lives in the same register. The old mute wrote SOFTMUTE alone
 *    and the unmute wrote DACOSR128 alone, so every pause dropped the DAC to
 *    64x oversampling and every play put it back — a filter reconfiguration
 *    at the exact instant the listener is most likely to hear it. OSR is
 *    128x always; only DACMU moves.
 *
 * WM_MUTE_RAMP_FRAMES is the datasheet-uncertain constant: Wolfson soft-mute
 * ramps in this family are a few hundred to ~1024 sample periods, so 1024 at
 * the programmed rate (23 ms at 44.1 kHz, 46 ms at 22.05 kHz) covers the
 * longest of them. Too long only delays a pause by that much; too short
 * leaves a tail of the ramp on the underrun output.
 */
#define WM_MUTE_RAMP_FRAMES  1024u

void wm8758_mute(bool mute)
{
    if (!mute) {
        (void)wm8758_write(WM_DACCTRL, DACCTRL_DACOSR128);
        return;
    }
    (void)wm8758_write(WM_DACCTRL, DACCTRL_DACOSR128 | DACCTRL_SOFTMUTE);
    uint32_t rate = rate_table[g_rate_idx].rate;
    wm8758_settle_us((uint32_t)(((uint64_t)WM_MUTE_RAMP_FRAMES * 1000000u)
                                / rate));
}

/*
 * Pop-suppressed power-DOWN — the datasheet "Recommended Power Down
 * Sequence": thermal shutdown off, VMIDTOG on, the VMID divider and the I/O
 * buffer off, WAIT for VMID to discharge, and only then R1/R2/R3 = 0. The
 * DAC soft-mute and the headphone-output mutes in front of it are ours (so
 * nothing is live while VMID falls); POBCTRL is asserted alongside VMIDTOG
 * for the same reason the power-up asserts it while VMID is absent.
 *
 * The drain wait used to be missing: the amps, VMID and bias were cut in
 * three consecutive I2C writes, ~150 us after the discharge began, i.e.
 * with VMID still at midrail — the DC pop on every power-down. Leaves the
 * codec cold; the next wm8758_init() does the full reset + bring-up, so this
 * is fully recoverable. MCLK is still running when this is called; the
 * caller gates clocks AFTER.
 */
static const struct wm_write powerdown_pre[] = {
    { WM_DACCTRL,   DACCTRL_DACOSR128 | DACCTRL_SOFTMUTE },   /* DAC muted (OSR kept) */
    { WM_LOUT1VOL,  OUTVOL_VU | OUTVOL_MUTE },                /* mute HP outputs    */
    { WM_ROUT1VOL,  OUTVOL_VU | OUTVOL_MUTE },
    { WM_OUTCTRL,   OUTCTRL_HP_COM | OUTCTRL_LINE_COM | OUTCTRL_TSOPCTRL
                    | OUTCTRL_VROI },                         /* thermal shutdown off */
    { WM_OUT4TOADC, OUT4TOADC_POBCTRL | OUT4TOADC_VMIDTOG },  /* pop ctrl + VMID discharge */
    { WM_PWRMGMT1,  PWRMGMT1_PLLEN | PWRMGMT1_BIASEN },       /* VMID divider + BUFIO off */
};

static const struct wm_write powerdown_post[] = {
    { WM_PWRMGMT1,  0 },                                      /* BIAS + PLL off     */
    { WM_PWRMGMT2,  0 },                                      /* output amps off    */
    { WM_PWRMGMT3,  0 },                                      /* DAC + mixers off   */
};

void wm8758_powerdown(void)
{
    (void)WM_RUN(powerdown_pre);
    wm8758_settle_us(WM_VMID_DRAIN_US);       /* VMID to ground BEFORE the amps go */
    (void)WM_RUN(powerdown_post);
    g_prog_idx = -1;                          /* nothing is programmed in a cold codec */
}
