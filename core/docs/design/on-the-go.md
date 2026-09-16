# On-The-Go — the two on-disk formats

Status: **implemented, UNFLASHED.** The modules below exist and are
host-tested (`core/library/otg.c`, `core/library/otg_slot.c`,
`core/kernel/otg_store.c`, `tools/make_otg.py`, `core/cli/internal/devicefs`),
and `kernel/main.c` wires the whole feature. **Neither write path has been
verified on hardware** — see the bring-up procedure at the top of
`kernel/otg_store.c`, which is `kernel/config.c`'s and is not optional for a
third and fourth writer, and the bench list in `STATUS.md`.

This is the FORMAT reference. The behaviour a user sees — hold Select to add,
the banner, the Playlists pinned row, Clear / Save / Delete, resuming into the
list — is `docs/USER_GUIDE.md`'s On-The-Go section.

## What On-The-Go is, and why it needs two formats

The original iPod's On-The-Go is a playlist you build while walking around:
hold Select on a song and it joins a list you can play, clear, or save. So
there are two different things to keep, and they are different KINDS of thing:

- **The live list** is session state, like the resume position. It changes on
  a button press, it is small, it must survive a power cut, and nobody outside
  the device will ever read it. It is binary, two-slot atomic, and it lives in
  a pre-allocated file in the volume root.
- **A saved list** is user data. It must survive a firmware switch, be
  readable on a desktop, and be copyable off the device. It is an ordinary
  extended M3U8 file in `Music/Playlists/`, which is where every other
  playlist already lives.

Both exist because the firmware **cannot create, grow, move or delete a file**
(`core/fs/fat32.c` is read-only apart from an in-place data overwrite). So the
host creates both, once, and the device only ever overwrites bytes inside
them. `tools/make_otg.py --create` and `core sync` do that, and neither ever
resets one that already holds something.

### The alternative that was rejected

A single binary container for the saved lists — five fixed slots of hashes
inside one more pre-allocated file — is smaller and simpler to write. It was
rejected because everything the M3U8 form gets for free would have had to be
built: the Playlists screen would need a second kind of row, `playlist_scan`
and `playlist_resolve` a second code path, `core sync --prune` a special case,
and a user who wanted a list on their desktop would have had nothing to copy.
The project's playlist memo already says user playlists are user data and not
derived index (`core/fs/m3u.h` says the same); a saved On-The-Go list is a
playlist the user made, and it should be a playlist.

## Format A — `COREOTG.DAT`, the live list

Volume root, 8.3 name, found by root enumeration exactly as `CORECFG.DAT` is.
The host creates it at 32 KiB (one stock cluster); the device accepts any size
at or above `OTG_STORE_MIN_BYTES` (10240) and addresses only the first two
slots.

```
slot (5120 bytes, little-endian)          offset  size
  magic       'C''O''T''G'                   0      4
  version     1                              4      2
  count       0..512                         6      2
  seq         monotonic write counter        8      4   (wrapping)
  gen         the list's mutation counter    12     2
  flags       reserved, 0                    14     2
  entries     count x { u32 folder_hash,     16   4096
                        u32 file_hash }
  reserved    0                            4112     4
  padding     0                            4116  1000
  crc32       CRC-32 over [0, 5116)        5116     4
```

