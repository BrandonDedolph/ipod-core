/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/player/player_clock_test.c — the reported position against what the
 * listener can actually hear.
 *
 * Three quantities are in play, and the player used to conflate two of them:
 *
 *   pushed    frames the decoder has written into the PCM ring;
 *   consumed  frames the DMA feeder has pulled OUT of the ring — on the
 *             device, in whole 8192-frame ping-pong buffers, two of which sit
 *             inside the HAL between the ring and the DAC at any moment;
 *   heard     frames that have actually left the DAC.
 *
 * consumed leads heard by one to two buffers (186..372 ms at 44.1 kHz). The
 * gapless hand-over committed its presentation switch — title, art, and the
 * re-anchoring of the elapsed clock — when the boundary frame was CONSUMED,
 * so every track after the first showed a clock that far ahead of the sound.
 *
 * The real player.c runs unmodified over the fakes in player_test_stubs.c.
 * stub_drain() is the DMA feeder: what this test pulls through the source
 * callback is, by definition, `consumed`, so the relationship between pulls
 * and the presented state is pinned exactly. `heard` comes back to the player
 * through hal_audio_frames_played(), which the stub models as a pipe of
 * pulled-but-unheard frames of a settable depth. The first sections run it at
 * the device's depth (2 x 8192, hal/hw/audio.c's ping-pong pair), because
 * that is the case the bug was found in; section 6 runs the same crossing at
 * the sim's depth and the bare FIFO's, because the player no longer knows
 * the depth at all — it used to subtract the device's two buffers, which
 * was 0..186 ms late on the device and ~350 ms late on the sim.
 *
 * Section 4 is the other side of the same ring: how much the pump pushes
 * per pass. One step while the ring is healthy, a refill to two pulls' worth
 * when it is thin — the ration a blocking caller (the library scan) would
 * otherwise starve the DMA through.
 */

#include <stdio.h>
#include <string.h>

#include "player.h"
#include "pp5022.h"
#include "mmio_mock.h"
#include "player_test_stubs.h"
#include "../xfail.h"

#define CLUS(i) (100u + (uint32_t)(i))

/* The HAL's ping-pong depth, in frames: see the header comment. */
#define HAL_BUF_FRAMES      8192u
#define HAL_INFLIGHT_FRAMES (2u * HAL_BUF_FRAMES)

/* The stub pulls at most 4096 frames per call; the drains below are all
 * multiples of it so `consumed` is an exact count. */
#define PULL 4096

static fat32_t g_fs;

static void make_entries(browse_entry_t *e, int n)
{
    memset(e, 0, sizeof(browse_entry_t) * (size_t)n);
    for (int i = 0; i < n; i++) {
        snprintf(e[i].name, sizeof e[i].name, "TRACK%02d", i);
        e[i].clus = CLUS(i);
        e[i].size = 1024u * 1024u;
    }
}

static void set_usec(uint32_t us)
{
    mmio_mock_set_read(USEC_TIMER_ADDR, us);
}

/* Pull exactly `frames` out of the ring as the DMA would, without letting the
 * pump run in between. Returns 1 if every pull was full — the ring held what
 * the scenario assumed it held. */
static int drain_exact(uint32_t frames)
{
    int ok = 1;
    while (frames > 0) {
        int want = frames < PULL ? (int)frames : PULL;
        ok &= stub_drain(want) == want;
        frames -= (uint32_t)want;
    }
    return ok;
}

/* Pull until the ring is dry; returns how much it held. */
static uint32_t drain_all(void)
{
    uint32_t total = 0;
    for (;;) {
        int got = stub_drain(PULL);
        if (got <= 0) {
            return total;
        }
        total += (uint32_t)got;
    }
}

/*
 * Start a two-track queue and decode both tracks as far as the ring allows,
 * WITHOUT pulling anything: track 0 reaches end-of-stream, track 1 is
 * prefetched behind it, and the hand-over boundary sits at exactly
 * TRACK_FRAMES pushed frames. Nothing has been consumed, so nothing has been
 * heard, and the presented track must still be 0.
 */
#define TRACK_FRAMES 131072u   /* ~2.97 s: long enough to overshoot by >1 s */

/* `depth` is the modelled HAL depth (see the header); `origin` is where the
 * HAL's played count starts, 0 unless a scenario wants it near the wrap. */
