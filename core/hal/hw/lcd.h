/*
 * core/hal/hw/lcd.h — BCM video coprocessor LCD driver (hw target only).
 *
 * Phase 1 surface: init + solid-color present. The full hal.h contract
 * (lcd_framebuffer / lcd_present over a host-side back buffer) sits on
 * top of this in a later PR; for bring-up we only need to prove the
 * BCM command path end to end.
 *
 * Not part of the portable HAL contract (hal.h) — sim has no BCM.
 */

#ifndef CORE_HAL_HW_LCD_H
#define CORE_HAL_HW_LCD_H

#include <stdint.h>

/* Host-side LCD port init (GPO32/GPIOC setup; no BCM bootstrap — the
 * chainload handoff guarantees the BCM is already powered, awake and
 * idle, see core/docs/hw/02-lcd.md "Chainload handoff state").
 * Returns nonzero if the BCM power-rail probe (GPO32_VAL bit 0x4000)
 * reads powered, 0 if not. Call once before lcd_fill(). */
int lcd_init(void);

/* Fill the entire 320x240 panel with one RGB565 color and present it
 * (full-frame BCMCMD_LCD_UPDATE, bootloader variant: returns without
 * waiting for completion). */
void lcd_fill(uint16_t rgb565);

/* Present a full host-side framebuffer to the panel: stream all
 * LCD_WIDTH*LCD_HEIGHT RGB565 pixels of `fb` (row-major, same layout
 * lcd_fill writes) via the full-frame BCMCMD_LCD_UPDATE fast path,
 * bootloader variant (returns without waiting for completion). `fb`
 * points to exactly LCD_WIDTH*LCD_HEIGHT uint16_t. Like lcd_fill, the
 * caller must have gated on lcd_init() reporting the BCM powered before
 * calling (the driver does no dead-BCM bootstrap).
 *
 * NOTE: this is the hw-only worker, deliberately NOT named lcd_present
 * — the portable HAL contract (hal.h) reserves `void lcd_present(void)`
 * for the back-buffer present that lands with lcd_framebuffer() in a
 * later PR; that entry point will simply forward
 * lcd_present_fb(lcd_framebuffer()). Naming it lcd_present here would
 * collide with hal.h (lcd.c includes both). */
void lcd_present_fb(const uint16_t *fb);

#endif /* CORE_HAL_HW_LCD_H */
