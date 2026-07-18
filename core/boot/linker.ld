/*
 * core/boot/linker.ld — link-time memory layout for the iPod Video image.
 *
 * Memory map (PP5022, see core/docs/hw/01-soc-pp5022.md):
 *   SDRAM @ 0x00000000 — code, rodata, data, bss. These are POST-REMAP
 *          addresses: at bootloader handoff SDRAM is still at its native
 *          0x10000000 and our crt0.S performs the MMAP0 remap to
 *          0x00000000 itself before relying on any address linked here
 *          (core/docs/hw/08-boot-dock.md, "Boot ROM → our code:
 *          handoff state"). Everything in crt0.S up to that point is
 *          position-independent.
 *   IRAM  @ 0x40000000 — stack (top-down), crt0's remap stub, and
 *          future fast paths
 *
 * The image entry point is _start (boot/crt0.S), placed first in .text
 * via the .text.startup section so it sits at offset 0 of the loaded
 * image — which is also what the bootloader's entryOffset = 0 expects.
 *
 * Size choices:
 *   SDRAM 32M — the 5G has 32 MB; the 5.5G has 64 MB but our image lives
 *               in the low 32 to keep one image bootable on both. Heap /
 *               codec buffer carving for the upper region lands later.
 *   IRAM  96K — full region is 128 KB; the top 32 KB is reserved for SoC
 *               scratch / future use per the hw doc.
 */

OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY {
    SDRAM (rwx) : ORIGIN = 0x00000000, LENGTH = 32M
    IRAM  (rwx) : ORIGIN = 0x40000000, LENGTH = 96K
}

SECTIONS {
    .text : ALIGN(4) {
        KEEP(*(.text.startup))
        *(.text*)
        *(.rodata*)
        *(.glue_7) *(.glue_7t)
        . = ALIGN(4);
    } > SDRAM

    .data : ALIGN(4) {
        *(.data*)
        . = ALIGN(4);
    } > SDRAM

    .bss (NOLOAD) : ALIGN(4) {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > SDRAM

    /DISCARD/ : {
        *(.note*)
        *(.ARM.attributes)
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.comment)
        *(.eh_frame*)
    }
}

/*
 * Two stacks carved from the top of IRAM, both growing downward:
 *   - IRQ stack:        [0x40017C00, 0x40018000)  — 1 KB at the very top,
 *                       banked SP_irq, set up in crt0.S reset_handler.
 *   - Supervisor stack: grows down from 0x40017C00 — SP_svc, the main
 *                       C-code stack.
 */
_irq_stack_top = ORIGIN(IRAM) + LENGTH(IRAM);   /* 0x40018000 */
_stack_top     = _irq_stack_top - 0x400;        /* 1 KB IRQ stack; svc top = 0x40017C00 */
