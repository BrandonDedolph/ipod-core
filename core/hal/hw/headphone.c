/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/headphone.c — headphone-jack insertion detect (PP5022 GPIO),
 * with the debounce that makes it safe to pause on, and the compiled-out probe
 * that identifies the pin on the device.
 *
 * Cleanroom from core/docs/hw/10-headphone-jack.md. Freestanding: <stdint.h>,
 * the mmio.h seam and (probe only) the SER0 UART driver, so this compiles
 * unchanged against the recording mock bus for the host trace test.
 *
 * WHY a debounce at all: the jack switch is a spring contact. On insertion it
 * bounces for milliseconds; a plug wiggled in a pocket can open it for longer.
 * The consumer of this signal pauses playback, and a pause nobody asked for in
 * the middle of a song is a worse defect than the feature it replaces. So a new
 * level has to hold for HEADPHONE_DEBOUNCE_US before it is believed, and any
 * return to the old level restarts the wait — a bouncing input produces
 * exactly one transition, at the end of the bounce.
 *
 * WHY microseconds: the hw build has no clock_ms() (hal.h declares it; only
 * the sim defines it). USEC_TIMER is the free-running 1 MHz counter every
 * other driver times itself on, and the mock bus can script it, which keeps
 * the golden trace exact. It wraps every ~71 minutes; unsigned subtraction
 * makes the elapsed-time compare wrap-safe.
 */

#include <stdint.h>

#include "pp5022.h"
#include "mmio.h"
#include "headphone.h"
#include "hal.h"          /* hal_headphones_present() contract */

/* ---------- Debounce (pure) ------------------------------------------------ */

void headphone_debounce_reset(headphone_debounce_t *d)
{
    d->primed      = 0;
    d->stable      = 0;
    d->cand_active = 0;
    d->cand        = 0;
    d->cand_since  = 0;
}

int headphone_debounce_feed(headphone_debounce_t *d, int raw, uint32_t now_us)
{
    uint8_t level = raw ? 1u : 0u;

    if (!d->primed) {
        /* First sample: take it as truth. Waiting here would only delay the
         * boot-time answer; there is no previous state a bounce could
         * corrupt. */
        d->primed = 1;
        d->stable = level;
        return d->stable;
    }
    if (level == d->stable) {
        /* Back at (or still at) the believed level: whatever was being timed
         * was a bounce or a wiggle. Drop it. */
        d->cand_active = 0;
        return d->stable;
    }
    if (!d->cand_active || d->cand != level) {
        d->cand_active = 1;
        d->cand        = level;
        d->cand_since  = now_us;
        return d->stable;
    }
    if ((uint32_t)(now_us - d->cand_since) >= HEADPHONE_DEBOUNCE_US) {
        d->stable      = level;
        d->cand_active = 0;
    }
    return d->stable;
}

/* ---------- Driver -------------------------------------------------------- */

/*
 * GPIO bank layout (10-headphone-jack.md, "GPIO bank layout"): three quads of
 * four 8-bit ports, per-port stride 4, register groups at +0x00 ENABLE,
 * +0x10 OUTPUT_EN, +0x20 OUTPUT_VAL, +0x30 INPUT_VAL. Port A is the first
 * port of the A-D quad, so these two plus the quad base ARE the A7
 * configuration registers. The remaining quads and groups are used only by
 * the UART probe and are defined with it.
 */
#define GPIO_QUAD_AD         0x6000D000u
#define GPIO_GRP_ENABLE      0x00u
#define GPIO_GRP_OUTPUT_EN   0x10u

static headphone_debounce_t s_db;   /* zero-init == unprimed */

int headphone_raw(void)
{
    uint32_t v = mmio_read32(HEADPHONE_DETECT_ADDR);
    int set = (v & HEADPHONE_DETECT_BIT) != 0u;
#if HEADPHONE_DETECT_ACTIVE_LOW
    return !set;
#else
    return set;
#endif
}

int headphone_pin_cfg(void)
{
    uint32_t en = mmio_read32(GPIO_QUAD_AD + GPIO_GRP_ENABLE);
    uint32_t oe = mmio_read32(GPIO_QUAD_AD + GPIO_GRP_OUTPUT_EN);
    int cfg = 0;

    if (en & HEADPHONE_DETECT_BIT) {
        cfg |= HEADPHONE_PIN_ENABLED;
    }
    if (oe & HEADPHONE_DETECT_BIT) {
        cfg |= HEADPHONE_PIN_OUTPUT;
    }
    return cfg;
}

