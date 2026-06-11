/*
 * core/hal/hw/uart.c — SER0 debug UART driver (PP5022, polled TX).
 *
 * First register-touching driver in the tree. Implements the init
 * sequence from core/docs/hw/08-boot-dock.md ("UART debug" -> "Init
 * sequence (default 115200 8-N-1)"), minus the steps the doc doesn't
 * fully specify (marked TODO(hw-doc) below) and minus the IRQ/FIFO
 * setup a polled transmitter doesn't need.
 *
 * Freestanding-clean: no libc, fixed-width types from <stdint.h> only.
 */

#include "pp5022.h"
#include "uart.h"

/*
 * Upper bound on the TX-ready poll so a dead UART can't hang the
 * kernel. One character at 115200 takes ~87 us = ~2100 CPU cycles at
 * the 24 MHz boot clock — a few hundred trips around this loop — so
 * 1<<16 iterations is orders of magnitude past "working hardware".
 * If we ever time out (e.g. SER0 unclocked because the DEV_SER0
 * enable step below is still a doc gap), the write is attempted
 * anyway and the byte is silently lost; the debug channel degrades,
 * the firmware keeps running.
 */
#define UART_TX_SPIN_LIMIT  (1u << 16)

static void uart_tx_byte(uint8_t b)
{
    uint32_t spin = UART_TX_SPIN_LIMIT;
    while (!(SER0_LSR & SER0_LSR_THRE) && --spin != 0) {
        /* poll */
    }
    SER0_THR = b;
}

void uart_init(void)
{
    /*
     * TODO(hw-doc): 08-boot-dock.md's init sequence starts with
     *
     *     DEV_EN |= DEV_SER0;     // power UART
     *     DEV_RS |= DEV_SER0;     // assert reset
     *     sleep(1);
     *     DEV_RS &= ~DEV_SER0;    // release
     *
     * but no doc in core/docs/hw/ records the DEV_SER0 bit value, so
     * the enable/reset pulse is omitted rather than guessed. If SER0
     * comes up dead on hardware, this is the first suspect: confirm
     * the bit from Rockbox firmware/export/pp5020.h, add it to the
     * doc and pp5022.h, and enable the block above. (The boot ROM /
     * chainloading bootloader may well leave SER0 enabled already,
     * which is why bring-up without this step is worth attempting.)
     */

    /* Route SER0 TX/RX to the dock-connector pins: clear bits 2-3 of
     * the routing register (08-boot-dock.md, "GPIO routing for SER0
     * on iPod Video"). */
    SER0_GPIO_ROUTE &= ~SER0_GPIO_ROUTE_MASK;

    /*
     * TODO(hw-doc): the doc's routing snippet has a second step,
     * `GPO32_ENABLE &= ~0x0000000C`, but GPO32_ENABLE's address is
     * not documented anywhere in core/docs/hw/ (02-lcd.md uses
     * GPO32_VAL, also addressless). Omitted rather than guessed —
     * second suspect if TX stays silent on hardware.
     */

    /* Program the divisor latch for 115200 on the 24 MHz reference:
     * divisor 13 = 0x0D (08-boot-dock.md, "Baud rate" table), then
     * drop DLAB and set 8N1 ("Init sequence"). */
    SER0_LCR = SER0_LCR_DLAB;
    SER0_DLM = 0x00;
    SER0_DLL = SER0_DIV_115200;
    SER0_LCR = SER0_LCR_8N1;

    /*
     * Deliberate deviations from the doc's init tail:
     *   - SER0_IER = 0x01 (RX IRQ) is skipped: we are polled-TX only
     *     and have no vector table installed yet, so enabling a
     *     source nothing services buys nothing.
     *   - SER0_FCR = 0x07 (enable + reset FIFOs) is skipped: an 8250
     *     transmits fine in non-FIFO mode, and the doc's IER/FCR
     *     addresses carry an unresolved stride inconsistency (see
     *     TODO(hw-doc) in pp5022.h) — not worth a blind write on the
     *     panic channel.
     */
}

void uart_putc(char c)
{
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
    static const char hex[16] = "0123456789ABCDEF";

    for (int shift = 28; shift >= 0; shift -= 4) {
        uart_tx_byte((uint8_t)hex[(v >> shift) & 0xF]);
    }
}
