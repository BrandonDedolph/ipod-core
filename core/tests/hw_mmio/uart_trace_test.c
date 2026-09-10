/*
 * tests/hw_mmio/uart_trace_test.c — golden-trace test for the SER0 UART
 * driver (hal/hw/uart.c), compiled host-side against the recording mock
 * bus (-DMMIO_MOCK).
 *
 * Proves the exact register grammar of uart_init and the TX path:
 * ordering, read-modify-write correctness (that the pin-route / GPO32
 * releases clear exactly bits 2-3 and nothing else), the 115200 divisor
 * program, that the DEV_RS reset hold is timed on USEC_TIMER (10 ms,
 * wrap-safe, bounded against a stuck counter) rather than counted in
 * loop trips, and that uart_tx_byte actually spins on LSR.THRE before
 * writing THR. These are driver-logic assertions; absolute address
 * correctness is the static doc-cross-check's job, so both sides here
 * use the pp5022.h symbols on purpose.
 */

#include "pp5022.h"
#include "uart.h"
#include "mmio_mock.h"
#include "trace_expect.h"

static int check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    return cond ? 0 : 1;
}

/* The reset hold the driver is required to time (08-boot-dock.md: one
 * ~10 ms tick). Duplicated here rather than exported from uart.c so a
 * change to the driver's constant has to be made deliberately in both
 * places. */
#define RESET_HOLD_US  10000u

/* The driver's MMIO_MOCK-side trip cap on the reset-hold wait (uart.c,
 * UART_RESET_GUARD_TRIPS). Mirrored for the stuck-counter case below. */
#define RESET_GUARD_TRIPS  64u

/* Program the RMW sources for uart_init: route + GPO32 read all-ones so
 * we can prove the &= ~0x0C clears exactly bits 2-3. DEV regs read 0. */
static void program_init_sources(void)
{
    mmio_mock_set_read(SER0_GPIO_ROUTE_ADDR, 0xFFFFFFFF);
    mmio_mock_set_read(GPO32_ENABLE_ADDR,    0xFFFFFFFF);
    mmio_mock_set_read(DEV_EN_ADDR,          0x00000000);
    mmio_mock_set_read(DEV_RS_ADDR,          0x00000000);
}

/* uart_init: pin route -> GPO32 release -> device enable -> reset pulse
 * -> divisor/line setup. RMW source regs are programmed so the masked
 * result is deterministic and asserted.
 *
 * The reset hold between the DEV_RS set and clear is timed on USEC_TIMER,
 * not counted in loop iterations: the old iteration-counted hold ran
 * before clock_init/cache_init on the 24 MHz boot crystal with every
 * fetch from uncached SDRAM, and cost on the order of a second, not the
 * 10 ms it was sized for. The counter is scripted to advance 0 -> 5000
 * -> 10000 so the wait exits on TIME (elapsed >= 10000) and the reads
 * land in the trace exactly where the hold is. */
static int test_uart_init(void)
{
    mmio_mock_reset();
    program_init_sources();
    const uint32_t usec_seq[] = { 0, 5000, RESET_HOLD_US };
    mmio_mock_queue_read(USEC_TIMER_ADDR, usec_seq, 3);

    uart_init();

    trace_cursor tc = trace_begin("uart_init");
    /* route release: clear bits 2-3 only */
    expect_r(&tc, 32, SER0_GPIO_ROUTE_ADDR);
    expect_w(&tc, 32, SER0_GPIO_ROUTE_ADDR, 0xFFFFFFFF & ~SER0_GPIO_ROUTE_MASK);
    expect_r(&tc, 32, GPO32_ENABLE_ADDR);
    expect_w(&tc, 32, GPO32_ENABLE_ADDR, 0xFFFFFFFF & ~SER0_GPIO_ROUTE_MASK);
    /* device enable, then reset pulse (set then clear bit 6) ... */
    expect_r(&tc, 32, DEV_EN_ADDR);
    expect_w(&tc, 32, DEV_EN_ADDR, DEV_SER0);
    expect_r(&tc, 32, DEV_RS_ADDR);
    expect_w(&tc, 32, DEV_RS_ADDR, DEV_SER0);
    /* ... with the hold timed on the microsecond counter: t0, then one
     * read that is still short (5000), then the one that completes it */
    expect_r(&tc, 32, USEC_TIMER_ADDR);
    expect_r(&tc, 32, USEC_TIMER_ADDR);
    expect_r(&tc, 32, USEC_TIMER_ADDR);
    expect_r(&tc, 32, DEV_RS_ADDR);
    expect_w(&tc, 32, DEV_RS_ADDR, 0x00000000);
    /* line/divisor: DLAB -> DLM=0 -> DLL=13 -> 8N1 */
    expect_w(&tc, 32, SER0_LCR_ADDR, SER0_LCR_DLAB);
    expect_w(&tc, 32, SER0_DLM_ADDR, 0x00);
    expect_w(&tc, 32, SER0_DLL_ADDR, SER0_DIV_115200);
    expect_w(&tc, 32, SER0_LCR_ADDR, SER0_LCR_8N1);
    trace_expect_end(&tc);

    int fails = trace_done(&tc);
    /* Belt and braces, independent of the positional trace above: the
     * hold READS the counter rather than spinning blind. */
    fails += check("uart_init: reset hold reads USEC_TIMER",
                   mmio_mock_count(MMIO_OP_READ, USEC_TIMER_ADDR) >= 2);
    return fails;
}

