/*
 * core/hal/hw/uart.c — SER0 debug UART driver (PP5022, polled TX).
 *
 * First register-touching driver in the tree. Implements the init
 * sequence from core/docs/hw/08-boot-dock.md ("UART debug" -> "Init
 * sequence (default 115200 8-N-1)"), verified against Rockbox
 * uart-pp.c / pp5020.h (2026-06-10), minus the IRQ/FIFO setup a
 * polled transmitter doesn't need.
 *
 * Freestanding-clean: no libc, fixed-width types from <stdint.h> only.
 */

#include "pp5022.h"
#include "mmio.h"
#include "irqlock.h"   /* DEV_EN RMW vs the timer ISR, for the suspend gate */
#include "uart.h"

/*
 * Upper bound on the TX-ready poll so a dead UART can't hang the
 * kernel. One character at 115200 takes ~87 us = ~2100 CPU cycles at
 * the 24 MHz boot clock — a few hundred trips around this loop — so
 * 1<<16 iterations is orders of magnitude past "working hardware".
 * If we ever time out (e.g. SER0 unclocked or no cable attached),
 * the write is attempted anyway and the byte is silently lost; the
 * debug channel degrades, the firmware keeps running.
 */
#define UART_TX_SPIN_LIMIT  (1u << 16)

/*
 * How long DEV_RS holds SER0 in reset. The reference holds it for one
 * ~10 ms scheduler tick (08-boot-dock.md, "Init sequence").
 */
#define UART_RESET_HOLD_US  10000u

/*
 * Trip cap on the reset-hold wait. USEC_TIMER is a free-running 1 MHz
 * counter clocked independently of the PLL and the cache (01-soc-pp5022.md,
 * "Timers"), so on silicon the elapsed test always terminates; the cap is
 * the HAL's "no unbounded loops, ever" rule. 1<<20 trips is chosen so that
 * the failure mode — a dead counter — degrades to EXACTLY the iteration-
 * counted loop this wait replaced, i.e. no worse than the firmware that
 * already booted on the device. Under the host mock bus the counter only
 * advances when the test scripts it, so the cap is small there: large
 * enough that a scripted wait exits on TIME, not on the guard, and small
 * enough that a stuck-counter test cannot flood the recording bus's
 * fixed-capacity event log. Same MMIO_MOCK split as wm8758_settle_us and
 * the PLL-lock spin in kernel/clock.c.
 */
#ifdef MMIO_MOCK
#define UART_RESET_GUARD_TRIPS  64u
#else
#define UART_RESET_GUARD_TRIPS  (1u << 20)
#endif

/*
 * Wait `us` microseconds on USEC_TIMER, bounded. Wrap-safe: the counter
 * wraps every ~71.6 min, so this compares an unsigned ELAPSED difference,
 * never "now >= deadline". At boot the counter is nowhere near the wrap,
 * but a warm reset (Select+Play) or a long-running unit reusing this
 * helper later should not have to care, so it is written correctly now.
 */
static void uart_wait_us(uint32_t us)
{
    uint32_t t0    = mmio_read32(USEC_TIMER_ADDR);
    uint32_t guard = UART_RESET_GUARD_TRIPS;
    while ((uint32_t)(mmio_read32(USEC_TIMER_ADDR) - t0) < us && --guard != 0) {
        /* wait */
    }
}

/*
 * Suspend gating of the SER0 clock. Set by uart_clock_suspend() when it
 * finds the bit on and clears it; the next byte out re-gates first. That
 * self-restore is not a nicety: with SER0 unclocked the THRE poll below
 * never sees ready, so every byte would burn its full UART_TX_SPIN_LIMIT
 * — a 300-character battery line would hold the core busy for seconds —
 * and a suspend-loop log line must never be that expensive to emit.
 */
static int g_gated;

void uart_clock_suspend(void)
{
    uint32_t f = hw_irq_save();
    uint32_t en = mmio_read32(DEV_EN_ADDR);
    if (en & DEV_SER0) {
        mmio_write32(DEV_EN_ADDR, en & ~DEV_SER0);
        g_gated = 1;
    }
    hw_irq_restore(f);
}

void uart_clock_resume(void)
{
    if (!g_gated) {
        return;
    }
    uint32_t f = hw_irq_save();
    mmio_write32(DEV_EN_ADDR, mmio_read32(DEV_EN_ADDR) | DEV_SER0);
    hw_irq_restore(f);
    g_gated = 0;
}

/*
 * Event-log tap. Every byte this driver is asked to send is also handed to
 * kernel/evlog.c's RAM ring — how the disk ends up holding a copy of the
 * UART narration. It only OBSERVES: the bytes on the wire are unchanged
 * (the clicky boot smoke compares them). Weak no-op here so the driver
 * still links on its own — the hw-uart trace test, and any build without
 * the log — and the strong definition in evlog.c takes over in the
 * firmware. Fed the character BEFORE the '\n' -> "\r\n" expansion and
 * each hex digit of uart_put_hex32, so the log carries plain text.
 */
