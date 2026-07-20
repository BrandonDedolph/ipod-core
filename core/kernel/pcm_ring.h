/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/pcm_ring.h — single-producer / single-consumer PCM ring.
 *
 * Decouples the blocking disk pump (producer, foreground) from the DMA-
 * completion ISR (consumer) on the streaming-audio path: the pump reads PCM
 * off the disk and writes it here; the ISR drains it into the DMA buffer
 * without ever touching the disk. Frames are interleaved 16-bit stereo
 * ([L,R,L,R,...]).
 *
 * SPSC discipline (mirrors apps/audio/engine.c so both audio paths share
 * one memory model): monotonic uint32 indices masked to the power-of-two
 * capacity. `free = cap - (wr - rd)`; `fill = wr - rd`. Unsigned wraparound
 * makes the subtraction correct even after 2^32 frames. Exactly one writer
 * of each index — the producer publishes `wr` with release ordering after
 * storing frames, the consumer acquire-loads `wr` before reading them, and
 * symmetrically for `rd`. On the PP502x the consumer is a DMA ISR on the
 * same core as the pump, but the release/acquire pair is the correct
 * portable contract regardless of how producer and consumer are scheduled.
 */
#ifndef CORE_KERNEL_PCM_RING_H
#define CORE_KERNEL_PCM_RING_H

#include <stdint.h>

typedef struct {
    int16_t *buf;         /* caller storage: cap_frames * 2 int16 ([L,R])   */
    uint32_t cap_frames;  /* capacity in stereo frames; MUST be a power of 2 */
    uint32_t wr;          /* frames written  (producer publishes; monotonic) */
    uint32_t rd;          /* frames consumed (consumer publishes; monotonic) */
} pcm_ring_t;

/*
 * Bind `storage` (cap_frames * 2 int16 of caller-owned memory) as the ring's
 * backing store and reset it empty. `cap_frames` MUST be a power of two.
 */
void pcm_ring_init(pcm_ring_t *r, int16_t *storage, uint32_t cap_frames);

/* Frames currently available to read. Safe from either side. */
uint32_t pcm_ring_fill(const pcm_ring_t *r);

/* Frames currently free to write. Safe from either side. */
uint32_t pcm_ring_free(const pcm_ring_t *r);

/*
 * Producer: copy up to `frames` interleaved stereo frames from `src` into
 * the ring. Returns the number actually written (bounded by free space; a
 * short return means the ring is full).
 */
uint32_t pcm_ring_write(pcm_ring_t *r, const int16_t *src, uint32_t frames);

/*
 * Consumer: copy up to `frames` interleaved stereo frames from the ring into
 * `dst`. Returns the number actually read (bounded by fill; a short return
 * means the ring is empty — the caller zero-pads the remainder).
 */
uint32_t pcm_ring_read(pcm_ring_t *r, int16_t *dst, uint32_t frames);

#endif /* CORE_KERNEL_PCM_RING_H */
