/*
 * core/player/player.h — background streaming playback engine.
 *
 * Extracted verbatim from the old core/kernel/main.c god-file: the streaming
 * FLAC/MP3 decode path, the SPSC PCM ring feeding the DMA-driven DAC, the
 * fat32-backed byte source + read-ahead glue, the per-folder album art, and the
 * queue-based auto-advancing player. The UI (main.c) drives it through this
 * narrow API and never touches the decoder/ring statics directly.
 *
 * Behaviour is load-bearing and device-proven — do NOT change the decode /
 * stop / advance logic. In particular player_stop()/player_advance() must NOT
 * close the decoder mid-decode (that hard-freezes the core); the next open
 * reclaims its memory via the arena reset.
 */
#ifndef CORE_PLAYER_PLAYER_H
#define CORE_PLAYER_PLAYER_H

#include <stdint.h>
#include "../fs/fat32.h"
#include "../codecs/flac_meta.h"

/* Max entries in a browse listing. Bounds the UI's g_browse[] (one album's
 * worth of tracks). */
#define BROWSE_MAX 128
/* Max entries in the PLAYBACK queue — larger than a browse listing so
 * "Shuffle Songs" can hold the whole library, not just one album. */
#define QUEUE_MAX  6000
#define NAME_MAX   64                    /* stored display name (Nunito, ASCII)  */

/* One list row: a subdirectory or a playable file. Shared between the browser
 * (which fills it) and the player (which copies a folder's worth as its queue). */
typedef struct {
    char     name[NAME_MAX + 1];         /* display name (uppercased, font-safe) */
    uint32_t clus;
    uint32_t size;
    uint32_t art_clus;                   /* this track's cover (folder.art); 0 =  */
    uint32_t art_size;                   /*   use the queue-level art (album play) */
    uint8_t  fmt;                        /* 0 = FLAC, 1 = MP3 (only when !is_dir) */
    uint8_t  is_dir;                     /* 1 = subdirectory                     */
    /*
     * What Shuffle Albums groups and orders by. Filled by whoever builds the
     * row (kernel/main.c, at every queue builder); the player only reads them.
     *
     * album is a group id, not a name: the library's album index + 1 for a
     * library-built queue, 1 for a folder listing (one folder is one album),
     * 0 for "unknown", which makes the entry its own group so it can never be
     * glued to an unrelated album. order_key places the entry WITHIN its
     * album and is the index's (disc << 16) | track — the very key
     * browse_bind() sorts a tracklist by, so an album plays in the order the
     * tracklist shows it, filenames notwithstanding. 0xFFFFFFFF is "the index
     * does not know this file" and sorts last; ties fall back to queue order.
     */
    uint16_t album;
    uint32_t order_key;
} browse_entry_t;

/* FAT32 block callback: read absolute 512-byte LBAs off the disk, with a
 * spin-up retry (rides over the drive spinning down during a browse idle).
 * Lives with the player because it's the disk read path the streaming decoder
 * rides on; main.c also passes it to fat32_mount. */
int player_disk_read(void *ud, uint32_t lba, uint32_t count, void *buf);

/* Bind the player to the mounted volume; call once before play. */
void player_init(fat32_t *fs);

/* Launch playback: copy `entries` as the queue, load the folder's album art
 * (folder.art at art_clus/art_size) once, and start at `start_idx`. Replaces
 * any current playback. */
void player_play_queue(const browse_entry_t *entries, int n, int start_idx,
                       uint32_t art_clus, uint32_t art_size);

/* Build a large queue incrementally (used by Shuffle Songs to enqueue the whole
 * library without a huge caller-side staging array): begin (stops playback and
 * clears), add each entry, then commit to start at `start`. Each entry's
 * art_clus is used for its cover (per-track). */
void player_queue_begin(void);
void player_queue_add(const browse_entry_t *e);
void player_queue_commit(int start);

/* Decode one bounded chunk and auto-advance at end of track. Call every
 * main-loop pass so audio runs while the UI is elsewhere. */
void player_pump(void);

/* Stop playback (does NOT close the decoder — see header note). */
void player_stop(void);

/* Pause / resume the current track (suspends the DAC, holds decoder+position);
 * toggle picks the opposite. No-ops when nothing is loaded. */
void player_pause(void);
void player_resume(void);
void player_toggle_pause(void);
int  player_paused(void);              /* 1 while paused */

