/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/backlight.c — LCD backlight dimmer driver (PP5022,
 * GPIO-bit-banged multi-level LED driver).
 *
 * WHAT THE HARDWARE ACTUALLY IS
 * -----------------------------
 * The iPod Video 5G/5.5G backlight is NOT a PWM peripheral and does NOT
 * live on the BCM video coprocessor. It is an external multi-level
 * LED-driver IC (an "S-wire"/charge-pump style dimmer) driven by three
 * plain PP5022 GPIO lines (02-lcd.md, "Backlight"). The hardware facts
 * below — the GPIO lines, the pulse protocol, the 32-level count — were
 * cross-referenced against Rockbox firmware/target/arm/ipod/
 * backlight-nano_video.c + firmware/export/pp5020.h (2026-07-21; the
 * shared Nano-1G/Video-5G backlight target, i.e. exactly our SoC). Only
 * those facts were taken — no code body is copied; the driver is our own:
 *
 *   GPIOB bit 3 (0x08)  circuit power   — powers the boost/charge pump
 *   GPIOD bit 7 (0x80)  step/clock line — pulses walk the level up/down
 *   GPIOL bit 7 (0x80)  LED enable      — fast on/off (no level change)
 *
 * The dimmer has 32 discrete levels and NO absolute-set interface: you
 * can only nudge it one step brighter or dimmer, so we must track the
 * current level in software. The chip resets to a mid level (16) when the
 * circuit is powered up — the one absolute reference we get.
 *
 * THE PULSE PROTOCOL (the load-bearing timing)
 * --------------------------------------------
 * With GPIOD idling HIGH, a LOW pulse on GPIOD steps the level, and the
 * DIRECTION is encoded by the low-pulse WIDTH:
 *   - a SHORT low pulse (~10 us)  -> one step BRIGHTER
 *   - a LONG  low pulse (~200 us) -> one step DIMMER
 * Each pulse is followed by a short (~10 us) high gap before the next.
 * The low phase is done with IRQs masked: an ISR stretching a "short"
 * low past the chip's threshold would be misread as a dim-step (the same
 * reason Rockbox wraps each low phase in disable_irq_save/restore).
 *
 * GPIO WRITE MECHANISM
 * --------------------
 * Every GPIO update uses the atomic bit set/clear alias at
 * +GPIO_BITWISE_OFFSET (single masked 32-bit write, no read-modify-write;
 * see pp5022.h). This keeps each op one bus write — clean to trace and
 * race-free against the LCD driver poking a different port.
 *
 * CONFIDENCE / DEVICE-GATED
 * -------------------------
 * The register addresses and the pulse protocol are transcribed facts
 * (high confidence). What is NOT verifiable off-device: the exact
 * microsecond pulse widths. We approximate them with bounded busy-loops
 * (no timer dependency, no libc) whose SHORT:LONG ratio (~1:20) matches
 * the up/down asymmetry; the absolute widths are best-effort and must be
 * confirmed on real hardware. If a level ends up wrong on-device, the
 * pulse-width calibration (BL_LOOPS_*) is suspect #1.
 *
 * Freestanding-clean: no libc, fixed-width types via pp5022.h, hardware
 * only through the mmio.h seam so the trace test can compile this file
 * host-side under -DMMIO_MOCK.
 */

#include "pp5022.h"
#include "mmio.h"
#include "backlight.h"

