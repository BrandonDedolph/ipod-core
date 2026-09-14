/* SPDX-License-Identifier: Apache-2.0 */
/*
 * kernel/timer.c — TIMER1 100 Hz system tick driver (PP5022).
 *
 * Implements the arm/ack sequence from core/docs/hw/01-soc-pp5022.md
 * ("Timers and the system tick"). The 1 MHz timer clock makes one count
 * one microsecond, so a HZ-hertz tick has a reload of (TIMER_FREQ/HZ)-1
 * microseconds; bit 30 makes the counter self-reload, so the ISR only
 * has to acknowledge (a read of TIMER1_VAL) and never reprograms CFG.
 *
 * Freestanding-clean: no libc, fixed-width types from <stdint.h> only,
 * hardware reached solely through hal/hw/mmio.h so the register grammar
 * is trace-testable host-side under -DMMIO_MOCK.
 */

#include "timer.h"
#include "sched.h"
#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hw/clickwheel.h"

/* Backlight housekeeping hook, run from the tick. Weak: the host timer test
 * links this file without hal/hw/backlight.c, and the call is guarded. */
__attribute__((weak)) void backlight_service(void);

/* Monotonic tick counter. In .bss, zeroed by crt0. volatile: written by
 * the ISR, read by mainline code (current_tick / sleep_ms). */
static volatile uint32_t g_tick;

/*
 * TICK RECONCILIATION against the free-running 1 MHz USEC_TIMER.
 *
 * TIMER1 self-reloads and the ISR acknowledges with a single TIMER1_VAL read,
 * so N elapsed periods collapse into ONE g_tick++ — the timer has no
 * accumulating count for us to read back. That is not hypothetical here: the
 * LCD present masks IRQs for longer than the 10 ms period while it streams a
 * frame, so every full-frame repaint loses at least one tick. Two consequences,
 * both real:
 *
 *   - sleep_ms() systematically over-sleeps. The 60 ms disk-retry backoff
 *     stretches without bound under repaint load, because the clock it is
 *     counting stops advancing while the screen is busy.
 *   - clickwheel_service() is skipped for exactly as long, i.e. input is
 *     sampled least often precisely when the UI is busiest.
 *
 * USEC_TIMER never stops and is not affected by the IRQ mask, so the ISR can
 * work out how many whole periods actually elapsed and advance g_tick by that
 * many. g_last_us keeps the sub-period remainder so there is no drift.
 */
#define TICK_PERIOD_US   (TIMER_FREQ / HZ)      /* 10000 us at HZ = 100 */

/* Ceiling on a single catch-up. A jump this large means the reference is
 * meaningless (first ISR after arm, or a stopped counter), not that a second
 * of ticks was really missed — advancing by a huge number would make every
 * outstanding sleep_ms() return instantly. */
#define TICK_MAX_CATCHUP 100u                   /* 1 second's worth */

static uint32_t g_last_us;      /* USEC_TIMER value at the last reconcile */
static int      g_have_us;      /* g_last_us seeded yet? */

/*
 * (Re)arm TIMER1 at `hz` interrupts per second. The same four documented
 * accesses whether it is the boot arm or a rate change: the disarm and the
 * TIMER1_VAL read clear whatever the previous period left latched, so a
 * change never delivers a stale IRQ at the old rate.
 *
 * THE TICK UNIT DOES NOT CHANGE WITH THE RATE. g_tick stays in HZ (10 ms)
 * units because the ISR reconciles against USEC_TIMER (above): at 10 Hz
 * each interrupt finds ~100 ms elapsed and advances the counter by ten.
 * current_tick() and sleep_ms() therefore keep their meaning at any rate,
 * only their granularity coarsens. What DOES change with the rate is how
 * often the ISR's per-interrupt work runs — clickwheel_service samples the
 * wheel once per interrupt, so at 10 Hz a press shorter than ~100 ms can
 * fall between samples. That is the trade the suspend loop makes on
 * purpose (kernel/main.c suspend_to_ram): it wants ten wakeups a second
 * instead of a hundred, and "hold any button" is the wake gesture.
 */