/*
 * Playback order/looping (driven by Settings).
 *
 * shuffle, one of PLAYER_SHUFFLE_*: OFF walks the queue; SONGS walks a seeded
 * PERMUTATION of the playable entries — every one once, then the queue ends
 * (or, under Repeat All, a new permutation is dealt); ALBUMS walks a seeded
 * permutation of ALBUM GROUPS (browse_entry_t.album), each group played
 * whole in (order_key, queue index) order, so the album you are on finishes
 * before another album starts.
 *
 * Any CHANGE of mode over a non-empty queue re-deals with the current track
 * pinned first — its whole album, under ALBUMS — so switching modes never
 * restarts or changes the song. Re-pushing the mode already in force is a
 * no-op: settings_apply() pushes this on every settings change, volume
 * included, and a re-deal there would silently reorder the album mid-listen.
 * The ids match ui/settings.h's shuffle_mode_t (main.c asserts it).
 *
 * repeat: 0 = off (stop at queue end), 1 = all (loop the queue), 2 = one
 * (replay the current track).
 */
#define PLAYER_SHUFFLE_OFF     0
#define PLAYER_SHUFFLE_SONGS   1
#define PLAYER_SHUFFLE_ALBUMS  2
void player_set_shuffle(int mode);
void player_set_repeat(int mode);

/*
 * The shuffle order is a pure function of (seed, keep) over the queue: the
 * LCG seed the deal started from, and the queue index that was pinned to the
 * front of it (the track playing when the deal happened; PLAYER_KEEP_NONE
 * when nothing was; PLAYER_KEEP_QUEUE when the order simply IS the queue
 * order — a queue that was enqueued already shuffled). Every deal records
 * both, so the pair can be saved across a power cut and dealt again with
 * player_reshuffle_with_seed(): same queue, same pair, same order — the
 * track after the one you left is still the track after it.
 *
 * player_order_seed() is 0 while no order has been dealt (empty queue, or
 * shuffle never turned on over it). player_reshuffle_with_seed() deals
 * immediately, whether or not shuffle is on (the order is simply unused
 * until it is), and is a no-op on an empty queue; a `keep` past the queue
 * end pins nothing.
 *
 * The MODE is the third input: the same pair under ALBUMS deals album groups
 * where under SONGS it deals tracks. Nothing stores it alongside the pair
 * because the record already carries the mode as a setting, and the boot path
 * pushes that before it deals (kernel/main.c: settings_apply, then
 * resume_restore).
 */
#define PLAYER_KEEP_NONE   (-1)
#define PLAYER_KEEP_QUEUE  (-2)
uint32_t player_order_seed(void);
int      player_order_keep(void);
void     player_reshuffle_with_seed(uint32_t seed, int keep);

/* 1 while a track is loaded (playing OR paused). This is the UI's notion:
 * every transport control, the Now Playing entry, the resume-position save
 * and the queue view are gated on it, and "paused" must keep all of those
 * alive. It is the WRONG gate for a power decision — see player_playing(). */
int  player_active(void);

/*
 * 1 while the DAC is actually running: a track is loaded AND not paused.
 *
 * This is what the power gates in the main loop want, and what they used to
 * get wrong. They keyed off player_active(), which stays 1 across a pause, so
 * a paused device was treated exactly like a playing one: the CPU never left
 * 80 MHz once the screen went dark, and the loop took the 200 us "keep the
 * DMA fed" halt instead of the 10 ms idle one — around 5,000 passes a second
 * of player_pump, timer reads, GPIO reads and event drains, feeding a DMA that
 * was stopped. A paused, screen-off iPod plausibly drew 2-3x true idle.
 *
 * Nothing about the transport keys off this; the UI keeps player_active().
 */
int  player_playing(void);

/*
 * How long a pause has to PERSIST before the player powers the codec down.
 *
 * hal_audio_stop() — the pause — only mutes and cuts the DMA; the WM8758's
 * PLL, VMID, DACs and headphone amps stay live and the I2S/MCLK clocks stay
 * ungated, which is most of the analog budget. Once a pause has lasted this
 * long the player suspends the codec (hal_audio_suspend) and wakes it again on
 * resume, re-latching volume/balance/tone, resuming from the exact position
 * and without a click.
 *
 * Why not immediately: the wake is a codec reset plus a ~40 ms VMID settle,
 * which a pause/unpause to answer a question should never pay. Why 5 s and not
 * 30: the cases split cleanly by duration. A pause that is about to be undone
 * is undone within a couple of seconds; anything longer is the device being
 * put down, and the first five seconds of a thirty-minute pause is a rounding
 * error against what the other twenty-nine minutes would have cost. Anything
 * much shorter risks the reset on a fumbled double-press.
 */
#define PLAYER_PAUSE_CODEC_OFF_US (5u * 1000000u)

/* Why the last open / auto-advance failed, so the UI can eventually say why a
 * track was skipped instead of silently scrolling past it. Valid after any
 * transport call and after player_pump() has ended a track. */
enum {
    PLAYER_OK        = 0,
    PLAYER_ERR_OPEN  = 1,   /* the file didn't parse as FLAC/MP3 (corrupt)   */
    PLAYER_ERR_RATE  = 2,   /* the file is fine; the DAC can't clock it      */
    PLAYER_ERR_READ  = 3,   /* a disk read failed mid-file (NOT end of track) */
};
int  player_last_error(void);

