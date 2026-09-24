/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/player/player_test_stubs.c — the world around core/player/player.c.
 *
 * player.c is the queue / auto-advance / repeat / shuffle brain of the
 * firmware and had no test at all. It is testable, though: everything it
 * depends on is reached through a header-declared function, so replacing the
 * disk, the codec and the DAC with controllable fakes puts the real player.c
 * under test unmodified — no #ifdefs in the driver, no copy of the logic.
 *
 * What is faked, and why that is honest:
 *   fat32_*        the queue logic never inspects file bytes; it only needs
 *                  "this cluster opened" or "this cluster did not".
 *   flac/mp3 open  the ONE behaviour the queue logic branches on. The stub
 *                  fails for clusters the test marks broken, which is how
 *                  "skip a track that won't open" gets exercised.
 *   decode         a generator that produces a fixed number of frames and
 *                  then reports end-of-stream, so auto-advance can be driven
 *                  deterministically instead of by decoding real audio.
 *   hal_audio_*    records start/stop so pause/resume and stop-before-open
 *                  ordering can be asserted.
 *   ata_*          the drive spin-down calls, counted.
 *   mmio_read32    served by the recording mock bus, so USEC_TIMER (and hence
 *                  the elapsed-seconds clock that player_prev branches on) is
 *                  under the test's control.
 *
 * pcm_ring.c and the arena are NOT faked — the real ones are linked, because
 * the player's interaction with the ring (prime, drain, end-of-stream) is part
 * of what is being tested.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fat32.h"
#include "decoder.h"
#include "flac.h"
#include "mp3.h"
#include "id3_meta.h"
#include "flac_meta.h"
#include "readahead.h"
#include "diskbuf.h"
#include "arena.h"
#include "ata.h"
#include "hal.h"
#include "timer.h"

#include "player_test_stubs.h"

/* ---- controllable state ---------------------------------------------- */

#define MAX_BROKEN 16
static uint32_t g_broken[MAX_BROKEN];
static int      g_broken_n;

static uint32_t g_last_open_clus;
int  stub_opens;          /* successful decoder opens                     */
int  stub_open_attempts;  /* opens attempted, including the failing ones  */
int  stub_closes;
int  stub_audio_starts;
int  stub_audio_stops;
int  stub_audio_running;
int  stub_audio_primed;   /* HAL still holds unplayed PCM (survives a stop)  */
int  stub_audio_flushes;
int  stub_audio_cold;     /* codec powered down (suspend/close), not by stop */
int  stub_audio_suspends;
int  stub_audio_wakes;
int  stub_audio_suspends_while_running;
int  stub_audio_inits_while_running;
int  stub_audio_drains;
int  stub_audio_drained_while_running;  /* drains issued BEFORE the stop      */
int  stub_ata_standbys;
int  stub_ata_wakeups;
int  stub_disk_pumps;
static int g_ata_parked;
int  stub_ata_reads;
static int g_ata_read_ok = 1;  /* stub_set_ata_read_ok(): make reads fail */
int  stub_meta_reads;
int  stub_seeks;
uint64_t stub_last_seek_frame;
static int g_seek_ok = 1;   /* stub_set_seek_ok(): force the decoder to refuse */
static uint64_t g_seek_max = ~(uint64_t)0;  /* stub_set_seek_max(): fail past this */
static int g_total_unknown; /* stub_set_total_unknown(): open reports no length */

static uint32_t g_disk_ahead = 64u * 1024u * 1024u;

/*
 * The DAC's position, as the player reads it back through
 * hal_audio_frames_played(). The real HAL holds pulled-but-unheard PCM
 * between the ring and the converter — two 8192-frame ping-pong buffers on
 * the device, one 1024-frame SDL buffer on the sim — and reports what is
 * past it. Modelled as a pipe of g_dac_depth frames: a pull fills it, and
 * only what overflows it has been heard. Silence counts (the DAC clocks a
 * padded buffer like any other, so an empty-ring pull still advances it), a
 * flush empties the pipe unheard, a drain plays it out, and hal_audio_init
 * restarts the count at g_played_origin — a test can put that near the
 * uint32 wrap. The depth defaults to the device's so the existing scenarios
 * describe the device; a test that wants the sim's, or the FIFO's, sets it.
 */