void timer_set_rate(uint32_t hz)
{
    if (hz == 0u || hz > TIMER_FREQ) {
        hz = HZ;
    }
    /* 1. Disarm: clear any stale enable/reload. */
    mmio_write32(TIMER1_CFG_ADDR, 0);
    /* 2. Clear a pending IRQ latched from a prior arm. */
    (void)mmio_read32(TIMER1_VAL_ADDR);
    /* 3. Arm periodic: enable + IRQ/reload + (TIMER_FREQ/hz)-1 us period. */
    mmio_write32(TIMER1_CFG_ADDR,
                 TIMER_CFG_ENABLE | TIMER_CFG_IRQEN | ((TIMER_FREQ / hz) - 1u));
    /* 4. Unmask TIMER1_IRQ (#0) in the CPU interrupt-enable register. */
    mmio_write32(CPU_INT_EN_ADDR, 1u << TIMER1_IRQ);
}

void timer_init(void)
{
    timer_set_rate(HZ);
}

void timer_tick_isr(void)
{
    /* Advance by however many whole tick periods have really elapsed, not by
     * one — see the reconciliation note above. The seeding read is done here
     * rather than in timer_init so the arm sequence stays exactly the four
     * documented accesses. */
    uint32_t now = mmio_read32(USEC_TIMER_ADDR);
    uint32_t ticks;
    if (!g_have_us) {
        g_have_us = 1;
        g_last_us = now;
        ticks = 1;
    } else {
        uint32_t elapsed = now - g_last_us;      /* wrap-safe */
        if (elapsed >= TICK_PERIOD_US) {
            ticks = elapsed / TICK_PERIOD_US;
            if (ticks > TICK_MAX_CATCHUP) {
                ticks = TICK_MAX_CATCHUP;
                g_last_us = now;                 /* reference was stale: resync */
            } else {
                /* Keep the remainder so the tick clock does not drift. */
                g_last_us += ticks * TICK_PERIOD_US;
            }
        } else {
            /* The IRQ fired slightly early against the microsecond counter
             * (jitter, or a counter that does not advance at all — the host
             * mock). It is still a real tick: count it and resync. */
            ticks = 1;
            g_last_us = now;
        }
    }
    g_tick += ticks;
    (void)mmio_read32(TIMER1_VAL_ADDR);   /* ack: clears the pending IRQ */

    /* Sample the click wheel every tick (10 ms). Running this from the tick
     * — not the main loop — is what keeps a quick face-button tap from being
     * lost when the main loop is blocked in a ~100 ms synchronous disk read:
     * the tick still fires, so clickwheel_service() latches the press for the
     * loop to drain later. Bounded work (one OPTO packet); a no-op until
     * clickwheel_init() arms it. */
    clickwheel_service();

    /* Backlight screen-off power policy (drop the charge pump once the panel
     * has been dark for a few seconds). At most one bus write per call.
     * Declared weak so the host timer test — which links kernel/timer.c
     * without the hal/hw backlight driver — still resolves.
     *
     * Called once per RECONCILED tick, not once per interrupt: it counts
     * calls as 10 ms (BL_OFF_GRACE_TICKS, backlight.c), so at the suspend
     * loop's 10 Hz rate a single call per interrupt would stretch its ~3 s
     * grace to ~30 s of charge pump — and a repaint that masked IRQs for
     * 50 ms used to cost it 40 ms of grace too. Cheap: it is a counter
     * compare until the one write, and returns at once after it. */
    if (backlight_service) {
        for (uint32_t i = 0; i < ticks; i++) {
            backlight_service();
        }
    }
}

uint32_t current_tick(void)
{
    return g_tick;
}

