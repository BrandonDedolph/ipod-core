# Plan — `core`, the app that runs the show

Host program that owns the whole loop for an everyday user: put music on the iPod in the
layout the firmware wants, bake every album-art size, write the index, create the two
pre-allocated device files, flash firmware versions, fetch updates. One Go binary, the
existing module `core/cli` (`github.com/BrandonDedolph/ipod_theme/core/cli`, go 1.22.10,
cobra). CLI first, web GUI last.

Written 2026-09-14 from the tree at 928727d (v0.1.2). Everything below was checked
against the source, the bring-up folder, the two ipodpatcher partition dumps and the
928-track oracle index unless marked "verify".

---

## 0. Facts the implementer must not re-derive

**Firmware partition (measured on the 5.5G 80 GB, dumps in
`/mnt/c/Users/brandon-home/ipod-bringup/`: `bootpartition-backup.bin` = Apple OSOS,
`after_bootpart.bin` = ours, both 131,475,456 B):**

- MBR partition 0: type 0x00, start sector 63, end 64259; partition 1: type 0x0B FAT32,
  start 64260. **The bridge reports 2048-byte logical sectors over USB** (ipodpatcher
  log: "Sector size is 2048 bytes"), so partition 0 = byte 129,024 .. +131,475,456
  (64,197 × 2048). Read the logical sector size from the OS, never assume 512.
