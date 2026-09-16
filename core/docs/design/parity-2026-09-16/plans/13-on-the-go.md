# 13 — On-The-Go playlist

Status: PLAN (read-only survey of `main` at dbce3b8). Nothing here has been built. Everything
device-facing is UNFLASHABLE in this job and is called out as such.

## Summary

Bring the original iPod's On-The-Go (OTG) playlist to `core`:

- **Hold Select on a song row** (Songs, an artist's All Songs, a genre's songs, an album's
  tracklist, a playlist's tracklist) adds that song to the OTG list; **hold Select on an album
  row** adds the whole album in disc/track order. A one-second banner in the top chrome — the
  Hold banner's own primitive, `top_banner_render` — confirms it: a `+` glyph, "Added to
  On-The-Go", and the new count as the small-caps token.
- **Playlists → On-The-Go** (a pinned first row with the count on the right) opens the list:
  two action rows, **Clear Playlist** and **Save Playlist**, then the tracks in the Songs shape.
  Select on a track plays the OTG list as the queue from that row (queue kind
  `RESUME_KIND_OTG`, so a cold boot comes back into it). Hold Select on a track inside OTG
  removes it. Hold Select on Now Playing still opens the queue view — untouched.
- **Persistence, without creating or growing a file.** Two host-pre-allocated things:
  1. `COREOTG.DAT` in the volume root: the LIVE list as 512 `(folder_hash, file_hash)` pairs in
     two CRC-32'd 5120-byte slots, written through the `cfg_commit` gate exactly like
     `CORECFG.DAT` (idle: debounced, never wakes a parked drive; soft; forced at suspend /
     power-off / disk mode; the DISKSAFE last write). New module `kernel/otg_store.c`,
     modelled on `config.c` + `evlog.c`.
  2. Five slot files `Music/Playlists/On-The-Go 1.m3u8` … `On-The-Go 5.m3u8`, 96 KiB each,
     created by the host tools. **Save Playlist** overwrites the lowest empty slot in place with
     a real extended M3U8 (`#EXTM3U`, a `#CORE-OTG` directive line, absolute
     `/Music/<folder>/<file>` lines, a `#CORE-OTG-END` trailer, newline padding to the file's
     size) and then clears the live list. The Playlists screen lists a used slot like any other
     playlist (it IS one — the existing `library/playlist.c` reader resolves it), hides an empty
     slot, `core sync --prune` keeps them, and a host can copy them straight into its own
     Playlists folder — they are ordinary M3U8 files with two comment lines.
- Host side: `tools/make_otg.py` (create / verify / dump / emit / selftest, the Python oracle)
  and `core sync` (`internal/devicefs` `EnsureOTG` + `EnsureOTGSlots`, byte-identical to the
  oracle, created only when absent, never reset; the syncer refuses a source playlist that would
  land on a slot name; `core doctor` reports both).

Chosen over a single binary container for the saved lists because the Playlists screen, prune,
and the host pull-back all fall out for free from "they are M3U8 files", and because the
project's playlist memo says user playlists are user data, not derived index. The live list IS
session state (like the resume record), so it is binary, small and two-slot atomic.

Plan size: firmware ~1900 lines new (three host-testable modules + tests) and ~350 lines of thin
wiring in `kernel/main.c`; host ~700 lines Python + ~600 lines Go with tests. Seven slices,
each a PR (see "Slices" at the end).

---

## Current behaviour (file:line)

All paths relative to `core/` unless noted.

### Select on list rows is a down-edge action, not press-length arbitrated

- `kernel/main.c:6160-6175` `case SCR_SONGS`: `ev.buttons & WHEEL_BTN_SELECT` →
  `library_play_song(fs, g_song_sel)` at once, then `scr_push(SCR_NOWPLAYING)`.
- `kernel/main.c:6209-6225` `case SCR_PLAYLIST`: SELECT → `playlist_play(g_plt_sel)` at once.
- `kernel/main.c:6227-6285` `case SCR_BROWSER`: SELECT at depth 0 enters the album
  (`browse_load`) or, on the All Songs row (`albumlist_album_at(g_br_sel) < 0`, `main.c:1354`),
  builds the artist's Songs view; at depth 1 `player_play_queue(g_browse, …, g_det_sel, …)`.
- The only press-length-sensitive SELECT today is Now Playing: the down-edge is recorded at
  `main.c:6335-6337` (`g_sel_down_us`, `g_sel_pending`), and the per-pass block at
  `main.c:6604-6640` decides: held ≥ `SEL_HOLD_US` (450 ms, `main.c:661`) → `scr_push(SCR_QUEUE)`;
  released → toggle the scrubber. It predates `ui/keyhold.c` and is hand-rolled.
- PLAY is the model to copy: `keyhold_t play_key` (`main.c:5641`), fed with LIVE button state at
  `main.c:5843-5872` (`keyhold_feed(&play_key, down, nowp, PLAY_HOLD_US)`), TAP → pause, HOLD →
  suspend. `keyhold_swallow_tap` at `main.c:5968, 5999` when the press woke the backlight or
  dismissed a modal. `keyhold_reset` on a Hold edge (`main.c:5896`).
- `ui/keyhold.h` / `ui/keyhold.c`: `keyhold_feed` returns at most one action per press; a
  swallowed tap still reports HOLD; after HOLD the release is silent. Tested in
  `tests/ui/keyhold_test.c`.

### Playlists today (read path only)

- `library/playlist.h:55-57`: `PLAYLIST_DIR "Playlists"`, `PLAYLIST_MAX 64`,
  `PLAYLIST_TRACKS_MAX BROWSE_MAX` (= 128, `player/player.h:24`). `playlist_t` carries the
  ext-trimmed filename, `clus`, `size`, `hash = name_hash(name)` (the resume context key).
- `library/playlist.c:78-97` `collect_cb`: lists by extension only (`.m3u8` / `.m3u`), never
  looks inside a file; `playlist_scan` (`:99-149`) sorts A–Z; `playlist_resolve` (`:153-260`)
  parses with `m3u_parse_file` into `scr->ent[PLAYLIST_TRACKS_MAX]` and walks each path with
  `fat32_resolve_path`, caching the last folder. Missing / unplayable / io / rejected are counts.
- `fs/m3u.h`: `M3U_PATH_MAX 191`, `M3U_LINE_MAX 512`, `M3U_FILE_MAX 1 MiB`; blank lines are
  ignored, `#` lines are directives/comments (`#EXTM3U`, `#EXTINF` parsed, everything else
  ignored); any byte < 0x20 rejects a line (`skipped_bad`).
- `kernel/main.c:2882-3115`: `g_playlists[PLAYLIST_MAX]`, `g_pl_tracks[PLAYLIST_TRACKS_MAX]`,
  `g_pl_song[]` (bound `g_songs` index or −1), `g_pl_scratch` (~35 KB .bss), `playlists_load`
  (`:2922`, re-scans on every entry), `playlist_bind_row` (`:2946`, bucket chain
  `g_song_hh[file_hash & …]` within `dir_clus`, cluster settles it), `playlist_open` (`:2974`),
  `playlist_play` (`:3001`: sets `g_queue_kind = RESUME_KIND_PLAYLIST`,
  `g_queue_ctx_hash = g_playlists[g_pl_open].hash`, `player_queue_begin/add/commit`, per-row art
  via `album_by_clus`), `playlists_row_draw` (`:3031`, single-line `list_row` with chevron),
  `playlists_render` (`:3036`), `playlist_row_draw` (`:3068`, Songs shape via
  `list_row_titled`), `playlist_render` (`:3084`, `LIST_ROWS2`).
- Entry points: main menu `MM_PLAYLISTS` (`main.c:6071-6073`) and Music menu `MU_PLAYLISTS`
  (`:6116-6118`), both `playlists_load(fs); scr_push(SCR_PLAYLISTS)`.
- Screen enum `main.c:3627-3630` (`SCR_PLAYLISTS, SCR_PLAYLIST` already in), stack
  `SCR_STACK_MAX 12` (`:3638`), `paint_current_screen` (`:4698`), the render switches
  (`:4707-4708`, `:6874-6875`), the list-view table (`:3990-4050`, `case SCR_PLAYLIST` at
  `:4015`), `list_sel_initial` (`:4194`, Songs only).
- `STATUS.md:700-705` "What's NOT done, 1. Playlists — write path … the write path is a
  filesystem project." This plan is the answer that avoids the filesystem project.

### The library's identity model (what an OTG entry can be)

- `lib_song_t` (`main.c:1566-1597`): `file[]` (on-disk stem after bind), `file_hash` (name_hash
  of the FULL on-disk name, the record↔file locator), `stem_hash` (resume locator), `dir_clus`,
  `file_clus`, `file_size`, `duration_s`, `track`, `disc`, `genre`. No folder hash on the song.
- `lib_album_t` (`main.c:429-440`): `folder[]` is the index's DISPLAY folder (tag album where it
  only differs by FAT sanitisation — so NOT reliably the on-disk name), `clus`, art. No hash.
