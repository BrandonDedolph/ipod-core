/*
 * core/hal/hw/pp5022.h — PortalPlayer PP5022 platform constants.
 *
 * Canonical home for the hardware constants used by the hw HAL backend
 * and (eventually) boot/crt0.S. Every value here is transcribed from
 * the Phase-0 hardware reference under core/docs/hw/ — that reference
 * is the ONLY permitted source for MMIO addresses and bit layouts.
 * Each block cites the doc section it came from. Where the docs are
 * silent or self-inconsistent, the gap is marked TODO(hw-doc) rather
 * than filled from memory.
 *
 * The header is dual-mode: bare addresses (*_ADDR / sizes / bit masks)
 * are visible to both C and assembly; the volatile lvalue accessors
 * (SER0_LCR = ...) are C-only behind the __ASSEMBLER__ guard. crt0.S
 * still carries its own inline constants for now — rewiring it to use
 * this header is deliberately out of scope for this PR.
 */

#ifndef CORE_HAL_HW_PP5022_H
#define CORE_HAL_HW_PP5022_H

/* ---------- MMIO accessors (C only) --------------------------------- */

#ifndef __ASSEMBLER__
#include <stdint.h>

#define PP_REG32(addr)  (*(volatile uint32_t *)(addr))
#define PP_REG16(addr)  (*(volatile uint16_t *)(addr))
#define PP_REG8(addr)   (*(volatile uint8_t  *)(addr))
#endif

/* ---------- Memory map ----------------------------------------------
 * core/docs/hw/01-soc-pp5022.md, "Memory map".
 *
 * SDRAM sits at its native 0x10000000 at handoff (the chainloading
 * bootloader restores the Apple-ROM MMAP state before jumping); OUR
 * crt0 remaps it to 0x00000000 so the ARM exception vectors land in
 * our image (see MMAP0 below and boot/crt0.S).
 */

#define SDRAM_BASE          0x00000000  /* post-MMAP0-remap logical base */
#define SDRAM_NATIVE_BASE   0x10000000  /* physical base, pre-remap      */
#define SDRAM_SIZE_5G       0x02000000  /* 32 MB (iPod Video 5G)         */
#define SDRAM_SIZE_5_5G     0x04000000  /* 64 MB (iPod Video 5.5G)       */

#define IRAM_BASE           0x40000000
#define IRAM_SIZE           0x00020000  /* 128 KB total                  */
#define IRAM_USABLE_SIZE    0x00018000  /* ~96 KB usable to user code    */

/* ---------- MMAP0: logical->physical remap ---------------------------
 * core/docs/hw/01-soc-pp5022.md, "Memory remap (MMAP0)", verified
 * against Rockbox crt0-pp.S + pp5020.h (2026-06-10): MMAP0_LOGICAL
 * (0xF000F000) takes the logical base OR'd with the window-size mask
 * (the mask lives HERE, not in the physical flags); MMAP0_PHYSICAL
 * (0xF000F004) takes the physical base OR'd with permission flags.
 * 0x0F00 = read|write|data|code; 0x84 is undocumented but present in
 * every known user. 0x0F84 is the PP502x flags value (0x3F84 is
 * PP5002's). Write order: logical first, then physical; no barrier.
 */

#define MMAP0_LOGICAL_ADDR    0xF000F000
#define MMAP0_PHYSICAL_ADDR   0xF000F004
#define MMAP0_REMAP_FLAGS     0x00000F84  /* read|write|data|code (+0x84) */
#define MMAP0_WINDOW_64M      0x00003C00  /* logical-register size mask   */
#define MMAP0_WINDOW_32M      0x00003E00  /*   (64M used for all Videos)  */

#ifndef __ASSEMBLER__
#define MMAP0_LOGICAL   PP_REG32(MMAP0_LOGICAL_ADDR)
#define MMAP0_PHYSICAL  PP_REG32(MMAP0_PHYSICAL_ADDR)
#endif

/* ---------- Processor ID ---------------------------------------------
 * core/docs/hw/01-soc-pp5022.md, "Dual core: CPU and COP". Reading
 * PROCESSOR_ID distinguishes the two ARM7TDMI cores.
 */

#define PROCESSOR_ID_ADDR   0x60000000
#define PROC_ID_CPU         0x55
#define PROC_ID_COP         0xAA

#ifndef __ASSEMBLER__
#define PROCESSOR_ID    PP_REG32(PROCESSOR_ID_ADDR)
#endif

