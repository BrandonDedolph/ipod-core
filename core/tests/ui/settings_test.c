/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ui/settings_test.c — host test for the Settings state model.
 *
 * settings.c (ui/settings.c) is the pure half of the Settings subsystem: it
 * touches nothing but settings_t + stdint, so the on-device path and this test
 * compile the SAME source. This proves the model contract main.c relies on:
 *   1. Defaults: the documented starting values.
 *   2. activate() SELECT: toggles Shuffle, cycles Repeat OFF->ALL->ONE->OFF,
 *      flips Crossfade / Resume, sets a theme.
 *   3. adjust() wheel: clamps Volume/Bass and Display Brightness to range.
 *   4. Navigation: Root rows return the right ENTER_* / RESET action codes;
 *      value/kind reporting for a toggle and a slider row.
 * No MMIO, no framebuffer — plain cc.
 */

#include "settings.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;
static int check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    if (!cond) {
        g_fail = 1;
    }
    return cond;
}

int main(void)
{
    settings_t s;

    /* --- Test 1: defaults --- */
    settings_defaults(&s);
    check("def-shuffle",   s.shuffle == 0);
    check("def-repeat",    s.repeat == REPEAT_OFF);
    check("def-volume",    s.volume == 70);
    check("def-bl-secs",   s.backlight_secs == 15);
    check("def-bl-bright", s.backlight_bright == 32);
    check("def-theme",     s.theme == 0);
    check("def-bass-0",    s.bass == 0 && s.treble == 0 && s.balance == 0);
    check("def-resume-on",  s.resume_on_startup == 1);
    /* A fresh (or freshly Reset) settings_t must carry NO resume locator —
     * "Reset Settings" routes through settings_defaults, so this is also what
     * makes a reset forget where you were. */
    check("def-resume-cleared",
          s.resume_hash == 0 && s.resume_secs == 0 && s.resume_total == 0);

    /* --- Test 2: activate() toggles Shuffle --- */
    check("shuffle-0", s.shuffle == 0);
    check("act-shuffle-none",
          settings_activate(SETTINGS_PLAYBACK, &s, 0) == SETTINGS_ACTION_NONE);
    check("shuffle-1", s.shuffle == 1);
    settings_activate(SETTINGS_PLAYBACK, &s, 0);
    check("shuffle-back-0", s.shuffle == 0);

    /* --- Test 3: activate() cycles Repeat OFF -> ALL -> ONE -> OFF --- */
    check("repeat-off", s.repeat == REPEAT_OFF);
    settings_activate(SETTINGS_PLAYBACK, &s, 1);
    check("repeat-all", s.repeat == REPEAT_ALL);
    settings_activate(SETTINGS_PLAYBACK, &s, 1);
    check("repeat-one", s.repeat == REPEAT_ONE);
    settings_activate(SETTINGS_PLAYBACK, &s, 1);
    check("repeat-wrap-off", s.repeat == REPEAT_OFF);

    /* --- Test 3b: activate() toggles Resume (row 2 of Playback) ---
     * The row was pulled from this screen while nothing could persist the
     * setting; it is back, and it is the switch that gates the whole boot
     * restore, so a silent regression to a 2-row Playback screen would make
     * the feature unreachable. */
    settings_defaults(&s);
    check("resume-on", s.resume_on_startup == 1);
    check("act-resume-none",
          settings_activate(SETTINGS_PLAYBACK, &s, 2) == SETTINGS_ACTION_NONE);
    check("resume-off", s.resume_on_startup == 0);
    settings_activate(SETTINGS_PLAYBACK, &s, 2);
    check("resume-back-on", s.resume_on_startup == 1);
    /* Toggling the SETTING must not disturb the stored locator — main.c owns
     * clearing that, and doing it here too would mean the pure model quietly
     * discarding state the caller had just loaded off disk. */
    s.resume_hash = 0xABCD1234u; s.resume_secs = 77; s.resume_total = 240;
    settings_activate(SETTINGS_PLAYBACK, &s, 2);
    check("resume-toggle-keeps-locator",
          s.resume_on_startup == 0 && s.resume_hash == 0xABCD1234u &&
          s.resume_secs == 77 && s.resume_total == 240);

    /* --- Test 4: Balance adjust clamps to [-100,100] --- */
    settings_defaults(&s);
    settings_adjust(SETTINGS_SOUND, &s, 3, +10);
    check("balance-right", s.balance == 10);
    settings_adjust(SETTINGS_SOUND, &s, 3, -1000);
    check("balance-clamp-lo", s.balance == -100);

    /* --- Test 5: adjust() clamps Volume to [0,100] --- */
    settings_defaults(&s);
    settings_adjust(SETTINGS_SOUND, &s, 0, +5);
    check("vol-up", s.volume == 75);
    settings_adjust(SETTINGS_SOUND, &s, 0, +1000);
    check("vol-clamp-hi", s.volume == 100);
    settings_adjust(SETTINGS_SOUND, &s, 0, -1000);
    check("vol-clamp-lo", s.volume == 0);

    /* --- Test 6: adjust() clamps Bass to [-12,12] --- */
    settings_defaults(&s);
    settings_adjust(SETTINGS_SOUND, &s, 1, +100);
    check("bass-clamp-hi", s.bass == 12);
    settings_adjust(SETTINGS_SOUND, &s, 1, -100);
    check("bass-clamp-lo", s.bass == -12);

    /* --- Test 7: adjust() clamps Display Brightness to [1,32] --- */
    settings_defaults(&s);                     /* bright = 32 */
    settings_adjust(SETTINGS_DISPLAY, &s, 1, +5);
    check("bright-clamp-hi", s.backlight_bright == 32);
    settings_adjust(SETTINGS_DISPLAY, &s, 1, -1000);
    check("bright-clamp-lo", s.backlight_bright == 1);

    /* --- Test 8: Display Backlight cycles the discrete list (activate wraps) */
    settings_defaults(&s);                     /* secs = 15 */
    settings_activate(SETTINGS_DISPLAY, &s, 0);
    check("bl-15->30", s.backlight_secs == 30);
    settings_activate(SETTINGS_DISPLAY, &s, 0);
    check("bl-30->60", s.backlight_secs == 60);
    settings_activate(SETTINGS_DISPLAY, &s, 0);
    check("bl-60->0(wrap)", s.backlight_secs == 0);
    /* adjust clamps at the ends (no wrap). */
    settings_adjust(SETTINGS_DISPLAY, &s, 0, -1);
    check("bl-0-clamp", s.backlight_secs == 0);

    /* --- Test 9: Theme select sets the theme index (Linen=0 / Onyx=1) --- */
    settings_defaults(&s);
    settings_activate(SETTINGS_THEME, &s, 1);
    check("theme-set-onyx", s.theme == 1);
    settings_activate(SETTINGS_THEME, &s, 0);
    check("theme-set-linen", s.theme == 0);

    /* --- Test 10: Root rows return the right action codes --- */
    check("enter-playback",
          settings_activate(SETTINGS_ROOT, &s, 0) == SETTINGS_ENTER_PLAYBACK);
    check("enter-sound",
          settings_activate(SETTINGS_ROOT, &s, 1) == SETTINGS_ENTER_SOUND);
    check("enter-theme",
          settings_activate(SETTINGS_ROOT, &s, 2) == SETTINGS_ENTER_THEME);
    check("enter-display",
          settings_activate(SETTINGS_ROOT, &s, 3) == SETTINGS_ENTER_DISPLAY);
    check("enter-about",
          settings_activate(SETTINGS_ROOT, &s, 5) == SETTINGS_ENTER_ABOUT);
    check("enter-diag",
          settings_activate(SETTINGS_ROOT, &s, 6) == SETTINGS_ENTER_DIAG);
    check("diskmode-action",
          settings_activate(SETTINGS_ROOT, &s, 7) == SETTINGS_ACTION_DISKMODE);
    check("reset-action",
          settings_activate(SETTINGS_ROOT, &s, 8) == SETTINGS_ACTION_RESET);
    /* Both fire on SELECT rather than descending — a SUBMENU kind here would
     * make the UI push a screen that does not exist. Boot Details, by
     * contrast, IS a screen, so it must stay a SUBMENU: the two action rows
     * are pinned by index, and inserting a row above them is exactly the edit
     * that would silently turn "Reset Settings" into "Disk Mode". */
    check("diag-is-submenu",
          settings_kind(SETTINGS_ROOT, 6) == SETTINGS_KIND_SUBMENU);
    check("diskmode-is-action",
          settings_kind(SETTINGS_ROOT, 7) == SETTINGS_KIND_ACTION);
    check("reset-is-action",
          settings_kind(SETTINGS_ROOT, 8) == SETTINGS_KIND_ACTION);
    /* The labels, so an index shift cannot pass by renumbering alone. */
    check("diag-label",     strcmp(settings_label(SETTINGS_ROOT, 6), "Boot Details") == 0);
    check("diskmode-label", strcmp(settings_label(SETTINGS_ROOT, 7), "Disk Mode") == 0);
    check("reset-label",    strcmp(settings_label(SETTINGS_ROOT, 8), "Reset Settings") == 0);
    check("enter-clicker",
          settings_activate(SETTINGS_ROOT, &s, 4) == SETTINGS_ENTER_CLICKER);
    check("count-clicker", settings_count(SETTINGS_CLICKER) == 8);
    settings_activate(SETTINGS_CLICKER, &s, 2);      /* pick "Click" */
    check("clicker-pick", s.clicker == 2);
    settings_activate(SETTINGS_CLICKER, &s, 0);      /* pick "Off"   */
    check("clicker-off", s.clicker == 0);

    /* --- Test 11: counts + generic value/kind reporting --- */
    check("count-root",  settings_count(SETTINGS_ROOT) == 9);
    check("count-play",  settings_count(SETTINGS_PLAYBACK) == 3);
    check("count-sound", settings_count(SETTINGS_SOUND) == 4);
    check("count-theme", settings_count(SETTINGS_THEME) == 2);

    settings_defaults(&s);
    {
        char buf[24];
        int is_toggle = 0, on = 0, num = 0, den = 0;

        /* Volume slider reports fraction 70/100 and "70%". */
        settings_value(SETTINGS_SOUND, &s, 0, buf, &is_toggle, &on, &num, &den);
        check("vol-slider-frac", is_toggle == 0 && num == 70 && den == 100);
        check("vol-slider-text",
              buf[0] == '7' && buf[1] == '0' && buf[2] == '%' && buf[3] == '\0');

        /* Balance slider reports "Center" at 0 and the mid fraction. */
        settings_value(SETTINGS_SOUND, &s, 3, buf, &is_toggle, &on, &num, &den);
        check("bal-slider-mid", is_toggle == 0 && num == 100 && den == 200);
        check("bal-text-center",
              buf[0] == 'C' && buf[1] == 'e' && buf[2] == 'n');

        /* Repeat select reports "Off" initially. */
        settings_value(SETTINGS_PLAYBACK, &s, 1, buf, &is_toggle, &on,
                       &num, &den);
        check("repeat-text-off",
              buf[0] == 'O' && buf[1] == 'f' && buf[2] == 'f');
        check("repeat-kind-select",
              settings_kind(SETTINGS_PLAYBACK, 1) == SETTINGS_KIND_SELECT);

        /* Resume renders its state as a right-hand On/Off value, and has a
         * label — an unlabelled row is an invisible one. */
        settings_value(SETTINGS_PLAYBACK, &s, 2, buf, &is_toggle, &on,
                       &num, &den);
        check("resume-text-on",
              buf[0] == 'O' && buf[1] == 'n' && buf[2] == '\0');
        s.resume_on_startup = 0;
        settings_value(SETTINGS_PLAYBACK, &s, 2, buf, &is_toggle, &on,
                       &num, &den);
        check("resume-text-off",
              buf[0] == 'O' && buf[1] == 'f' && buf[2] == 'f');
        check("resume-label",
              settings_label(SETTINGS_PLAYBACK, 2)[0] == 'R');
    }

    /* --- Test 12: NOOP — SELECT that changes nothing must SAY so ---
     * Every SETTINGS_ACTION_NONE ends in a disk write (main.c touches, 3 s
     * later config_save() puts a sector on the user's disk). Until NOOP
     * existed, SELECT on the About page returned NONE like a real toggle and
     * spun the drive up for a byte-identical record. The invariant checked
     * here is two-sided: NOOP => the record is byte-identical to a copy, and
     * NONE => it is not. Either half failing is a missed or a spurious write. */
    settings_defaults(&s);
    {
        settings_t copy;

        memcpy(&copy, &s, sizeof s);
        check("noop-about",
              settings_activate(SETTINGS_ABOUT, &s, 0) == SETTINGS_ACTION_NOOP);
        check("noop-about-unchanged", memcmp(&copy, &s, sizeof s) == 0);

        check("noop-diag",
              settings_activate(SETTINGS_DIAG, &s, 0) == SETTINGS_ACTION_NOOP);
        check("noop-diag-unchanged", memcmp(&copy, &s, sizeof s) == 0);

        /* Sound rows are sliders: SELECT on them is not a control. */
        check("noop-sound",
              settings_activate(SETTINGS_SOUND, &s, 0) == SETTINGS_ACTION_NOOP);
        check("noop-sound-unchanged", memcmp(&copy, &s, sizeof s) == 0);

        /* Brightness (Display row 1) is wheel-only; Backlight (row 0) wraps
         * and therefore always changes. */
        check("noop-display-brightness",
              settings_activate(SETTINGS_DISPLAY, &s, 1) == SETTINGS_ACTION_NOOP);
        check("noop-display-unchanged", memcmp(&copy, &s, sizeof s) == 0);
        check("none-display-backlight",
              settings_activate(SETTINGS_DISPLAY, &s, 0) == SETTINGS_ACTION_NONE);
        check("none-display-changed", memcmp(&copy, &s, sizeof s) != 0);

        /* Re-picking the theme already active is a NOOP; picking the other
         * one is a NONE and the field moves. */
        settings_defaults(&s);
        memcpy(&copy, &s, sizeof s);
        check("noop-theme-same",
              settings_activate(SETTINGS_THEME, &s, s.theme) == SETTINGS_ACTION_NOOP);
        check("noop-theme-unchanged", memcmp(&copy, &s, sizeof s) == 0);
        int other = 1 - s.theme;
        check("none-theme-other",
              settings_activate(SETTINGS_THEME, &s, other) == SETTINGS_ACTION_NONE);
        check("none-theme-changed", s.theme == other && memcmp(&copy, &s, sizeof s) != 0);
        check("noop-theme-range",
              settings_activate(SETTINGS_THEME, &s, 7) == SETTINGS_ACTION_NOOP);

        /* Same for the clicker profile. */
        settings_defaults(&s);
        memcpy(&copy, &s, sizeof s);
        check("noop-clicker-same",
              settings_activate(SETTINGS_CLICKER, &s, s.clicker) == SETTINGS_ACTION_NOOP);
        check("noop-clicker-unchanged", memcmp(&copy, &s, sizeof s) == 0);
        check("none-clicker-other",
              settings_activate(SETTINGS_CLICKER, &s, s.clicker + 1) == SETTINGS_ACTION_NONE);
        check("noop-clicker-range",
              settings_activate(SETTINGS_CLICKER, &s, 99) == SETTINGS_ACTION_NOOP);

        /* Out-of-range rows on the root and on Playback. */
        settings_defaults(&s);
        memcpy(&copy, &s, sizeof s);
        check("noop-root-range",
              settings_activate(SETTINGS_ROOT, &s, 42) == SETTINGS_ACTION_NOOP);
        check("noop-playback-range",
              settings_activate(SETTINGS_PLAYBACK, &s, 9) == SETTINGS_ACTION_NOOP);
        check("noop-range-unchanged", memcmp(&copy, &s, sizeof s) == 0);

        /* The real toggles still say NONE — a NOOP here would be a change
         * that never reaches the disk. */
        check("none-shuffle",
              settings_activate(SETTINGS_PLAYBACK, &s, 0) == SETTINGS_ACTION_NONE);
        check("none-repeat",
              settings_activate(SETTINGS_PLAYBACK, &s, 1) == SETTINGS_ACTION_NONE);
        check("none-resume",
              settings_activate(SETTINGS_PLAYBACK, &s, 2) == SETTINGS_ACTION_NONE);
        check("none-changed", memcmp(&copy, &s, sizeof s) != 0);

        /* NOOP was appended: the codes main.c switches on keep their values. */
        check("noop-appended-last",
              SETTINGS_ACTION_NOOP > SETTINGS_ACTION_DISKMODE &&
              SETTINGS_ACTION_NONE == 0);
    }

    /* --- Test 13: adjust() reports whether anything moved ---
     * A wheel pinned at a rail used to earn a disk write per tick. */
    settings_defaults(&s);                     /* volume 70, bright 32 */
    check("adj-mid-changed",  settings_adjust(SETTINGS_SOUND, &s, 0, +1) == 1);
    check("adj-zero-delta",   settings_adjust(SETTINGS_SOUND, &s, 0, 0) == 0);
    settings_adjust(SETTINGS_SOUND, &s, 0, +1000);         /* pin at 100 */
    check("adj-rail-hi",      settings_adjust(SETTINGS_SOUND, &s, 0, +1) == 0);
    check("adj-rail-hi-val",  s.volume == 100);
    check("adj-off-rail",     settings_adjust(SETTINGS_SOUND, &s, 0, -1) == 1);
    check("adj-bright-rail",  settings_adjust(SETTINGS_DISPLAY, &s, 1, +1) == 0);
    check("adj-bright-down",  settings_adjust(SETTINGS_DISPLAY, &s, 1, -1) == 1);
    check("adj-bl-mid",       settings_adjust(SETTINGS_DISPLAY, &s, 0, +1) == 1);
    settings_adjust(SETTINGS_DISPLAY, &s, 0, +100);        /* clamp at 60 */
    check("adj-bl-rail",      settings_adjust(SETTINGS_DISPLAY, &s, 0, +1) == 0);
    check("adj-bl-rail-val",  s.backlight_secs == 60);
    /* Every Sound row and both rails: main.c gates apply + settings_touch on
     * this return, so a row that reported "moved" at its rail would put the
     * per-tick disk write back for that slider only. */
    settings_adjust(SETTINGS_SOUND, &s, 0, -1000);          /* volume 0     */
    check("adj-vol-rail-lo",  settings_adjust(SETTINGS_SOUND, &s, 0, -1) == 0);
    settings_adjust(SETTINGS_DISPLAY, &s, 1, -1000);        /* brightness 1 */
    check("adj-bright-rail-lo", settings_adjust(SETTINGS_DISPLAY, &s, 1, -1) == 0);
    for (int row = 1; row <= 3; row++) {                    /* bass/treble/bal */
        check("adj-row-mid",  settings_adjust(SETTINGS_SOUND, &s, row, +1) == 1);
        settings_adjust(SETTINGS_SOUND, &s, row, +1000);
        check("adj-row-hi",   settings_adjust(SETTINGS_SOUND, &s, row, +1) == 0);
        check("adj-row-off",  settings_adjust(SETTINGS_SOUND, &s, row, -1) == 1);
        settings_adjust(SETTINGS_SOUND, &s, row, -1000);
        check("adj-row-lo",   settings_adjust(SETTINGS_SOUND, &s, row, -1) == 0);
    }
    check("adj-rails-vals",   s.bass == -12 && s.treble == -12 && s.balance == -100);
    check("adj-non-slider",   settings_adjust(SETTINGS_PLAYBACK, &s, 0, +1) == 0);
    check("adj-bad-row",      settings_adjust(SETTINGS_SOUND, &s, 9, +1) == 0);

    printf("settings_test: %s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
