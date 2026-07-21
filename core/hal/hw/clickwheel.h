/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/clickwheel.h — PP5022 click wheel + face buttons + hold.
 *
 * See core/docs/hw/03-clickwheel.md. The capacitive wheel and all five
 * face buttons arrive as one 32-bit status word at CLICKWHEEL_DATA; the
 * hold switch is a plain GPIO. This driver is POLLED (no IRQ wiring) —
 * the wheel chip only updates every ~10 ms, so a 10-20 ms poll cadence
 * from the UI loop / tick loses nothing. Freestanding: stdint/stdbool
 * only, asm-free, and all hardware access goes through the mmio.h seam so
 * the decode logic host-compiles for the unit test.
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

/* True while the hold switch is engaged (a single cheap GPIO read). */
bool clickwheel_hold(void);

#endif /* CORE_HAL_HW_CLICKWHEEL_H */