/* ---------- Dual-core control: CPU_CTL / COP_CTL ---------------------
 * core/docs/hw/01-soc-pp5022.md, "Dual core: CPU and COP" -> "Sleep /
 * wake". Per-core power/flow control; PROC_CTL(core) selects between
 * them. CPU is 0x55 / COP is 0xAA in PROCESSOR_ID. A write must be
 * followed by 3 NOPs (the pipeline needs a beat to take effect).
 *
 *   PROC_SLEEP     sleep until an interrupt (hangs with no IRQ source)
 *   PROC_WAIT_CNT  sleep until a countdown expires (self-wakes)
 *   PROC_WAKE_INT  also raise an IRQ on wake
 *   PROC_CNT_*     countdown tick unit; low 8 bits are the count
 *
 * clicky models this (cpucon.rs: a core with any flow bit set is not
 * stepped), so a COP_CTL sleep halts the COP in the emulator exactly as
 * on hardware — verified 2026-07-17.
 */

#define CPU_CTL_ADDR        0x60007000
#define COP_CTL_ADDR        0x60007004

#define PROC_SLEEP          0x80000000
#define PROC_WAIT_CNT       0x40000000
#define PROC_WAKE_INT       0x20000000
#define PROC_CNT_USEC       0x02000000
#define PROC_CNT_MSEC       0x01000000
#define PROC_CNT_MASK       0x000000FF  /* low 8 bits: countdown value */

#ifndef __ASSEMBLER__
#define CPU_CTL         PP_REG32(CPU_CTL_ADDR)
#define COP_CTL         PP_REG32(COP_CTL_ADDR)
#endif

/* ---------- Interrupt controller -------------------------------------
 * core/docs/hw/01-soc-pp5022.md, "Interrupt controller". Low-priority
 * bank only (the timer sources we need live there). Sources #<32 route
 * through CPU_INT_EN/DIS at bit #N; write-1-to-enable / write-1-to-
 * disable. The CPSR I-bit gates IRQs at the core.
 */

#define CPU_INT_STAT_ADDR     0x60004000
#define CPU_INT_EN_ADDR       0x60004024  /* write 1<<N to enable IRQ N  */
#define CPU_INT_DIS_ADDR      0x60004028  /* write 1<<N to disable IRQ N */
#define CPU_INT_PRIORITY_ADDR 0x6000402C

#define TIMER1_IRQ          0             /* low bank, 1<<0 (01-soc, IRQ #) */
#define TIMER2_IRQ          1             /* low bank, 1<<1 */

#define CPSR_I_BIT          0x00000080    /* CPSR IRQ-disable bit */

#ifndef __ASSEMBLER__
#define CPU_INT_STAT    PP_REG32(CPU_INT_STAT_ADDR)
#define CPU_INT_EN      PP_REG32(CPU_INT_EN_ADDR)
#define CPU_INT_DIS     PP_REG32(CPU_INT_DIS_ADDR)
#endif

/* ---------- Timers / system tick -------------------------------------
 * core/docs/hw/01-soc-pp5022.md, "Timers and the system tick". Two
 * programmable down-counters + a free-running microsecond counter, all
 * clocked at a fixed 1 MHz (TIMER_FREQ). TIMERx_CFG is armed in one
 * write: 0xC0000000 | (period_us - 1) — enable + IRQ/periodic-reload +
 * the reload period. Reading TIMERx_VAL acknowledges that timer's IRQ.
 */

#define TIMER1_CFG_ADDR     0x60005000
#define TIMER1_VAL_ADDR     0x60005004
#define TIMER2_CFG_ADDR     0x60005008  /* recorded; unused by the tick */
#define TIMER2_VAL_ADDR     0x6000500C
#define USEC_TIMER_ADDR     0x60005010  /* free-running 1 MHz counter   */

#define TIMER_FREQ          1000000     /* 1 MHz timer clock            */
#define TIMER_CFG_ENABLE    0x80000000  /* CFG bit 31: timer enable     */
#define TIMER_CFG_IRQEN     0x40000000  /* CFG bit 30: IRQ + periodic reload */

#ifndef __ASSEMBLER__
#define TIMER1_CFG      PP_REG32(TIMER1_CFG_ADDR)
#define TIMER1_VAL      PP_REG32(TIMER1_VAL_ADDR)
#define USEC_TIMER      PP_REG32(USEC_TIMER_ADDR)
#endif

