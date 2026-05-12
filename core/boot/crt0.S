/*
 * core/boot/crt0.S — ARM7TDMI startup for the iPod Video.
 *
 * The Rockbox-compatible bootloader hands off to our OSOS image with:
 *   - SDRAM remapped to 0x00000000 (where we link / where we're loaded)
 *   - MMU off, cache off
 *   - IRQ + FIQ disabled, CPSR = 0xD3 (supervisor mode)
 *   - Stack pointer undefined
 *   - Vector table still pointing at boot ROM (we don't enable IRQs yet)
 * (See core/docs/hw/08-boot-dock.md "Boot ROM → our code: handoff state".)
 *
 * Phase 1 PR #1: set up a stack in IRAM, zero .bss, call kernel_main, hang.
 * SDRAM remap, .data copy, cache init, COP wake, vector table install and
 * IRQ setup come in subsequent PRs as the kernel grows.
 */

    .section .text.startup, "ax"
    .arm
    .global _start

_start:
    /* Re-assert supervisor mode + IRQ/FIQ masked. The bootloader already
     * leaves us this way; the explicit msr is defensive and ~free. */
    msr     cpsr_c, #0xD3

    /* Stack at the top of usable IRAM, growing downward.
     * IRAM is 0x40000000..0x4001FFFF (128 KB); 96 KB usable to user code
     * per core/docs/hw/01-soc-pp5022.md. _stack_top is defined in linker.ld. */
    ldr     sp, =_stack_top

    /* Zero .bss. Aligned 4-byte stores; the linker script asserts
     * (__bss_end - __bss_start) is word-multiple by aligning the section. */
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
1:
    cmp     r0, r1
    strlo   r2, [r0], #4
    blo     1b

    /* Hand off to C. */
    bl      kernel_main

    /* kernel_main is _Noreturn; if it ever does return, park. */
2:
    b       2b

    .size _start, . - _start
