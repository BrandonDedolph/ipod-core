/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/screen_charging.h — full-screen "charging" battery view.
 *
 * The screen shown when the iPod is plugged in and otherwise idle/off
 * (design_reference/system-screens.jsx ChargingScreen): a near-black
 * field with a big horizontal battery whose fill tracks the charge level,
 * a lightning bolt cut into the fill while charging, a large percent
 * number, and a short status line.
 *
 * Pure rendering: it draws the whole 320x240 panel into the console
 * framebuffer and returns. No hardware reads and no present — the caller
 * samples pct/charging/external (kernel/main.c battery_refresh) and hands
 * the framebuffer to lcd_present_fb(). Freestanding, integer-only, no
 * libc/libm/malloc — same idioms as the rest of core/ui.
 */

#ifndef CORE_UI_SCREEN_CHARGING_H
#define CORE_UI_SCREEN_CHARGING_H

/*
 * Render the charging screen into the console framebuffer.
 *
 *   pct       battery charge 0..100 (clamped internally; <0 treated as 0).
 *   charging  nonzero while the pack is actively charging (green fill +
 *             lightning bolt + "CHARGING").
 *   external  nonzero when external power (USB/FireWire) is present. When
 *             zero the status line prompts "CONNECT CABLE".
 */
void screen_charging_render(int pct, int charging, int external);

#endif /* CORE_UI_SCREEN_CHARGING_H */
