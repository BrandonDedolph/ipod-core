/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/player/player_queue_test.c — the queue / auto-advance / repeat /
 * shuffle logic of core/player/player.c, host-side.
 *
 * player.c is 753 lines and had zero tests. The decode path needs real audio
 * and real hardware, but the part that decides WHICH TRACK PLAYS NEXT is pure
 * bookkeeping over an array, and it is the part users notice when it is wrong:
 * a queue that stops one track early, a Repeat All that doesn't wrap, a
 * Shuffle that replays the song you just heard (or never ends), a Prev that
 * restarts instead of going back, a broken file that stalls playback instead
 * of being skipped.
 *
 * The real player.c is compiled unmodified; its disk, codec and DAC are
 * replaced by the controllable fakes in player_test_stubs.c. Which entry the
 * player picked is read back from the CLUSTER the fake decoder was opened on,
 * not from an index accessor — so the assertions are about the file that would
 * actually play.
 *
 * Note on what is NOT covered here: player_advance(), shuffle_build() and
 * successor()/predecessor() are `static` in player.c and therefore not
 * directly reachable. They are exercised INDIRECTLY, through player_pump()
 * running a track to end-of-stream (which is how auto-advance happens in the
 * firmware) and through player_next/player_prev. The shuffle order itself is
 * never read out: the tests observe which files play and in what sequence,
 * which is the contract the listener experiences. The decode/ring path,
 * load_folder_art() and player_probe_meta() need real file bytes and are out
 * of scope for this test.
 */

#include <stdio.h>
#include <string.h>

#include "player.h"
#include "hal.h"                 /* hal_audio_close: the UI's edge action */
#include "pp5022.h"
#include "mmio_mock.h"
#include "player_test_stubs.h"
#include "../xfail.h"

/* Clusters are the identity of a queue entry throughout this test. */
#define CLUS(i) (100u + (uint32_t)(i))

static fat32_t g_fs;

/* Build an n-entry queue of playable files; `dir_mask` bit i marks entry i as
 * a subdirectory (which the player must skip when choosing what to play). */
static void make_entries(browse_entry_t *e, int n, unsigned dir_mask)
{
    memset(e, 0, sizeof(browse_entry_t) * (size_t)n);
    for (int i = 0; i < n; i++) {
        snprintf(e[i].name, sizeof e[i].name, "TRACK%02d", i);
        e[i].clus   = CLUS(i);
        e[i].size   = 1024u * 1024u;
        e[i].fmt    = 0;                       /* FLAC */
        e[i].is_dir = (dir_mask >> i) & 1u;
    }
}

/* Advance the virtual microsecond clock the player reads for elapsed time. */
static void set_usec(uint32_t us)
{
    mmio_mock_set_read(USEC_TIMER_ADDR, us);
}

/* Run the player until the current track ends and auto-advance has happened,
 * or until we give up. Returns the number of pumps it took. player_pump() is
 * the firmware's main-loop call, so this is exactly how a track ends on the
 * device. */
/*
 * Pump until the AUDIBLE track changes.
 *
 * Waiting on stub_opens would be wrong now that hand-over is gapless: the next
 * track is opened and primed WHILE the current one is still playing, so the
 * open count bumps well before the presentation switches, and the caller would
 * sample player_queue_current() too early and see the old index. The presented
 * index is the honest signal for "the listener now hears a different track".
 */
static int pump_to_track_end(int max_pumps)
{
    uint32_t start_seq = player_open_seq();
    int      start_stops = stub_audio_stops;
    for (int i = 0; i < max_pumps; i++) {
        player_pump();
        stub_drain(4096);          /* stand in for the DMA-completion ISR */
        if (player_open_seq() != start_seq ||
            (!player_active() && stub_audio_stops != start_stops)) {
            return i + 1;
        }
    }
    return max_pumps;
}