/* ---------- Device enable / reset / clock control --------------------
 * core/docs/hw/01-soc-pp5022.md, "Power management" (DEV_*) and
 * "Clock tree" (PLL_*, CLOCK_SOURCE, DEV_TIMING1). These gate
 * per-peripheral clock and reset. DEV_SER0/SER1 bit values verified
 * against Rockbox pp5020.h (2026-06-10); used by the 08-boot-dock.md
 * "Init sequence" enable + reset pulse.
 */

#define DEV_RS_ADDR         0x60006004
#define DEV_RS2_ADDR        0x60006008
#define DEV_EN_ADDR         0x6000600C
#define DEV_EN2_ADDR        0x60006010

#define DEV_SER0            0x00000040  /* bit 6 in DEV_EN / DEV_RS */
#define DEV_SER1            0x00000080  /* bit 7 */
#define DEV_INIT1_ADDR      0x70000010
#define DEV_INIT2_ADDR      0x70000020

#define CLOCK_SOURCE_ADDR   0x60006020
#define PLL_CONTROL_ADDR    0x60006034
#define PLL_STATUS_ADDR     0x6000603C
#define DEV_TIMING1_ADDR    0x70000034

#ifndef __ASSEMBLER__
#define DEV_RS          PP_REG32(DEV_RS_ADDR)
#define DEV_RS2         PP_REG32(DEV_RS2_ADDR)
#define DEV_EN          PP_REG32(DEV_EN_ADDR)
#define DEV_EN2         PP_REG32(DEV_EN2_ADDR)
#define DEV_INIT1       PP_REG32(DEV_INIT1_ADDR)
#define DEV_INIT2       PP_REG32(DEV_INIT2_ADDR)

#define CLOCK_SOURCE    PP_REG32(CLOCK_SOURCE_ADDR)
#define PLL_CONTROL     PP_REG32(PLL_CONTROL_ADDR)
#define PLL_STATUS      PP_REG32(PLL_STATUS_ADDR)
#define DEV_TIMING1     PP_REG32(DEV_TIMING1_ADDR)
#endif

/* ---------- Clock tree / PLL programming values ---------------------
 * core/docs/hw/01-soc-pp5022.md, "Clock tree" (the frequency-switch
 * sequence). Opaque multiplier/divider encodings transcribed from the
 * doc, not re-derived. The PLL runs off the 24 MHz crystal. The
 * PLL_STATUS lock poll MUST be a bounded spin: clicky (and a dead PLL)
 * never sets the lock bit, so an unbounded wait would hang.
 */
#define DEV_INIT2_PLL_POWER   0x40000000  /* DEV_INIT2 bit 30: PLL power */
#define PLL_CONTROL_ENABLE    0x88000000  /* PLL_CONTROL enable bits     */
#define PLL_STATUS_LOCK       0x80000000  /* PLL_STATUS bit 31: locked   */

/* PP5022 programs the PLL in a SINGLE write per target — no pre-stage.
 * (The 0x8A020A03 pre-stage in some references is the PP5020 sequence.) */
#define PLL_CONTROL_30MHZ     0x8A220501  /* 30 MHz: mult 5/1, /4         */
#define PLL_CONTROL_80MHZ     0x8A121403  /* 80 MHz: mult 20/3, /2        */

#define CLOCK_SOURCE_XTAL     0x20002222  /* all domains -> 24 MHz xtal   */
#define CLOCK_SOURCE_PLL      0x20007777  /* all domains -> PLL output    */

#define DEV_TIMING1_SLOW      0x00000303  /* relaxed timing (slow clock)  */
#define DEV_TIMING1_FAST      0x00000808  /* high-speed timing            */

/* ---------- SER0: dock-connector debug UART (8250-style) -------------
 * core/docs/hw/08-boot-dock.md, "UART debug" -> "SoC registers
 * (8250-style at SER0)", stride verified against Rockbox pp5020.h
 * (2026-06-10): the standard 8250 register file with each byte
 * register widened to a 32-bit word slot (uniform 4-byte stride, all
 * accesses word-wide). The classic 8250 sharing holds: RBR (read),
 * THR (write) and DLL (with LCR.DLAB) share +0x00; IER/DLM share
 * +0x04; IIR (read) / FCR (write) share +0x08. Reference clock is
 * 24 MHz; divisor = 24e6 / (16 * baud) (08-boot-dock.md, "Baud rate").
 * (An earlier doc revision byte-strided IER/FCR at 0x70006001/2 —
 * that was a transcription slip, now corrected in the doc.)
 */

#define SER0_BASE_ADDR      0x70006000

