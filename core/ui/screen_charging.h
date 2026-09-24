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

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Palette (system-screens.jsx ChargingScreen tokens -> RGB565)
 *
 * Shared with screen_battery.c: the low-battery modal and the goodbye screen
 * are the charging screen's siblings — same dark field, same battery glyph,
 * same type colours — so they take the tokens from here rather than carrying
 * their own copies of the same six numbers.
 * ------------------------------------------------------------------------- */
#define CHG_BG      0x0861u      /* #0e0d0c near-black background            */
#define CHG_OUTLINE 0x5A89u      /* #5a5048 battery outline + nub            */
#define CHG_FILL    0xEF3Bu      /* #e8e4dd light fill (normal, not low)     */
#define CHG_GREEN   0x3E4Du      /* charging fill (oklch(0.78 0.16 145))     */
#define CHG_RED     0xDA46u      /* low-battery fill (oklch(0.65 0.18 30))   */
#define CHG_TEXT    0xEF3Bu      /* #e8e4dd big percent digits / headlines   */
#define CHG_UNIT    0xACF2u      /* #a89e92 muted "%" unit / body copy       */
#define CHG_MUTED   0x7B8Du      /* #7a736a muted status / "not charging"    */

/* At or above this percent, a stopped charger on external power means
 * "full", not "failed". Shared with the Battery page's status word. */
#define CHG_FULL_PCT 90

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

/*
 * The big battery glyph on its own: outline + terminal nub + a pct-
 * proportional inner fill in `fill`, at the charging screen's stroke/inset/
 * nub proportions (so it is the SAME glyph the charging screen draws — the
 * low-battery screens in screen_battery.c share it rather than carrying a
 * copy that would drift the next time the outline is retuned). (x, y, w, h)
 * is the outline box; the nub is drawn to its right, outside it. The four
 * outer corner pixels are knocked back to the charging background, so this
 * is only correct over that dark field. The minimum-8px stub rule applies:
 * pct 0 still draws a short fill, never a bare outline.
 */
void screen_charging_draw_battery(int x, int y, int w, int h, int pct,
                                  uint16_t fill);

/*
 * One muted caption under the status line — the numbers behind the big
 * percent: millivolts, the budget asked of the charger, what the cell has
 * done since the cable went in ("3912 mV · 500 mA · +38 mV in 12 min").
 * Drawn over the field screen_charging_render() painted; the caller builds
 * the string (it has the numbers) and passes "" or NULL for none.
 */
void screen_charging_note(const char *note);

/*
 * The Hold padlock, in the top-right corner of the dark field: the 8x10
 * glyph the status strip shows while Hold is on, in the muted tone, at the
 * corner ChargingScreen (system-screens.jsx) keeps for its tiny status token
 * and LockedScreen's "persistent corner lock" sits in. These screens have no
 * strip and no header, so the Hold banner has nowhere to land — this glyph is
 * the whole of what Hold shows here. Drawn over whichever sibling painted the
 * field (charging or low-battery); touches nothing outside its box.
 */
void screen_charging_lock_render(void);

#endif /* CORE_UI_SCREEN_CHARGING_H */