/*
 * Monotonic counter, bumped once per successful open (including the deferred
 * hand-over at the end of a track). The reliable "the audible track changed"
 * trigger: a queue-index compare misses repeat-one, a prev-restart, and a
 * single-track shuffle, all of which reopen the SAME index.
 */
uint32_t player_open_seq(void);

/*
 * Monotonic counter, bumped once each time the queue reaches its end and the
 * player goes idle BY ITSELF (auto-advance found no next track). NOT bumped by
 * player_stop(), a manual skip, or starting a new queue.
 *
 * The UI needs this to distinguish "the album finished" from "still playing":
 * a finished queue has to drop any saved resume position, or the next boot
 * restores a track the listener already heard to the end, cued at 0:00.
 */
uint32_t player_end_seq(void);

/*
 * Seek within the current track. player_seek_to takes an absolute position in
 * seconds (clamped to the track length); player_seek_seconds moves relative to
 * the current position. Both return 0 on success, -1 when nothing is playing,
 * the codec can't seek, or a track hand-over is in flight. The DAC is stopped
 * and re-primed across the seek but NOT re-initialised, so the codec's gain is
 * untouched; a paused player stays paused at the new position.
 */
int  player_seek_to(uint32_t sec);
int  player_seek_seconds(int delta);

/*
 * Real-time margin counters, cheap enough to leave permanently enabled (two
 * timer reads per ~23 ms of audio and one increment in the DMA ISR). Intended
 * for a debug screen: if decode_us_per_kframe approaches the real-time budget
 * (22676 us/kframe at 44.1 kHz) the codec is out of headroom.
 */
typedef struct {
    uint32_t decode_us_per_kframe;  /* CPU microseconds per 1000 frames decoded */
    uint32_t decode_rate;           /* the rate those frames play back at, Hz.  */
                                    /*   The real-time budget is 1e9/rate us    */
                                    /*   per 1000 frames — 22676 at 44.1 kHz,   */
                                    /*   20833 at 48 — so the microseconds are   */
                                    /*   meaningless without it. 0 = nothing     */
                                    /*   has been decoded yet.                   */
    uint32_t ring_low_frames;       /* PCM ring low-water since the last present */
    uint32_t underruns;             /* times the ISR found the ring short        */
    uint32_t arena_high_water;      /* peak decoder-arena bytes this session      */
    int      arena_oom;             /* an allocation didn't fit (track ends early) */
} player_stats_t;

const player_stats_t *player_stats(void);

/* Tags + duration of the current track (parsed at open; fields empty/0 when a
 * tag is absent — fall back to the filename). */
const flac_meta_t *player_meta(void);

/* Probe an arbitrary file's tags without touching playback (library scan). */
int player_probe_meta(uint32_t clus, uint32_t size, flac_meta_t *out);

/* Now-playing readouts (valid while active; elapsed/buf return 0 otherwise). */
const char *player_track_name(void);
uint32_t    player_elapsed_s(void);
uint32_t    player_total_s(void);
uint32_t    player_buf_pct(void);

/* Reset the ring low-water mark; the now-playing renderer calls this once per
 * present so the buffer-health readout tracks the last frame's low point. */
void player_note_presented(void);

/* The playback queue (the folder a track was launched from), for the queue view.
 * player_jump switches to entry `i` and plays it (no-op for folders/out-of-range). */
int         player_queue_len(void);
int         player_queue_current(void);
const char *player_queue_name(int i);
int         player_queue_is_dir(int i);
void        player_jump(int i);

/* Manual track skip (Prev/Next buttons). player_prev restarts the current track
 * if >~3s in, else goes to the previous, wrapping to the last entry.
 * player_next on the LAST track (Repeat off) ENDS playback deliberately — the
 * queue is finished, exactly as if it had played out, and player_end_seq()
 * counts it as such. It was previously a no-op there, which was an
 * overcorrection: the original bug was tearing the transport down BEFORE
 * establishing there was no successor, leaving the player inactive with every
 * UI transport gated on player_active(). Picking the successor first is what
 * fixed that; doing nothing merely made the button dead.
 *
 * Both ignore Repeat-One, and both leave a paused player paused at the new
 * track. */
void        player_next(void);
void        player_prev(void);

/* Album art accessors for the now-playing renderer (the PLAYING folder's art,
 * held across browsing elsewhere). player_art_pixels() is RGB565, w*h. */
int             player_art_ok(void);
int             player_art_w(void);
int             player_art_h(void);
const uint16_t *player_art_pixels(void);

/* Bumped every time the loaded art changes, INCLUDING when it is cleared for a
 * track whose album has no cover. Use it as the cache key for any downscaled
 * copy: the queue index doesn't change on repeat-one, and a fresh queue can
 * start at the same index as the old one. */
uint32_t        player_art_seq(void);

#endif /* CORE_PLAYER_PLAYER_H */