#define SER0_RBR_ADDR       0x70006000  /* RX buffer (read)              */
#define SER0_THR_ADDR       0x70006000  /* TX holding (write)            */
#define SER0_DLL_ADDR       0x70006000  /* divisor low  (when LCR.DLAB)  */
#define SER0_IER_ADDR       0x70006004  /* IRQ enable                    */
#define SER0_DLM_ADDR       0x70006004  /* divisor high (when LCR.DLAB)  */
#define SER0_IIR_ADDR       0x70006008  /* IRQ identification (read)     */
#define SER0_FCR_ADDR       0x70006008  /* FIFO control (write)          */
#define SER0_LCR_ADDR       0x7000600C  /* line control                  */
#define SER0_MCR_ADDR       0x70006010  /* modem control                 */
#define SER0_LSR_ADDR       0x70006014  /* line status                   */
#define SER0_MSR_ADDR       0x70006018  /* modem status                  */

/* SER0_LCR bits (08-boot-dock.md, "Init sequence"). */
#define SER0_LCR_DLAB       0x80        /* divisor latch access          */
#define SER0_LCR_8N1        0x03        /* 8 data, no parity, 1 stop     */

/* SER0_LSR bits (08-boot-dock.md, register table). */
#define SER0_LSR_RX_RDY     0x01        /* bit 0: RX data ready          */
#define SER0_LSR_THRE       0x20        /* bit 5: TX holding empty       */

/* Divisor latch values for the 24 MHz reference
 * (08-boot-dock.md, "Baud rate" table). DLM is 0 for all of these. */
#define SER0_DIV_9600       0x9C
#define SER0_DIV_19200      0x4E
#define SER0_DIV_38400      0x27
#define SER0_DIV_57600      0x1A
#define SER0_DIV_115200     0x0D

/* GPIO routing for SER0 on the iPod Video (08-boot-dock.md, "GPIO
 * routing for SER0 on iPod Video"): clear bits 2-3 of the routing
 * register at 0x7000008C (unnamed even in Rockbox, which uses the raw
 * literal), then clear the same bits in GPO32_ENABLE to release those
 * pads from general-purpose-output mode so the SER0 alternate function
 * drives them. GPO32 addresses verified against Rockbox pp5020.h
 * (2026-06-10). */
#define SER0_GPIO_ROUTE_ADDR  0x7000008C
#define SER0_GPIO_ROUTE_MASK  0x0000000C

#define GPO32_VAL_ADDR        0x70000080
#define GPO32_ENABLE_ADDR     0x70000084

/* SER0 interrupt: IRQ #36, i.e. high bank bit 4
 * (01-soc-pp5022.md, "IRQ numbers" table). Unused by the polled
 * driver; recorded for the future IRQ-driven RX path. */
#define SER0_IRQ            36

#ifndef __ASSEMBLER__
/* Word-wide accessors throughout: each 8250 byte register occupies a
 * 32-bit slot and Rockbox accesses them all as 32-bit words (verified
 * 2026-06-10 against pp5020.h / uart-pp.c) — we follow that precedent
 * rather than gamble on byte-lane behavior. */
#define SER0_RBR        PP_REG32(SER0_RBR_ADDR)
#define SER0_THR        PP_REG32(SER0_THR_ADDR)
#define SER0_DLL        PP_REG32(SER0_DLL_ADDR)
#define SER0_IER        PP_REG32(SER0_IER_ADDR)
#define SER0_DLM        PP_REG32(SER0_DLM_ADDR)
#define SER0_IIR        PP_REG32(SER0_IIR_ADDR)
#define SER0_FCR        PP_REG32(SER0_FCR_ADDR)
#define SER0_LCR        PP_REG32(SER0_LCR_ADDR)
#define SER0_MCR        PP_REG32(SER0_MCR_ADDR)
#define SER0_LSR        PP_REG32(SER0_LSR_ADDR)
#define SER0_MSR        PP_REG32(SER0_MSR_ADDR)

#define SER0_GPIO_ROUTE PP_REG32(SER0_GPIO_ROUTE_ADDR)
#define GPO32_VAL       PP_REG32(GPO32_VAL_ADDR)
#define GPO32_ENABLE    PP_REG32(GPO32_ENABLE_ADDR)
#endif