- Preamble: `{{~~  /-----\` … `Copyright(C) 2001 Apple Computer, Inc.---` in the first
  512 B. `]ih[` at 0x100, LE32 0x4000 at 0x104 → directory at 0x4200, LE16 version 3 at
  0x10A. Four 40-byte entries: `!ATA`/`soso` (OSOS), `crsr`, `dpua`, `ebih`.
  **Correction (S5, from the dumps): the row after HIBE is NOT all zeros — its `loadAddr2`
  (0x42C4) reads 0xFFFFFFFF.** The list ends at the first row with no container tag, no
  image type, devOffset 0 and len 0; an all-zero test walks one row too far. `AUPD`/`HIBE`
  carry stale checksums on a shipping device (BAD on inspect is normal); OSOS and RSRC verify.
- OSOS entry on the device today: devOffset 0x4800, **body at devOffset + 0x800 =
  0x5000**, len 237,640 (= exact `core.bin` size at that time, NOT padded), addr
  0x10000000, entryOffset 0, chksum 0x01589d64 = **plain 32-bit sum of the `len` body
  bytes, NO model seed**, vers 0xB012, loadAddr2 0xFFFFFFFF. Apple's own entry: len
  7,618,128, entryOffset 0x736000, same addr/vers/la2. ipodpatcher zero-pads the body
  write to a 0x800 boundary and leaves every other entry field alone.
- Body capacity for OSOS = next entry's devOffset (RSRC at 0x749000) − 0x5000 =
  7,618,560 B. Refuse a larger image.
- `.ipod` transport file = BE32(sum(image) + 5) ++ `ipvd` ++ image. The seed 5 belongs
  to the `.ipod` header only. `internal/firmware/imageheader.go`'s comment "model +
  sum-of-bytes" on `DirectoryEntry.Checksum` is wrong for the partition entry; fix it.
- ipodpatcher `-r` = whole partition to file (what `after_bootpart.bin` is); `-rfb` =
  the OSOS body, exactly `len` bytes (that is what `cmp readback.bin core.bin`
  verified); `-rf x.ipod` = the same wrapped as `.ipod`. `-wf core.ipod` = what we
  replace.
- Recovery floor is ROM disk mode (Select+Play). Never touch the preamble, the
  partition table, or any entry other than OSOS.

**Library (from `tools/build_index.py`, `core/kernel/main.c`, the oracle):**

- Source tree: `C:\Users\brandon-home\Music\MC` (`/mnt/c/Users/brandon-home/Music/MC`),
  102 folders, 928 FLACs, folders named **`Album - Artist`** (split on the LAST " - ";
  the artist is the part after it). Device layout: `Music/<Artist - Album>/NN. Title.flac`
  (FAT-safe: `[\/:*?"<>|]` → `_`, then rstrip `" ."`), multi-disc source `Disc N/`
  subfolders flattened, plus `folder.art` and `folder.thm` per album,
  `Music/CORELIB.IDX`, `Music/Playlists/*.m3u8`, and at the volume root `CORECFG.DAT`
  (32 KiB) and `CORELOG.BIN` (4 MiB).
- Tag keys as `build_index.py` sees them are ffprobe's CONVERTED names: `album_artist`
  (from ALBUMARTIST), `track` (TRACKNUMBER), `disc` (DISCNUMBER); the raw Vorbis keys
  are what `internal/flac` returns, so `library` tries both spellings. `glob("*.flac")`
  is CASE-SENSITIVE on Linux (a `.FLAC` file is silently absent from the Python index;
  Go warns and skips to keep parity — revisit in S4b, where the copy renames to
  `.flac` anyway), and Python's `glob` also globs the DIRECTORY part, so an album folder
  containing `[`, `]`, `*` or `?` vanishes from the Python index; Go uses `ReadDir` and
  indexes it (deliberate superset, MC has no such folder). The dedup `while` loop in
  the script is unreachable and would not terminate; Go bounds it.
- Oracle: `/mnt/c/Users/brandon-home/ipod-bringup/CORELIB.IDX` — header
  `CIDX`, v2, rec 256, count 928, CRC 3248459348, 237,584 B, built from MC on
  2026-09-10 with `ffprobe`. ffprobe/ffmpeg are on this host (`/usr/sbin`).
- Record (256 B, LE): `u32 duration_s, u16 track, u16 disc, folder[64], file[64],
  title[48], artist[40], genre[24], u32 folder_hash, u32 file_hash`. Strings: UTF-8,
  C0 controls dropped, truncated to n−1 bytes on a rune boundary, NUL-padded. Header
  `<4sHHII`: `CIDX`, 2, 256, count, zlib CRC-32 over the records only.
- Locator hash: FNV-1a 32 over UTF-8 of `norm_key` (U+2018/2019 → `'`, U+201C/201D →
  `"`, U+2013/2014 → `-`, ASCII A–Z lowercased, nothing else). Golden vectors:
  `core/tests/kernel/name_hash_vectors.h` (`NAME_HASH_VEC(label, octal-escaped-utf8,
  hash)`), parsed by regex in `core/tests/scripts/check_name_hash_parity.py`.
- Caps in `core/kernel/main.c`: `LIB_MAX_SONGS 6000`, `LIB_MAX_ALBUMS 1024`,
  `LIB_MAX_GENRES 128` (artists 512 elsewhere). Past a cap the device silently drops.
- Art: firmware reads exactly two sidecars — `folder.art` 120×120 (`ARTCACHE_MAX_DIM`,
  now-playing hero, also shrunk on-device to 56 for the album header) and `folder.thm`
  28×28 (`ARTCACHE_DIM`, list chip, must be exact size). CoreArt = `CART`, u16 1, u16 w,
  u16 h, u16 0, then w·h RGB565 LE. There is no third size; "all the keyart sizes" =
  these two. Both are produced from the SOURCE picture independently (not 28 from 120).
  **Correction (S3): ffmpeg DITHERS its rgb565le output with a fixed ordered pattern and
  `-sws_dither none` does not reach the auto-inserted output converter** — verified four
  ways. So the sidecars `coreart.py` wrote are dithered, and no deterministic packer can
  match them to mean ≤ 2. Oracle ruling (review 2026-09-14): the plan's mean 2.0 / p99 12
  bound is enforced against ffmpeg's rgb24 at the target size, over the CORPUS (all
  sources pooled); per-source outliers (fine-detail 1280 px covers) are printed and
  counted, not failed; the literal 565 comparison is kept as a diagnostic held to the
  dither floor + tolerance. Measured on 99 MC covers: N=120 mean 0.645 / p99 6, N=28
  mean 1.433 / p99 11; linear-light scaling was tried and is much worse (ffmpeg scales
  sRGB values as-is).
- Playlists: firmware lists `<lib root>/Playlists/*.m3u8|*.m3u`. **Correction (S4a, from
  `core/kernel/main.c:2899` and `core/fs/m3u.c`): a relative line resolves against the
  playlist's OWN directory (`Music/Playlists`), not `Music/`.** The two working forms are
  `/Music/<Artist - Album>/<NN. Title>.flac` (absolute from the volume root; what
  `devicefs.PlaylistEntry` writes) and `../<Artist - Album>/<file>`. `/` and `\` both
  separate, a drive letter at index 1 is accepted, `#` lines are directives, CR/LF/CRLF all
  end a line, a BOM is skipped, any byte < 0x20 or a `:` elsewhere rejects the line, a `..`
  past the root rejects, resolved path ≤ 191 bytes (`M3U_PATH_MAX`).
- CORECFG.DAT record (`tools/make_config.py`, mirrors `kernel/config.c`): 1024-B slots
  ×2, magic `CORE` LE (0x45524F43), version 2, length 44, seq, 12 payload bytes
  (shuffle 0, repeat 0, resume_on_startup 1, crossfade 0, volume 70, bass 0, treble 0,
  balance 0, backlight_secs 15, backlight_bright 32, theme 0, clicker 1; bass/treble/
  balance signed), 3×u32 resume locator = 0, 20 B context = 0, CRC-32 over [0,1020) at
  1020. File = slot0 valid at seq 1, slot1 zero, padded to 32 KiB. **Hazard:** the
  defaults must equal `settings_defaults()` in `core/ui/settings.c` (a 0 there once
  silently disabled Resume on the device). `make_config.py --emit <file>` writes a
  2048-B file (both slots), not stdout. The firmware also refuses a file shorter than
  `CONFIG_MIN_BYTES` (2048) regardless of slot contents (`config.c:479`).
- CORELOG.BIN (`tools/make_log.py`, mirrors `kernel/evlog.c`): 2048-B blocks; block 0 =
  `CLOG` LE, u16 1, u16 2048, u32 block_count, u32 random file_id, u32 CRC-32 over
  [0,16), rest zero; ring blocks left zero. 4 MiB = 2048 blocks. `evlog_mount()` also
  requires `block_count × 2048 == file size` or the log stays off silently
  (`evlog.c:508`) — a header that validates but disagrees with the length is rewritten.
  `make_log.py --emit` is a 6-block ring FIXTURE for `evlog_test.c`, not a bare header;
  the Go header golden was taken from its `encode_header()` directly.
- Version strings in the image: `CORE_VERSION` ("v0.1.2") and `CORE_BUILD_ID` are in
  `.rodata` and `strings` finds "v0.1.2", but nothing tags it — a host can't tell it from
  any other string. See §6.
- The firmware never creates files; the host creates everything, the device only
  overwrites bytes inside CORECFG.DAT / CORELOG.BIN.
- WSL: `/mnt/d` here is a STALE drvfs mount (3 files, Jul 27); the device is only
  reachable from Windows. drvfs writes to FAT are unreliable (see make_log.py header) —
  the app refuses to sync through a 9p/drvfs mount.

---

## 1. Decisions

1. **Form.** One binary, `core`, in `core/cli`. Commands: `info`, `sync`, `index`,
   `art`, `backup`, `flash`, `update`, `doctor`, `firmware {pack,unpack,inspect,read}`,
   `build`; the GUI is the separate `core-app` executable (S10). Drop the stubs that will never exist (`sim`, `test`, `debug`,
   `release`, `install`, `recover` → `flash` covers install; `recover` = `flash
   --from-backup`). *Why:* the user's own phrase is "the application that runs the
   show"; one executable per OS from GitHub Releases is the everyday-user story in
   the notes. **GUI = a native desktop window, `core-app`, built with Gio (`gioui.org` v0.8.0) as a second executable from the same module — REVISED 2026-09-15, the user's call: "a standalone app not a web app".** *Why Gio:* its Windows backend is pure Go, so `CGO_ENABLED=0 GOOS=windows go build` from this WSL box produces the exe the user tests (measured: 10.3 MB, builds); Fyne and Wails need cgo/webview toolchains per OS and cannot be cross-built here. Linux and macOS builds need cgo and are made on native CI runners. *Why v0.8.0:* it is the last Gio on `go 1.21` and pins `golang.org/x/image v0.18.0`, exactly what the CLI already has; v0.9.0 needs Go 1.23.8 and v0.10 Go 1.24 (with `GOTOOLCHAIN=auto` they would silently pull a newer toolchain and bump x/image to v0.26). The elevated flash child is the `core` CLI beside the app, so the write path is one code path.
2. **Library pipeline in Go, Python becomes the oracle.** Own FLAC metadata parser
   (`internal/flac`): STREAMINFO (block 0: sample_rate 20 bits at byte 10, total_samples
   36 bits at byte 13) gives **duration = total_samples / sample_rate, integer
   division** (ffprobe reports total/rate and build_index does `int(float(...))`, so
   truncation agrees); VORBIS_COMMENT (block 4) → map with lower-cased keys, last
   duplicate wins (both ffprobe and dhowden behave so); PICTURE (block 6). *Why not
   dhowden/tag:* it has no STREAMINFO, so we need the block walker anyway; dropping it
   removes the only dependency of the dead `tagcache` package. Tag keys used, exactly as
   ffprobe exposes them: `title`, `artist` (else `albumartist`), `album`, `genre`,
   `tracknumber` (ffprobe's `track`), `discnumber` (`disc`); leading-integer parse.
   Everything else mirrors build_index.py rule for rule (§3 of S2). Output must be
   **byte-identical** to build_index.py on the same tree; `LIB_MAX_SONGS` etc. are Go
   constants with a test that greps `core/kernel/main.c` when the repo is present.
3. **Art in Go.** `image/jpeg` + `image/png` decode, **Lanczos3 via
   `golang.org/x/image/draw` `Kernel{Support:3}`** (mirrors ffmpeg `flags=lanczos`),
   stretch to square exactly like ffmpeg `scale=N:N` (no crop), RGB565 by rounding
   (`(v*31+127)/255`, `(v*63+127)/255`). Oracle = ffmpeg with `-sws_dither none`;
   tolerance in §S3. Sizes are the two firmware constants, checked against
   `core/ui/artcache.h` by test.
4. **Sync semantics.** Source `Album - Artist/` tree → `Music/Artist - Album/NN.
   Title.flac`; unchanged files skipped by (size equal, mtime within 2 s — FAT
   resolution); `--verify` re-hashes; nothing deleted without `--prune`; playlists
   rewritten to device paths; CORECFG.DAT / CORELOG.BIN created only if absent or
   invalid (never reset saved settings); **index written last**, then per-file
   write-through + flush; `core eject`. *Why:* the device loads the index in one read
   and binds records to folders by hash — an index that names a folder not yet copied
   just orphans the record, but an index written before a failed copy is worse to
   explain, so it goes last.
5. **Flash = pure Go clean-room from `08-boot-dock.md` + the measured entry.** Whole
   partition backup to `<UserConfigDir>/core/backups/fwpart-<size>-<RFC3339>.bin` before
   any write, body write zero-padded to 0x800, entry `len`/`chksum` updated and all
   other fields preserved, directory sector rewritten, read-back compare of body AND
   directory. Per-OS raw access in `internal/disk`. **Windows: the binary re-launches
   itself elevated (`ShellExecuteW` verb `runas`) with `--elevated-log <file>` and the
   parent tails/prints the result; Linux/macOS: refuse and print the exact `sudo core …`
   command.** *Why:* UAC cannot elevate an existing console; sudo can. ipodpatcher stays
   documented as the fallback and `doflash.cmd` stays in the bring-up folder until two
   real flashes have been verified through `core`.
6. **Update.** GitHub Releases API (`BrandonDedolph/ipod-core`), asset `core.ipod`,
   verify the `.ipod` checksum (seed 5), compare to the device's version, flash via §5.
   Device version: add a tagged marker to the firmware, `static const char
   core_version_marker[] __attribute__((used, section(".rodata.core_version"))) =
   "CORE-FW-VERSION:" CORE_VERSION "|" CORE_BUILD_ID "\0";` — the host scans the OSOS
   body for `CORE-FW-VERSION:`. *Why:* the bare "v0.1.2" string is untagged and
   unfindable; one line in `main.c`, screen/UART untouched, clicky golden untouched.
7. **Releases/CI.** `ci.yml`'s `cli` job gains a cross-compile matrix (linux/amd64,
   linux/arm64, darwin/amd64, darwin/arm64, windows/amd64) with `-ldflags` version
   stamping; a new `release.yml` on tag push `v*` builds the ten binaries (five `core` CLI + five `core-app` GUI, S10) and
   `gh release upload --clobber`s them onto the release the human flow creates.
   **core.ipod stays the locally built, flashed image** (CI's pinned gcc 14 vs local
   gcc 16 would produce a different binary than the one verified on the device).
   `tools/release.py` prints the upload line and checks the binaries are attached.
8. **Migration.** `tools/build_index.py`, `coreart.py`, `make_config.py`, `make_log.py`
   stay as **reference implementations and parity oracles** (Go tests shell out to them
   when python3/ffmpeg are present, skip otherwise); their README rows say so. Delete
   `internal/tagcache` and `internal/artistart` (dead: nothing on the device reads TCDB,
   artist photos have no consumer, and they are 6 of the 9 gofmt-dirty files holding the
   CI gate open). `make ipod` keeps `go run ./cmd/core firmware pack`.

---

## 2. Package layout (final)

```
core/cli/
  cmd/core/main.go
  internal/cli/         cobra commands, one file each (root, info, sync, index, art,
                        backup, flash, update, doctor, firmware, build, ui, eject)
  internal/flac/        metadata block reader (STREAMINFO, VORBIS_COMMENT, PICTURE)
  internal/library/     scan.go (source tree → albums/tracks with device names),
                        names.go (norm_key, name_hash, fat_safe, utf8_field, title rules),
                        genres.go, caps.go
  internal/cidx/        CORELIB.IDX v2 writer + reader (reader for `doctor`/tests)
  internal/coreart/     picture → CART 120 / 28
  internal/devicefs/    CORECFG.DAT + CORELOG.BIN creators/validators, M3U8 writer,
                        layout paths, FAT LBA resolver (read-only, for `doctor`)
  internal/syncer/      plan + execute + report
  internal/fwpart/      partition image model: parse directory, OSOS, capacity, build
                        the write set, read-back verify (pure, io.ReaderAt/WriterAt)
  internal/firmware/    (existing) .ipod codec, checksum, entry codec
  internal/disk/        enumerate + open raw disks per OS, sector size, lock/dismount,
                        elevation (windows), iPod identification
  internal/ghrelease/   latest release + asset download + cache
  internal/app/         Gio desktop UI (State model, job runner, screens, headless snapshot)
  internal/doctor/      the read-only device+volume checks (extracted from cli/doctor.go)
  internal/eject/       flush + eject per OS (extracted from cli/eject*.go)
  cmd/core-app/         the GUI main (-H windowsgui on Windows)
  internal/version/     (existing)
```

Cross-cutting rules for every slice: `gofmt`, `go vet`, `go test ./...` green; no cgo;
no new dependency beyond `golang.org/x/image` (pin `v0.18.0`, go 1.22-compatible) and
`golang.org/x/sys` (pin `v0.24.0`); every parity test skips (not fails) when the repo
tree, python3, ffmpeg or the MC tree is absent, keyed by env `CORE_REPO` (default: walk
up from the test file), `CORE_PARITY_SRC` (the MC path), `CORE_PARITY_IDX` (the oracle
index), `CORE_FWPART_DUMP` (a partition dump); document these in `core/cli/README.md`.

---

## 3. Slices (ordered; each ≤ ~1500 lines of Go incl. tests; one Opus run each)

**Status 2026-09-14: S0, S1, S2, S3, S4a, S5 DONE and reviewed** (Opus built, Fable
reviewed; parity suites green with the MC tree, the oracle index and both partition
dumps; S2 output byte-identical to `build_index.py` on the real library).
**S4b and S6 DONE and reviewed 2026-09-14** (host tests only; the device checks below are
staged in the bring-up folder as `core_check.cmd` (elevated, read-only) and
`core_sync_dryrun.cmd` (non-elevated, `--dry-run`) and wait for the iPod in disk mode).
**S7 DONE 2026-09-15, host tests only** — `internal/flasher` + `core flash`, with the
sequence, the backup-first rule, the loop guard and the fault-after-the-body-write case
all covered against `disk.NewMemDevice`; its device check is staged as
`core_flash_check.cmd` (elevated, re-flashes the SAME v0.1.2 image, confirmed by
ipodpatcher `-rfb` + `fc /b`) and waits for the iPod in disk mode. Corrections from S7:
the pack plausibility rules moved to `internal/firmware` (`ValidateImage`) so `flash`
applies the same ones to a raw `.bin`; the whole-partition backup naming lives in
`internal/flasher` and `core backup` delegates to it; `cmd/core` grew a `cli.ExitError`
so the elevated child's exit code reaches the shell. Note the honest gap: on Windows an
unelevated process cannot open `\\.\PhysicalDriveN` at all, so `FindIPods` is refused
before a plan can be printed and `core flash` prints the RunAs line rather than
relaunching — the relaunch fires only from a process that could read the device.
**S7 reviewed 2026-09-15 (Fable), accept with fixes:** the backup path is decided once
(the plan and the backup could name different files across a second boundary); a `.ipod`
whose header names another model (`nano`) is refused; the recovery line uses the right
ipodpatcher verb (`-wf` .ipod / `-wfb` bare image / `-w` partition dump — `-wf` on a .bin
would flash 8 bytes of header as code); the pre-plan Windows advice leads with a
`--dry-run` line before the `--yes` one; a whole-partition restore honours ctx only
before its first chunk (after the directory chunk lands, only finishing boots).
**S8 firmware half DONE and reviewed 2026-09-15:** `core_version_marker` in
`kernel/main.c` with `section(".rodata.core_version")`, `KEEP` + a `LONG(0)` guard word
in `boot/linker.ld` (both measured necessary), `tests/scripts/check_version_marker.sh`
in `verify-hw`; image 367,624 → 367,664 B; one marker, `CORE-FW-VERSION:v0.1.2|…`;
both build trees identical; 58/58 host tests. The next tagged image is the first with
a marker. **S9 CI half DONE and reviewed 2026-09-15:** `ci.yml` cli job cross-builds
the five binaries via `core/cli/scripts/build-all.sh` (one ldflags stamp, `fetch-depth:
0` for tags), `release.yml` on `v*` waits for the human release and `gh release upload
--clobber`s them (draft if it never appears); `tools/release.py --check` lists the six
assets. **Asset-name convention, corrected:** every existing release carries the
firmware as `core-<tag>.ipod` (`core-v0.1.2.ipod`), not `core.ipod`; the updater
accepts `core-<tag>.ipod` first and `core.ipod` as a fallback. Device checks run in
this order once the iPod is in disk mode: `core_check.cmd` (elevated, read-only) →
`core_sync_dryrun.cmd` (plain) → `core_flash_check.cmd` (elevated, re-flash of v0.1.2).
Corrections from S4b/S6: the MC tree is **101** albums (the 102nd folder has no " - ");
a `.FLAC` source is skipped by the scan and therefore NOT copied (renaming it would put
a file on the device no record names); playlist lines map source→device through the
scan, with `<album folder>/<file>` as the last-resort key; `--prune --yes` is refused
when the source scanned to zero tracks (a wrong `--src` must not empty the device).
Disk layer: there is **no USB VID/PID** without cgo — check 1 of the checklist is the
OS's vendor/product strings ("Apple"/"iPod"); Linux `Lock` is a refusal when any
partition is mounted, not a kernel lock; darwin `Flush` is a documented no-op on
`/dev/rdiskN`; Windows needs sector-aligned *buffers* under `FILE_FLAG_NO_BUFFERING`;
`IOCTL_DISK_GET_LENGTH_INFO` needs read access, so an unelevated `info --all-disks`
takes the size from `DISK_GEOMETRY_EX`; `IsElevated` is token elevation, not group
membership; the Windows elevation line logs to an absolute path next to the exe (a
RunAs child starts in System32). Tested window [74 GiB, 80 GiB] brackets the real
device: partition 1 ends at sector 39,075,370 × 2048 = 80.03 GB = 74.53 GiB. S6 came in
at ~4,600 lines (three OS backends + a plist reader), above the slice guideline.
**S8 CLI half DONE and reviewed 2026-09-15 (Fable, accept):** `fwpart.FindVersion` /
`CountVersionMarkers` / `VersionText` (128-byte payload cap, control bytes rejected, first
marker wins); `internal/ghrelease` (`Latest`/`ByTag`, `browser_download_url` with
`Accept: application/octet-stream`, size check, temp+rename, `.ipod` checksum via
`firmware.ReadIPodFile` before the cache entry exists, cache hit re-verified every run,
`GITHUB_TOKEN` optional; the real API was checked once: fields `tag_name`/`name`/`body`/
`assets[].name|size|browser_download_url`, v0.1.2 carries `core-v0.1.2.ipod`, and the
download 302s to `release-assets.githubusercontent.com`, where Go drops the
Authorization header); `core update` (device version read first and non-fatal, `--check`
writes nothing, an unknown device version never skips the flash, `--tag` re-flashes the
installed version, the elevated child is `flash <cached file>` never `update`); `core
doctor` (read-only; WARN for a marker-less image, FAIL past a cap). **S9 docs half DONE
and reviewed 2026-09-15:** README, user guide and tools/README describe the `core` flow
first; the reviewer softened the guide's and README's "verified on the 5.5G 80 GB"
claim to "tested against its partition dumps on the host; the first on-device flash
through `core` is the v0.1.3 release", and rewrote the guide's Windows paragraph: from
an ordinary console `update` downloads, verifies and prints the elevated `core flash`
lines (`--dry-run`, then `--yes`) rather than relaunching, because an unelevated
process cannot read the device to make a plan (see the S7 note above).

**S10 DONE and reviewed 2026-09-15 (Fable, accept with fixes):** `internal/app` (Gio
v0.8.0 window: State/Event/Runner/Backend, four cards + log, `--screenshot`/`--state`/
`--size`/`--log`), `cmd/core-app`, `internal/doctor` and `internal/eject` extracted with
thin CLI delegations, `disk.RelaunchElevatedExe`/`SudoCommandExe`, `build-all.sh` builds
the 7 the host can and names the 3 cgo GUI builds for CI, `release.yml` gained the
ubuntu-24.04-arm and macos-latest jobs, `release.py --check` expects eleven assets.
Reviewer fixes: (1) the flash dialog parsed the flasher's prompt for a parenthesised path
the flasher never writes, so every GUI flash aborted at the confirmation — the prompt's
shape is now owned by `flasher.ConfirmPrompt`/`ConfirmTarget` and the dialog refuses an
unknown shape rather than waving a write through; (2) the confirm closure read
`State.CLIPath` on the job goroutine (race) — captured on the UI goroutine; (3) Cancel is
greyed for the two write jobs (`State.CanCancel`) instead of cancelling a context the
flasher would report as "cancelled" over a finished write; (4) a panic in a job becomes an
error event instead of killing the window; (5) Update logs when the iPod already runs the
release's tag. Corrections to the plan: the darwin GUI cannot even be vetted here (Gio's
darwin GL layer is cgo-only); `Backend.Eject` takes `emit` and `Backend.Sync` returns the
plan (the prune dialog needs the orphan list). The window opened on the user's desktop
(`app.log`: "window opened"); the pixels could not be captured from WSL (locked session,
DWM not compositing), the `--screenshot` frames from the same exe are the review evidence.
Not yet on a device: same status as S7.