__attribute__((weak)) void evlog_capture(uint8_t b)
{
    (void)b;
}

static void uart_tx_byte(uint8_t b)
{
    if (g_gated) {
        uart_clock_resume();
    }
    uint32_t spin = UART_TX_SPIN_LIMIT;
    while (!(mmio_read32(SER0_LSR_ADDR) & SER0_LSR_THRE) && --spin != 0) {
        /* poll */
    }
    mmio_write32(SER0_THR_ADDR, b);
}

void uart_init(void)
{
    /* Init order follows the iPod Video reference sequence (doc
     * corrected + verified against Rockbox uart-pp.c, 2026-06-10):
     * pin routing -> GPO32 release -> device enable -> reset pulse ->
     * line/divisor setup. */

    /* Route SER0 TX/RX to the dock-connector pins: clear bits 2-3 of
     * the routing register, then clear the same bits in GPO32_ENABLE
     * to release those pads from general-purpose-output mode so the
     * SER0 alternate function drives them (08-boot-dock.md, "GPIO
     * routing for SER0 on iPod Video"). */
    mmio_write32(SER0_GPIO_ROUTE_ADDR,
                 mmio_read32(SER0_GPIO_ROUTE_ADDR) & ~SER0_GPIO_ROUTE_MASK);
    mmio_write32(GPO32_ENABLE_ADDR,
                 mmio_read32(GPO32_ENABLE_ADDR) & ~SER0_GPIO_ROUTE_MASK);

    /* Power the UART block, then pulse its reset (08-boot-dock.md,
     * "Init sequence"; DEV_SER0 = bit 6). The reference holds reset
     * for one ~10 ms scheduler tick. We have no tick yet — uart_init is
     * the first thing kernel_main does — but we do not need one:
     * USEC_TIMER is free-running from power-on, so the hold is timed
     * against it directly.
     *
     * This used to be a 1M-iteration `volatile` counting loop, sized on
     * the estimate "multiple ms even at 80 MHz". That estimate was off
     * by more than an order of magnitude for where this code actually
     * runs: BEFORE clock_init() and cache_init(), i.e. on the 24 MHz
     * boot crystal with the cache off and every one of the loop's seven
     * instructions fetched from SDRAM with wait states. Measured from
     * the linked image the loop was ldr/add/b + str/ldr/cmp/bcc per
     * trip — ~16 cycles even with zero-wait-state memory, several times
     * that uncached — so the "10 ms" hold was really on the order of a
     * second, all of it spent before the first byte of boot log. Ten
     * milliseconds on the microsecond counter is what was intended. */
    mmio_write32(DEV_EN_ADDR, mmio_read32(DEV_EN_ADDR) | DEV_SER0);
    g_gated = 0;
    mmio_write32(DEV_RS_ADDR, mmio_read32(DEV_RS_ADDR) | DEV_SER0);
    uart_wait_us(UART_RESET_HOLD_US);
    mmio_write32(DEV_RS_ADDR, mmio_read32(DEV_RS_ADDR) & ~DEV_SER0);

    /* Program the divisor latch for 115200 on the 24 MHz reference:
     * divisor 13 = 0x0D (08-boot-dock.md, "Baud rate" table), then
     * drop DLAB and set 8N1 ("Init sequence"). The reference programs
     * DLL through a second DLAB window after LCR/IER/FCR; writing it
     * inside the first window is functionally equivalent. */
    mmio_write32(SER0_LCR_ADDR, SER0_LCR_DLAB);
    mmio_write32(SER0_DLM_ADDR, 0x00);
    mmio_write32(SER0_DLL_ADDR, SER0_DIV_115200);
    mmio_write32(SER0_LCR_ADDR, SER0_LCR_8N1);

    /*
     * Deliberate deviations from the reference init tail (the IER/FCR
     * stride question is resolved — IER/DLM share +0x04, FCR/IIR
     * +0x08, all word-wide — so these are now choices, not doc gaps):
     *   - SER0_IER = 0x01 (RX IRQ) is skipped: the reference enables
     *     it for accessory-protocol RX and then masks the SER0 IRQ at
     *     the controller anyway; we are polled-TX only with no vector
     *     table installed.
     *   - SER0_FCR = 0x07 (enable + reset FIFOs) is skipped: an 8250
     *     transmits fine in non-FIFO mode via THR/THRE, and the panic
     *     channel has no use for RX buffering yet.
     */
}

void uart_putc(char c)
{
    evlog_capture((uint8_t)c);
    if (c == '\n') {
        uart_tx_byte('\r');
    }
    uart_tx_byte((uint8_t)c);
}

void uart_puts(const char *s)
{
    while (*s != '\0') {
        uart_putc(*s++);
    }
}

void uart_put_hex32(uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";   /* 17 B incl NUL; idx 0..15 */

    for (int shift = 28; shift >= 0; shift -= 4) {
        evlog_capture((uint8_t)hex[(v >> shift) & 0xF]);
        uart_tx_byte((uint8_t)hex[(v >> shift) & 0xF]);
    }
}
