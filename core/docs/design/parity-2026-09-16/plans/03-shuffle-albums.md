# 03 — Shuffle Albums

Repo: /home/brando/Projects/ipod_theme (main @ dbce3b8). Firmware in `core/`. All paths below are
relative to `core/` unless they start with `docs/`, `tools/` or `STATUS.md`. Line numbers are as of
dbce3b8. Nothing in this plan can be verified on the device in this job; every user-visible change
ships **UNFLASHED** and is listed for the bench at the end.

## Summary

Settings → Playback → Shuffle becomes a three-way select, Off / Songs / Albums, like the original
iPod. Songs is exactly today's shuffle (a seeded permutation of the queue's tracks). Albums plays
the current album through in its track order, then picks another album from the queue at random
and plays it through, until every album in the queue has played once; Repeat All re-deals the
album order, Repeat One is unchanged. The mode is stored in the existing shuffle byte of the
config record (0 off, 1 songs, 2 albums; unknown → 0), so old records load unchanged and an old
build reading a new record degrades to Songs. The album deal is a new branch of the player's
existing seeded deal (`shuffle_deal`), driven by two new per-entry fields in `browse_entry_t`
(`album` group id, `order_key` = the index's `(disc << 16) | track`), so it is a pure function of
(queue, seed, keep) exactly like the Songs deal and rides the resume record's existing
`resume_order_seed` / `resume_order_keep` pair with no record change. Now Playing shows
`SHUF·ALB` instead of `SHUF`. No new kernel/main.c logic: main.c fills the two new entry fields at
its four queue builders, adds one honest-token branch, and one `resume_capture()` call.

## Current behaviour (file:line)

**Settings model** — `ui/settings.h:47` `int shuffle; /* 0/1 */`. `ui/settings.c:134` `PLAY_L`
= {Shuffle, Repeat, Resume}; `settings_kind` returns `SETTINGS_KIND_SELECT` for the whole
Playback screen (`ui/settings.c:241-242`); `settings_value` row 0 prints `"On"/"Off"`
(`ui/settings.c:285`); `settings_activate` row 0 does `s->shuffle = !s->shuffle` and returns
`SETTINGS_ACTION_NONE` (`ui/settings.c:355`); `settings_defaults` sets `s->shuffle = 0`
(`ui/settings.c:159`). The Go test `cli/internal/devicefs/settings_defaults_test.go:32-116` greps
that function body with the regex `s->(\w+)\s*=\s*([A-Za-z0-9_+-]+)\s*;` and maps only
`REPEAT_*`/`RESUME_KIND_NONE` identifiers through `cValue()` (line 106) — so the literal `0` must
stay a literal (or `cValue` must learn the new enum).

**Record codec** — `kernel/config.c:161` `P_SHUFFLE = 0` (payload byte 0). Encode
`p[P_SHUFFLE] = s->shuffle ? 1 : 0` (`kernel/config.c:308`); decode `s->shuffle = p[P_SHUFFLE] ? 1 : 0`
(`kernel/config.c:385`). `CONFIG_VERSION 2` (`kernel/config.h:95`), `length` 44
(`CFG_PAYLOAD_V2Q`, `kernel/config.c:207`). Rule in force (`kernel/config.h:72-93`,
`kernel/config.c:110-130`): never move an offset; `length`, not `version`, gates what is read;
every decoded field is clamped, theme's precedent is "unknown id → the default"
(`kernel/config.c:395-397`). Host mirrors: `tools/make_config.py:87` `("shuffle", 0)`;
`cli/internal/devicefs/config.go:81` `Shuffle uint8 // 0/1`, encode at :137, decode at :192 (raw
byte, no clamp — the firmware clamps). Fixture: meson builds `corecfg_golden.bin` with
`make_config.py --emit` and `tests/kernel/config_test.c:1240-1290` decodes it and compares with
`settings_defaults()`.

**Player** — `player/player.h:32-40` `browse_entry_t` {name[65], clus, size, art_clus, art_size,
fmt, is_dir}: sizeof 88 on both host and ARM EABI, with **2 bytes of tail padding at offset 86**
(checked with gcc and arm-none-eabi-gcc). `player/player.c:671` `g_shuffle` (0/1), `:672 g_repeat`,
`:698 g_order[QUEUE_MAX]` (uint16), `:699 g_order_n`, `:707-708 g_order_seed / g_order_keep`.
`shuffle_deal(seed, keep)` `player/player.c:861-884`: collects playable indices, Fisher–Yates with
the LCG (`rng_next` :793) from `seed`, moves `keep` to the front, `PLAYER_KEEP_QUEUE` = queue order.
`shuffle_build(keep)` :891 draws a fresh seed from `g_rng` + `USEC_TIMER`. `shuffle_next` :911
walks `g_order`; at the end under Repeat All re-deals up to 8 times until `g_order[0] != from`,
then a track-level `order_swap` fallback (:927-928). `shuffle_prev` :935. `successor/predecessor`
:946-954 select by `g_shuffle`. `player_set_shuffle(int on)` :963: deals only on off→on, pinning
`g_queue_idx`; called on EVERY `settings_apply()` so "on while on" must stay a no-op.
`player_order_seed/keep` :972-973; `player_reshuffle_with_seed` :975 deals now, whether or not
shuffle is on. Deals also happen in `player_play_queue` :1575 and `player_queue_commit` :1610.
`auto_next_index` :1381 (Repeat One returns `from`). Queue accessors :1877-1891 expose queue order
only; `player_jump` :1893; `player_next` :1917 takes the pump's prefetched successor; `player_prev`
:1978.

**main.c wiring** — `settings_apply()` `kernel/main.c:2871-2880` pushes `g_settings.shuffle` to
`player_set_shuffle`. The four queue builders: album folder `browse_collect` :552-604 (file entries
filled at :596-602, `art_clus = 0`), then `browse_bind` :3690-3745 sorts `g_browse` by the index's
`(disc << 16) | track` (`g_browse_key`, unbound rows 0xFFFFFFFF sort last) — the album's PLAY order
is this order; `library_play_song` :2607-2645 (entry filled :2627-2638; `album_by_clus(s->dir_clus)`
already computed for art); `shuffle_songs_build` :2665-2707 (entry :2689-2700; ends with
`player_reshuffle_with_seed(0, PLAYER_KEEP_QUEUE)`); `playlist_play` :3000-3029 (entry
:3013-3024, `g_pl_song[i]` holds the bound song index or -1). `queue_kind` bookkeeping :2565-2570.
Now Playing tokens `nowplaying_render` :3466-3478: `if (g_settings.shuffle) "SHUF"`, then
`RPT`/`RPT1`, right-aligned at `rc = bx - 6` (bx = 289) in `FONT_SMALL`; "TRACK N OF M" :3499-3512
uses `player_queue_current()+1` / `player_queue_len()`. Queue view `queue_render` :2833-2860 lists
`player_queue_name(i)` in queue order, header count `cur+1 / n`, playing row marked. Settings
SELECT handler :6410-6466: `SETTINGS_ACTION_NONE` → `settings_apply(); settings_touch();`.
**The status strip (`ui/chrome.c status_strip_render`) draws no SHUF token** — the token lives only
in main.c's Now Playing header; `docs/screens/render.py:863` draws a literal `"SHUF"` on the Now
Playing still and renders no Playback screen.

**Resume** — `resume_capture` :4348-4386 stores `player_order_seed()/keep()` via
`resume_ctx_store` (`kernel/resume_ctx.h:267`); `resume_restore` :4614-4695 rebuilds the queue by
kind, then (:4670-4680) `player_reshuffle_with_seed(resume_order_seed, resume_order_keep)` when
`g_queue_kind == kind` and (seed != 0 || keep == KEEP_QUEUE). Boot order: `settings_apply()` :5608
(pushes the mode) then `resume_restore(fs)` :5615. A shuffle toggle in Settings re-deals (new seed)
but does NOT capture, so the saved pair is stale until the next capture edge.

**Tests today** — `tests/player/player_shuffle_test.c` (seed/keep reproducibility, 200 lines),
`tests/player/player_queue_test.c` §10a-10j (permutation, Repeat All, Prev, folders, builder),
`tests/ui/settings_test.c` Test 2 + "none-shuffle" (:52-58, :337), `tests/kernel/config_test.c`
(`spicy()` :248 sets `shuffle = 1`; hostile-record clamp test :406-440; fixture :1240-1290).
Test entries come from `make_entries()` which `memset`s the struct to 0.

## Design

### 1. The setting: three states

`ui/settings.h`:

```c
/* Shuffle policy (FUNCTIONAL — the player's playback order). */
typedef enum { SHUFFLE_OFF = 0, SHUFFLE_SONGS = 1, SHUFFLE_ALBUMS = 2 } shuffle_mode_t;
...
    shuffle_mode_t shuffle;  /* SHUFFLE_* — FUNCTIONAL (player queue order)  */
```

Every existing truthiness use (`if (g_settings.shuffle)`, `player_set_shuffle(g_settings.shuffle)`)
keeps compiling and keeps meaning "some shuffle is on". `settings_defaults` keeps the literal
`s->shuffle = 0;` (the Go grep). Row 0 of Playback: `settings_value` prints `"Off"/"Songs"/"Albums"`
(widths in regular-11: 18/34/41 px; "60 sec" on Display is 38, so the right-aligned value column
is unaffected); `settings_activate` cycles `(s->shuffle + 1) % 3` and returns `SETTINGS_ACTION_NONE`
(always a change, like Repeat). `PLAY_L` and row indices do not move.

### 2. On disk: reuse the shuffle byte

`kernel/config.c`: encode `p[P_SHUFFLE] = (uint8_t)clampi((int)s->shuffle, 0, 2);` decode
`s->shuffle = (p[P_SHUFFLE] <= 2) ? (shuffle_mode_t)p[P_SHUFFLE] : SHUFFLE_OFF;` (unknown → Off,
the theme precedent — not a clamp to Albums). No version bump, no length change, no offset moves:
a v1/24-byte/44-byte record with 0 or 1 reads exactly as before; a build at dbce3b8 reading a 2
gets `p ? 1 : 0` = Songs, which is the right downgrade. Update the comment at `config.c:161`, the
table in `docs/design/settings-persistence.md:155` (`u8 (0 off, 1 songs, 2 albums)`),
`tools/make_config.py:87` comment, `cli/internal/devicefs/config.go:81` comment. The host default
stays 0. `make_config.py --emit` fixture is unchanged.

### 3. The album key on every queue entry

`player/player.h` `browse_entry_t` gains, after `is_dir`:

```c
    uint16_t album;      /* Shuffle Albums group: album_by_clus()+1 for library-built
                          * queues, 1 for a folder listing, 0 = unknown (its own group) */
    uint32_t order_key;  /* place within the album, (disc << 16) | track — the SAME key
                          * browse_bind() orders the tracklist by; 0xFFFFFFFF = unbound
                          * (sorts last), ties fall back to queue order                */
```

sizeof goes 88 → 92 (the `uint16_t` fits the existing tail padding at 86; the `uint32_t` adds a
word). `g_queue[6000]` grows 24 KB, `g_browse[128]` 512 B; against the 14 MB `BSS_MAX` in
`tests/scripts/check_size.sh` (bss ~10.1 MB today) this is noise, but `make verify-hw` must be run.
Why `order_key` and not filename order: `browse_bind` (`main.c:3712-3720`) already declares "one
authority: the index" — an album whose numbers did not come from filenames must play in the same
order the tracklist shows, and multi-disc folders named `01. x` per disc would interleave under a
name sort. Why an album id and not `dir_clus`: 16 bits are enough (`LIB_MAX_ALBUMS 1024`) and
the id is what `album_by_clus()` already yields at every builder.

Every site that fills a `browse_entry_t` sets both (they are NOT memset; a stale value from a
previous listing would silently regroup):

| site | album | order_key |
|---|---|---|
| `browse_collect` dir branch (`main.c:581-587`) | 0 | 0 |
| `browse_collect` file branch (`:596-602`) | 1 (one folder = one album) | 0, then **`browse_bind` copies `g_browse_key[i]` into `g_browse[i].order_key`** after its in-place permutation (add the copy at the end of `browse_bind`, or set `g_browse[i].order_key` where `g_browse_key[i]` is assigned at :3708 and in the reset loop :3695, then let the existing swap loop carry it — the swap loop already moves whole entries, so assigning before the sort is simplest) |
| `library_play_song` (`:2627-2638`) | `(uint16_t)(ai + 1)` (ai from the existing `album_by_clus`) — 0 when ai < 0 | `((uint32_t)s->disc << 16) \| s->track` |
| `shuffle_songs_build` (`:2689-2700`) | same | same |
| `playlist_play` (`:3013-3024`) | same (ai already computed) | from `g_pl_song[i]` when ≥ 0, else `0xFFFFFFFFu` |

Optional tidy: a `static void queue_entry_from_song(browse_entry_t *e, const lib_song_t *s)` in
main.c shared by the three library builders (they are already three copies of nine lines). Not
required; if done, keep it beside `library_play_song`.

### 4. The album deal in the player

`player/player.h`:

```c
#define PLAYER_SHUFFLE_OFF    0
#define PLAYER_SHUFFLE_SONGS  1
#define PLAYER_SHUFFLE_ALBUMS 2
/* mode: OFF walks the queue; SONGS walks a seeded permutation of the playable
 * entries; ALBUMS walks a seeded permutation of ALBUM GROUPS, each group in
 * (order_key, queue index) order. Any change of mode over a non-empty queue
 * re-deals with the current track (SONGS) or its whole album (ALBUMS) pinned
 * first; the same mode re-pushed is a no-op. */
void player_set_shuffle(int mode);
```

The name and 0/1 semantics stay so the 40-odd existing call sites and tests compile unchanged.
main.c adds `_Static_assert(SHUFFLE_ALBUMS == PLAYER_SHUFFLE_ALBUMS, "...")` (and SONGS) next to
its includes rather than including `ui/settings.h` in the player.

`player/player.c`:

- `g_shuffle` becomes the mode (0/1/2); every `g_shuffle ?` truthiness test keeps working.
- `player_set_shuffle(int mode)`: `if (mode < 0 || mode > 2) mode = 1;`
  `if (mode && mode != g_shuffle) shuffle_build(g_queue_n > 0 ? g_queue_idx : PLAYER_KEEP_NONE);`
  `g_shuffle = mode;` — off→on, songs→albums and albums→songs all re-deal pinned on the current
  track; same→same stays a no-op (settings_apply re-pushes on every volume tick).
- `shuffle_deal(seed, keep)`: after collecting playable indices and recording seed/keep,
  `if (keep == PLAYER_KEEP_QUEUE) return;` (unchanged, mode-independent — the Shuffle Songs queue
  is its own order); `if (g_shuffle == PLAYER_SHUFFLE_ALBUMS) { album_deal(seed, keep); return; }`
  else the existing Fisher–Yates. A deal made while the mode is OFF (via
  `player_reshuffle_with_seed`) is a Songs deal, as today.
- New statics: `static uint16_t g_sorted[QUEUE_MAX];` (12 KB scratch) and `static int g_groups;`
  (album groups in the last album deal; 0 after a songs deal).
- `album_deal(seed, keep)`, deterministic in (queue, seed, keep):
  1. Copy the playable indices to `g_sorted[0..n)` and heapsort them by
     `(g_queue[i].album, g_queue[i].order_key, i)` — in place, O(n log n), no stability needed
     because the key is a total order. Keep the comparator a small static function; do not pull
     `library/sort.c` (it needs a scratch of its own and the player must stay UI/library-free).
  2. Write group starts into `g_order[0..G)`: position `p` starts a group when `p == 0`, or
     `album` differs from `g_sorted[p-1]`'s, or `album == 0` (unknown albums are singletons).
     Run length of the group at start `s` is recomputed when needed: 1 if `album == 0`, else the
     run of equal `album` in `g_sorted`. No length array.
  3. `g_rng = seed;` Fisher–Yates over `g_order[0..G)` with `rng_next()` — the same LCG and the
     same seed discipline as the Songs deal, so `(seed, keep)` reproduces it.
  4. `keep >= 0`: find `p` = position of `keep` in `g_sorted`, then the group `g` whose
     `[start, start+len)` contains `p`; `order_swap(0, g)` (a swap of two starts is a swap of two
     groups). Moving one group to the front of a uniform permutation leaves the rest uniform.
  5. Materialise **back to front, in place**: `out = n; for (g = G-1; g >= 0; g--) { s = g_order[g];
     len = run_len(s); out -= len; copy g_sorted[s..s+len) → g_order[out..out+len); }`. Safe because
     after processing groups `G-1..g` the write cursor is `Σ_{h<g} len_h ≥ g` (every group has at
     least one entry), so no unread start is overwritten. Then `g_order_n = n; g_groups = G;`.
- `shuffle_next` Repeat All wrap: in ALBUMS mode the constraint becomes "the new first ALBUM is not
  the album that just finished" — retry `shuffle_build(PLAYER_KEEP_NONE)` up to 8 times while
  `g_groups > 1 && same_album(g_order[0], from)` (where `same_album` is equal nonzero `album`, or
  equal index when `album == 0`); **no `order_swap` fallback in ALBUMS mode** (it would break an
  album's order; after 8 tries the chance of a repeat is (1/G)^8 and a repeated album is not a
  bug). With `g_groups <= 1` (a single-album queue) accept the first re-deal: the album loops in
  order. The Songs branch is untouched.
- `shuffle_prev`, `successor`, `predecessor`, `auto_next_index`, `player_next`, `player_prev`,
  `player_jump`, `player_play_queue`, `player_queue_commit`, `player_reshuffle_with_seed`: no
  change — they only ask "what is before/after this entry in `g_order`".

Behaviour that falls out:

- **Single-album queue** (an album from the browser, or a Songs queue that happens to be one
  album): one group, order = the tracklist's; plays through; Repeat Off ends (`player_end_seq`
  bumps, the UI drops the resume position as today); Repeat All loops the album in order; Repeat
  One repeats the track.