static uint32_t g_dac_depth = 2u * 8192u;
static uint32_t g_dac_pipe;        /* frames pulled and not yet heard */
static uint32_t g_dac_played;      /* the count the HAL reports        */
static uint32_t g_played_origin;   /* what init restarts it at         */
static uint32_t g_stub_rate = 44100u;  /* stub_set_rate(): the decoder's, and accepted by init */
int  stub_audio_inits;
uint32_t stub_last_init_rate;

void stub_set_seek_ok(int ok)          { g_seek_ok = ok ? 1 : 0; }
void stub_set_seek_max(uint64_t m)     { g_seek_max = m; }
void stub_set_total_unknown(int u)     { g_total_unknown = u ? 1 : 0; }
void stub_set_ata_read_ok(int ok)      { g_ata_read_ok = ok ? 1 : 0; }
void stub_set_disk_ahead(uint32_t b)   { g_disk_ahead = b; }
void stub_set_dac_depth(uint32_t f)    { g_dac_depth = f; }
void stub_set_played_origin(uint32_t f){ g_played_origin = f; }
void stub_set_rate(uint32_t hz)        { g_stub_rate = hz; }

/* The DAC took `frames` from the source (real PCM or padding alike). */
static void dac_pull(uint32_t frames)
{
    g_dac_pipe += frames;
    if (g_dac_pipe > g_dac_depth) {
        g_dac_played += g_dac_pipe - g_dac_depth;
        g_dac_pipe    = g_dac_depth;
    }
}

/* Frames the fake decoder produces before reporting end-of-stream. Small, so a
 * track ends after a bounded number of player_pump() calls. */
static uint32_t g_track_frames = 8192;
static uint32_t g_frames_left;

void stub_reset(void)
{
    g_broken_n = 0;
    g_last_open_clus = 0;
    stub_opens = stub_open_attempts = stub_closes = 0;
    stub_audio_starts = stub_audio_stops = stub_audio_running = 0;
    stub_audio_primed = stub_audio_flushes = stub_audio_drains = 0;
    stub_audio_drained_while_running = 0;
    stub_audio_cold = stub_audio_suspends = stub_audio_wakes = 0;
    stub_audio_suspends_while_running = 0;
    stub_audio_inits_while_running = 0;
    stub_ata_standbys = stub_ata_reads = stub_meta_reads = 0;
    stub_ata_wakeups = stub_disk_pumps = 0;
    g_ata_parked = 0;
    stub_seeks = 0;
    stub_last_seek_frame = 0;
    g_seek_ok  = 1;
    g_seek_max = ~(uint64_t)0;
    g_total_unknown = 0;
    g_ata_read_ok = 1;
    g_disk_ahead  = 64u * 1024u * 1024u;
    g_frames_left = 0;
    g_dac_depth   = 2u * 8192u;
    g_dac_pipe    = 0;
    g_dac_played  = 0;
    g_played_origin = 0;
    g_stub_rate   = 44100u;
    stub_audio_inits = 0;
    stub_last_init_rate = 0;
}

void stub_break_cluster(uint32_t clus)
{
    if (g_broken_n < MAX_BROKEN) {
        g_broken[g_broken_n++] = clus;
    }
}

void stub_set_track_frames(uint32_t frames)
{
    g_track_frames = frames;
}

uint32_t stub_last_open_clus(void)
{
    return g_last_open_clus;
}

static int cluster_is_broken(uint32_t clus)
{
    for (int i = 0; i < g_broken_n; i++) {
        if (g_broken[i] == clus) {
            return 1;
        }
    }
    return 0;
}

/* ---- fake FAT32 ------------------------------------------------------- */

void fat32_stream_open(fat32_stream_t *st, fat32_t *fs,
                       uint32_t first_clus, uint32_t size)
{
    st->fs        = fs;
    st->clus      = first_clus;
    st->clus_off  = 0;
    st->remaining = size;
    /* The player opens the byte source immediately before asking the codec to
     * open it, so this is a reliable record of "which file is being opened". */
    g_last_open_clus = first_clus;
}

int32_t fat32_stream_read(fat32_stream_t *st, void *buf, uint32_t len)
{
    uint32_t n = (st->remaining < len) ? st->remaining : len;
    memset(buf, 0, n);
    st->remaining -= n;
    st->clus_off  += n;
    return (int32_t)n;
}

uint32_t fat32_stream_skip(fat32_stream_t *st, uint32_t n)
{
    uint32_t got = (st->remaining < n) ? st->remaining : n;
    st->remaining -= got;
    st->clus_off  += got;
    return got;
}