/* ---------- GPIO port C ----------------------------------------------
 * Used by the LCD host-side port init (02-lcd.md, "Host-side port
 * init"): GPIOC bit 6 is the BCM interrupt pin (configured as a GPIO
 * input), bit 7 is released to its alternate function. The doc's GPIO
 * sections carry no port-register addresses, so these come from
 * verified Rockbox pp5020.h facts (2026-06-11): ports A-D base
 * 0x6000D000, ENABLE at +0x00/04/08/0C per port A/B/C/D, OUTPUT_EN at
 * +0x10/14/18/1C. Only the two registers the LCD driver touches are
 * defined here.
 */

#define GPIOC_ENABLE_ADDR     0x6000D008
#define GPIOC_OUTPUT_EN_ADDR  0x6000D018

#ifndef __ASSEMBLER__
#define GPIOC_ENABLE     PP_REG32(GPIOC_ENABLE_ADDR)
#define GPIOC_OUTPUT_EN  PP_REG32(GPIOC_OUTPUT_EN_ADDR)
#endif

/* ---------- BCM video coprocessor (LCD) -------------------------------
 * core/docs/hw/02-lcd.md, "Memory-mapped BCM interface" + "Internal
 * BCM addresses" + "Commands", verified against Rockbox lcd-video.c /
 * ipodloader2 fb.c (2026-06-11).
 *
 * Each BCM "register" is a port: the BCM decodes only PP address bits
 * 16..18, low address bits are undecoded, so 16-bit and 32-bit
 * accesses to the same base address both work — a 32-bit store is
 * consumed as two consecutive 16-bit pushes (this is the pixel-stream
 * fast path). Address ports take 32-bit writes; status polls are
 * 16-bit reads.
 */

#define BCM_DATA_ADDR         0x30000000  /* data port (pixels/params)   */
#define BCM_WR_ADDR_ADDR      0x30010000  /* write-destination port      */
#define BCM_RD_ADDR_ADDR      0x30020000  /* read-source port            */
#define BCM_CONTROL_ADDR      0x30030000  /* status flags + cmd strobe   */
#define BCM_ALT_DATA_ADDR     0x30040000  /* alt channel (bootstrap only,*/
#define BCM_ALT_WR_ADDR_ADDR  0x30050000  /*   unused by this driver —   */
#define BCM_ALT_RD_ADDR_ADDR  0x30060000  /*   constants recorded for    */
#define BCM_ALT_CONTROL_ADDR  0x30070000  /*   the future bootstrap PR)  */

/* BCM_CONTROL bits / values (02-lcd.md, "BCM_CONTROL"). */
#define BCM_CONTROL_WR_READY  0x02        /* can accept write addr/data  */
#define BCM_CONTROL_RD_READY  0x10        /* data available on BCM_DATA32*/
#define BCM_CONTROL_STROBE    0x31        /* write: execute queued cmd   */

/* BCM_RD_ADDR (16-bit read) bit 0: read port ready to take an address
 * — poll this BEFORE writing the address (02-lcd.md, read handshake). */
#define BCM_RD_ADDR_READY     0x01

/* BCM-internal absolute addresses (02-lcd.md, "Internal BCM
 * addresses"). CMDPARAM doubles as the LCD_UPDATE framebuffer. */
#define BCMA_CMDPARAM         0xE0000
#define BCMA_COMMAND          0x1F8
#define BCMA_STATUS           0x1FC

/* GPO32 bit 14: BCM power rail. Nonzero in GPO32_VAL means the BCM is
 * powered (and, post-chainload, initialized) — the lcd_init probe
 * (02-lcd.md, "Host-side port init"). */
#define GPO32_BCM_POWER       0x4000

#ifndef __ASSEMBLER__
/* Command encoding: low half is the command, high half its bit
 * inverse, so the BCM's parser rejects corrupt writes (02-lcd.md,
 * "Commands"). BCM_CMD(0) = 0xFFFF0000 = LCD_UPDATE. */
#define BCM_CMD(x)            ((~(uint32_t)(x) << 16) | (uint32_t)(x))
#define BCMCMD_LCD_UPDATE     BCM_CMD(0)

/* 32-bit views: address ports + data fast path. */
#define BCM_DATA32      PP_REG32(BCM_DATA_ADDR)
#define BCM_WR_ADDR32   PP_REG32(BCM_WR_ADDR_ADDR)
#define BCM_RD_ADDR32   PP_REG32(BCM_RD_ADDR_ADDR)

/* 16-bit views: status polls + control strobe. */
#define BCM_RD_ADDR     PP_REG16(BCM_RD_ADDR_ADDR)
#define BCM_CONTROL     PP_REG16(BCM_CONTROL_ADDR)
#endif

#endif /* CORE_HAL_HW_PP5022_H */
