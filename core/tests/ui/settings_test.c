/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ui/settings_test.c — host test for the Settings state model.
 *
 * settings.c (ui/settings.c) is the pure half of the Settings subsystem: it
 * touches nothing but settings_t + stdint, so the on-device path and this test
 * compile the SAME source. This proves the model contract main.c relies on:
 *   1. Defaults: the documented starting values.
 *   2. activate() SELECT: cycles Shuffle Off->Songs->Albums->Off, cycles
 *      Repeat OFF->ALL->ONE->OFF, flips Crossfade / Resume, cycles the Sleep
 *      Timer's durations, sets a theme.
 *   3. adjust() wheel: clamps Volume/Bass and Display Brightness to range.
 *   4. Navigation: Root rows return the right ENTER_* / RESET action codes;
 *      value/kind reporting for a toggle and a slider row.
 *   5. Sound: the Volume Limit couples to Volume in both directions, and an
 *      EQ preset locks the two shelf rows without touching what they store.
 * No MMIO, no framebuffer — plain cc.
 */

#include "settings.h"
#include "eq.h"                  /* EQ_PRESET_COUNT + the preset names */

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
    check("def-vol-limit", s.volume_limit == 100);   /* 100 = no limit */
    check("def-eq-off",    s.eq == EQ_OFF);
    check("def-resume-on",  s.resume_on_startup == 1);
    /* A fresh (or freshly Reset) settings_t must carry NO resume locator —
     * "Reset Settings" routes through settings_defaults, so this is also what
     * makes a reset forget where you were. */
    check("def-resume-cleared",
          s.resume_hash == 0 && s.resume_secs == 0 && s.resume_total == 0);

    /* --- Test 2: activate() cycles Shuffle Off -> Songs -> Albums -> Off ---
     *
     * Three states since Shuffle Albums, on the same row and the same record
     * byte, so the cycle (and the label each state shows) is what a reviewer
     * of the persisted byte has to be able to trust. */
    {
        char v[24];
        int tog, on, num, den;
        check("shuffle-off", s.shuffle == SHUFFLE_OFF);
        check("shuffle-kind-select",
              settings_kind(SETTINGS_PLAYBACK, 0) == SETTINGS_KIND_SELECT);
        settings_value(SETTINGS_PLAYBACK, &s, 0, v, &tog, &on, &num, &den);
        check("shuffle-val-off", strcmp(v, "Off") == 0);

        check("act-shuffle-songs-none",
              settings_activate(SETTINGS_PLAYBACK, &s, 0) == SETTINGS_ACTION_NONE);
        check("shuffle-songs", s.shuffle == SHUFFLE_SONGS);
        settings_value(SETTINGS_PLAYBACK, &s, 0, v, &tog, &on, &num, &den);
        check("shuffle-val-songs", strcmp(v, "Songs") == 0);

        check("act-shuffle-albums-none",
              settings_activate(SETTINGS_PLAYBACK, &s, 0) == SETTINGS_ACTION_NONE);
        check("shuffle-albums", s.shuffle == SHUFFLE_ALBUMS);
        settings_value(SETTINGS_PLAYBACK, &s, 0, v, &tog, &on, &num, &den);
        check("shuffle-val-albums", strcmp(v, "Albums") == 0);

        check("act-shuffle-wrap-none",
              settings_activate(SETTINGS_PLAYBACK, &s, 0) == SETTINGS_ACTION_NONE);
        check("shuffle-wrap-off", s.shuffle == SHUFFLE_OFF);
    }

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
    settings_adjust(SETTINGS_SOUND, &s, 5, +10);
    check("balance-right", s.balance == 10);
    settings_adjust(SETTINGS_SOUND, &s, 5, -1000);
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
    settings_adjust(SETTINGS_SOUND, &s, 3, +100);
    check("bass-clamp-hi", s.bass == 12);
    settings_adjust(SETTINGS_SOUND, &s, 3, -100);
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

    /* --- Test 9: Theme select sets the theme id (the picker row IS the id) --- */
    settings_defaults(&s);
    settings_activate(SETTINGS_THEME, &s, 1);
    check("theme-set-onyx", s.theme == 1);
    settings_activate(SETTINGS_THEME, &s, 0);
    check("theme-set-linen", s.theme == 0);
    {
        /* All seven, by name and in picker order — main.c hands s.theme
         * straight to theme_set(), so the row order here is the palette
         * order in ui/palette.h. */
        static const char *const want[] = {
            "Linen", "Onyx", "Sage", "Plaster", "Olive", "Umber", "Mushroom",
        };
        const int n = (int)(sizeof want / sizeof want[0]);
        check("theme-count-7", settings_count(SETTINGS_THEME) == n);
        int names_ok = 1, picks_ok = 1;
        for (int i = 0; i < n; i++) {
            names_ok &= strcmp(settings_label(SETTINGS_THEME, i), want[i]) == 0;
            names_ok &= strcmp(settings_theme_name(i), want[i]) == 0;
            settings_defaults(&s);
            s.theme = (i + 1) % n;                 /* start on some other one */
            picks_ok &= settings_activate(SETTINGS_THEME, &s, i)
                        == SETTINGS_ACTION_NONE;
            picks_ok &= (s.theme == i);
        }
        check("theme-names-in-order", names_ok);
        check("theme-pick-each",      picks_ok);
        /* Past the end: no row label, the name falls back to Linen, and a
         * pick is refused (the id must never leave the palette's range). */
        settings_defaults(&s);
        check("theme-label-past-end",
              strcmp(settings_label(SETTINGS_THEME, n), "") == 0);
        check("theme-name-past-end",
              strcmp(settings_theme_name(n), "Linen") == 0 &&
              strcmp(settings_theme_name(-1), "Linen") == 0 &&
              strcmp(settings_theme_name(255), "Linen") == 0);
        check("theme-pick-past-end",
              settings_activate(SETTINGS_THEME, &s, n) == SETTINGS_ACTION_NOOP &&
              settings_activate(SETTINGS_THEME, &s, -1) == SETTINGS_ACTION_NOOP &&
              s.theme == 0);
        /* The right-hand value on the root Theme row is the current name. */
        char buf[24];
        int tg = 0, on = 0, num = 0, den = 0;
        s.theme = 6;
        settings_value(SETTINGS_ROOT, &s, 2, buf, &tg, &on, &num, &den);
        check("theme-root-value", strcmp(buf, "Mushroom") == 0);
    }

    /* --- Test 10: Root rows return the right action codes --- */
    check("enter-playback",
          settings_activate(SETTINGS_ROOT, &s, 0) == SETTINGS_ENTER_PLAYBACK);
    check("enter-sound",
          settings_activate(SETTINGS_ROOT, &s, 1) == SETTINGS_ENTER_SOUND);
    check("enter-theme",
          settings_activate(SETTINGS_ROOT, &s, 2) == SETTINGS_ENTER_THEME);
    check("enter-display",
          settings_activate(SETTINGS_ROOT, &s, 3) == SETTINGS_ENTER_DISPLAY);
    check("enter-datetime",
          settings_activate(SETTINGS_ROOT, &s, 5) == SETTINGS_ENTER_DATETIME);
    check("enter-about",
          settings_activate(SETTINGS_ROOT, &s, 6) == SETTINGS_ENTER_ABOUT);
    check("enter-diag",
          settings_activate(SETTINGS_ROOT, &s, 7) == SETTINGS_ENTER_DIAG);
    check("diskmode-action",
          settings_activate(SETTINGS_ROOT, &s, 8) == SETTINGS_ACTION_DISKMODE);
    check("reset-action",
          settings_activate(SETTINGS_ROOT, &s, 9) == SETTINGS_ACTION_RESET);
    /* Both fire on SELECT rather than descending — a SUBMENU kind here would
     * make the UI push a screen that does not exist. Boot Details, by
     * contrast, IS a screen, so it must stay a SUBMENU: the two action rows
     * are pinned by index, and inserting a row above them is exactly the edit
     * that would silently turn "Reset Settings" into "Disk Mode". */
    check("diag-is-submenu",
          settings_kind(SETTINGS_ROOT, 7) == SETTINGS_KIND_SUBMENU);
    check("diskmode-is-action",
          settings_kind(SETTINGS_ROOT, 8) == SETTINGS_KIND_ACTION);
    check("reset-is-action",
          settings_kind(SETTINGS_ROOT, 9) == SETTINGS_KIND_ACTION);
    /* The labels, so an index shift cannot pass by renumbering alone. */
    check("datetime-label", strcmp(settings_label(SETTINGS_ROOT, 5), "Date & Time") == 0);
    check("about-label",    strcmp(settings_label(SETTINGS_ROOT, 6), "About") == 0);
    check("diag-label",     strcmp(settings_label(SETTINGS_ROOT, 7), "Boot Details") == 0);
    check("diskmode-label", strcmp(settings_label(SETTINGS_ROOT, 8), "Disk Mode") == 0);
    check("reset-label",    strcmp(settings_label(SETTINGS_ROOT, 9), "Reset Settings") == 0);
    check("enter-clicker",
          settings_activate(SETTINGS_ROOT, &s, 4) == SETTINGS_ENTER_CLICKER);
    check("count-clicker", settings_count(SETTINGS_CLICKER) == 8);
    settings_activate(SETTINGS_CLICKER, &s, 2);      /* pick "Click" */
    check("clicker-pick", s.clicker == 2);
    settings_activate(SETTINGS_CLICKER, &s, 0);      /* pick "Off"   */
    check("clicker-off", s.clicker == 0);

    /* --- Test 11: counts + generic value/kind reporting --- */
    check("count-root",  settings_count(SETTINGS_ROOT) == 10);
    check("count-play",  settings_count(SETTINGS_PLAYBACK) == 4);
    check("count-sound", settings_count(SETTINGS_SOUND) == 6);
    /* Addressed BY LABEL, so re-ordering the screen cannot pass by
     * renumbering the tests that index it. */
    check("sound-labels",
          settings_label(SETTINGS_SOUND, 0)[0] == 'V' &&
          strcmp(settings_label(SETTINGS_SOUND, 1), "Volume Limit") == 0 &&
          strcmp(settings_label(SETTINGS_SOUND, 2), "EQ") == 0 &&
          strcmp(settings_label(SETTINGS_SOUND, 3), "Bass") == 0 &&
          strcmp(settings_label(SETTINGS_SOUND, 4), "Treble") == 0 &&
          strcmp(settings_label(SETTINGS_SOUND, 5), "Balance") == 0);
    check("sound-kinds",
          settings_kind(SETTINGS_SOUND, 0) == SETTINGS_KIND_SLIDER &&
          settings_kind(SETTINGS_SOUND, 1) == SETTINGS_KIND_SLIDER &&
          settings_kind(SETTINGS_SOUND, 2) == SETTINGS_KIND_SELECT &&
          settings_kind(SETTINGS_SOUND, 3) == SETTINGS_KIND_SLIDER &&
          settings_kind(SETTINGS_SOUND, 4) == SETTINGS_KIND_SLIDER &&
          settings_kind(SETTINGS_SOUND, 5) == SETTINGS_KIND_SLIDER);
    check("count-theme", settings_count(SETTINGS_THEME) == 7);

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
        settings_value(SETTINGS_SOUND, &s, 5, buf, &is_toggle, &on, &num, &den);
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

        /* Sleep Timer (Playback row 3): a SELECT row like the rest, reading
         * Off out of settings_defaults because it is never restored from
         * disk (settings.h — the field is runtime-only). */
        check("sleep-label",
              strcmp(settings_label(SETTINGS_PLAYBACK, 3), "Sleep Timer") == 0);
        check("sleep-kind-select",
              settings_kind(SETTINGS_PLAYBACK, 3) == SETTINGS_KIND_SELECT);
        settings_defaults(&s);
        check("sleep-default-off", s.sleep_timer_min == 0);
        settings_value(SETTINGS_PLAYBACK, &s, 3, buf, &is_toggle, &on,
                       &num, &den);
        check("sleep-text-off", strcmp(buf, "Off") == 0);

        /* SELECT walks the six options and wraps back to Off, and every step
         * reports SLEEPTIMER — never NONE, which main.c would answer with a
         * settings_touch() and a byte-identical sector on the user's disk. */
        {
            static const int want[6] = { 15, 30, 60, 90, 120, 0 };
            static const char *const want_txt[6] = {
                "15 min", "30 min", "60 min", "90 min", "120 min", "Off",
            };
            int cycle_ok = 1, code_ok = 1, text_ok = 1;
            for (int i = 0; i < 6; i++) {
                if (settings_activate(SETTINGS_PLAYBACK, &s, 3)
                    != SETTINGS_ACTION_SLEEPTIMER) {
                    code_ok = 0;
                }
                if (s.sleep_timer_min != want[i]) {
                    cycle_ok = 0;
                }
                settings_value(SETTINGS_PLAYBACK, &s, 3, buf, &is_toggle, &on,
                               &num, &den);
                if (strcmp(buf, want_txt[i]) != 0) {
                    text_ok = 0;
                }
            }
            check("sleep-cycle", cycle_ok);
            check("sleep-cycle-action", code_ok);
            check("sleep-cycle-text", text_ok);
        }

        /* A duration not in the table cannot arise on the device (the field
         * is runtime-only and only this cycle ever writes it), but the step
         * must still land inside the table rather than running off the end:
         * an unknown value indexes to Off, so SELECT gives the head of the
         * list, exactly as bl_step does from an unknown backlight timeout. */
        s.sleep_timer_min = 45;
        settings_value(SETTINGS_PLAYBACK, &s, 3, buf, &is_toggle, &on,
                       &num, &den);
        check("sleep-unknown-text", strcmp(buf, "45 min") == 0);
        settings_activate(SETTINGS_PLAYBACK, &s, 3);
        check("sleep-unknown-steps-into-the-table", s.sleep_timer_min == 15);

        /* Reset Settings routes through settings_defaults: the timer is off
         * again (main.c re-applies the field to ui/sleeptimer.c after one). */
        settings_defaults(&s);
        check("sleep-reset-off", s.sleep_timer_min == 0);
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
              settings_activate(SETTINGS_THEME, &s, 7) == SETTINGS_ACTION_NOOP &&
              settings_activate(SETTINGS_THEME, &s, 99) == SETTINGS_ACTION_NOOP);

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

        /* THE THIRD CLASS. Sleep Timer changes the record like any toggle,
         * so it must not report NOOP — main.c has to re-arm ui/sleeptimer.c
         * from the new value. But the field is runtime-only (config.c neither
         * encodes nor restores it), so NONE would spend a disk write on a
         * byte-identical record. SLEEPTIMER is "changed, but only the runtime
         * part": the record moved AND main.c must not persist it. */
        settings_defaults(&s);
        memcpy(&copy, &s, sizeof s);
        check("sleeptimer-action",
              settings_activate(SETTINGS_PLAYBACK, &s, 3)
              == SETTINGS_ACTION_SLEEPTIMER);
        check("sleeptimer-changed", memcmp(&copy, &s, sizeof s) != 0);
        check("sleeptimer-changed-only-that-field",
              s.sleep_timer_min == 15 &&
              (copy.sleep_timer_min = s.sleep_timer_min,
               memcmp(&copy, &s, sizeof s) == 0));

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
    for (int row = 3; row <= 5; row++) {                    /* bass/treble/bal */
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
    check("adj-eq-row-is-select",
          settings_adjust(SETTINGS_SOUND, &s, 2, +1) == 0 && s.eq == EQ_OFF);

    /* --- Test 14: Volume Limit — the one rule, applied from both rows ---
     * The invariant is that *s never leaves settings_adjust with
     * volume > volume_limit, from EITHER row, and that a wheel that cannot
     * move says so (main.c gates the disk write on that answer). */
    settings_defaults(&s);
    s.volume_limit = 40;
    s.volume       = 38;
    check("vol-stops-at-the-limit",
          settings_adjust(SETTINGS_SOUND, &s, 0, +5) == 1 && s.volume == 40);
    check("vol-rail-at-the-limit",
          settings_adjust(SETTINGS_SOUND, &s, 0, +1) == 0 && s.volume == 40);
    check("vol-below-the-limit-still-moves",
          settings_adjust(SETTINGS_SOUND, &s, 0, -1) == 1 && s.volume == 39);

    /* Lowering the ceiling under the current volume pulls the volume down in
     * the SAME call — main.c's single settings_apply() then pushes both. */
    settings_defaults(&s);                          /* volume 70, limit 100 */
    check("limit-down-moves",
          settings_adjust(SETTINGS_SOUND, &s, 1, -40) == 1 &&
          s.volume_limit == 60 && s.volume == 60);
    check("limit-down-again-drags-volume",
          settings_adjust(SETTINGS_SOUND, &s, 1, -10) == 1 &&
          s.volume_limit == 50 && s.volume == 50);
    /* Raising it again leaves the volume where the user left it. */
    check("limit-up-leaves-volume",
          settings_adjust(SETTINGS_SOUND, &s, 1, +30) == 1 &&
          s.volume_limit == 80 && s.volume == 50);

    /* The limit's own rails: a 10% floor (a 0 would be a mute switch nobody
     * can find) and 100 = no limit. */
    settings_adjust(SETTINGS_SOUND, &s, 1, -1000);
    check("limit-rail-lo", s.volume_limit == 10 && s.volume == 10);
    check("limit-rail-lo-reports",
          settings_adjust(SETTINGS_SOUND, &s, 1, -1) == 0);
    settings_adjust(SETTINGS_SOUND, &s, 1, +1000);
    check("limit-rail-hi", s.volume_limit == 100);
    check("limit-rail-hi-reports",
          settings_adjust(SETTINGS_SOUND, &s, 1, +1) == 0);

    /* settings_volume_clamp is the rule itself; the Now Playing wheel calls
     * the same function, which is the whole point of it existing. */
    settings_defaults(&s);
    s.volume_limit = 55;
    check("clamp-under",  settings_volume_clamp(&s, 20) == 20);
    check("clamp-over",   settings_volume_clamp(&s, 90) == 55);
    check("clamp-at",     settings_volume_clamp(&s, 55) == 55);
    check("clamp-neg",    settings_volume_clamp(&s, -5) == 0);
    s.volume_limit = 100;
    check("clamp-no-limit", settings_volume_clamp(&s, 100) == 100 &&
                            settings_volume_clamp(&s, 140) == 100);
    /* A limit outside its own range cannot drag the ceiling below the floor. */
    s.volume_limit = 0;
    check("clamp-bad-limit", settings_volume_clamp(&s, 100) == 10);

    /* The Volume Limit row renders as its own percentage bar. */
    settings_defaults(&s);
    s.volume_limit = 40;
    {
        char buf[24];
        int is_toggle = 0, on = 0, num = 0, den = 0;
        settings_value(SETTINGS_SOUND, &s, 1, buf, &is_toggle, &on, &num, &den);
        check("limit-slider-frac", num == 40 && den == 100);
        check("limit-slider-text",
              buf[0] == '4' && buf[1] == '0' && buf[2] == '%' && buf[3] == '\0');
    }

    /* --- Test 15: the EQ row cycles, and cycles ONLY the EQ field --- */
    settings_defaults(&s);
    {
        settings_t copy;
        int cycle_ok = 1, touch_ok = 1;
        for (int i = 1; i <= EQ_PRESET_COUNT; i++) {
            memcpy(&copy, &s, sizeof s);
            if (settings_activate(SETTINGS_SOUND, &s, 2) != SETTINGS_ACTION_NONE) {
                cycle_ok = 0;
            }
            int want = i % EQ_PRESET_COUNT;
            if (s.eq != want) {
                cycle_ok = 0;
            }
            /* Nothing but eq may move: patch the copy's eq and compare the
             * whole record, so a stray write to bass (say) is caught. */
            copy.eq = s.eq;
            if (memcmp(&copy, &s, sizeof s) != 0) {
                touch_ok = 0;
            }
        }
        check("eq-cycles-through-every-preset-and-back", cycle_ok);
        check("eq-cycle-touches-only-eq", touch_ok);
    }
    /* The row's value text is the preset's name, from the one table. */
    {
        char buf[24];
        int is_toggle = 0, on = 0, num = 0, den = 0;
        int names_ok = 1;
        for (int i = 0; i < EQ_PRESET_COUNT; i++) {
            s.eq = i;
            settings_value(SETTINGS_SOUND, &s, 2, buf, &is_toggle, &on,
                           &num, &den);
            if (strcmp(buf, eq_preset_name(i)) != 0 || den != 0) {
                names_ok = 0;
            }
            if (strcmp(settings_eq_name(i), eq_preset_name(i)) != 0) {
                names_ok = 0;
            }
        }
        check("eq-row-names-the-preset", names_ok);
    }

    /* --- Test 16: an EQ preset LOCKS Bass and Treble ---
     * The codec has one low shelf and one high shelf and the preset owns
     * both, so those rows show the preset's gains, refuse the wheel, and
     * leave what the user stored alone. */
    settings_defaults(&s);
    s.bass = -4; s.treble = 7;
    {
        int unlocked_ok = 1;
        for (int r = 0; r < 6; r++) {
            if (settings_row_locked(SETTINGS_SOUND, &s, r)) unlocked_ok = 0;
        }
        check("no-row-locked-at-eq-off", unlocked_ok);
    }
    s.eq = 2;                                   /* Bass Booster: +6 / … / 0 */
    check("locked-rows-are-bass-and-treble",
          settings_row_locked(SETTINGS_SOUND, &s, 3) == 1 &&
          settings_row_locked(SETTINGS_SOUND, &s, 4) == 1 &&
          settings_row_locked(SETTINGS_SOUND, &s, 0) == 0 &&
          settings_row_locked(SETTINGS_SOUND, &s, 1) == 0 &&
          settings_row_locked(SETTINGS_SOUND, &s, 2) == 0 &&
          settings_row_locked(SETTINGS_SOUND, &s, 5) == 0);
    check("lock-is-a-sound-screen-rule",
          settings_row_locked(SETTINGS_DISPLAY, &s, 1) == 0);
    /* A preset id from a newer build reads as Off everywhere — including
     * here, so the rows do not lock over a curve this build cannot show. */
    s.eq = EQ_PRESET_COUNT;
    check("unknown-preset-does-not-lock",
          settings_row_locked(SETTINGS_SOUND, &s, 3) == 0 &&
          settings_row_locked(SETTINGS_SOUND, &s, 4) == 0);
    s.eq = 2;
    check("locked-bass-refuses-the-wheel",
          settings_adjust(SETTINGS_SOUND, &s, 3, +1) == 0 && s.bass == -4);
    check("locked-treble-refuses-the-wheel",
          settings_adjust(SETTINGS_SOUND, &s, 4, -1) == 0 && s.treble == 7);
    {
        char buf[24];
        int is_toggle = 0, on = 0, num = 0, den = 0;
        settings_value(SETTINGS_SOUND, &s, 3, buf, &is_toggle, &on, &num, &den);
        check("locked-bass-shows-the-preset-shelf",
              strcmp(buf, "+6 dB") == 0 && num == 18 && den == 24);
        settings_value(SETTINGS_SOUND, &s, 4, buf, &is_toggle, &on, &num, &den);
        check("locked-treble-shows-the-preset-shelf",
              strcmp(buf, "0 dB") == 0 && num == 12 && den == 24);
    }
    /* Back to Off and the user's own tone is exactly where they left it. */
    s.eq = EQ_OFF;
    {
        char buf[24];
        int is_toggle = 0, on = 0, num = 0, den = 0;
        settings_value(SETTINGS_SOUND, &s, 3, buf, &is_toggle, &on, &num, &den);
        check("off-restores-the-users-bass",
              s.bass == -4 && strcmp(buf, "-4 dB") == 0 && num == 8);
        settings_value(SETTINGS_SOUND, &s, 4, buf, &is_toggle, &on, &num, &den);
        check("off-restores-the-users-treble",
              s.treble == 7 && strcmp(buf, "+7 dB") == 0 && num == 19);
    }
    check("unlocked-bass-moves-again",
          settings_adjust(SETTINGS_SOUND, &s, 3, +1) == 1 && s.bass == -3);

    /* SELECT on the new rows keeps the NOOP/NONE contract: only EQ writes. */
    settings_defaults(&s);
    {
        settings_t copy;
        memcpy(&copy, &s, sizeof s);
        check("noop-volume-limit-row",
              settings_activate(SETTINGS_SOUND, &s, 1) == SETTINGS_ACTION_NOOP);
        check("noop-sound-rows-unchanged", memcmp(&copy, &s, sizeof s) == 0);
        check("none-eq-row",
              settings_activate(SETTINGS_SOUND, &s, 2) == SETTINGS_ACTION_NONE);
        check("none-eq-changed", memcmp(&copy, &s, sizeof s) != 0);
    }

    /* --- Test 13: Date & Time ---------------------------------------------
     * The screen is three rows, and the first one's VALUE is a clock this
     * module cannot compute — main.c injects it. The injection seam is worth
     * pinning because the failure it prevents is silent: with no clock, the
     * row must say "Not set" rather than a plausible 1970 date. */
    settings_defaults(&s);
    check("dt-defaults", s.time_24h == 0 && s.time_in_title == 0 &&
                         s.utc_off_min == 0 && s.host_epoch == 0 &&
                         s.host_off_min == 0 && s.applied_epoch == 0);
    check("dt-count", settings_count(SETTINGS_DATETIME) == 3);
    /* The TITLE, for every screen. This is here because it was wrong: the
     * renderer's own switch had no Date & Time case and titled the screen
     * "Settings", while the committed gallery still (docs/screens/datetime.png)
     * said "Date & Time". Asserting the model's answer for every screen is what
     * stops the code and the picture saying different things again. */
    check("titles",
          strcmp(settings_title(SETTINGS_ROOT), "Settings") == 0 &&
          strcmp(settings_title(SETTINGS_PLAYBACK), "Playback") == 0 &&
          strcmp(settings_title(SETTINGS_SOUND), "Sound") == 0 &&
          strcmp(settings_title(SETTINGS_DISPLAY), "Display") == 0 &&
          strcmp(settings_title(SETTINGS_THEME), "Theme") == 0 &&
          strcmp(settings_title(SETTINGS_CLICKER), "Clicker") == 0 &&
          strcmp(settings_title(SETTINGS_DATETIME), "Date & Time") == 0 &&
          strcmp(settings_title(SETTINGS_SETTIME), "Set Date & Time") == 0 &&
          strcmp(settings_title(SETTINGS_ABOUT), "About") == 0 &&
          strcmp(settings_title(SETTINGS_DIAG), "Boot Details") == 0);
    check("titles: every screen in the enum has one, and nothing else does",
          settings_title(SETTINGS_SCREEN_COUNT)[0] == '\0' &&
          settings_title(-1)[0] == '\0');
    {
        /* No screen may fall through to a title that belongs to another one:
         * the bug was exactly a missing case answering with the root's. */
        int titled = 1;
        for (int sc = 0; sc < SETTINGS_SCREEN_COUNT; sc++) {
            const char *t = settings_title(sc);
            if (t[0] == '\0') {
                titled = 0;
            }
            if (sc != SETTINGS_ROOT && strcmp(t, "Settings") == 0) {
                titled = 0;                 /* the fall-through shape */
            }
        }
        check("titles: every screen has its own, none inherits the root's",
              titled);
    }
    check("dt-labels",
          strcmp(settings_label(SETTINGS_DATETIME, 0), "Set Date & Time") == 0 &&
          strcmp(settings_label(SETTINGS_DATETIME, 1), "Time Format") == 0 &&
          strcmp(settings_label(SETTINGS_DATETIME, 2), "Time in Title") == 0);
    check("dt-kinds",
          settings_kind(SETTINGS_DATETIME, 0) == SETTINGS_KIND_SUBMENU &&
          settings_kind(SETTINGS_DATETIME, 1) == SETTINGS_KIND_SELECT &&
          settings_kind(SETTINGS_DATETIME, 2) == SETTINGS_KIND_SELECT);
    check("dt-enter-editor",
          settings_activate(SETTINGS_DATETIME, &s, 0) == SETTINGS_ENTER_SETTIME);
    {
        char buf[SETTINGS_VALUE_MAX];
        int tg = 0, on = 0, num = 0, den = 0;

        /* No clock: the row says so. */
        settings_set_now(0, 0);
        settings_value(SETTINGS_DATETIME, &s, 0, buf, &tg, &on, &num, &den);
        check("dt-now-unset", strcmp(buf, "Not set") == 0);

        /* An epoch this device cannot hold is also "Not set" — a 1970 date on
         * the row would look like a working clock. */
        settings_set_now(1, 1000u);
        settings_value(SETTINGS_DATETIME, &s, 0, buf, &tg, &on, &num, &den);
        check("dt-now-out-of-range", strcmp(buf, "Not set") == 0);

        /* 2026-09-16 10:42:00 local, in both formats. */
        settings_set_now(1, 1789555320u);
        settings_value(SETTINGS_DATETIME, &s, 0, buf, &tg, &on, &num, &den);
        check("dt-now-12h", strcmp(buf, "10:42 AM") == 0);
        settings_value(SETTINGS_DATETIME, &s, 1, buf, &tg, &on, &num, &den);
        check("dt-format-12h", strcmp(buf, "12-hour") == 0);

        check("dt-format-select",
              settings_activate(SETTINGS_DATETIME, &s, 1) == SETTINGS_ACTION_NONE
              && s.time_24h == 1);
        settings_value(SETTINGS_DATETIME, &s, 0, buf, &tg, &on, &num, &den);
        check("dt-now-24h", strcmp(buf, "10:42") == 0);
        settings_value(SETTINGS_DATETIME, &s, 1, buf, &tg, &on, &num, &den);
        check("dt-format-24h", strcmp(buf, "24-hour") == 0);

        settings_value(SETTINGS_DATETIME, &s, 2, buf, &tg, &on, &num, &den);
        check("dt-in-title-off", strcmp(buf, "Off") == 0);
        check("dt-in-title-select",
              settings_activate(SETTINGS_DATETIME, &s, 2) == SETTINGS_ACTION_NONE
              && s.time_in_title == 1);
        settings_value(SETTINGS_DATETIME, &s, 2, buf, &tg, &on, &num, &den);
        check("dt-in-title-on", strcmp(buf, "On") == 0);
    }
    /* The wheel steps the two-valued rows and reports honestly: an even number
     * of detents is not a change, so it earns no disk write. */
    settings_defaults(&s);
    check("dt-wheel",
          settings_adjust(SETTINGS_DATETIME, &s, 1, +1) == 1 && s.time_24h == 1 &&
          settings_adjust(SETTINGS_DATETIME, &s, 1, -1) == 1 && s.time_24h == 0 &&
          settings_adjust(SETTINGS_DATETIME, &s, 2, +3) == 1 &&
          s.time_in_title == 1);
    check("dt-wheel-even-detents-are-not-a-change",
          settings_adjust(SETTINGS_DATETIME, &s, 1, +2) == 0 && s.time_24h == 0 &&
          settings_adjust(SETTINGS_DATETIME, &s, 0, +1) == 0);

    /* settings_defaults() zeroes every clock field — it is the state before
     * config_load() has read anything, so a host stamp surviving it would be a
     * stamp nobody wrote. (kernel/main.c's Reset Settings puts the HOST's two
     * fields back afterwards, so a Reset costs the mark and not the clock; that
     * half is wiring and lives there.) */
    s.host_epoch = 1789555320u; s.host_off_min = 120;
    s.applied_epoch = 1789555320u; s.utc_off_min = 120;
    settings_defaults(&s);
    check("dt-reset-forgets-the-stamp",
          s.host_epoch == 0 && s.applied_epoch == 0 && s.utc_off_min == 0 &&
          s.host_off_min == 0);

    printf("settings_test: %s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