- **Turning Albums on mid-album** at track 5/12: the album's group is pinned first, the current
  track stays current (no reopen), tracks 6..12 follow, then random albums; Prev reaches 4, 3….
- **Songs/artist/genre/playlist queues**: groups are by album id regardless of adjacency (a Songs
  queue is title-sorted, so an album's tracks are scattered — grouping by adjacency would give
  singletons). Within a group the order is the index's (disc, track), i.e. the tracklist's order,
  not the list's. Tracks whose folder is not in the album table (`ai < 0`, only past
  `LIB_MAX_ALBUMS`) are singletons.
- **Shuffle Songs (Music menu)** always shuffles SONGS, whatever the setting — the queue IS the
  seeded library order and the existing `player_reshuffle_with_seed(0, PLAYER_KEEP_QUEUE)` keeps
  it that way (KEEP_QUEUE is mode-independent). This matches the original iPod, where the menu
  item ignores the Shuffle setting. Changing the setting to Albums mid-Shuffle-Songs re-deals album
  groups over the library queue (that IS "shuffle every album in the library"), and back to Songs
  re-deals a permutation. To shuffle every album in the library deliberately: Albums on, play any
  song from Songs.
- **Prefetch / Repeat All re-deal timing**: the "Known edge" comment at `player.c:903-909` applies
  unchanged.