#ifndef MMIO_MOCK
#include "../../kernel/irq.h"   /* arch_irq_save/restore: the low phase of
                                 * each dimmer pulse is a critical section
                                 * (real build only; the host trace test
                                 * builds with -DMMIO_MOCK where the CPSR asm
                                 * can't compile). */
#define BL_IRQ_ENTER()  uint32_t bl_irq_saved_ = arch_irq_save()
#define BL_IRQ_EXIT()   arch_irq_restore(bl_irq_saved_)
#else
#define BL_IRQ_ENTER()  ((void)0)
#define BL_IRQ_EXIT()   ((void)0)
#endif

/* Backlight GPIO bit assignments (02-lcd.md, "Backlight"). */
#define BL_GPIOB_POWER  0x08    /* GPIOB bit 3: circuit power             */
#define BL_GPIOD_STEP   0x80    /* GPIOD bit 7: step/clock line           */
#define BL_GPIOL_LED    0x80    /* GPIOL bit 7: LED enable (on/off)       */

/* Level the driver IC powers up at, before any stepping — the absolute
 * reference for the relative walk (Rockbox current_dim = 16 after
 * enable). */
#define BL_DIM_RESET    16

/*
 * Busy-loop iteration counts approximating the pulse timing. These are
 * DEVICE-GATED best-effort values, not measured: at the boot/UI clock a
 * volatile-loop trip is a few CPU cycles, so ~200 short trips lands in
 * the ~10 us ballpark and the long low is ~20x that. Only the ratio is
 * protocol-critical; the absolute widths need on-hardware confirmation
 * (see file header). Kept as bounded loops on purpose: no timer driver
 * dependency, no libc.
 */
#define BL_LOOPS_SHORT  200u            /* ~10 us  low pulse -> step up   */
#define BL_LOOPS_LONG   (BL_LOOPS_SHORT * 20u) /* ~200 us low -> step down */
#define BL_LOOPS_GAP    200u            /* ~10 us  high gap between pulses */
#define BL_LOOPS_SETTLE 20000u          /* ~ms-scale power-up settle      */

/* Tracked dimmer level (1..BACKLIGHT_MAX). Undefined until backlight_init
 * seeds it from the BL_DIM_RESET power-up reference. */
static int bl_level = BACKLIGHT_MAX;

/* Bounded settling delay. volatile so the compiler can't elide it; no
 * MMIO, so it is invisible to the trace test (which asserts only the
 * register grammar, not wall-clock timing). */
static void bl_delay(volatile uint32_t loops)
{
    while (loops-- != 0) {
        /* spin */
    }
}

/* Atomic masked set of `mask` in the GPIO OUTPUT_VAL at `val_addr`
 * (single write to the +0x800 alias; see pp5022.h). */
static void bl_gpio_set(uint32_t val_addr, uint32_t mask)
{
    mmio_write32(val_addr + GPIO_BITWISE_OFFSET, (mask << 8) | mask);
}

/* Atomic masked clear of `mask` (single write to the +0x800 alias). */
static void bl_gpio_clear(uint32_t val_addr, uint32_t mask)
{
    mmio_write32(val_addr + GPIO_BITWISE_OFFSET, mask << 8);
}

/*
 * Emit one dimmer step: a low pulse on GPIOD whose width picks direction.
 * `low_loops` = BL_LOOPS_SHORT steps brighter, BL_LOOPS_LONG steps dimmer.
 * The low phase runs with IRQs masked so an interrupt can't stretch a
 * short pulse into a long one (misreading a brighten as a dim).
 */
static void bl_pulse(uint32_t low_loops)
{
    BL_IRQ_ENTER();
    bl_gpio_clear(GPIOD_OUTPUT_VAL_ADDR, BL_GPIOD_STEP);
    bl_delay(low_loops);
    bl_gpio_set(GPIOD_OUTPUT_VAL_ADDR, BL_GPIOD_STEP);
    BL_IRQ_EXIT();
    bl_delay(BL_LOOPS_GAP);
}

/* Walk the tracked level to `target` (already clamped to 1..MAX) with the
 * minimum number of directional pulses. */
static void bl_step_to(int target)
{
    while (bl_level < target) {
        bl_pulse(BL_LOOPS_SHORT);   /* short low: one step brighter */
        bl_level++;
    }
    while (bl_level > target) {
        bl_pulse(BL_LOOPS_LONG);    /* long low: one step dimmer */
        bl_level--;
    }
}

void backlight_init(void)
{
    /* Route the three lines to GPIO output (ENABLE + OUTPUT_EN), in the
     * Rockbox init order: B (power), D (step), then — after the circuit
     * is live and stepped — L (LED). */
    bl_gpio_set(GPIOB_ENABLE_ADDR,    BL_GPIOB_POWER);
    bl_gpio_set(GPIOB_OUTPUT_EN_ADDR, BL_GPIOB_POWER);
    bl_gpio_set(GPIOD_ENABLE_ADDR,    BL_GPIOD_STEP);
    bl_gpio_set(GPIOD_OUTPUT_EN_ADDR, BL_GPIOD_STEP);

    /* Power the charge pump and idle the step line HIGH, then let the
     * boost rail settle before pulsing. */
    bl_gpio_set(GPIOB_OUTPUT_VAL_ADDR, BL_GPIOB_POWER);
    bl_gpio_set(GPIOD_OUTPUT_VAL_ADDR, BL_GPIOD_STEP);
    bl_delay(BL_LOOPS_SETTLE);

    /* The chip is now at its power-up reference level; walk it to full. */
    bl_level = BL_DIM_RESET;
    bl_step_to(BACKLIGHT_MAX);

    /* Finally enable the LED output. */
    bl_gpio_set(GPIOL_ENABLE_ADDR,     BL_GPIOL_LED);
    bl_gpio_set(GPIOL_OUTPUT_EN_ADDR,  BL_GPIOL_LED);
    bl_gpio_set(GPIOL_OUTPUT_VAL_ADDR, BL_GPIOL_LED);
}

void backlight_set(int level)
{
    if (level <= 0) {
        /* Off: drop the LED enable. The circuit stays powered and the
         * tracked level is preserved, so a later non-zero set relights
         * instantly without re-walking the dimmer. */
        bl_gpio_clear(GPIOL_OUTPUT_VAL_ADDR, BL_GPIOL_LED);
        return;
    }
    if (level > BACKLIGHT_MAX) {
        level = BACKLIGHT_MAX;
    }

    /* Ensure the LED is on (it may have been turned off), then step the
     * dimmer to the requested level. */
    bl_gpio_set(GPIOL_OUTPUT_VAL_ADDR, BL_GPIOL_LED);
    bl_step_to(level);
}
