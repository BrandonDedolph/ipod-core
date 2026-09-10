/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ui/screen_battery_test.c — the low-battery warning UI on the host.
 *
 * Two things are proven here, and the second is the one that matters.
 *
 * RENDERS. The framebuffer is the oracle (as in chrome_test.c): a sentinel
 * colour is painted first, and the assertions read pixels back. The full-
 * screen renders must own the whole panel (no sentinel left), keep the red
 * fill inside the battery, and put type where the copy bands are. The toast
 * must touch NOTHING outside its plate and report exactly the plate as damage
 * — that is the property that lets the caller present only that band while
 * audio is playing, and the one a stray glyph descender would silently break.
 *
 * STATE MACHINE. battwarn_* is pure — sample, level, cable and clock all come
 * from the caller — so the entire flap-prevention argument in
 * screen_battery.h is executed, not just described: a sagging-and-recovering
 * cell produces one toast, not a flicker; the modal follows the policy's
 * edges and only those; a cable hides everything; SHUTOFF is terminal. The
 * clock starts at 0xFFFFFF00 so the 4 s toast window straddles the 32-bit
 * microsecond wrap, and a stale toast is shown NOT to come back one wrap
 * later. Every one of these would be a real on-device bug that no amount of
 * looking at the screen would find in a reasonable time.
 */

#include <stdio.h>
#include <string.h>

#include "screen_battery.h"
#include "screen_charging.h"
#include "chrome.h"
#include "palette.h"
#include "text.h"
#include "console.h"
#include "hal.h"
#include "hw/battery.h"          /* battery_level_t: the int contract only */

#include "../xfail.h"

#define FB_PIXELS (LCD_WIDTH * LCD_HEIGHT)
#define SENTINEL  0xF81Fu        /* magenta: in no palette this UI uses */

/* screen_battery.h passes the policy level as a plain int and promises the
 * values are battery_level_t's. Check it where the compiler can see both,
 * so a reordering of the enum breaks the build rather than the warning. */
_Static_assert(BATTERY_LEVEL_OK == 0 && BATTERY_LEVEL_DISKSAFE == 1 &&
               BATTERY_LEVEL_SHUTOFF == 2,
               "battwarn_feed() level contract: 0 OK, 1 DISKSAFE, 2 SHUTOFF");

/* Count pixels equal to `c` inside the box (x,y,w,h). */
static int count_in(int x, int y, int w, int h, uint16_t c)
{
    const uint16_t *fb = console_framebuffer();
    int n = 0;
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            if (yy < 0 || xx < 0 || yy >= LCD_HEIGHT || xx >= LCD_WIDTH) continue;
            if (fb[yy * LCD_WIDTH + xx] == c) n++;
        }
    }
    return n;
}

/* Pixels equal to `c` ANYWHERE outside the box — the out-of-bounds detector. */
static int count_outside(int x, int y, int w, int h, uint16_t c)
{
    const uint16_t *fb = console_framebuffer();
    int n = 0;
    for (int yy = 0; yy < LCD_HEIGHT; yy++) {
        for (int xx = 0; xx < LCD_WIDTH; xx++) {
            int inside = (xx >= x && xx < x + w && yy >= y && yy < y + h);
            if (!inside && fb[yy * LCD_WIDTH + xx] == c) n++;
        }
    }
    return n;
}

/* Pixels that are NOT the background inside a band — "is there type here". */
static int count_not(int x, int y, int w, int h, uint16_t bg)
{
    return w * h - count_in(x, y, w, h, bg);
}

/* Battery glyph box on the full screens (mirrors screen_battery.c). */
#define BATT_W 150
#define BATT_H 68
#define BATT_X ((LCD_WIDTH - BATT_W) / 2)
#define BATT_Y 56

/* One policy sample every 5 s, the cadence kernel/main.c actually uses. */
#define STEP 5000000u

/* Feed a sample and report whether the toast came UP on this very sample:
 * the "shows exactly once" assertions count these, not the level. */
static int feed_shows(int mv, int level, int ext, uint32_t now)
{
    int before = battwarn_toast_up(now);
    battwarn_feed(mv, level, ext, now);
    int after = battwarn_toast_up(now);
    return !before && after;
}