### S0 — clear the deck
**Change:** delete `internal/tagcache`, `internal/artistart`, `internal/cli/tagcache.go`,
`sim.go`, `test.go`, `debug.go`, `release.go`, `install.go`, `recover.go`, `update.go`,
`flash.go` stubs (keep `firmware.go`, `build.go`, `info.go` as a stub that will be
filled in S6); move the SAFETY CHECKLIST comment from `install.go` into
`internal/fwpart/doc.go` verbatim; `go mod tidy` (drops dhowden/tag, mousetrap stays via
cobra); `gofmt -w .`; delete the `continue-on-error` gofmt block in `ci.yml`; fix the
`Checksum` field comment in `imageheader.go` ("plain additive sum over `Length` body
bytes; the model seed applies to the .ipod transport header only"); rewrite
`core/cli/README.md` Status/Layout to the new command list marked "planned" per slice.
**Tests:** existing `go test ./...`; `make -C core ipod` still packs.
**Done when:** CI `cli` job is green with gofmt enforced and `core --help` lists only
commands that exist or are marked planned.

### S1 — `internal/flac`
**Interface:**
```go
type StreamInfo struct{ SampleRate uint32; Channels, BitsPerSample uint8; TotalSamples uint64 }
type Picture struct{ Type uint32; MIME string; Data []byte }
type Meta struct{ Info StreamInfo; Tags map[string]string /* lower-cased keys */; Pictures []Picture }
func Read(r io.ReadSeeker) (*Meta, error)          // stops at the last metadata block; never reads audio
func ReadFile(path string) (*Meta, error)
func (m *Meta) DurationSeconds() uint32           // TotalSamples / SampleRate, 0 if either is 0
func (m *Meta) FrontCover() *Picture              // type 3 first, else the first picture, else nil
func (m *Meta) Tag(keys ...string) string         // first non-empty of the keys
```
Rules: `fLaC` marker; block header = 1 bit last, 7 bits type, 24-bit length; unknown
blocks skipped; VORBIS_COMMENT = LE32 vendor len, vendor, LE32 count, then LE32 len +
`KEY=value` (split at first `=`, key lower-cased, last wins, invalid UTF-8 kept as
bytes); PICTURE = BE32 type, BE32 mime len, mime, BE32 desc len, desc, 4×BE32 dims, BE32
data len, data. Hard cap 64 MiB per block.
**Tests:** synthetic FLACs assembled in-test (a helper `buildFLAC(blocks...)`);
duplicate keys; picture type selection; truncated block → error; if
`CORE_PARITY_SRC` is set, every FLAC under it parses and `DurationSeconds()` equals
`int(float(ffprobe format.duration))` for every file (shell out; skip without ffprobe).
**Done when:** the 928-file duration parity is exact (any mismatch is a bug to explain,
not a tolerance to add).

### S2 — `internal/library` + `internal/cidx` + `core index`
**Interface:**
```go
// library
type Track struct {
    SrcPath   string; Pos int         // enumeration position, 1-based (= NN)
    DeviceName string                 // "NN. Title.flac" — THE locator contract
    Title, Artist, Genre string; Disc, Track int; DurationS uint32
    NumberFrom string                 // "tag" | "filename" | "position"
}
type Album struct {
    SrcDir string; FolderArtist, FolderAlbum string
    DeviceFolder string               // fat_safe("Artist - Album") — locator
    DisplayFolder string              // "Artist - <album_disp>" — record field
    Tracks []Track                    // sorted (Disc, Track, Pos) = record order
}
type Scan struct{ Albums []Album; Skipped []string; Failures []Failure; Drift []Track }
type Options struct{ GenreMap map[string]string; Meta func(path string) (*flac.Meta, error) }
func ScanTree(src string, o Options) (*Scan, error)
func LoadGenreMap(path string) (map[string]string, error)   // tools/artist_genres.json shape
// names
func NormKey(s string) string; func NameHash(s string) uint32; func FatSafe(s string) string
func Straighten(s string) string; func UTF8Field(s string, n int) []byte
func TrackTitle(fname string) string; func LeadTrack(stem, artist string) (disc, track int)
func TrackNumber(stem string, tagTrack, tagDisc, folderDisc int, artist string) (disc, track int)
// caps
const MaxSongs = 6000; const MaxAlbums = 1024; const MaxGenres = 128
// cidx
type Record struct{ DurationS uint32; Track, Disc uint16; Folder, File, Title, Artist, Genre string; FolderHash, FileHash uint32 }
func Encode(recs []Record) []byte; func Decode(b []byte) ([]Record, error)
func RecordsFromScan(s *library.Scan) []Record
```
Rules, mirrored exactly from build_index.py: folders = `sorted(os.listdir)` by code
point (Go `sort.Strings` on UTF-8 = same order); skip non-dirs and folders with no
" - "; enumeration = `Disc *` dirs sorted, `*.flac` sorted within, else `*.flac` sorted
(extension match case-insensitive — MC has only lowercase, so parity holds; warn on
uppercase); `fname = "%02d. %s.flac"` with `fat_safe(TrackTitle(basename))`, dedup
`"%02d. %s (%d).flac"` with the seen-count (bound the loop, error past 99); `track =
tag || lead || position`; drift list; title = tag or `TrackTitle`; artist = folder
artist; genre = map[folderArtist] else first comma-part of the tag; `album_disp` decided
by the FIRST track only (`straighten(fat_safe(tagAlbum)) == straighten(folderAlbum)` →
`straighten(tagAlbum)`, else folder album); records sorted `(disc, track, pos)`;
`folder_hash = NameHash(DeviceFolder)`, `file_hash = NameHash(fname)`; caps → error
past `MaxSongs`, warning within 10 %, warnings for albums > `MaxAlbums`, genres >
`MaxGenres`.
`core index --src DIR --out FILE [--genre-map] [--show-drift] [--dry-run]` prints the
same summary lines as build_index.py.
**Tests:** (1) golden vectors parsed from `core/tests/kernel/name_hash_vectors.h` with
the same regex as the parity script, every `NAME_HASH_VEC` and `NAME_HASH_XFAIL` held
to the correct value; (2) a synthetic tree in `t.TempDir()` covering: unnumbered files,
"7 rings", "1999", "2-05 Title", `Disc 1/2`, duplicate titles, curly apostrophes,
FAT-unsafe album name with a matching tag album, comma genre, mapped artist, control
chars, a 70-byte title (truncation on a rune boundary), an `Album` folder with no
" - "; assert the decoded records field by field AND, when python3 is present, run
`tools/build_index.py` on the same tree with ffprobe replaced by a stub (see
`check_build_index.py` for the stubbing pattern — simplest: the synthetic FLACs are real
minimal FLACs written by the S1 helper with real tags, so real ffprobe works) and
`bytes.Equal` the two files; (3) caps: a test that greps `core/kernel/main.c` for the
three defines and compares with the constants (skip outside the repo); (4) parity on
the MC tree when `CORE_PARITY_SRC`/`CORE_PARITY_IDX` are set: Go output `bytes.Equal`
to `build_index.py --src MC` run fresh (not the stale oracle file — MC may have changed
since Sep 10; also print whether it equals the oracle).
**Done when:** `go test` passes with the MC parity test enabled and the Go index is
byte-identical to the Python one on the real tree.

### S3 — `internal/coreart` + `core art`
**Interface:**
```go
const ArtSize = 120; const ThumbSize = 28
func Render(pic image.Image, size int) []byte         // CART file bytes
func FromPicture(p *flac.Picture) (image.Image, error) // jpeg/png (image.Decode with both registered)
func WriteAlbum(dir string, first *flac.Meta) (Result, error) // folder.art + folder.thm; skip cleanly when no picture
func ToRGB565(img image.Image, size int) []uint16
```
Lanczos3 kernel: `draw.Kernel{Support: 3, At: func(t float64) float64 { sinc(t)*sinc(t/3) }}`
(clamp, `draw.Over` not needed — `Scale` with `draw.Src`). Convert source to
`image.RGBA` first (image/draw handles YCbCr→RGB; ffmpeg scales in YUV — accepted
difference). Rounded 565 pack.
**Tests:** CART header bytes; a synthetic 2×2 PNG scaled to 28 hits expected corner
colours; ffmpeg oracle (skip without ffmpeg): for every album under
`CORE_PARITY_SRC` that has a picture (or a checked-in 300×300 test JPEG if unset), run
`ffmpeg -i pic -vf scale=N:N:flags=lanczos -sws_dither none -pix_fmt rgb565le -f
rawvideo -` and compare per channel expanded to 8 bits: **mean |Δ| ≤ 2.0, 99th
percentile ≤ 12, max ≤ 40** for N = 120 and 28; print the numbers. If the real MC
covers miss the bound, the implementer reports the distribution and the reviewer
decides between tightening the kernel (ffmpeg's lanczos uses a=3 with bicubic-like
normalisation) and widening the bound — do not silently widen.
**Done when:** the oracle test passes on the MC covers and `core art --batch <MC>`
into a temp copy produces sidecars the firmware's `artcache.c` validation accepts
(header check reproduced in the test).

### S4a — `internal/devicefs`
**Interface:**
```go
const ConfigName = "CORECFG.DAT"; const LogName = "CORELOG.BIN"; const MusicDir = "Music"; const IndexName = "CORELIB.IDX"; const PlaylistDir = "Playlists"
type Settings struct{ Shuffle, Repeat, ResumeOnStartup, Crossfade, Volume uint8; Bass, Treble, Balance int8; BacklightSecs, BacklightBright, Theme, Clicker uint8 }
func DefaultSettings() Settings                     // MUST equal settings_defaults()
func EncodeConfigSlot(s Settings, seq uint32) [1024]byte
func DecodeConfigSlot(b []byte) (seq uint32, s Settings, ok bool)   // gated on length like config_decode
func EnsureConfig(volumeRoot string) (created bool, err error)     // 32 KiB, slot0 seq1, slot1 zero; leaves a valid file alone
func EncodeLogHeader(blockCount uint32, fileID uint32) [2048]byte
func EnsureLog(volumeRoot string, size int64) (created bool, err error) // 4 MiB default; leaves a valid header alone
func WriteM3U8(path string, entries []string) error  // "#EXTM3U\n" + one relative path per line, UTF-8, LF
func RefusesMount(path string) error                // drvfs/9p on Linux → error explaining to run on Windows
```
Windows: open with `FILE_FLAG_WRITE_THROUGH`, `FlushFileBuffers` before close (in
`internal/disk` helpers or here behind build tags). Confirm the playlist path shape by
reading `core/fs/m3u.c`, `core/library/playlist.c` and `core/tests` for the accepted
forms; write what the resolver walks (expected: `Artist - Album/NN. Title.flac`
relative to `Music/`, `/` separators; state in a doc comment with the file:line that
proves it).
**Tests:** slot bytes equal a hex golden generated once from `make_config.py --emit`
AND, when python3 is present, equal a fresh `--emit`; `DecodeConfigSlot` accepts a
v1 (length 12) record; `DefaultSettings` values equal a regex read of
`settings_defaults()` in `core/ui/settings.c` (skip outside repo); log header equals a
golden for a fixed file_id; `Ensure*` idempotence (valid file untouched, invalid
rewritten, missing created); M3U8 round-trip through a tiny reader that mirrors the
firmware's line rules.
**Done when:** `config_test.c`'s fixture path (`make_config.py --emit`) and Go agree
byte for byte and the settings-defaults grep test exists.

### S4b — `internal/syncer` + `core sync` + `core eject`
**Interface:**
```go
type Plan struct{ Copy, Skip, Rename []FileOp; Art []ArtOp; Playlists []PlaylistOp; Prune []string; Index IndexOp; Config, Log bool; Warnings []string }
type Options struct{ Src, Dst string; Prune, Verify, DryRun, NoArt bool; GenreMap string; Playlists string; Progress func(Event) }
func MakePlan(o Options, scan *library.Scan) (*Plan, error)
func Execute(ctx context.Context, p *Plan, o Options) (*Report, error)
```
Order: refuse a drvfs `Dst`; `ScanTree`; plan: for each album `Music/<DeviceFolder>/`,
each track copied to `DeviceName` (skip when dest size == src size and |mtime| ≤ 2 s,
or hash-equal under `--verify`); art (skip when both sidecars exist and are valid CART
of the right dims unless `--art-refresh`); playlists from `<Src>/Playlists/*.m3u8` (or
`--playlists DIR`), each line mapped source-path → device path through the scan (an
unmapped line is a warning and dropped); prune only with `--prune` (lists first, needs
`--yes`); `EnsureConfig`/`EnsureLog`; index LAST to `Music/CORELIB.IDX` via temp +
rename; `core eject <drive>` (Windows: `CM_Request_Device_Eject` via the volume's
device instance, or shell out to PowerShell's `Shell.Application` eject as a documented
fallback; Linux/macOS: print `udisksctl`/`diskutil eject`). Copies stream with a 1 MiB
buffer, write-through on Windows, `fsync` elsewhere. `--dry-run` prints the plan with
byte totals. Progress events feed the CLI (one line per album) and later the UI.
**Tests:** temp source tree + temp "device" dir: fresh sync creates everything in the
right order (assert index mtime ≥ all others); second run copies nothing; a changed
file re-copies; `--prune` removes an orphan album only with `--yes`; playlist mapping;
index equals `cidx.Encode(RecordsFromScan)`; the plan for the MC tree against an
empty dst reports 102 albums / 928 tracks when `CORE_PARITY_SRC` is set.
**Device check (needs the iPod in disk mode, run from Windows):** build
`GOOS=windows GOARCH=amd64 go build -o /mnt/c/Users/brandon-home/ipod-bringup/core.exe
./cmd/core`, run `core.exe sync --src C:\Users\brandon-home\Music\MC --dst D:\ --dry-run`
(expect ~0 copies: the device already has the tree; the report must list any real
diffs), then a real `sync`, then `Write-VolumeCache D` is no longer needed but harmless;
boot: About shows 928 songs / 102 albums, no orphans, art chips present. This retires
`build_index.py` + `coreart.py` + `Copy-Item`/`Write-VolumeCache` from the daily flow.
**Done when:** the dry-run on the real device is clean and a real sync + boot shows the
same library the Python index did.

### S5 — `internal/fwpart` + `core firmware inspect`
**Interface:**
```go
type Directory struct{ Version uint16; Start uint32; Entries []firmware.DirectoryEntry }
type Partition struct{ R io.ReaderAt; Size int64 }
func Parse(p Partition) (*Directory, error)          // preamble present, marker, version 2|3, entries until zero row (max 16)
func (d *Directory) OSOS() (idx int, e firmware.DirectoryEntry, ok bool)
func (d *Directory) Capacity(idx int) uint32         // next entry's DevOffset − (e.DevOffset+0x800); last entry → Size − body start
func ReadBody(p Partition, e firmware.DirectoryEntry) ([]byte, error)  // exactly e.Length bytes at DevOffset+0x800
func VerifyEntry(p Partition, e firmware.DirectoryEntry) error         // plain sum == Checksum
type Write struct{ Off int64; Data []byte }
func PlanWrite(d *Directory, idx int, image []byte) ([]Write, firmware.DirectoryEntry, error)
   // Write 1: body zero-padded to 0x800 at DevOffset+0x800 (error if > Capacity)
   // Write 2: the ONE sector (sectorSize-aligned) holding the entry, re-encoded with Length=len(image), Checksum=sum(image), every other field kept
func VerifyWritten(p Partition, e firmware.DirectoryEntry, image []byte) error  // read back body + entry, compare
func CheckPreamble(head []byte) error                 // "{{~~" at 0 and "Copyright(C)" within the first 512 B, "]ih[" at 0x100
```
`core firmware inspect <partition.bin | .ipod | core.bin>` prints the directory table
(like ipodpatcher `-l` plus checksum OK/BAD per entry) or the .ipod header.
**Tests:** a synthetic partition built in-test from the measured facts (preamble text,
directory at 0x4200 v3, the four entries with the real offsets/lengths, random bodies
with correct sums): parse, OSOS found at idx 0 with body 0x5000, capacity 7,618,560,
`PlanWrite` for a 367,608-B image yields a 368,640-B body write and a directory-sector
write whose decoded entry has Length 367608, the plain sum, and addr/ent/vers/la2
untouched; oversize image refused; `VerifyWritten` after applying the writes to the
in-memory image passes, and fails when one body byte is flipped; when
`CORE_FWPART_DUMP` names `after_bootpart.bin` or `bootpartition-backup.bin`: OSOS len
237,640 / 7,618,128, checksum recompute matches, `dpua`/`ebih` mismatch is reported
not fatal. **Do not commit the dumps.**
**Done when:** the synthetic tests pass and both real dumps parse with the OSOS
checksum matching.

### S6 — `internal/disk` + `core info` + `core backup` + `core firmware read`
**Interface:**
```go
type Disk struct{ Path string; SizeBytes int64; SectorSize int; Vendor, Model, Serial string; Removable, USB bool; Volumes []string /* "D:" | /dev/sdb2 | /dev/disk4s2 */; MountPoints []string }
func List() ([]Disk, error)                          // windows: \\.\PhysicalDrive0..31 + IOCTL_STORAGE_QUERY_PROPERTY + IOCTL_DISK_GET_LENGTH_INFO + IOCTL_DISK_GET_DRIVE_GEOMETRY_EX + IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS per volume; linux: /sys/block/sd*, BLKGETSIZE64, BLKSSZGET, /sys/.../device/{vendor,model}; darwin: `diskutil list -plist` + `diskutil info -plist` (no IOKit cgo)
type Handle interface{ io.ReaderAt; io.WriterAt; SectorSize() int; Size() int64; Lock() error; Unlock() error; Flush() error; Close() error }
func Open(path string, write bool) (Handle, error)   // aligned I/O (bounce buffer), windows FILE_FLAG_NO_BUFFERING|WRITE_THROUGH; Lock = FSCTL_LOCK_VOLUME on the drive handle, then FSCTL_LOCK_VOLUME+FSCTL_DISMOUNT_VOLUME on each volume; darwin: caller must `diskutil unmountDisk` (we run it); linux: refuse if any partition is mounted
func IsElevated() bool; func RelaunchElevated(args []string) (exitCode int, log string, err error)  // windows only; others return ErrNeedsSudo with the exact command
type IPod struct{ Disk Disk; FWPartStart, FWPartLen int64; DataPartStart int64; Model string /* "iPod Video 5.5G 80 GB" */; Tested bool }
func FindIPods() ([]IPod, error)                     // MBR part0 type 0x00 + preamble + marker (read-only open), part1 0x0B/0x0C, vendor "Apple" or model containing "iPod"; Tested only for size 76–80 GB
```
Hardware gate: `Tested == false` (30/60 GB, iFlash sizes, anything else) → every write
command refuses without `--untested-hardware`; the error text says only the 5.5G 80 GB
has booted this firmware.
`core info` prints the disk, partitions, sector size, directory table, OSOS
length/checksum status, and (after S8) the firmware version. `core backup [--out]`
dumps the whole firmware partition and fsyncs. `core firmware read --out` writes the
OSOS body (must `cmp` ipodpatcher `-rfb`).
**Tests:** MBR parser on synthetic tables (2048 and 512 sector units); `FindIPods`
against a fake `List` + fake handles; aligned-I/O bounce buffer round-trips at odd
offsets; elevation decision table.
**Device check (read-only, Windows, disk mode):** stage `core.exe`, run elevated via
`powershell.exe -NoProfile -Command "Start-Process cmd.exe -ArgumentList '/c','C:\Users\brandon-home\ipod-bringup\core.exe info > C:\Users\brandon-home\ipod-bringup\core_info.txt 2>&1' -Verb RunAs -Wait"`
(the RunAs child has its own console, so redirect inside `cmd /c`); then `backup --out
…\core_backup.bin` and `cmp` against a fresh ipodpatcher `-r` dump (or the existing
`after_bootpart.bin` only if the partition has not changed — it has; take a fresh
one); `firmware read --out …\core_read.bin` must `cmp` `core/build-hw/core.bin`
(v0.1.2). Also verify `List()` from a non-elevated shell degrades gracefully (Windows
allows read of PhysicalDrive without admin only partially — record what happens).
**Done when:** `core info` identifies the iPod, `backup` equals ipodpatcher's dump byte
for byte, `firmware read` equals `core.bin`.

### S7 — `core flash`
```
core flash <core.ipod | core.bin> [--device PATH] [--dry-run] [--yes] [--backup-dir DIR] [--untested-hardware]
core flash --from-backup <fwpart.bin>      # restore a whole partition (the "recover" path)
```
Sequence (all mandatory, in this order): input parse (`.ipod` checksum verified,
`.bin` accepted with the pack validator's plausibility rules); `FindIPods` (exactly one,
or `--device`); Tested gate; `Parse` + `VerifyEntry` on the current OSOS (a BAD
checksum on the existing image is reported but not fatal — a previous bad flash is what
we may be fixing); print the plan (device, partition offset, body offset, old/new
length, old/new checksum, backup path); `--dry-run` stops here; confirmation = type the
disk path (skipped by `--yes`); Windows: if not elevated → `RelaunchElevated` with the
same args + `--yes` + `--elevated-log`, parent prints the log and exits with the
child's code; `backup` to `--backup-dir` (default `<UserConfigDir>/core/backups`),
fsync; `Lock`; writes from `PlanWrite` (body first, directory sector second); `Flush`;
`Unlock`; re-open read-only and `VerifyWritten`; print "VERIFIED" + backup path, or on
any failure print the backup path and the exact ipodpatcher fallback line. Never write
if the backup did not fsync.
**Tests:** an in-memory `Handle` fake: the full sequence, including a fault injected
after the body write (verify must fail, message names the backup); `--dry-run` writes
nothing; `--from-backup` requires the backup's own preamble/marker/OSOS checksum to be
valid before writing.
**Device check (Windows, disk mode, user says "in disk mode"):** (1) `core flash
core.ipod --dry-run` for the v0.1.2 image; (2) real flash of the SAME v0.1.2 image (the
device already runs it, so a failure is recoverable by `doflash.cmd` and a success is
provable by `ipodpatcher -rfb` + `cmp` from the old script); (3) boot check; (4) from
then on the release flow's "stage + doflash.cmd" step becomes `core flash`; keep
`doflash.cmd` in the folder as the documented fallback for two more releases.
**Done when:** a flash through `core` is read-back verified, independently confirmed by
ipodpatcher `-rfb`, and the device boots.

### S8 — firmware version marker + `core update`
**Firmware change (one commit, own CHANGELOG line):** in `core/kernel/main.c` next to
the version includes:
```c
/* Host-findable version stamp: `core info` scans the OSOS body for this tag. */
const char core_version_marker[] __attribute__((used)) =
    "CORE-FW-VERSION:" CORE_VERSION "|" CORE_BUILD_ID "\0";
```
plus a `verify-hw` script check that `strings core.bin | grep -c '^CORE-FW-VERSION:'`
== 1, and a note in `08-boot-dock.md`. Clicky golden untouched (nothing on the UART).
**CLI:** `internal/fwpart.FindVersion(body []byte) (version, buildID string, ok bool)`;
`core info` prints "firmware: v0.1.3 (build …)" or "firmware: unknown (pre-v0.1.3
image)"; `internal/ghrelease`: `Latest(repo) (Release{Tag, Notes, Assets}, error)`,
`Download(asset, dst) error` (to `<UserCacheDir>/core/<tag>/core.ipod`, checksum
verified via `firmware.ReadIPodFile`), honours `GITHUB_TOKEN` if set; `core update
[--check] [--tag vX.Y.Z] [--yes]`: compare, download, print the release notes, then the
S7 path. `core doctor`: read-only walk — device found, Tested, directory OK, OSOS
checksum OK, version, `Music/` present, `CORELIB.IDX` header/CRC OK and record count vs
caps, `CORECFG.DAT`/`CORELOG.BIN` present and valid, and (elevated) the resolved
`CORECFG.DAT` / `CORELOG.BIN` LBAs computed with the `fat32.c` formula, printed for
comparison with the UART line.
**Tests:** marker found/not found in synthetic bodies; `ghrelease` against an
`httptest` server; `update --check` output; `doctor` on a temp volume.
**Done when:** `core info` on the device shows the version after the next flash and
`core update --check` reports up-to-date against GitHub.