### 5. What the UI shows

- **Now Playing token** (`main.c:3466-3478`): `"SHUF"` for Songs, `"SHUF" UI_GLYPH_MIDDOT "ALB"`
  for Albums, with one honesty rule: `mode == ALBUMS && player_order_keep() == PLAYER_KEEP_QUEUE`
  prints `"SHUF"`, because a Shuffle Songs queue is walking songs. Width budget (measured with the
  gallery renderer's atlas metrics, `FONT_SMALL`): `SHUF·ALB RPT1` = 75 px, right-aligned at
  x 283 (271 when Hold is on) → left edge ≥ 196; "Now Playing" in bold-12 ends at x 89. Fits with
  >100 px to spare. `st[16]` holds `SHUF\x03ALB RPT1\0` (14 bytes) — leave the buffer at 16.
  `UI_GLYPH_MIDDOT` (`ui/text.h:31`) maps to U+00B7 in every face (`ui/text.c:157`).
- **Queue view**: unchanged — queue order with the playing row marked, header `cur+1 / n`. This
  is the same relationship the view already has to a Songs shuffle over an album queue (the view
  is the queue, playback is the order). A play-order queue view is a separate follow-up (it would
  need `player_queue_name/jump` to map through `g_order` for BOTH shuffle modes and the resume
  `qidx` hint to change meaning); not in this feature.
- **"TRACK N OF M"**: unchanged — queue position of queue length, as for Songs shuffle.
- **Settings row**: `Shuffle    Songs` / `Albums`; SELECT cycles. No new rows.
- **Boot phase label** `SHUFFLING SONGS` (`main.c:2685`): unchanged (Shuffle Songs is songs).

### 6. Resume across boots

No record change. `resume_capture` already stores `player_order_seed()/keep()`; `resume_restore`
already re-deals from them after `settings_apply()` has pushed the mode, so an album deal is
reproduced from the same (queue, seed, keep, mode). Two wiring additions in main.c:

1. In the Settings SELECT handler (`main.c:6459-6466`), the `SETTINGS_ACTION_NONE` branch becomes
   `settings_apply(); resume_capture(); settings_touch();`. `resume_capture` is idempotent
   (`resume_ctx_store` compares) and forward-declared at :723; after a mode change the player
   re-dealt with a fresh seed, and without this the record carries the old pair until the next
   capture edge (track change / pause / 5 min) — a boot in that window would deal a valid but
   different order. (Also fixes the same staleness for today's off→on toggle.)
2. Nothing else: `resume_restore`'s guard `(order_seed != 0 || keep == KEEP_QUEUE)` and the
   `g_queue_kind == kind` check already cover Albums. The album fallback path
   (`resume_open_album`) gets `album = 1` from `browse_collect` and keys from `browse_bind` because
   it runs `browse_load` (:4462), which calls `browse_bind` (:3775).

### 7. Repeat interaction (summary)

| Repeat | Albums mode |
|---|---|
| Off | every album in the queue once, each in order; then the queue ends (`player_end_seq`) |
| All | at the end, a fresh album order under a fresh seed; first album ≠ the one just finished when there is more than one; a single album loops in order |
| One | the current track repeats (`auto_next_index`); Next/Prev ignore it as today |

## Files to change

- `ui/settings.h` — `shuffle_mode_t`; field type; comment at :47.
- `ui/settings.c` — `settings_value` :285 (`"Off"/"Songs"/"Albums"`); `settings_activate` :355
  (cycle mod 3); keep `s->shuffle = 0;` literal at :159; update the header comment block
  (`settings.h:22`) that lists shuffle as 0/1.
- `kernel/config.c` — encode :308, decode :385, comment :161.
- `kernel/config.h` — one line in the version comment (:75-80) noting byte 0 now carries 0..2
  under the same version.
- `player/player.h` — `browse_entry_t` two fields; `PLAYER_SHUFFLE_*`; `player_set_shuffle` doc.
- `player/player.c` — `g_shuffle` as mode; `player_set_shuffle`; `shuffle_deal` branch;
  `album_deal` + heapsort + comparator + `run_len`/`same_album` helpers; `g_sorted`, `g_groups`;
  `shuffle_next` wrap rule; the "Playback order" comment block (:768-780) gains the Albums
  invariant: "in ALBUMS mode `g_order` is the concatenation of album groups, each in
  (order_key, index) order".
- `kernel/main.c` — thin wiring only: `_Static_assert`s; `browse_collect` (2 sites);
  `browse_bind` (order_key copy); `library_play_song`, `shuffle_songs_build`, `playlist_play`
  (2 lines each, or the shared helper); `nowplaying_render` token; Settings handler
  `resume_capture()`.
- `tools/make_config.py` — comment at :87. `cli/internal/devicefs/config.go` — comment at :81
  (`// 0 off, 1 songs, 2 albums`). No behaviour change on the host.
- `docs/design/settings-persistence.md` — row 0 of the table.
- `docs/USER_GUIDE.md`, `STATUS.md`, `CHANGELOG.md` — see Docs.
- Tests: see below; `tests/meson.build` gains one executable if the album cases go in a new file.

Not touched: `ui/chrome.c` (no token there), `ui/screen_settings.c` (generic SELECT rendering),
`kernel/resume_ctx.h`, `tests/scripts/check_resume_parity.py` (`resume_find_song` unchanged),
`docs/screens/render.py` (no Playback still; the Now Playing still keeps `SHUF`).

## Tests to add

All host-side, run with `make sim && meson test -C build-sim`; then `make hw && make verify-hw`
(size gate for the +36 KB bss, `-Werror` for the enum/int conversions).

**`tests/player/player_album_test.c`** (new, registered as `player-album` in `tests/meson.build`
with the same sources/includes as `player_shuffle_test`; alternatively append to
`player_shuffle_test.c`). Fixture: `make_entries` variant that sets `album` and `order_key`, e.g.
12 entries = 3 albums (ids 1..3) × 4 tracks, laid out **interleaved and with track numbers out of
queue order** (A3, B1, C4, A1, B3, C2, A4, B2, C1, A2, B4, C3 with `order_key = track`), so a
correct deal must both regroup and reorder. Read the walk back with `stub_last_open_clus()` after
each `player_next()` (the pattern of `player_shuffle_test.c:51-60`), never `g_order` directly.

1. Albums mode, Repeat Off, start on A1: the 12-step walk plays every entry once, each album's
   four tracks are contiguous and in track order, and the 13th Next ends the queue
   (`player_active()==0`, `player_end_seq()` bumped, `stub_opens == 12`).
2. keep pins the current ALBUM: start on B2 → the next two are B3, B4, then two whole albums;
   `player_prev()` from B2 (elapsed < 3 s) is B1; `player_order_keep()` == index of B2.
3. Reproducibility: same `(seed, keep)` via `player_reshuffle_with_seed` replays the same walk;
   a different timer value deals a different album order (with 3 albums and fixed timer values
   this is deterministic — pick values and assert the two orders differ, as
   `player_shuffle_test.c:103-110` does).
4. Single album (4 entries, album 1): Repeat Off — in order, then ends; Repeat All — the second
   pass is again in order (no track-level swap), `stub_last_open_clus()` after the wrap is track 1.
5. Repeat All, 3 albums: after the wrap the first album differs from the last one heard; the
   second pass is again 12 distinct entries with contiguous albums.
6. `album == 0` everywhere (the memset fixture): the walk is a permutation of every entry
   (regression: existing queues with unknown albums still shuffle).
7. `PLAYER_KEEP_QUEUE` under Albums mode walks queue order (Shuffle Songs stays songs).
8. Mode switch mid-play SONGS→ALBUMS: the open cluster does not change, `player_order_keep()` is
   the current index, the walk after it is the rest of the current album first; ALBUMS→SONGS
   re-deals (seed changes) and keeps the track; ALBUMS→ALBUMS (settings_apply re-push) is a no-op
   (seed unchanged).
9. Unbound `order_key = 0xFFFFFFFF` in a group sorts last; two entries with equal `order_key`
   keep queue order.
10. Folders (`is_dir`) never appear; an empty queue with mode ALBUMS is safe
    (`player_set_shuffle(2)` before any queue, `player_order_seed()==0`).
11. Auto-advance (pump-driven, `pump_to_track_end` from `player_queue_test.c`) through one album
    boundary under Albums mode reaches the next album's FIRST track (the prefetch path uses the
    same successor).

**`tests/ui/settings_test.c`** — extend Test 2: activate row 0 three times → 1, 2, 0; each returns
`SETTINGS_ACTION_NONE`; `settings_value(SETTINGS_PLAYBACK, …, 0)` reads `"Off"`, `"Songs"`,
`"Albums"` in turn; `settings_kind` still SELECT.

**`tests/kernel/config_test.c`** — `spicy()` sets `shuffle = 2` (the new range extreme) so the
round trip (`settings_eq`) covers it; hostile-record test (:406-440): also poke `rec[12 + 0] = 7`
and assert `out.shuffle == 0`; add a check that a record with byte 0 == 2 decodes to
`SHUFFLE_ALBUMS` and byte 0 == 1 to `SHUFFLE_SONGS`. The fixture checks (:1240-1290) are
unchanged (host default 0).

**Go** — `cli/internal/devicefs/settings_defaults_test.go` passes unchanged as long as the literal
`0` stays; if `settings_defaults` uses `SHUFFLE_OFF` instead, add it to `cValue()`.

## Docs to update

- `docs/USER_GUIDE.md` — :49-50 (status strip paragraph: "SHUF or SHUF·ALB and RPT tokens when
  shuffle or repeat are on"), :108-109 ("They show as SHUF, SHUF·ALB, RPT or RPT1 in the top
  band"), :129 Playback bullet: "Shuffle Off, Songs or Albums. Songs shuffles the tracks of the
  list you played from. Albums plays the album you are on to its end in track order, then another
  album from that list at random, and so on; Shuffle Songs on the Music menu always shuffles
  songs." Mention that an album queue under Albums simply plays in order.
- `STATUS.md` — a new dated bullet under the current "issue sweep" list (the style of the
  2026-09-14 entries): what changed, the record byte reuse, the +36 KB bss, **UNFLASHED**, and a
  bench list: (a) Songs → play a track with Albums on: the album finishes in tracklist order, the
  next album starts at its track 1; (b) Repeat All wraps to a different album; (c) power off
  mid-album, boot: Next is the album's next track; (d) Settings → Shuffle cycles Off/Songs/Albums
  and the token reads SHUF·ALB; (e) Shuffle Songs from Music still shuffles songs with the
  setting on Albums and shows SHUF; (f) a v0.1.3 record (byte 0 = 0/1) loads unchanged. Also the
  "Settings — nine rows: Playback (shuffle / repeat / resume)" line at :688 → "shuffle
  Off/Songs/Albums".
- `CHANGELOG.md` — no "Unreleased" section exists; add the line at the next release
  (`tools/release.py`), or start an "Unreleased" heading if the job's other plans do.
- `docs/design/settings-persistence.md:155` — the byte-0 row.
- `core/README.md` "Settings and resume" (:150-160) — one clause: "the shuffle byte carries the
  three-way mode since <date>, same record version".
- `docs/screens/render.py` — nothing required; optionally a `screen_nowplaying_albums` still is
  NOT worth a gallery entry.

## Acceptance criteria

1. `make sim && meson test -C build-sim` green, including the new `player-album` suite; `make hw
   && make verify-hw` clean (-Werror, layout, size gate).
2. A record written by v0.1.3 (byte 0 ∈ {0,1}, length 24 or 44) decodes to Off/Songs with every
   other field unchanged; a record with byte 0 = 2 decodes to Albums; any other value → Off.
   Encode never writes a value above 2.
3. In Albums mode every playable entry of the queue plays exactly once per pass; entries with the
   same nonzero `album` are contiguous and ordered by `(order_key, queue index)`; the album of the
   track current at deal time is first when `keep >= 0`.
4. The same `(seed, keep)` over the same queue in the same mode deals the same order
   (`player_reshuffle_with_seed`), which is what a boot resume relies on.
5. Repeat Off ends the queue after the last album; Repeat All re-deals with a different first album
   when the queue holds more than one; a one-album queue loops in order with no track swapped.
6. `player_set_shuffle` with the same mode is a no-op (no re-deal on a volume tick); a mode change
   never reopens the current track.
7. Settings → Playback → Shuffle cycles Off → Songs → Albums → Off; Now Playing shows `SHUF` /
   `SHUF·ALB`; Shuffle Songs from the Music menu shows `SHUF` under either mode.
8. Existing suites pass without edits other than those listed (in particular
   `player_queue_test` §10 and `player_shuffle_test`, whose memset entries carry `album == 0`).
9. User guide, STATUS.md (UNFLASHED + bench list), persistence design doc updated.

## Risks

- **Stale entry fields.** `browse_entry_t` is filled field by field at five sites and never
  memset; a missed site leaves `album`/`order_key` from a previous listing and regroups tracks
  silently. Mitigation: the table in §3 is exhaustive as of dbce3b8 (`grep -n "browse_entry_t e;\|g_browse\[g_browse_n++\]" kernel/main.c`); re-run the grep before merging.
- **Deal cost on the audio path.** Repeat All's re-deal runs inside `prefetch_next` (up to ~6 s of
  ring ahead). Heapsort of 6000 halfwords with a three-key compare is ~150k compares — well under
  a millisecond on the ARM7 at 80 MHz — but the album path also recomputes run lengths (O(n));
  keep everything O(n log n) and integer-only. No allocation.
- **Materialise-in-place proof.** Step 5 depends on every group having ≥1 entry; a bug there
  corrupts `g_order` (never the queue). Test 1/5/6 cover it; add an assert-style check in the
  test that the walk length equals n.
- **Old-build downgrade.** dbce3b8 reads Albums as Songs — acceptable and documented; no build
  rejects the record.
- **Mode/seed skew in the record.** Covered by the `resume_capture()` call in the Settings
  handler; without it a boot within one capture window after a mode change deals a valid but
  different order (not a crash).
- **bss growth** +36 KB (`g_queue` +24 KB, `g_sorted` +12 KB): far inside the 14 MB gate; still
  run `check_size.sh` via `make verify-hw`.
- **Device-only unknowns.** None of this touches the HAL, but the whole feature is unflashed;
  the bench list in STATUS.md is the verification.

## Conflict surface

Shared files/functions another plan in this job is likely to touch — coordinate hunks:

- **`kernel/main.c`** (7344 lines, the merge hot spot). Hunks this plan owns, all small:
  `browse_collect` :581-602; `browse_bind` :3690-3745 (one assignment); `library_play_song`
  :2627-2638; `shuffle_songs_build` :2689-2700; `playlist_play` :3013-3024; `nowplaying_render`
  token block :3466-3478; Settings SELECT handler `SETTINGS_ACTION_NONE` branch :6459-6466; two
  `_Static_assert`s near the includes. Anyone adding a queue builder or a Now Playing header
  element lands on the same lines.
- **`ui/settings.c` / `ui/settings.h`**: Playback row 0 (`settings_value` :285,
  `settings_activate` :355), `settings_t` field at :47, the header comment. Plans that add rows to
  `PLAY_L` must append (row indices 0/1/2 are hardcoded in `settings_value`/`activate` and in
  `tests/ui/settings_test.c`).
- **`kernel/config.c`** payload byte 0 (:308, :385) and the comment at :161. Any plan appending
  record fields works at the tail (`CFG_PAYLOAD_V2Q`) and does not collide, but both change
  `config_test.c`'s `spicy()`/`settings_eq()`.
- **`player/player.h` `browse_entry_t`**: a plan adding its own per-entry field conflicts on the
  struct and on the five fill sites; agree the field order once (this plan: `album` at the padding
  slot, `order_key` after it).
- **`player/player.c`**: `shuffle_deal`, `shuffle_next`, `player_set_shuffle` and the statics block
  :671-708. Nothing else in the transport changes.
- **`tests/meson.build`**: one new `executable`/`test` block after `player-shuffle` (:972-986).
- **Docs**: `docs/USER_GUIDE.md` Playback bullet (:129) and the two token sentences; `STATUS.md`
  top list — other plans append bullets there too; keep each bullet self-contained.
