/*
 * core/kernel/main.c — C entry point, called from boot/crt0.S.
 *
 * Phase 1 PR #3 deliverable: the SER0 debug UART comes up first thing
 * and prints a boot banner — the out-of-band channel every later
 * bring-up step (LCD, cache, COP wake, scheduler) reports through.
 * After the banner: spin. The cooperative scheduler lands in a
 * subsequent PR.
 */

#include "hw/pp5022.h"
#include "hw/uart.h"

_Noreturn void kernel_main(void) {
    uart_init();

    uart_puts("core: kernel alive (iPod 5G/5.5G, PP5022)\n");

    /* Hex-path self-test: a fixed pattern exercising every nibble
     * position plus digits and letters. If the line below doesn't
     * read 1234ABCD on the terminal, distrust every register dump
     * that follows. */
    uart_puts("core: uart self-test ");
    uart_put_hex32(0x1234ABCD);
    uart_putc('\n');

    /* First real register dump: which core are we? Expect the low
     * byte to read PROC_ID_CPU = 0x55 (core/docs/hw/01-soc-pp5022.md,
     * "Dual core: CPU and COP"). */
    uart_puts("core: PROCESSOR_ID ");
    uart_put_hex32(PROCESSOR_ID);
    uart_putc('\n');

    for (;;) {
        __asm__ volatile ("nop");
    }
}