/*
 * Halt this core until its countdown expires. PROC_WAIT_CNT self-wakes on
 * the countdown, so unlike PROC_SLEEP it is safe even if no interrupt is
 * pending (01-soc-pp5022.md, "Sleep / wake"). Three NOPs after the write per
 * the doc's pipeline rule.
 *
 * WHY 200 us AND NOT 1 ms. sleep_ms is reachable while audio is playing:
 * player.c backs off with sleep_ms(60) after a failed disk read, mid-track.
 * Playback is a single-shot DMA transfer re-kicked from its completion ISR,
 * with no chained descriptor; only the 16-frame I2S FIFO (~363 us at 44.1 kHz)
 * covers a late re-kick. A halt that outlasts that window is an underrun.
 * So no single halt may be longer than the FIFO, and kernel/main.c already
 * sizes its own idle halts at 200 us on exactly this basis. This used to be
 * 1 ms — one transient PIO error mid-track became 60 back-to-back 1 ms
 * halts, and any DMA completion landing inside one was serviced up to 1 ms
 * late, an underrun stacked on top of the disk hiccup. The total sleep is
 * unchanged (it is bounded by the tick count, not the halt length); only
 * the granularity is.
 *
 * OPEN QUESTION — does PROC_WAIT_CNT wake early on an interrupt? The doc
 * table says only "Sleep until countdown" for PROC_WAIT_CNT and reserves
 * "Sleep until interrupt" for PROC_SLEEP; it promises nothing about an
 * interrupt cutting a countdown short, and an earlier version of this
 * comment asserted that it does. That assertion was not backed by the doc
 * and is not relied on here: 200 us is inside the FIFO deadline whether the
 * core wakes on the DMA IRQ (best case: latency ~0) or sleeps the full
 * countdown (worst case: latency 200 us, deadline 363 us). It is on the
 * device-test list — measure DMA-ISR latency under sleep_ms(60) with a
 * scope or the USEC_TIMER stamp — and until then nothing in this file may
 * be sized on the optimistic reading.
 *
 * 200 fits the 8-bit count field; PROC_CNT_USEC selects the 1 us unit.
 */
#define SLEEP_HALT_US  200u

/*
 * The count is the LOW 8 BITS of CPU_CTL (01-soc-pp5022.md, "Sleep / wake":
 * "[7:0] Read: cycles remaining; write: cycles to skip"). A value that does
 * not fit is TRUNCATED SILENTLY by the hardware, not rejected — writing 1000
 * here would program 1000 & 0xFF = 232 us, a halt that still looks plausible
 * and still passes a deadline check, so nothing downstream would notice the
 * number was not the one written. Catch it at compile time instead.
 */
_Static_assert(SLEEP_HALT_US <= 0xFFu,
               "SLEEP_HALT_US must fit the 8-bit CPU_CTL count field");

static void tick_halt(void)
{
    mmio_write32(CPU_CTL_ADDR, PROC_WAIT_CNT | PROC_CNT_USEC | SLEEP_HALT_US);
#ifndef MMIO_MOCK
    __asm__ volatile("nop\n\tnop\n\tnop");
#endif
}

void sleep_ms(uint32_t ms)
{
    /* PRECONDITION: IRQs must be enabled at the core — the wait ends only
     * when the timer ISR advances the tick, so calling this inside an
     * arch_irq_disable() critical section spins forever. Cooperative:
     * other tasks run while it waits.
     *
     * Round up to whole ticks (ceil); the 64-bit intermediate keeps a
     * large ms from overflowing the multiply. */
    uint32_t ticks = (uint32_t)(((uint64_t)ms * HZ + 999u) / 1000u);
    uint32_t start = current_tick();

    /* Unsigned subtraction is wrap-safe across the UINT32_MAX boundary. */
    while ((current_tick() - start) < ticks) {
        /*
         * HALT, then yield. On the shipping path the scheduler never actually
         * runs — run_ui() never returns, so sched_start() is only reached on
         * the no-LCD / mount-failure fallback — which made sched_yield() an
         * immediate return and this loop a flat-out 80 MHz busy-spin for the
         * whole delay. Every sleep_ms burned full power for nothing.
         *
         * Halting first costs at most SLEEP_HALT_US of scheduling latency when
         * a scheduler IS running (the only task there is an idle task that
         * halts anyway) and turns the shipping path into an actual sleep. We
         * cannot ask the scheduler whether it is live — kernel/sched.c is not
         * ours to change and its state is private — so doing both
         * unconditionally is the correct, non-invasive answer.
         */
        tick_halt();
        sched_yield();
    }
}

#ifdef MMIO_MOCK
/*
 * Host-test-only hook. Seeds the tick counter so a test can exercise
 * paths (e.g. the unsigned wrap in sleep_ms) that would otherwise
 * require billions of real ticks to reach. Compiled out of the
 * freestanding firmware image entirely — the arm-none-eabi build never
 * defines MMIO_MOCK, so g_tick stays a pure ISR-owned counter there.
 */
void timer_test_set_tick(uint32_t v)
{
    g_tick    = v;
    g_have_us = 0;      /* re-seed the USEC reference for the next ISR */
}
#endif