int main(void)
{
    xfail_ctx c = { "player-queue", 0, 0, 0 };
    browse_entry_t ents[8];

    mmio_mock_reset();
    set_usec(0);
    player_init(&g_fs);

    /* ---- 1. play_queue starts at the requested entry ------------------ */
    stub_reset();
    make_entries(ents, 4, 0);
    player_play_queue(ents, 4, 2, 0, 0);
    xpect(&c, "play_queue opens the entry it was asked to start at",
          stub_last_open_clus() == CLUS(2));
    xpect(&c, "play_queue reports the queue length", player_queue_len() == 4);
    xpect(&c, "play_queue reports the current index", player_queue_current() == 2);
    xpect(&c, "play_queue leaves the player active", player_active() == 1);
    xpect(&c, "play_queue starts the DAC", stub_audio_running == 1);
    xpect(&c, "play_queue reads the track's tags once", stub_meta_reads == 1);
    xpect(&c, "the queue exposes the entry names",
          strcmp(player_queue_name(2), "TRACK02") == 0);
    xpect(&c, "out-of-range queue names are empty, not out-of-bounds reads",
          player_queue_name(-1)[0] == '\0' &&
          player_queue_name(99)[0] == '\0');

    /* ---- 2. auto-advance walks the queue in order --------------------- */
    stub_reset();
    stub_set_track_frames(4096);
    make_entries(ents, 4, 0);
    player_play_queue(ents, 4, 0, 0, 0);
    xpect(&c, "advance: starts on entry 0", stub_last_open_clus() == CLUS(0));
    pump_to_track_end(2000);
    xpect(&c, "advance: end of track 0 moves to track 1",
          stub_last_open_clus() == CLUS(1) && player_queue_current() == 1);
    pump_to_track_end(2000);
    xpect(&c, "advance: end of track 1 moves to track 2",
          stub_last_open_clus() == CLUS(2) && player_queue_current() == 2);

    /* ---- 3. Repeat OFF stops at the end of the queue ------------------ */
    stub_reset();
    player_set_repeat(0);
    player_set_shuffle(0);
    make_entries(ents, 2, 0);
    player_play_queue(ents, 2, 1, 0, 0);      /* start on the LAST entry */
    pump_to_track_end(2000);
    xpect(&c, "repeat off: the queue ends idle, it does not wrap",
          player_active() == 0);
    xpect(&c, "repeat off: the DAC is stopped at the end of the queue",
          stub_audio_running == 0);

    /* ---- 4. Repeat ALL wraps to the first playable entry -------------- */
    stub_reset();
    player_set_repeat(1);
    make_entries(ents, 3, 0);
    player_play_queue(ents, 3, 2, 0, 0);      /* last entry */
    pump_to_track_end(2000);
    xpect(&c, "repeat all: wraps from the last entry to the first",
          player_active() == 1 && stub_last_open_clus() == CLUS(0) &&
          player_queue_current() == 0);

    /* ---- 5. Repeat ONE replays the same track ------------------------- */
    stub_reset();
    player_set_repeat(2);
    make_entries(ents, 3, 0);
    player_play_queue(ents, 3, 1, 0, 0);
    int opens_before = stub_opens;
    pump_to_track_end(2000);
    xpect(&c, "repeat one: replays the SAME entry",
          player_queue_current() == 1 && stub_last_open_clus() == CLUS(1));
    xpect(&c, "repeat one: really re-opened the file (not a no-op)",
          stub_opens == opens_before + 1);

    /* Repeat One must not spin forever on a track that stops opening: the
     * player's own comment says the replay path returns early precisely so a
     * lone broken track cannot loop. Break the track, then end it. */
    stub_break_cluster(CLUS(1));
    int pumps = pump_to_track_end(2000);
    xpect(&c, "repeat one: a track that stops opening does not spin forever",
          pumps < 2000);
    player_set_repeat(0);

    /* ---- 6. folders in the queue are never played --------------------- */
    stub_reset();
    player_set_repeat(0);
    make_entries(ents, 4, 0x2u);              /* entry 1 is a directory */
    player_play_queue(ents, 4, 0, 0, 0);
    pump_to_track_end(2000);
    xpect(&c, "a subdirectory entry is skipped, not opened",
          stub_last_open_clus() == CLUS(2) && player_queue_current() == 2);
    xpect(&c, "the queue still reports the folder as a folder",
          player_queue_is_dir(1) == 1 && player_queue_is_dir(0) == 0);

    /* ---- 7. a broken file is skipped, and a fully broken queue idles --- */
    stub_reset();
    make_entries(ents, 4, 0);
    stub_break_cluster(CLUS(0));
    stub_break_cluster(CLUS(1));
    player_play_queue(ents, 4, 0, 0, 0);
    xpect(&c, "a broken first track is skipped to the first that opens",
          player_active() == 1 && stub_last_open_clus() == CLUS(2));
    xpect(&c, "skipping tried each broken track exactly once",
          stub_open_attempts == 3);

    stub_reset();
    make_entries(ents, 3, 0);
    stub_break_cluster(CLUS(0));
    stub_break_cluster(CLUS(1));
    stub_break_cluster(CLUS(2));
    player_play_queue(ents, 3, 0, 0, 0);
    xpect(&c, "a queue where nothing opens ends idle rather than looping",
          player_active() == 0);
    xpect(&c, "a queue where nothing opens is bounded by the queue length",
          stub_open_attempts <= 3 + 1);

    /* ---- 8. player_jump ----------------------------------------------- */
    stub_reset();
    make_entries(ents, 4, 0x4u);              /* entry 2 is a directory */
    player_play_queue(ents, 4, 0, 0, 0);
    player_jump(3);
    xpect(&c, "jump plays the requested entry",
          player_queue_current() == 3 && stub_last_open_clus() == CLUS(3));
    player_jump(2);
    xpect(&c, "jump to a folder is a no-op", player_queue_current() == 3);
    player_jump(-1);
    player_jump(99);
    xpect(&c, "jump out of range is a no-op", player_queue_current() == 3);

    /* ---- 9. next / prev ------------------------------------------------ */
    stub_reset();
    player_set_repeat(0);
    player_set_shuffle(0);
    make_entries(ents, 4, 0);
    player_play_queue(ents, 4, 1, 0, 0);
    player_next();
    xpect(&c, "next moves forward one entry",
          player_queue_current() == 2 && stub_last_open_clus() == CLUS(2));

    /* Prev within the first ~3 s of a track goes BACK; past it, restarts. */
    set_usec(0);
    player_play_queue(ents, 4, 2, 0, 0);
    set_usec(1000000u);                        /* 1 s in */
    player_prev();
    xpect(&c, "prev early in a track goes to the previous entry",
          player_queue_current() == 1 && stub_last_open_clus() == CLUS(1));

    set_usec(0);
    player_play_queue(ents, 4, 2, 0, 0);
    set_usec(9000000u);                        /* 9 s in */
    player_prev();
    xpect(&c, "prev late in a track restarts the current one",
          player_queue_current() == 2 && stub_last_open_clus() == CLUS(2));

    /* Prev wraps at the start of the queue even with Repeat off — the
     * familiar iPod behaviour player.h documents. */
    set_usec(0);
    player_play_queue(ents, 4, 0, 0, 0);
    set_usec(500000u);
    player_prev();
    xpect(&c, "prev at the head of the queue wraps to the tail",
          player_queue_current() == 3);

    /*
     * Next at the tail with Repeat OFF ENDS the queue: the user asked to leave
     * the last track and there is nowhere to go, so playback finishes exactly
     * as if it had played out. It must NOT wrap.
     *
     * This assertion previously demanded the opposite ("keeps playing"), which
     * encoded an overcorrection rather than the intent. The original bug was
     * that player_next() stopped the DAC and cleared g_pl_active BEFORE
     * looking for a successor, so Next on the last track killed the transport
     * with every handler gated on player_active() — no way back into the
     * queue. Choosing the successor first is what fixed that. Making the press
     * do nothing at all was a separate mistake: on device the button was
     * simply dead on the final track (reported 2026-07-27).
     *
     * Ending here is safe precisely because we only reach it having already
     * established there is no next track, and it routes through the same
     * end-of-queue signal, so the UI tears down the dead player views and
     * drops the saved resume position.
     */
    stub_reset();
    player_set_repeat(0);
    make_entries(ents, 3, 0);
    player_play_queue(ents, 3, 2, 0, 0);
    uint32_t end_before = player_end_seq();
    player_next();
    xpect(&c, "next past the last entry with repeat off ends playback",
          player_active() == 0);
    xpect(&c, "next past the last entry counts as the queue ending",
          player_end_seq() == end_before + 1);
    xpect(&c, "next past the last entry does NOT wrap to the first",
          player_queue_current() == 2);

    /* ...and with Repeat ALL it wraps. */
    stub_reset();
    player_set_repeat(1);
    player_play_queue(ents, 3, 2, 0, 0);
    player_next();
    xpect(&c, "next past the last entry with repeat all wraps to the first",
          player_active() == 1 && player_queue_current() == 0);
    player_set_repeat(0);

    /* ---- 10. shuffle --------------------------------------------------- */
    /*
     * Shuffle is a PERMUTATION of the playable entries, walked in order — not
     * a fresh random draw per advance. The assertions that used to live here
     * passed against the old draw-per-advance code, which means they encoded
     * its bug: they pressed Next 60 times on a 6-track queue with Repeat OFF
     * and required every press to land somewhere new. A queue that survives
     * 60 Nexts with Repeat off is precisely a queue that can never end, and
     * "shuffle with a single playable entry REPLAYS it" on Next with Repeat
     * off is the same fault at n=1. Everything below is only true of a real
     * order: each track once, then the end; Prev is the track just heard.
     */

    /* 10a. Auto-advance (player_pump running each track out — how an album
     * plays on the device) visits every playable entry exactly once, and
     * THEN the queue ends with the same signal a plain queue gives. */
    stub_reset();
    stub_set_track_frames(4096);
    player_set_repeat(0);
    player_set_shuffle(1);
    make_entries(ents, 8, 0);
    player_play_queue(ents, 8, 3, 0, 0);
    xpect(&c, "shuffle: playback starts on the entry that was picked",
          player_queue_current() == 3 && stub_last_open_clus() == CLUS(3));
    {
        int plays[8] = { 0 };
        int walked   = 1;
        plays[3]++;
        for (int k = 1; k < 8; k++) {
            pump_to_track_end(2000);
            int cur = player_queue_current();
            if (!player_active() || cur < 0 || cur >= 8 ||
                stub_last_open_clus() != CLUS(cur)) {
                walked = 0;
                break;
            }
            plays[cur]++;
        }
        xpect(&c, "shuffle: seven auto-advances stay inside the queue and active",
              walked);
        int once = 1;
        for (int i = 0; i < 8; i++) {
            if (plays[i] != 1) once = 0;
        }
        xpect(&c, "shuffle: every track played exactly once before any repeat",
              once);
        uint32_t ends0 = player_end_seq();
        pump_to_track_end(2000);
        xpect(&c, "shuffle + repeat off: the queue ENDS after the last unplayed track",
              player_active() == 0 && stub_audio_running == 0);
        xpect(&c, "shuffle + repeat off: the end counts as the queue finishing",
              player_end_seq() == ends0 + 1);
        xpect(&c, "shuffle + repeat off: nothing was opened past the end",
              stub_opens == 8);
    }

    /* 10b. Repeat All: the second time round is a NEW permutation (not the
     * same sequence forever), still each track once, and the seam never
     * plays the same track twice in a row. The RNG is an LCG over a mocked
     * timer, so this is deterministic, not a coin flip. */
    stub_reset();
    player_set_repeat(1);
    make_entries(ents, 8, 0);
    player_play_queue(ents, 8, 0, 0, 0);
    {
        int cyc1[8], cyc2[8];
        cyc1[0] = 0;
        for (int k = 1; k < 8; k++) {
            pump_to_track_end(2000);
            cyc1[k] = player_queue_current();
        }
        for (int k = 0; k < 8; k++) {
            pump_to_track_end(2000);
            cyc2[k] = player_queue_current();
        }
        xpect(&c, "shuffle + repeat all: still playing after two full passes",
              player_active() == 1);
        int plays[8] = { 0 };
        int perm = 1;
        for (int k = 0; k < 8; k++) {
            if (cyc2[k] < 0 || cyc2[k] >= 8) { perm = 0; break; }
            plays[cyc2[k]]++;
        }
        for (int i = 0; i < 8 && perm; i++) {
            if (plays[i] != 1) perm = 0;
        }
        xpect(&c, "shuffle + repeat all: the second pass is again each track once",
              perm);
        xpect(&c, "shuffle + repeat all: the wrap does not replay the track just heard",
              cyc2[0] != cyc1[7]);
        xpect(&c, "shuffle + repeat all: the second pass is a different order",
              memcmp(cyc1, cyc2, sizeof cyc1) != 0);
    }

    /* 10c. The same contract driven by the Next button: n-1 presses land on
     * n-1 distinct unplayed tracks, the n-th ENDS the queue (Repeat off) —
     * exactly what test 9 requires of Next in plain order. */
    stub_reset();
    player_set_repeat(0);
    make_entries(ents, 6, 0);
    player_play_queue(ents, 6, 0, 0, 0);
    {
        int seen[6] = { 0 };
        int in_range = 1;
        seen[0] = 1;
        for (int i = 0; i < 5; i++) {
            set_usec((uint32_t)(i * 7919));
            player_next();
            int cur = player_queue_current();
            if (cur < 0 || cur >= 6 || !player_active()) {
                in_range = 0;
                break;
            }
            seen[cur]++;
        }
        xpect(&c, "shuffle: Next only ever selects entries inside the queue",
              in_range);
        int distinct = 0;
        for (int i = 0; i < 6; i++) {
            distinct += (seen[i] == 1);
        }
        xpect(&c, "shuffle: five Nexts on six tracks reach the other five, once each",
              distinct == 6);
        uint32_t ends1 = player_end_seq();
        player_next();
        xpect(&c, "shuffle: Next after the last unplayed track ends the queue",
              player_active() == 0 && player_end_seq() == ends1 + 1);
    }

    /* 10d. Prev is the track just heard, and the order is stable under it:
     * Next after Prev returns to the same track, not a new draw. */
    stub_reset();
    set_usec(0);
    make_entries(ents, 6, 0);
    player_play_queue(ents, 6, 0, 0, 0);
    {
        set_usec(1000000u);
        player_next();
        int a = player_queue_current();
        xpect(&c, "shuffle: the first Next leaves the starting track", a != 0);
        player_prev();                          /* 0 s into `a` */
        xpect(&c, "shuffle: Prev returns to the track just played, not a random one",
              player_queue_current() == 0 && stub_last_open_clus() == CLUS(0));
        player_next();
        xpect(&c, "shuffle: Next after Prev goes forward to the same track again",
              player_queue_current() == a && stub_last_open_clus() == CLUS(a));

        pump_to_track_end(2000);                /* auto-advance to a third track */
        int b = player_queue_current();
        xpect(&c, "shuffle: auto-advance moves to a third distinct track",
              b != a && b != 0);
        player_prev();
        xpect(&c, "shuffle: Prev after an auto-advance is the track that just ended",
              player_queue_current() == a);

        set_usec(9000000u + 1000000u);          /* well past 3 s into `a` */
        int opens_a = stub_opens;
        player_prev();
        xpect(&c, "shuffle: Prev late in a track restarts it, as in plain order",
              player_queue_current() == a && stub_opens == opens_a + 1);
    }

    /* Prev at the HEAD of the order wraps to its tail (as prev_playable wraps
     * to the last entry); Next from the tail with Repeat off then ends. */
    stub_reset();
    set_usec(0);
    player_play_queue(ents, 6, 2, 0, 0);
    {
        player_prev();
        int tail = player_queue_current();
        xpect(&c, "shuffle: Prev at the head of the order wraps to its tail",
              tail != 2 && player_active() == 1);
        uint32_t ends2 = player_end_seq();
        player_next();
        xpect(&c, "shuffle: the wrapped-to tail really is the last of the order",
              player_active() == 0 && player_end_seq() == ends2 + 1);
    }

    /* 10e. Shuffle OFF returns to plain queue order from wherever we are. */
    stub_reset();
    player_set_repeat(1);
    set_usec(0);
    make_entries(ents, 6, 0);
    player_play_queue(ents, 6, 0, 0, 0);
    {
        player_next();
        int x = player_queue_current();
        player_set_shuffle(0);
        player_next();
        xpect(&c, "shuffle off: the next track is the queue's next entry",
              player_queue_current() == (x + 1) % 6 &&
              stub_last_open_clus() == CLUS((x + 1) % 6));
        player_next();
        xpect(&c, "shuffle off: and so is the one after",
              player_queue_current() == (x + 2) % 6);
        player_prev();                          /* 0 s in: goes back */
        xpect(&c, "shuffle off: Prev is the queue's previous entry",
              player_queue_current() == (x + 1) % 6);
    }

    /* 10f. Shuffle ON mid-playback keeps the current track playing
     * untouched and shuffles only what comes after it; re-applying the
     * setting while already on does not re-deal (the UI pushes every
     * setting on every change, volume included). */
    stub_reset();
    player_set_repeat(0);
    player_set_shuffle(0);
    set_usec(0);
    make_entries(ents, 6, 0);
    player_play_queue(ents, 6, 2, 0, 0);
    {
        set_usec(2000000u);
        int      opens0 = stub_opens;
        uint32_t seq0   = player_open_seq();
        player_set_shuffle(1);
        xpect(&c, "shuffle on mid-track: the current track keeps playing",
              player_queue_current() == 2 && player_active() == 1 &&
              stub_opens == opens0 && player_open_seq() == seq0 &&
              stub_audio_running == 1);
        xpect(&c, "shuffle on mid-track: the elapsed clock is not reset",
              player_elapsed_s() == 2u);
        player_next();
        int a = player_queue_current();
        player_prev();
        player_set_shuffle(1);                  /* already on: must not re-deal */
        player_next();
        xpect(&c, "shuffle on while already on: the order is not re-dealt",
              player_queue_current() == a);

        int plays[6] = { 0 };
        plays[2]++;
        plays[a]++;
        for (int k = 0; k < 4; k++) {
            pump_to_track_end(2000);
            int cur = player_queue_current();
            if (cur >= 0 && cur < 6) plays[cur]++;
        }
        int once = 1;
        for (int i = 0; i < 6; i++) {
            if (plays[i] != 1) once = 0;
        }
        xpect(&c, "shuffle on mid-track: the rest plays once each, current excluded",
              once && player_active() == 1);
        pump_to_track_end(2000);
        xpect(&c, "shuffle on mid-track: then the queue ends",
              player_active() == 0);
    }

    /* 10g. Folders are never in the order: three passes under Repeat All
     * over a queue with folders interleaved, driven by Next. Each pass is a
     * permutation of the playable entries, no pass ever lands on a folder,
     * and no two consecutive picks are the same track. */
    stub_reset();
    player_set_repeat(1);
    make_entries(ents, 7, 0x2Au);             /* entries 1, 3, 5 are folders */
    player_play_queue(ents, 7, 0, 0, 0);
    {
        int picks[12];
        int picked_folder = 0, immediate_repeat = 0;
        picks[0] = 0;
        for (int i = 1; i < 12; i++) {
            set_usec((uint32_t)(i * 104729));
            player_next();
            picks[i] = player_queue_current();
            if (player_queue_is_dir(picks[i])) picked_folder = 1;
            if (picks[i] == picks[i - 1])      immediate_repeat = 1;
        }
        xpect(&c, "shuffle never selects a subdirectory", !picked_folder);
        xpect(&c, "shuffle never plays the same track twice running, even at a wrap",
              !immediate_repeat);
        int each_pass = 1;
        for (int p = 0; p < 3; p++) {
            int plays[7] = { 0 };
            for (int k = 0; k < 4; k++) {
                plays[picks[p * 4 + k]]++;
            }
            if (plays[0] != 1 || plays[2] != 1 || plays[4] != 1 || plays[6] != 1) {
                each_pass = 0;
            }
        }
        xpect(&c, "shuffle: every pass over a queue with folders is each track once",
              each_pass);
    }

    /* 10h. One playable entry: Repeat off ends on Next (the one track WAS the
     * whole order — replaying it here was the never-ending-queue bug at n=1);
     * Repeat all replays it, bounded, on both Next and auto-advance. */
    stub_reset();
    player_set_repeat(0);
    make_entries(ents, 3, 0x6u);              /* only entry 0 is playable */
    player_play_queue(ents, 3, 0, 0, 0);
    {
        uint32_t ends3 = player_end_seq();
        player_next();
        xpect(&c, "shuffle, one playable, repeat off: Next ends the queue",
              player_active() == 0 && player_end_seq() == ends3 + 1 &&
              player_queue_current() == 0);
    }
    stub_reset();
    player_set_repeat(1);
    set_usec(0);
    player_play_queue(ents, 3, 0, 0, 0);
    {
        int opens0 = stub_opens;
        player_next();
        xpect(&c, "shuffle, one playable, repeat all: Next replays it, bounded",
              player_queue_current() == 0 && player_active() == 1 &&
              stub_opens == opens0 + 1);
        uint32_t seq0 = player_open_seq();
        pump_to_track_end(2000);
        xpect(&c, "shuffle, one playable, repeat all: auto-advance replays it",
              player_queue_current() == 0 && player_active() == 1 &&
              player_open_seq() == seq0 + 1);
        player_prev();
        xpect(&c, "shuffle, one playable: Prev wraps onto the same track",
              player_queue_current() == 0 && player_active() == 1);
    }

    /* 10i. No playable entry at all, and an empty queue: nothing to order,
     * so the transport calls must not crash, loop, or pick a folder. (The UI
     * never launches on a folder; the first open here is the stub obliging
     * a request the firmware would not make.) */
    stub_reset();
    player_set_repeat(0);
    make_entries(ents, 3, 0x7u);              /* every entry is a folder */
    player_play_queue(ents, 3, 0, 0, 0);
    player_next();
    xpect(&c, "shuffle, nothing playable: Next ends rather than picking a folder",
          player_active() == 0);
    player_prev();
    xpect(&c, "shuffle, nothing playable: Prev is a no-op",
          player_active() == 0 && stub_open_attempts == 1);
    player_pump();
    xpect(&c, "shuffle, nothing playable: pump is harmless",
          player_active() == 0 && player_queue_len() == 3);

    stub_reset();
    player_queue_begin();
    player_set_shuffle(0);
    player_set_shuffle(1);                    /* dealt over an EMPTY queue */
    player_next();
    player_prev();
    xpect(&c, "shuffle turned on over an empty queue is harmless",
          player_active() == 0 && stub_open_attempts == 0);

    /* 10j. The incremental builder (Shuffle Songs / Songs) deals the order at
     * commit, starting on the requested entry, and the run ends too. */
    make_entries(ents, 6, 0);
    for (int i = 0; i < 6; i++) {
        player_queue_add(&ents[i]);
    }
    player_queue_commit(4);
    xpect(&c, "shuffle via the builder: commit starts on the requested entry",
          player_queue_current() == 4 && stub_last_open_clus() == CLUS(4));
    {
        int plays[6] = { 0 };
        plays[4]++;
        for (int k = 1; k < 6; k++) {
            pump_to_track_end(2000);
            int cur = player_queue_current();
            if (cur >= 0 && cur < 6) plays[cur]++;
        }
        int once = 1;
        for (int i = 0; i < 6; i++) {
            if (plays[i] != 1) once = 0;
        }
        xpect(&c, "shuffle via the builder: each track once, still active",
              once && player_active() == 1);
        pump_to_track_end(2000);
        xpect(&c, "shuffle via the builder: then the queue ends",
              player_active() == 0);
    }
    player_set_shuffle(0);
    player_set_repeat(0);

    /* ---- 11. pause / resume -------------------------------------------- */
    stub_reset();
    make_entries(ents, 3, 0);
    set_usec(0);
    player_play_queue(ents, 3, 0, 0, 0);
    xpect(&c, "a fresh track is not paused", player_paused() == 0);

    set_usec(5000000u);                       /* 5 s in */
    xpect(&c, "elapsed time tracks the microsecond timer",
          player_elapsed_s() == 5u);
    player_pause();
    xpect(&c, "pause stops the DAC", stub_audio_running == 0);
    xpect(&c, "pause keeps the track loaded", player_active() == 1);
    xpect(&c, "pause reports itself", player_paused() == 1);

    set_usec(12000000u);                      /* 7 s of paused wall time  */
    xpect(&c, "the elapsed clock freezes while paused",
          player_elapsed_s() == 5u);
    int opens_at_pause = stub_opens;
    player_pump();
    xpect(&c, "pump does nothing while paused", stub_opens == opens_at_pause);

    player_resume();
    xpect(&c, "resume restarts the DAC", stub_audio_running == 1);
    xpect(&c, "resume does not re-open the track", stub_opens == opens_at_pause);
    xpect(&c, "resume clears the paused flag", player_paused() == 0);
    xpect(&c, "resume does not count the paused time as elapsed",
          player_elapsed_s() == 5u);
    set_usec(14000000u);
    xpect(&c, "the clock runs again after resume", player_elapsed_s() == 7u);

    player_toggle_pause();
    xpect(&c, "toggle pauses a playing track", player_paused() == 1);
    player_toggle_pause();
    xpect(&c, "toggle resumes a paused track", player_paused() == 0);

    /* ---- 12. stop ------------------------------------------------------ */
    player_stop();
    xpect(&c, "stop leaves the player inactive", player_active() == 0);
    xpect(&c, "stop stops the DAC", stub_audio_running == 0);
    xpect(&c, "stop clears the paused flag", player_paused() == 0);
    /* Device-proven invariant from player.h: stop must NOT close the decoder
     * mid-decode — doing so hard-freezes the core. */
    xpect(&c, "stop does not close the decoder (device-proven invariant)",
          stub_closes == 0);
    player_stop();
    xpect(&c, "stop is idempotent", player_active() == 0);

    /* ---- 12b. paused is not playing: what the power gates consume ------ *
     *
     * An audit found that a PAUSED device was treated as a PLAYING one by
     * every power gate in the main loop, because they all keyed off
     * player_active() — which must stay 1 across a pause for the UI's sake.
     * So a paused, screen-off iPod held 80 MHz, spun the main loop ~5,000
     * times a second on the DMA-feeding halt, and kept the codec's PLL,
     * VMID, DACs and headphone amps live indefinitely. player_playing() is
     * the gate those decisions want; the codec power-down is the third fix.
     *
     * The stubs model the codec's power state (stub_audio_cold) separately
     * from the DAC's running state, and keep stub_audio_primed across a
     * suspend — which is what lets a test tell "powered down and resumed
     * from the same place" from "powered down and lost a third of a second".
     */
    stub_reset();
    /* Set explicitly: the shuffle cases above leave the fake track at 4096
     * frames, and the ring arithmetic below depends on knowing the length. */
    stub_set_track_frames(8192u);
    make_entries(ents, 3, 0);
    set_usec(0);
    xpect(&c, "nothing loaded: not playing", player_playing() == 0);
    player_play_queue(ents, 3, 0, 0, 0);
    xpect(&c, "a fresh track is playing", player_playing() == 1);
    xpect(&c, "a fresh track's codec is up", stub_audio_cold == 0);
    /* Pull some of the ring so "the ring survives" below is a real claim: an
     * 8192-frame track is primed in full at open. */
    {
        int pulled = 0;
        while (pulled < 3000) {
            int got = stub_drain(1000);
            if (got <= 0) break;
            pulled += got;
        }
        xpect(&c, "the DAC drew from the ring before the pause", pulled == 3000);
    }

    set_usec(5000000u);                       /* 5 s in */
    player_pause();
    xpect(&c, "paused: NOT playing, for the power gates", player_playing() == 0);
    xpect(&c, "paused: still loaded, for the UI", player_active() == 1);

    /* A SHORT pause leaves the codec exactly as the pause left it: unpausing
     * has to be instant and silent, and the wake is a codec reset. */
    set_usec(5000000u + 2000000u);            /* 2 s into the pause */
    player_pump();
    xpect(&c, "a short pause does not power the codec down",
          stub_audio_suspends == 0 && stub_audio_cold == 0);
    player_resume();
    xpect(&c, "resume after a short pause needs no wake",
          stub_audio_wakes == 0 && stub_audio_running == 1 &&
          player_playing() == 1);
    xpect(&c, "resume after a short pause keeps the clock",
          player_elapsed_s() == 5u);

    /* A pause that PERSISTS powers the codec down — once, and not before the
     * documented timeout. */
    set_usec(10000000u);                      /* 8 s in on the running clock */
    player_pause();
    set_usec(10000000u + PLAYER_PAUSE_CODEC_OFF_US - 1u);
    player_pump();
    xpect(&c, "just under the timeout: the codec is still up",
          stub_audio_suspends == 0 && stub_audio_cold == 0);
    set_usec(10000000u + PLAYER_PAUSE_CODEC_OFF_US);
    player_pump();
    xpect(&c, "a pause held past the timeout powers the codec down",
          stub_audio_suspends == 1 && stub_audio_cold == 1);
    xpect(&c, "the power-down is a suspend, never issued under a running DMA",
          stub_audio_suspends_while_running == 0);
    player_pump();
    player_pump();
    xpect(&c, "powered down once per pause, not once per pass",
          stub_audio_suspends == 1);
    xpect(&c, "the power-down keeps the HAL's buffered PCM (suspend, not close)",
          stub_audio_primed == 1 && stub_audio_flushes == 0);
    xpect(&c, "powered down: still paused, still loaded, still not playing",
          player_paused() == 1 && player_active() == 1 && player_playing() == 0);
    xpect(&c, "powered down: the elapsed clock stays frozen",
          player_elapsed_s() == 8u);
    int opens_at_powerdown = stub_opens;

    /* Resume from the powered-down state: wake, restart, same place. */
    set_usec(10000000u + 60000000u);          /* a minute later */
    player_resume();
    xpect(&c, "resume after a power-down wakes the codec",
          stub_audio_wakes == 1 && stub_audio_cold == 0);
    xpect(&c, "resume after a power-down restarts the DAC",
          stub_audio_running == 1 && player_playing() == 1);
    xpect(&c, "resume after a power-down does not re-open the track",
          stub_opens == opens_at_powerdown);
    xpect(&c, "resume after a power-down does not reset the elapsed clock",
          player_elapsed_s() == 8u);
    xpect(&c, "resume after a power-down does not discard the HAL's PCM",
          stub_audio_primed == 1 && stub_audio_flushes == 0);
    {
        /* The ring itself: 8192 frames were primed, 3000 pulled before the
         * pause. Exactly the other 5192 must still be there — no reset, no
         * re-prime, no loss. */
        int pulled = 0, got;
        while ((got = stub_drain(1000)) > 0) {
            pulled += got;
        }
        xpect(&c, "resume after a power-down keeps the ring's contents",
              pulled == 8192 - 3000);
    }

    /* Pause -> resume -> pause -> resume, past the timeout each time, stays
     * consistent: one suspend and one wake per cycle, the clock only ever
     * counts running time, the DAC comes back every time. */
    {
        int      consistent = 1;
        uint32_t t          = 80000000u;     /* running clock: 8 s + (80-70) */
        uint32_t elapsed    = 8u + 10u;
        set_usec(t);
        for (int k = 0; k < 4; k++) {
            player_pause();
            t += PLAYER_PAUSE_CODEC_OFF_US + 3000000u;
            set_usec(t);
            player_pump();
            if (stub_audio_suspends != 2 + k || stub_audio_cold != 1 ||
                player_playing() != 0 || player_elapsed_s() != elapsed) {
                consistent = 0;
            }
            t += 1000000u;
            set_usec(t);
            player_resume();
            if (stub_audio_wakes != 2 + k || stub_audio_cold != 0 ||
                stub_audio_running != 1 || player_playing() != 1 ||
                player_elapsed_s() != elapsed) {
                consistent = 0;
            }
            t += 2000000u;                    /* 2 s of listening */
            elapsed += 2u;
            set_usec(t);
        }
        xpect(&c, "repeated pause/resume past the timeout stays consistent",
              consistent);
        xpect(&c, "repeated cycles never suspended under a running DMA",
              stub_audio_suspends_while_running == 0);
    }

    /* A skip while powered down: the new track's bring-up is a full init, so
     * the codec is up again — and still paused, as a paused skip must be. The
     * new pause then times out on its own. */
    {
        uint32_t t = 200000000u;
        set_usec(t);
        player_pause();
        set_usec(t + PLAYER_PAUSE_CODEC_OFF_US);
        player_pump();
        int suspends_before = stub_audio_suspends;
        player_next();
        xpect(&c, "a skip while powered down brings the codec up with the new "
                  "track", stub_audio_cold == 0 && player_queue_current() == 1);
        xpect(&c, "a skip while powered down stays paused",
              player_paused() == 1 && player_playing() == 0 &&
              stub_audio_running == 0);
        set_usec(t + PLAYER_PAUSE_CODEC_OFF_US + PLAYER_PAUSE_CODEC_OFF_US);
        player_pump();
        xpect(&c, "the pause on the new track times out and powers down again",
              stub_audio_suspends == suspends_before + 1 && stub_audio_cold == 1);
        /* Seek while powered down: paused, stopped, flushed — and the resume
         * that follows wakes the codec and starts from the seek target. */
        stub_set_track_frames(44100u * 10u);
        player_jump(2);                       /* a 10 s track, still paused */
        set_usec(t + 3u * PLAYER_PAUSE_CODEC_OFF_US);
        player_pump();
        xpect(&c, "seek while powered down succeeds", player_seek_to(4u) == 0);
        xpect(&c, "seek while powered down stays paused and cold",
              player_paused() == 1 && stub_audio_cold == 1 &&
              stub_audio_running == 0);
        int wakes_before = stub_audio_wakes;
        set_usec(t + 3u * PLAYER_PAUSE_CODEC_OFF_US + 1000000u);
        player_resume();
        xpect(&c, "resume after a seek while powered down wakes and starts",
              stub_audio_wakes == wakes_before + 1 && stub_audio_running == 1);
        /* This one also guards a bug the earlier paused-seek test could not
         * see: a paused seek re-anchored the start against NOW but left the
         * pause stamp at the original pause, so resume added the whole
         * pre-seek paused stretch back on — here that would read 4 s minus
         * five-plus seconds, i.e. a wrapped, absurd elapsed. The earlier test
         * pauses and seeks in the same instant, where the two agree. */
        xpect(&c, "resume after a seek while powered down is at the target",
              player_elapsed_s() == 4u);
        stub_set_track_frames(8192u);
    }

    /* Stop while powered down: inactive, not playing, and the UI's close on
     * the active->inactive edge is harmless over an already-cold codec. */
    set_usec(300000000u);
    player_pause();
    set_usec(300000000u + PLAYER_PAUSE_CODEC_OFF_US);
    player_pump();
    player_stop();
    xpect(&c, "stop while powered down leaves the player inactive and not "
              "playing", player_active() == 0 && player_playing() == 0);
    hal_audio_close();
    xpect(&c, "the UI's close after a stop while powered down is harmless",
          stub_audio_cold == 1 && stub_audio_running == 0);
    player_play_queue(ents, 3, 0, 0, 0);
    xpect(&c, "a new queue after a cold stop comes up playing",
          player_playing() == 1 && stub_audio_cold == 0 &&
          stub_audio_running == 1);

    /* ---- 13. the incremental queue builder ----------------------------- */
    stub_reset();
    player_queue_begin();
    xpect(&c, "queue_begin empties the queue", player_queue_len() == 0);
    make_entries(ents, 5, 0);
    for (int i = 0; i < 5; i++) {
        player_queue_add(&ents[i]);
    }
    xpect(&c, "queue_add accumulates entries", player_queue_len() == 5);
    player_queue_commit(3);
    xpect(&c, "queue_commit starts at the requested index",
          player_queue_current() == 3 && stub_last_open_clus() == CLUS(3));

    player_queue_begin();
    for (int i = 0; i < 5; i++) {
        player_queue_add(&ents[i]);
    }
    player_queue_commit(-1);
    xpect(&c, "queue_commit clamps an out-of-range start to 0",
          player_queue_current() == 0);
    player_queue_begin();
    for (int i = 0; i < 5; i++) {
        player_queue_add(&ents[i]);
    }
    player_queue_commit(999);
    xpect(&c, "queue_commit clamps a too-large start to 0",
          player_queue_current() == 0);

    stub_reset();
    player_queue_begin();
    player_queue_commit(0);
    xpect(&c, "committing an empty queue plays nothing",
          stub_open_attempts == 0 && player_active() == 0);

    /* The builder must not run off the end of its fixed array. */
    stub_reset();
    player_queue_begin();
    {
        browse_entry_t one;
        make_entries(&one, 1, 0);
        for (int i = 0; i < QUEUE_MAX + 50; i++) {
            player_queue_add(&one);
        }
    }
    xpect(&c, "queue_add stops at QUEUE_MAX instead of overflowing",
          player_queue_len() == QUEUE_MAX);

    /* play_queue must clamp too: it copies a caller array of any length. */
    stub_reset();
    make_entries(ents, 4, 0);
    player_play_queue(ents, 4, 0, 0, 0);
    xpect(&c, "play_queue takes the whole (short) queue", player_queue_len() == 4);

    /* ---- 13b. seek: what the HAL is left holding ----------------------- *
     *
     * These four exist because an audit found that every scrub replayed up to
     * ~370 ms of the OLD position before jumping, and this suite could not see
     * it: hal_audio_start/stop were bare counters, so "stopped and started
     * again" looked the same whether or not the backend's ping-pong buffers
     * still held the pre-seek audio. The stubs now model that buffer
     * (stub_audio_primed), which is what makes a missing flush observable. */
    stub_reset();
    /* A 10 s track: the default fake is 8192 frames (~0.19 s), so any seek
     * target would clamp to 0 and the clock assertion below would pass for the
     * wrong reason. */
    stub_set_track_frames(44100u * 10u);
    make_entries(ents, 4, 0);
    player_play_queue(ents, 4, 0, 0, 0);
    set_usec(0);

    int seeks_before = stub_seeks;
    xpect(&c, "seek reports success", player_seek_to(3u) == 0);
    xpect(&c, "seek reaches the decoder", stub_seeks == seeks_before + 1);
    xpect(&c, "seek discards the PCM the HAL had buffered from the old "
              "position", stub_audio_flushes >= 1 && stub_audio_primed == 0);
    xpect(&c, "seek restarts the DAC", stub_audio_running == 1);
    xpect(&c, "seek moves the elapsed clock to the target",
          player_elapsed_s() == 3u);

    /* A seek the decoder refuses must not stop playback — but it cannot
     * "leave it as it was" either: the codec's seek moves the byte cursor as
     * it searches, so after a failure the decoder is somewhere unknown. The
     * old path restarted the DAC into that: the ring's tail of the old spot,
     * then whatever the decoder found. The one position that can be vouched
     * for is the top of the track, so a failed seek restarts there, with the
     * stale PCM flushed, and reports the failure. */
    stub_reset();
    player_play_queue(ents, 4, 0, 0, 0);
    set_usec(10000000u);
    stub_set_seek_ok(0);                   /* every seek fails, even to 0 */
    int opens_before_refusal = stub_opens;
    xpect(&c, "a refused seek reports failure", player_seek_to(3u) == -1);
    xpect(&c, "a refused seek leaves the DAC running", stub_audio_running == 1);
    xpect(&c, "a refused seek leaves the track playing", player_active() == 1);
    xpect(&c, "a codec that cannot even reach frame 0 is reopened",
          stub_opens == opens_before_refusal + 1);
    xpect(&c, "a refused seek flushes the PCM of the position it left",
          stub_audio_flushes >= 1 && stub_audio_primed == 0);
    xpect(&c, "a refused seek restarts the clock at 0:00, not somewhere unknown",
          player_elapsed_s() == 0u);
    xpect(&c, "a refused seek leaves audio in the ring to play",
          stub_drain(1024) == 1024);
    stub_set_seek_ok(1);

    /* The same, for a seek that overshoots while the top of the track is
     * still reachable: no reopen, a re-seek to 0. The fake decoder models the
     * cursor motion by ending the stream on a failed seek, so resuming into
     * it (the old behaviour) would leave nothing to play. */
    stub_reset();
    stub_set_track_frames(44100u * 10u);
    player_play_queue(ents, 4, 0, 0, 0);
    set_usec(10000000u);
    stub_set_seek_max(44100u * 5u);        /* targets past 5 s fail */
    int opens_before_overshoot = stub_opens;
    xpect(&c, "an overshooting seek reports failure", player_seek_to(8u) == -1);
    xpect(&c, "an overshooting seek re-seeks the live decoder to 0 rather "
              "than reopening", stub_opens == opens_before_overshoot &&
          stub_last_seek_frame == 0u);
    xpect(&c, "an overshooting seek restarts from the top: DAC running, "
              "clock at 0:00", stub_audio_running == 1 &&
          player_elapsed_s() == 0u);
    xpect(&c, "an overshooting seek leaves audio in the ring to play",
          stub_drain(1024) == 1024);
    /* ...and a subsequent good seek still works on the same decoder. */
    xpect(&c, "a later in-range seek on the same track succeeds",
          player_seek_to(4u) == 0 && player_elapsed_s() == 4u);
    stub_set_seek_max(~(uint64_t)0);

    /* An MP3 whose length is unknown had no clamp at all: a scrub past the
     * end sent the decoder scanning to EOF. The file size still bounds it —
     * MPEG audio is never below 8 kbps — so the target is clamped to what
     * the file could possibly hold. */
    stub_reset();
    stub_set_track_frames(44100u * 7200u);
    stub_set_total_unknown(1);
    make_entries(ents, 1, 0);
    ents[0].fmt  = 1;                      /* MP3 */
    ents[0].size = 1024u * 1024u;          /* 1 MiB: at most 1048 s */
    player_play_queue(ents, 1, 0, 0, 0);
    set_usec(0);
    xpect(&c, "unknown length: the player reports no total",
          player_total_s() == 0u);
    xpect(&c, "unknown-length MP3: a seek far past what the file could hold "
              "succeeds, clamped", player_seek_to(5000u) == 0);
    xpect(&c, "unknown-length MP3: the decoder was asked for the last frame "
              "the file could possibly hold",
          stub_last_seek_frame == (uint64_t)(1048576u / 1000u) * 44100u - 1u);
    xpect(&c, "unknown-length MP3: the clock reads the clamped position",
          player_elapsed_s() == 1047u);
    stub_set_total_unknown(0);
    stub_set_track_frames(44100u * 10u);   /* back to this section's 10 s track */
    make_entries(ents, 4, 0);

    /* Seeking while PAUSED must not start the DAC — and must not corrupt the
     * resume position, which is what hal_audio_stop's unguarded recompute did
     * when it ran against an already-stopped engine. */
    stub_reset();
    player_play_queue(ents, 4, 0, 0, 0);
    set_usec(0);
    player_pause();
    xpect(&c, "a paused seek succeeds", player_seek_to(2u) == 0);
    xpect(&c, "a paused seek leaves the DAC stopped", stub_audio_running == 0);
    xpect(&c, "a paused seek stays paused", player_paused() == 1);
    xpect(&c, "a paused seek still flushes the stale PCM",
          stub_audio_primed == 0);
    xpect(&c, "a paused seek repositions the frozen clock",
          player_elapsed_s() == 2u);
    stub_set_track_frames(8192u);          /* back to the short default */

    /* ---- 13c. seek inside the end-of-track window ---------------------- *
     *
     * Once a track reaches decoder EOS the pump prefetches the next one, which
     * re-points the byte-source chain and leaves g_dec owned by a different
     * file. Seek used to refuse outright for that whole window — the last ~6 s
     * of EVERY track, during which the listener is still hearing it — so
     * dragging the scrubber back did nothing and the position snapped forward.
     * It must now reopen the current track and land the seek. */
    stub_reset();
    make_entries(ents, 4, 0);
    player_play_queue(ents, 4, 0, 0, 0);
    set_usec(0);
    {
        /* Pump until the prefetch has happened but the hand-over has not:
         * stub_opens goes up while the presented track is still index 0. */
        int guard = 0;
        while (stub_opens < 2 && player_queue_current() == 0 && guard++ < 200) {
            player_pump();
        }
        xpect(&c, "the next track really was prefetched behind the current one",
              stub_opens >= 2 && player_queue_current() == 0);

        int opens_at_prefetch = stub_opens;
        xpect(&c, "a seek inside the prefetch window succeeds instead of "
                  "being refused", player_seek_to(1u) == 0);
        xpect(&c, "it reopens the CURRENT track rather than seeking the "
                  "prefetched one", stub_opens == opens_at_prefetch + 1);
        xpect(&c, "the presented track is still the one being listened to",
              player_queue_current() == 0);
        xpect(&c, "it flushes the stale PCM too", stub_audio_primed == 0);
    }

    /* ---- 13d. the tail of a track reaches the DAC ---------------------- *
     *
     * The player advances on ring-empty, but at that instant the HAL still
     * holds up to two un-clocked buffers. hal_audio_drain() was written for
     * exactly this, documented as required before stop at end-of-track, and
     * called from nowhere — so the last fraction of a second of every album
     * was discarded. It must be drained on the AUTOMATIC end, and deliberately
     * NOT on a button press. */
    stub_reset();
    make_entries(ents, 2, 0);
    player_play_queue(ents, 2, 0, 0, 0);
    set_usec(0);
    for (int i = 0; i < 400 && player_active(); i++) {
        player_pump();
        stub_drain(4096);
    }
    xpect(&c, "the queue ran out on its own", player_active() == 0);
    xpect(&c, "the automatic end drains the in-flight buffers BEFORE cutting "
              "the DAC", stub_audio_drained_while_running >= 1);

    stub_reset();
    make_entries(ents, 2, 0);
    player_play_queue(ents, 2, 0, 0, 0);
    set_usec(0);
    player_next();                    /* -> track 1 */
    player_next();                    /* past the last track, Repeat off */
    xpect(&c, "Next past the last track ends playback", player_active() == 0);
    xpect(&c, "a button press does NOT wait for the buffers to drain",
          stub_audio_drains == 0);

    /* ---- 13e. quiet playback parks the drive ONCE ---------------------- *
     *
     * Between refill bursts the anti-skip buffer is topped up and idle, and
     * the pump parks the platters. That state persists for tens of seconds
     * per burst, i.e. thousands of pump passes. Each pass used to un-park
     * the drive in software (without any read to justify it) and then, seeing
     * an un-parked idle drive, park it again: a STANDBY IMMEDIATE — cpu_boost,
     * ready wait, command, wait — hundreds of times a second for the whole of
     * quiet playback. The fakes model the idle state exactly: diskbuf_pump()
     * has nothing to do and fill_ahead sits far above the low watermark. */
    stub_reset();
    stub_set_track_frames(44100u * 60u);   /* long enough never to hit EOS */
    make_entries(ents, 1, 0);
    player_play_queue(ents, 1, 0, 0, 0);
    set_usec(0);
    for (int i = 0; i < 300; i++) {
        player_pump();                     /* fills the ring past the parked gate */
    }
    /* Model one refill burst starting and ending: a pass with the buffer
     * drained below the watermark (the pump would read here), then topped up
     * again. Whatever state the drive was left in by the sections above, it
     * is now unparked-and-idle, and the pass after must park it. */
    stub_set_disk_ahead(0);
    player_pump();
    stub_set_disk_ahead(64u * 1024u * 1024u);
    int standbys_before = stub_ata_standbys;
    for (int i = 0; i < 400; i++) {
        player_pump();
    }
    xpect(&c, "quiet playback parks the drive exactly once, not once per pass",
          stub_ata_standbys == standbys_before + 1);
    /*
     * The same passes used to arm the pump's one-shot spin-up probe and never
     * consume it, so the next block read from ANYWHERE went out with the
     * retry loop disabled — the intermittent "OPEN FAILED" the six tries were
     * written to ride over, back again for every prefetch, art load and
     * library probe that followed a quiet stretch. With reads failing, a
     * latched probe shows up as one attempt instead of six.
     */
    {
        uint8_t sector[512];
        stub_set_ata_read_ok(0);
        int reads_before = stub_ata_reads;
        xpect(&c, "a failing read still fails",
              player_disk_read(0, 0, 1, sector) == -1);
        xpect(&c, "the spin-up probe is not left armed by an idle pump pass: "
                  "the next read gets its full retry loop",
              stub_ata_reads - reads_before == 6);
        stub_set_ata_read_ok(1);
    }
    stub_set_track_frames(8192u);

    /* ---- 14. calls with nothing loaded are safe ------------------------ */
    stub_reset();
    player_queue_begin();          /* stops playback, empties the queue */
    player_next();
    player_prev();
    player_pump();
    player_pause();
    player_resume();
    player_toggle_pause();
    xpect(&c, "next/prev/pump/pause on an empty queue do nothing",
          player_active() == 0 && stub_open_attempts == 0 &&
          player_queue_len() == 0);
    xpect(&c, "nothing loaded is not playing, and pumping it never touches "
              "the codec", player_playing() == 0 && stub_audio_suspends == 0 &&
          stub_audio_wakes == 0);
    xpect(&c, "elapsed/total read as zero when nothing is loaded",
          player_elapsed_s() == 0u && player_total_s() == 0u);

    return xfail_done(&c);
}
