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

/* Decode/quadrature state, carried across samples. Shared by the polled
 * path (clickwheel_poll) and the tick sampler (clickwheel_service) — in a
 * given build only one drives the decode, so there is no interleaving. */
static uint8_t  s_pos_old;    /* last absolute position 0..0x5F           */
static bool     s_have_pos;   /* has s_pos_old been seeded since touch?    */
static uint8_t  s_btn_prev;   /* last mapped WHEEL_BTN_* set (edge basis)  */
static bool     s_hold_old;   /* last hold state, for edge detection       */

/*
 * Latch shared between the tick sampler (writer, ISR context) and the main
 * loop drain (reader). SPSC: the sampler only ever runs in the timer ISR,
 * so it cannot be preempted by the main loop; the drain masks IRQs for the
 * snapshot-and-clear so the sampler cannot run mid-read. That makes plain
 * statics sufficient — no atomics — but they are volatile so the compiler
 * reloads them across the critical-section boundary.
 */
static volatile uint8_t s_pending_btn;   /* OR of button-DOWN edges         */
static volatile int8_t  s_accum_delta;   /* accumulated, sat. wheel motion   */
static volatile bool    s_touched;       /* last-sampled finger-touch state  */
static volatile bool    s_hold_edge;     /* a hold transition is pending     */
static volatile bool    s_hold_state;    /* hold state to report on that edge*/
static volatile bool    s_armed;         /* service() is a no-op until init  */

/*
 * Tiny nestable critical section, mirroring kernel/irq.h
 * arch_irq_save/restore but kept local so this driver stays include-clean
 * (hal/hw is not on the kernel include path). The core-mask asm is ARMv4T
 * mrs/orr|bic/msr; it is compiled out under -DMMIO_MOCK so the host decode
 * test builds and runs unchanged (no IRQs to mask on the host).
 */
static inline uint32_t cw_irq_save(void)
{
#ifndef MMIO_MOCK
    uint32_t cpsr, tmp;
    __asm__ volatile(
        "mrs %0, cpsr\n\t"
        "orr %1, %0, %2\n\t"
        "msr cpsr_c, %1\n\t"
        : "=&r"(cpsr), "=&r"(tmp)
        : "i"(CPSR_I_BIT)
        : "cc", "memory");
    return cpsr & CPSR_I_BIT;   /* prior I-bit: 0 == IRQs were enabled */
#else
    return 0;
#endif
}

static inline void cw_irq_restore(uint32_t saved)
{
#ifndef MMIO_MOCK
    uint32_t cpsr, tmp;
    __asm__ volatile(
        "mrs %0, cpsr\n\t"
        "bic %1, %0, %3\n\t"
        "orr %1, %1, %2\n\t"
        "msr cpsr_c, %1\n\t"
        : "=&r"(cpsr), "=&r"(tmp)
        : "r"(saved), "i"(CPSR_I_BIT)
        : "cc", "memory");
#else
    (void)saved;
#endif
}

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
    s_btn_prev = 0;
    s_hold_old = clickwheel_hold();
    /* Clear the tick-sampler latch and arm it. Until this runs, a timer
     * tick that fires during boot (timer_init precedes clickwheel_init)
     * must not touch the OPTO block, so clickwheel_service() bails on
     * !s_armed. */
    s_pending_btn = 0;
    s_accum_delta = 0;
    s_touched     = false;
    s_hold_edge   = false;
    s_hold_state  = s_hold_old;
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

    s_armed = true;              /* the tick sampler may run from here on */
}

/* Decoded one status word, shared by clickwheel_poll and clickwheel_service. */
typedef struct {
    uint8_t btn_map;      /* WHEEL_BTN_* set currently down                 */
    uint8_t down_edges;   /* buttons that transitioned up->down this sample */
    int     delta;        /* sensitivity-gated wheel step (0 if none)       */
    bool    touched;      /* finger on the wheel this sample                */
    bool    changed;      /* button set changed OR wheel moved              */
} cw_sample_t;

/*
 * Read + acknowledge one status word and decode it (assumes NOT held; the
 * callers handle the hold edge/gating). Returns false — leaving *s
 * untouched, and the quadrature/button state unchanged — when the word
 * fails the header gate (stale or partially latched). Advances the shared
 * decode state (s_pos_old / s_have_pos / s_btn_prev) exactly as the old
 * inline poll did, so the polled and tick paths behave identically.
 */