### S9 — CI, release, docs
`ci.yml` `cli` job: matrix build of the five targets with
`-ldflags "-X …/version.Version=$(git describe --tags --always) -X …/version.Commit=… -X …/version.Date=…"`,
artifacts uploaded; new `.github/workflows/release.yml` on `push: tags: v*`: builds the
binaries (`core-linux-amd64`, `core-linux-arm64`, `core-darwin-amd64`,
`core-darwin-arm64`, `core-windows-amd64.exe`, and from S10 the five `core-app-*` GUI builds made on
native runners) and `gh release upload $TAG … --clobber`
(the release itself is created by the human flow with `core.ipod` — the workflow waits
up to ~10 min for it to exist, or creates a draft if it does not). `tools/release.py`:
add the `gh release upload` line to its "next" block and a post-check that lists the
release's assets. `docs/USER_GUIDE.md`: "Install / Update / Add music with `core`"
sections (download the binary, `core info`, `core sync`, `core update`, recovery =
Select+Play, `core flash --from-backup`); `README.md`: one section + the binary table;
`tools/README.md`: the four Python tools marked "reference / parity oracle for
`core`"; `core/cli/README.md` rewritten.
**Done when:** a tag push produces the binaries on the release (five CLI; ten once S10 lands) and the docs
describe the `core` flow first, ipodpatcher second.

