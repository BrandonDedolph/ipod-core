/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/screen_battery.h — the low-battery warnings: what the user SEES.
 *
 * WHY THIS FILE EXISTS
 *
 * The low-battery policy (hal/hw/battery.h) already does the right thing to
 * the DATA: at 3500 mV filtered it flushes the settings record and parks the
 * drive, and at 3300 mV confirmed it powers the device off. What it never did
 * was tell the person holding the iPod. The drive parked, the screen went
 * black, and the device was simply off — from the outside indistinguishable
 * from a crash. And the "Settings will not be saved" state lasted for minutes
 * with no indication that a volume change or a theme pick was being quietly
 * held in RAM.
 *
 * This module is the UI half of that policy. Three levels:
 *
 *   LOW       a transient toast ("Battery low / Plug in soon"), shown ONCE as
 *             the filtered cell crosses BATTWARN_MV_LOW. UI-only: the policy
 *             has no such level, because nothing needs to be DONE at 8% — the
 *             person just deserves to know before the modal below.
 *   DISKSAFE  a full-screen modal on the policy's OK->DISKSAFE edge, sticky
 *             until any button, explaining that settings are not being saved.
 *   SHUTOFF   a full-screen goodbye on the SHUTOFF edge, not dismissable —
 *             kernel/main.c holds it on the panel for a moment before
 *             entering standby, so the last thing on the screen says why.
 *
 * THE LOAD-BEARING PART IS NOT THE DRAWING, IT IS THE FLAP PREVENTION. The
 * cell sags by tens to low hundreds of millivolts under a drive spin-up and
 * recovers when playback pauses, and in the 3720..3840 mV plateau the curve
 * is 30 mV per 10 %. A naive "show while below" would put a toast up and take
 * it down on every refill burst. So:
 *
 *   1. The input is the policy's FILTERED millivolts (median of 5 at 5 s), and
 *      only once the filter is ready. A 2 s sag lands on one or two samples
 *      and moves a median of five by zero.
 *   2. LOW is a Schmitt trigger: it fires at <= BATTWARN_MV_LOW and re-arms
 *      only at >= BATTWARN_MV_LOW_CLEAR, 100 mV higher. A rebound smaller
 *      than that cannot re-arm it.
 *   3. LOW is a LATCH, one show per arming, not a level. Oscillating between
 *      3690 and 3790 forever produces exactly one toast.
 *   4. DISKSAFE and SHUTOFF are driven by the policy's own edges, which already
 *      carry 100 mV of hysteresis and a 15 s confirm. The modal shows once per
 *      OK->DISKSAFE edge, is latched until dismissed, and the latch clears
 *      only on the DISKSAFE->OK recovery.
 *   5. External power hides LOW and DISKSAFE: a cable is the answer to both.
 *      Pulling the cable while the policy is still at DISKSAFE re-raises the
 *      modal, even one that was dismissed before the plug-in.
 *   6. The toast's 4 s run from the first PAINT, not from the sample that
 *      fired it: the caller reports the paint (battwarn_toast_shown), so a
 *      toast that fires behind a dark backlight is still waiting when the
 *      panel comes back rather than having expired unseen.
 *
 * The state machine (battwarn_*) is PURE — the caller supplies the sample and
 * the clock — so the whole sequence above is proven on the host in
 * tests/ui/screen_battery_test.c, including the 4 s toast window crossing the
 * 32-bit microsecond timer wrap.
 *
 * The renderers follow screen_charging.c exactly: draw into the console
 * framebuffer, no present, no hardware reads, NO framebuffer save. The full-
 * screen renders own the whole panel (the caller pushes a screen and the
 * underlying one repaints on pop); the toast paints a plate over whatever is
 * in the framebuffer and reports only that plate as damage, which is what
 * makes a band-only present of it legal.
 *
 * Depends on text.h, chrome.h, screen_charging.h (the shared battery glyph
 * and palette), ../kernel/console.h and ../hal/hal.h for LCD_WIDTH/HEIGHT.
 * No hw/, no MMIO, no battery.h: the policy level arrives as a plain int whose
 * values are those of battery_level_t (0 OK, 1 DISKSAFE, 2 SHUTOFF) — see the
 * note beside that enum.
 */

#ifndef CORE_UI_SCREEN_BATTERY_H
#define CORE_UI_SCREEN_BATTERY_H

#include <stdint.h>