- `g_folder_map[FOLDER_MAP_MAX]` (`main.c:1613-1622`): on-disk folder `name` (capped at 63
  chars), `clus`, `hash = name_hash(full on-disk name)` — built once at index load
  (`library_load_index`, `main.c:2243`; `folder_map_index` `:1798`; `folder_clus_h` `:1810`)
  and NEVER reused afterwards, so it is available all session. The index record itself carries
  `u32 folder_hash` at 248 and `u32 file_hash` at 252 (`main.c:2216, 2331-2335`;
  `tools/build_index.py:71, 349, 399`).
- `resolve_art_cb` (`main.c:2058-2105`) binds a record to a dirent by `file_hash` within
  `dir_clus`, tiebreak `name_bind_exact`. `playlist_bind_row` repeats the rule.
- The resume record (`ui/settings.h:98-108` `RESUME_KIND_*`, `RESUME_KIND_MAX = 6`;
  `kernel/resume_ctx.h`; `kernel/config.c:192-203` `P_RES_KIND … P_RES_CTX`): a kind byte and a
  `ctx_hash` word. `config_decode` maps an unknown kind to NONE (`config.c:430-431`), so an older
  build reading a new kind falls back to the album, and `config_encode` writes kinds ≤ MAX
  (`:336`). `resume_capture` `main.c:4348-4386`; `resume_restore` switch `main.c:4632-4640`;
  `resume_open_playlist` `main.c:4548-4583` is the template for an OTG restore.

### The write discipline (what any new writer must copy)

- `kernel/config.h:47-60` geometry (`CONFIG_SLOT_BYTES 1024 = ATA_PHYS_LOG*512`, two slots),
  `config.c:125-153` record layout (magic/version/length/seq/payload/CRC over `[0,1020)`),
  `config_slot_lba` (`:470-525`: re-resolve through `fat32_file_lba`, first cluster only,
  alignment, bounds, never LBA 0), `config_load` (`:610-725`: root enumeration by 8.3 short
  name, settle-and-retry, both slots read via the RAW block callback, newest valid seq wins,
  unreadable+no-valid ⇒ not writable), `config_save` (`:758-799`: alternate slot, re-resolve,
  encode, `ata_write_sectors` (issues FLUSH CACHE), bookkeeping only on success).
- `kernel/evlog.c/.h`: the second writer; injects `evlog_write_fn` / `evlog_wake_fn` at mount so
  the host test drives a RAM disk; spans clusters with `fat32_file_lba_at` (`fs/fat32.h:324`:
  byte offset multiple of 512, `*max_sectors` = what remains in THAT cluster).
- `kernel/cfg_commit.h/.c`: modes IDLE / SOFT / FORCE / LAST; the parked rule (IDLE and SOFT never
  wake), the battery gate, `CFG_GATE_WRITE_WAKE`, retry-on-failure bounded by
  `CFG_SAVE_MAX_FAILURES`. `main.c:3128-3190` `g_cfg_commit`, `settings_touch`,
  `settings_commit(mode)`; `evlog_commit` `:3202`. Commit sites: idle `main.c:6590`
  (`settings_commit(0)`), Settings exit SOFT `:6501`, disk mode `:6447`, suspend `:5171-5172`,
  standby `:4937-4940`, DISKSAFE LAST `:900-901`. `cfg_commit_clear` at boot `:5621`.
- `hal/hw/ata.h:150-167` `ata_write_sectors(lba, count, buf)`: lba AND count multiples of
  `ATA_PHYS_LOG` (2), count ≤ 256 (`ata.c:728`), buffer 16-bit aligned.
- `fs/fat32.c:74-113`: ONE FAT sector cache and ONE data sector cache (4096 B each), **no write
  invalidation** — safe today only because both writers read their files through the raw
  callback. An M3U8 slot file written by the firmware IS read back through `fat32_stream_read`
  (the playlist parser), so this plan adds `fat32_cache_drop()`.
- Host halves: `tools/make_config.py` (`encode/decode/do_create/do_emit/do_verify`, `--create`
  never resets a valid file, `--emit` is the meson fixture at `tests/meson.build:522-531`),
  `tools/make_log.py` (`--selftest` under meson at `tests/meson.build:1074-1085`);
  `cli/internal/devicefs/config.go` (`EncodeConfigSlot`, `DecodeConfigSlot`, `ConfigFileValid`,
  `EnsureConfig` `:247`, `ConfigFileValid` `:216`, `DecodeConfigSlot` `:169`), `log.go` (`EnsureLog` `:101`), `m3u.go` (`WriteM3U8`, `PathMax 191`,
  `PlaylistEntry` → `/Music/<folder>/<file>`), `devicefs.go:28-33` names.
- `cli/internal/syncer/syncer.go`: `Plan` (`:176-215`, `Config`/`Log` bools), keep-set
  (`:305`, playlists added `:409-420`), `planPrune` (`:604-660`: inside `Music/Playlists/`
  every file not in the keep set is pruned), `configValid`/`logValid` (`:672-700`);
  `playlist.go` `planPlaylists` (`:29-100`, `devicePlaylistName`, dup-name warning);
  `execute.go` order (`:60-72`), playlists phase (`:216-232`), prune (`:238-252`), device files
  (`:256-282`, `EnsureConfig` then `EnsureLog`), index last. `cli/internal/doctor/doctor.go`
  `checkConfig` `:391`, `checkPlaylists` `:447`. `cli/internal/cli/sync.go` flags `:111-116`.

### The confirmation primitive

- `main.c:4804` `top_banner_h()` (23 on Now Playing, strip+header through the divider on lists /
  Settings); `main.c:4825-4873` `top_banner_render(inverted, bm, bn, bm_dy, label, token)` — 14-wide
  bitmap, bold-13 label ellipsised to the room left of the small-caps token; `lock_banner_render`
  `:4877`; bitmaps `LOCK_BM_CLOSED/OPEN`; `ui_window_t` + `ui_window_arm/up` `main.c:106-119`;
  `g_lock_flash` `:636`, `LOCK_FLASH_US 1 s` `:686`; the banner render block `main.c:6720-6752`
  (paint context + banner once, present the band only, `continue`, repaint underneath on fade);
  the unlock banner is dismissed by the first real input at `main.c:5939-5955`.
- `docs/screens/render.py:995` `top_banner`, `:1042` `lock_banner`, `:1555` `screen_playlists`,
  `:2008` `main()`.

---

## Design

### 1. RAM model — `library/otg.h` / `library/otg.c` (pure, host-tested)

```c
#define OTG_MAX          512u
#define OTG_SLOTS        5u                 /* saved-playlist slot files            */

typedef struct {
    uint32_t folder_hash;   /* name_hash(full on-disk album folder name)  — index rec @248 */
    uint32_t file_hash;     /* name_hash(full on-disk file name, ext incl.) — index rec @252 */
} otg_entry_t;

typedef struct {
    uint16_t    n;
    uint16_t    gen;             /* bumps on every mutation; written into the slot header
                                  * and into the m3u8 directive on save                */
    otg_entry_t e[OTG_MAX];
} otg_list_t;                    /* 4 + 4096 = 4100 B .bss */

void otg_init(otg_list_t *l);                                   /* n = 0, gen = 0 */
int  otg_add(otg_list_t *l, uint32_t folder_hash, uint32_t file_hash);
                                /* 1 added; 0 full (n == OTG_MAX) or hash pair (0,0) */
int  otg_add_many(otg_list_t *l, const otg_entry_t *e, int n);  /* returns how many fit */
int  otg_remove(otg_list_t *l, int idx);                        /* 1 removed; 0 bad idx */
void otg_clear(otg_list_t *l);
```

Duplicates are allowed (the original iPod allows them; a playlist may list a track twice and
`playlist_resolve` already copes). `otg_add` refuses only the null pair. Order is insertion
order; nothing sorts.