### S10 — `core-app`, the native desktop app (REVISED 2026-09-15)

The user's decision: a standalone application, not a web page. Toolkit **Gio v0.8.0**
(`gioui.org`), see §1 decision 1 for why that toolkit and that version. Verified on this
box 2026-09-15 with a throwaway module (scratchpad `giotry/`): `CGO_ENABLED=0
GOOS=windows GOARCH=amd64 go build -ldflags "-H windowsgui"` builds a window exe with no
mingw; the Linux build needs cgo and `-tags novulkan` here (`vulkan/vulkan.h` is absent;
the X11/Wayland/EGL/GLES/xkbcommon headers are present); `gioui.org/gpu/headless`
renders a frame to a PNG on this box through WSLg's Mesa EGL (software path, a DRI3
warning is normal) — that is the review channel; a darwin build cannot be made here at
all (cgo) and is CI's job on a mac runner.

**Binary layout — decision (b), two executables from one module:**
- `core` — the CLI, console subsystem, unchanged.
- `core-app` — `cmd/core-app/main.go`, the Gio window; Windows build with `-H windowsgui`
  so no console flashes. It links the same internal packages directly (no IPC, no server).
*Why not one binary:* a `-H windowsgui` exe has no console, so `core sync` typed in a
terminal would print nothing; without the flag the app launch flashes a console. Two
files avoid both. *Elevated flash on Windows:* the app never relaunches itself; it runs
the `core` CLI that ships beside it (`filepath.Join(filepath.Dir(os.Executable()),
"core.exe")`, else the same name on PATH) as the elevated child — `flash <file> --yes
--no-relaunch --elevated-log <file>` — via the disk package — **checked: both
`disk.RelaunchElevated` variants (`elevate_windows.go:94`, `elevate.go:109`) call
`os.Executable()` themselves**, so add `disk.RelaunchElevatedExe(exe string, args
[]string)` in both OS files, make `RelaunchElevated` a one-line wrapper over it, and have
the app set `flasher.Deps.Executable` to the CLI path and `Deps.Relaunch` to a closure
over `RelaunchElevatedExe(cliPath, …)`; the printed elevation line then names `core.exe`
too. If the CLI is missing beside the app, the Firmware card says so and shows the
command to run; the app does not embed a hidden headless-flash mode. Linux/macOS: the
app prints the `sudo core flash …` line in its log pane (same as the CLI).
Release assets become ten: the five `core-*` plus `core-app-linux-amd64`,
`core-app-linux-arm64`, `core-app-darwin-amd64`, `core-app-darwin-arm64`,
`core-app-windows-amd64.exe`. `tools/release.py --check` expects eleven assets
(`core-<tag>.ipod` + ten).

