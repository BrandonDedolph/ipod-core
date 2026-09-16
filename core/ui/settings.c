/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/settings.c — the Settings state model (no framebuffer, no hardware).
 *
 * The pure half of the Settings subsystem: defaults, the generic per-row view
 * (count / label / kind / value) that lets main.c drive navigation without
 * hardcoding a screen, and the SELECT/wheel mutators (activate / adjust). It
 * touches nothing but settings_t and stdint, so the host unit test compiles this
 * exact source. The renderer lives in screen_settings.c.
 *
 * Freestanding, integer-only: no libc/libm/malloc. Small string formatting is
 * done by hand into caller buffers.
 */

#include "settings.h"
#include "palette.h"                   /* THEME_* ids + THEME_COUNT (header only) */
#include "eq.h"                       /* EQ preset names + the locked shelves   */
#include "../kernel/datetime.h"       /* the Date & Time row's clock formatter  */

/* ---------------------------------------------------------------------------
 * Small freestanding helpers
 * ------------------------------------------------------------------------- */

/* Copy a NUL-terminated string (no libc). Caller guarantees the destination is
 * large enough (all uses below write <= 20 bytes into a >= 24-byte buffer). */
static void scopy(char *d, const char *s)
{
    while (*s) {
        *d++ = *s++;
    }
    *d = '\0';
}