int32_t fat32_read_file(fat32_t *fs, uint32_t first_clus,
                        void *buf, uint32_t maxlen)
{
    (void)fs;
    (void)first_clus;
    memset(buf, 0, maxlen);
    return (int32_t)maxlen;
}

/* ---- fake byte-source plumbing (transparent pass-through) ------------- */

void readahead_init(readahead_t *ra, decoder_source_t *inner,
                    uint8_t *buf, uint32_t cap)
{
    (void)buf;
    (void)cap;
    memset(ra, 0, sizeof *ra);
    ra->inner = inner;
}

void readahead_as_source(readahead_t *ra, decoder_source_t *out)
{
    *out = *ra->inner;
}

void diskbuf_init(diskbuf_t *db, decoder_source_t *inner, uint8_t *buf,
                  uint32_t cap, uint32_t low, uint32_t high)
{
    (void)buf;
    (void)cap;
    memset(db, 0, sizeof *db);
    db->inner = inner;
    /* The watermarks are kept: the player reads them back to predict whether
     * a pump pass will touch the drive, and a zeroed pair would make every
     * pass look idle. */
    db->low   = low;
    db->high  = high;
}

void diskbuf_as_source(diskbuf_t *db, decoder_source_t *out)
{
    *out = *db->inner;
}

uint32_t diskbuf_pump(diskbuf_t *db, uint32_t chunk)
{
    (void)db;
    (void)chunk;
    stub_disk_pumps++;    /* observable: the player asked for a burst */
    return 0;             /* always topped up: the pump has nothing to do */
}

uint32_t diskbuf_fill_ahead(const diskbuf_t *db)
{
    (void)db;
    /* By default comfortably above the player's DISK_LOW watermark, so the
     * "topped up and idle -> park the drive" branch is the one that runs. */
    return g_disk_ahead;
}

/* No disk error: the player distinguishes a persistent read failure from a
 * clean end-of-track, and the queue tests exercise the end-of-track side. */
int diskbuf_error(const diskbuf_t *db)
{
    (void)db;
    return 0;
}

/* ---- fake metadata ---------------------------------------------------- */

int flac_meta_read(decoder_source_t *src, flac_meta_t *out)
{
    (void)src;
    memset(out, 0, sizeof *out);
    out->have        = 1;
    out->sample_rate = 44100;
    out->duration_s  = 123;
    stub_meta_reads++;
    return 0;
}

/* The MP3 side of the same seam. player.c only reaches it when flac_meta_read
 * declines, which the stub above never does — it exists so the link resolves
 * and so a future stub that DOES decline gets the same answer either way. */
int id3_meta_read(decoder_source_t *src, flac_meta_t *out)
{
    return flac_meta_read(src, out);
}

/* ---- fake decoder ----------------------------------------------------- */

static int fake_decode(decoder_t *d, int16_t *out, int max_frames)
{
    (void)d;
    if (g_frames_left == 0) {
        return 0;                     /* end of stream */
    }
    uint32_t n = ((uint32_t)max_frames < g_frames_left)
               ? (uint32_t)max_frames : g_frames_left;
    memset(out, 0, (size_t)n * 2u * sizeof(int16_t));
    g_frames_left -= n;
    return (int)n;
}

/*
 * A working seek, so the seek path is reachable at all.
 *
 * It used to return ERR_UNSUPPORTED unconditionally, which meant every
 * player_seek_to() bailed at its first guard and none of the seek logic — the
 * ring re-prime, the HAL flush, the end-of-track reopen — was ever executed by
 * a test. Landing the decoder at `frame` also makes the post-seek decode
 * produce the right NUMBER of frames, so a test can tell a real seek from a
 * no-op.
 */
static int fake_seek(decoder_t *d, uint64_t frame)
{
    (void)d;
    stub_seeks++;
    stub_last_seek_frame = frame;
    if (!g_seek_ok) {
        return DECODER_ERR_UNSUPPORTED;
    }
    if (frame > g_seek_max) {
        /* Like the real wrappers, a failed seek is not a no-op: the scan
         * moved the cursor. Model that as "somewhere unrelated" — here, the
         * end — so a player that resumes into it visibly plays nothing. */
        g_frames_left = 0;
        return DECODER_ERR_INTERNAL;
    }
    g_frames_left = (frame < g_track_frames)
                  ? (uint32_t)(g_track_frames - frame) : 0u;
    return DECODER_OK;
}

static void fake_close(decoder_t *d)
{
    (void)d;
    stub_closes++;
}

