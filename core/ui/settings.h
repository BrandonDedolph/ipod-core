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
 * (player), volume + volume_limit (mirrors hal_volume), eq / bass / treble /
 * balance (the codec's 5-band EQ and output gains), backlight_secs +
 * backlight_bright (backlight HAL), resume_on_startup (kernel/main.c re-opens
 * the saved track at boot), sleep_timer_min (kernel/main.c arms
 * ui/sleeptimer.c from it, and sleeps the device when it runs out). Crossfade
 * is the one field left that nothing consumes: it is still carried in the
 * record, but no row shows it and there is no crossfade mixer to drive.
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
 * Shuffle policy (FUNCTIONAL — the player's playback order).
 *
 * SONGS is a seeded permutation of the queue's tracks. ALBUMS keeps each
 * album whole: the album you are on plays out in its tracklist order, then
 * another album from the same queue at random, until every album in it has
 * played once. The ids ARE the byte stored at payload offset 0 (kernel/
 * config.c), so they must never be renumbered: a build that only knows
 * OFF/SONGS reads a 2 as SONGS, which is the right downgrade, and this build
 * reads anything it does not know as OFF.
 */
typedef enum {
    SHUFFLE_OFF    = 0,
    SHUFFLE_SONGS  = 1,
    SHUFFLE_ALBUMS = 2
} shuffle_mode_t;

/*
 * The whole persisted settings state. Ranges + which fields are live are noted
 * per field; keep this in sync with settings_defaults() and the clamps in
 * settings_adjust().
 */
typedef struct {
    shuffle_mode_t shuffle;  /* SHUFFLE_* — FUNCTIONAL (player queue order)    */
    repeat_mode_t repeat;    /* FUNCTIONAL (player auto-advance)               */
    int  resume_on_startup;  /* 0/1 — FUNCTIONAL (boot re-opens the last track)*/
    int  crossfade;          /* 0/1 — COSMETIC (no crossfade mixer yet)       */
    int  volume;             /* 0..100 — FUNCTIONAL (mirrors hal_volume)      */
    /*
     * VOLUME LIMIT — the ceiling the Volume slider and the Now Playing wheel
     * are held to, 10..100 with 100 meaning "no limit". The floor is 10, not
     * 0: a limit of 0 would be a mute switch with no obvious way back.
     * settings_volume_clamp() is the ONE place the rule is applied; nothing
     * else may re-derive it, or the two wheels would disagree.
     */
    int  volume_limit;       /* 10..100 — FUNCTIONAL (caps volume)            */
    /*
     * EQ PRESET — 0 (EQ_OFF) or one of ui/eq.c's named curves. While a preset
     * is selected it owns BOTH codec shelves, so bass/treble below are held
     * (the rows render locked, showing the preset's shelf gains) and the
     * user's own tone comes back untouched the moment EQ returns to Off.
     *
     * "Selected" means an id THIS BUILD HAS A CURVE FOR: a value from a newer
     * build reads as Off in eq_effective_curve(), settings_row_locked() and
     * config_decode() alike, so the shelves can never be locked over a curve
     * the rows would then report as the user's own tone.
     */
    int  eq;                 /* 0..EQ_PRESET_COUNT-1 — FUNCTIONAL (ui/eq.c)   */
    int  bass, treble;       /* -12..12 dB — FUNCTIONAL (codec shelving EQ,   */
                             /* ignored while eq names a preset this build    */
                             /* knows; an id it does not reads as Off)        */
    int  balance;            /* -100..100 — FUNCTIONAL (pans the OUT1 gains)  */
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
     * other kind — RESUME_KIND_OTG included, because the live list is not a
     * file and has no name to fold. All meaningless while resume_kind is RESUME_KIND_NONE, and
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

    /*
     * SLEEP TIMER — runtime state too, and the only field here that is NEVER
     * WRITTEN TO DISK. It rides along in settings_t for the same reason the
     * resume locator does (this is the record the Settings model is pure
     * over, so the row's value has to live in it), but kernel/config.c
     * deliberately neither encodes it nor restores it: config_decode() zeroes
     * it, so it reads Off after every boot.
     *
     * Why not persist it. A countdown is relative to the moment it was armed
     * and a boot means the device was off, so a stored countdown means
     * nothing; only the last-picked duration could persist, and that would
     * make arming — which happens right before sleep, with the drive parked —
     * a disk write for one saved SELECT a night. Apple's own timer resets to
     * Off after it fires.
     *
     * The countdown itself is NOT here: ui/sleeptimer.c owns it, and
     * kernel/main.c keeps `sleeptimer_total_min(&g_sleep) == sleep_timer_min`
     * at every loop top. This side is only what the row shows.
     */
    int  sleep_timer_min;    /* 0 (off) / 15 / 30 / 60 / 90 / 120 minutes —
                              * RUNTIME ONLY, never persisted                 */

    /*
     * CLOCK. Two user preferences and four numbers that are state, not
     * preference — the same arrangement the resume locator has, and for the
     * same reason: kernel/config.c's record is the only thing on this device
     * that survives a power cut, so everything that has to persist rides in
     * settings_t whether or not a row shows it.
     *
     * time_24h / time_in_title ARE rows (Settings > Date & Time). A Reset
     * returns THREE of the six to their defaults — those two and
     * applied_epoch, the mark, so the next boot looks at the stamp again:
     * settings_defaults() zeroes all six (it is also the pre-load state, where
     * a stamp nobody wrote would be an invention), and kernel/main.c's Reset
     * puts host_epoch, host_off_min and utc_off_min back, because those three
     * are facts about the world rather than preferences of the user's.
     *
     * utc_off_min is the display offset: the RTC holds UTC and local time is
     * RTC + utc_off_min minutes. A device that has never met the host app has
     * offset 0 and runs "local as UTC" — internally consistent, and it
     * displays whatever the user set by hand.
     *
     * host_epoch / host_off_min are written by the HOST (core sync / eject /
     * install stamp them into the record; the firmware never changes them and
     * carries them through every save unchanged, which is what makes
     * "applied == host" a stable comparison). applied_epoch is the firmware's
     * mark: the host_epoch it last acted on. kernel/timesync.h owns the rules.
     */
    int  time_24h;           /* 0/1 — a row: 12-hour vs 24-hour clock         */
    int  time_in_title;      /* 0/1 — a row: show the clock on the strip      */
    int  utc_off_min;        /* -720..840, the device's display offset        */
    uint32_t host_epoch;     /* the host's stamp; 0 = never stamped           */
    int  host_off_min;       /* the host's UTC offset when it stamped         */
    uint32_t applied_epoch;  /* the stamp we acted on; 0 = none yet           */
} settings_t;

/* time_24h / time_in_title share one byte on disk (kernel/config.c). The bit
 * numbers are part of the record format: append, never renumber. */
#define TIME_FLAG_24H      0x01u
#define TIME_FLAG_IN_TITLE 0x02u
#define TIME_FLAGS_MASK    0x03u

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
    RESUME_KIND_OTG      = 7,  /* the On-The-Go live list (library/otg.h): it
                                * is not a file and has no name, so ctx is 0
                                * and the boot path rebuilds the queue from
                                * the list COREOTG.DAT restored. A SAVED
                                * On-The-Go playlist is an ordinary file and
                                * resumes as KIND_PLAYLIST                    */
    RESUME_KIND_MAX      = RESUME_KIND_OTG
};

/* Populate `s` with sensible defaults (shuffle off, repeat off, volume 70 with
 * no volume limit, EQ off, backlight 15 s at full brightness, Linen theme). */
void settings_defaults(settings_t *s);

/*
 * The Settings screens. main.c keeps the current screen + a selection index and
 * calls the generic accessors below against them. SETTINGS_SCREEN_COUNT is the
 * count sentinel, not a real screen.
 */
typedef enum {
    SETTINGS_ROOT,       /* the top Settings menu                             */
    SETTINGS_PLAYBACK,   /* Shuffle / Repeat / Resume / Sleep Timer selects   */
    SETTINGS_SOUND,      /* Volume / Volume Limit / EQ / Bass / Treble / Bal  */
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
    /*
     * Date & Time: three rows (Set Date & Time / Time Format / Time in Title)
     * and, behind the first of them, the field editor — which is not a list at
     * all (ui/settime.h owns its model and ui/screen_settings.c paints it), so
     * it reports one non-interactive row here and main.c drives it.
     * Appended after DIAG so no existing screen id moves.
     */
    SETTINGS_DATETIME,
    SETTINGS_SETTIME,
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
    SETTINGS_ACTION_NOOP,
    /*
     * "The record changed, but only its RUNTIME part — apply it, do not
     * persist it." Today that is exactly Sleep Timer: sleep_timer_min moved,
     * so NOOP would be a lie (main.c must re-arm ui/sleeptimer.c from it),
     * but nothing on disk stores it, so NONE would make main.c touch the
     * config and write a byte-identical record three seconds later — the
     * spurious write NOOP exists to prevent. Also appended last.
     */
    SETTINGS_ACTION_SLEEPTIMER,
    /* Appended after NOOP for the same reason SLEEPTIMER was: main.c switches
     * on the names and nothing on disk stores them, so the only thing
     * renumbering would achieve is a diff. */
    SETTINGS_ENTER_DATETIME,
    SETTINGS_ENTER_SETTIME
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

/*
 * The SCREEN's own title — what ui_header draws at the top of it.
 *
 * Model data rather than a switch in the renderer, because that switch had no
 * case for Date & Time and fell through to "Settings" while the committed
 * gallery still said "Date & Time". A title that lives here is one the host
 * suite can assert for every screen, so the code and the still cannot say
 * different things again. A screen with no title of its own answers
 * "Settings"; an out-of-range screen answers "" (there is nothing to draw).
 *
 * EVERY painter reads it, the two dashboards (About, Boot Details) included:
 * they draw their own bodies but not their own headers, or the assertions in
 * the host suite would be pinning a string nothing on screen uses.
 */
const char *settings_title(int screen);

/* The display name of theme id `theme` ("Linen", "Onyx", "Sage", "Plaster",
 * "Olive", "Umber", "Mushroom" — ui/palette.h THEME_* order). An id outside
 * that range names "Linen", which is also what palette.c renders for it. */
const char *settings_theme_name(int theme);

/* The display name of clicker profile `profile` ("Off"/"Tick"/"Click"/"Pop"). */
const char *settings_clicker_name(int profile);

/* The display name of EQ preset `preset` ("Off", "Rock", …; ui/eq.c). A
 * preset id outside the table names "Off", which is also what config.c
 * decodes such a byte to. */
const char *settings_eq_name(int preset);

/*
 * Apply the Volume Limit to a candidate volume: the value `v` clamped to
 * [0, s->volume_limit]. THE single statement of the rule — the Volume slider
 * (settings_adjust) and the Now Playing wheel (kernel/main.c) both go through
 * here, so the ceiling cannot come out different on the two screens. A
 * volume_limit outside its own 10..100 range is clamped first, so a hand-
 * edited record cannot pin the user at 0.
 */
int settings_volume_clamp(const settings_t *s, int v);

/*
 * THE CLOCK, INJECTED. Settings > Date & Time's first row shows the time it
 * would edit ("10:42 AM", "22:42", or "Not set"), and this module is pure — it
 * has no RTC, no software clock and no idea what time it is. main.c hands the
 * current LOCAL epoch in once per Settings paint, the way ui/chrome.c is handed
 * a marquee clock through ui_set_scroll_text.
 *
 * `valid` 0 (or an epoch outside 2001..2099) makes the row read "Not set",
 * which is exactly what a device with a drained cell and no host stamp shows.
 */
void settings_set_now(int valid, uint32_t local_epoch);

/*
 * Is row (screen, idx) present but not adjustable right now? True only for
 * Sound's Bass and Treble while an EQ preset is selected: the codec has one
 * low shelf and one high shelf and the preset owns both, so those rows report
 * the PRESET's shelf gains, render greyed (ui/screen_settings.c) and refuse
 * the wheel (settings_adjust returns 0) until EQ goes back to Off. The
 * stored bass/treble are never touched by any of this.
 */
int settings_row_locked(int screen, const settings_t *s, int idx);

/* The settings_kind_t of row (screen, idx). */
int settings_kind(int screen, int idx);

/* The buffer every caller of settings_value() must provide. Named because the
 * Date & Time row formats a clock into it through kernel/datetime.c, which
 * takes a size. */
#define SETTINGS_VALUE_MAX 24

/*
 * Fill in the render-facing value of row (screen, idx):
 *   buf         (>= SETTINGS_VALUE_MAX bytes) receives the display text for
 *               SELECT/SLIDER/
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
 * Returns 1 if *s changed, 0 if not — at a rail, on a locked row
 * (settings_row_locked), on a non-adjustable row, or for delta 0 — so the
 * caller can skip the persist (see settings_action_t).
 *
 * Two Sound rows are coupled: Volume is clamped to the limit through
 * settings_volume_clamp(), and lowering Volume Limit below the current volume
 * pulls the volume down in the same call. The invariant both halves keep is
 * that *s never leaves here with volume > volume_limit.
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
 * THE HEADPHONE-JACK PROBE, ON SCREEN.
 *
 * The detect line (GPIO A7) is documented but its polarity has never been
 * observed on this device, and the probe that would settle it prints to the
 * SER0 UART — a cable this device does not have and is not getting. So the
 * probe is here instead: the About footer draws the live pin next to the ADC
 * and LOG tokens, in EVERY build, trusted or not, because reading it is one
 * 32-bit register read. Plug, unplug, read the digit; one flash, no cable.
 * The bench procedure is core/docs/hw/10-headphone-jack.md.
 *
 * `pin_cfg` mirrors HEADPHONE_PIN_* from hal/hw/headphone.h, which this
 * host-built file must not include; kernel/main.c passes headphone_pin_cfg()
 * straight through and _Static_asserts the two sets agree.
 */
#define ABOUT_JACK_PIN_ENABLED 0x1  /* A7 is a GPIO (not its alternate fn)  */
#define ABOUT_JACK_PIN_OUTPUT  0x2  /* A7 is driven as an output            */

typedef struct {
    int8_t   raw;        /* headphone_raw(): 0 absent, 1 seated             */
    int8_t   debounced;  /* hal_headphones_present(): -1 untrusted, 0, 1    */
    uint8_t  pin_cfg;    /* ABOUT_JACK_PIN_* of the detect pin              */
    uint16_t edges;      /* raw transitions since boot (jackwatch raw_edges)*/
} about_jack_t;

/*
 * Render the About screen from live values (main.c owns these — do not
 * fabricate). free/total are whole megabytes; pct<0 or mv<=0 render as "--".
 * log_seq / log_state describe the event log (kernel/evlog.c): the footer
 * reads "LOG <seq> on", "LOG off" or "LOG <seq> err". lib_truncated != 0
 * draws the "library too large" warning (the counts hit a LIB_MAX_* cap).
 *
 * `version` is the release tag ("v0.1.0"), drawn in the firmware chip as
 * "Core v0.1.0". It is a PARAMETER rather than a #include because this file
 * is host-built (the settings unit test links it) and CORE_VERSION lives in a
 * meson-generated header that only the firmware build produces; main.c passes
 * CORE_VERSION, everything else passes "v0.0.0". NULL or "" renders the bare
 * "Core" chip. Long strings are fine — the chip is sized from text_width.
 *
 * `jack` adds the live jack token to that footer (above); NULL leaves the
 * footer exactly as it was, which is what the placeholder path and anything
 * without a GPIO to read pass.
 */
void settings_about_render(int battery_pct, int battery_mv, int battery_raw,
                           uint32_t total_mb, uint32_t free_mb,
                           int n_songs, int n_albums, int n_artists,
                           uint32_t log_seq, int log_state, int lib_truncated,
                           const char *version, const about_jack_t *jack);

/*
 * Render the Boot Details screen (SETTINGS_DIAG). All times are milliseconds
 * as MEASURED — a 0 renders as "--" rather than a fake zero, and the screen
 * derives "OTHER" as total minus the named phases so unmeasured time is
 * visible instead of lost. lba0/lba1 are the absolute sectors config_save()
 * would write; cfg_writable 0 renders the locator as "not writable".
 * log_hdr_lba/log_next_lba are the event log's header block and the block
 * its next flush would write (both 0 when the log is off) — the same
 * cross-check against tools/make_log.py --verify.
 *
 * decode_rate is the sample rate the decode statistics were measured at; the
 * DECODE percentage is against 1e9/decode_rate microseconds per 1000 frames,
 * which is what real time means for THAT stream. 0 falls back to 44.1 kHz's
 * budget, which is what the readout shows before anything has played.
 *
 * `build_id` is the full build stamp ("v0.1.0-3-g1234567-dirty"), drawn in
 * the header's right-hand slot — the one place on this page with a free text
 * row, since every pixel between the phase bar and the LOG line is spoken
 * for. A parameter for the same reason as settings_about_render's `version`:
 * this file is host-built and cannot see the generated header. NULL or ""
 * draws the plain header.
 */
void settings_diag_render(uint32_t total_ms, uint32_t lcd_ms, uint32_t disk_ms,
                          uint32_t lib_ms, uint32_t resume_ms,
                          uint32_t res_dir_ms, uint32_t res_open_ms,
                          uint32_t res_seek_ms,
                          uint32_t decode_us_kframe, uint32_t decode_rate,
                          uint32_t underruns,
                          int cfg_writable, uint32_t cfg_seq,
                          uint32_t lba0, uint32_t lba1,
                          uint32_t log_hdr_lba, uint32_t log_next_lba,
                          const char *build_id);

#endif /* CORE_UI_SETTINGS_H */
