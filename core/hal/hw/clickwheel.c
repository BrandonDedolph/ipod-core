/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/clickwheel.c — PP5022 click wheel + face buttons + hold,
 * polled path.
 *
 * Implements core/docs/hw/03-clickwheel.md. One 32-bit status word at
 * CLICKWHEEL_DATA carries the wheel (absolute position + touch) and all
 * five face buttons; the hold switch is GPIOA_INPUT_VAL bit 5. Phase 1
 * is POLLED — the wheel chip samples at ~10 ms, so the interrupt facts
 * (I2C_IRQ #40, the CTRLB ack / CTRLA re-arm) are recorded in pp5022.h
 * for a later IRQ path but not exercised here.
 *
 * Relative wheel motion is derived in software (the hardware only reports
 * absolute position): difference successive positions, wrap at half a
 * rotation, and gate at CW_WHEEL_SENSITIVITY. Sub-threshold motion
 * accumulates (we advance the reference position only when a tick fires),
 * so slow scrolling still eventually steps. All hardware access goes
 * through the mmio.h seam, so the decode is host-testable under
 * -DMMIO_MOCK with synthetic status words.
 */

#include "pp5022.h"
#include "mmio.h"
#include "clickwheel.h"

/*
 * Reset-hold spin after asserting DEV_OPTO reset. Same bounded-spin
 * posture as i2c.c / uart.c: a fixed iteration count rather than a real
 * timer (the block only needs a few microseconds to settle, and this
 * keeps the driver freestanding + host-compilable).
 */
#define CW_RESET_HOLD_SPIN (1u << 12)

/* Decode/quadrature state, carried across polls. */
static uint8_t  s_pos_old;    /* last absolute position 0..0x5F           */
static bool     s_have_pos;   /* has s_pos_old been seeded since touch?    */
static uint16_t s_btn_old;    /* last raw button bits (CW_BTN_ALL subset)  */
static bool     s_hold_old;   /* last hold state, for edge detection       */

/*
 * Power/clock-gate the wheel (OPTO) block on or off. On is the full
 * bring-up: clock-gate + reset pulse + button-latch enable + the two
 * config writes (03-clickwheel.md, "Initialization"). Off just drops the
 * DEV_OPTO clock — what makes the wheel feel dead while hold is engaged.
 */
static void opto_power(bool on)
{
    if (on) {
        mmio_write32(DEV_EN_ADDR, mmio_read32(DEV_EN_ADDR) | DEV_OPTO);
        mmio_write32(DEV_RS_ADDR, mmio_read32(DEV_RS_ADDR) | DEV_OPTO);
        for (volatile uint32_t i = 0; i < CW_RESET_HOLD_SPIN; i++) {
            /* hold reset (~udelay(5)) */
        }
        mmio_write32(DEV_RS_ADDR, mmio_read32(DEV_RS_ADDR) & ~DEV_OPTO);
        mmio_write32(DEV_INIT1_ADDR,
                     mmio_read32(DEV_INIT1_ADDR) | INIT_BUTTONS);
        mmio_write32(CLICKWHEEL_CTRLA_ADDR, CW_CTRLA_INIT);
        mmio_write32(CLICKWHEEL_CTRLB_ADDR, CW_CTRLB_CLEAR);
    } else {
        mmio_write32(DEV_EN_ADDR, mmio_read32(DEV_EN_ADDR) & ~DEV_OPTO);
    }
}

/* Map the raw status-word button bits to the compact WHEEL_BTN_* set. */
static uint8_t map_buttons(uint32_t st)
{
    uint8_t b = 0;
    if (st & CW_BTN_SELECT) { b |= WHEEL_BTN_SELECT; }
    if (st & CW_BTN_MENU)   { b |= WHEEL_BTN_MENU;   }
    if (st & CW_BTN_PLAY)   { b |= WHEEL_BTN_PLAY;   }
    if (st & CW_BTN_LEFT)   { b |= WHEEL_BTN_LEFT;   }
    if (st & CW_BTN_RIGHT)  { b |= WHEEL_BTN_RIGHT;  }
    return b;
}

bool clickwheel_hold(void)
{
    /* Active-low: bit clear == held (03-clickwheel.md, "Hold switch"). */
    return (mmio_read32(GPIOA_INPUT_VAL_ADDR) & CW_HOLD_BIT) == 0;
}

void clickwheel_init(void)
{
    s_have_pos = false;
    s_btn_old  = 0;
    s_hold_old = clickwheel_hold();
    /* Only power the block if we are not already held; if held, leave it
     * gated (poll() brings it back up on the release edge). Polled path:
     * the wheel IRQ stays masked — no CPU_HI_INT_EN write. */
    if (!s_hold_old) {
        opto_power(true);
    }
    /* Hard-mask the wheel's shared I2C IRQ (#40 = high-bank bit 8) at the
     * core. We poll, so it must never reach the CPU — and poll() re-arms
     * CTRLA (which re-arms that IRQ output) every call, so a button/wheel edge
     * would otherwise assert #40 into a dispatcher that can't ack it and
     * livelock the core. Masking it here makes the polled design airtight;
     * irq_dispatch's reactive high-bank mask is the backstop. */
    mmio_write32(CPU_HI_INT_DIS_ADDR, 1u << 8);
}

bool clickwheel_poll(wheel_event_t *ev)
{
    if (ev == 0) {
        return false;
    }

    /* Hold edge: gate the block and report the transition. While held,
     * the wheel is dead so we report nothing further. */
    bool hold = clickwheel_hold();
    if (hold != s_hold_old) {
        s_hold_old = hold;
        opto_power(!hold);
        s_have_pos = false;      /* reseed position on the next touch */
        s_btn_old  = 0;
        ev->buttons     = 0;
        ev->wheel_delta = 0;
        ev->touched     = false;
        ev->hold        = hold;
        return true;
    }
    if (hold) {
        return false;
    }

    /* Read + validate one status word. The header gate rejects stale or
     * partially-latched words (03-clickwheel.md, "Packet format"). */
    uint32_t st = mmio_read32(CLICKWHEEL_DATA_ADDR);
    /* Acknowledge the controller so it latches the NEXT sample. Without this
     * the OPTO block produces one packet and stalls with its IRQ asserted —
     * the wheel/buttons read dead (Hold, a plain GPIO read, still works). Ack
     * CTRLB + re-arm CTRLA every poll, valid data or not, mirroring what the
     * IRQ path would do (03-clickwheel.md, "Servicing"). */
    mmio_write32(CLICKWHEEL_CTRLB_ADDR,
                 mmio_read32(CLICKWHEEL_CTRLB_ADDR) | CW_CTRLB_ACK);
    mmio_write32(CLICKWHEEL_CTRLA_ADDR, CW_CTRLA_REARM);
    if ((st & CW_STAT_HEADER_MASK) != CW_STAT_HEADER_VALUE) {
        return false;
    }

    bool     touched = (st & CW_STAT_TOUCH) != 0;
    uint8_t  pos     = (uint8_t)((st >> CW_POS_SHIFT) & CW_POS_MASK);
    uint16_t btn     = (uint16_t)(st & CW_BTN_ALL);

    /* Software quadrature: differenced absolute position, wrapped at half
     * a rotation, gated at the sensitivity threshold. */
    int delta = 0;
    if (touched) {
        if (!s_have_pos) {
            s_pos_old  = pos;    /* first touched sample: seed, no motion */
            s_have_pos = true;
        }
        int d = (int)pos - (int)s_pos_old;
        if (d < -(CW_CLICKS_PER_ROT / 2)) { d += CW_CLICKS_PER_ROT; }
        if (d >  (CW_CLICKS_PER_ROT / 2)) { d -= CW_CLICKS_PER_ROT; }
        if (d >= CW_WHEEL_SENSITIVITY || d <= -CW_WHEEL_SENSITIVITY) {
            delta     = d;
            s_pos_old = pos;     /* advance reference only when a tick fires */
        }
        /* else: keep s_pos_old so sub-threshold motion accumulates. */
    } else {
        s_have_pos = false;      /* finger lifted: reseed on next touch */
    }

    /* Fire only on a real change so a static finger / fast scroll cannot
     * flood the caller with duplicate events. */
    bool changed = (btn != s_btn_old) || (delta != 0);
    s_btn_old = btn;
    if (!changed) {
        return false;
    }

    ev->buttons     = map_buttons(st);
    ev->wheel_delta = (int8_t)delta;
    ev->touched     = touched;
    ev->hold        = false;
    return true;
}