/* What, if anything, the warning UI wants on the panel. NONE means the normal
 * screen shows through (a LOW toast may still be up over it — that is a
 * separate, timed query, battwarn_toast_up()). */
typedef enum {
    BATTWARN_NONE = 0,
    BATTWARN_LOW,           /* the toast; a render kind, never a screen       */
    BATTWARN_DISKSAFE,      /* full-screen modal, dismissed by any button     */
    BATTWARN_SHUTOFF        /* full-screen goodbye, terminal                  */
} battwarn_kind_t;

/* ---------- Renderers (pure: framebuffer only, NO present) ------------- */

/*
 * Full-panel render for BATTWARN_DISKSAFE or BATTWARN_SHUTOFF: clears to the
 * charging screen's dark field, draws the battery glyph with a red stub, and
 * the copy for that level. Any other kind draws the DISKSAFE screen (there is
 * no sensible full-screen for NONE/LOW, and a blank panel would be worse).
 */
void screen_battery_render(battwarn_kind_t kind);

/*
 * The LOW toast: a 200x36 Linen plate at (60, 101) — the volume overlay's
 * position and width, so the two transient plates sit in the same place —
 * painted OVER the current framebuffer contents. Reports exactly the plate
 * rectangle as damage and touches nothing outside it.
 */
void screen_battery_toast_render(void);

/* Plate geometry, public so the caller can present just that band. */
#define BATTWARN_TOAST_X   60
#define BATTWARN_TOAST_Y   101
#define BATTWARN_TOAST_W   200
#define BATTWARN_TOAST_H   36

/* ---------- State machine (pure: the caller supplies the clock) ---------- */

/*
 * LOW thresholds on the FILTERED millivolts. These are a REASONED CHOICE ON
 * AN UNCALIBRATED CURVE, not a measurement: the percent curve in battery.c is
 * transcribed from the 2005 Apple cell and has never been checked against the
 * fitted one. On that curve 3700 mV is ~8 % and 3800 mV is ~37 %, which sits
 * the toast a little above DISKSAFE (3500) with a wide enough gap that the
 * modal is not on its heels. A wrong number here mis-times a toast; it can
 * never mis-time a shutdown, which lives in battery.h and is not touched.
 */
#define BATTWARN_MV_LOW        3700   /* toast fires at or below this        */
#define BATTWARN_MV_LOW_CLEAR  3800   /* ...and re-arms only at or above this */

/* How long the toast stays up, in microseconds of the caller's clock. */
#define BATTWARN_TOAST_US      4000000u

/* Forget everything: no toast, no modal, LOW armed, level OK. Boot state. */
void battwarn_reset(void);

/*
 * Feed one policy sample. Call ONCE per policy evaluation (every 5 s), right
 * after battery_policy_feed():
 *
 *   filt_mv   battery_filtered_mv() if battery_filter_ready(), else -1. A
 *             negative value means "no usable reading" and cannot move the
 *             LOW trigger in either direction; level edges still apply.
 *   level     battery_policy_level() as an int: 0 OK, 1 DISKSAFE, 2 SHUTOFF.
 *   external  power_is_external(): nonzero hides the toast and the modal.
 *   now_us    the caller's microsecond clock (wraps every ~71 min; fine).
 */
void battwarn_feed(int filt_mv, int level, int external, uint32_t now_us);

/*
 * Any user input: hides the toast, dismisses the DISKSAFE modal. Does NOT
 * clear SHUTOFF — nothing does. The caller should also swallow the press so a
 * dismissal cannot double as a transport command.
 */
void battwarn_input(uint32_t now_us);

/* Which full screen, if any, should be on top: NONE, DISKSAFE or SHUTOFF
 * (never LOW). */
battwarn_kind_t battwarn_screen(void);

/*
 * 1 while the LOW toast should be painted. Elapsed-time compare against the
 * show stamp, and the toast DISARMS ITSELF on expiry, so a clock wrap 71 min
 * later can never bring a stale one back (the ui_window_t rule in main.c).
 * Until the caller has reported a paint (below) it stays 1 regardless of the
 * clock: an unseen toast has not started its 4 s.
 */
int battwarn_toast_up(uint32_t now_us);

/*
 * The caller PAINTED the toast: call right after screen_battery_toast_render()
 * with the same clock. The first call after a show stamps the 4 s window;
 * later calls are no-ops, so it is safe on every repaint.
 */
void battwarn_toast_shown(uint32_t now_us);

#endif /* CORE_UI_SCREEN_BATTERY_H */
