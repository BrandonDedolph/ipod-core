/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/player/player_album_test.c — Shuffle Albums: the deal that shuffles
 * ALBUMS and plays each one whole.
 *
 * player_shuffle_test.c proves an order is a value (a pure function of the
 * (seed, keep) pair). This file proves the shape the ALBUMS mode gives that
 * order: every playable entry once per pass, each album's tracks contiguous
 * and in the index's (disc, track) order, the album you were listening to
 * first. The fixture is deliberately hostile to a naive implementation — the
 * three albums are interleaved in the queue AND their track numbers are out
 * of queue order — so a deal that only regroups, or only reorders, fails.
 *
 * As in the sibling files the order itself is never read out: every assertion
 * is about the sequence of files that would actually play, read back from the
 * cluster the fake decoder was opened on. Same fakes as player-queue; the
 * mock USEC timer is the player's only entropy, so every sequence here is
 * fixed for a given timer value rather than a coin toss.
 */

#include <stdio.h>
#include <string.h>

#include "player.h"
#include "pp5022.h"
#include "mmio_mock.h"
#include "player_test_stubs.h"
#include "../xfail.h"

#define CLUS(i) (100u + (uint32_t)(i))
#define N       12                       /* 3 albums x 4 tracks */
#define ALB_N   4                        /* tracks per album    */

/*
 * The queue, as a builder would hand it over: three albums interleaved, and
 * each album's tracks scattered out of track order.
 *
 *   idx  0  1  2  3  4  5  6  7  8  9 10 11
 *        A3 B1 C4 A1 B3 C2 A4 B2 C1 A2 B4 C3
 *
 * A correct ALBUMS pass therefore has to gather by album AND sort inside it;
 * a pass that merely kept adjacent entries together would produce twelve
 * one-track "albums".
 */
static const uint16_t FIX_ALBUM[N] = { 1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 2, 3 };
static const uint32_t FIX_TRACK[N] = { 3, 1, 4, 1, 3, 2, 4, 2, 1, 2, 4, 3 };

static fat32_t g_fs;

/* Which fixture entry a cluster is, or -1. */
static int clus_idx(uint32_t clus)
{
    if (clus < CLUS(0) || clus >= CLUS(N)) return -1;
    return (int)(clus - CLUS(0));
}

/* The fixture queue. `albums` 0 builds the same twelve tracks with no album
 * information at all (album 0), which is what a memset entry looks like. */
static void make_entries(browse_entry_t *e, int n, int albums)
{
    memset(e, 0, sizeof(browse_entry_t) * (size_t)n);
    for (int i = 0; i < n; i++) {
        snprintf(e[i].name, sizeof e[i].name, "TRACK%02d", i);
        e[i].clus = CLUS(i);
        e[i].size = 1024u * 1024u;
        if (albums) {
            e[i].album     = FIX_ALBUM[i];
            e[i].order_key = FIX_TRACK[i];
        }
    }
}

static void set_usec(uint32_t us)
{
    mmio_mock_set_read(USEC_TIMER_ADDR, us);
}

/* Press Next `n` times, recording the cluster that plays after each press.
 * Returns how many presses still had a track. */
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

/* The whole pass the listener hears: the track that is already open, then
 * every Next until the queue is used up. */
static int walk_pass(uint32_t *out, int max)
{
    out[0] = stub_last_open_clus();
    return 1 + walk_next(out + 1, max - 1);
}

static int same_walk(const uint32_t *a, int na, const uint32_t *b, int nb)
{
    return na == nb && memcmp(a, b, sizeof(uint32_t) * (size_t)na) == 0;
}

/*
 * Is `w` a valid Shuffle Albums pass over the fixture? Every entry exactly
 * once; each album's four tracks contiguous, in track order, and never
 * resumed after another album has started. Says nothing about WHICH album
 * order was dealt — that is the random part.
 */
