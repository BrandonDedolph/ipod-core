/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/settings.h — data-driven Settings model + render API.
 *
 * A self-contained Settings subsystem for the Linen menu UI
 * (design_reference/menus.jsx SettingsMenu/SettingsPlayback/SettingsSound/
 * SettingsAbout + system-screens.jsx ThemePicker). The state lives in a plain
 * settings_t owned by the caller (kernel/main.c); this module is a pure,
 * freestanding model over it plus a matching renderer:
 *
 *   - settings_count / settings_label / settings_kind / settings_value give
 *     main.c a GENERIC per-row view of each screen, so navigation can be driven
 *     without hardcoding any one screen.
 *   - settings_activate handles SELECT on a row: it either mutates *s in place
 *     (toggle a bool, cycle a select) and returns SETTINGS_ACTION_NONE, or — for
 *     a submenu/reset row — returns an action code for main.c to switch on.
 *   - settings_adjust handles a wheel tick on a slider row (Sound / Display),
 *     nudging the value clamped to its range.
 *   - settings_render / settings_about_render draw the full 320x240 panel into
 *     the console framebuffer, matching the jsx.
 *
 * FUNCTIONAL fields drive real hardware once main.c wires them: shuffle, repeat
 * (player), volume (mirrors hal_volume), backlight_secs + backlight_bright
 * (backlight HAL), resume_on_startup (kernel/main.c re-opens the saved track at
 * boot). COSMETIC fields render + store but nothing consumes them yet
 * (crossfade, bass, treble, balance) — flagged at their declarations below.
 *
 * Freestanding: integer-only, no libc/libm/malloc, no allocation. The model
 * half (settings.c) has no hardware or framebuffer dependency at all, so it
 * host-compiles into the unit test unchanged.
 */

#ifndef CORE_UI_SETTINGS_H
#define CORE_UI_SETTINGS_H

#include <stdint.h>

/* Repeat policy (FUNCTIONAL — consumed by the player's auto-advance). */
typedef enum { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE } repeat_mode_t;

/*
 * The whole persisted settings state. Ranges + which fields are live are noted
 * per field; keep this in sync with settings_defaults() and the clamps in
 * settings_adjust().
 */
