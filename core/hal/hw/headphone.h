/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/headphone.h — headphone-jack insertion detect, and the GPIO
 * probe that confirms which pin it is.
 *
 * The 5G/5.5G jack has a mechanical switch that closes when a plug is seated,
 * read by the SoC as a plain GPIO input — the same house pattern as the hold
 * switch (GPIOA bit 5) and the charger-present bits (GPIOL). Everything about
 * the jack, including why no inline-button headset can ever be sensed on this
 * board, is in core/docs/hw/10-headphone-jack.md.
 *
 * WHICH PIN. Public documentation (the iPodLinux wiki GPIO table, whose port-L
 * row matches every charger/backlight pin this firmware already runs on) puts
 * "Headphone attached (input)" on GPIO port A bit 7. The polarity is inferred
 * from that table's notation and has NOT been observed on this device. Until
 * the pin has been READ on hardware, HEADPHONE_DETECT_TRUSTED stays 0 and
 * hal_headphones_present() answers -1 ("unknown") without touching the bus —
 * a wrong polarity would pause playback every time headphones are plugged IN,
 * which is worse than no feature. That reading sets the two knobs below
 * (TRUSTED, and ACTIVE_LOW if the levels come out inverted), nothing else.
 *
 * WHICH PROBE. The UART probe at the bottom of headphone.c is the thorough
 * one — all twelve ports, one diffable line per change — and it is unusable
 * on THIS device: there is no serial cable and the owner ruled one out
 * (2026-07-17). So the probe that actually gets run is on the screen: the
 * Settings > About footer draws headphone_raw() and headphone_pin_cfg() live,
 * in every build, trusted or not, and the bench is "open About, plug, unplug,
 * read the digit". The UART probe stays for a bench that has a cable — it is
 * the better instrument, since it watches all twelve ports at once. Both
 * procedures are in 10-headphone-jack.md, "Confirming it on the device".
 *
 * The register constants live here rather than in pp5022.h only because this
 * driver was written while other work owned that header; they follow the
 * bank layout the doc derives and belong there eventually.
 */

#ifndef CORE_HAL_HW_HEADPHONE_H
#define CORE_HAL_HW_HEADPHONE_H

#include <stdint.h>

/* ---------- The line (10-headphone-jack.md, "Which GPIO") ------------------ */

/* GPIOA_INPUT_VAL: A-D quad 0x6000D000, port A +0x00, INPUT_VAL group +0x30. */
#ifndef HEADPHONE_DETECT_ADDR
#define HEADPHONE_DETECT_ADDR        0x6000D030u
#endif
#ifndef HEADPHONE_DETECT_BIT
#define HEADPHONE_DETECT_BIT         0x80u   /* port A bit 7                  */
#endif
/* 0: bit set = plug seated (the table's "(input)" convention). 1: inverted. */
#ifndef HEADPHONE_DETECT_ACTIVE_LOW
#define HEADPHONE_DETECT_ACTIVE_LOW  0
#endif
/* 0 until the pin has been read on the device: line AND polarity. */
#ifndef HEADPHONE_DETECT_TRUSTED
#define HEADPHONE_DETECT_TRUSTED     0
#endif
/* 1 compiles the pin-finding UART probe in (never in a shipping image). */
#ifndef HEADPHONE_PROBE
#define HEADPHONE_PROBE              0
#endif

/* ---------- Debounce ------------------------------------------------------ */

/*
 * A new raw level must hold for this long before it is reported. 200 ms is
 * the window 07-usb.md already uses for the USB cable switch, ~50x a jack
 * switch's contact bounce, and long enough to ride out a plug being wiggled.
 * The latency is paid only on unplug (a pause landing 200 ms late), never on
 * insert, because the recommended policy does not auto-resume. Microseconds,
 * because the hw time source is the free-running 1 MHz USEC_TIMER.
 */
#define HEADPHONE_DEBOUNCE_US        200000u

/* Pure debouncer state. Zero-initialised == "no sample yet": the FIRST sample
 * primes `stable` without waiting, so a device booted with headphones in reads
 * present at once and never reports a phantom unplug at start-up. */
typedef struct {
    uint8_t  primed;       /* 1 once the first sample has set `stable`      */
    uint8_t  stable;       /* the debounced level (0/1)                     */
    uint8_t  cand_active;  /* 1 while a differing level is being timed      */
    uint8_t  cand;         /* the level being timed                         */
    uint32_t cand_since;   /* USEC_TIMER value when `cand` was first seen   */
} headphone_debounce_t;

void headphone_debounce_reset(headphone_debounce_t *d);

/* Feed one raw sample (0/1) taken at `now_us`; returns the debounced level.
 * Wrap-safe: elapsed time is unsigned 32-bit subtraction. */
int headphone_debounce_feed(headphone_debounce_t *d, int raw, uint32_t now_us);

/* ---------- Driver -------------------------------------------------------- */

/* Raw, un-debounced plug state from the register: 1 seated, 0 absent. One
 * 32-bit read — the same register the hold switch is read from every
 * main-loop pass. Exposed for the probe, the trace test and the About
 * footer's live JACK token; callers that want to ACT want
 * hal_headphones_present() (hal.h), which debounces this. */
int headphone_raw(void);

/* headphone_pin_cfg() bits: how the boot ROM left the detect pin. A pin that
 * is not an enabled, non-driven GPIO cannot report anything, and "the level
 * never changes" looks identical to "wrong pin" without this. */
#define HEADPHONE_PIN_ENABLED  0x1   /* GPIOA ENABLE bit 7: pin is a GPIO   */
#define HEADPHONE_PIN_OUTPUT   0x2   /* GPIOA OUTPUT_EN bit 7: driven out   */

/* The detect pin's configuration, as the bits above. Two 32-bit reads, no
 * writes: this driver never reconfigures a pin it has not been proven to own
 * (only the UART probe does, and only in probe builds). Called by the About
 * screen, which is the only place on this device that can show it. */
int headphone_pin_cfg(void);

/* Forget the debounce history (boot, and between test cases). */
void headphone_reset(void);

/*
 * The probe (compiled only with -DHEADPHONE_PROBE=1; see the doc, "Confirming
 * it on the device"). Snapshots all twelve GPIO input ports and prints, on the
 * SER0 UART, one diffable line per CHANGE plus one `diff` line per changed
 * bit. Silent while nothing changes; rate-limited; a hard line budget stops it
 * for good if a pin oscillates. hal_headphones_present() calls it, so wiring
 * that one poll into the main loop is the only call site the probe needs.
 */
#if HEADPHONE_PROBE
void headphone_probe_poll(void);
#endif

#endif /* CORE_HAL_HW_HEADPHONE_H */