**Why a hash pair and not a path.** The pair is exactly the locator CORELIB.IDX already uses to
bind a record to its file (`main.c:2331-2335`), so it survives an index rebuild and a re-sync
that keeps names, and it costs 8 bytes per entry — the whole list is one 5 KiB slot. A path
would cost 192 bytes per entry (96 KiB per slot, 200 KiB file) for a list that is session
state, and the device would still have to resolve it by walking directories at every boot. The
host-portable form is produced at Save time, where it belongs.

**Binding** (device side, lives in `main.c` because `g_songs`/`g_folder_map` do):
`int16_t g_otg_song[OTG_MAX]` — `otg_bind_all()` after library load / list load, and
`otg_bind_one(i)` after an add. Resolution of entry `i`:
`dc = folder_clus_h(folder_hash, "")` (`main.c:1810`; the empty name means "first in the chain"
on a fold collision — accepted), then the `g_song_hh[file_hash & (SONG_HASH_BUCKETS-1)]` chain
with `s->dir_clus == dc && s->file_hash == file_hash`, first match. Unresolved entries stay in
the list (a later sync may bring the file back), render greyed with sub-line "Not on this iPod",
are skipped when the queue is built, and are counted (`g_otg_missing`) for the banner and the
tracklist header.

`otg_entry_of_song(si)`: `file_hash = g_songs[si].file_hash`; `folder_hash` = the
`g_folder_map[]` entry whose `clus == g_songs[si].dir_clus` (linear over ≤ 1024, once per add;
no new field on `lib_album_t`). A song whose folder is not in the map (scan-path libraries with
`g_folder_n == 0`) cannot be added: banner "Not in the library" — the tag-scan fallback has no
folder hashes and is not a supported layout for OTG.

### 2. On-disk format A — `COREOTG.DAT` (the live list) — `kernel/otg_store.h` / `.c`

Volume root, 8.3 name `COREOTG.DAT`, found by root enumeration exactly as `cfg_root_cb` finds
`CORECFG.DAT` (`config.c:581-608`). Host creates it at **32768 bytes** (one stock cluster);
device accepts any size ≥ `OTG_STORE_MIN_BYTES`.

```
#define OTG_SLOT_BYTES     5120u   /* 5 physical sectors = 10 LBAs; a multiple of 1024 */
#define OTG_SLOTS_ON_DISK  2u
#define OTG_STORE_MIN_BYTES (OTG_SLOT_BYTES * 2)        /* 10240 */
#define OTG_MAGIC          0x47544F43u                   /* 'C''O''T''G' LE */
#define OTG_VERSION        1u

slot (5120 bytes, little-endian)          offset  size
  magic       'C''O''T''G'                   0      4
  version     1                              4      2
  count       0..512                         6      2
  seq         monotonic write counter        8      4   (wrapping; config_seq_newer rule)
  gen         otg_list_t.gen at save        12      2
  flags       reserved, 0                   14      2
  entries     count × { u32 folder_hash, u32 file_hash }  16   4096 (512 × 8; unused zero)
  reserved    0                           4112      4
  crc32       zlib CRC-32 over [0, 5116)  5116      4
```

Slot 0 at byte 0, slot 1 at byte 5120. Every byte of an unused entry is 0 and the CRC covers
the whole slot, so padding is deterministic (same reasoning as `config.c:141-145`).

Rules, copied from `config.c` and `evlog.c` and not relaxed:

- LBA re-resolved before EVERY read and write with `fat32_file_lba_at(fs, first_clus,
  byte_offset, &lba, &max)` per **cluster run**: a slot that straddles a cluster boundary
  (clusters < 8 KiB) is read/written as two runs, each ≤ `max`, each with `lba % ATA_PHYS_LOG == 0`
  and `(offset/512) % ATA_PHYS_LOG == 0`; never LBA 0, never ≤ `part_lba`, never past the run.
  A resolve failure disables the store for the session (`otg_store_writable() == 0`).
- Reads via the RAW block callback `fs->read` (never the cached file path), with the same
  settle-and-retry (`CONFIG_READ_RETRIES` / `_RETRY_MS`) and the same "unreadable + no valid ⇒
  not writable" rule.
- Two slots alternate; `seq` decides; a torn write damages only the slot not being read.
- Write and wake are injected function pointers (`otg_store_mount(fs, write, wake)`) as in
  `evlog_mount`, so the host test drives a RAM disk and records every write. `main.c` passes
  `ata_write_sectors` and `ata_wakeup`.
- Its own `cfg_commit_t g_otg_commit` and `otg_store_touch()` / `otg_store_commit(mode)` in
  `main.c`, one line each beside `settings_touch` / `settings_commit`, same env gathering, same
  UART narration (`core: otg save rc … seq …`). The idle commit rides next to
  `settings_commit(0)` at `main.c:6590`; the forced sites (`:900`, `:4940`, `:5172`, `:6447`)
  and the SOFT Settings exit (`:6501`) each gain the matching `otg_store_commit(mode)`. A change
  made with the drive parked therefore lands when the platters next turn or at the next forced
  commit — the same stated trade-off as `cfg_commit.h`.
- Boot: `otg_store_mount` right after `config_load` (both need only `fs`), decode the newest
  slot into `g_otg` (count > 512 ⇒ reject slot), `cfg_commit_clear(&g_otg_commit)`. Binding
  happens after `library_ensure`, before `resume_restore`. UART: `core: otg load <n> writable <w>
  seq <s> lba <slot0>/<slot1>` — the line `make_otg.py --verify` must match before the FIRST
  device write (the `config.c` banner procedure, owed by this THIRD writer).

API (host-testable, `tests/kernel/otg_store_test.c`):

```c
typedef int (*otg_write_fn)(uint32_t lba, uint32_t count, const void *buf);
typedef int (*otg_wake_fn)(void);
int      otg_store_mount(fat32_t *fs, otg_write_fn w, otg_wake_fn wake, otg_list_t *out);
                                     /* 1 = a valid slot was loaded into *out */
int      otg_store_writable(void);
int      otg_store_save(const otg_list_t *l);        /* 0 ok, negative refused/ATA rc */
uint32_t otg_store_seq(void);
int      otg_store_probe_lba(uint32_t slot, uint32_t *lba);
void     otg_slot_encode(uint8_t *slot, const otg_list_t *l, uint32_t seq);   /* codec, pure */
int      otg_slot_decode(const uint8_t *slot, otg_list_t *l, uint32_t *seq);
```

`otg_slot_decode` clamps nothing (hash pairs have no range) but rejects magic/version/`count >
OTG_MAX`/CRC, and zero pairs inside `count` are dropped (a hand-edited file cannot inject a
null entry).

### 3. On-disk format B — the saved slot files `Music/Playlists/On-The-Go N.m3u8`

Five files, N = 1..5, **98304 bytes** each (96 KiB = 3 stock clusters; 512 entries × the
192-byte worst case path line + headers always fits). The host creates them; the device only
overwrites bytes inside them. Device requirements on a slot file: size ≥ `OTG_SLOT_FILE_MIN`
(4096) and a multiple of 1024; otherwise the slot is "unusable" (never written, listed as a
plain playlist if it has entries).

Contents written by the firmware (and by the host when creating an EMPTY slot):

```
#EXTM3U\n
#CORE-OTG v1 count=00012 crc=1A2B3C4D gen=00042\n        <- fixed 46 bytes: zero-padded decimals
/Music/Artist - Album/03. Title.flac\n                    <- count lines, exact on-disk names
…
#CORE-OTG-END gen=00042\n                                  <- fixed 24 bytes
\n\n\n… (newlines to the end of the file)
```

- Every other M3U8 reader sees two comments and blank lines. `fs/m3u.c` ignores `#` lines that
  are not `#EXTM3U`/`#EXTINF` and ignores blank lines, so `library/playlist.c` reads a used slot
  unchanged. `\n` padding, not NUL: a NUL byte would be `skipped_bad` per line
  (`m3u.h` ROBUSTNESS).
- `count` = entry lines written; `crc` = zlib CRC-32 over the entry lines' bytes (from the byte
  after the directive's `\n` to the `\n` ending the last entry) — for the host and for
  `--verify`; `gen` = the OTG list's `gen` at save time, and it appears twice.
- **Torn-write detection on the device** is cheap and needs no CRC pass: the trailer's `gen`
  must equal the header's `gen` AND the parser's `listed` must equal `count`. The write order
  makes any tear fail one of the two (below). A damaged slot lists as "On-The-Go N", opens to
  the empty state "Playlist damaged — save again", and counts as FREE for the next Save.
