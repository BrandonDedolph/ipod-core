/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/screen_battery.c — low-battery warning renders + the flap-proof
 * state machine behind them. See screen_battery.h for what and why.
 *
 * Two halves, deliberately in one file: the renderers know nothing about
 * voltage, and the state machine knows nothing about pixels, but they are one
 * feature and the header's contract is written for both.
 *
 * Renderers: same primitives as screen_charging.c (console_fill_rect shapes,
 * the Nunito renderer for type), pure integer math into the RGB565 console
 * framebuffer, no present, no hardware. The full-screen renders sit on the
 * charging screen's dark field and reuse its battery glyph and palette, so
 * the three "system" screens read as one family. The toast sits on a Linen
 * plate — it appears over the normal UI, so it takes the live theme palette
 * like the volume overlay does.
 */

#include "screen_battery.h"

#include "chrome.h"
#include "text.h"
#include "screen_charging.h"     /* CHG_* palette + the shared battery glyph */
#include "../kernel/console.h"
#include "../hal/hal.h"          /* LCD_WIDTH / LCD_HEIGHT */

/* ---------------------------------------------------------------------------
 * Full-screen layout (320x240) — the charging screen's geometry, so the
 * battery lands in the same place when the DISKSAFE modal follows a plug-in
 * screen or vice versa.
 * ------------------------------------------------------------------------- */
#define BATT_W   150
#define BATT_H    68
#define BATT_X    ((LCD_WIDTH - BATT_W) / 2)   /* 85 */
#define BATT_Y    56                           /* outline top; bottom at 124 */

/* Copy baselines. Bold 13 has a 14 px ascent, so the headline's cap sits at
 * 136 — a 12 px breath under the glyph. The two body lines are regular 11
 * (16 px line height) stacked at 18 px, and the dismiss hint hangs below them
 * in regular 9, 28 px down so it reads as a footnote rather than a fourth
 * line of copy. Measured with text_ascent/text_descent, not guessed. */
#define HEAD_BASE   150
#define BODY1_BASE  172
#define BODY2_BASE  190
#define HINT_BASE   218

void screen_battery_render(battwarn_kind_t kind)
{
    console_clear(CHG_BG);

    /* The glyph: outline + nub + the minimum 8 px stub, in the low-battery
     * red. pct 0 is deliberate — this is not a gauge, it is the picture of
     * an empty battery, and both screens show the same one. */
    screen_charging_draw_battery(BATT_X, BATT_Y, BATT_W, BATT_H, 0, CHG_RED);

    const text_font_t *head = text_font_bold_13();
    const text_font_t *body = text_font_regular_11();
    const text_font_t *hint = text_font_regular_9();

    if (kind == BATTWARN_SHUTOFF) {
        /* Three lines and no hint: there is nothing to press. Deliberately
         * NOT claiming "your place is saved" — the DISKSAFE flush happened
         * minutes ago and may have been refused (config_writable() false),
         * and a promise the firmware cannot check is worse than none. */
        ui_text_centered(HEAD_BASE,  "Battery empty",     head, CHG_TEXT);
        ui_text_centered(BODY1_BASE, "Powering off now",  body, CHG_UNIT);
        ui_text_centered(BODY2_BASE, "Plug in to charge", body, CHG_UNIT);
        return;
    }

    /* DISKSAFE (and anything else — a blank panel is never the right
     * fallback). The second body line is the one that earns this screen:
     * the write gate is silent, and without it a theme change made now would
     * simply not be there after the next charge. */
    ui_text_centered(HEAD_BASE,  "Battery very low",                       head, CHG_TEXT);
    ui_text_centered(BODY1_BASE, "Plug in now to keep listening.",         body, CHG_UNIT);
    ui_text_centered(BODY2_BASE, "Settings will not be saved until then.", body, CHG_UNIT);
    ui_text_centered(HINT_BASE,  "Press any button to dismiss",            hint, CHG_MUTED);
}

/* ---------------------------------------------------------------------------
 * The toast
 * ------------------------------------------------------------------------- */

/* The status strip's 22x12 battery (kernel/main.c draw_battery), redrawn here
 * at a fixed near-empty fill. A private copy rather than a shared drawer: the
 * original is a static in main.c, which this file cannot reach and must not
 * pull in, and six fill_rects are cheaper to keep honest than an export.
 * The stub is the palette's ACCENT — palette.h names that token as the
 * low-battery colour, and it follows the theme where the modal's CHG_RED
 * (tuned for the dark field) would not. */
static void toast_battery(int x, int y)
{
    const int w = 22, h = 12;
    console_fill_rect(x,         y,         w, 1, LINEN_MUTED_D);   /* top    */
    console_fill_rect(x,         y + h - 1, w, 1, LINEN_MUTED_D);   /* bottom */
    console_fill_rect(x,         y,         1, h, LINEN_MUTED_D);   /* left   */
    console_fill_rect(x + w - 1, y,         1, h, LINEN_MUTED_D);   /* right  */
    console_fill_rect(x + w,     y + 4,     2, h - 8, LINEN_MUTED_D); /* nub  */
    console_fill_rect(x + 2,     y + 2,     4, h - 4, LINEN_ACCENT); /* stub  */
}