**Package `internal/app` (≤ ~2000 lines with tests):**
```go
// state.go — one model, mutated only by the UI goroutine
type Device struct{ Found bool; Path, Model, Serial string; Size int64; SectorSize int; Tested bool; Volume string; Firmware string /* fwpart.VersionText */; OSOSOK bool; OSOSNote string; Err string }
type Library struct{ Present bool; Songs, Albums, Genres int; IndexBytes int64; ConfigValid, LogValid bool; Note string }
type Release struct{ Checked bool; Tag, Notes, Asset string; Err string }
type State struct{ Device Device; Library Library; Release Release; Source string; Job *JobStatus; Log []string /* ring, 2000 lines */ }
type JobKind int  // JobRefresh, JobDryRun, JobSync, JobSyncPrune, JobCheck, JobUpdate, JobFlash, JobBackup, JobEject
type JobStatus struct{ Kind JobKind; Text string; Pct float32 /* -1 = indeterminate */; Done, Failed bool }
type Event struct{ Kind EventKind /* Log|Progress|Done|Error|Confirm */; Job JobKind; Text string; Pct float32 }

// jobs.go — the runner: one job at a time, events over a channel, the UI drains it each frame
type Backend interface {
    Refresh(ctx context.Context) (Device, Library, error)                    // disk.FindIPods → fwpart.Parse/OSOS/VerifyEntry/ReadBody+FindVersion; doctor.CheckVolume for Library
    Sync(ctx context.Context, o syncer.Options, emit func(Event)) error      // ScanSource → CheckPaths → MakePlan → Execute (Progress wired to emit)
    Release(ctx context.Context) (Release, error)                            // ghrelease.Latest + FirmwareAsset
    Download(ctx context.Context, rel Release, emit func(Event)) (string, error)
    Flash(ctx context.Context, file string, confirm func(prompt string) (string, error), emit func(Event)) error // flasher.Flash with Deps{Out: emit-writer, Confirm: confirm, Executable: cliBeside, ChildArgs: []string{"flash", file, "--backup-dir", …}}
    Backup(ctx context.Context, emit func(Event)) (string, error)
    Eject(ctx context.Context, volume string) error                          // eject.Eject
}
type Runner struct{ … }  // Start(kind, func(ctx, emit) error) → error if busy (ErrBusy); Cancel(); Events() <-chan Event
func NewRealBackend() Backend   // the production wiring; tests use a fake

// ui.go — Gio
func Run(o Options) error               // app.NewWindow(Title "Core", MinSize 720×520), the event loop, drains Runner events, calls Layout
type Options struct{ ConfigPath string; Screenshot string; Backend Backend }
func (u *UI) Layout(gtx layout.Context) layout.Dimensions   // pure: State → widgets
// config.go — <UserConfigDir>/core/config.json {source, backup_dir}; Windows path handling
// snapshot.go — func Snapshot(st State, w, h int, out string) error : renders Layout via gioui.org/gpu/headless to a PNG (build tag: not on windows-without-cgo? headless is pure Go on Windows too — verify; if it needs cgo on Linux only, guard with a build tag and skip)
```
`core-app --screenshot <png> [--state demo|empty|nodevice]` renders one frame to the file
and exits (no window), using a canned `State` — this is how the orchestrator reviews the
layout from WSL, and how the layout test works. `core-app --source DIR` presets the
folder. Config file shared with the CLI's future needs (`source`, `backup_dir`).