static const decoder_ops_t g_fake_ops = {
    "stub", NULL, fake_decode, fake_seek, fake_close,
};

static int fake_open(decoder_t *d)
{
    stub_open_attempts++;
    if (cluster_is_broken(g_last_open_clus)) {
        return DECODER_ERR_INVALID;
    }
    memset(d, 0, sizeof *d);
    d->ops             = &g_fake_ops;
    d->sample_rate     = g_stub_rate;
    d->channels        = 2;
    d->bits_per_sample = 16;
    d->total_frames    = g_total_unknown ? 0u : g_track_frames;
    g_frames_left      = g_track_frames;
    stub_opens++;
    return 0;
}

int flac_open_stream(decoder_t *d, decoder_source_t *src,
                     const decoder_alloc_t *alloc)
{
    (void)src;
    (void)alloc;
    return fake_open(d);
}

int mp3_open_stream(decoder_t *d, decoder_source_t *src,
                    const decoder_alloc_t *alloc)
{
    (void)src;
    (void)alloc;
    return fake_open(d);
}

/* ---- fake arena ------------------------------------------------------- */

void decoder_arena_init(decoder_arena_t *a, void *buf, size_t cap)
{
    memset(a, 0, sizeof *a);
    a->base = (unsigned char *)buf;
    a->cap  = cap;
}

static void *arena_alloc(void *ud, size_t bytes)
{
    (void)ud;
    (void)bytes;
    return NULL;      /* the fake decoder allocates nothing */
}

static void *arena_realloc(void *ud, void *p, size_t bytes)
{
    (void)ud;
    (void)p;
    (void)bytes;
    return NULL;
}

static void arena_free(void *ud, void *p)
{
    (void)ud;
    (void)p;
}

decoder_alloc_t decoder_arena_allocator(decoder_arena_t *a)
{
    decoder_alloc_t al = { arena_alloc, arena_realloc, arena_free, a };
    return al;
}

/* ---- fake HAL --------------------------------------------------------- */

int hal_audio_init(uint32_t rate, uint16_t channels)
{
    /* 44.1 kHz always; whatever rate the fake decoder has been told to report
     * as well, so a format change between tracks is reachable. */
    if ((rate != 44100u && rate != g_stub_rate) || channels != 2u) {
        return -1;
    }
    /* A re-init under a running DAC is a caller bug: the real one stops the
     * stream itself first (mute ramp, DMA cut), but a caller that relies on
     * that has skipped the stop the transport contract asks for. Counted,
     * never refused, so a scenario can see it. */
    if (stub_audio_running) {
        stub_audio_inits_while_running++;
    }
    /* A full bring-up: the codec is up and, as on the device, the buffers are
     * severed from whatever stream came before — and the DAC's count restarts. */
    stub_audio_inits++;
    stub_last_init_rate = rate;
    stub_audio_cold   = 0;
    stub_audio_primed = 0;
    g_dac_pipe   = 0;
    g_dac_played = g_played_origin;
    return 0;
}

uint32_t hal_audio_frames_played(void)
{
    return g_dac_played;
}

/* Codec gain/balance. player_open_current() re-applies these on every open so a
 * codec reset can't leave the amp at 0 dB (repeat-one used to replay the whole
 * track at full scale); the queue tests only need them to link and to record
 * that the re-apply happened. */
static int g_stub_balance;
static int g_stub_volume = 50;

void hal_balance_set(int balance) { g_stub_balance = balance; }
int  hal_balance_get(void)        { return g_stub_balance; }
void hal_volume_set(int vol)      { g_stub_volume = vol; }
int  hal_volume_get(void)         { return g_stub_volume; }

/* The player registers ring_source() here — a `static` function inside
 * player.c that drains the PCM ring. Capturing it is what lets the test act as
 * the DAC: on the device the DMA-completion ISR pulls from this callback, and
 * without something pulling, the ring never empties and the end-of-track
 * auto-advance can never fire. stub_drain() below is that pull. */
static audio_source_fn g_src;
static void           *g_src_ud;

void hal_audio_set_source(audio_source_fn fn, void *ud)
{
    g_src    = fn;
    g_src_ud = ud;
}

int stub_drain(int frames)
{
    static int16_t sink[4096 * 2];
    if (g_src == NULL || !stub_audio_running) {
        return 0;
    }
    int want = (frames > 4096) ? 4096 : frames;
    int got  = g_src(g_src_ud, sink, want);
    dac_pull((uint32_t)want);      /* the DAC clocks the whole pull, padding too */
    return got;
}