static int albums_pass_ok(const uint32_t *w, int n)
{
    if (n != N) return 0;
    int seen[N] = { 0 };
    for (int i = 0; i < n; i++) {
        int e = clus_idx(w[i]);
        if (e < 0 || seen[e]) return 0;
        seen[e] = 1;
    }
    int done[4] = { 0 };                 /* albums 1..3, once each */
    for (int i = 0; i < n; ) {
        int a = FIX_ALBUM[clus_idx(w[i])];
        if (done[a]) return 0;           /* an album picked back up later */
        int len = 0;
        while (i + len < n && FIX_ALBUM[clus_idx(w[i + len])] == a) len++;
        if (len != ALB_N) return 0;
        for (int k = 0; k < len; k++) {
            if (FIX_TRACK[clus_idx(w[i + k])] != (uint32_t)(k + 1)) return 0;
        }
        done[a] = 1;
        i += len;
    }
    return 1;
}

/* Is `w` a permutation of the whole fixture, whatever its shape? */
static int is_permutation(const uint32_t *w, int n, int want)
{
    if (n != want) return 0;
    int seen[N] = { 0 };
    for (int i = 0; i < n; i++) {
        int e = clus_idx(w[i]);
        if (e < 0 || e >= want || seen[e]) return 0;
        seen[e] = 1;
    }
    return 1;
}

/*
 * Pump until the AUDIBLE track changes — the same helper player_queue_test.c
 * uses, and for the same reason: the hand-over is gapless, so the next track
 * is opened while the current one is still playing and stub_opens bumps well
 * before the listener hears anything new.
 */
static int pump_to_track_end(int max_pumps)
{
    uint32_t start_seq   = player_open_seq();
    int      start_stops = stub_audio_stops;
    for (int i = 0; i < max_pumps; i++) {
        player_pump();
        stub_drain(4096);
        if (player_open_seq() != start_seq ||
            (!player_active() && stub_audio_stops != start_stops)) {
            return i + 1;
        }
    }
    return max_pumps;
}