static void start_two_tracks(browse_entry_t *ents, uint32_t depth,
                             uint32_t origin)
{
    stub_reset();
    stub_set_dac_depth(depth);
    stub_set_played_origin(origin);
    stub_set_track_frames(TRACK_FRAMES);
    make_entries(ents, 2);
    player_play_queue(ents, 2, 0, 0, 0);
    /* Each pump decodes at most one step; 400 is far more than the ring can
     * absorb, so this ends with the ring full and both tracks opened. */
    for (int i = 0; i < 400; i++) {
        player_pump();
    }
}

int main(void)
{
    xfail_ctx c = { "player-clock", 0, 0, 0 };
    browse_entry_t ents[4];

    mmio_mock_reset();
    set_usec(0);
    player_init(&g_fs);

    /* ---- 1. the boundary is presented when HEARD, not when consumed ----- */
    start_two_tracks(ents, HAL_INFLIGHT_FRAMES, 0u);
    xpect(&c, "setup: track 1 was prefetched behind track 0",
          stub_opens == 2 && player_queue_current() == 0);
    uint32_t seq0 = player_open_seq();

    /* Pull the whole of track 0 through the feeder: the boundary frame is
     * now CONSUMED. It is inside the HAL, two buffers from the DAC. */
    xpect(&c, "the ring held all of track 0", drain_exact(TRACK_FRAMES));
    player_pump();
    xpect(&c, "consuming the boundary does not present the next track",
          player_queue_current() == 0 && player_open_seq() == seq0);

    /* One pull short of the in-flight depth: the boundary is still in the
     * second buffer, not yet at the DAC. */
    xpect(&c, "the ring held track 1's first frames",
          drain_exact(HAL_INFLIGHT_FRAMES - PULL));
    player_pump();
    xpect(&c, "one pull short of the HAL's depth: still the old track",
          player_queue_current() == 0 && player_open_seq() == seq0);

    /* The pull that takes consumed to boundary + in-flight depth: the
     * boundary has now left the DAC. */
    set_usec(5000000u);
    xpect(&c, "drain the last in-flight buffer", drain_exact(PULL));
    player_pump();
    xpect(&c, "consumed == boundary + in-flight depth presents track 1",
          player_queue_current() == 1 && player_open_seq() == seq0 + 1);
    xpect(&c, "the new track's clock starts at 0:00 at that instant",
          player_elapsed_s() == 0u);
    set_usec(5999999u);
    xpect(&c, "...and reads 0 just before its first second",
          player_elapsed_s() == 0u);
    set_usec(6000000u);
    xpect(&c, "...and 1 at its first second", player_elapsed_s() == 1u);

    /* ---- 2. frames already heard past the boundary are credited --------- *
     * The pump only looks between passes. If it first sees the crossing 45056
     * frames (1.0217 s at 44.1 kHz) after the fact, the clock must read that,
     * not restart from zero. */
    start_two_tracks(ents, HAL_INFLIGHT_FRAMES, 0u);
    seq0 = player_open_seq();
    xpect(&c, "credit: the ring held track 0 and the overshoot",
          drain_exact(TRACK_FRAMES + HAL_INFLIGHT_FRAMES + 11u * PULL));
    set_usec(20000000u);
    player_pump();
    xpect(&c, "credit: the hand-over is presented", player_queue_current() == 1);
    xpect(&c, "credit: the clock already reads 1 s, the overshoot credited",
          player_elapsed_s() == 1u);
    /* 45056 frames = 1021678 us; 978000 more is 1.999678 s, 979000 is 2.0007. */
    set_usec(20978000u);
    xpect(&c, "credit: 1 s at 1.9997 s", player_elapsed_s() == 1u);
    set_usec(20979000u);
    xpect(&c, "credit: 2 s at 2.0007 s", player_elapsed_s() == 2u);

    /* ---- 3. a next track shorter than the HAL's depth still presents ---- *
     * Everything decoded has been pulled, the feeder is padding silence, and
     * `heard` can never reach the boundary. The commit must fall through on
     * the empty ring rather than wait for a crossing that will not come. */
    stub_reset();
    stub_set_track_frames(HAL_BUF_FRAMES);         /* 186 ms per track */
    make_entries(ents, 2);
    player_play_queue(ents, 2, 0, 0, 0);
    for (int i = 0; i < 40; i++) {
        player_pump();                             /* decode both tracks */
    }
    xpect(&c, "short: both tracks decoded, track 0 presented",
          stub_opens == 2 && player_queue_current() == 0);
    xpect(&c, "short: the ring held both tracks",
          drain_exact(2u * HAL_BUF_FRAMES));
    xpect(&c, "short: the ring is now empty", stub_drain(PULL) == 0);
    player_pump();
    xpect(&c, "short: an empty ring presents the hand-over instead of stalling",
          player_queue_current() == 1);
    for (int i = 0; i < 40 && player_active(); i++) {
        player_pump();
        stub_drain(PULL);
    }
    xpect(&c, "short: playback then runs out at the end of the queue",
          player_active() == 0);

    /* ---- 4. the pump's decode ration versus a thin ring ------------------ *
     * One 1024-frame step per pass while the ring is healthy (the UI's
     * latency budget). Below two DMA pulls' worth, a caller that blocks
     * between pumps — the library scan — would drain the ring at one step per
     * pass, so the pump must instead refill to that line in one pass. The
     * fill after a pump is read back by draining the ring dry. */
    stub_reset();
    stub_set_track_frames(262144u);                /* never reaches EOS here */
    make_entries(ents, 1);
    player_play_queue(ents, 1, 0, 0, 0);           /* primes 65536 frames */
    xpect(&c, "ration: leave 20480 in the ring (above the 16384 line)",
          drain_exact(65536u - 20480u));
    player_pump();
    xpect(&c, "ration: a healthy ring gets exactly one 1024-frame step",
          drain_all() == 20480u + 1024u);
    player_pump();                                 /* ring was empty */
    xpect(&c, "ration: an empty ring is refilled to the 16384-frame line",
          drain_all() == HAL_INFLIGHT_FRAMES);
    player_pump();                                 /* empty again: back to the line */
    xpect(&c, "ration: pull one buffer, leaving the ring one pull below the line",
          drain_exact(HAL_BUF_FRAMES));
    player_pump();
    xpect(&c, "ration: one pass restores the line, not one step",
          drain_all() == HAL_INFLIGHT_FRAMES);

    /* ---- 5. the clock survives the microsecond timer wrapping ------------ *
     * USEC_TIMER is a free-running 32-bit 1 MHz counter: it wraps every
     * 4294.97 s (71:35). The elapsed clock used to be (now - start) in 32-bit
     * microseconds, so a two-hour podcast snapped back to 0:00 at 1:11:35 —
     * and the seek anchor computed now - sec*1e6 in the same width, so a
     * scrub to 1:30:00 (5400e6 us, itself past 2^32) landed at 18:25. The
     * scenario below crosses BOTH: the timer wraps 295 s into the track, and
     * the elapsed time passes 2^32 us at 4295 s. Every step is 50 s of wall
     * time with one pump, far under the wrap, as on the device. */
    stub_reset();
    stub_set_track_frames(44100u * 7200u);         /* a 2 h track */
    make_entries(ents, 1);
    uint32_t t = 4000000000u;                      /* 295 s before the wrap */
    set_usec(t);
    player_play_queue(ents, 1, 0, 0, 0);
    xpect(&c, "wrap: a track started near the wrap reads 0:00",
          player_elapsed_s() == 0u);
    int clock_ok = 1;
    for (uint32_t sec = 50; sec <= 5000; sec += 50) {
        t = 4000000000u + sec * 1000000u;          /* modular: wraps naturally */
        set_usec(t);
        player_pump();
        if (player_elapsed_s() != sec) {
            clock_ok = 0;
        }
    }
    xpect(&c, "wrap: the clock reads true elapsed time across the timer wrap "
              "and past 2^32 us of it", clock_ok);
    xpect(&c, "wrap: 1:23:20 in, the clock says so", player_elapsed_s() == 5000u);
    set_usec(t + 30000000u);                       /* no pump in between */
    xpect(&c, "wrap: a read between pumps still runs",
          player_elapsed_s() == 5030u);
    t += 30000000u;
    set_usec(t);
    player_pump();

    /* The seek anchor, past 71:35. */
    xpect(&c, "wrap: a scrub to 1:30:00 succeeds", player_seek_to(5400u) == 0);
    xpect(&c, "wrap: ...and the clock reads 1:30:00, not 18:25",
          player_elapsed_s() == 5400u);
    t += 10000000u;
    set_usec(t);
    player_pump();
    xpect(&c, "wrap: and runs on from the target", player_elapsed_s() == 5410u);

    /* A pause that itself outlives the timer's period: nothing of it counts,
     * and the codec power-down inside it (timed from the pause stamp) is
     * unaffected. */
    player_pause();
    for (int k = 0; k < 10; k++) {
        t += 500000000u;                           /* 5000 s of pause, in 500 s steps */
        set_usec(t);
        player_pump();
    }
    xpect(&c, "wrap: a 5000 s pause counts nothing", player_elapsed_s() == 5410u);
    xpect(&c, "wrap: the pause powered the codec down once, as usual",
          stub_audio_suspends == 1);
    player_resume();
    t += 7000000u;
    set_usec(t);
    player_pump();
    xpect(&c, "wrap: resume after the long pause carries on, +7 s",
          player_elapsed_s() == 5417u);
    /* And relative seeks compute against the true position. */
    xpect(&c, "wrap: a relative seek from 1:30:17", player_seek_seconds(-17) == 0);
    xpect(&c, "wrap: ...lands on 1:30:00", player_elapsed_s() == 5400u);
    stub_set_track_frames(8192u);

    /* ---- 6. the hand-over follows the HAL's count, whatever its depth ---- *
     * The player used to subtract the device's two buffers from what it had
     * pulled. It now asks hal_audio_frames_played(), so the same crossing
     * must present at the exact frame for the bare I2S FIFO (16), the sim's
     * SDL buffer (1024) and the device's ping-pong pair (16384) alike — and
     * credit the overshoot exactly in each case. The residual left is the
     * pump's own pass granularity, which these pumps make zero. */
    {
        static const uint32_t depths[3] = { 16u, 1024u, HAL_INFLIGHT_FRAMES };
        for (int d = 0; d < 3; d++) {
            uint32_t depth = depths[d];
            char label[96];

            start_two_tracks(ents, depth, 0u);
            seq0 = player_open_seq();
            snprintf(label, sizeof label,
                     "depth %u: one frame short of the boundary is still track 0",
                     (unsigned)depth);
            xpect(&c, label, drain_exact(TRACK_FRAMES + depth - 1u));
            player_pump();
            xpect(&c, label,
                  player_queue_current() == 0 && player_open_seq() == seq0);
            snprintf(label, sizeof label,
                     "depth %u: the frame that crosses it presents track 1",
                     (unsigned)depth);
            xpect(&c, label, drain_exact(1u));
            set_usec(30000000u);
            player_pump();
            xpect(&c, label,
                  player_queue_current() == 1 && player_open_seq() == seq0 + 1);
            snprintf(label, sizeof label,
                     "depth %u: ...with the clock at 0:00", (unsigned)depth);
            xpect(&c, label, player_elapsed_s() == 0u);

            /* Late by 45056 frames (1.0217 s): credited, at this depth too. */
            start_two_tracks(ents, depth, 0u);
            snprintf(label, sizeof label,
                     "depth %u: an overshoot seen a pass late is credited",
                     (unsigned)depth);
            xpect(&c, label, drain_exact(TRACK_FRAMES + depth + 11u * PULL));
            set_usec(40000000u);
            player_pump();
            xpect(&c, label,
                  player_queue_current() == 1 && player_elapsed_s() == 1u);
            set_usec(40978000u);
            xpect(&c, label, player_elapsed_s() == 1u);
            set_usec(40979000u);
            xpect(&c, label, player_elapsed_s() == 2u);
        }
    }

    /* ---- 7. the HAL's count wraps under the crossing -------------------- *
     * hal.h: a uint32, take differences. Start it 256 frames short of 2^32 so
     * it wraps inside track 0; the crossing and the credit must not notice. */
    start_two_tracks(ents, HAL_INFLIGHT_FRAMES, 0xFFFFFF00u);
    seq0 = player_open_seq();
    xpect(&c, "hal wrap: one frame short is still track 0",
          drain_exact(TRACK_FRAMES + HAL_INFLIGHT_FRAMES - 1u));
    player_pump();
    xpect(&c, "hal wrap: ...",
          player_queue_current() == 0 && player_open_seq() == seq0);
    xpect(&c, "hal wrap: the crossing presents track 1 and credits nothing",
          drain_exact(1u));
    set_usec(50000000u);
    player_pump();
    xpect(&c, "hal wrap: ...",
          player_queue_current() == 1 && player_elapsed_s() == 0u);
    start_two_tracks(ents, HAL_INFLIGHT_FRAMES, 0xFFFFFF00u);
    xpect(&c, "hal wrap: an overshoot across the wrap is credited exactly",
          drain_exact(TRACK_FRAMES + HAL_INFLIGHT_FRAMES + 11u * PULL));
    set_usec(60000000u);
    player_pump();
    xpect(&c, "hal wrap: ...",
          player_queue_current() == 1 && player_elapsed_s() == 1u);

    /* ---- 8. a pause in the middle moves nothing ------------------------- *
     * The count is frozen by the stop and carries on from the resume: the
     * boundary is still reached by the same pull, not one earlier or later,
     * however long the pause (long enough here to power the codec down). */
    start_two_tracks(ents, HAL_INFLIGHT_FRAMES, 0u);
    seq0 = player_open_seq();
    xpect(&c, "pause: half of track 0 is pulled", drain_exact(TRACK_FRAMES / 2u));
    set_usec(70000000u);
    player_pause();
    xpect(&c, "pause: nothing is pulled while paused", stub_drain(PULL) == 0);
    for (int k = 0; k < 20; k++) {
        set_usec(70000000u + (uint32_t)(k + 1) * 3000000u);   /* a minute */
        player_pump();
    }
    xpect(&c, "pause: the codec went cold underneath", stub_audio_suspends == 1);
    player_resume();
    xpect(&c, "pause: the other half, less one frame, is still track 0",
          drain_exact(TRACK_FRAMES / 2u + HAL_INFLIGHT_FRAMES - 1u));
    player_pump();
    xpect(&c, "pause: ...",
          player_queue_current() == 0 && player_open_seq() == seq0);
    xpect(&c, "pause: the same frame as without the pause presents track 1",
          drain_exact(1u));
    player_pump();
    xpect(&c, "pause: ...",
          player_queue_current() == 1 && player_open_seq() == seq0 + 1);

    /* ---- 9. a seek re-pairs the counts ---------------------------------- *
     * A seek resets the ring (g_written restarts at 0) and flushes the HAL,
     * but the HAL's played count carries on. The player must re-pair the two
     * at that moment, or the next boundary would be judged against a count
     * that is off by everything heard before the seek. */
    start_two_tracks(ents, HAL_INFLIGHT_FRAMES, 0u);
    xpect(&c, "seek: some of track 0 is heard first",
          drain_exact(3u * HAL_BUF_FRAMES));      /* 8192 heard, 16384 in flight */
    set_usec(80000000u);
    xpect(&c, "seek: a scrub to 0:01 succeeds", player_seek_to(1u) == 0);
    xpect(&c, "seek: ...which flushed the HAL", stub_audio_flushes == 1);
    for (int i = 0; i < 400; i++) {
        player_pump();                              /* decode the rest, prefetch */
    }
    seq0 = player_open_seq();
    xpect(&c, "seek: track 1 is prefetched behind the seeked track 0",
          player_queue_current() == 0);
    /* Track 0 has TRACK_FRAMES - 44100 frames left from 0:01; the boundary
     * sits there in the fresh ring's numbering. */
    xpect(&c, "seek: one frame short of the new boundary is still track 0",
          drain_exact((TRACK_FRAMES - 44100u) + HAL_INFLIGHT_FRAMES - 1u));
    player_pump();
    xpect(&c, "seek: ...",
          player_queue_current() == 0 && player_open_seq() == seq0);
    xpect(&c, "seek: the frame that crosses it presents track 1", drain_exact(1u));
    player_pump();
    xpect(&c, "seek: ...",
          player_queue_current() == 1 && player_open_seq() == seq0 + 1);
    xpect(&c, "seek: ...with the clock at 0:00", player_elapsed_s() == 0u);

    /* ---- 10. a format change re-pairs them through hal_audio_init -------- *
     * Track 0 at 44.1 kHz, tracks 1 and 2 at 48 kHz. The 0->1 hand-over
     * re-clocks the DAC (a hal_audio_init, which zeroes the HAL's count) while
     * the ring's numbering runs on; the 1->2 hand-over is then gapless and
     * must be judged against the re-paired counts. */
    stub_reset();
    stub_set_track_frames(TRACK_FRAMES);
    make_entries(ents, 3);
    player_play_queue(ents, 3, 0, 0, 0);            /* track 0 opens at 44.1 kHz */
    stub_set_rate(48000u);                          /* everything after: 48 kHz */
    for (int i = 0; i < 400; i++) {
        player_pump();
    }
    xpect(&c, "rate: track 1 is prefetched and decode is held at the boundary",
          stub_opens == 2 && player_queue_current() == 0 &&
          stub_audio_inits == 1);
    xpect(&c, "rate: the ring holds exactly track 0", drain_exact(TRACK_FRAMES));
    xpect(&c, "rate: ...and nothing more", stub_drain(PULL) == 0);
    set_usec(90000000u);
    player_pump();                                  /* the ring ran dry: commit */
    xpect(&c, "rate: the format change re-clocks the DAC and presents track 1",
          player_queue_current() == 1 && stub_audio_inits == 2 &&
          stub_audio_running == 1);
    xpect(&c, "rate: ...with the clock at 0:00", player_elapsed_s() == 0u);
    for (int i = 0; i < 400; i++) {
        player_pump();                              /* decode track 1, prefetch 2 */
    }
    seq0 = player_open_seq();
    xpect(&c, "rate: track 2 is prefetched gapless behind track 1",
          stub_opens == 3 && player_queue_current() == 1);
    xpect(&c, "rate: one frame short of the gapless boundary is still track 1",
          drain_exact(TRACK_FRAMES + HAL_INFLIGHT_FRAMES - 1u));
    player_pump();
    xpect(&c, "rate: ...",
          player_queue_current() == 1 && player_open_seq() == seq0);
    xpect(&c, "rate: the frame that crosses it presents track 2", drain_exact(1u));
    player_pump();
    xpect(&c, "rate: ...",
          player_queue_current() == 2 && player_open_seq() == seq0 + 1);
    xpect(&c, "rate: ...with the clock at 0:00", player_elapsed_s() == 0u);
    stub_reset();

    /* ---- 11. a deferred bring-up re-pairs them too ----------------------- *
     * A skip while paused no longer brings the DAC up; the resume does. Both
     * shapes of that bring-up have to leave frames_heard() honest for the
     * hand-over that follows: the same-rate one (no init — the HAL's count
     * stands, the open flushed what the HAL held) and the new-rate one (an
     * init, which restarts the count — here right next to the wrap). Track 0
     * plays a little, the listener pauses and skips to track 1, resumes;
     * track 2 is then prefetched gapless behind track 1, and its boundary
     * must present exactly when the DAC reaches it. */
    for (int newrate = 0; newrate < 2; newrate++) {
        stub_reset();
        stub_set_dac_depth(HAL_INFLIGHT_FRAMES);
        stub_set_track_frames(TRACK_FRAMES);
        make_entries(ents, 3);
        player_play_queue(ents, 3, 0, 0, 0);
        player_pump();
        xpect(&c, "deferred: track 0 played a little before the pause",
              drain_exact(PULL * 5u));
        set_usec(1000000u);
        player_pause();
        if (newrate) {
            stub_set_rate(48000u);
            stub_set_played_origin(0xFFFFFF00u); /* the init lands by the wrap */
        }
        player_next();                          /* -> track 1, bring-up owed */
        xpect(&c, newrate ? "deferred/new rate: the skip brought nothing up"
                          : "deferred/same rate: the skip brought nothing up",
              stub_audio_inits == 1 && stub_audio_starts == 1 &&
              player_paused() == 1);
        set_usec(2000000u);
        player_resume();
        xpect(&c, newrate ? "deferred/new rate: the resume inits at 48 kHz"
                          : "deferred/same rate: the resume needs no init",
              stub_audio_inits == (newrate ? 2 : 1) &&
              (!newrate || stub_last_init_rate == 48000u) &&
              stub_audio_running == 1);
        for (int i = 0; i < 400; i++) {
            player_pump();                      /* decode track 1, prefetch 2 */
        }
        seq0 = player_open_seq();
        xpect(&c, "deferred: track 2 is prefetched gapless behind track 1",
              stub_opens == 3 && player_queue_current() == 1 &&
              stub_audio_inits == (newrate ? 2 : 1));
        xpect(&c, "deferred: one frame short of the boundary is still track 1",
              drain_exact(TRACK_FRAMES + HAL_INFLIGHT_FRAMES - 1u));
        player_pump();
        xpect(&c, "deferred: ...",
              player_queue_current() == 1 && player_open_seq() == seq0);
        xpect(&c, "deferred: the frame that crosses it presents track 2",
              drain_exact(1u));
        player_pump();
        xpect(&c, "deferred: ...",
              player_queue_current() == 2 && player_open_seq() == seq0 + 1);
        xpect(&c, "deferred: ...with the clock at 0:00",
              player_elapsed_s() == 0u);
    }
    stub_reset();

    return xfail_done(&c);
}