typedef struct {
    int  shuffle;            /* 0/1 — FUNCTIONAL (player queue order)          */
    repeat_mode_t repeat;    /* FUNCTIONAL (player auto-advance)               */
    int  resume_on_startup;  /* 0/1 — FUNCTIONAL (boot re-opens the last track)*/
    int  crossfade;          /* 0/1 — COSMETIC (no crossfade mixer yet)       */
    int  volume;             /* 0..100 — FUNCTIONAL (mirrors hal_volume)      */
    int  bass, treble;       /* -12..12 dB — COSMETIC (no EQ wired yet)       */
    int  balance;            /* -100..100 — COSMETIC                          */
    int  backlight_secs;     /* 0=never / 5/10/15/30/60 — FUNCTIONAL          */
    int  backlight_bright;   /* 1..32 — FUNCTIONAL                            */
    int  theme;              /* THEME_* id (ui/palette.h): 0 Linen, 1 Onyx,   */
                             /* 2 Sage, 3 Plaster, 4 Olive, 5 Umber,          */
                             /* 6 Mushroom — FUNCTIONAL (palette.c theme_set) */
    int  clicker;            /* 0/1 — FUNCTIONAL (piezo click on navigation)  */

    /*
     * RESUME LOCATOR — runtime state, not a user preference. No Settings row
     * shows these and settings_activate() never touches them; they ride along
     * in settings_t only because kernel/config.c's record is the one thing on
     * this device that survives a power cut, and a second record would mean a
     * second write.
     *
     * resume_hash is the case/quote-folded name_hash() of the playing track's
     * EXT-TRIMMED filename — a NAME, deliberately, because a queue index or a
     * cluster stops meaning anything the moment the library is rebuilt or the
     * disk is re-imported, while the filename survives both. 0 = nothing to
     * resume, and the codec enforces that (hash 0 zeroes the other two).
     *
     * resume_total is the track length as the decoder reported it, kept purely
     * as a CROSS-CHECK: filenames like "01 Intro" repeat across a library, so
     * the restore only trusts a name match that the duration also agrees with
     * (or a name that is unique). Resuming the wrong track is worse than not
     * resuming, so the ambiguous case declines.
     */
    uint32_t resume_hash;    /* name_hash of the track filename; 0 = none     */
    uint32_t resume_secs;    /* elapsed seconds within that track             */
    uint32_t resume_total;   /* that track's length, as a sanity cross-check  */

    /*
     * RESUME QUEUE CONTEXT — what the track was playing IN, so the boot path
     * can rebuild that queue instead of the track's album. Every queue this
     * firmware builds is a deterministic function of the library plus a few
     * words: which list (resume_kind — the song's own artist/genre name the
     * list), the LCG seed Shuffle Songs drew the library order from
     * (resume_seed), and the player's shuffle deal (resume_order_seed +
     * resume_order_keep, see player_order_seed). resume_qidx is where in that
     * queue the track sat, a hint for the rebuild and the "N of M" the user
     * remembers. resume_ctx_hash names the one context the song's record
     * cannot: under RESUME_KIND_PLAYLIST it is name_hash() of the playlist's
     * ext-trimmed filename (the same folding as the locator), 0 for every
     * other kind. All meaningless while resume_kind is RESUME_KIND_NONE, and
     * zeroed with the locator (hash 0) on both sides of the codec.
     * resume_flags is reserved (written 0, ignored).
     */
    uint8_t  resume_kind;    /* RESUME_KIND_*                                 */
    uint8_t  resume_flags;   /* reserved                                      */
    uint16_t resume_qidx;    /* queue index of the track when captured        */
    uint32_t resume_seed;    /* Shuffle Songs' library-order LCG seed         */
    uint32_t resume_order_seed;  /* player_order_seed() — 0 = no deal         */
    int      resume_order_keep;  /* player_order_keep(); PLAYER_KEEP_*        */
    uint32_t resume_ctx_hash;/* KIND_PLAYLIST: the playlist's name hash       */
} settings_t;

/* What kind of queue the resume locator's track was playing in. On disk as
 * one byte; unknown values read back as NONE (the album fallback). */
enum {
    RESUME_KIND_NONE     = 0,  /* unknown — rebuild the track's album         */
    RESUME_KIND_ALBUM    = 1,  /* the album folder, entered from the browser  */
    RESUME_KIND_SONGS    = 2,  /* Songs: every song, title order              */
    RESUME_KIND_ARTIST   = 3,  /* an artist's All Songs                       */
    RESUME_KIND_GENRE    = 4,  /* a genre's songs                             */
    RESUME_KIND_SHUFFLE  = 5,  /* Shuffle Songs: the library in seed order    */
    RESUME_KIND_PLAYLIST = 6,  /* an M3U8 playlist: resume_ctx_hash names it  */
    RESUME_KIND_MAX      = RESUME_KIND_PLAYLIST
};

/* Populate `s` with sensible defaults (shuffle off, repeat off, volume 70,
 * backlight 15 s at full brightness, Linen theme). */
void settings_defaults(settings_t *s);

/*
 * The Settings screens. main.c keeps the current screen + a selection index and
 * calls the generic accessors below against them. SETTINGS_SCREEN_COUNT is the
 * count sentinel, not a real screen.
 */
