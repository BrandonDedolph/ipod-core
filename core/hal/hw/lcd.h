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

#endif /* CORE_HAL_HW_LCD_H */