static bool cw_read_sample(cw_sample_t *s)
{
    /* Read one status word. The header gate rejects stale or partially-
     * latched words (03-clickwheel.md, "Packet format"). */
    uint32_t st = mmio_read32(CLICKWHEEL_DATA_ADDR);
    /* Acknowledge the controller so it latches the NEXT sample. Without this
     * the OPTO block produces one packet and stalls with its IRQ asserted —
     * the wheel/buttons read dead (Hold, a plain GPIO read, still works). Ack
     * CTRLB + re-arm CTRLA every sample, valid data or not, mirroring what the
     * IRQ path would do (03-clickwheel.md, "Servicing"). */
    mmio_write32(CLICKWHEEL_CTRLB_ADDR,
                 mmio_read32(CLICKWHEEL_CTRLB_ADDR) | CW_CTRLB_ACK);
    mmio_write32(CLICKWHEEL_CTRLA_ADDR, CW_CTRLA_REARM);
    if ((st & CW_STAT_HEADER_MASK) != CW_STAT_HEADER_VALUE) {
        return false;
    }

    bool    touched = (st & CW_STAT_TOUCH) != 0;
    uint8_t pos     = (uint8_t)((st >> CW_POS_SHIFT) & CW_POS_MASK);

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

    uint8_t map = map_buttons(st);
    s->btn_map    = map;
    s->down_edges = (uint8_t)(map & ~s_btn_prev);   /* up->down transitions */
    s->delta      = delta;
    s->touched    = touched;
    s->changed    = (map != s_btn_prev) || (delta != 0);
    s_btn_prev    = map;
    return true;
}

/* Clamp-add into the int8 wheel accumulator (saturating, both directions). */
static int8_t cw_accum_add(int8_t acc, int delta)
{
    int sum = (int)acc + delta;
    if (sum >  127) { sum =  127; }
    if (sum < -127) { sum = -127; }
    return (int8_t)sum;
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
        s_btn_prev = 0;
        ev->buttons     = 0;
        ev->wheel_delta = 0;
        ev->touched     = false;
        ev->hold        = hold;
        return true;
    }
    if (hold) {
        return false;
    }

    cw_sample_t s;
    if (!cw_read_sample(&s)) {
        return false;
    }
    /* Fire only on a real change so a static finger / fast scroll cannot
     * flood the caller with duplicate events. */
    if (!s.changed) {
        return false;
    }

    ev->buttons     = s.btn_map;
    ev->wheel_delta = (int8_t)s.delta;
    ev->touched     = s.touched;
    ev->hold        = false;
    return true;
}

void clickwheel_service(void)
{
    if (!s_armed) {
        return;              /* not brought up yet: leave the OPTO block alone */
    }

    /* Hold edge: gate the block and latch the transition. Clearing the
     * pending input mirrors clickwheel_poll's "hold edge carries no input".
     * s_hold_old is owned solely by the sampler (the main loop reads Hold via
     * clickwheel_hold(), which is an independent GPIO read). */
    bool hold = clickwheel_hold();
    if (hold != s_hold_old) {
        s_hold_old = hold;
        opto_power(!hold);
        s_have_pos = false;      /* reseed position on the next touch */
        s_btn_prev = 0;
        s_pending_btn = 0;
        s_accum_delta = 0;
        s_touched     = false;
        s_hold_state  = hold;
        s_hold_edge   = true;
        return;
    }
    if (hold) {
        return;                  /* wheel is dead while held */
    }

    cw_sample_t s;
    if (!cw_read_sample(&s)) {
        return;
    }

    /* Latch. The sampler runs only in the timer ISR, so it cannot be
     * preempted by the main-loop drain; no critical section is needed on
     * this side (the drain masks IRQs for its snapshot-and-clear). Record
     * button-DOWN edges (so a press that comes and goes between two drains
     * survives) and accumulate wheel motion. */
    s_pending_btn = (uint8_t)(s_pending_btn | s.down_edges);
    if (s.delta != 0) {
        s_accum_delta = cw_accum_add(s_accum_delta, s.delta);
    }
    s_touched = s.touched;
}

bool clickwheel_get_event(wheel_event_t *ev)
{
    if (ev == 0) {
        return false;
    }

    /* Snapshot-and-clear the latch with the sampler masked out, so the tick
     * cannot slip an edge in between the read and the clear (SPSC). */
    uint32_t f = cw_irq_save();
    bool    hold_edge  = s_hold_edge;
    bool    hold_state = s_hold_state;
    uint8_t btn        = s_pending_btn;
    int8_t  acc        = s_accum_delta;
    bool    touched    = s_touched;
    s_hold_edge   = false;
    s_pending_btn = 0;
    s_accum_delta = 0;
    cw_irq_restore(f);

    /* A hold edge takes precedence and carries no input (the sampler zeroed
     * the pending mask when it latched the edge). */
    if (hold_edge) {
        ev->buttons     = 0;
        ev->wheel_delta = 0;
        ev->touched     = false;
        ev->hold        = hold_state;
        return true;
    }

    if (btn == 0 && acc == 0) {
        return false;
    }
    ev->buttons     = btn;
    ev->wheel_delta = acc;
    ev->touched     = touched;
    ev->hold        = false;
    return true;
}
