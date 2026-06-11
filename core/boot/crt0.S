/*
 * core/boot/crt0.S — ARM7TDMI startup for the iPod Video.
 *
 * The Rockbox-compatible bootloader hands off to our OSOS image with
 * (core/docs/hw/08-boot-dock.md, "Boot ROM → our code: handoff state"):
 *   - SDRAM at its NATIVE address (0x10000000) — NOT yet remapped to
 *     0x00000000. We are loaded into and entered in that native window,
 *     while linker.ld links every symbol at 0x0-based addresses, so
 *     performing the remap is OUR job and everything before the remap
 *     completes must be position-independent.
 *   - MMU off, cache off
 *   - IRQ + FIQ disabled, CPSR = 0xD3 (supervisor mode)
 *   - Stack pointer undefined
 *   - Vector table still pointing at boot ROM (we don't enable IRQs yet)
 *
 * Startup sequence (08-boot-dock.md, "Our crt0.S job", steps 1, 2, 4, 7):
 *   1. Temporary stack at the top of usable IRAM.
 *   2. Remap SDRAM to 0x00000000 via the MMAP0 registers, executing the
 *      remap from an IRAM-resident stub so the code doing the remap is
 *      not itself affected by it (08-boot-dock.md step 2;
 *      01-soc-pp5022.md, "Memory remap (MMAP0)").
 *   3. Zero .bss, call kernel_main, hang.
 *
 * No .data copy is needed (doc step 3 doesn't apply to us): the
 * bootloader loads the entire image into SDRAM and linker.ld links with
 * VMA == LMA, so .data is already in place at its final address. Cache
 * init, COP wake, vector table install and IRQ setup come in subsequent
 * PRs as the kernel grows.
 */

    .section .text.startup, "ax"
    .arm
    .global _start

_start:
    /* ===================================================================
     * PRE-REMAP. The PC is somewhere in SDRAM's native 0x10000000 window
     * but all symbols are linked at 0x0-based addresses. Until the MMAP0
     * remap below takes effect, nothing here may dereference or branch to
     * a link-time SDRAM address — PC-relative and fixed hardware
     * constants only.
     * =================================================================== */

    /* Re-assert supervisor mode + IRQ/FIQ masked. The bootloader already
     * leaves us this way; the explicit msr is defensive and ~free.
     * (Immediate-only, no memory access — position-independent.) */
    msr     cpsr_c, #0xD3

    /* Step 1 (08-boot-dock.md): temporary stack at the top of usable
     * IRAM, growing downward. IRAM is 0x40000000..0x4001FFFF (128 KB);
     * 96 KB usable to user code per core/docs/hw/01-soc-pp5022.md.
     * Safe pre-remap: the ldr is a PC-relative literal load, and the
     * VALUE fetched (_stack_top, from linker.ld) is an IRAM address —
     * IRAM's mapping is unaffected by the SDRAM remap. */
    ldr     sp, =_stack_top

    /* Step 2 (08-boot-dock.md): remap SDRAM to 0x00000000, "using
     * IRAM-resident remap code so we don't pull the rug out from under
     * ourselves" (08-boot-dock.md step 2, after Rockbox crt0-pp.S
     * 84-107). Copy the stub below to IRAM base and jump to it.
     * adr is PC-relative; 0x40000000 is a fixed hardware constant,
     * not a link-time address. */
    adr     r0, remap_stub
    adr     r1, remap_stub_end
    mov     r2, #0x40000000
1:
    cmp     r0, r1
    ldrlo   r3, [r0], #4
    strlo   r3, [r2], #4
    blo     1b
    mov     r2, #0x40000000
    mov     pc, r2

    /* SDRAM remap stub — copied to, and executed from, IRAM 0x40000000.
     * Self-contained and position-independent: constants are built with
     * mov/orr immediates, and its one literal (the post-remap target)
     * sits inside the copied region.
     *
     * Register programming per core/docs/hw/01-soc-pp5022.md, "Memory
     * remap (MMAP0)", verified against Rockbox crt0-pp.S + pp5020.h
     * (2026-06-10, master):
     *   MMAP0_LOGICAL (0xF000F000) = 0x00003C00 — logical base 0x0 OR
     *     the window mask. The size mask lives HERE, not in the physical
     *     flags: 0x3C00 = 64 MB window, 0x3E00 = 32 MB. Rockbox builds
     *     iPod Video with the 64 MB mask unconditionally (32 MB 5Gs
     *     included; actual RAM size is probed at runtime, not via MMAP),
     *     so we do the same. Writing 0 here (no mask bits) is a value no
     *     known firmware ever writes — undefined behavior.
     *   MMAP0_PHYSICAL (0xF000F004) = 0x10000F84 — physical base OR
     *     flags 0x0F84. 0x0F00 = read|write|data|code permission bits;
     *     0x84 is undocumented but present in every known user. 0x0F84
     *     is confirmed correct for PP502x (0x3F84 is the PP5002 value).
     * Logical is written first, physical second — the order Rockbox has
     * shipped on this silicon for two decades. Both writes execute from
     * IRAM, so the transient half-programmed mapping never bites. */
remap_stub:
    mov     r0, #0xF0000000
    orr     r0, r0, #0x0000F000     /* r0 = 0xF000F000 (MMAP0_LOGICAL) */
    mov     r2, #0x00003C00
    str     r2, [r0]                /* MMAP0_LOGICAL  = 0x3C00: base 0 | 64M mask */
    mov     r1, #0x10000000
    orr     r1, r1, #0x00000F00
    orr     r1, r1, #0x00000084     /* r1 = 0x10000F84: phys | flags 0x0F84 */
    str     r1, [r0, #4]            /* MMAP0_PHYSICAL = 0x10000F84 */
    /* Enter the remapped image: load the link-time (0x0-based) address
     * of post_remap into pc. The ldr itself is PC-relative into the
     * stub's own copied literal; only the loaded VALUE is absolute, and
     * by now SDRAM-at-0x0 is live. */
    ldr     pc, remap_stub_target
remap_stub_target:
    .word   post_remap
remap_stub_end:

    /* ===================================================================
     * POST-REMAP. Executing at link addresses; SDRAM is at 0x00000000.
     * Absolute (link-time) addresses are fine from here on.
     * =================================================================== */
post_remap:
    /* Zero .bss. Aligned 4-byte stores; the linker script asserts
     * (__bss_end - __bss_start) is word-multiple by aligning the section. */
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
2:
    cmp     r0, r1
    strlo   r2, [r0], #4
    blo     2b

    /* Hand off to C. */
    bl      kernel_main

    /* kernel_main is _Noreturn; if it ever does return, park. */
3:
    b       3b

    .size _start, . - _start
