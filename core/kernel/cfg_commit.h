/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/cfg_commit.h — the settings-save COMMIT GATE: when a pending
 * change to g_settings is allowed to become a disk write, and what happens
 * to it when the write fails.
 *
 * EXTRACTED OUT OF kernel/main.c's settings_commit(), where it was a static
 * function reading the clock, the drive, the player and the battery directly
 * and therefore uncompilable on the host. Every input it needs is handed in
 * (cfg_commit_env_t), so the decision is pure — state in, verdict out — and
 * main.c keeps only the thin wrapper that gathers the inputs, wakes the
 * drive when told to, calls config_save() and reports the result back. The
 * precedent is ui/wheel.c and library/idx.c.
 *
 * WHY THE GATE EXISTS. config_save() was the ONLY thing in the firmware
 * that wrote to the user's disk, and this is its only caller, so these few
 * decisions are the whole write policy. (The event log, kernel/evlog.c,
 * has since become the second writer — and goes through THIS gate with its
 * own cfg_commit_t, plus a stricter idle rule: it never wakes a parked
 * drive.)
 *
 *   - DEBOUNCE: a change is written CFG_SAVE_DEBOUNCE_US after the LAST
 *     change, not on every wheel tick — one write per settling.
 *   - PARKED DRIVE UNDER A LIVE PLAYER: an idle commit does not spin the
 *     platters up for 1 KB while audio plays out of the anti-skip buffer;
 *     the change rides out on the next refill, on playback stopping, or on
 *     the forced commit at suspend/power-off.
 *   - BATTERY: below the disk-safe line nothing is written (a cut mid-sector
 *     tears a record) — except the ONE flush the low-battery policy makes at
 *     the DISKSAFE edge, which is exempt. See the modes below.
 *   - WAKE BEFORE WRITE: a write into a PARKED drive triggers a spin-up
 *     inside ata_write_raw's DRQ budget; on overrun ata_recover soft-resets
 *     the channel mid-spin-up, the save returns -2, and the change — the
 *     resume position captured a line earlier included — was gone, because
 *     the dirty flag had been cleared BEFORE the write. The forced commits
 *     at suspend and power-off, which skip the parked check by design, hit
 *     exactly this. The gate now says WRITE_WAKE when the drive is parked,
 *     so the caller pre-pays the spin-up through ata_wakeup() (the read
 *     path, built for the multi-second wait) before a byte is written.
 *   - RETRY ON FAILURE: the change stays pending when the write fails, and
 *     the next attempt is another debounce away, so the main loop retries
 *     without hammering a drive that is already unhappy. After
 *     CFG_SAVE_MAX_FAILURES consecutive failures it is dropped: a drive that
 *     refuses that many times in a row is not going to take it, and the
 *     previous good record is still intact on disk.
 *
 * settings_commit() MODES (the `mode` argument):
 *
 *   CFG_COMMIT_IDLE   the main loop: debounced, deferred while the drive is
 *                     parked under a live player, refused below the
 *                     disk-safe battery line.
 *   CFG_COMMIT_FORCE  now (suspend, power-off): no debounce, no parked
 *                     check — still refused below the disk-safe line.
 *   CFG_COMMIT_LAST   the ONE write the low-battery policy makes at the
 *                     DISKSAFE edge, exempt from the battery gate.
 *
 * The exemption is not optional. battery_policy_feed() latches DISKSAFE
 * BEFORE it returns the edge, so by the time battery_refresh() acts on that
 * edge battery_disk_writes_allowed() is already 0 — and a gated commit there
 * refuses the very flush the policy exists to make while the cell still has
 * the energy for it. That is exactly what happened when the flush and the
 * gate landed as two separate changes, each written without the other: the
 * DISKSAFE handler called settings_commit(1), the gate turned it away, and a
 * low-battery shutdown persisted nothing at all — no resume position, no
 * pending setting — while both commit messages described a flush that
 * never ran.
 */
#ifndef CORE_KERNEL_CFG_COMMIT_H
#define CORE_KERNEL_CFG_COMMIT_H

#include <stdint.h>

#define CFG_COMMIT_IDLE   0
#define CFG_COMMIT_FORCE  1
#define CFG_COMMIT_LAST   2

#define CFG_SAVE_DEBOUNCE_US   3000000u   /* 3 s after the last change        */
#define CFG_SAVE_MAX_FAILURES  3u         /* consecutive failed writes before
                                           * a pending change is given up on  */

/* The pending-change state. Zero-initialised = nothing pending. */
typedef struct {
    int      dirty;           /* a change is pending a write                 */
    uint32_t dirty_us;        /* when it was made — or when the last failed
                               * write was, so a retry waits a debounce too  */
    int      deferred_logged; /* the battery refusal has been reported once  */
    uint32_t failures;        /* consecutive failed writes of this change    */
} cfg_commit_t;

/* What the gate needs to know about the world, gathered by the caller. */
typedef struct {
    uint32_t now_us;          /* the microsecond clock (wraps)               */
    int      parked;          /* ata_is_parked()                             */
    int      player_active;   /* player_active()                             */
    int      battery_ok;      /* battery_disk_writes_allowed()               */
    int      writable;        /* config_writable()                           */
} cfg_commit_env_t;

/* The gate's verdicts. */
enum {
    CFG_GATE_NOTHING = 0,     /* nothing pending, or not yet: the debounce is
                               * still running, or the drive is parked under
                               * a live player (idle mode only)              */
    CFG_GATE_DEFER_LOG,       /* refused by the battery gate: the change stays
                               * pending; LOG this (first refusal)           */
    CFG_GATE_DEFER_QUIET,     /* refused by the battery gate, already logged */
    CFG_GATE_WRITE,           /* write now                                   */
    CFG_GATE_WRITE_WAKE,      /* write now — the drive is parked: ata_wakeup()
                               * FIRST, then write                           */
};

/* A change was made to the settings at `now_us`. */
void cfg_commit_touch(cfg_commit_t *c, uint32_t now_us);

/* Nothing is pending (loading the saved record is not a change). */
void cfg_commit_clear(cfg_commit_t *c);

/*
 * Decide. Returns a CFG_GATE_* verdict. The pending flag is NOT cleared by
 * a WRITE verdict — only by cfg_commit_result() reporting success — so a
 * write that fails leaves the change pending. It IS cleared when there is
 * no file to write to (env->writable == 0): nothing can ever persist it.
 */
int cfg_commit_gate(cfg_commit_t *c, int mode, const cfg_commit_env_t *env);

/*
 * The result of the write the gate allowed. rc == 0: the change is on disk,
 * nothing pending. rc < 0: the change stays pending for another attempt one
 * debounce from `now_us`, unless this was the CFG_SAVE_MAX_FAILURES'th
 * consecutive failure, in which case it is dropped. Returns 1 if the change
 * is still pending, 0 if not.
 */
int cfg_commit_result(cfg_commit_t *c, int rc, uint32_t now_us);

#endif /* CORE_KERNEL_CFG_COMMIT_H */