int main(void)
{
    xfail_ctx c = { "screen_battery", 0, 0, 0 };
    theme_set(THEME_LINEN);

    /* ---- 1. DISKSAFE render ------------------------------------------- */
    console_clear(SENTINEL);
    console_damage_reset();
    screen_battery_render(BATTWARN_DISKSAFE);
    xpect(&c, "disksafe paints the whole panel (no sentinel survives)",
          count_in(0, 0, LCD_WIDTH, LCD_HEIGHT, SENTINEL) == 0);
    xpect(&c, "disksafe draws the red stub inside the battery",
          count_in(BATT_X, BATT_Y, BATT_W, BATT_H, CHG_RED) > 0);
    xpect(&c, "disksafe draws no red anywhere else",
          count_outside(BATT_X, BATT_Y, BATT_W, BATT_H, CHG_RED) == 0);
    xpect(&c, "disksafe draws the outline",
          count_in(BATT_X, BATT_Y, BATT_W, BATT_H, CHG_OUTLINE) > 0);
    /* Headline band: bold 13 at baseline 150 -> rows 136..155. */
    xpect(&c, "disksafe puts headline ink in the headline band",
          count_in(0, 136, LCD_WIDTH, 20, CHG_TEXT) > 0);
    /* Nothing but background between the glyph and the headline: the copy
     * must not creep up into the glyph. */
    xpect(&c, "disksafe leaves the gap under the glyph empty",
          count_not(0, BATT_Y + BATT_H, LCD_WIDTH, 136 - (BATT_Y + BATT_H),
                    CHG_BG) == 0);
    /* Hint band: regular 9 at 218 -> rows 208..222; it exists and is muted. */
    xpect(&c, "disksafe draws the dismiss hint below the copy",
          count_not(0, 206, LCD_WIDTH, 18, CHG_BG) > 0);
    xpect(&c, "disksafe writes nothing under the hint",
          count_not(0, 224, LCD_WIDTH, LCD_HEIGHT - 224, CHG_BG) == 0);
    {
        int dx, dy, dw, dh;
        int any = console_damage_get(&dx, &dy, &dw, &dh);
        xpect(&c, "disksafe damage is the full panel",
              any && dx == 0 && dy == 0 && dw == LCD_WIDTH && dh == LCD_HEIGHT);
    }
    /* Keep a copy of the copy band for the SHUTOFF diff. */
    static uint16_t disksafe_band[LCD_WIDTH * 86];
    memcpy(disksafe_band, console_framebuffer() + 140 * LCD_WIDTH,
           sizeof disksafe_band);

    /* ---- 2. SHUTOFF render -------------------------------------------- */
    console_clear(SENTINEL);
    screen_battery_render(BATTWARN_SHUTOFF);
    xpect(&c, "shutoff paints the whole panel",
          count_in(0, 0, LCD_WIDTH, LCD_HEIGHT, SENTINEL) == 0);
    xpect(&c, "shutoff draws the red stub inside the battery",
          count_in(BATT_X, BATT_Y, BATT_W, BATT_H, CHG_RED) > 0 &&
          count_outside(BATT_X, BATT_Y, BATT_W, BATT_H, CHG_RED) == 0);
    xpect(&c, "shutoff puts headline ink in the headline band",
          count_in(0, 136, LCD_WIDTH, 20, CHG_TEXT) > 0);
    {
        /* The two screens must not be the same picture: a copy-paste that
         * left the SHUTOFF branch drawing DISKSAFE text would say "press any
         * button" on a device that is about to power off. */
        const uint16_t *band = console_framebuffer() + 140 * LCD_WIDTH;
        int diff = 0;
        for (int i = 0; i < LCD_WIDTH * 86; i++) {
            if (band[i] != disksafe_band[i]) diff++;
        }
        xpect(&c, "shutoff copy differs from disksafe copy (rows 140..225)",
              diff > 0);
    }
    /* No dismiss hint on a screen nothing can dismiss. */
    xpect(&c, "shutoff has no hint line",
          count_not(0, 206, LCD_WIDTH, 34, CHG_BG) == 0);

    /* ---- 3. Toast render ---------------------------------------------- */
    console_clear(SENTINEL);
    console_damage_reset();
    screen_battery_toast_render();
    {
        const int PX = BATTWARN_TOAST_X, PY = BATTWARN_TOAST_Y;
        const int PW = BATTWARN_TOAST_W, PH = BATTWARN_TOAST_H;
        xpect(&c, "toast plate colour appears nowhere outside the plate",
              count_outside(PX, PY, PW, PH, LINEN_PLATE) == 0);
        /* THE band-present guarantee: every pixel outside the plate is still
         * the sentinel, i.e. the toast touched nothing else. */
        xpect(&c, "toast leaves every pixel outside its plate untouched",
              count_outside(PX, PY, PW, PH, SENTINEL) == FB_PIXELS - PW * PH);
        xpect(&c, "toast plate is mostly plate colour (rounded corners aside)",
              count_in(PX, PY, PW, PH, LINEN_PLATE) > (PW * PH) / 2);
        xpect(&c, "toast headline ink is on the plate",
              count_in(PX, PY, PW, PH, LINEN_INK) > 0);
        xpect(&c, "toast battery glyph shows its accent stub",
              count_in(PX + 14, PY + 12, 22, 12, LINEN_ACCENT) > 0 &&
              count_outside(PX + 14, PY + 12, 22, 12, LINEN_ACCENT) == 0);
        int dx, dy, dw, dh;
        int any = console_damage_get(&dx, &dy, &dw, &dh);
        xpect(&c, "toast reports damage", any == 1);
        xpect(&c, "toast damage rect is contained in the plate",
              dx >= PX && dy >= PY && dx + dw <= PX + PW && dy + dh <= PY + PH);
        xpect(&c, "toast damage rect covers the plate (a band present is enough)",
              dx == PX && dy == PY && dw == PW && dh == PH);
    }
    /* The toast must be legible on the dark theme too: it is the ONE render
     * here that takes the live palette. */
    theme_set(THEME_ONYX);
    console_clear(SENTINEL);
    screen_battery_toast_render();
    xpect(&c, "toast under Onyx still touches only its plate",
          count_outside(BATTWARN_TOAST_X, BATTWARN_TOAST_Y, BATTWARN_TOAST_W,
                        BATTWARN_TOAST_H, SENTINEL)
              == FB_PIXELS - BATTWARN_TOAST_W * BATTWARN_TOAST_H);
    theme_set(THEME_LINEN);

    /* ---- 4. State machine --------------------------------------------- */
    /* 256 us before the timer wraps: the toast shown at T0 expires AFTER the
     * wrap, so every elapsed-time compare below has to be the wrap-safe
     * (uint32_t)(now - t0) form or it fails right here. */
    uint32_t T0 = 0xFFFFFF00u;
    uint32_t now = T0 - 6 * STEP;
    int shows = 0;

    battwarn_reset();
    xpect(&c, "reset: no screen, no toast",
          battwarn_screen() == BATTWARN_NONE && !battwarn_toast_up(now));

    /* Not-ready reading: a discharging device at boot must not toast on -1. */
    battwarn_feed(-1, 0, 0, now); now += STEP;
    xpect(&c, "filter-not-ready sample fires nothing", !battwarn_toast_up(now));

    /* Ramp down through the plateau; nothing until 3700 is crossed. */
    const int ramp[] = { 3900, 3850, 3800, 3750, 3720 };
    for (unsigned i = 0; i < sizeof ramp / sizeof ramp[0]; i++) {
        shows += feed_shows(ramp[i], 0, 0, now); now += STEP;
    }
    xpect(&c, "no toast above 3700 on the way down", shows == 0);

    now = T0;
    shows += feed_shows(3690, 0, 0, now);
    xpect(&c, "toast shows at the first <=3700 sample", shows == 1);
    xpect(&c, "toast is a toast, not a screen",
          battwarn_screen() == BATTWARN_NONE);

    /* The toast window crosses the wrap. 3.9 s later the clock reads
     * 0x003B8480-ish: numerically BEFORE T0. */
    xpect(&c, "toast still up 3.9 s later, across the wrap",
          battwarn_toast_up(T0 + 3900000u) == 1);
    xpect(&c, "toast still up at 3999999 us", battwarn_toast_up(T0 + 3999999u));
    xpect(&c, "toast down at exactly 4 s", battwarn_toast_up(T0 + 4000000u) == 0);
    /* Expiry disarmed it: the same numeric instant one wrap later (or the
     * moment we passed at 3.9 s) must not resurrect it. */
    xpect(&c, "an expired toast does not come back at a stale-looking clock",
          battwarn_toast_up(T0 + 3900000u) == 0 && battwarn_toast_up(T0) == 0);

    /* Sag and recover under 100 mV, forever: no second show. This is the
     * spin-up flicker the whole latch exists to prevent. */
    now = T0 + STEP;
    for (int i = 0; i < 20; i++) {
        shows += feed_shows((i & 1) ? 3720 : 3650, 0, 0, now); now += STEP;
    }
    xpect(&c, "oscillating 3650/3720 never re-shows the toast", shows == 1);
    /* 3790 is above LOW but below CLEAR: still no re-arm. */
    shows += feed_shows(3790, 0, 0, now); now += STEP;
    shows += feed_shows(3690, 0, 0, now); now += STEP;
    xpect(&c, "a rebound short of 3800 does not re-arm", shows == 1);

    /* A genuine climb to >= 3800 re-arms; the next crossing shows again. */
    shows += feed_shows(3810, 0, 0, now); now += STEP;
    xpect(&c, "re-arming at 3810 does not itself toast", shows == 1);
    shows += feed_shows(3700, 0, 0, now);
    xpect(&c, "after re-arm, crossing 3700 shows again", shows == 2);
    xpect(&c, "...and it is up", battwarn_toast_up(now) == 1);

    /* Input hides it early. */
    battwarn_input(now + 1000);
    xpect(&c, "input hides the toast early", battwarn_toast_up(now + 1000) == 0);
    now += STEP;

    /* A -1 reading while a toast is up changes nothing. */
    shows += feed_shows(3810, 0, 0, now); now += STEP;   /* re-arm */
    shows += feed_shows(3690, 0, 0, now);                 /* show #3 */
    xpect(&c, "setup: third show", shows == 3 && battwarn_toast_up(now));
    battwarn_feed(-1, 0, 0, now + 1);
    xpect(&c, "a -1 reading leaves a live toast alone",
          battwarn_toast_up(now + 1) == 1 && battwarn_screen() == BATTWARN_NONE);
    now += STEP;

    /* Policy edge OK -> DISKSAFE: the modal, and the toast goes with it. */
    shows += feed_shows(3810, 0, 0, now); now += STEP;
    shows += feed_shows(3690, 0, 0, now);                 /* toast up again */
    xpect(&c, "setup: toast up before the edge", battwarn_toast_up(now));
    battwarn_feed(3500, 1, 0, now + STEP); now += STEP;
    xpect(&c, "OK->DISKSAFE edge raises the modal",
          battwarn_screen() == BATTWARN_DISKSAFE);
    xpect(&c, "the modal supersedes the toast", battwarn_toast_up(now) == 0);

    /* Time passes at level 1: the modal stays (sticky until a press). */
    battwarn_feed(3480, 1, 0, now); now += STEP;
    battwarn_feed(3470, 1, 0, now); now += STEP;
    xpect(&c, "modal is sticky across samples at DISKSAFE",
          battwarn_screen() == BATTWARN_DISKSAFE);
    battwarn_feed(-1, 1, 0, now);
    xpect(&c, "a -1 reading leaves the modal alone",
          battwarn_screen() == BATTWARN_DISKSAFE);

    /* Dismiss; further samples at level 1 must NOT bring it back — that
     * would be the modal flickering on every 5 s tick. */
    battwarn_input(now);
    xpect(&c, "input dismisses the modal", battwarn_screen() == BATTWARN_NONE);
    for (int i = 0; i < 10; i++) {
        battwarn_feed(3450 + (i & 1) * 40, 1, 0, now); now += STEP;
    }
    xpect(&c, "dismissed modal stays dismissed while DISKSAFE persists",
          battwarn_screen() == BATTWARN_NONE);
    xpect(&c, "no toast at DISKSAFE (the modal owns that band)",
          battwarn_toast_up(now) == 0);

    /* Recover (policy edge 1 -> 0), then a fresh DISKSAFE edge shows it
     * again: one modal per edge, not one per boot. */
    battwarn_feed(3610, 0, 0, now); now += STEP;
    xpect(&c, "RECOVERED clears the modal latch",
          battwarn_screen() == BATTWARN_NONE);
    battwarn_feed(3500, 1, 0, now); now += STEP;
    xpect(&c, "a second OK->DISKSAFE edge raises the modal again",
          battwarn_screen() == BATTWARN_DISKSAFE);

    /* External power hides it and suppresses the toast even at 3600. */
    battwarn_feed(3500, 1, 1, now); now += STEP;
    xpect(&c, "external power hides the modal",
          battwarn_screen() == BATTWARN_NONE);
    battwarn_reset();
    now += STEP;
    shows = 0;
    shows += feed_shows(3600, 0, 1, now); now += STEP;
    shows += feed_shows(3550, 0, 1, now); now += STEP;
    xpect(&c, "no toast at 3600 while on external power",
          shows == 0 && battwarn_toast_up(now) == 0);
    /* Plug in with a toast up: it goes away. */
    shows += feed_shows(3690, 0, 0, now);
    xpect(&c, "setup: toast up unplugged", shows == 1 && battwarn_toast_up(now));
    now += 1000000u;                        /* 1 s in: still inside the window */
    battwarn_feed(3690, 0, 1, now);
    xpect(&c, "plugging in hides a live toast", battwarn_toast_up(now) == 0);
    /* Cable out again at the same low cell: the latch was consumed by the
     * show above, so no second toast until a real climb to 3800. */
    now += STEP;
    shows += feed_shows(3690, 0, 0, now); now += STEP;
    xpect(&c, "unplugging at the same low cell does not re-toast", shows == 1);
    /* But a plug that did NOT charge the cell must not eat an unshown toast:
     * fresh state, plug at 3750 (never below 3700), unplug, fall to 3690. */
    battwarn_reset();
    shows = 0;
    shows += feed_shows(3750, 0, 1, now); now += STEP;
    shows += feed_shows(3750, 0, 0, now); now += STEP;
    shows += feed_shows(3690, 0, 0, now); now += STEP;
    xpect(&c, "a brief plug-in does not forfeit the first toast", shows == 1);

    /* Cable out at DISKSAFE: the policy sat at level 1 while plugged (the
     * edge was hidden); unplugging is when the person needs the modal. */
    battwarn_reset();
    battwarn_feed(3500, 1, 1, now); now += STEP;
    xpect(&c, "DISKSAFE edge while plugged in shows nothing",
          battwarn_screen() == BATTWARN_NONE);
    battwarn_feed(3500, 1, 0, now); now += STEP;
    xpect(&c, "unplugging at DISKSAFE raises the modal",
          battwarn_screen() == BATTWARN_DISKSAFE);

    /* SHUTOFF is terminal: input does not clear it, nor do later samples,
     * nor a cable. */
    battwarn_feed(3300, 2, 0, now); now += STEP;
    xpect(&c, "SHUTOFF edge reports the goodbye screen",
          battwarn_screen() == BATTWARN_SHUTOFF);
    battwarn_input(now);
    xpect(&c, "input does not clear SHUTOFF",
          battwarn_screen() == BATTWARN_SHUTOFF);
    battwarn_feed(3300, 2, 0, now); now += STEP;
    battwarn_feed(-1, 2, 0, now); now += STEP;
    xpect(&c, "later samples do not clear SHUTOFF",
          battwarn_screen() == BATTWARN_SHUTOFF && battwarn_toast_up(now) == 0);
    battwarn_feed(3300, 2, 1, now); now += STEP;
    xpect(&c, "external power does not un-happen a SHUTOFF",
          battwarn_screen() == BATTWARN_SHUTOFF);
    battwarn_reset();
    xpect(&c, "reset clears SHUTOFF (a fresh boot)",
          battwarn_screen() == BATTWARN_NONE);

    /* reset() re-arms LOW: after a consumed latch, a reset device toasts on
     * the first crossing again. */
    battwarn_reset();
    shows = 0;
    shows += feed_shows(3690, 0, 0, now); now += STEP;
    shows += feed_shows(3690, 0, 0, now); now += STEP;
    battwarn_reset();
    shows += feed_shows(3690, 0, 0, now); now += STEP;
    xpect(&c, "reset re-arms the LOW latch", shows == 2);

    return xfail_done(&c);
}
