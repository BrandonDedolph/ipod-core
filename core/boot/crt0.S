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
 *   3. Zero .bss, set up the IRQ-mode stack, call kernel_main, hang.
 *
 * The 0x0-based exception vector table forms the first 32 bytes of the
 * image (it IS _start), and the banked IRQ stack is established in
 * reset_handler; IRQs are left MASKED for the C side to unmask after
 * timer_init. The first-level IRQ entry stub (irq_vector_entry) is a leaf
 * ISR that calls the extern C dispatcher irq_dispatch (kernel/irq.c).
 *
 * No .data copy is needed (doc step 3 doesn't apply to us): the
 * bootloader loads the entire image into SDRAM and linker.ld links with
 * VMA == LMA, so .data is already in place at its final address. Cache
 * init and COP wake come in subsequent PRs as the kernel grows.
 */

    .section .text.startup, "ax"
    .arm
    .global _start

    /* ===================================================================
     * ARM7TDMI exception vector table — the first 32 bytes of the image.
     * _start IS the table, so _start stays at offset 0x0, which is what
     * both the layout check and the bootloader's entry-at-offset-0 rely on
     * (the table and the entry are the same bytes).
     *
     * Reset (0x00) is a PC-RELATIVE `b`: at bootloader entry SDRAM is still
     * at native 0x10000000 and the MMAP0 remap to 0x0 has NOT happened, so
     * an absolute `ldr pc,[pc,#..]` loading a 0x0-based address would jump
     * into unmapped space and brick. `b` is PC-relative and safe pre-remap.
     *
     * The other seven slots (0x04..0x1C) use `ldr pc, <literal>` loading
     * ABSOLUTE 0x0-based handler addresses from the explicit `.word` pool
     * placed immediately after the slots (NOT `ldr pc,=sym`, which would
     * let the literal pool drift out of the [pc,#imm] +/-4KB range and
     * reorder the table). These only ever fire AFTER remap + IRQ-unmask,
     * when 0x0-based addresses are live.
     * =================================================================== */
_start:
    b       reset_handler           @ 0x00 reset  (PC-relative, pre-remap safe)
    ldr     pc, _v_undef            @ 0x04 undefined instruction
    ldr     pc, _v_swi              @ 0x08 software interrupt (SWI)
    ldr     pc, _v_pabt             @ 0x0C prefetch abort
    ldr     pc, _v_dabt             @ 0x10 data abort
    ldr     pc, _v_resv             @ 0x14 reserved
    ldr     pc, _v_irq              @ 0x18 IRQ
    ldr     pc, _v_fiq              @ 0x1C FIQ

    /* Absolute (0x0-based) handler literals — kept adjacent to the slots
     * so every `ldr pc` above stays in [pc,#imm] range and the table does
     * not get reordered by an implicit literal pool. */
_v_undef:   .word trap_hang
_v_swi:     .word trap_hang
_v_pabt:    .word trap_hang
_v_dabt:    .word trap_hang
_v_resv:    .word trap_hang
_v_fiq:     .word trap_hang
_v_irq:     .word irq_vector_entry

    /* Deterministic park for unhandled exceptions — a tight self-branch so
     * a stray undef/abort/SWI stops here where GDB can see it, instead of
     * running off into garbage. */
trap_hang:
    b       trap_hang

    /* ===================================================================
     * RESET PATH. Reached via the reset slot's `b` above. This is the
     * entire original crt0 startup body, unchanged.
     * =================================================================== */
reset_handler:
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

    /* Park the COP. The handoff contract says only the CPU runs, but
     * Rockbox parks the second core at entry defensively — and the
     * clicky emulator's HLE boot really does start both cores, where
     * an unparked COP races the CPU through BSS-zero and uart_init
     * (caught 2026-06-11; doubled UART output). PROCESSOR_ID low byte
     * is 0x55 on the CPU, 0xAA on the COP (01-soc-pp5022.md, "Dual
     * core: CPU and COP"). The COP is now put into COP_CTL sleep
     * (PROC_SLEEP = 0x80000000, sleep until interrupt) rather than
     * merely busy-spinning — clicky models this (a slept core is not
     * stepped), so the emulator's dual-core HLE boot no longer doubles
     * UART output (verified 2026-07-17). A busy-spin `cop_park` follows
     * as a backstop in case of a spurious wake before mailbox IPC
     * exists. (01-soc-pp5022.md, "Dual core: CPU and COP" -> "Sleep /
     * wake": CPU_CTL = 0x60007000, COP_CTL = 0x60007004; a write to a
     * PROC_CTL register must be followed by 3 NOPs for the pipeline.)
     * (Immediates + fixed MMIO only — position-independent.) */
    mov     r0, #0x60000000
    ldr     r0, [r0]
    and     r0, r0, #0xFF
    cmp     r0, #0x55
    beq     cpu_start

    /* COP: sleep via its control register until an interrupt. Build
     * COP_CTL (0x60007004) as a 0x60007000 base + [#4] offset, and
     * PROC_SLEEP (0x80000000) as a single rotated immediate. */
    mov     r0, #0x60000000
    orr     r0, r0, #0x00007000     /* r0 = 0x60007000 (PROC_CTL base) */
    mov     r1, #0x80000000
    str     r1, [r0, #4]            /* COP_CTL (base+4 = 0x60007004) = PROC_SLEEP */
    nop
    nop
    nop
cop_park:
    b       cop_park

cpu_start:
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

    /* Set up the banked IRQ-mode stack, then return to supervisor. We are
     * post-remap and running at link addresses, so `ldr sp,=` literals now
     * resolve to live IRAM addresses. Enter IRQ mode (I+F still masked) to
     * get at the banked SP_irq, point it at its own 1 KB region at the top
     * of IRAM, then switch back to supervisor and re-load SP_svc. IRQs stay
     * MASKED here — the C side unmasks after timer_init. (The earlier
     * `ldr sp,=_stack_top` at cpu_start ran on the temp/remap stack; this
     * re-load is harmless and just re-establishes SP_svc post-remap.) */
    msr     cpsr_c, #0xD2           @ IRQ mode, I+F masked (0x12 | 0xC0)
    ldr     sp, =_irq_stack_top
    msr     cpsr_c, #0xD3           @ supervisor mode, I+F masked
    ldr     sp, =_stack_top

    /* Hand off to C. */
    bl      kernel_main

    /* kernel_main is _Noreturn; if it ever does return, park. */
3:
    b       3b

    /* ===================================================================
     * First-level IRQ entry. Leaf ISR — no context switch (the scheduler
     * is cooperative). Vectored to from slot 0x18 via _v_irq. Fixes up the
     * IRQ return address, saves the caller-saved (AAPCS) registers plus the
     * return address, calls the C dispatcher, then restores and returns,
     * with `^` on the ldm restoring CPSR from SPSR_irq. `irq_dispatch` is
     * an extern C function (kernel/irq.c) supplied by another agent.
     * =================================================================== */
    .global irq_vector_entry
irq_vector_entry:
    sub     lr, lr, #4              @ IRQ return-address fixup
    @ Save an EVEN number of registers (6) so sp stays 8-byte aligned at
    @ the bl below, per AAPCS — do not add/remove one without padding.
    stmfd   sp!, {r0-r3, r12, lr}   @ save caller-saved + return
    bl      irq_dispatch            @ C dispatcher (extern, kernel/irq.c)
    ldmfd   sp!, {r0-r3, r12, pc}^  @ restore + return, ^ = CPSR <- SPSR

    .size _start, . - _start
