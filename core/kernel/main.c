/*
 * core/kernel/main.c — C entry point, called from boot/crt0.S.
 *
 * Phase 1 PR #3 brought up the SER0 debug UART (boot banner — the
 * out-of-band channel every later bring-up step reports through).
 * PR #4 adds the first visible sign of life: BCM LCD init plus a
 * red → green → blue solid-fill cycle, narrated over the UART so the
 * serial log and the panel can be cross-checked. After that: spin.
 * The cooperative scheduler lands in a subsequent PR.
 */

#include "hw/pp5022.h"
#include "hw/uart.h"
#include "hw/lcd.h"

/*
 * Crude bounded busy-delay between LCD fill colors so a human can see
 * the cycle — same volatile-loop idiom as uart.c's reset hold, just
 * bigger: ~16.8M iterations is on the order of a second at the 80 MHz
 * core clock. Replaced by a real timer driver in a later PR.
 */
static void delay_eyeball(void) {
    for (volatile uint32_t i = 0; i < (1u << 24); i++) {
        /* spin */
    }
}

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

    /* LCD bring-up: host-side port init, then probe the BCM power
     * rail. After a chainload the BCM is already powered and idle
     * (core/docs/hw/02-lcd.md, "Chainload handoff state"). If the
     * probe reads unpowered we skip the fills entirely: Rockbox never
     * touches the BCM ports of an unpowered BCM (it bootstraps
     * first), and the clicky emulator (4G model, no BCM) raises a
     * FatalMemException on any 0x3xxxxxxx access — gating on the
     * probe keeps both the dead-BCM hardware case and the emulator
     * smoke well-defined. */
    if (lcd_init()) {
        uart_puts("core: lcd bcm powered\n");

        /* Solid-fill cycle: red -> green -> blue, ~1 s apart, ending
         * on blue. One narration line per fill so the serial log
         * pinpoints exactly which BCM transaction wedged if the
         * panel stays dark. */
        uart_puts("core: lcd fill red\n");
        lcd_fill(0xF800);
        delay_eyeball();

        uart_puts("core: lcd fill green\n");
        lcd_fill(0x07E0);
        delay_eyeball();

        uart_puts("core: lcd fill blue\n");
        lcd_fill(0x001F);
        uart_puts("core: lcd cycle done (panel should be blue)\n");
    } else {
        uart_puts("core: lcd bcm NOT powered, skipping fills (no bootstrap yet)\n");
    }

    for (;;) {
        __asm__ volatile ("nop");
    }
}
