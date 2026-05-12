/*
 * core/boot/linker.ld — link-time memory layout for the iPod Video image.
 *
 * Memory map (PP5022, see core/docs/hw/01-soc-pp5022.md):
 *   SDRAM @ 0x00000000 (after bootloader remap) — code, rodata, data, bss
 *   IRAM  @ 0x40000000 — stack (top-down) and future fast paths
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

_stack_top = ORIGIN(IRAM) + LENGTH(IRAM);