- **Empty slot** = `count=00000`: hidden from the Playlists list (see §6) and free for Save.
- **What the firmware writes as the path.** Exact on-disk names, not display names:
  `library_ensure`'s folder map (`g_folder_map[].name`) is capped at 63 chars and `lib_song_t.file`
  has lost its extension, so the writer walks: `fat32_readdir(lib_root)` to find the folder
  whose `first_clus == dir_clus` (its full `name`, `name_lossy` ⇒ unsaveable), then
  `fat32_readdir(dir_clus)` for the file whose `first_clus == file_clus`. Both walks are cached
  on the last folder, so an album-grouped run costs one root walk + one album walk. Worst case
  (512 entries, all different albums) ≈ 512 × ~6 directory reads ≈ 1-3 s on the device, shown
  under the load bar (`load_bar_progress("SAVING PLAYLIST", pct)`). Prefix is `"/Music/"` on the
  documented layout, `"/"` on the no-`Music/` layout (`playlists_base_dir`'s rule).
- A path over `M3U_PATH_MAX` (191) or a lossy name is not written: counted `unsaveable`, and the
  banner says "Saved 10 of 12".

**Write order (the atomicity argument).** The file is streamed through a 4096-byte staging
buffer (`OTG_SAVE_STAGE 4096`, a multiple of 1024) in one pass that formats lines and updates
the CRC as it goes: stage 0 (header + the first ~40 lines) is **held in RAM**, stages 1..K are
written as they fill (each via `fat32_file_lba_at` per cluster run), the padding stages follow,
the trailer goes wherever the last entry ended, and **stage 0 is written last** with the final
`count`/`crc`. A power cut before the last write leaves the OLD stage 0 (old header gen, old
first lines) over NEW tail lines: `gen` mismatch ⇒ damaged. A cut mid-tail leaves the new header
over a mix: `listed ≠ count` or trailer gen mismatch ⇒ damaged. A cut after everything but
FLUSH: `ata_write_sectors` issues FLUSH CACHE per call, and the last call is stage 0, so a
completed final write is durable. `fat32_cache_drop(fs)` is called after the last write (new
`fs/fat32.h` API: sets both `*_cache_valid = 0`; one line each, one test).

API — `library/otg_slot.h` / `.c` (host-tested against an in-RAM volume, write fn injected):

```c
#define OTG_SLOT_FILE_MIN   4096u
#define OTG_SAVE_STAGE      4096u
#define OTG_SLOT_NAME_FMT   "On-The-Go %u"          /* the ext-trimmed playlist name */

int  otg_slot_index(const char *playlist_name);       /* 1..5 for "On-The-Go N", else 0 */
void otg_slot_name(char *dst, int n);                 /* "On-The-Go N" (no extension)   */

typedef struct { uint16_t count, gen; uint32_t crc; uint8_t present, damaged; } otg_slot_info_t;
int  otg_slot_probe(fat32_t *fs, uint32_t clus, uint32_t size, otg_slot_info_t *out);
        /* reads the first 512 B (fat32_read_file); present = the directive line was found */
int  otg_slot_verify(fat32_t *fs, uint32_t clus, uint32_t size, uint32_t listed,
                     otg_slot_info_t *out);           /* + the trailer: damaged = gen/count mismatch
                                                       * — reads the last used stage only */

typedef struct { uint32_t dir_clus, file_clus; } otg_save_row_t;   /* main.c fills from g_songs */
typedef struct { uint32_t written, unsaveable, io_err; uint8_t truncated; } otg_save_stats_t;
typedef struct { uint8_t stage0[OTG_SAVE_STAGE], stage[OTG_SAVE_STAGE]; fat32_dirent_t de;
                 char folder[FAT32_NAME_BYTES]; uint32_t folder_clus; } otg_save_scratch_t;
int  otg_slot_save(fat32_t *fs, uint32_t clus, uint32_t size, const char *root_prefix,
                   const otg_save_row_t *rows, int n, uint16_t gen,
                   otg_write_fn write, otg_save_scratch_t *scr, otg_save_stats_t *st);
        /* 0 ok; negative: size bad, resolve failed, write failed (stats say where) */
```

`otg_slot_save` is the FOURTH caller of the disk write path and owes the `config.c` banner
procedure too: `make_otg.py --verify` prints every slot file's first LBA and cluster chain; the
firmware prints `core: otg slot N lba <first>` at the first Save of a session. Untestable here.

### 4. Host-tool changes

**`tools/make_otg.py`** (new; the Python oracle, mirrors `make_config.py` / `make_log.py`):

- `--create MOUNTPOINT`: `COREOTG.DAT` (32 KiB: slot 0 = a valid EMPTY record at seq 1, slot 1
  zero) and `Music/Playlists/On-The-Go 1..5.m3u8` (96 KiB each, the empty form above with
  `count=00000 crc=00000000 gen=00000`). Existing files: a `COREOTG.DAT` with a valid slot is
  left alone (`--force` resets); a slot `.m3u8` that EXISTS is ALWAYS left alone (it may hold a
  saved list, or a user's own playlist that happens to use the name — say so).
  `--size`/`--slot-size` as `make_config.py --size`.
- `--verify DEVICE`: read-only; MBR → FAT32 BPB (the ×4 ambiguity as `make_config.py`), root walk
  for `COREOTG.DAT`, prints both slot LBAs and decodes both slots; then walks `Music/Playlists`
  for the five slot files and prints each one's first-cluster LBA and chain length. The device's
  `core: otg load … lba A/B` and `core: otg slot N lba X` lines MUST match.
- `--dump FILE`: decode a pulled `COREOTG.DAT` (both slots, newest marked) and/or a slot `.m3u8`
  (header/trailer/count/crc/gen consistency, the entries).
- `--emit FILE` (two-slot region of `COREOTG.DAT`, slot 0 valid at seq 1 holding THREE known
  pairs, slot 1 zero) and `--emit-slot FILE` (an empty 8 KiB slot m3u8) — meson fixtures the
  firmware decoders must accept; `--selftest` (encode/decode round-trip, CRC vectors) under
  `meson test` as `make_log.py --selftest` is.

**`tools/make_config.py`**: comment only — `resume_kind` values now go to 7 (`RESUME_KIND_OTG`).

**`tools/README.md`**: a `make_otg.py` row; the `make_config.py` row's list of writers.

**`core/cli` (Go)** — `internal/devicefs/otg.go` (+ `otg_test.go`):

- Constants: `OTGName = "COREOTG.DAT"`, `OTGSlotBytes = 5120`, `OTGMinBytes = 10240`,
  `OTGFileBytes = 32*1024`, `OTGMagic 0x47544F43`, `OTGVersion 1`, `OTGMax 512`,
  `OTGPlaylistSlots = 5`, `OTGSlotFileBytes = 98304`, `OTGSlotFileMin = 4096`,
  `OTGSlotName(n) = fmt.Sprintf("On-The-Go %d.m3u8", n)`, `OTGDirective = "#CORE-OTG v1 "`.
- `EncodeOTGSlot(entries [][2]uint32, seq uint32, gen uint16) [OTGSlotBytes]byte`,
  `DecodeOTGSlot([]byte) (seq uint32, gen uint16, entries [][2]uint32, ok bool)`,
  `OTGFileValid(head []byte) (seq uint32, ok bool)`, `EnsureOTG(volumeRoot) (created bool, err)`
  — `EnsureConfig`'s shape (`config.go:247`): absent/short/no-valid-slot ⇒ write fresh;
  valid ⇒ untouched.
- `EmptyOTGSlotFile(size int) []byte`, `ParseOTGSlotHeader(head []byte) (count int, crc uint32,
  gen uint16, ok bool)`, `OTGSlotFileState(path) (state: absent | empty | used | damaged | foreign)`
  (foreign = an `.m3u8` at that name with no directive: a user's playlist, never touched),
  `EnsureOTGSlots(musicDir) (created []string, err)` — creates each of the five ONLY when absent.
- `ReadOTGSlotFile(path) (entries []string, info, err)`: the pull-back primitive (verifies
  count/crc/gen, strips the two directives and the padding) — implemented and tested because it
  is what the host pull needs and it doubles as the test's read-back; NOT wired to a command in
  this plan.
- `internal/syncer`: `Plan.OTG bool` / `Plan.OTGSlots []string` (what `Execute` will create);
  `planPlaylists`: a source playlist whose device name is a slot name (`otg_slot_index > 0`) is
  **refused with a warning** ("`On-The-Go 1.m3u8` is a device slot; rename the playlist") and not
  planned; the keep-set gains all five slot paths (`keep.add(filepath.Join(musicDir, PlaylistDir,
  OTGSlotName(n)))`) so `planPrune` never lists them whether or not they exist; `Execute` device-files
  phase: `EnsureOTG(o.Dst)` then `EnsureOTGSlots(musicDir)` after `EnsureLog`, before the index;
  `Report.OTGCreated bool`, `Report.OTGSlotsCreated int`; dry run writes nothing and reports the
  plan. `execute.go`'s order comment and `cli/sync.go`'s summary printer gain the two lines.
- `internal/doctor`: `checkOTG` (file present/valid/newest seq/entry count, like `checkConfig`)
  and `checkPlaylists` marks each slot as `slot N: empty | 12 tracks | DAMAGED | foreign`.
- `internal/installer` (the "afterwards it creates … CORECFG.DAT, CORELOG.BIN and an empty
  Music\" step, `cli/README.md:248`) and `internal/app/backend.go:273` ("Flash, then create …")
  call `EnsureOTG` + `EnsureOTGSlots` too, so a fresh install has them.

### 5. UI — key map per screen

Threshold: `SEL_HOLD_US` (450 ms, existing). Arbitration: ONE `keyhold_t row_key` beside
`play_key`, fed every pass with the live SELECT state while a row press is pending. Mechanics
(thin, in `main.c`, ~60 lines):

1. In the event switch, for the four screens below, the `WHEEL_BTN_SELECT` down-edge no longer
   acts. It records `g_rowsel = { scr, depth, sel, list_epoch }`, sets `g_rowsel_pending = 1`, and
   calls `keyhold_feed(&row_key, 1, now, SEL_HOLD_US)` once so the press origin is the event's
   own tick (the latch can be a pass ahead of `clickwheel_buttons()`).
2. A per-pass block next to the Now Playing one (`main.c:6604`): if pending and (`scr_cur()`
   differs, or `g_list_epoch` moved, or `g_locked`) ⇒ drop (`keyhold_reset`). Else
   `keyhold_feed(&row_key, live_select_down, now, SEL_HOLD_US)`: `KEYHOLD_TAP` ⇒
   `row_select_tap(fs)` (the four SELECT bodies, MOVED into one function, keyed on the recorded
   screen/depth/sel); `KEYHOLD_HOLD` ⇒ `row_select_hold(fs)` (below), then the rest of the press
   is silent (keyhold's rule). Wheel motion while a press is pending does not cancel it (the
   thumb rocks); the recorded `sel` is what acts.
3. A press whose down-edge woke the backlight, dismissed a modal, or landed under Hold never
   reaches the switch (those sites zero `ev.buttons`), so it is never pending. On a Hold edge
   `keyhold_reset(&row_key)` beside `play_key`'s.

Screens and rows:

| Screen (state) | Select tap (unchanged action, now on release) | Select hold (new) | Menu | Right / Left | Wheel |
|---|---|---|---|---|---|
| Songs / All Songs / a genre (`SCR_SONGS`) | play the view from the row → Now Playing | **add the song**; banner "Added to On-The-Go" · token `N SONGS` | back | Right → Now Playing (existing rule) | move |
| Album list (`SCR_BROWSER`, depth 0) | enter the album | **add the whole album** (its `g_songs` with `dir_clus == album.clus`, sorted disc, track; unresolved ones skipped) — banner "Added 12 songs" · token `ON-THE-GO`. On the **All Songs** row: nothing (no banner) | back | same | move |
| Album tracklist (`SCR_BROWSER`, depth 1) | play the album from the row | **add the track** (`g_browse_song[g_det_sel]`; −1 ⇒ banner "Not in the library"; a Disc header row ⇒ nothing) | album list | same | move |
| A playlist's tracklist (`SCR_PLAYLIST`, a file OR a saved On-The-Go N) | play the playlist from the row | **add the track** (`g_pl_song[g_plt_sel]`; −1 ⇒ "Not in the library") | Playlists | same | move |
| Playlists (`SCR_PLAYLISTS`) | row 0 "On-The-Go" → `SCR_OTG`; other rows → open the playlist | nothing | back | same | move |
| **On-The-Go (`SCR_OTG`, new)** | row 0 **Clear Playlist**: first press re-labels the row "Clear? Select again" for 3 s (`ui_window_t g_otg_confirm`); second press within the window clears (banner "Cleared", stay on the empty state). Row 1 **Save Playlist**: writes the lowest free slot, banner "Saved as On-The-Go 2" (or "Saved 10 of 12 as …"), clears the live list, pops to Playlists with the cursor on the new row. Greyed when the list is empty (Clear/Save) or all 5 slots used (Save; label "Save Playlist · 5 of 5 used"). Rows ≥ 2: play the OTG list as the queue from that row (`RESUME_KIND_OTG`) → Now Playing | rows ≥ 2: **remove the row**; banner "Removed" · token `N SONGS`; selection stays on the same index (clamped). Rows 0-1: nothing | Playlists | same | move |
| A saved On-The-Go N tracklist (`SCR_PLAYLIST` with `otg_slot_index(name) > 0`) | play from the row | add the track (as any playlist) | Playlists | same | move |
| Now Playing | tap = scrubber | **hold = queue view** (unchanged, `main.c:6604-6640`) | back | skip | volume / seek |
| Queue view | jump | nothing (unchanged) | back | skip | move |

The `SCR_OTG` list uses `LIST_ROWS2` / `ROW_H2` rows in the Songs shape (title, artist,
duration) via `list_row_titled`; the two action rows are single-line `list_row` height inside the
same list — simpler: render every row at `ROW_H2` and draw the action rows as title-only rows
("Clear Playlist", "Save Playlist") with no sub-line. Header: "On-The-Go", right value
`"<sel-1> / <n>"` for track rows and `"<n> songs"` on the action rows; when `g_otg_missing > 0`
the header right value is `"<n> · <m> missing"`. Empty state: "On-The-Go is empty" /
"Hold Select on a song to add it" (FONT_SMALL muted, as `playlists_render`'s empty state).
Initial cursor: row 2 when tracks exist (no accidental Save), row 0 otherwise.

The Playlists list: `playlists_render` draws a pinned first row "On-The-Go" (chevron, right value
= `g_otg.n` as text) before the scanned files; `g_pl_sel` 0 is that row, files start at 1.
The list-view table entry for `SCR_PLAYLISTS` (`main.c:4008`) counts `g_playlists_n + 1`. The
original iPod puts On-The-Go LAST; pinned first is chosen because 64 playlists is a long spin
to the bottom and the row is the most-used one. `resume_open_playlist`'s `g_pl_sel = pi` becomes
`pi + 1`.

**Playing the OTG list** — `otg_play(int start)` next to `playlist_play` (`main.c:3001`): same
loop over `g_otg` skipping unresolved entries, `g_queue_kind = RESUME_KIND_OTG`,
`g_queue_seed = 0`, `g_queue_ctx_hash = 0`, each entry with its album's art via `album_by_clus`.
After Save Playlist, if the queue that is playing IS the OTG list (`g_queue_kind ==
RESUME_KIND_OTG`), re-point it: `g_queue_kind = RESUME_KIND_PLAYLIST`, `g_queue_ctx_hash =
name_hash("On-The-Go N")` — the queue is a copy inside the player, so playback continues and the
next resume lands in the saved list. Clear Playlist under a playing OTG queue leaves the player
alone (its copy plays on); resume then falls back to the album, which is correct.

### 6. Playlists screen and the slot files

- `playlist_scan` (`library/playlist.c:78-97`): `collect_cb` keeps listing by extension; a new
  optional filter runs after the sort — for each `playlist_t` with `otg_slot_index(name) > 0`,
  `otg_slot_probe`; `present && count == 0` ⇒ removed from the list (5 single-sector reads at
  most). Done inside `playlist_scan` behind a new `int hide_empty_slots` argument (0 keeps the
  old behaviour for existing callers/tests), so it is host-tested with the rest of the scan.
- `playlist_open` for a slot: after `playlist_resolve`, `otg_slot_verify(listed)`; damaged ⇒
  `g_pl_err = OTG_EDAMAGED` and the empty-state text "Playlist damaged — save again". A used slot
  otherwise behaves exactly as a file playlist (resume kind PLAYLIST, ctx = `name_hash("On-The-Go
  N")`, `resume_open_playlist` unchanged).
- The saved-list tracklist gets a trailing action row **Delete Playlist** (only when
  `otg_slot_index > 0`): Select empties the slot (writes the empty form, same writer) and pops to
  Playlists. Without it, five saves would dead-end the feature until a host sync; the original
  iPod has no delete, so this is the one addition. Same two-press confirm as Clear.

### 7. `PLAYLIST_TRACKS_MAX` 128 → 512

A saved 512-entry list must open whole. `library/playlist.h:57` decouples from `BROWSE_MAX`:
`#define PLAYLIST_TRACKS_MAX 512`. Costs: `g_pl_tracks` 512 × 84 B = 43 KB, `g_pl_scratch.ent`
512 × 264 B = 135 KB, `g_pl_song` 1 KB — ~180 KB more .bss (12.18 MB → ~12.37 MB against
`BSS_MAX` 14 MB, `tests/scripts/check_size.sh:46`). `playlist_play` already enqueues per row
(no `BROWSE_MAX` dependence). The playlist test's `BIG_LINES 130` fixture
(`tests/library/playlist_test.c:174`, assertion `:435`) becomes `PLAYLIST_TRACKS_MAX + 2` with
the RAM image sized to hold it (the chain at `:201` grows accordingly).

### 8. Resume

- `ui/settings.h`: `RESUME_KIND_OTG = 7`, `RESUME_KIND_MAX = RESUME_KIND_OTG`. `resume_ctx_store`
  stores `ctx = 0` for it (only PLAYLIST keeps a ctx hash). `config.c` needs no byte change; its
  comment at `:192` and `make_config.py` `CTX_FIELDS` comment and `devicefs/config.go`'s list the
  new value.
- `resume_capture` (`main.c:4348`): unchanged — `g_queue_kind` carries OTG.
- `resume_restore` (`main.c:4632`): `case RESUME_KIND_OTG: ok = resume_open_otg(fs, si);`.
  `resume_open_otg`: the list is already loaded and bound; find the row by song (`g_otg_song[i]
  == si`, `resume_qidx` as the first guess since the queue index equals the OTG index only when
  nothing before it was unresolved — so try the hint, then the scan); `otg_play(idx) == idx &&
  resume_landed()`; anything else falls back to the album as every kind does.
- `tests/kernel/resume_test.c` / `check_resume_parity.py`: `resume_find_song` is untouched; the
  resume test gains `resume_kind_of_view`-style coverage only for the enum ceiling
  (`RESUME_KIND_MAX == 7`, `resume_ctx_store` zeroes ctx for OTG).

### 9. Index rebuilt / entries that no longer resolve

Binding is recomputed at every boot and after every `library_ensure`; an entry that resolves to
nothing is kept, drawn greyed with "Not on this iPod", skipped by `otg_play` and by Save
(counted `unsaveable`), and counted in the header. No entry is ever silently dropped from the
persisted list by the device; only Clear / remove / Save do that. A host re-sync that renames
files invalidates them the same way it invalidates a saved playlist's paths — the same rule.

### 10. Confirmation visual

`otg_banner_render(kind)` beside `lock_banner_render` (`main.c:4877`): `top_banner_render(0,
OTG_BM_PLUS, 16, 0, label, token)` — a 14×16 "+" in a circle bitmap (`OTG_BM_PLUS`, new, drawn in
the same rows-of-bits style as `LOCK_BM_*`), and for remove `OTG_BM_MINUS`. Label / token pairs:
"Added to On-The-Go" / `12 SONGS`; "Added 12 songs" / `ON-THE-GO`; "Removed" / `11 SONGS`;
"On-The-Go is full" / `512`; "Not in the library" / (none); "Cleared" / (none); "Saved as
On-The-Go 2" / `12 SONGS`; "Saved 10 of 12 as On-The-Go 2" / (none); "Could not save" / (none).
`OTG_FLASH_US 900000`. `ui_window_t g_otg_flash` + `otg_flash_kind`.

Render: the block at `main.c:6720-6752` becomes `if (lock up) … else if (otg up) …` with the
same paint-context-then-band present and the same halts; unlike the LOCKED banner it is not
modal: the input drain (`main.c:5939`) disarms it on the first button or wheel event (the
UNLOCK banner's rule), so nothing is applied unseen. A Hold edge while it is up: the lock banner
wins (its window is tested first). The gallery (`docs/screens/render.py`) gets `otg_banner(sc,
label, token, screen)` over `top_banner` with the two bitmaps ported, and the stills
`otg.png` (the On-The-Go screen) and `otg_added.png` (the banner over Songs), plus
`playlists.png` re-rendered with the pinned row.

---

## Files to change

### Firmware

New:
- `library/otg.h`, `library/otg.c` — the list model (§1). ~120 lines.
- `library/otg_slot.h`, `library/otg_slot.c` — slot naming, directive/trailer format + parse,
  probe/verify, the streaming M3U8 writer with directory-exact names (§3). ~450 lines.
- `kernel/otg_store.h`, `kernel/otg_store.c` — `COREOTG.DAT` mount/load/save, slot codec, LBA
  resolution per cluster run, injected write/wake (§2). ~420 lines. `core/meson.build:307-308`
  lists the library sources (`names.c`, `idx.c`, `sort.c`, `playlist.c`) — add `otg.c` and
  `otg_slot.c` there; `otg_store.c` goes where `config.c`/`evlog.c` are listed for the hw target.
- `tests/library/otg_test.c`, `tests/library/otg_slot_test.c`, `tests/kernel/otg_store_test.c`,
  `tests/fs/fat32_cache_test.c` (or a case in `fat32_test.c`).

Modified:
- `fs/fat32.h` / `fs/fat32.c` — `void fat32_cache_drop(fat32_t *fs);` (both caches invalid when
  `*_cache_fs == fs`); the `fat32.c:96-109` comment updated to name the third and fourth writers
  and this API.
- `library/playlist.h` / `.c` — `PLAYLIST_TRACKS_MAX 512`; `playlist_scan(..., int hide_empty_slots)`
  + the probe filter; a `PLAYLIST_EDAMAGED` code is NOT needed (main.c keeps the verdict).
- `ui/settings.h` — `RESUME_KIND_OTG`, `RESUME_KIND_MAX`. `kernel/resume_ctx.h` comment.
- `kernel/config.c` — comments at `:192-203` (kind list). No byte change.
- `kernel/main.c` (thin wiring, every site named):
  - includes for the three headers; `.bss`: `otg_list_t g_otg`, `int16_t g_otg_song[OTG_MAX]`,
    `int g_otg_missing`, `cfg_commit_t g_otg_commit`, `otg_save_scratch_t g_otg_save_scr` (~8.3 KB),
    `int g_otg_sel, g_otg_accum`, `ui_window_t g_otg_flash, g_otg_confirm`, `keyhold_t row_key`
    (a main-loop local like `play_key`), `g_rowsel*`.
  - `screen_t` (`:3627`): `SCR_OTG` after `SCR_PLAYLIST`. Stack depth arithmetic comment: MENU,
    PLAYLISTS, OTG, NOWPLAYING, QUEUE + modal = 6, inside 12.
  - `otg_store_touch/commit` beside `settings_touch/commit` (`:3130-3190`); calls at `:900`,
    `:4940`, `:5172`, `:6447`, `:6501`, `:6590`; `cfg_commit_clear(&g_otg_commit)` beside `:5621`.
  - boot: `otg_store_mount(fs, ata_write_sectors, ata_wakeup, &g_otg)` after `config_load`;
    `otg_bind_all()` after `library_ensure` in the boot path and inside `library_ensure`'s tail
    (`:2543`, after `build_artists`); UART lines.
  - `otg_entry_of_song`, `otg_bind_one/all`, `otg_add_song(si)`, `otg_add_album(ai)`,
    `otg_play`, `otg_save_to_slot`, `otg_slot_free_index`, `resume_open_otg`, `otg_banner_render`,
    `otg_render` / `otg_row_draw`, `row_select_tap`, `row_select_hold`.
  - `playlists_row_draw/render` and the list-view table (`:4008`) for the pinned row;
    `playlist_render`'s empty-state text and the Delete Playlist row; `playlist_open`'s verify;
    `playlists_load` passes `hide_empty_slots = 1`; `resume_open_playlist` `g_pl_sel = pi + 1`.
  - event switch: `SCR_SONGS`, `SCR_PLAYLIST`, `SCR_BROWSER` SELECT bodies move into
    `row_select_tap`; `SCR_PLAYLISTS` SELECT handles row 0; new `case SCR_OTG`.
  - the per-pass row-hold block beside `:6604`; the banner block `:6720-6752`; the input-drain
    disarm at `:5939`; `keyhold_reset(&row_key)` at `:5896`; `paint_current_screen` `:4698`,
    render switches `:4707`, `:6874`.
  - `list_sel_initial` unchanged (no A-Z on OTG).
- `tests/meson.build` — four new test executables + `make_otg.py --emit` / `--emit-slot`
  fixtures (as `:522-531`) + `make-otg-selftest` (as `:1074-1085`); `playlist_test` unchanged
  wiring.
- `tests/library/playlist_test.c` — cap fixture; slot-file hiding cases.
- `tests/kernel/resume_test.c` — enum ceiling case.
- `tests/hw_mmio/lcd_present_test.c` — only if the banner band rect changes (it does not:
  `top_banner_h()` is reused).

### tools/*.py
- `tools/make_otg.py` — new (§4).
- `tools/make_config.py` — comment (kind 7).
- `tools/README.md` — the row.

### core/cli (Go)
- `internal/devicefs/devicefs.go` — names; `otg.go`, `otg_test.go` (golden vs `make_otg.py --emit`
  when python3 is on PATH, as `config_test.go:80` does; round-trip; rejects; `EnsureOTG`
  idempotent/rewrites-invalid as `ensure_test.go`; `EnsureOTGSlots` creates-only-absent, leaves a
  foreign file; `ReadOTGSlotFile` on a used, empty, damaged and foreign file).
- `internal/syncer/syncer.go`, `playlist.go`, `execute.go`, `syncer_test.go`.
- `internal/doctor/doctor.go` (+ its test).
- `internal/installer/installer.go`, `internal/app/backend.go` (the post-flash create step).
- `internal/cli/sync.go` (summary lines), `cli/README.md` (sync/doctor/install rows).

---

## Tests to add

### Host C suites (meson, `suite: 'unit'`)

**`otg_test`** (`tests/library/otg_test.c` + `library/otg.c`): add/remove/clear; full at 512
returns 0 and leaves n; `otg_add_many` returns the partial fit; remove keeps order; duplicates
allowed; null pair refused; `gen` bumps on every mutation and not on a refused add.

**`otg_slot_test`** (`tests/library/otg_slot_test.c` + `library/otg_slot.c`, `library/playlist.c`,
`library/names.c`, `fs/m3u.c`, `fs/fat32.c`): the `playlist_test.c` in-RAM VFAT volume
(`build_image`, `put_lfn`) extended with `On-The-Go 1.m3u8` (empty form, 8 KiB, 2 clusters so a
stage straddles), `On-The-Go 2.m3u8` (used, 3 entries), `On-The-Go 3.m3u8` (torn: header gen 5,
trailer gen 4), `On-The-Go 4.m3u8` (foreign: a plain playlist under the name), a >63-char album
folder and a `.fla` file; a recording write fn that REFUSES any LBA outside the slot file's own
clusters:
- naming: `otg_slot_index("On-The-Go 3") == 3`, `"On-The-Go 6"`, `"on-the-go 1"` (case-insensitive
  → 1), `"On-The-Go"`, `"On-The-Go 1.m3u8"` (name is ext-trimmed: 0).
- probe: empty/used/torn/foreign classification; count/crc/gen parse; a directive at line 1
  (no `#EXTM3U`) still parses; a directive past the first 512 B is "not present".
- `playlist_scan(hide_empty_slots=1)` hides slot 1 and lists 2, 3, 4; `=0` lists all.
- save: exact bytes of the whole file (header line 46 B, entries in order with the EXACT on-disk
  names from the dirents — the long folder's row is `unsaveable`, the `.fla` row is written as
  `.fla`), trailer, `\n` padding to the last byte; stage 0 is the LAST write recorded; every write
  is `≤ max_sectors` of its cluster and `ATA_PHYS_LOG`-aligned; `count`/`crc` equal a reference
  computed in the test; refusal on size 4095, on size not a multiple of 1024, on a write fn error
  (stats.io_err, negative return); the `truncated` flag when entries outgrow the file.
- round trip: `playlist_resolve` over the just-written slot yields the same rows in order
  (through `fat32_cache_drop`, and the test asserts the STALE result without it — the fs test
  below pins the API itself).
- verify: gen mismatch and `listed != count` both report `damaged`; a good slot does not.

**`otg_store_test`** (`tests/kernel/otg_store_test.c` + `kernel/otg_store.c`,
`kernel/cfg_commit.c`, `library/otg.c`, `fs/fat32.c`): the `evlog_test.c` volume (BytesPerSector
2048, part_lba 64, `COREOTG.DAT` FRAGMENTED so slot 1 straddles clusters):
- codec: round-trip 0/1/512 entries; rejects magic, version 0/2, count 513, a flipped bit in
  header / entries / padding / CRC; zero pairs inside count dropped; `config_seq_newer`
  semantics reused (wrap).
- LBA: both slots' every run equals the test's own formula over the chain; a run never crosses
  a cluster; misaligned base refused; short file refused; absent file ⇒ not writable.
- mount: newest seq wins; torn slot loses; both bad ⇒ writable, first save lands slot 0;
  unreadable + no valid ⇒ not writable; transient read error retried.
- save: alternation; a failed write leaves seq/slot untouched; the fixture from `make_otg.py
  --emit` decodes to the three known pairs (host/device parity).
- gate (through `cfg_commit_gate` with a hand clock, as `cfg_commit_test.c`): IDLE debounced and
  never wakes parked; SOFT never wakes; FORCE wakes (the wake fn is recorded before the write);
  battery refuses IDLE/SOFT/FORCE, not LAST; three failures drop the change.

**`fat32_cache`** (a case in `tests/fs/fat32_test.c`): read a sector through the file path, mutate
the RAM disk, read again (stale), `fat32_cache_drop`, read again (fresh); the FAT cache the same.

**`playlist_test`**: `BIG_LINES = PLAYLIST_TRACKS_MAX + 2` and the `:435` assertion; nothing else.

**`resume_test`**: `RESUME_KIND_MAX == RESUME_KIND_OTG`; `resume_ctx_store` with kind OTG stores
ctx 0 whatever is passed.

**`keyhold_test`**: no change needed — the row press uses the existing contract; add one case
documenting "a wheel tick during a pending press does not end it" only if the main.c block ends
up owning that rule (it does not; keyhold ignores the wheel).

**meson fixtures**: `otg_golden.bin` (`--emit`), `otg_slot_empty.m3u8` (`--emit-slot`),
`make-otg-selftest`.

### Python
`tools/make_otg.py --selftest`: encode/decode round trip, CRC vector, slot header
format/parse, `--dump` of a synthetic torn slot reports damage.

### Go (`go test ./...` from `core/cli`)
- `devicefs`: `TestEncodeOTGSlotMatchesMakeOTG` (skips without python3), `TestOTGSlotRoundTrip`,
  `TestOTGSlotRejects`, `TestEnsureOTGCreatesAndIsIdempotent`, `TestEnsureOTGRewritesInvalid`,
  `TestEnsureOTGSlotsCreatesOnlyAbsent` (a pre-existing foreign `On-The-Go 2.m3u8` is byte- and
  mtime-untouched), `TestReadOTGSlotFile` (used / empty / damaged / foreign).
- `syncer`: `TestOTGFilesAreCreatedOnce` (fresh sync creates 6 files; second run reports none;
  dry run creates none), `TestPruneKeepsOTGSlots` (`--prune --yes` with a used slot 3 and an empty
  slot 1 leaves both; an unrelated stale playlist is still pruned), `TestSourcePlaylistOnSlotNameIsRefused`
  (warning, not planned, slot untouched), `TestFreshSyncWritesEverythingAndIndexLast` extended
  with the new files' order (before the index).
- `doctor`: OTG line OK/FAIL cases; slot states.

---

## Docs to update

- `docs/USER_GUIDE.md`: Controls table — Select row: "On a song or album, hold: adds it to
  On-The-Go"; a new **On-The-Go** subsection under Browsing (add, the banner, the list, Clear /
  Save with the confirm press, five saved lists `On-The-Go 1..5`, Delete Playlist, what
  "Not on this iPod" means); Playlists paragraph (the pinned row; the slot files are ordinary
  M3U8 files you can copy off); "Putting music on it" step 5 → "Three files it writes to" +
  `tools/make_otg.py --create`; "Disk mode and the files on the drive" lists `COREOTG.DAT` and
  the five slots; the `core sync` paragraph (creates them once, never resets, refuses a playlist
  named like a slot, prune keeps them).
- `README.md`: What it does — "Browses…" bullet gains On-The-Go; "Remembers" bullet gains the
  live list; Status paragraph drops "writing playlists" from "Not there"; Layout unchanged.
- `STATUS.md`: a dated "What works" entry (UNFLASHED), item 1 of "What's NOT done" rewritten
  (the write path exists for pre-allocated files; general playlist editing still does not), and
  three first-flash checklist items: (a) `make_otg.py --verify` LBAs vs the two UART lines BEFORE
  the first add / Save, then raw read-back + `chkdsk`/`fsck -n`; (b) add / remove / clear survive
  a power-off; (c) Save, open the saved list, resume into it after a power cut.
- `core/README.md`: "Playlists — read only" → "Playlists" with the OTG paragraph and the four
  writers; "Settings and resume" mentions `RESUME_KIND_OTG`.
- `core/docs/design/on-the-go.md` (new): §1-§3 of this plan as the format reference, with the
  rejected container alternative recorded.
- `tools/README.md`, `core/cli/README.md` (sync / doctor / install rows), `CHANGELOG.md`
  (Unreleased).
- `docs/screens/render.py` + the three stills; `docs/screens/README.md` if it lists them.

---

## Acceptance criteria

Host (all must hold before any device work):
1. `make sim && meson test -C build-sim` green with the four new suites, the extended
   `playlist`/`resume`/`fat32` cases, `make-otg-selftest`, and both fixtures decoding.
2. `make hw && make verify-hw` clean under `-Werror`; `.bss` ≤ 12.5 MB reported by
   `check_size.sh` (was 12.18 MB); text growth < 40 KB.
3. `go test ./...` green in `core/cli`; `TestEncodeOTGSlotMatchesMakeOTG` passes with python3.
4. `core sync --dry-run` on a fixture tree lists `COREOTG.DAT` + the five slots as "will create";
   a second real sync creates nothing and `--prune --yes` deletes none of them; a used slot's
   bytes are untouched by ten syncs.
5. `make_otg.py --verify` on a raw image made from a synced volume prints slot LBAs that equal
   what the C test's formula gives for the same image (a Python-vs-C parity script is NOT added;
   the two are compared by hand on the image once and recorded in `STATUS.md`).

Behavioural (device — UNVERIFIABLE in this job; they are the bench list):
6. Hold Select on a Songs row: the banner reads "Added to On-The-Go · 1 SONGS" within ~450 ms,
   the finger's release does nothing, and Playlists → On-The-Go shows the row. Tap still plays.
7. Hold on an album row adds its tracks in disc/track order; on All Songs nothing happens.
8. Inside On-The-Go: Select on a track plays the list from there, TRACK N OF M counts only
   resolved rows; hold removes; Clear needs two presses; Save writes `On-The-Go 1.m3u8`, the
   banner names it, the live list is empty, Playlists lists "On-The-Go 1" and opens it whole.
9. Power off (hold Play 5 s) and cold boot: the live list is back; resume lands in the OTG queue
   on the same track, paused; after a Save the next resume lands in the saved playlist.
10. `fsck.vfat -n` / `chkdsk` clean after ten adds, one Save, one Delete; the raw read-back of
    `COREOTG.DAT` shows alternating slots; the slot m3u8 opens in a desktop player.
11. Adding with the drive parked does not spin it up (UART shows no wake until the next forced
    or platter-turning event); Save does wake it (explicit action) and shows the load bar.
12. With `COREOTG.DAT` absent, everything works for the session and nothing persists; the UART
    says `otg load 0 writable 0`. With no free slot, Save is greyed and says why.

---

## Risks

- **Third and fourth disk writers.** Every `config.c` banner rule is copied, not reinterpreted:
  re-resolve per write, raw reads, alignment, bounds, fail closed. The per-cluster-run writing
  in both new writers is the one piece `config.c` does not have; `evlog.c`'s
  `fat32_file_lba_at` is the proven primitive and the RAM-disk tests refuse any write outside the
  file's own clusters. Still: nothing here is device-verified in this job, and the two
  `--verify`-vs-UART checks are mandatory before the first add.
- **Select-on-release changes the feel of every list.** A tap now acts ~on release (the
  original iPod's behaviour). The album-enter disk read moves by one press length. If the bench
  finds it laggy, the threshold can drop to 350 ms without touching the code shape.
- **Stale sector cache.** `fat32_cache_drop` is new and easy to forget in a future writer; the
  `fat32.c` comment names the rule and the test pins the API. The OTG store itself reads raw.
- **Slot file semantics collide with a user's own "On-The-Go N.m3u8".** Handled on both sides:
  the syncer refuses to plan one; the device treats a directive-less file as a plain playlist
  (listed, playable, never written to — Save skips a foreign slot, as does Delete).
- **`.bss` growth** (~200 KB: playlist cap ×4, the list, scratch). Within budget; recorded.
- **`PLAYLIST_TRACKS_MAX` 512 lengthens every playlist open's worst case** (512 directory walks
  for a pathological file) — already bounded and reported; unchanged behaviour for real files.
- **Unresolved entries after a re-sync** show as greyed rows rather than vanishing; a user who
  renames their whole library will see a list of "Not on this iPod". Clear fixes it; the
  alternative (silent drop) hides data loss, which the project rejects elsewhere.
- **`g_folder_map` dependency** for the folder hash: the tag-scan fallback library
  (`library_scan`) leaves `g_folder_n == 0`, so OTG cannot add there ("Not in the library"). The
  documented layout always has an index.
- **Banner arbitration.** Two windows can be armed (Hold edge during an OTG banner): the lock
  banner is tested first and the OTG one simply expires underneath; the disarm-on-input rule
  means no press is ever applied invisibly.

## Conflict surface

Files other in-flight plans are likely to touch at the same time — merge by hand, in this order:
- `kernel/main.c`: the event switch `SCR_SONGS` / `SCR_PLAYLIST` / `SCR_BROWSER` SELECT bodies
  (moved, not edited — anyone changing what Select does on those rows must edit
  `row_select_tap`), the `screen_t` enum + `paint_current_screen` + both render switches + the
  list-view table (any new screen collides here), the banner render block `:6720-6752` and the
  input-drain disarm `:5939` (anything else that wants a top banner), the commit sites
  (`:900`, `:4940`, `:5172`, `:6447`, `:6501`, `:6590`) and the boot sequence around
  `config_load` / `library_ensure` / `resume_restore`, the playlists section `:2882-3115`.
- `ui/settings.h` `RESUME_KIND_*` (any new queue kind) and `kernel/resume_ctx.h`.
- `library/playlist.h/.c` (the cap and `playlist_scan`'s signature — a search/browse plan
  listing playlists must pass the new argument).
- `fs/fat32.h/.c` (the new API; any other fs change).
- `tests/meson.build` (appends only; take both sides).
- `core/cli/internal/syncer/{syncer,playlist,execute}.go`, `internal/devicefs/devicefs.go`,
  `internal/doctor/doctor.go`, `internal/installer`, `internal/app/backend.go` — the
  library-manager plan (L1-L7) edits the same files; land this plan's Go slice after or rebase
  onto it.
- `docs/USER_GUIDE.md` controls table, `STATUS.md` top and checklist, `docs/screens/render.py`
  `main()` and the banner section, `CHANGELOG.md` Unreleased — every UI plan touches these.

## Slices (one PR each, in order)

1. `fs`: `fat32_cache_drop` + test. `library/otg.c` model + test. `PLAYLIST_TRACKS_MAX 512` +
   fixture. `RESUME_KIND_OTG`. (Pure, no behaviour change on device.)
2. `kernel/otg_store.c` + test + `tools/make_otg.py` (`--create/--verify/--dump/--emit/--selftest`)
   + meson fixture wiring. Boot mount + UART line in `main.c` (no UI yet).
3. `library/otg_slot.c` + test (probe/verify/save/round-trip) + `playlist_scan` hiding.
4. `main.c` UI: row keyhold, add/remove, the banner, `SCR_OTG`, the pinned row, Clear/Save/Delete,
   `otg_play`, resume kind, commit wiring. Gallery stills. Guide + STATUS + CHANGELOG.
5. Go: `devicefs/otg.go` + tests; syncer/doctor/installer/app wiring + tests; README rows.
6. Design doc `core/docs/design/on-the-go.md`; `core/README.md`, `tools/README.md`.
7. (Bench, outside this job) the two `--verify` checks, then the acceptance list 6-12.
