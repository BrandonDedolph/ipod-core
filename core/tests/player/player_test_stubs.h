/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/player/player_test_stubs.h — control surface for the fake world the
 * player test runs core/player/player.c inside. See player_test_stubs.c.
 */
#ifndef CORE_TESTS_PLAYER_STUBS_H
#define CORE_TESTS_PLAYER_STUBS_H

#include <stdint.h>

/* Clear every counter and the broken-cluster list. Call before each case. */
void stub_reset(void);

/* Make the decoder refuse to open the file starting at `clus` — the "this
 * track is corrupt / missing" case the queue logic must skip past. */
void stub_break_cluster(uint32_t clus);

/* Frames the fake decoder yields before reporting end-of-stream. Keep it small
 * so a track finishes within a bounded number of player_pump() calls. */
void stub_set_track_frames(uint32_t frames);

/*
 * Act as the DAC: pull up to `frames` frames through the source callback the
 * player registered. On the device the DMA-completion ISR does this; without
 * it the PCM ring never empties and end-of-track auto-advance never fires, so
 * any test of the queue's advance behaviour has to drive it. Returns the
 * frames actually drained (0 when stopped/paused or the ring is empty).
 */
int stub_drain(int frames);

/* Cluster of the file most recently opened — i.e. which queue entry the player
 * actually chose, independent of what its index says. */
uint32_t stub_last_open_clus(void);

/*
 * Seek control. stub_set_seek_ok(0) makes every seek fail (a codec that
 * cannot seek at all); stub_set_seek_max(n) fails only targets past frame n
 * (a seek that overshoots the stream — the top of the track is still
 * reachable). stub_last_seek_frame is the target the decoder last received,
 * so a clamp applied by the player is visible. stub_set_total_unknown(1)
 * makes the next open report total_frames == 0, the "length unknown" case.
 */
void stub_set_seek_max(uint64_t max_frame);
void stub_set_total_unknown(int unknown);
extern uint64_t stub_last_seek_frame;

/*
 * The drive's read path. player_disk_read() retries a failed sector read six
 * times — unless the pump has armed its one-shot spin-up probe, in which case
 * it tries once. That flag is private to player.c, but it is observable here:
 * make the reads fail and count how many attempts one player_disk_read() costs.
 */
void stub_set_ata_read_ok(int ok);
extern int stub_ata_reads;      /* ata_read_sectors calls, success or not   */

/* Bytes the fake anti-skip buffer reports as buffered ahead of the decoder.
 * Defaults to far above the player's low watermark ("topped up and idle");
 * dropping it below the watermark for a pass models the start of a refill
 * burst, which is what lets a test steer the drive park/unpark bookkeeping. */
void stub_set_disk_ahead(uint32_t bytes);

extern int stub_opens;          /* decoder opens that succeeded             */
extern int stub_open_attempts;  /* opens attempted, incl. the failures      */
extern int stub_closes;         /* decoder closes                           */
extern int stub_audio_starts;
extern int stub_audio_stops;
extern int stub_audio_running;  /* 1 while the DAC is running               */
/*
 * The real backend keeps its ping-pong PCM across a stop so unpause is
 * seamless; only hal_audio_flush() discards it. Modelling that here is what
 * lets a test see a MISSING flush — with start/stop as bare counters, a seek
 * that resumed into ~370 ms of the old position looked identical to a correct
 * one.
 */
extern int stub_audio_primed;   /* HAL holds unplayed PCM                    */
extern int stub_audio_flushes;
/*
 * Codec power state. hal_audio_stop() (the pause) leaves the codec fully
 * powered; only hal_audio_suspend()/hal_audio_close() take it down, and
 * hal_audio_wake()/hal_audio_init() bring it back. Modelled so a test can see
 * that a PERSISTENT pause powers the codec down, that a short one does not,
 * and that the resume path wakes it — while stub_audio_primed shows whether
 * the power-down kept the HAL's PCM (suspend) or discarded it (close).
 */
extern int stub_audio_cold;     /* 1 while the codec is powered down         */
extern int stub_audio_suspends; /* hal_audio_suspend() calls that took effect */
extern int stub_audio_wakes;    /* hal_audio_wake() calls that took effect    */
extern int stub_audio_suspends_while_running; /* caller bug: suspend under DMA */
extern int stub_audio_drains;
extern int stub_audio_drained_while_running;
extern int stub_seeks;

/* Make the fake decoder's seek refuse, to exercise the failure path. */
void stub_set_seek_ok(int ok);
extern int stub_ata_standbys;   /* drive spin-down requests                 */
extern int stub_meta_reads;     /* tag parses                               */

#endif /* CORE_TESTS_PLAYER_STUBS_H */