void headphone_reset(void)
{
    headphone_debounce_reset(&s_db);
}

int hal_headphones_present(void)
{
#if HEADPHONE_PROBE
    headphone_probe_poll();
#endif
#if HEADPHONE_DETECT_TRUSTED
    /* Time first, then the pin, in separate statements: C leaves argument
     * evaluation order unspecified and the trace test asserts this order. */
    uint32_t now = mmio_read32(USEC_TIMER_ADDR);
    int raw = headphone_raw();
    return headphone_debounce_feed(&s_db, raw, now);
#else
    /* Line/polarity not yet confirmed on the device (headphone.h). Say so
     * rather than guess; the caller must treat -1 as "do nothing". No bus
     * traffic. */
    return -1;
#endif
}

/* ---------- Probe --------------------------------------------------------- */

#if HEADPHONE_PROBE

#include "uart.h"

/* The rest of the bank layout (10-headphone-jack.md, "GPIO bank layout"):
 * the A-D quad base and the two configuration groups the driver itself needs
 * are up with headphone_pin_cfg(); these are the ones only the probe walks. */
#define PROBE_QUAD_EH        0x6000D080u   /* derived midpoint; unconfirmed */
#define PROBE_QUAD_IL        0x6000D100u
#define PROBE_GRP_OUTPUT_VAL 0x20u
#define PROBE_GRP_INPUT_VAL  0x30u
#define PROBE_BITWISE        0x800u        /* masked-write shadow (02-lcd.md) */

/* Sample no faster than this (a pin oscillating at kHz must not become
 * kHz of UART), and stop for good after this many change events — a session
 * needs six. Both are the "cannot spam" guarantee the doc promises. */
#define PROBE_MIN_INTERVAL_US  20000u
#define PROBE_EVENT_BUDGET     400u

/* Twelve 8-bit ports packed into three words (A in bits 7:0 of the first
 * word, B in 15:8, ...). Scalars, not an array: bss is at 82% of budget and
 * this is compiled out of shipping images anyway. */
static uint32_t s_pr_ad, s_pr_eh, s_pr_il;
static uint32_t s_pr_last_us;
static uint16_t s_pr_events;
static uint8_t  s_pr_started;
static uint8_t  s_pr_exhausted;

static uint32_t probe_quad_base(int q)
{
    return q == 0 ? GPIO_QUAD_AD : q == 1 ? PROBE_QUAD_EH : PROBE_QUAD_IL;
}

static uint32_t probe_read_port(int q, int p, uint32_t group)
{
    return mmio_read32(probe_quad_base(q) + group + (uint32_t)p * 4u) & 0xFFu;
}

static uint32_t probe_pack_inputs(int q)
{
    uint32_t w = 0;
    for (int p = 0; p < 4; p++) {
        w |= probe_read_port(q, p, PROBE_GRP_INPUT_VAL) << (p * 8);
    }
    return w;
}

static void probe_put_hex8(uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    uart_putc(hex[(v >> 4) & 0xFu]);
    uart_putc(hex[v & 0xFu]);
}

