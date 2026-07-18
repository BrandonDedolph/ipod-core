/*
 * tests/hw_mmio/uart_trace_test.c — golden-trace test for the SER0 UART
 * driver (hal/hw/uart.c), compiled host-side against the recording mock
 * bus (-DMMIO_MOCK).
 *
 * Proves the exact register grammar of uart_init and the TX path:
 * ordering, read-modify-write correctness (that the pin-route / GPO32
 * releases clear exactly bits 2-3 and nothing else), the 115200 divisor
 * program, and that uart_tx_byte actually spins on LSR.THRE before
 * writing THR. These are driver-logic assertions; absolute address
 * correctness is the static doc-cross-check's job, so both sides here
 * use the pp5022.h symbols on purpose.
 */

#include "pp5022.h"
#include "uart.h"
#include "mmio_mock.h"
#include "trace_expect.h"

/* uart_init: pin route -> GPO32 release -> device enable -> reset pulse
 * -> divisor/line setup. RMW source regs are programmed so the masked
 * result is deterministic and asserted. */
static int test_uart_init(void)
{
    mmio_mock_reset();
    /* Program RMW sources: route + GPO32 read all-ones so we can prove
     * the &= ~0x0C clears exactly bits 2-3. DEV regs read 0. */
    mmio_mock_set_read(SER0_GPIO_ROUTE_ADDR, 0xFFFFFFFF);
    mmio_mock_set_read(GPO32_ENABLE_ADDR,    0xFFFFFFFF);
    mmio_mock_set_read(DEV_EN_ADDR,          0x00000000);
    mmio_mock_set_read(DEV_RS_ADDR,          0x00000000);

    uart_init();

    trace_cursor tc = trace_begin("uart_init");
    /* route release: clear bits 2-3 only */
    expect_r(&tc, 32, SER0_GPIO_ROUTE_ADDR);
    expect_w(&tc, 32, SER0_GPIO_ROUTE_ADDR, 0xFFFFFFFF & ~SER0_GPIO_ROUTE_MASK);
    expect_r(&tc, 32, GPO32_ENABLE_ADDR);
    expect_w(&tc, 32, GPO32_ENABLE_ADDR, 0xFFFFFFFF & ~SER0_GPIO_ROUTE_MASK);
    /* device enable, then reset pulse (set then clear bit 6) */
    expect_r(&tc, 32, DEV_EN_ADDR);
    expect_w(&tc, 32, DEV_EN_ADDR, DEV_SER0);
    expect_r(&tc, 32, DEV_RS_ADDR);
    expect_w(&tc, 32, DEV_RS_ADDR, DEV_SER0);
    expect_r(&tc, 32, DEV_RS_ADDR);
    expect_w(&tc, 32, DEV_RS_ADDR, 0x00000000);
    /* line/divisor: DLAB -> DLM=0 -> DLL=13 -> 8N1 */
    expect_w(&tc, 32, SER0_LCR_ADDR, SER0_LCR_DLAB);
    expect_w(&tc, 32, SER0_DLM_ADDR, 0x00);
    expect_w(&tc, 32, SER0_DLL_ADDR, SER0_DIV_115200);
    expect_w(&tc, 32, SER0_LCR_ADDR, SER0_LCR_8N1);
    trace_expect_end(&tc);
    return trace_done(&tc);
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
    fails += test_uart_puts_newline();
    fails += test_uart_thre_spin();
    return fails == 0 ? 0 : 1;
}