typedef enum {
    SETTINGS_ROOT,       /* the top Settings menu                             */
    SETTINGS_PLAYBACK,   /* Shuffle / Repeat / Crossfade / … toggles+selects  */
    SETTINGS_SOUND,      /* Volume / Bass / Treble / Balance / Width sliders  */
    SETTINGS_DISPLAY,    /* Backlight timeout (select) + Brightness (slider)  */
    SETTINGS_ABOUT,      /* device info key/value rows                        */
    SETTINGS_THEME,      /* theme picker (swatch rows)                        */
    SETTINGS_CLICKER,    /* clicker profile picker (Off / sound profiles)     */
    /*
     * Boot Details: the cold-boot phase breakdown and the settings-file
     * locator. Diagnostics, deliberately on their own page — About is the
     * "what is this device" screen a user reads, and hex LBAs were crowding
     * it (they collided with the stat columns outright). Non-interactive,
     * rendered wholly by main.c, which is the only place that has the numbers.
     */
    SETTINGS_DIAG,
    SETTINGS_SCREEN_COUNT
} settings_screen_t;

/*
 * Return codes from settings_activate(). NONE means "handled in place — the
 * record WAS mutated, persist it — stay on this screen"; NOOP means "nothing
 * changed, there is nothing to persist"; the ENTER_* codes ask main.c to push
 * the named sub-screen; RESET asks it to restore defaults.
 *
 * NONE vs NOOP is the difference between a disk write and none. Every
 * settings_touch() ends, 3 s later, in config_save() -> ata_write_sectors():
 * the ONLY code in this firmware that writes to the user's disk. Before NOOP
 * existed, activate() answered NONE both when it changed the record and when
 * it did nothing (SELECT on the About page, re-picking the theme already
 * active), and main.c touched on every NONE — so pressing SELECT on About
 * spun the drive up and burned a config slot for a byte-identical record.
 */
typedef enum {
    SETTINGS_ACTION_NONE = 0,
    SETTINGS_ENTER_PLAYBACK,
    SETTINGS_ENTER_SOUND,
    SETTINGS_ENTER_DISPLAY,
    SETTINGS_ENTER_ABOUT,
    SETTINGS_ENTER_THEME,
    SETTINGS_ENTER_CLICKER,
    SETTINGS_ENTER_DIAG,
    SETTINGS_ACTION_RESET,
    /* Reboot into the Apple boot ROM's USB mass-storage mode. Lives under
     * Settings rather than the main menu: it is a maintenance action, not a
     * place you browse to. */
    SETTINGS_ACTION_DISKMODE,
    /* Appended LAST so every value above keeps its number: main.c switches on
     * these and the config record does not store them, but nothing is gained
     * by renumbering either. */
    SETTINGS_ACTION_NOOP
} settings_action_t;

/*
 * Per-row kind, so a caller (and the renderer) can treat any screen generically:
 *   SUBMENU  a row that enters another screen (chevron, or a right value)
 *   ACTION   a row that fires an action on SELECT (Reset Settings)
 *   TOGGLE   an on/off pill (SELECT flips it)
 *   SELECT   a right-aligned text value (SELECT cycles it)
 *   SLIDER   a label + fill bar (the wheel adjusts it)
 *   THEME    a theme-picker swatch row
 *   INFO     an About key/value row
 */
typedef enum {
    SETTINGS_KIND_SUBMENU,
    SETTINGS_KIND_ACTION,
    SETTINGS_KIND_TOGGLE,
    SETTINGS_KIND_SELECT,
    SETTINGS_KIND_SLIDER,
    SETTINGS_KIND_THEME,
    SETTINGS_KIND_INFO
} settings_kind_t;

/* Number of selectable rows on `screen` (About returns 1 — it is a non-
 * interactive info page). Out-of-range screens return 0. */
int settings_count(int screen);

/* The row label for (screen, idx), or "" if out of range. Stable .rodata. */
const char *settings_label(int screen, int idx);

/* The display name of theme id `theme` ("Linen", "Onyx", "Sage", "Plaster",
 * "Olive", "Umber", "Mushroom" — ui/palette.h THEME_* order). An id outside
 * that range names "Linen", which is also what palette.c renders for it. */
const char *settings_theme_name(int theme);

/* The display name of clicker profile `profile` ("Off"/"Tick"/"Click"/"Pop"). */
const char *settings_clicker_name(int profile);