static void probe_put_dec(uint32_t v)
{
    char b[11];
    int n = 0;
    do {
        b[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0u);
    while (n > 0) {
        uart_putc(b[--n]);
    }
}

/* One line per port, once: the configuration the boot ROM left behind. This
 * is what tells us, if nothing ever flips, whether the candidate pin was even
 * a GPIO input. */
static void probe_dump_config(void)
{
    for (int q = 0; q < 3; q++) {
        for (int p = 0; p < 4; p++) {
            uart_puts("core: hpprobe cfg ");
            uart_putc((char)('A' + q * 4 + p));
            uart_puts(" en=");  probe_put_hex8(probe_read_port(q, p, GPIO_GRP_ENABLE));
            uart_puts(" oe=");  probe_put_hex8(probe_read_port(q, p, GPIO_GRP_OUTPUT_EN));
            uart_puts(" ov=");  probe_put_hex8(probe_read_port(q, p, PROBE_GRP_OUTPUT_VAL));
            uart_puts(" in=");  probe_put_hex8(probe_read_port(q, p, PROBE_GRP_INPUT_VAL));
            uart_putc('\n');
        }
    }
}

/* If the ROM did not leave the candidate configured as a GPIO input, make it
 * one — through the masked-write shadow so no other bit of port A is touched —
 * and say so. Done only in probe builds: the shipping driver never reconfigures
 * pins it has not been proven to own. */
static void probe_force_candidate_input(void)
{
    uint32_t en = probe_read_port(0, 0, GPIO_GRP_ENABLE);
    uint32_t oe = probe_read_port(0, 0, GPIO_GRP_OUTPUT_EN);
    if ((en & HEADPHONE_DETECT_BIT) && !(oe & HEADPHONE_DETECT_BIT)) {
        return;
    }
    mmio_write32(GPIO_QUAD_AD + GPIO_GRP_OUTPUT_EN + PROBE_BITWISE,
                 HEADPHONE_DETECT_BIT << 8);                        /* clear */
    mmio_write32(GPIO_QUAD_AD + GPIO_GRP_ENABLE + PROBE_BITWISE,
                 (HEADPHONE_DETECT_BIT << 8) | HEADPHONE_DETECT_BIT); /* set */
    uart_puts("core: hpprobe A7 forced to GPIO input (was en=");
    probe_put_hex8(en);
    uart_puts(" oe=");
    probe_put_hex8(oe);
    uart_puts(")\n");
}

static void probe_print_snapshot(uint32_t now, uint32_t ad, uint32_t eh,
                                 uint32_t il)
{
    uart_puts("core: hpprobe t=");
    probe_put_dec(now);
    for (int i = 0; i < 12; i++) {
        uint32_t w = i < 4 ? ad : i < 8 ? eh : il;
        uart_putc(' ');
        uart_putc((char)('A' + i));
        uart_putc('=');
        probe_put_hex8((w >> ((i & 3) * 8)) & 0xFFu);
    }
    uart_putc('\n');
}

static void probe_print_diffs(int q, uint32_t old, uint32_t now)
{
    uint32_t x = old ^ now;
    for (int i = 0; i < 32; i++) {
        if (!(x & (1u << i))) {
            continue;
        }
        uart_puts("core: hpprobe diff ");
        uart_putc((char)('A' + q * 4 + i / 8));
        uart_puts(" bit");
        uart_putc((char)('0' + i % 8));
        uart_putc(' ');
        uart_putc((old >> i) & 1u ? '1' : '0');
        uart_puts("->");
        uart_putc((now >> i) & 1u ? '1' : '0');
        uart_putc('\n');
    }
}

void headphone_probe_poll(void)
{
    uint32_t now = mmio_read32(USEC_TIMER_ADDR);

    if (!s_pr_started) {
        s_pr_started = 1;
        probe_dump_config();
        probe_force_candidate_input();
        s_pr_ad = probe_pack_inputs(0);
        s_pr_eh = probe_pack_inputs(1);
        s_pr_il = probe_pack_inputs(2);
        s_pr_last_us = now;
        probe_print_snapshot(now, s_pr_ad, s_pr_eh, s_pr_il);
        return;
    }
    if (s_pr_exhausted ||
        (uint32_t)(now - s_pr_last_us) < PROBE_MIN_INTERVAL_US) {
        return;
    }
    s_pr_last_us = now;

    uint32_t ad = probe_pack_inputs(0);
    uint32_t eh = probe_pack_inputs(1);
    uint32_t il = probe_pack_inputs(2);
    if (ad == s_pr_ad && eh == s_pr_eh && il == s_pr_il) {
        return;
    }
    probe_print_snapshot(now, ad, eh, il);
    probe_print_diffs(0, s_pr_ad, ad);
    probe_print_diffs(1, s_pr_eh, eh);
    probe_print_diffs(2, s_pr_il, il);
    s_pr_ad = ad;
    s_pr_eh = eh;
    s_pr_il = il;

    if (++s_pr_events >= PROBE_EVENT_BUDGET) {
        s_pr_exhausted = 1;
        uart_puts("core: hpprobe event budget exhausted; silent from here\n");
    }
}

#endif /* HEADPHONE_PROBE */