void hal_audio_start(void)
{
    stub_audio_starts++;
    /* A start on a cold codec is the bug the wake exists to prevent: on the
     * device the DMA would stream into an unclocked FIFO and simply never
     * complete. Modelled as "not running" so a missing wake shows up as a DAC
     * that did not restart, rather than being invisible. */
    stub_audio_running = stub_audio_cold ? 0 : 1;
}

void hal_audio_suspend(void)
{
    if (stub_audio_running) {
        stub_audio_suspends_while_running++;   /* the real one refuses this */
        return;
    }
    if (stub_audio_cold) {
        return;                                /* idempotent, as on the device */
    }
    stub_audio_suspends++;
    stub_audio_cold = 1;
    /* Deliberately NOT clearing stub_audio_primed: keeping the buffered PCM is
     * the whole difference between a suspend and a close. */
}

int hal_audio_wake(void)
{
    if (!stub_audio_cold) {
        return 0;
    }
    stub_audio_wakes++;
    stub_audio_cold = 0;
    return 0;
}

void hal_audio_stop(void)
{
    stub_audio_stops++;
    stub_audio_running = 0;
    /* Modelled, because the bug this suite missed lived here: the real backend
     * KEEPS its buffered PCM across a stop so unpause is seamless, and a
     * caller that has changed what the source will produce has to say so. A
     * stub that forgets that cannot see a missing flush. */
    stub_audio_primed = 1;
}

void hal_audio_flush(void)
{
    stub_audio_flushes++;
    stub_audio_primed = 0;
    g_dac_pipe = 0;                /* discarded unheard; the count stands */
}

int hal_audio_drain(uint32_t timeout_ms)
{
    (void)timeout_ms;
    stub_audio_drains++;
    /* Ordering is what matters to the tests: a drain is only meaningful while
     * the engine is still running, i.e. before the stop it precedes. */
    stub_audio_drained_while_running += stub_audio_running ? 1 : 0;
    if (stub_audio_running) {
        g_dac_played += g_dac_pipe;   /* the pipe plays out */
        g_dac_pipe    = 0;
    }
    return 0;
}

void hal_audio_close(void)
{
    hal_audio_stop();
    stub_audio_cold   = 1;
    stub_audio_primed = 0;    /* close severs the buffers; suspend does not */
}

/* ---- fake drive ------------------------------------------------------- */

int ata_read_sectors(uint32_t lba, uint32_t count, void *buf)
{
    (void)lba;
    stub_ata_reads++;
    g_ata_parked = 0;
    if (!g_ata_read_ok) {
        return -1;
    }
    memset(buf, 0, (size_t)count * 512u);
    return 0;
}

int ata_standby(void)
{
    stub_ata_standbys++;
    g_ata_parked = 1;
    return 0;
}

/* The HAL's "shared truth" for the platter state: set by a STANDBY, cleared
 * by any read (the real ata_read_raw clears it as the command goes out) and
 * by the explicit wake. Tests can force it to model a park the main loop
 * made (its idle spin-down) that the player never saw. */
int ata_is_parked(void)
{
    return g_ata_parked;
}

int ata_wakeup(void)
{
    stub_ata_wakeups++;
    g_ata_parked = 0;
    return 0;
}

void stub_set_ata_parked(int parked) { g_ata_parked = parked; }

void sleep_ms(uint32_t ms)
{
    (void)ms;
}

/*
 * Disk-error hook. The real diskbuf calls this to ask the source whether a
 * short read was a genuine end-of-file or a persistent read failure — before
 * that distinction existed, one bad sector three minutes into a track looked
 * exactly like the track ending, and the player just advanced. The queue tests
 * exercise clean end-of-track, so the hook is recorded and never fired.
 */
void diskbuf_set_error_hook(diskbuf_t *db, int (*fn)(void *ud), void *ud)
{
    (void)db;
    (void)fn;
    (void)ud;
}

/* Arena reset between tracks: the fake decoder allocates nothing. */
void decoder_arena_reset(decoder_arena_t *a)
{
    (void)a;
}

/* ReplayGain pre-scale. Only applied when a REPLAYGAIN_* tag is present, and
 * the fake metadata below never supplies one. */
void flac_set_gain_db_q8(decoder_t *d, int db_q8)
{
    (void)d;
    (void)db_q8;
}