/* The settings_kind_t of row (screen, idx). */
int settings_kind(int screen, int idx);

/*
 * Fill in the render-facing value of row (screen, idx):
 *   buf         (>= 24 bytes) receives the display text for SELECT/SLIDER/
 *               submenu-with-value rows ("" otherwise).
 *   *is_toggle  set to 1 for a TOGGLE row (then *toggle_on = its state).
 *   *num,*den   for a SLIDER row, the fill fraction num/den (den>0); 0 else.
 * Every out-param is always written. Pure — no side effects on *s.
 */
void settings_value(int screen, const settings_t *s, int idx,
                    char *buf, int *is_toggle, int *toggle_on,
                    int *num, int *den);

/*
 * Apply SELECT to row (screen, idx): mutate *s in place for a toggle/select and
 * return SETTINGS_ACTION_NONE, or return a settings_action_t for main.c to act
 * on (enter a sub-screen / reset). Returns SETTINGS_ACTION_NOOP — and leaves
 * *s byte-identical — from every branch that has nothing to do: info pages,
 * slider screens, out-of-range rows, and re-picking the value already set.
 */
int settings_activate(int screen, settings_t *s, int idx);

/*
 * Apply a wheel tick of `delta` to a SLIDER row (Sound values, Display
 * Brightness) or step a discrete SELECT (Display Backlight). Clamped to range.
 * Returns 1 if *s changed, 0 if not — at a rail, on a non-adjustable row, or
 * for delta 0 — so the caller can skip the persist (see settings_action_t).
 */
int settings_adjust(int screen, settings_t *s, int idx, int delta);

/*
 * Render the full 320x240 panel for `screen` into the console framebuffer
 * (matches the jsx). For SETTINGS_ABOUT this draws placeholder dashes — main.c
 * should call settings_about_render() directly with the live device values.
 */
void settings_render(int screen, const settings_t *s, int sel);

/* The on-disk event log's state, for the About screen's "LOG" line. */
#define ABOUT_LOG_OFF 0     /* no CORELOG.BIN, or it did not validate       */
#define ABOUT_LOG_ON  1     /* writing; log_seq is the next block's number  */
#define ABOUT_LOG_ERR 2     /* turned itself off after failed writes        */

/*
 * Render the About screen from live values (main.c owns these — do not
 * fabricate). free/total are whole megabytes; pct<0 or mv<=0 render as "--".
 * log_seq / log_state describe the event log (kernel/evlog.c): the line
 * reads "LOG <seq> on", "LOG off" or "LOG <seq> err".
 */
void settings_about_render(int battery_pct, int battery_mv, int battery_raw,
                           uint32_t total_mb, uint32_t free_mb,
                           int n_songs, int n_albums, int n_artists,
                           uint32_t log_seq, int log_state);

/*
 * Render the Boot Details screen (SETTINGS_DIAG). All times are milliseconds
 * as MEASURED — a 0 renders as "--" rather than a fake zero, and the screen
 * derives "OTHER" as total minus the named phases so unmeasured time is
 * visible instead of lost. lba0/lba1 are the absolute sectors config_save()
 * would write; cfg_writable 0 renders the locator as "not writable".
 * log_hdr_lba/log_next_lba are the event log's header block and the block
 * its next flush would write (both 0 when the log is off) — the same
 * cross-check against tools/make_log.py --verify.
 */
void settings_diag_render(uint32_t total_ms, uint32_t lcd_ms, uint32_t disk_ms,
                          uint32_t lib_ms, uint32_t resume_ms,
                          uint32_t res_dir_ms, uint32_t res_open_ms,
                          uint32_t res_seek_ms,
                          uint32_t decode_us_kframe, uint32_t underruns,
                          int cfg_writable, uint32_t cfg_seq,
                          uint32_t lba0, uint32_t lba1,
                          uint32_t log_hdr_lba, uint32_t log_next_lba);

#endif /* CORE_UI_SETTINGS_H */