/* The hold is a >= compare on elapsed microseconds, not >: 9999 us is
 * not enough, 10000 us is. Counter scripted 0 -> 9999 -> 10000, so the
 * driver must take exactly three counter reads (t0 + two polls) before
 * it releases DEV_RS. A wait that exited on the guard rather than on
 * time would also take a fixed number of reads, but not three — the
 * mock-side guard is RESET_GUARD_TRIPS. */
static int test_uart_reset_hold_is_10ms(void)
{
    mmio_mock_reset();
    program_init_sources();
    const uint32_t usec_seq[] = { 0, RESET_HOLD_US - 1u, RESET_HOLD_US };
    mmio_mock_queue_read(USEC_TIMER_ADDR, usec_seq, 3);

    uart_init();

    return check("reset hold: exits at elapsed == 10000, not 9999",
                 mmio_mock_count(MMIO_OP_READ, USEC_TIMER_ADDR) == 3 &&
                 mmio_mock_count(MMIO_OP_WRITE, DEV_RS_ADDR) == 2);
}

/* USEC_TIMER wraps every ~71.6 min. Start the hold 4096 us before the
 * wrap: the first poll (0xFFFFFFFF) is 4095 us elapsed, the second
 * (0x00001C00) is 11264 us elapsed across the wrap. An absolute
 * "now >= t0 + 10000" compare would overflow to a small deadline and
 * exit on the first poll; a signed compare would see the post-wrap
 * value as "before" t0 and never exit. Exactly three reads proves the
 * unsigned-elapsed form. */
static int test_uart_reset_hold_wrap(void)
{
    mmio_mock_reset();
    program_init_sources();
    const uint32_t usec_seq[] = { 0xFFFFF000u, 0xFFFFFFFFu, 0x00001C00u };
    mmio_mock_queue_read(USEC_TIMER_ADDR, usec_seq, 3);

    uart_init();

    return check("reset hold: wrap-safe elapsed compare",
                 mmio_mock_count(MMIO_OP_READ, USEC_TIMER_ADDR) == 3 &&
                 mmio_mock_count(MMIO_OP_WRITE, DEV_RS_ADDR) == 2);
}

/* A counter that never advances (unprogrammed: every read returns 0)
 * must not hang boot. The hold gives up after the mock-side trip cap,
 * still releases DEV_RS and still programs the line, and the bounded
 * spin is short enough not to overflow the recording bus. */
static int test_uart_reset_hold_bounded(void)
{
    mmio_mock_reset();
    program_init_sources();

    uart_init();

    int fails = 0;
    fails += check("reset hold: stuck counter exits at the guard",
                   mmio_mock_count(MMIO_OP_READ, USEC_TIMER_ADDR)
                       == 1u + RESET_GUARD_TRIPS);
    fails += check("reset hold: stuck counter still releases DEV_RS",
                   mmio_mock_count(MMIO_OP_WRITE, DEV_RS_ADDR) == 2 &&
                   mmio_mock_count(MMIO_OP_WRITE, SER0_LCR_ADDR) == 2);
    fails += check("reset hold: stuck counter does not flood the log",
                   mmio_mock_dropped() == 0);
    return fails;
}

/* uart_puts drives uart_putc/uart_tx_byte: each byte is one THRE poll
 * (LSR programmed ready) then a THR write; '\n' expands to '\r','\n'. */
static int test_uart_puts_newline(void)
{
    mmio_mock_reset();
    mmio_mock_set_read(SER0_LSR_ADDR, SER0_LSR_THRE);   /* always ready */

    uart_puts("Hi\n");

    trace_cursor tc = trace_begin("uart_puts");
    const char expanded[] = { 'H', 'i', '\r', '\n' };
    for (unsigned i = 0; i < sizeof expanded; i++) {
        expect_r(&tc, 32, SER0_LSR_ADDR);
        expect_w(&tc, 32, SER0_THR_ADDR, (uint8_t)expanded[i]);
    }
    trace_expect_end(&tc);
    return trace_done(&tc);
}

/* Prove the TX poll actually waits: LSR reads not-ready 3x then ready,
 * so exactly 4 LSR reads must precede the single THR write. */
static int test_uart_thre_spin(void)
{
    mmio_mock_reset();
    const uint32_t lsr_seq[] = { 0x00, 0x00, 0x00, SER0_LSR_THRE };
    mmio_mock_queue_read(SER0_LSR_ADDR, lsr_seq, 4);

    uart_putc('A');

    trace_cursor tc = trace_begin("uart_thre_spin");
    expect_r(&tc, 32, SER0_LSR_ADDR);
    expect_r(&tc, 32, SER0_LSR_ADDR);
    expect_r(&tc, 32, SER0_LSR_ADDR);
    expect_r(&tc, 32, SER0_LSR_ADDR);
    expect_w(&tc, 32, SER0_THR_ADDR, 'A');
    trace_expect_end(&tc);
    return trace_done(&tc);
}

int main(void)
{
    int fails = 0;
    fails += test_uart_init();
    fails += test_uart_reset_hold_is_10ms();
    fails += test_uart_reset_hold_wrap();
    fails += test_uart_reset_hold_bounded();
    fails += test_uart_puts_newline();
    fails += test_uart_thre_spin();
    return fails == 0 ? 0 : 1;
}