/* Write unsigned `v` as decimal at `d`; returns the digit count. */
static int u_to_str(char *d, unsigned v)
{
    char tmp[10];
    int t = 0;
    do {
        tmp[t++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v && t < 10);
    int i = 0;
    while (t > 0) {
        d[i++] = tmp[--t];
    }
    d[i] = '\0';
    return i;
}

/* Clamp `v` into [lo, hi]. */
static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Format a signed dB value: "0 dB" / "+N dB" / "-N dB". */
static void fmt_db(char *d, int v)
{
    int i = 0;
    if (v > 0) {
        d[i++] = '+';
    } else if (v < 0) {
        d[i++] = '-';
        v = -v;
    }
    i += u_to_str(d + i, (unsigned)v);
    scopy(d + i, " dB");
}

/* Format a percent: "NN%". */
static void fmt_pct(char *d, int v)
{
    int i = u_to_str(d, (unsigned)(v < 0 ? 0 : v));
    d[i++] = '%';
    d[i] = '\0';
}

/* Format a balance value: "Center" / "Left N" / "Right N". */
static void fmt_balance(char *d, int v)
{
    if (v == 0) {
        scopy(d, "Center");
        return;
    }
    const char *side = v < 0 ? "Left " : "Right ";
    int i = 0;
    while (side[i]) { d[i] = side[i]; i++; }
    if (v < 0) v = -v;
    u_to_str(d + i, (unsigned)v);
}

/* ---------------------------------------------------------------------------
 * Sound: the volume ceiling and the EQ-owned shelves
 * ------------------------------------------------------------------------- */

int settings_volume_clamp(const settings_t *s, int v)
{
    /* The limit itself is clamped first: the field's range is 10..100, and a
     * record that somehow held 0 would otherwise mute the device with no row
     * explaining why (config.c reads a 0 byte as "unset" for the same
     * reason). */
    return clampi(v, 0, clampi(s->volume_limit, 10, 100));
}

int settings_row_locked(int screen, const settings_t *s, int idx)
{
    /* Bass (3) and Treble (4) while a preset is on, and nothing else. The
     * preset test is eq.c's: an id this build has no curve for reads as Off
     * everywhere, so the rows must not lock over a value whose shelf gains
     * they would then report as the user's own. */
    return screen == SETTINGS_SOUND && (idx == 3 || idx == 4) &&
           s->eq > EQ_OFF && s->eq < EQ_PRESET_COUNT;
}

const char *settings_eq_name(int preset)
{
    return eq_preset_name(preset);
}

/* The dB the Bass (band 0) / Treble (band 4) row should SHOW: the preset's
 * shelf gain while one is active, the user's own value otherwise. One call
 * into eq.c covers both — eq_effective_curve is where "who owns the shelves"
 * is decided, and this row must never answer it differently. */
static int shelf_db(const settings_t *s, int band)
{
    eq_curve_t c;
    eq_effective_curve(s->eq, s->bass, s->treble, &c);
    return c.gain_db[band];
}

/* ---------------------------------------------------------------------------
 * Backlight-timeout discrete steps (0=never / 5 / 10 / 15 / 30 / 60 seconds)
 * ------------------------------------------------------------------------- */
static const int BL_OPTS[6] = { 0, 5, 10, 15, 30, 60 };

static int bl_index(int secs)
{
    for (int i = 0; i < 6; i++) {
        if (BL_OPTS[i] == secs) {
            return i;
        }
    }
    return 3;                                  /* default to 15 s if unknown */
}

/* Step to the next backlight option in direction `dir` (+/-). `wrap` wraps the
 * ends (used by SELECT); otherwise it clamps (used by the wheel). */
static int bl_step(int secs, int dir, int wrap)
{
    int i = bl_index(secs) + (dir > 0 ? 1 : -1);
    if (wrap) {
        i = (i + 6) % 6;
    } else {
        i = clampi(i, 0, 5);
    }
    return BL_OPTS[i];
}

/* ---------------------------------------------------------------------------
 * Sleep-timer discrete steps (0=off / 15 / 30 / 60 / 90 / 120 minutes)
 * ------------------------------------------------------------------------- */
static const int SLEEP_OPTS[6] = { 0, 15, 30, 60, 90, 120 };

static int sleep_index(int mins)
{
    for (int i = 0; i < 6; i++) {
        if (SLEEP_OPTS[i] == mins) {
            return i;
        }
    }
    return 0;                        /* an unknown duration reads as Off     */
}

/* Step to the next sleep-timer option, wrapping. SELECT-only (the wheel moves
 * the selection on a SELECT row), so there is no clamping variant. */
static int sleep_step(int mins)
{
    return SLEEP_OPTS[(sleep_index(mins) + 1) % 6];
}

/* ---------------------------------------------------------------------------
 * Row labels (stable .rodata tables, one per list screen)
 * ------------------------------------------------------------------------- */
/* Only rows that actually do something are listed — the cosmetic placeholders
 * (Crossfade, Replaygain, Skip Length, Stereo Width, Shortcuts, Language) were
 * removed so the menu never presents a control that has no effect. */
static const char *const ROOT_L[10] = {
    "Playback", "Sound", "Theme", "Display", "Clicker", "Date & Time",
    "About", "Boot Details", "Disk Mode", "Reset Settings",
};
/* Resume is back on this list: it was pulled with the other placeholders while
 * nothing could persist it, and it is now the switch that decides whether boot
 * re-opens the track you left off on (kernel/main.c resume_restore). Sleep
 * Timer sits last: it is the only row here that does not change how music
 * plays, and the one you reach for at night. */
static const char *const PLAY_L[4] = { "Shuffle", "Repeat", "Resume",
                                       "Sleep Timer" };
/* Volume Limit sits under Volume because it is the same bar; EQ sits above
 * Bass/Treble because, while it is on, it owns them. */
static const char *const SOUND_L[6] = {
    "Volume", "Volume Limit", "EQ", "Bass", "Treble", "Balance",
};
static const char *const DISP_L[2] = { "Backlight", "Brightness" };
/* Date & Time. The editor is behind the first row rather than being the screen
 * itself because the other two are ordinary rows and a screen that is half a
 * list and half a widget reads as neither. */
static const char *const DT_L[3] = { "Set Date & Time", "Time Format",
                                     "Time in Title" };
/* Theme picker rows, in THEME_* id order (ui/palette.h) — the id IS the row. */
static const char *const THEME_L[THEME_COUNT] = {
    [THEME_LINEN]    = "Linen",
    [THEME_ONYX]     = "Onyx",
    [THEME_SAGE]     = "Sage",
    [THEME_PLASTER]  = "Plaster",
    [THEME_OLIVE]    = "Olive",
    [THEME_UMBER]    = "Umber",
    [THEME_MUSHROOM] = "Mushroom",
};
/* Clicker: index 0 = Off, 1..N = sound profiles (main.c maps to piezo tones). */
static const char *const CLICK_L[8] = {
    "Off", "Tick", "Click", "Pop", "Blip", "Tock", "Double", "Chirp",
};
#define CLICK_N ((int)(sizeof CLICK_L / sizeof CLICK_L[0]))

/* ---------------------------------------------------------------------------
 * Public model API
 * ------------------------------------------------------------------------- */

void settings_defaults(settings_t *s)
{
    s->shuffle           = SHUFFLE_OFF;
    s->repeat            = REPEAT_OFF;
    s->resume_on_startup = 1;
    s->crossfade         = 0;
    s->volume            = 70;
    s->volume_limit      = 100;            /* 100 = no limit                 */
    s->eq                = EQ_OFF;
    s->bass              = 0;
    s->treble            = 0;
    s->balance           = 0;
    s->backlight_secs    = 15;
    s->backlight_bright  = 32;
    s->theme             = 0;
    s->clicker           = 1;
    /* Nothing to resume from a fresh install — and "Reset Settings" routes
     * through here too, so it also forgets where you were. */
    s->resume_hash       = 0;
    s->resume_secs       = 0;
    s->resume_total      = 0;
    s->resume_kind       = RESUME_KIND_NONE;
    s->resume_flags      = 0;
    s->resume_qidx       = 0;
    s->resume_seed       = 0;
    s->resume_order_seed = 0;
    s->resume_order_keep = 0;
    s->resume_ctx_hash   = 0;
    /* Off, like every boot: the timer is never restored from disk. Reset
     * Settings routes through here too, so it also disarms (main.c re-applies
     * the field to ui/sleeptimer.c after a reset). */
    s->sleep_timer_min   = 0;
    /* The clock. 12-hour and no clock in the title bar, which is what the
     * device does today. Reset Settings routes through here, so it also
     * forgets the host's stamp AND the mark — and the next boot therefore
     * re-applies the stamp, which is the right reading of "reset": the rule
     * that stops a stale stamp from being applied is the RTC's own position
     * (kernel/timesync.h, rule 5), not the mark alone. */
    s->time_24h          = 0;
    s->time_in_title     = 0;
    s->utc_off_min       = 0;
    s->host_epoch        = 0;
    s->host_off_min      = 0;
    s->applied_epoch     = 0;
}

/* ---------------------------------------------------------------------------
 * The injected clock (settings.h, settings_set_now)
 * ------------------------------------------------------------------------- */
static int      g_now_valid;
static uint32_t g_now_local;

void settings_set_now(int valid, uint32_t local_epoch)
{
    g_now_valid = valid ? 1 : 0;
    g_now_local = local_epoch;
}

int settings_count(int screen)
{
    switch (screen) {
    case SETTINGS_ROOT:     return 10;
    case SETTINGS_PLAYBACK: return 4;
    case SETTINGS_SOUND:    return 6;
    case SETTINGS_DISPLAY:  return 2;
    case SETTINGS_ABOUT:    return 1;   /* non-interactive info page */
    case SETTINGS_DIAG:     return 1;   /* non-interactive info page */
    case SETTINGS_DATETIME: return 3;
    case SETTINGS_SETTIME:  return 1;   /* the editor; main.c drives it */
    case SETTINGS_THEME:    return THEME_COUNT;
    case SETTINGS_CLICKER:  return CLICK_N;   /* Off + sound profiles */
    default:                return 0;
    }
}

const char *settings_label(int screen, int idx)
{
    if (idx < 0 || idx >= settings_count(screen)) {
        return "";
    }
    switch (screen) {
    case SETTINGS_ROOT:     return ROOT_L[idx];
    case SETTINGS_PLAYBACK: return PLAY_L[idx];
    case SETTINGS_SOUND:    return SOUND_L[idx];
    case SETTINGS_DISPLAY:  return DISP_L[idx];
    case SETTINGS_DATETIME: return DT_L[idx];
    case SETTINGS_THEME:    return THEME_L[idx];
    case SETTINGS_CLICKER:  return CLICK_L[idx];
    default:                return "";
    }
}

const char *settings_theme_name(int theme)
{
    if (theme < 0 || theme >= THEME_COUNT) {
        return THEME_L[THEME_LINEN];   /* what palette.c renders for it too */
    }
    return THEME_L[theme];
}

const char *settings_clicker_name(int profile)
{
    if (profile < 0 || profile >= CLICK_N) {
        return "Off";
    }
    return CLICK_L[profile];
}

int settings_kind(int screen, int idx)
{
    switch (screen) {
    case SETTINGS_ROOT:
        /* Disk Mode + Reset Settings both fire on SELECT; the rest descend. */
        if (idx == 8 || idx == 9) return SETTINGS_KIND_ACTION;
        return SETTINGS_KIND_SUBMENU;                 /* incl. Clicker submenu */
    case SETTINGS_CLICKER:
        return SETTINGS_KIND_SELECT;                  /* radio pick, marked active */
    case SETTINGS_PLAYBACK:
        return SETTINGS_KIND_SELECT;       /* all four: cycling selects        */
    case SETTINGS_SOUND:
        /* Every row is a slider but EQ, which cycles named presets. */
        return (idx == 2) ? SETTINGS_KIND_SELECT : SETTINGS_KIND_SLIDER;
    case SETTINGS_DISPLAY:
        return (idx == 1) ? SETTINGS_KIND_SLIDER : SETTINGS_KIND_SELECT;
    case SETTINGS_THEME:
        return SETTINGS_KIND_THEME;
    case SETTINGS_DATETIME:
        /* Set Date & Time descends into the editor; the other two cycle. */
        return (idx == 0) ? SETTINGS_KIND_SUBMENU : SETTINGS_KIND_SELECT;
    case SETTINGS_ABOUT:
    case SETTINGS_DIAG:
    case SETTINGS_SETTIME:
        return SETTINGS_KIND_INFO;
    default:
        return SETTINGS_KIND_SELECT;
    }
}

void settings_value(int screen, const settings_t *s, int idx,
                    char *buf, int *is_toggle, int *toggle_on,
                    int *num, int *den)
{
    buf[0] = '\0';
    *is_toggle = 0;
    *toggle_on = 0;
    *num = 0;
    *den = 0;

    switch (screen) {
    case SETTINGS_ROOT:
        /* Theme + Clicker carry a right value (the current choice); rest chevrons. */
        if (idx == 2) {
            scopy(buf, settings_theme_name(s->theme));
        } else if (idx == 4) {
            scopy(buf, settings_clicker_name(s->clicker));
        }
        break;

    case SETTINGS_CLICKER:
        if (idx == s->clicker) {
            scopy(buf, "\x03");                 /* middot marks the active profile */
        }
        break;

    case SETTINGS_PLAYBACK:
        switch (idx) {
        case 0: scopy(buf, s->shuffle == SHUFFLE_SONGS  ? "Songs"
                         : s->shuffle == SHUFFLE_ALBUMS ? "Albums" : "Off");
                break;
        case 1: scopy(buf, s->repeat == REPEAT_OFF ? "Off"
                         : s->repeat == REPEAT_ALL ? "All" : "One"); break;
        case 2: scopy(buf, s->resume_on_startup ? "On" : "Off"); break;
        /* The CHOSEN duration, not the countdown — the minutes left live in
         * the status strip's SLEEP token (kernel/main.c). */
        case 3:
            if (s->sleep_timer_min <= 0) {
                scopy(buf, "Off");
            } else {
                int n = u_to_str(buf, (unsigned)s->sleep_timer_min);
                scopy(buf + n, " min");
            }
            break;
        default: break;
        }
        break;

    case SETTINGS_SOUND:
        /* Bass/Treble read through shelf_db(), so a locked row shows the
         * PRESET's shelf gain rather than a stored value that is not
         * currently reaching the codec. */
        switch (idx) {
        case 0: fmt_pct(buf, s->volume); *num = s->volume;      *den = 100; break;
        case 1: { int lim = clampi(s->volume_limit, 10, 100);
                  fmt_pct(buf, lim);     *num = lim;            *den = 100; }
                break;
        case 2: scopy(buf, settings_eq_name(s->eq)); break;
        case 3: { int db = shelf_db(s, 0);
                  fmt_db(buf, db);       *num = db + 12;        *den = 24; }
                break;
        case 4: { int db = shelf_db(s, EQ_BANDS - 1);
                  fmt_db(buf, db);       *num = db + 12;        *den = 24; }
                break;
        case 5: fmt_balance(buf, s->balance);
                *num = s->balance + 100; *den = 200; break;
        default: break;
        }
        break;

    case SETTINGS_DATETIME:
        switch (idx) {
        /* The clock as it is right now, in the format the row below selects —
         * injected by main.c (settings_set_now), because this module has no
         * idea what time it is. */
        case 0: {
            datetime_t now;
            if (!g_now_valid || !datetime_from_epoch(g_now_local, &now) ||
                datetime_fmt_time(buf, SETTINGS_VALUE_MAX, &now,
                                  s->time_24h) == 0) {
                scopy(buf, "Not set");
            }
            break;
        }
        case 1: scopy(buf, s->time_24h ? "24-hour" : "12-hour"); break;
        case 2: scopy(buf, s->time_in_title ? "On" : "Off"); break;
        default: break;
        }
        break;

    case SETTINGS_DISPLAY:
        if (idx == 0) {
            if (s->backlight_secs == 0) {
                scopy(buf, "Never");
            } else {
                int i = u_to_str(buf, (unsigned)s->backlight_secs);
                scopy(buf + i, " sec");
            }
        } else if (idx == 1) {
            int pc = s->backlight_bright * 100 / 32;
            fmt_pct(buf, pc);
            *num = s->backlight_bright;
            *den = 32;
        }
        break;

    default:
        break;                                 /* Theme/About draw their own */
    }
}

/*
 * NONE only where *s was actually written; NOOP everywhere else. The
 * distinction is what stops SELECT on an info page from reaching the disk
 * (settings.h, settings_action_t). Note the fix lives HERE, in what is
 * reported, and deliberately not in settings_touch() or the commit gate: a
 * touch must always record a pending change (the resume-position captures
 * legitimately touch this same record, and a memcmp against the last saved
 * copy would suppress those too), and the gate is where a previous session
 * hit a deadlock against the DISKSAFE flush. Fewer touches, never fewer
 * commits.
 */
int settings_activate(int screen, settings_t *s, int idx)
{
    switch (screen) {
    case SETTINGS_ROOT:
        switch (idx) {
        case 0: return SETTINGS_ENTER_PLAYBACK;
        case 1: return SETTINGS_ENTER_SOUND;
        case 2: return SETTINGS_ENTER_THEME;
        case 3: return SETTINGS_ENTER_DISPLAY;
        case 4: return SETTINGS_ENTER_CLICKER;
        case 5: return SETTINGS_ENTER_DATETIME;
        case 6: return SETTINGS_ENTER_ABOUT;
        case 7: return SETTINGS_ENTER_DIAG;
        case 8: return SETTINGS_ACTION_DISKMODE;
        case 9: return SETTINGS_ACTION_RESET;
        default: return SETTINGS_ACTION_NOOP;
        }

    case SETTINGS_DATETIME:
        switch (idx) {
        case 0: return SETTINGS_ENTER_SETTIME;
        /* Both rows are two-valued, so SELECT always changes something. The
         * format is a display preference and the clock in the title bar is a
         * display preference; neither touches the RTC. */
        case 1: s->time_24h = !s->time_24h; return SETTINGS_ACTION_NONE;
        case 2: s->time_in_title = !s->time_in_title;
                return SETTINGS_ACTION_NONE;
        default: return SETTINGS_ACTION_NOOP;
        }

    case SETTINGS_PLAYBACK:
        switch (idx) {
        /* Off -> Songs -> Albums -> Off. Three states rather than a toggle
         * since Albums arrived, so — like Repeat below — every press is a
         * change and there is no NOOP step to report. */
        case 0: s->shuffle = (shuffle_mode_t)((s->shuffle + 1) % 3);
                return SETTINGS_ACTION_NONE;
        case 1: s->repeat = (repeat_mode_t)((s->repeat + 1) % 3);
                return SETTINGS_ACTION_NONE;
        /* Turning Resume OFF does not clear the stored locator here — this
         * module is pure, and main.c owns that (it drops it on the next pass,
         * so the record on disk stops carrying a position you asked it to
         * forget). */
        case 2: s->resume_on_startup = !s->resume_on_startup;
                return SETTINGS_ACTION_NONE;
        /* Wraps through six options, so this is always a change — including
         * the step that lands back on the duration already showing, which
         * main.c treats as a deliberate restart of the countdown. Never NONE:
         * nothing on disk stores this, and a touch would write a
         * byte-identical record (settings.h, SETTINGS_ACTION_SLEEPTIMER). */
        case 3: s->sleep_timer_min = sleep_step(s->sleep_timer_min);
                return SETTINGS_ACTION_SLEEPTIMER;
        default: return SETTINGS_ACTION_NOOP;
        }

    case SETTINGS_DISPLAY:
        if (idx == 0) {
            /* Wraps through six options, so this is always a change. */
            s->backlight_secs = bl_step(s->backlight_secs, +1, 1 /*wrap*/);
            return SETTINGS_ACTION_NONE;
        }
        return SETTINGS_ACTION_NOOP;       /* Brightness is wheel-adjusted */

    case SETTINGS_THEME:
        if (idx >= 0 && idx < THEME_COUNT && idx != s->theme) {
            s->theme = idx;
            return SETTINGS_ACTION_NONE;
        }
        return SETTINGS_ACTION_NOOP;       /* re-picking the active theme */

    case SETTINGS_CLICKER:
        if (idx >= 0 && idx < CLICK_N && idx != s->clicker) {
            s->clicker = idx;
            return SETTINGS_ACTION_NONE;
        }
        return SETTINGS_ACTION_NOOP;       /* re-picking the active profile */

    case SETTINGS_SOUND:
        /* EQ cycles Off -> the 17 presets -> Off. Eighteen values is a long
         * cycle for one row, but a picker screen needs MENU handling that
         * main.c's ROOT/non-ROOT pop does not have yet. Every press lands on
         * a different preset, so this branch is always a change. */
        if (idx == 2) {
            s->eq = (s->eq + 1) % EQ_PRESET_COUNT;
            return SETTINGS_ACTION_NONE;
        }
        return SETTINGS_ACTION_NOOP;       /* the rest are wheel-adjusted */

    default:
        /* ABOUT and DIAG (info pages) and anything out of range: SELECT does
         * nothing, and says so. */
        return SETTINGS_ACTION_NOOP;
    }
}

int settings_adjust(int screen, settings_t *s, int idx, int delta)
{
    if (delta == 0) {
        return 0;
    }
    /* Each branch computes the new value and reports whether it moved, so a
     * wheel already pinned at a rail (volume 100, brightness 32) does not
     * count as a change and does not earn a disk write. */
    int old, nv;
    switch (screen) {
    case SETTINGS_SOUND:
        if (settings_row_locked(SETTINGS_SOUND, s, idx)) {
            return 0;        /* an EQ preset owns this shelf; wheel ignored */
        }
        switch (idx) {
        case 0: old = s->volume;  nv = settings_volume_clamp(s, old + delta);
                s->volume = nv;  return nv != old;
        /* Lowering the ceiling under the current volume pulls the volume down
         * in the SAME call, so the caller's one settings_apply() pushes both
         * to the codec and *s can never leave here with volume > limit. */
        case 1: old = s->volume_limit; nv = clampi(old + delta, 10, 100);
                s->volume_limit = nv;
                if (s->volume > nv) { s->volume = nv; }
                return nv != old;
        case 2: return 0;                      /* EQ is a SELECT row       */
        case 3: old = s->bass;    nv = clampi(old + delta, -12, 12);
                s->bass = nv;    return nv != old;
        case 4: old = s->treble;  nv = clampi(old + delta, -12, 12);
                s->treble = nv;  return nv != old;
        case 5: old = s->balance; nv = clampi(old + delta, -100, 100);
                s->balance = nv; return nv != old;
        default: return 0;
        }

    case SETTINGS_DATETIME:
        /* The wheel steps the two-valued rows the way it steps Backlight: any
         * detent lands on the other value, and an even count lands back where
         * it started, so the record only moves when the value does. */
        if (idx == 1 || idx == 2) {
            int *field = (idx == 1) ? &s->time_24h : &s->time_in_title;
            if ((delta & 1) == 0) {
                return 0;
            }
            *field = !*field;
            return 1;
        }
        return 0;

    case SETTINGS_DISPLAY:
        if (idx == 1) {
            old = s->backlight_bright;
            nv  = clampi(old + delta, 1, 32);
            s->backlight_bright = nv;
            return nv != old;
        } else if (idx == 0) {
            old = s->backlight_secs;
            nv  = bl_step(old, delta, 0 /*clamp*/);
            s->backlight_secs = nv;
            return nv != old;
        }
        return 0;

    default:
        return 0;
    }
}
