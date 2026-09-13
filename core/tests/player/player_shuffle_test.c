/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/player/player_shuffle_test.c — the shuffle ORDER as a value that can
 * be saved and dealt again, host-side.
 *
 * player_queue_test.c proves the shuffle is a permutation (every track once,
 * Prev goes back, Repeat All re-deals). This file proves the thing the resume
 * path leans on: an order is a pure function of the (seed, keep) pair the
 * player records for it, so the pair alone — 4 bytes and an index in the
 * settings record — brings back "what plays next" after a power cut. The
 * order itself is never read out; as in player_queue_test.c the assertions
 * are about the sequence of files that would actually play, read back from
 * the cluster the fake decoder was opened on.
 *
 * Same fakes as player_queue_test.c. The mock USEC timer is the ONLY entropy
 * the player has, so every sequence here is fixed for a given timer value —
 * "two deals differ" is a deterministic fact of this file, not a coin toss.
 */

#include <stdio.h>
#include <string.h>

#include "player.h"
#include "pp5022.h"
#include "mmio_mock.h"
#include "player_test_stubs.h"
#include "../xfail.h"

#define CLUS(i) (100u + (uint32_t)(i))
#define N       8

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

/* Press Next `n` times from wherever the player is, recording the cluster
 * that plays after each press. Returns how many presses still had a track. */
static int walk_next(uint32_t *out, int n)
{
    int got = 0;
    for (int i = 0; i < n; i++) {
        player_next();
        if (!player_active()) break;
        out[got++] = stub_last_open_clus();
    }
    return got;
}

static int same_walk(const uint32_t *a, int na, const uint32_t *b, int nb)
{
    return na == nb && memcmp(a, b, sizeof(uint32_t) * (size_t)na) == 0;
}