void screen_battery_toast_render(void)
{
    const int PX = BATTWARN_TOAST_X, PY = BATTWARN_TOAST_Y;
    const int PW = BATTWARN_TOAST_W, PH = BATTWARN_TOAST_H;

    /* Integer-stepped corners: the anti-aliased plate (fill_round_rect_aa)
     * is a static in main.c. r=6 matches the design's plate radius. */
    ui_round_rect(PX, PY, PW, PH, 6, LINEN_PLATE);

    toast_battery(PX + 14, PY + 12);

    /* Text column starts clear of the glyph + nub (14 + 22 + 2) with a 10 px
     * gutter. Bold 12 ascends 13 px, so baseline PY+15 puts its cap at PY+2;
     * regular 9 at PY+29 descends to PY+33, inside the 36 px plate. Both
     * matter: the damage rect the text reports must stay inside the plate,
     * or a band-only present of the toast would be lying. */
    int tx = PX + 48;
    ui_text(tx, PY + 15, "Battery low",  text_font_bold_12(), LINEN_INK);
    ui_text(tx, PY + 29, "Plug in soon", FONT_SMALL,          LINEN_MUTED_D);
}

/* ---------------------------------------------------------------------------
 * The state machine
 *
 * All of it in one ~24-byte struct, no framebuffer save (the underlying screen
 * repaints on pop, as with the charging screen), no clock of its own.
 * ------------------------------------------------------------------------- */
typedef struct {
    int      low_armed;        /* LOW may fire on the next <= 3700 sample     */
    int      toast_on;         /* toast is (or was, until it expires) showing */
    uint32_t toast_t0;         /* clock stamp when it was shown               */
    int      modal_pending;    /* a DISKSAFE edge happened and has not cleared */
    int      modal_dismissed;  /* ...and the person has pressed a button since */
    int      shutoff;          /* SHUTOFF seen: terminal                      */
    int      prev_level;       /* last policy level fed, for edge detection   */
} battwarn_t;

/* Armed from power-on: the first crossing of 3700 after boot should toast
 * without waiting for a climb to 3800 that a discharging cell never makes. */
static battwarn_t g_bw = { 1, 0, 0, 0, 0, 0, 0 };

void battwarn_reset(void)
{
    g_bw.low_armed       = 1;
    g_bw.toast_on        = 0;
    g_bw.toast_t0        = 0;
    g_bw.modal_pending   = 0;
    g_bw.modal_dismissed = 0;
    g_bw.shutoff         = 0;
    g_bw.prev_level      = 0;
}

void battwarn_feed(int filt_mv, int level, int external, uint32_t now_us)
{
    battwarn_t *b = &g_bw;

    /* SHUTOFF first, before the external-power check: the policy has decided
     * to power off and kernel/main.c is about to do it, cable or no cable.
     * External power suppresses the two warnings a cable can still answer;
     * it does not un-happen a shutdown. */
    if (level >= 2) {
        b->shutoff       = 1;
        b->toast_on      = 0;
        b->modal_pending = 0;
        b->prev_level    = level;
        return;
    }

    /* A cable is the answer to both LOW and DISKSAFE, so it hides whatever is
     * showing. prev_level is deliberately NOT updated here: if the policy is
     * at DISKSAFE while plugged in and the cable comes out, the next sample
     * sees 0 -> 1 and puts the modal up — which is exactly the moment the
     * person needs it. low_armed is left alone as well: the latch has its own
     * memory of whether the toast has been shown, and a plug that did not
     * charge the cell above 3800 should not silently forfeit the one toast
     * the person has not seen yet. */
    if (external) {
        b->toast_on      = 0;
        b->modal_pending = 0;
        return;
    }

    if (level == 1 && b->prev_level == 0) {
        /* OK -> DISKSAFE edge: the modal. It supersedes any toast. */
        b->modal_pending   = 1;
        b->modal_dismissed = 0;
        b->toast_on        = 0;
    } else if (level == 0 && b->prev_level == 1) {
        /* DISKSAFE -> OK: RECOVERED. The policy's 100 mV hysteresis is what
         * makes this a real recovery rather than a load-release rebound. */
        b->modal_pending = 0;
    }

    /* The LOW Schmitt trigger + latch, only at level OK and only on a real
     * reading. Below DISKSAFE the modal owns the screen; a negative reading
     * means the filter is not ready and must not move the trigger either way. */
    if (level == 0 && filt_mv >= 0) {
        if (b->low_armed && filt_mv <= BATTWARN_MV_LOW) {
            b->toast_on  = 1;
            b->toast_t0  = now_us;
            b->low_armed = 0;              /* one show per arming */
        } else if (!b->low_armed && filt_mv >= BATTWARN_MV_LOW_CLEAR) {
            b->low_armed = 1;
        }
    }

    b->prev_level = level;
}

void battwarn_input(uint32_t now_us)
{
    (void)now_us;                  /* reserved: a debounce stamp if ever needed */
    g_bw.toast_on = 0;
    if (g_bw.modal_pending) {
        g_bw.modal_dismissed = 1;
    }
}

battwarn_kind_t battwarn_screen(void)
{
    if (g_bw.shutoff) {
        return BATTWARN_SHUTOFF;
    }
    if (g_bw.modal_pending && !g_bw.modal_dismissed) {
        return BATTWARN_DISKSAFE;
    }
    return BATTWARN_NONE;
}

int battwarn_toast_up(uint32_t now_us)
{
    if (!g_bw.toast_on) {
        return 0;
    }
    if ((uint32_t)(now_us - g_bw.toast_t0) < BATTWARN_TOAST_US) {
        return 1;
    }
    g_bw.toast_on = 0;             /* expired: cannot reopen on the next wrap */
    return 0;
}