Slot 0 at byte 0, slot 1 at byte 5120. The slots alternate; the valid one with
the newer `seq` wins, compared with the signed-difference rule so a wrap reads
as +1 (`config_seq_newer`'s rule, restated in `otg_store.c` as `seq_newer`).

**An entry is a pair of folded name hashes**, not a path: the folder hash and
the file hash of the full on-disk names, which is exactly the locator
`CORELIB.IDX` already binds a record to its file by (record offsets 248 and
252). That costs eight bytes, so the whole 512-entry list is one slot. A path
would cost 192 bytes an entry — a 200 KiB file for session state — and the
device would still have to walk directories to resolve it at every boot.

**Why 5120 and not one sector.** 512 entries need 4096 bytes; with the header
and the CRC that is more than one physical sector, and the stock drive
(MK8010GAH) returns IDNF for any access smaller than one physical sector, so a
slot has to be a whole number of them. 5120 = 5 × 1024 is the smallest that
fits.

**Why that means a slot can straddle a cluster.** `config.c` deliberately
never follows a chain — its record fits inside the first cluster. A 5120-byte
slot does not, on a volume with small clusters, so `otg_store.c` resolves per
**cluster run** with `fat32_file_lba_at()` (the primitive `evlog.c` already
proved) and reads or writes a slot as one or more runs, each inside the
cluster it resolved to, each physical-sector aligned. A slot whose runs do not
ALL resolve is refused whole rather than half-written.

**Rules copied from `config.c` and not relaxed:** every address re-resolved
before every read and every write, reads through the raw block callback (never
the cached file path), the settle-and-retry at boot, "a slot the drive would
not read plus no valid record anywhere ⇒ not writable for the session", and
the CRC over the whole slot including the header, because `seq` is what
decides which slot wins.

**The commit gate lives in the module.** `otg_store_commit(mode, env, list)`
runs `cfg_commit_gate` with the module's own `cfg_commit_t` — evlog.c's shape.
IDLE is debounced and never wakes a parked drive, SOFT skips the debounce and
still never wakes one, FORCE pays the spin-up through the injected wake, and
`CFG_COMMIT_LAST` (the DISKSAFE edge) is the one write exempt from the battery
gate. A change made with the drive parked lands the next time the platters
turn, or at the next forced commit — the same stated trade-off as
`cfg_commit.h`.

## Format B — `Music/Playlists/On-The-Go 1..5.m3u8`, the saved lists

Five files, created by the host at 128 KiB each. The device only overwrites
bytes inside them, and only when a slot file's size is at least
`OTG_SLOT_FILE_MIN` (4096) **and** a multiple of 1024 — so every staged write
is a whole number of physical sectors. Anything else is "unusable": never
written to, and simply listed as whatever playlist it is.

```
#EXTM3U\n                                                      8 B
#CORE-OTG v1 count=00012 crc=1A2B3C4D gen=00042\n             48 B
/Music/Artist - Album/03. Title.flac\n                  <- count lines
...
#CORE-OTG-END gen=00042\n                                     24 B
\n\n\n...                                    <- to the last byte of the file
```

- `count` — entry lines actually written, five zero-padded decimals.
- `crc` — zlib CRC-32 of the entry lines' bytes, eight upper-case hex digits.
  The HOST checks it (`make_otg.py --dump`, `core doctor`); the device does
  not need to.
- `gen` — the live list's mutation counter at save time, written **twice**.

Everything else reads it as two comments and some blank lines: `fs/m3u.c`
ignores a `#` line that is not `#EXTM3U`/`#EXTINF` and ignores blank lines, so
`library/playlist.c` opens a used slot with no special case at all.

**Why the padding is newlines and not NUL.** `m3u.c` rejects any line holding
a byte below 0x20, so a NUL run would be reported as thousands of bad lines. A
blank line is ignored, by specification.

**Why the file is always written whole.** The device cannot truncate, so
saving a shorter list over a longer one must overwrite the old tail — or the
entries past the new trailer would still be there, and `m3u.c` (which ignores
`#` lines but not paths) would happily play them.

**128 KiB, not 96.** A line is `/` + the canonical path + `\n`, and the
canonical path is capped at `M3U_PATH_MAX` (191), so a full list's worst case
is 512 × 193 + 80 = 98896 bytes. 96 KiB is 592 bytes short of that; 128 KiB is
four stock clusters with room to spare.

### The write order, and why it is the whole tear-detection scheme

The file is streamed through a 4096-byte staging buffer in ONE pass that
formats the lines and accumulates the CRC as it goes. **Stage 0 is held in
RAM**; stages 1..K are written as they fill, then the padding, and **stage 0
is written LAST**, once the final count and CRC are known (which is only
possible because every field in the header line is fixed width, so patching
count and crc cannot move a byte).

So a power cut leaves one of exactly two states, and both are detectable with
two small reads and the line count the parser already produced — no CRC pass:

- **cut before the last write** — the OLD stage 0 (old header gen, old first
  lines) sits over the NEW tail: the header's `gen` no longer equals the
  trailer's;
- **cut mid-tail** — the old header sits over a mix: the trailer is missing or
  carries the old gen, and the parser's line count does not equal the header's
  `count`.

A completed final write is durable because the injected writer issues FLUSH
CACHE and stage 0 is the last call.

A damaged slot is never believed: it is listed as "On-The-Go N" and opens to
"Playlist damaged — save again" with **Delete Playlist** under it, which is
how it is recovered. It is **not** free for the next Save.

That is load-bearing, not a simplification. Save only ever writes into an
EMPTY slot — a file that is nothing but its two directive lines and padding —
so it never writes content over content, and the one tear the gen/count test
cannot see is therefore unreachable: a new entry line that happened to be
exactly as long as the old line it landed on would leave a file whose count
still matched its lines. Delete rewrites the file WHOLE, so a damaged slot
comes back through the same one-pass writer as everything else.

### Naming a row

The device writes **exact on-disk names**, read back out of the directory
entries at save time — so the format is codec-agnostic by construction: a
`.mp3` row is written `.mp3` and a `.fla` row `.fla`, and what a line PLAYS as
is `classify_ext`'s answer on the read side, not anything the writer decided.
The names come from the dirents because `lib_song_t.file` has lost its extension and
`g_folder_map[].name` is capped at 63 characters, and a path that is not
byte-exact names nothing. Both walks are cached on the last folder, so an
album-grouped run costs one root walk plus one album walk.

A row is counted `unsaveable` — never silently dropped — when its folder is
not in the library root, when a long name is lossy (unmatchable, as
`resolve_art_cb` treats it), when the canonical path would exceed
`M3U_PATH_MAX`, or when a name holds a byte the parser would reject. The
banner can then say "Saved 10 of 12" instead of the user finding out on a
train.

### The fourth writer and the sector caches

`fs/fat32.c` keeps one FAT sector and one data sector cached and has **no
write invalidation**, which was safe while every writer read its own file
through the raw block callback. A slot playlist is read back through
`fat32_read_file` and `fat32_stream_read`, so it is not. `fat32_cache_drop()`
exists for that and `otg_slot_save()` calls it before returning; removing the
call fails three checks in `tests/library/otg_slot_test.c`.

## Empty, used, damaged, foreign

| state | what it is | Playlists list | Save |
|---|---|---|---|
| empty | `count=00000` | hidden | free |
| used | `count>0`, gen and count agree | listed, opens | in use |
| damaged | trailer gen or line count disagrees | listed, opens to "Playlist damaged — save again" | NOT free — Delete first |
| foreign | an `.m3u8` at a slot name with no `#CORE-OTG` line | listed, plays | skipped |

"Foreign" is a playlist of the user's own that happens to use the name. The
device never writes to it, and that is enforced in two places rather than
one: `otg_slot_of()` is what the UI asks before it offers **Delete Playlist**
(a NAME says only which slot a file could be — the `#CORE-OTG` directive is
what says it is one), and `otg_slot_save()` / `otg_slot_erase()` refuse a
directive-less target themselves, so the promise does not depend on a caller
remembering it. `core sync` refuses to plan a source playlist onto a slot
name (it warns and copies nothing); `core doctor` says which slot it is.

The five slot files always exist, so `playlist_scan(..., hide_empty_slots)`
drops the empty ones after the sort — otherwise the Playlists screen would
show five playlists nobody made, each opening to nothing. A damaged or foreign
one is kept: the first is something the user saved and needs to be told about,
the second is theirs.

## Resume

`RESUME_KIND_OTG` (7, `ui/settings.h`) is the queue kind for the live list.
It stores **no context hash**: the list is not a file and has no name, and the
boot path rebuilds the queue from the list `COREOTG.DAT` restored.
`resume_ctx_store` already zeroes the hash for every kind but PLAYLIST, so
this needed no codec change — only the enum's ceiling moved, and
`config_decode` maps an unknown kind to NONE, so an older build reading a
record written by this one falls back to the album.

A SAVED On-The-Go list is an ordinary file and resumes as
`RESUME_KIND_PLAYLIST` with `name_hash("On-The-Go N")` as its context.

## The host halves

`tools/make_otg.py` is the reference implementation of both formats:
`--create` (never resets a valid `COREOTG.DAT` without `--force`, and NEVER
touches an existing slot playlist), `--verify` (every cluster run of both
slots and of the five slot files, to be compared against the firmware's UART
line BEFORE the first write), `--dump`, `--emit` / `--emit-slot` (the meson
fixtures the firmware's own decoders must accept) and `--selftest`.

`core/cli/internal/devicefs/otg.go` is the Go half, byte-identical and tested
against the Python; `core sync` creates both files once, keeps all five slot
paths out of prune whether or not they exist, and refuses a source playlist
that would land on one; `core doctor` reports `COREOTG.DAT` and each slot's
state; `core install` creates them on a fresh device.
