# Settings persistence — design (for review; no code yet)

Status: **proposal**. This documents how `core` will persist `settings_t`
across reboots before any disk-write code lands. Nothing here is implemented.

## Problem

`settings_t` (shuffle, repeat, volume, balance, bass, treble, backlight
timeout/brightness, theme) is initialised by `settings_defaults()` on every
boot. There is no write path, so every cold start forgets the user's choices.
The FAT32 driver (`core/fs/fat32.c`) is **read-only** by design, and the disk
holds the user's music — so the bar for "safe to write" is high.

## Non-goals

- General FAT write support (allocating clusters, creating/renaming files,
  updating directory entries, extending file length). Explicitly out of scope —
  that is where disk corruption risk lives.
- Persisting playback position / "resume on startup". That needs per-track
  state and is a separate feature; this covers `settings_t` only.

## Approach: in-place overwrite of a pre-allocated config file

The host importer creates a **fixed-size, contiguous** file `CORECFG.DAT` in
the volume root, once, at import time. The device never allocates, grows, or
shrinks it — it only **overwrites the bytes of its existing data sector(s)**.
Because the file already exists at a known length with clusters already in the
FAT and a directory entry already written, a data-only overwrite touches **zero
filesystem metadata**: no FAT entries, no directory entries, no free-count. The
on-disk structures the read-only driver relies on are never mutated.

This is the same trick the library index uses to be cheap to read
(`CORELIB.IDX` is host-built and only ever read); here we additionally write
back into an equally pre-placed file.

### Why one sector

`settings_t` is a few dozen bytes. We define a **512-byte on-disk record** so a
write is exactly one 512-byte ATA sector — atomic at the drive level (a sector
write either completes or, on power loss, leaves the old contents; there is no
torn sub-sector state). `CORECFG.DAT` is sized to one cluster; only its first
sector is ever written.

## LBA resolution (device)

Mirrors how `CORELIB.IDX` is already found (`index_root_cb` in `main.c`):

1. At mount, `fat32_readdir(fs, fs->root_clus, cb, …)`; the callback matches
   `CORECFG.DAT` (case-insensitive) and captures `first_clus` + `size`.
2. Convert the first cluster to an **absolute LBA**. `fat32.c` already has
   `cluster_fs_sector()` (cluster → FS-relative sector); we add a small public
   helper:

   ```c
   /* Absolute 512-byte LBA of the first sector of `clus`. */
   uint32_t fat32_cluster_lba(const fat32_t *fs, uint32_t clus);
   ```

   = `fs->part_lba + cluster_fs_sector(fs, clus)` (the same base the read path
   already adds). No new disk knowledge — just exposes an address the driver
   computes internally today.
3. Validate before trusting it: `size >= 512`, `first_clus >= 2`, LBA within
   the volume. If `CORECFG.DAT` is absent or fails validation → **fall back to
   `settings_defaults()` and disable writes** (the file will appear after the
   next import).

## On-disk record layout (512 bytes, little-endian)

```
off  size  field
0    4     magic       'C''O''R''E'  (0x45524F43 LE)
4    2     version     layout version (start at 1)
6    2     length      bytes of payload that follow the header (for fwd-compat)
8    4     seq         monotonic write counter (newest wins; see below)
12   4     crc32       CRC-32 over bytes [16 .. 16+length)
16   N     payload     the packed settings fields (see below)
...        zero-pad to 512
```

Payload (v1), each a fixed width, endian-fixed — **not** a raw `struct` dump
(avoid ABI/padding coupling):

```
shuffle            u8   (0/1)
repeat             u8   (0=off 1=all 2=one)
volume             u8   (0..100)
balance            i8   (-100..100)
bass               i8   (-12..12)
treble             i8   (-12..12)
backlight_secs     u8   (0/5/10/15/30/60)
backlight_bright   u8   (1..32)
theme              u8   (0=Linen 1=Onyx)
```

Loader validates magic + version + length + crc32. Any mismatch → defaults +
writes disabled until a good record is written. Unknown-but-newer `version`
with a known prefix → read the fields we understand, ignore the rest (the
`length` field makes this safe).

### Torn-write safety (optional two-slot scheme)

A single 512-byte write is atomic on the drive, so a bare single-slot record is
already safe against *torn* writes. To also survive power loss *mid-write*
(sector left half-old/half-new is not possible, but a drive can fail the write
entirely), we can use **two slots** (sector 0 and sector 1 of the file) with
the `seq` counter: read both, pick the valid record with the highest `seq`,
and always write to the *other* slot. This guarantees a good previous record
always survives. Recommended; costs one extra sector in the same cluster.

