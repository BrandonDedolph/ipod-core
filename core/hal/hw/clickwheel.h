/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/clickwheel.h — PP5022 click wheel + face buttons + hold.
 *
 * See core/docs/hw/03-clickwheel.md. The capacitive wheel and all five
 * face buttons arrive as one 32-bit status word at CLICKWHEEL_DATA; the
 * hold switch is a plain GPIO. This driver is POLLED (no IRQ wiring) —
 * the wheel chip only updates every ~10 ms. Freestanding: stdint/stdbool
 * only, asm-free hot path, and all hardware access goes through the mmio.h
 * seam so the decode logic host-compiles for the unit test.
 *
 * TWO consumers of the same decode:
 *
 *   - clickwheel_service() is the SAMPLER. It is meant to run from the
 *     100 Hz timer tick (kernel/timer.c), so the wheel is drained every
 *     10 ms REGARDLESS of what the main loop is doing. This is the fix for
 *     "a face-button tap during a ~100 ms blocking disk read is lost": the
 *     tick keeps firing while the loop is blocked, so a brief press that
 *     lands entirely between two main-loop passes is still captured. The
 *     service routine records button-DOWN edges into a latch and
 *     accumulates wheel motion.
 *
 *   - clickwheel_get_event() is the DRAIN. The main loop calls it (in
 *     place of the old clickwheel_poll) to pull the latched button-down
 *     edges + accumulated wheel delta since the last call. It is
 *     SPSC-safe against the tick sampler via a tiny IRQ-masked critical
 *     section (mirrors kernel/irq.h arch_irq_save/restore; the core-mask
 *     asm is compiled out under -DMMIO_MOCK for the host test).
 *
 * clickwheel_poll() is retained as the standalone single-sample decode
 * (sample + decode + report in one call) for callers/tests that are not
 * driven from the tick.
 */
#ifndef CORE_HAL_HW_CLICKWHEEL_H
#define CORE_HAL_HW_CLICKWHEEL_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Compact button bitmask returned in wheel_event_t.buttons. These are
 * driver-local codes (adjacent bits so the UI can switch on them
 * cleanly) — distinct from the raw CW_BTN_* status-word bit positions in
 * pp5022.h. The set reported is the currently-DOWN buttons for the
 * sample that produced the event.
 */
enum {
    WHEEL_BTN_SELECT = 1u << 0,
    WHEEL_BTN_MENU   = 1u << 1,
    WHEEL_BTN_PLAY   = 1u << 2,
    WHEEL_BTN_LEFT   = 1u << 3,   /* Prev / Rewind */
    WHEEL_BTN_RIGHT  = 1u << 4,   /* Next / FFwd   */
};

typedef struct {
    uint8_t  buttons;      /* WHEEL_BTN_* bitmask of currently-down buttons */
    int8_t   wheel_delta;  /* signed, sensitivity-gated step count          */
                           /*   (+ = clockwise, - = counter-clockwise; 0    */
                           /*    when motion is below the sensitivity gate)  */
    bool     touched;      /* finger on the wheel for this sample           */
    bool     hold;         /* hold switch engaged (this is a hold-edge evt) */
} wheel_event_t;

/*
 * Power up, reset, and configure the wheel block, then sample the hold
 * switch once to seed edge detection. If hold is already engaged the
 * block is left gated off (matching hardware). Idempotent enough to call
 * once at bring-up.
 */
void clickwheel_init(void);

/*
 * Poll one hardware sample (non-blocking, bounded). Returns true and
 * fills *ev when there is something NEW to report:
 *
 *   - a hold-switch edge (ev->hold carries the new state; other fields
 *     are zero),
 *   - a button set that differs from the previous sample, or
 *   - wheel motion that crossed the sensitivity threshold
 *     (ev->wheel_delta non-zero).
 *
 * Returns false when the status word is stale/invalid (fails the header
 * gate) or nothing changed. Safe to call every UI frame or from a tick.
 */
bool clickwheel_poll(wheel_event_t *ev);

/*
 * SAMPLER — call from the 100 Hz timer tick (kernel/timer.c). Bounded and
 * non-blocking (one status-word read + ack + decode, same work as
 * clickwheel_poll). Drains the OPTO packet so the controller latches the
 * next sample, decodes it, and:
 *
 *   - ORs any button-DOWN edge (up->down transition) into a latch, so a
 *     press that comes and goes entirely between two main-loop drains is
 *     remembered;
 *   - accumulates sensitivity-gated wheel motion into a running delta;
 *   - tracks the finger-touch state and hold-switch edges (gating the
 *     OPTO block on a hold edge exactly as clickwheel_poll does).
 *
 * No-op until clickwheel_init() has armed the driver, so it is safe for
 * the tick ISR to call it before bring-up finishes. Writes only the latch;
 * the drain side (clickwheel_get_event) owns the concurrency.
 */
void clickwheel_service(void);

/*
 * DRAIN — the main-loop counterpart to clickwheel_service(). Atomically
 * takes and clears the latch filled by the tick sampler. Returns true and
 * fills *ev when there is something to report:
 *
 *   - a hold-switch edge (ev->hold carries the new state; other fields 0),
 *   - one or more latched button-DOWN edges (ev->buttons bitmask), and/or
 *   - accumulated wheel motion (ev->wheel_delta, signed, clamped to int8).
 *
 * Returns false when nothing has been latched since the last call. Drop-in
 * for clickwheel_poll in the UI loop: same wheel_event_t semantics, but no
 * event is ever lost to a busy main loop because the tick did the sampling.
 */
bool clickwheel_get_event(wheel_event_t *ev);

/* True while the hold switch is engaged (a single cheap GPIO read). */
bool clickwheel_hold(void);

/* Current held-button set (WHEEL_BTN_* bitmask) from the last tick sample —
 * the LIVE state, vs clickwheel_get_event's latched down-edges. For long-press
 * detection (e.g. hold PLAY to power off). 0 while held/before bring-up. */
uint8_t clickwheel_buttons(void);

#endif /* CORE_HAL_HW_CLICKWHEEL_H */