int main(void)
{
    xfail_ctx c = { "player-shuffle", 0, 0, 0 };
    browse_entry_t ents[N];
    uint32_t walk_a[N], walk_b[N];
    int na, nb;

    mmio_mock_reset();
    set_usec(0);
    player_init(&g_fs);
    player_set_repeat(0);

    /* ---- 1. nothing dealt yet reads as seed 0 -------------------------- */
    stub_reset();
    xpect(&c, "no order dealt: seed reads 0, keep reads NONE",
          player_order_seed() == 0u && player_order_keep() == PLAYER_KEEP_NONE);
    player_reshuffle_with_seed(0x1234u, 0);
    xpect(&c, "reshuffle on an empty queue is a no-op",
          player_order_seed() == 0u);

    /* ---- 2. a deal records what it was dealt from ---------------------- */
    stub_reset();
    make_entries(ents, N);
    player_set_shuffle(1);
    set_usec(1000);
    player_play_queue(ents, N, 3, 0, 0);
    uint32_t seed_a = player_order_seed();
    int      keep_a = player_order_keep();
    xpect(&c, "a deal records a non-zero seed", seed_a != 0u);
    xpect(&c, "a deal at play_queue pins the picked track", keep_a == 3);
    xpect(&c, "the picked track is what plays first",
          stub_last_open_clus() == CLUS(3));
    na = walk_next(walk_a, N - 1);
    xpect(&c, "seven Nexts on eight tracks all land on a track", na == N - 1);

    /* ---- 3. the same (seed, keep) deals the same order ----------------- */
    stub_reset();
    set_usec(999999);                       /* different entropy on purpose */
    player_play_queue(ents, N, 3, 0, 0);    /* a fresh deal... */
    xpect(&c, "a second deal draws a different seed",
          player_order_seed() != seed_a);
    nb = walk_next(walk_b, N - 1);
    xpect(&c, "...and a different order",
          !same_walk(walk_a, na, walk_b, nb));

    player_jump(3);
    player_reshuffle_with_seed(seed_a, keep_a);   /* ...dealt back */
    xpect(&c, "reshuffle records the pair it was given",
          player_order_seed() == seed_a && player_order_keep() == keep_a);
    nb = walk_next(walk_b, N - 1);
    xpect(&c, "the same (seed, keep) replays the same sequence of files",
          same_walk(walk_a, na, walk_b, nb));

    /* ---- 4. the pair survives skipping: keep is the deal's, not the
     *         current track ------------------------------------------------ */
    stub_reset();
    set_usec(4242);
    player_play_queue(ents, N, 0, 0, 0);
    uint32_t seed_b = player_order_seed();
    player_next();
    player_next();
    player_next();                          /* three tracks in */
    int cur = player_queue_current();
    xpect(&c, "skipping does not re-deal or move the pin",
          player_order_seed() == seed_b && player_order_keep() == 0 && cur != 0);
    na = walk_next(walk_a, 4);              /* the four still to come */

    set_usec(777);
    player_play_queue(ents, N, cur, 0, 0);  /* what a boot rebuild looks like */
    player_reshuffle_with_seed(seed_b, 0);  /* the SAVED keep, not `cur` */
    nb = walk_next(walk_b, 4);
    xpect(&c, "replaying with the saved pin resumes the same tail of the order",
          same_walk(walk_a, na, walk_b, nb));
    xpect(&c, "...and the order ends where the original would have",
          nb == 4 && (player_next(), player_active() == 0));

    /* ---- 5. PLAYER_KEEP_QUEUE: the order is the queue order ------------ */
    stub_reset();
    set_usec(31337);
    player_play_queue(ents, N, 0, 0, 0);
    player_reshuffle_with_seed(0, PLAYER_KEEP_QUEUE);
    xpect(&c, "a queue-order deal reports KEEP_QUEUE",
          player_order_keep() == PLAYER_KEEP_QUEUE);
    na = walk_next(walk_a, N - 1);
    int in_order = (na == N - 1);
    for (int i = 0; i < na; i++) {
        if (walk_a[i] != CLUS(i + 1)) in_order = 0;
    }
    xpect(&c, "under shuffle, KEEP_QUEUE walks the queue in enqueue order",
          in_order);
    player_next();
    xpect(&c, "...and ends at the queue's end like any other order",
          player_active() == 0);

    /* ---- 6. turning shuffle on mid-queue deals with the current pinned - */
    stub_reset();
    player_set_shuffle(0);
    set_usec(555);
    player_play_queue(ents, N, 0, 0, 0);
    player_next();
    player_next();
    xpect(&c, "shuffle off: a new queue drops the old order, so none is reported",
          player_order_seed() == 0u && player_order_keep() == PLAYER_KEEP_NONE);
    player_set_shuffle(1);
    xpect(&c, "the off->on toggle pins the track that is playing",
          player_order_keep() == 2 && player_order_seed() != 0u);
    xpect(&c, "...without changing it", stub_last_open_clus() == CLUS(2));

    /* ---- 7. Repeat All's wrap is a fresh deal with a fresh seed -------- */
    stub_reset();
    player_set_repeat(1);
    set_usec(9001);
    player_play_queue(ents, N, 0, 0, 0);
    uint32_t seed_c = player_order_seed();
    for (int i = 0; i < N - 1; i++) player_next();
    uint32_t last = stub_last_open_clus();
    player_next();                          /* wraps: re-deal */
    xpect(&c, "the Repeat All wrap deals a new order under a new seed",
          player_order_seed() != seed_c && player_order_seed() != 0u &&
          player_order_keep() == PLAYER_KEEP_NONE);
    xpect(&c, "the wrap does not replay the track just heard",
          stub_last_open_clus() != last && player_active());
    player_set_repeat(0);

    /* ---- 8. a stale keep pins nothing rather than reading past the queue */
    stub_reset();
    set_usec(12);
    player_play_queue(ents, 4, 0, 0, 0);
    player_reshuffle_with_seed(seed_a, 6000);
    xpect(&c, "a keep past the queue end is recorded as NONE",
          player_order_keep() == PLAYER_KEEP_NONE && player_order_seed() == seed_a);

    return xfail_done(&c);
}