## Write flow (device)

Trigger: debounced, **not** on every wheel tick. Persist when the user leaves
Settings (or on a 2–3 s idle after the last change), and coalesce — one write
per settling, so a volume sweep is a single sector write, not dozens.

1. Pack `settings_t` → payload, bump `seq`, compute crc32, zero-pad to 512.
2. `ata_write_sectors(target_lba, 1, buf)` (new; see below).
3. Best-effort. Failure is logged (UART) and non-fatal — settings simply do not
   persist that session; nothing else is affected.

Never write during playback disk activity if it risks the audio buffer — gate
on the same `player_active()` / diskbuf state the drive-park logic already uses,
or simply defer the write until playback is idle.

## New HAL surface: `ata_write_sectors`

`ata.c` today is read-only (`ata_read_sectors`). We add the mirror:

```c
/* Write `count` (1..256) 512-byte sectors from `buf` to LBA `lba`.
 * Returns 0 on success. Uses WRITE SECTORS (0x30, PIO) — the exact mirror of
 * ata_read_sectors' READ SECTORS (0x20): same LBA28 addressing, same DRQ
 * handshake, but the host drives the data-out phase and then waits for BSY to
 * drop with ERR clear. A cache-flush (0xE7) follows the last sector. */
int ata_write_sectors(uint32_t lba, uint32_t count, void *buf);
```

This is the single most safety-critical addition. Mitigations:

- **Guard LBA range.** The write helper used for settings only ever accepts the
  resolved `CORECFG.DAT` LBA (+1 for slot two); a wrapper `config_write()` owns
  that address and nothing else calls `ata_write_sectors` for settings. A wrong
  LBA is the only way this corrupts data, so the address comes exclusively from
  the validated directory lookup — never a literal.
- **Cache flush (0xE7)** after the write so the record is durable before we
  claim success (and before any later spin-down).
- **Host precondition:** the importer guarantees `CORECFG.DAT` is contiguous
  and ≥ 1 cluster, so "first cluster's first sector" is always the file's own
  data — never shared with anything else.

## Host importer changes (`tools/build_index.py` / importer)

- After writing `CORELIB.IDX`, create `CORECFG.DAT` if absent: a fixed
  (cluster-sized, e.g. 4 KiB) file pre-filled with a **valid default record**
  (magic + version + defaults + crc) in sector 0 (and a zeroed/invalid sector 1
  for the two-slot scheme). Writing it through the normal filesystem guarantees
  correct FAT/dir metadata.
- Must be **contiguous**. On these freshly-populated FAT32 volumes new files are
  contiguous in practice; the importer should verify (re-read the FAT chain for
  the file and assert a single run) and, if not, rewrite it. The device also
  only ever touches the *first* cluster, so even a fragmented file is safe as
  long as cluster 1 is correctly mapped — but keeping it contiguous keeps the
  option open to grow the record later.
- Never overwrite an existing `CORECFG.DAT` that already validates (don't clobber
  the user's saved settings on re-import).

## Testing (host, before any device write)

- **Codec/record unit test:** pack → unpack round-trips every field; crc catches
  bit-flips; version/length forward-compat (short record read by newer loader,
  long record read by older loader).
- **Two-slot selection:** given slot A `seq=n` valid + slot B `seq=n+1` invalid
  crc, loader picks A; both valid picks higher seq; both invalid → defaults.
- **`ata_write_sectors` trace test:** mock-bus assertion of the 0x30 / DRQ-out /
  0xE7 grammar, exactly like the existing `volume_trace_test` / ATA read tests —
  no real disk touched.
- Device bring-up: first write to `CORECFG.DAT`, power-cycle, confirm settings
  survive; deliberately corrupt the record and confirm graceful fallback.

## Open questions for review

1. **Two-slot or single-slot?** Single is simpler and already atomic-per-sector;
   two-slot additionally survives a failed write. Recommendation: two-slot (cheap).
2. **Write trigger:** on Settings-exit only, or also on a Now-Playing change
   (volume/shuffle changed outside the Settings screen)? Leaning: persist on any
   `settings_apply()` that changed a field, debounced ~2 s, deferred past active
   playback.
3. **Where `config_write()` lives:** a small `core/kernel/config.c` owning the
   LBA + record codec, sitting above `ata_write_sectors`. Agree?
4. **Importer ownership:** fold into `build_index.py`, or a separate
   `build_config.py` step? (The device is agnostic.)
```