**Screens (one window, vertical stack, scrollable, Linen palette from
`docs/screens/render.py` converted to 24-bit — SURFACE #F7F3EF, INK #191410, MUTED
#7B716B, MUTED2 #9C8E84, MUTED_D #5A514A, ACCENT #C56942 (progress and the primary
button only, as on the device), BORDER #E6E3DE, PLATE #F7F7F7, TRK #DEDBD6, SEL_SUB
#B5B2AD, SEL_TRK #423D3A, PILL_OFF #CECAC5; Gio's `gofont` collection is fine, Nunito not
required):**
1. **Device card** — "iPod Video 5.5G · 80 GB · serial · D:" or "No iPod found — put it
   in disk mode (Select + Play) and plug it in", the Tested badge, `firmware: v0.1.2
   (build …)` / `unknown (images before v0.1.3 carry none)`, OSOS checksum OK/BAD,
   library line (`928 songs · 101 albums · index 237,584 B`, config/log valid), a
   **Refresh** button. Unelevated on Windows: the card explains reads need Administrator
   and shows the command (the CLI's advice text).
2. **Music** — source folder text field + **Browse…** (Gio has no picker: Windows =
   `powershell -NoProfile -Command "Add-Type -AssemblyName System.Windows.Forms;
   $d=New-Object System.Windows.Forms.FolderBrowserDialog; if($d.ShowDialog() -eq 'OK'){$d.SelectedPath}"`,
   macOS = `osascript -e 'POSIX path of (choose folder)'`, Linux = `zenity
   --file-selection --directory` else `kdialog --getexistingdirectory` else disabled with
   a hint), saved on change; buttons **Dry run**, **Sync**, **Sync + prune** (two
   confirmations: a dialog, then the same dialog listing the orphan count from a dry run
   — implement as dry-run-first, then prune with `Yes: true`); a progress bar (albums done
   / total from the plan) and the per-album lines in the log.