int main(void)
{
    xfail_ctx c = { "player-album", 0, 0, 0 };
    browse_entry_t ents[N];
    uint32_t walk_a[N], walk_b[N];
    int na, nb;

    mmio_mock_reset();
    set_usec(0);
    player_init(&g_fs);
    player_set_repeat(0);

    /* ---- 1. one pass plays every album whole, then the queue ends ------ */
    stub_reset();
    make_entries(ents, N, 1);
    player_set_shuffle(PLAYER_SHUFFLE_ALBUMS);
    set_usec(1000);
    player_play_queue(ents, N, 3, 0, 0);          /* start on A1 */
    xpect(&c, "the picked track is what plays first",
          stub_last_open_clus() == CLUS(3));
    na = walk_pass(walk_a, N);
    xpect(&c, "a pass plays every entry once, each album whole and in "
              "track order", albums_pass_ok(walk_a, na));
    xpect(&c, "the album the pick belongs to is dealt first",
          na == N && FIX_ALBUM[clus_idx(walk_a[0])] == 1);
    player_next();
    xpect(&c, "Repeat Off ends the queue after the last album",
          player_active() == 0);
    xpect(&c, "...having opened each entry exactly once", stub_opens == N);

    /* ---- 2. keep pins the ALBUM, not just the track -------------------- */
    stub_reset();
    set_usec(2200);
    player_play_queue(ents, N, 7, 0, 0);          /* start on B2 */
    xpect(&c, "a mid-album start records its own index as the pin",
          player_order_keep() == 7 && stub_last_open_clus() == CLUS(7));
    na = walk_next(walk_a, N - 1);
    xpect(&c, "the rest of the current album comes first, in track order",
          na == N - 2 && walk_a[0] == CLUS(4) /* B3 */ &&
          walk_a[1] == CLUS(10) /* B4 */);
    {
        /* The pass is short by the tracks the pin skipped past (B1 sits
         * BEFORE B2 in its album, as it would under no shuffle at all); put
         * them back and the whole pass is a valid albums pass. */
        uint32_t tail[N];
        tail[0] = CLUS(1); tail[1] = CLUS(7);     /* B1, B2 — already heard */
        for (int i = 0; i < na; i++) tail[i + 2] = walk_a[i];
        xpect(&c, "...and the albums after it are whole and in track order",
              albums_pass_ok(tail, na + 2));
    }
    stub_reset();
    set_usec(2200);
    player_play_queue(ents, N, 7, 0, 0);
    player_prev();
    xpect(&c, "Prev from a mid-album track is the track before it in the album",
          stub_last_open_clus() == CLUS(1) /* B1 */);

    /* ---- 3. the same (seed, keep) deals the same album order ----------- */
    stub_reset();
    set_usec(4242);
    player_play_queue(ents, N, 3, 0, 0);          /* A1: a whole pass follows */
    uint32_t seed_a = player_order_seed();
    xpect(&c, "an albums deal records a non-zero seed and its pin",
          seed_a != 0u && player_order_keep() == 3);
    set_usec(99999);
    player_play_queue(ents, N, 3, 0, 0);          /* different entropy */
    xpect(&c, "a second deal draws a different seed",
          player_order_seed() != seed_a);

    /*
     * Two seeds spelled out rather than taken from the timer: with three
     * albums and one of them pinned there are only two orders to land on, so
     * "these two deals differ" has to name seeds that actually do — and
     * naming them also keeps this case independent of how many deals the
     * cases above happened to make (the LCG carries its state between them).
     */
    player_jump(3);
    player_reshuffle_with_seed(0x1111u, 3);
    na = walk_pass(walk_a, N);
    player_jump(3);
    player_reshuffle_with_seed(0x2222u, 3);
    nb = walk_pass(walk_b, N);
    xpect(&c, "both seeds deal a whole, well-formed pass",
          albums_pass_ok(walk_a, na) && albums_pass_ok(walk_b, nb));
    xpect(&c, "a different seed deals a different album order",
          !same_walk(walk_a, na, walk_b, nb));

    player_jump(3);
    player_reshuffle_with_seed(0x1111u, 3);
    nb = walk_pass(walk_b, N);
    xpect(&c, "the same (seed, keep) replays the same album order",
          same_walk(walk_a, na, walk_b, nb));

    /* ---- 4. a single-album queue just plays in order ------------------- */
    {
        browse_entry_t one[ALB_N];
        memset(one, 0, sizeof one);
        for (int i = 0; i < ALB_N; i++) {
            snprintf(one[i].name, sizeof one[i].name, "ONE%02d", i);
            one[i].clus      = CLUS(i);
            one[i].size      = 1024u * 1024u;
            one[i].album     = 1;
            one[i].order_key = (uint32_t)(ALB_N - i);   /* 4,3,2,1 in queue */
        }
        stub_reset();
        set_usec(7777);
        player_play_queue(one, ALB_N, ALB_N - 1, 0, 0); /* start on track 1 */
        int in_order = (stub_last_open_clus() == CLUS(ALB_N - 1));
        na = walk_next(walk_a, ALB_N - 1);
        for (int i = 0; i < na; i++) {
            if (walk_a[i] != CLUS(ALB_N - 2 - i)) in_order = 0;
        }
        xpect(&c, "one album plays in track order, whatever the queue order",
              na == ALB_N - 1 && in_order);
        player_next();
        xpect(&c, "...and ends under Repeat Off", player_active() == 0);

        /* Repeat All over one album loops it in order — the track-level
         * fallback swap must NOT fire here, or the loop would start on a
         * random track of the album. */
        stub_reset();
        player_set_repeat(1);
        set_usec(8888);
        player_play_queue(one, ALB_N, ALB_N - 1, 0, 0);
        for (int i = 0; i < ALB_N - 1; i++) player_next();
        player_next();                                 /* the wrap */
        int loop_ok = (stub_last_open_clus() == CLUS(ALB_N - 1));
        na = walk_next(walk_a, ALB_N - 1);
        for (int i = 0; i < na; i++) {
            if (walk_a[i] != CLUS(ALB_N - 2 - i)) loop_ok = 0;
        }
        xpect(&c, "Repeat All on one album replays it in track order",
              na == ALB_N - 1 && loop_ok);
        player_set_repeat(0);
    }

    /* ---- 5. Repeat All wraps to a DIFFERENT album ---------------------- */
    stub_reset();
    player_set_repeat(1);
    set_usec(31337);
    player_play_queue(ents, N, 3, 0, 0);          /* A1 */
    na = walk_pass(walk_a, N);
    xpect(&c, "the first pass is a valid albums pass", albums_pass_ok(walk_a, na));
    player_next();                                     /* the wrap */
    xpect(&c, "the wrap does not start the album that just finished",
          player_active() &&
          FIX_ALBUM[clus_idx(stub_last_open_clus())] !=
              FIX_ALBUM[clus_idx(walk_a[N - 1])]);
    xpect(&c, "...and it starts that album at its first track",
          FIX_TRACK[clus_idx(stub_last_open_clus())] == 1u);
    nb = walk_pass(walk_b, N);
    xpect(&c, "the second pass is a valid albums pass too",
          albums_pass_ok(walk_b, nb));
    player_set_repeat(0);

    /* ---- 6. no album information: still a plain permutation ------------ */
    stub_reset();
    make_entries(ents, N, 0);                          /* album 0 throughout */
    set_usec(606);
    player_play_queue(ents, N, 0, 0, 0);
    na = walk_pass(walk_a, N);
    xpect(&c, "entries with no album are singletons, so the pass is still a "
              "permutation of the whole queue", is_permutation(walk_a, na, N));
    player_next();
    xpect(&c, "...and it ends after one pass", player_active() == 0);

    /* ---- 7. PLAYER_KEEP_QUEUE is mode-independent (Shuffle Songs) ------ */
    stub_reset();
    make_entries(ents, N, 1);
    set_usec(4711);
    player_play_queue(ents, N, 0, 0, 0);
    player_reshuffle_with_seed(0, PLAYER_KEEP_QUEUE);
    xpect(&c, "a queue-order deal under ALBUMS still reports KEEP_QUEUE",
          player_order_keep() == PLAYER_KEEP_QUEUE);
    na = walk_next(walk_a, N - 1);
    {
        int in_order = (na == N - 1);
        for (int i = 0; i < na; i++) {
            if (walk_a[i] != CLUS(i + 1)) in_order = 0;
        }
        xpect(&c, "Shuffle Songs' own order is not regrouped by album",
              in_order);
    }

    /* ---- 8. changing mode mid-play ------------------------------------- */
    stub_reset();
    player_set_shuffle(PLAYER_SHUFFLE_SONGS);
    set_usec(1234);
    player_play_queue(ents, N, 7, 0, 0);               /* B2 under SONGS */
    uint32_t seed_s = player_order_seed();
    player_set_shuffle(PLAYER_SHUFFLE_ALBUMS);
    xpect(&c, "SONGS->ALBUMS re-deals, pinning the track that is playing",
          player_order_seed() != seed_s && player_order_keep() == 7);
    xpect(&c, "...without reopening or changing it",
          stub_last_open_clus() == CLUS(7) && stub_opens == 1);
    na = walk_next(walk_a, 2);
    xpect(&c, "...and the rest of its album plays before any other",
          na == 2 && walk_a[0] == CLUS(4) && walk_a[1] == CLUS(10));

    uint32_t seed_al = player_order_seed();
    player_set_shuffle(PLAYER_SHUFFLE_ALBUMS);
    xpect(&c, "re-pushing the SAME mode is a no-op (settings_apply does it "
              "on every volume tick)", player_order_seed() == seed_al);
    set_usec(4321);
    player_set_shuffle(PLAYER_SHUFFLE_SONGS);
    xpect(&c, "ALBUMS->SONGS re-deals and still keeps the current track",
          player_order_seed() != seed_al &&
          player_order_keep() == player_queue_current());
    player_set_shuffle(PLAYER_SHUFFLE_ALBUMS);

    /* ---- 9. the sort keys: unbound last, ties by queue order ----------- */
    {
        browse_entry_t k[ALB_N];
        memset(k, 0, sizeof k);
        for (int i = 0; i < ALB_N; i++) {
            snprintf(k[i].name, sizeof k[i].name, "KEY%02d", i);
            k[i].clus  = CLUS(i);
            k[i].size  = 1024u * 1024u;
            k[i].album = 1;
        }
        k[0].order_key = 0xFFFFFFFFu;    /* the index does not know this file */
        k[1].order_key = 2;
        k[2].order_key = 2;              /* a tie with k[1] */
        k[3].order_key = 1;
        stub_reset();
        set_usec(515);
        player_play_queue(k, ALB_N, 3, 0, 0);
        na = walk_next(walk_a, ALB_N - 1);
        xpect(&c, "an unbound order_key sorts last and equal keys keep queue "
                  "order",
              na == ALB_N - 1 && stub_last_open_clus() == CLUS(0) &&
              walk_a[0] == CLUS(1) && walk_a[1] == CLUS(2) &&
              walk_a[2] == CLUS(0));
    }

    /* ---- 10. folders, and an empty queue ------------------------------- */
    {
        browse_entry_t d[5];
        memset(d, 0, sizeof d);
        for (int i = 0; i < 5; i++) {
            snprintf(d[i].name, sizeof d[i].name, "ROW%02d", i);
            d[i].clus      = CLUS(i);
            d[i].size      = 1024u * 1024u;
            d[i].album     = 1;
            d[i].order_key = (uint32_t)i;
        }
        d[0].is_dir = 1;                 /* a subdirectory row */
        d[3].is_dir = 1;
        stub_reset();
        set_usec(99);
        player_play_queue(d, 5, 1, 0, 0);
        na = walk_pass(walk_a, N);
        int only_files = (na == 3);
        for (int i = 0; i < na; i++) {
            if (walk_a[i] == CLUS(0) || walk_a[i] == CLUS(3)) only_files = 0;
        }
        xpect(&c, "folders never enter the album order", only_files);
    }
    stub_reset();
    player_queue_begin();
    player_queue_commit(0);              /* nothing added: still empty */
    player_set_shuffle(PLAYER_SHUFFLE_SONGS);
    player_set_shuffle(PLAYER_SHUFFLE_ALBUMS);
    xpect(&c, "ALBUMS over an empty queue deals nothing and does not fault",
          player_order_seed() == 0u && player_active() == 0);

    /* ---- 11. the prefetch takes the same successor --------------------- */
    stub_reset();
    make_entries(ents, N, 1);
    stub_set_track_frames(4096);
    set_usec(24680);
    player_play_queue(ents, N, 3, 0, 0);               /* A1 */
    {
        uint32_t heard[ALB_N + 1];
        int got = 0;
        heard[got++] = stub_last_open_clus();
        for (int i = 0; i < ALB_N && player_active(); i++) {
            pump_to_track_end(200000);
            if (!player_active()) break;
            heard[got++] = stub_last_open_clus();
        }
        int ok = (got == ALB_N + 1);
        for (int i = 0; i < ALB_N && ok; i++) {
            if (FIX_ALBUM[clus_idx(heard[i])] != 1 ||
                FIX_TRACK[clus_idx(heard[i])] != (uint32_t)(i + 1)) ok = 0;
        }
        xpect(&c, "auto-advance plays the album out in track order", ok);
        xpect(&c, "...and crosses into the NEXT album at its first track",
              got == ALB_N + 1 && FIX_ALBUM[clus_idx(heard[ALB_N])] != 1 &&
              FIX_TRACK[clus_idx(heard[ALB_N])] == 1u);
    }

    return xfail_done(&c);
}