3. **Firmware** — "installed v0.1.2 · latest v0.1.3" (latest blank until **Check**),
   buttons **Check**, **Update** (download → flash), **Flash file…** (a path field +
   button; the picker as above with a file dialog variant), **Backup** (writes to the
   configured backup dir, prints the path). The **flash confirmation dialog** shows the
   plan text the flasher printed (captured from `Deps.Out` before `Confirm` is called) and
   requires typing the device path exactly, like the CLI; Cancel aborts. On Windows the
   dialog also says a UAC prompt will appear for `core.exe`.
4. **Eject** — one button, enabled when a volume is known; result in the log.
5. **Log pane** — monospace, the last 2000 lines, auto-scroll, a **Copy** button
   (clipboard via `gioui.org/io/clipboard`).
Buttons are disabled while a job runs; a **Cancel** appears for sync/download (the
flasher ignores cancel between its two writes by design — say so in the button tooltip
text: "cannot cancel once writing").

**Extractions (thin delegations, tests kept green):** `internal/cli/doctor.go`'s check
functions → `internal/doctor` (`CheckDevice`, `CheckVolume`, `CheckIndex`, `CheckConfig`,
`CheckLog`, `CheckPlaylists` returning a `[]Check{State, Name, Text}` instead of printing;
the CLI prints them); `internal/cli/eject*.go` → `internal/eject` (`Eject(w io.Writer,
target string) error`, per-OS files moved as-is; the CLI calls it).

**Tests (no window):** `Runner` with a fake `Backend`: one job at a time (`ErrBusy`),
events arrive in order, cancel propagates, a failing job ends with `Error`; `State`
reducers (an `Event` applied to `State` yields the expected `JobStatus`/`Log`);
`Flash` wiring: the fake backend records the `Deps` the real backend would build —
better: a `buildFlashDeps(file, cliPath, confirm, emit)` function tested directly for
`ChildArgs == ["flash", file, "--backup-dir", dir]`, `Executable` = the CLI beside the
app, `Confirm` = the dialog's answer; config round-trip; the picker command per OS
(strings only). **Layout test:** `TestSnapshot` renders the demo state at 900×600 and
720×520 through `headless` into `$CORE_SNAPSHOT_DIR` (default the scratchpad `app_shots/`)
— on this box it runs (cgo, `-tags novulkan`); it `t.Skip`s when `headless.NewWindow`
errors (no EGL). CI: the Linux job installs `libwayland-dev libx11-dev
libxkbcommon-x11-dev libgles2-mesa-dev libegl1-mesa-dev libffi-dev libxcursor-dev` and
builds with `-tags novulkan`; the mac runner builds darwin/{amd64,arm64} with cgo; the
Windows binary is cross-built on Linux with `CGO_ENABLED=0`. `build-all.sh` grows the
`core-app-*` targets; the darwin ones are produced only where `GOOS=darwin` cgo works
(the script skips them with a notice elsewhere, and `release.yml` gains a
`macos-latest` job for them).

**Local run (device not required):** `cd core/cli && GOOS=windows GOARCH=amd64
CGO_ENABLED=0 go build -ldflags "-H windowsgui" -o /mnt/c/Users/brandon-home/ipod-bringup/core-app.exe ./cmd/core-app`,
then `/mnt/c/Windows/System32/cmd.exe /c start "" C:\Users\brandon-home\ipod-bringup\core-app.exe`
opens the window on the user's desktop (the orchestrator cannot see it; it can only check
the process started and that `core-app.exe --screenshot C:\…\app.png --state demo` wrote
a PNG, which it then reads from `/mnt/c`). The Linux build (`go build -tags novulkan
./cmd/core-app`) opens a window through WSLg too. Review = the two headless PNGs plus the
Windows `--screenshot` PNG, read as images.

**Done when:** `go test ./...` green including the snapshot test on this box; both
PNGs show the five sections laid out at 720×520 without overlap; `core-app.exe` starts
on Windows and its `--screenshot` matches the headless render; the fake-backend tests
cover every button's job; the CLI's `doctor` and `eject` tests still pass after the
extraction; README/guide name `core-app` as the app and `core` as the CLI; the
release set is eleven assets.

---

## 4. Reviewer checklist (per slice)

- Byte-level contracts have a test against the *other* side (Python oracle, C header
  grep, golden vectors, dump facts) — not just a Go-vs-Go round trip.
- No write path without: iPod proven three ways, backup fsynced, read-back compared,
  `--dry-run`, typed confirmation. Preamble/partition table/other entries never
  written. Body offset is `DevOffset + 0x800`; checksum has no seed; padding is zeros.
- Sector size comes from the OS and is cross-checked by the preamble position.
- Nothing deletes music without `--prune --yes`; `EnsureConfig` never resets a valid
  record; the index is the last file written.
- Skips, not failures, when oracles are absent; never a network call in tests.
- No cgo; cross-builds for windows/amd64 and darwin/arm64 from this box.

## 5. Decisions the user may want to reverse

- **ipodpatcher is replaced, not wrapped.** `doflash.cmd` stays as the fallback for two
  releases; recovery floor is still ROM disk mode.
- **Windows elevation = the binary re-launches itself via UAC** (a second console
  window flashes up; output comes back through a log file). Alternative: refuse and
  print an "open an elevated terminal" line, as on Linux/macOS.
- **GUI = a native Gio window in a second executable, `core-app`** (revised 2026-09-15 at the
  user's request from the embedded web page). Consequences: the Linux/macOS builds need cgo
  and native CI runners, Gio is pinned at v0.8.0 to stay on Go 1.22, and the release carries
  ten binaries plus the firmware image.
- **Source layout stays `Album - Artist`** and the device layout `Artist - Album`, exactly
  as today; the app does not re-tag or re-name sources.
- **Art parity is a tolerance, not byte identity** (ffmpeg dithers and scales in YUV);
  existing sidecars are kept unless `--art-refresh`, so the device's current art does not
  churn on the first Go sync.
- **The Python tools stay in `tools/`** as oracles; `tagcache`/`artistart` are deleted.
- **A one-line firmware change** (the version marker) is needed for `core info`/`update`
  to know what the device runs; older images report "unknown".
- **CI does not build the released `core.ipod`**; the flashed local build remains the
  release asset. CI adds the host binaries only.
