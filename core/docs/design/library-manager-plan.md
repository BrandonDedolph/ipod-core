# Plan — phase 2 of `core-app`: install, auto-detect, and the library manager

Addendum to `companion-app-plan.md` (S0–S10 shipped as v0.1.3, 2026-09-15). The user's
phase-2 asks, 2026-09-15: the app finds the iPod by itself and the only time it stops is when
Core is not on it (one-button install); it manages the library — pulls missing cover art,
corrects filenames from tags, organizes folders from tags — with a preview and undo; it
should be "easy to love". Windows is the only target that matters now; the code stays
portable, no effort goes into other OSes. UI direction is the user's call (leading: B,
album grid + a Library tab with a "things to fix" list); the UI slices here are written
against B and are the only slices that change if another direction wins.

Written from the tree at 823afef. Everything below was checked against the source unless
marked "verify".

---

## 0. Facts the implementer must not re-derive

**The locator contract (`internal/library/scan.go`, `names.go`).** The device filename is
`fmt.Sprintf("%02d. %s.flac", pos, FatSafe(TrackTitle(base)))` where `pos` is the file's
1-based position in the **byte-sorted** listing of `*.flac` in its folder (or, with `Disc N/`
subfolders, the sorted discs then the sorted files within each) and `TrackTitle(base)` splits
the stem on `" - "`: ≥3 parts → `parts[2:]` joined, 2 parts → `parts[1]`, else the stem.
The device folder is `FatSafe(artist + " - " + album)` with artist/album split from the
SOURCE folder name on its LAST `" - "` (`SplitAlbumArtist`: source = `Album - Artist`).
Tags decide only what is DISPLAYED (title, disc, track, genre) and the record order
`(disc, track, pos)`. The record's `file_hash` = `NameHash(DeviceName)`: **rename a source
file and its device name changes, so the old file on the iPod becomes an orphan and the
track is re-copied on the next sync.** This is the whole cost model of "fix filenames".

**The source convention that already produces the right locator** (what the MC tree
uses, e.g. `01 - Morgan Wallen - I'm The Problem.flac`): folder `Album - Artist/`,
optional `Disc N/` subfolders, file `NN - Artist - Title.flac`. Sorted byte order of
zero-padded `NN` is track order, so `pos == NN`, and `TrackTitle` returns `Title`. The
organizer writes exactly this convention; it never invents a new one.

**Track number and title from tags** (`TrackNumber`, `probeFile`): `track` = tag
`tracknumber`/`track` (leading integer) else the filename's leading number (`LeadTrack`)
else the position; `disc` = `Disc N/` folder else tag `discnumber`/`disc` else the filename's
`D-NN` else 1; `title` = tag `title` else `TrackTitle(base)`; `artist` for the record = the
FOLDER artist (tag `artist`/`albumartist` only when the folder has none, which the scan
skips anyway). Extension match is case-sensitive: `.FLAC` files are warned and skipped.

**Art today.** `syncer` keeps `folder.art`/`folder.thm` when both are present and pass
`coreart.Valid` at the exact size, regardless of the source picture; `--art-refresh`
re-renders everything. The art source is the album's `Pos == 1` file's front cover
(`artSource`). `internal/flac` READS `PICTURE` blocks; there is no writer.

**Firmware directory (`internal/fwpart`).** `PlanWrite` rewrites the OSOS row with a
new `Length` and `Checksum` and preserves every other field. On a STOCK iPod the OSOS
row is Apple's: `len 7,618,128, entryOffset 0x736000, addr 0x10000000, vers 0xB012,
loadAddr2 0xFFFFFFFF` (from `bootpartition-backup.bin`, plan §0 and the fwpart real-dump
test). Our image needs `entryOffset 0` (crt0 is at the image start); `addr`, `vers`,
`loadAddr2` are the same values ipodpatcher leaves in place and must stay. **Flashing a
stock iPod with today's `core flash` writes a row whose entry point is 7.5 MB into a 368 KB
image**: recoverable by Select+Play, but it is a brick on first use. ipodpatcher `-wf`
zeroes `entryOffset`. `VerifyWritten` compares the whole row, so once `PlanWrite` sets the
field the read-back proves it.

**Version marker.** `fwpart.FindVersion(body)` finds `CORE-FW-VERSION:<v>|<build>`; images
before v0.1.3 have none. `FindVersion` ok ⇒ Core. Not ok ⇒ either an old Core (entryOffset
0, length < 1 MiB) or somebody else's firmware.

**Device state (`internal/disk`, `internal/app`).** `FindIPods()` opens each physical
drive read-only (Administrator on Windows; the app's manifest has it), reads the MBR and
the preamble, and returns `IPod{Disk{Path, Serial, Model, Volumes, MountPoints}, Tested,
FWPartStart/Len, SectorSize}`. `app.State` has `Device{Found, Path, Model, Serial, Size,
Tested, Volume, Firmware, OSOSOK, Elevated}`, `Library{Songs, Albums, Genres, ConfigValid,
LogValid}`, `Job`, `Log`; `Runner` runs one job; `Backend` is the interface the UI calls
(`Refresh, Sync, Release, Download, Flash, Backup, Eject`). The four-card layout lives in
`ui.go`; `actions.go` holds one starter per button. `devicefs.EnsureConfig/EnsureLog`
create the two device files; `Music/` is created by the syncer's first copy.

**Cover-art sources (verify live once, outside tests).** iTunes Search API:
`GET https://itunes.apple.com/search?term=<q>&media=music&entity=album&limit=5`, JSON
`results[].{artistName, collectionName, artworkUrl100}`; `artworkUrl100` ends in
`100x100bb.jpg` and the same path with `1200x1200bb.jpg` serves the large original for
nearly every album (fall back to `600x600bb.jpg`, then the 100). No key, no documented
rate limit, ~20 req/min is polite. MusicBrainz: `GET
https://musicbrainz.org/ws/2/release/?query=artist:"<a>" AND release:"<b>"&fmt=json&limit=5`,
**mandatory `User-Agent: core-app/<version> (https://github.com/BrandonDedolph/ipod-core)`**
and **1 request/second**; results `releases[].{id, title, artist-credit[].name, score}`.
Cover Art Archive: `GET https://coverartarchive.org/release/<mbid>/front-500` (302 to
archive.org; `front-1200` exists too; 404 = no art). The art is the rights-holder's: the
app writes it into the user's own files and its own cache only, never into the repo, a
release, or anywhere shared.

---

## 1. Decisions

1. **Install is `flash` with an entry-point policy, not a new writer.** `fwpart.PlanWrite`
   gains a `Policy` argument: `KeepEntry` (today's behaviour, used by `--from-backup` and
   for re-flashing a Core image where the field is already 0) and `CoreImage` (`EntryOffset
   = 0`, everything else preserved). The flasher passes `CoreImage` for every `.ipod`/`.bin`
   image, always — a Core image's entry point is its first byte on every device, so there
   is no case where keeping Apple's field is right. *Why:* one write path, one checklist,
   one set of tests; the field is a property of the image, not of the device.
2. **"Core not installed" is a three-way predicate on the OSOS row + body**, computed by
   `fwpart.Classify(body, entry) Installed` → `InstalledCore{Version}` (marker found),
   `InstalledCoreOld` (no marker, `EntryOffset == 0`, `Length < 1 MiB`), `InstalledOther`
   (anything else: Apple's `entryOffset 0x736000 / 7.6 MB`, a Rockbox-patched OSOS). The
   app's launch stop is `InstalledOther`; `InstalledCoreOld` is an ordinary "update
   available". *Why:* the marker alone cannot tell v0.1.2 from Apple; the row can.
3. **The Apple backup is named so a person can find it, and never pruned.**
   `<UserConfigDir>\core\backups\apple-<serial>-<yyyy-mm-dd>.bin` on an `InstalledOther`
   install (ordinary flashes keep `fwpart-<size>-<time>.bin`). The install's last line and
   the app's Install card both print it with the restore command. *Why:* it is the only copy
   of that device's Apple firmware; Apple no longer hosts it.
4. **Auto-detect is a 2-second poll of `disk.FindIPods`, owned by the app, paused during
   jobs.** No OS device-change notifications (WM_DEVICECHANGE needs a window procedure Gio
   does not expose). The poll compares the found set to the last one by `Disk.Path+Serial`;
   on a change it runs `Refresh` (the doctor) once as a job. *Why:* ~2 raw opens per second
   is free, and it also catches the unplug.
5. **The organizer changes names and folders only, in place, journaled.** No tag writing in
   this phase except embedding a front cover. Files whose tags lack `title`, `artist` or
   `album` are "needs attention" items, never guessed; an inline tag editor is a later slice.
   *Why:* the tags are the truth the app is about to trust; rewriting them silently would
   destroy that.
6. **The organizer writes the existing source convention**, `Album - Artist/[Disc N/]NN -
   Artist - Title.flac`, with `NN` = the tag track number (zero-padded to 2, or 3 when an
   album has ≥100 tracks), `Disc N/` only when the album has more than one disc. *Why
   (load-bearing):* `library.ScanTree` on the result must produce `pos == NN` and
   `TrackTitle == Title`, so the index names exactly the files that exist. `" - "` inside an
   artist or title is replaced by `" – "` (en dash) in the FILENAME only (never in tags),
   because `TrackTitle` splits on `" - "`; the title on the device comes from the tag and is
   unaffected. A test proves: organize a fixture → `ScanTree` → every `DeviceName ==
   fmt("%02d. %s.flac", track, FatSafe(tagTitle))` and every `NumberFrom == "tag"`.
7. **Renames are visible costs.** The organizer's preview counts how many tracks will be
   re-copied to the iPod (every renamed file, since the locator changes) and the Fix screen
   says so; the next sync copies them and lists the old names under prune. *Why:* a 2 GB
   re-copy nobody expected reads as a bug.
8. **Undo is a journal replayed backwards, verified.** Before the first rename the plan is
   written to `<UserConfigDir>\core\journal\<RFC3339>.json` (`{From, To}` per op, plus the
   file size and mtime at `To` after the move); `Undo` walks it in reverse, refuses any op
   whose `To` is missing or changed size, and applies the rest, reporting what it skipped.
   The journal is kept until the user starts the next sync (then archived, not deleted).
   *Why:* an undo that could clobber a newer file is worse than none.
9. **Art fetching is confirm-first.** `artfetch` returns ranked candidates with a 200 px
   thumbnail; the app shows the match beside the album name and the user accepts per album
   or "accept all ≥ 0.9"; the CLI's `--yes` accepts the top candidate only when its score is
   ≥ 0.9. iTunes first (no key, big images), MusicBrainz + Cover Art Archive second (one
   request per second, mandatory User-Agent). Score = 1.0 for normalized artist and album
   equal, 0.9 for equal after stripping `(deluxe|remaster|edition|feat.…)` parentheticals,
   0.7 for containment, else the provider's own rank scaled below 0.5. *Why:* wrong art on
   the iPod is the most visible mistake this app can make.
10. **Embedding art rewrites only the metadata chain.** `flac.WritePicture` inserts a
    `PICTURE` (type 3, the given MIME/dimensions) into the block chain; it consumes
    `PADDING` when there is enough, and otherwise rewrites the file once with 8 KiB of new
    padding. The audio frames are never touched (a test hashes them before and after). An
    existing type-3 picture is replaced. *Why:* a 25 MB rewrite per album is fine once;
    silently changing audio bytes is never fine.
11. **Sidecars follow the source picture.** `syncer` re-renders `folder.art`/`folder.thm`
    when either is missing/invalid (today) OR older than the art-source FLAC's mtime (new).
    `--art-refresh` stays as the force. *Why:* embedding art must reach the iPod on the next
    sync without a flag.
12. **The Library tab is a report, not a wizard.** `librarian.Inspect` returns one `Report`
    (missing art, misnamed, unorganized, needs attention, counts and byte costs);
    `librarian.Fix` applies the ticked parts as ONE job with one journal; Undo reverts that
    job. Sync stays a separate button. *Why:* one preview, one button, one undo is the
    "easy to love" shape; a five-step wizard is not.
13. **Direction B's grid is fed by a host thumbnail cache**: 120 px PNGs under
    `<UserCacheDir>\core\thumbs\<sha1 of source path + size + mtime>.png`, decoded from the
    album's art source once, loaded lazily on the UI goroutine's behalf by a worker. *Why:*
    101 JPEG decodes on every repaint would freeze Gio; on disk they survive restarts.

---

## 2. Package layout (additions)

```
internal/fwpart/      Policy, Classify (+ tests on the Apple-shaped synthetic partition)
internal/flasher/     Options.Policy default CoreImage; Result.AppleBackup
internal/installer/   Install(ctx, Options, Deps): classify → apple backup → flash → devicefs → verify
internal/flac/        write.go: WritePicture, RemovePictures, block-chain rewrite
internal/organizer/   scan (tags → canonical paths), Plan/Move, Apply (journal), Undo
internal/artfetch/    Provider interface, itunes.go, musicbrainz.go, score.go, cache.go
internal/librarian/   Inspect → Report; Fix(report, choices) → journal; progress events
internal/devicefs/    volume label read/write (Windows), LegalLabel
internal/thumbs/      host thumbnail cache for the grid
internal/app/         detect.go (poll), install state, grid.go, library_tab.go, fixview.go
internal/cli/         install.go, organize.go, `art --fetch`, `library inspect|fix|undo`, name.go
```

Cross-cutting rules (unchanged from phase 1): gofmt/vet/`go test ./...` green with and
without `-tags novulkan`; `CGO_ENABLED=0 GOOS=windows` builds for both binaries; no new
dependency beyond what exists (`golang.org/x/image`, `gioui.org v0.8.0`); every test that
needs the network, the MC tree or the device skips with a reason; tests never touch the
real MC tree (copy ≤ 3 albums into `t.TempDir()`); the organizer's fixtures are real FLACs
from `flac.BuildFile`.

---

## 3. Slices (ordered; one Opus run each; ≤ ~1500 lines incl. tests)

### L1 — entry-point policy + `core install` (safety-critical, small)

**STATUS: DONE, 2026-09-15** (unflashed — no stock iPod is on hand; the only real
device runs Core). `fwpart.Policy`/`Classify`/`ClassifyEntry`/`ClassifyPartition`,
`PlanWrite(..., pol)`, `flasher.Options.Install` + `Result.Installed` +
`AppleBackupFileName`, `core install`. Run against the real dumps: Apple's
`bootpartition-backup.bin` classifies `Other` — "Apple firmware (7.6 MB, entry
0x736000)" — and `after_bootpart.bin` (our v0.1.2-era image) classifies `CoreOld`.
Two deviations from what is written below, both deliberate: `InstalledKind` is a
string type, not an iota, so the zero `Installed` cannot read as "Core is
installed"; and there is no `Options.Policy` field on the flasher — the flasher
passes `fwpart.CoreImage` at its one `PlanWrite` call, because an option whose
only other value produces a device that does not boot (and whose zero value,
`fwpart.KeepEntry`, is exactly that one) is a trap, not a knob. There is no
`internal/installer` package either: the sequence around the flasher is 300 lines
of `internal/cli/install.go` with the device read behind one seam, and the app
will call it through L6's backend.

**fwpart.** `type Policy int; const (KeepEntry Policy = iota; CoreImage)`;
`PlanWrite(d, idx, image, sectorSize, pol Policy)` — `CoreImage` sets
`updated.EntryOffset = 0`; nothing else changes. `Classify(e firmware.DirectoryEntry, body
[]byte) Installed` with `type Installed struct{ Kind InstalledKind; Version, BuildID string
}` and kinds `Core`, `CoreOld`, `Other` per decision 2 (`Other` also carries a one-line
description: "Apple firmware (7.6 MB, entry 0x736000)" / "unknown firmware (…)").
**flasher.** `Options.Policy` (zero value = `CoreImage`; `--from-backup` never consults it);
the plan print shows `entryOffset 0x736000 → 0x0` when it changes; `Result.Installed
Installed` (what was there before). When `Installed.Kind == Other` the backup path is the
Apple name from decision 3 and the VERIFIED line adds "Apple firmware kept at <path>;
`core flash --from-backup <path>` puts it back".
**installer.** `Install(ctx, Options{Image or Release, Device, Yes, BackupDir, Untested},
Deps)`: find the iPod; classify; if `Core`/`CoreOld` say so and defer to `update`; else
flash via the flasher with the Apple backup name; then on the FAT volume
`devicefs.EnsureConfig`, `EnsureLog`, `MkdirAll(Music)`; then re-read and print
`firmware: vX (was: Apple firmware)`. `core install [<core.ipod>] [--device] [--yes]
[--dry-run] [--backup-dir] [--untested-hardware]` (no image → latest release via
`ghrelease`). `core info` prints `firmware: Apple firmware — Core is not installed (core
install)` for `Other`.
**Tests.** fwpart: on the synthetic partition, an Apple-shaped OSOS row (len 7,618,128,
entryOffset 0x736000) + `CoreImage` → the planned row has `EntryOffset 0`, `Length`/`Checksum`
new, `Addr/Version/LoadAddr2` identical; `KeepEntry` leaves 0x736000; the real-dump test
(`CORE_FWPART_DUMP` on `bootpartition-backup.bin`) classifies `Other`, `after_bootpart.bin`
classifies `CoreOld` (v0.1.2 era body has no marker), and a body with the marker
classifies `Core`. flasher: the full sequence on the Apple-shaped fake device ends with a
row whose entry point is 0 and a backup named `apple-<serial>-<date>.bin`; a Core-shaped
device keeps `fwpart-…`. installer: fake device + fake volume dir → config/log/`Music/`
exist afterwards; `--dry-run` writes nothing.
**Device check (later, on a stock iPod — none is on hand; the only real device runs Core):**
none possible now; say so in the report. The synthetic Apple-shaped test IS the gate.
**Done when:** `core flash` of a Core image onto the Apple-dump-shaped fixture produces a
bootable row (entry 0) and the real Apple dump classifies as `Other`.

### L2 — `internal/organizer` + `core organize`

**Status: DONE 2026-09-15** (`internal/organizer/{organizer,apply}.go` + tests,
`internal/cli/organize.go` + tests). The load-bearing claim HOLDS: a `--dry-run` against a
copy of `RUNNING WILD - Cameron Dallas`, `DOA - ericdoa` and `I'm The Problem - Morgan
Wallen` is EMPTY, and organizing a junk-renamed copy of `DOA - ericdoa` reproduces the MC
names byte for byte (then undoes back to the byte). Eight corrections to what is written
below (7 and the amendment to 1 came out of a `core fix --dry-run` on the REAL library,
2026-09-16):

1. **A difference the locator hash cannot see is not worth a rename.** The first run against
   the real MC copy wanted to move all 37 Morgan Wallen tracks, because the folder and files
   spell `I'm The Problem` with an ASCII apostrophe and the TAGS spell it with U+2019. That
   is 790 MB re-copied for a typographic difference the device cannot even perceive
   (`NormKey` folds curly quotes, so the record resolves either way) — and `library` already
   has this rule for the display name (`Straighten(FatSafe(album)) == Straighten(folderAlbum)`).
   So an existing folder or file name that equals the canonical one after folding curly
   quotes is KEPT. **Amended 2026-09-16: the fold is the WHOLE typographic set `NormKey`
   folds — the en/em dashes too, not just the quotes.** The first `core fix --dry-run`
   against the real MC tree listed `our little angel - EP - ROLE MODEL` as unorganized for
   one reason: the tag album spells the EP with U+2013. The locator is
   `NameHash(NormKey(name))`, so the two names resolve to the SAME file on the device, and
   the worry that ` - ` is the field separator is answered by the canonical form itself —
   `CanonicalFolder` en-dashes both halves, so the ASCII ` - ` it compares against is the
   separator. The rule is now exactly "`NormKey(existing) == NormKey(canonical)`, except
   that a case-only difference is still a rename", implemented as
   `library.FoldTypography(existing) == library.FoldTypography(canonical)` —
   `FoldTypography` is `NormKey` minus the lower-casing, exported so organizer reuses
   `library`'s fold instead of keeping a second table (`TestFoldTypographyIsNormKeyWithoutTheCase`
   pins `NormKey(s) == strings.ToLower(FoldTypography(s))`). Case is still NOT folded: the
   device shows the folder's own spelling, and a case-only device name is a rename on FAT,
   not a copy. Nine files, one album, off the list.
2. **The title does NOT get the en-dash treatment; only the artist does.** `TrackTitle`
   re-joins `parts[2:]` with `" - "`, so a title containing the separator round-trips
   exactly. En-dashing it would break the slice's own test — `DeviceName ==
   fmt("%02d. %s.flac", track, FatSafe(tagTitle))` — since the tag would still hold the
   hyphen. The album half of the folder does not need it either (the split is on the LAST
   separator) but it is applied there, as written, so the folder cannot be re-split wrongly
   by a future change.
3. **`pos == track` only holds within ONE disc.** The enumeration keeps counting across
   `Disc N/` folders, so disc 2 track 1 is `pos` 3 in a 2+2 album. The test asserts the
   locator identity (`DeviceName` from `pos` and the tag title, `NumberFrom == "tag"`,
   `Track` == the tag) everywhere and `pos == track` for single-disc albums. It also does
   not hold when an album's numbering has a GAP (the MC `RUNNING WILD` folder is 01, 04..10):
   that is ordinary drift, the index is still correct, and the organizer leaves it alone.
4. **`<RFC3339>.json` is not a legal Windows filename** (colons). Journals are
   `<user config dir>/core/journal/20060102-150405.json`, local time, `-2` on a collision.
5. **The journal is written in FULL before the first rename**, with each op's size and mtime
   taken from the SOURCE (rename preserves both), not appended after each move. Appending
   after the fact is exactly the record that is missing when it is needed; each completed op
   then flips a `done` flag and the file is rewritten and fsynced.
7. **The multi-disc split is opt-in (`Options.DiscFolders`, default OFF), added
   2026-09-16.** The same real-library dry run wanted to move all 43 files of
   `SWAG II - Justin Bieber` into `Disc 1/` and `Disc 2/` because their tags carry disc
   numbers. Nothing is wrong with that album: `library.ScanTree` reads a flat folder as one
   enumeration and `TrackNumber` takes disc and track from the TAGS, so it already indexes,
   sorts and plays correctly — the split is a 43-file re-copy for a layout nobody can see on
   the device. So a FLAT multi-disc album stays flat by default and its files keep the
   leading number they already carry (`library.LeadTrack` reads it back, `D-NN` and all),
   because `DeviceName` is the file's POSITION in the byte-sorted folder and renumbering the
   album reshuffles every one of them; only the artist and title in the name are
   canonicalised. An album ALREADY in `Disc N/` folders keeps them — this option is about
   splitting a flat one, never about flattening. The moves the split would make are reported
   in `Plan.DiscSplit` (`Reason: "disc"`), are not in `Plan.Moves`, cost nothing in the
   re-copy line, and `--disc-folders` on both `core organize` and `core fix` turns them on.
8. `Canonical(m, multiDisc, width)` exists as written but the organizer does not use it: an
   album's artist is decided ONCE for the album (majority `albumartist`, else majority
   `artist`, a tie is a question), because `library` gives every track in a folder the
   FOLDER's artist. `Scan` also returns `TracksNew` beside `TracksRecopied` — a nested
   album (`Downloads\X\`) is invisible to `ScanTree` today, so its tracks are first copies,
   not re-copies. One nit left open: an album-local `.m3u` (MC keeps one per folder) stays
   behind when the tracks move, since it is not one of the listed sidecars.

**Interface.**
```go
type Options struct{ Root string; Meta func(string) (*flac.Meta, error); JournalDir string; Progress func(Event); DiscFolders bool }
type Move struct{ From, To string; Reason string /* "rename" | "move" | "disc" */; Bytes int64 }
type Attention struct{ Path, Reason string }              // missing title/artist/album/track, unreadable, duplicate target
type Plan struct{ Moves []Move; DiscSplit []Move; Attention []Attention; AlbumsTouched, TracksRecopied int; Journal string }
func Scan(o Options) (*Plan, error)                        // never writes
func Apply(ctx context.Context, p *Plan, o Options) (*Journal, error)
func Undo(ctx context.Context, journalPath string, o Options) (*UndoReport, error)
func Canonical(m *flac.Meta, multiDisc bool, width int) (folder, disc, file string, ok bool) // the naming rule, exported for the librarian and the tests
```
**Rules.** Walk `Root` one level deep (folders) plus `Disc N/` (the scan's shape) plus any
deeper folder that contains FLACs (the "unorganized" case: `Downloads\RUNNING WILD\`); for
each FLAC read tags: `album` (fallback `albumartist`-less files → attention), `artist` =
`albumartist` else `artist` (the FOLDER artist is what the index uses, so the album's
artist is decided ONCE per album by majority of `albumartist`, else majority of `artist`;
a split vote is attention), `title`, `tracknumber`, `discnumber`, `totaldiscs`/the max disc
seen. Canonical folder `FatSafe(album + " - " + artist)` with `" - "` in either replaced by
`" – "` before the join; canonical file `fmt("%0*d - %s - %s.flac", width, track,
fat(artist), fat(title))` with the same en-dash rule; `Disc N/` when the album's tracks
span >1 disc. A file already at its canonical path is not a move. Two files that resolve
to the same target → both to attention (never overwrite). Targets that exist and are not
one of the sources → attention. Moves are computed on a case-insensitive map (Windows) and
a case-only rename is emitted as a two-step move through `<name>.core-tmp`. Non-FLAC files:
`folder.art`, `folder.thm`, `cover.jpg`, `cover.png`, `folder.jpg` move with their album
folder (when the whole folder renames); every other file stays where it is, and a source
folder is removed only when it is empty afterwards. `TracksRecopied` = moves whose
`DeviceName` (per `library` rules, computed before and after) differs — that is every
rename, and it is printed. **Apply:** journal first (`{Version:1, Root, When, Ops:[{From,
To, Size, MTime}]}`, fsynced), then moves in plan order with `os.MkdirAll` for targets,
progress per album, stop on the first error and report the journal path (the partial
journal is still undoable). **Undo:** reverse order; each op's `To` must exist with the
journaled size; skipped ops are listed; empty directories created by Apply are removed.
**CLI.** `core organize --root DIR [--apply] [--undo <journal>] [--json]`; without
`--apply` it prints the plan: per album the renames as `before → after`, the attention
list with reasons, and the last line `N tracks will be re-copied to the iPod on the next
sync`.
**Tests.** Fixture tree built with `flac.BuildFile`: an already-canonical album (no moves);
an album with tags but junk names (`Track 3.flac`, `morgan wallen - tn.flac`); a nested
`Downloads\X\` album; a two-disc album flat in one folder (→ `Disc 1/`, `Disc 2/`); a title
with `" - "` in it; a file missing `title` (attention); two files claiming track 5
(attention, none moved); a case-only rename; sidecars moving with the folder; a `.FLAC`
(untouched, warned). Then **the load-bearing test**: after `Apply`, `library.ScanTree` on
the tree yields for every track `DeviceName == fmt("%02d. %s.flac", track,
FatSafe(tagTitle))`, `NumberFrom == "tag"`, and `cidx.Encode` of the scan decodes back to
the same titles/tracks. Undo restores every path byte-for-byte (compare the tree listing),
and refuses when a `To` was modified. On this box, run the CLI plan (not apply) against a
COPY of `Cameron Dallas` + `ericdoa` + `Morgan Wallen` from MC in a temp dir and paste the
plan in the report.
**Done when:** the fixture round-trip (organize → scan → index) holds and undo restores
the copy of the three MC albums to the byte.

### L3 — `flac.WritePicture`

**Status: DONE 2026-09-15** (`internal/flac/write.go`, `write_test.go`; `build.go` gained
`BuildFileP`, `flac.go`'s `Picture` gained `Width/Height/Depth/Colors`). Two corrections to
what is written below: the chain helper is `metaBlock`, not `rawBlock` (that name is already
a hand-assembly helper in `flac_test.go`), and the in-place test is not "is there a big
enough PADDING block" but "is the whole new chain no longer than the old region" — the space
freed by the picture being REPLACED counts too, which is what puts a real MC file (4 KiB of
padding, a 300 KB cover coming out) on the in-place path. A leftover of 1–3 bytes cannot be
written as a block, so it falls to the rewrite.

**Interface.** `WritePicture(path string, pic Picture) error` (replaces any type-3 picture;
sets width/height/depth from the decoded image — `image.DecodeConfig`), `RemovePictures(path
string) error`, and an `Options.Padding` (default 8 KiB) for the rewrite path. Internals:
`readChain(f) (blocks []rawBlock, audioOff int64)`; `fitInPadding` (the new block needs
`4 + len(body)` bytes; usable when a PADDING block of length P satisfies `P + 4 ≥ needed`
and the remainder `R = P + 4 − needed` is 0 or ≥ 4: write the PICTURE header+body at the
padding's offset, then a PADDING header with `R − 4` and zeroed body when `R ≥ 4`; the
"last" bit moves to whichever block is last; done with `WriteAt` on the open file, no
rewrite); otherwise `rewrite`: temp file in the same directory = `fLaC` + STREAMINFO +
every block except old type-3 pictures and old paddings + the new PICTURE + one PADDING of
`Options.Padding` (marked last) + the audio bytes copied from `audioOff`; fsync; rename over
the original; mtime of the original is NOT preserved (the change must show up to the
sidecar rule). A block over 16 MiB − 1 cannot be encoded → error before touching the file.
**Tests.** Fixtures with and without padding (`flac.BuildFile` gains a `Padding int`
variant, `BuildFileP`); in-place path taken when padding fits and the file size is
unchanged; rewrite path otherwise; audio bytes (`file[audioOff:]`) identical before/after
(sha256); `Read` returns the picture with the right dims; a second `WritePicture` replaces
rather than appends (exactly one type-3 block); `ffprobe` (skip if absent) reports the same
duration and a `png`/`mjpeg` video stream; `ffmpeg -f null` decodes with empty stderr; a
crash simulated between temp write and rename leaves the original intact.
**Done when:** the audio-hash and ffprobe tests pass on both paths.

### L4 — `internal/artfetch` + `core art --fetch`

**Status: DONE 2026-09-15** (`internal/artfetch/{artfetch,score,itunes,musicbrainz,cache}.go`
+ `artfetch_test.go`, `score_test.go`, `testdata/{itunes_search,musicbrainz_release}.json`;
`internal/cli/art_fetch.go`, `art_fetch_test.go`, flags on `art.go`). Both API shapes in §0
were checked with one live request each and are right as written; the corrections and the
choices that differ from the sketch below:

- The client's entry points are `Lookup(ctx, artist, album)` and `Find(ctx, Query)` (the
  same call, two shapes), and `Fetch` returns `([]byte, mime string, error)` rather than an
  `Image` — the two callers that exist want the bytes and the MIME, and nothing else.
  `Candidate` is `{Provider, Artist, Album, ID, ThumbURL, FullURL, Fallbacks, Score, Rank}`:
  `Album` rather than `Title` (it is an album everywhere else in this program) and
  `FullURL`+`Fallbacks` rather than one `ImageURL`, because the 1200 px iTunes rewrite is a
  guess that has to be able to fall back to the 600 and the 100.
- iTunes `artworkUrl100` really is `…/25UMGIM46049.rgb.jpg/100x100bb.jpg` and the 1200 and
  600 rewrites both return 200 (635 KB / 144 KB) — only the numbers are rewritten, since
  the `bb` suffix varies. MusicBrainz's `score` is an integer 0–100, not a float, and it is
  100 for every plausible pressing, so it is kept only as `Rank` (scaled below 0.5) and
  never decides a match. A release search returns one row per pressing — three "One Thing
  at a Time" rows in the fixture — so candidates are de-duplicated by normalised
  artist+album. `coverartarchive.org/release/<mbid>/front-500` answers 200 through **two**
  redirects (archive.org, then a storage node), which is why the redirect budget is 5 and
  not 0; a bad MBID is a 404, as documented.
- `Score` is the plan's full ladder (1.0 / 0.9 after stripping edition words / 0.7
  containment / provider rank scaled to ≤ 0.45), taken at the WEAKER of the two fields, so
  an exact album title on the wrong artist cannot reach `--yes`.
- The CLI is `core art --fetch <folder>|--album|--batch [--write] [--yes] [--dry-run]
  [--min-score]` rather than `--root`: `art` already has `--album`/`--batch` and a third
  spelling of the same idea would be a wart. `--write` is what writes; `--yes` only decides
  whether each album is asked about (below `--min-score` it is skipped even with `--yes`).
- Politeness beyond the plan: an exact iTunes match ends the provider walk (`StopAtScore`),
  so MusicBrainz is asked only about albums iTunes missed; the 1 req/s limiter also covers
  the Cover Art Archive hop, not just the search; a "no match" is remembered for a week
  (`Cache.NegativeTTL`) so re-inspecting a library does not re-ask about the same bootlegs.

**Checked live, outside the tests** (a temp copy of `4TH WALL - Ruel` with its pictures
removed by `metaflac`, never the MC tree): `core art --fetch --album …` printed
`1.00  itunes       Ruel — 4TH WALL`, and `--write --yes` embedded the 1200×1200 cover
(403,129 bytes) into all 14 files, wrote `cover.jpg` and produced a 28,812-byte `folder.art`
and a 1,580-byte `folder.thm`; `ffmpeg -f null` decodes every file with an empty stderr.

**Interface.**
```go
type Query struct{ Artist, Album string }
type Candidate struct{ Provider, Title, Artist, ImageURL, ThumbURL, ID string; Score float64 }
type Provider interface{ Name() string; Search(ctx, Query) ([]Candidate, error) }
type Client struct{ Providers []Provider; HTTP *http.Client; Cache Cache; UserAgent string }
func (c *Client) Find(ctx, q) ([]Candidate, error)     // merged, scored, sorted; provider errors are warnings unless all fail
func (c *Client) Fetch(ctx, cand) (Image, error)      // bytes + MIME + dims, cached by ImageURL sha1
func Score(q Query, cand Candidate) float64           // decision 9
```
`itunes.go` (search + the `1200x1200bb` rewrite with fallbacks), `musicbrainz.go` (search +
CAA `front-500`/`front-1200` with a token-bucket limiter at 1 req/s and the mandatory
User-Agent), `cache.go` (`<UserCacheDir>\core\art\<sha1>.jpg|png` + a `meta.json` index).
A 10 s per-request timeout; 3 retries on 5xx/429 with backoff; never follow more than 5
redirects. **CLI.** `core art --fetch --root DIR [--yes] [--dry-run] [--min-score 0.9]`:
albums under `Root` whose art source has no front cover → query → print the top candidate
per album (`score  provider  title — artist`) → with `--yes` embed those ≥ min-score via
`flac.WritePicture` into EVERY FLAC of the album (the index uses `Pos == 1` but a user
copying one file elsewhere expects its cover), and write `cover.jpg` beside them; without
`--yes` it only prints. **Tests.** `httptest` servers for both providers with recorded
JSON/JPEG fixtures (record them ONCE by hand from a real query for one album, strip to the
fields used, commit them under `testdata/`, ≤ 50 KB total); scoring table; rate limiter
timing; cache hit skips the network; a provider 500 is a warning when the other answers.
No network in tests. Once, outside tests, the implementer may run `core art --fetch
--dry-run` against a temp copy of one MC album with its pictures removed
(`RemovePictures`) and paste the candidate line.
**Done when:** the fixture-driven fetch embeds art that `coreart.WriteAlbum` then turns
into valid sidecars.

### L5 — `internal/librarian`

**Reviewed 2026-09-15 (Fable): accept with one fix.** Verified on copies of three MC albums: clean copy → "Nothing to fix"; one album nested under `Downloads/` and one junk-named → report lists 18 moves (11 tracks + sidecars), `--names --organize --yes` re-foldered 11 with a byte-identical sha256 listing, `--undo` put all 18 back to the pre-fix state. Fix order (covers → moves → sidecars) and the journal stamps taken at move time confirmed in `fix.go`/`organizer.Apply`; art not undone and the CLI says so; a failed fetch is a `Failure`, never an abort; `--dry-run` writes nothing (no journal, tree unchanged). Syncer decision 11: the MC plan test passes and a second dry run against a fake device reports "0 to render, 2 already valid". One live iTunes lookup on a cover-stripped copy of `4TH WALL - Ruel` returned `1.00 itunes Ruel — 4TH WALL [take]`; tests never reach a real host. Fixed: `--dry-run` without `--yes` used to print only the report; it now runs the whole decision pass ("would fix: …") and still writes nothing (`TestFixDryRunAloneShowsWhatYesWouldDo`). Note for L7: `Report.Unorganized` counts sidecar moves too (18 here for 11 tracks); the UI should count tracks (`Reason` not `art`/`playlist`/`cycle`) in its headline.

**Status: DONE 2026-09-15** (`internal/librarian/{librarian,fix}.go` + `librarian_test.go`,
`internal/cli/fix.go` + `fix_test.go`, registered in `root.go`; the decision-11 rule and its
test in `internal/syncer`). Verified against a copy of three MC albums (`24 - Arizona
Zervas`, `DOA - ericdoa` and `4TH WALL - Ruel` nested under `Downloads/`): the report is
`Missing art (0) / Misnamed (0) / Unorganized (17)` — the two albums already in place need
nothing, the nested one moves with its `cover.jpg`, `folder.thm` and its album-local `.m3u` —
and `--yes` then `--undo` leaves the tree listing identical to the byte. Eight corrections
to what is written below (7 added 2026-09-16):

1. **The order inside `Fix` is covers, then moves, then sidecars — and the sketch's "art
   first, then moves" was right for a reason it does not give.** Embedding a cover rewrites
   the FLAC (new size, new mtime, deliberately — decision 11 depends on that mtime moving),
   and organizer's journal records each file's size and mtime AS IT MOVES IT, with `Undo`
   refusing anything that changed since. A cover written after the rename therefore makes
   that rename un-undoable: the whole album comes back from `Undo` as "it changed after the
   move; left alone". `TestUndoPutsTheNamesBackAndLeavesTheArt` is that pin. The two
   sidecars and `cover.jpg` are written LAST, into whatever folder the album ended up in
   (the picture is read back out of the moved art source rather than held in memory: a
   thousand accepted covers is 400 MB of JPEG kept alive for two file writes).
2. **The three steps are three calls, not one.** `Inspect` never touches the network —
   a UI refreshes on it — so the lookup is `Candidates(ctx, report, client, opts)`, which is
   cancellable, and its per-album failures are log lines rather than an error. `Fix` takes
   the decisions.
3. **A `Decision` carries the `Candidate`, not an index into the candidate list.** An index
   drifts the moment anything re-sorts or re-fetches; the URLs cannot. `AcceptAbove(cands,
   min)` is the "accept all ≥ 0.9" rule, and it is what `--yes` builds.
4. **The report carries the plan's ORDER, and a report that came back from JSON cannot be
   applied.** `Misnamed`/`Unorganized` are views of `organizer.Plan.Moves`; the order is what
   stops a rename landing on a file that has not moved out of the way yet, and the
   temporary hop that breaks a rename cycle has to stay with its partner. `Moves` is
   therefore `json:"-"` and `Fix` refuses a decoded report rather than half-applying it.
5. **Un-ticking one category cannot be allowed to fail the other.** A ticked move whose
   target is still held by a file whose own move was NOT ticked would stop
   `organizer.Apply` at that op; it is deferred instead, reported in `Result.Deferred`, and
   cycle groups are kept or dropped whole.
6. **The re-copy cost is two numbers, not one.** `RecopyTracks` (organizer's) counts tracks
   that stay put and still change their device name because a neighbour left the folder, so
   it cannot be turned into bytes; what CAN be is the moved set, split by whether
   `library.ScanTree` can see the file today — `RecopyBytes` (already on the iPod under a
   name about to change) and `NewBytes` (never been there). Against the MC copy that is the
   difference between "0 tracks re-copied (306.8 MB)", which reads as a bug, and "0 tracks
   re-copied (0 B), and 14 … for the first time (306.8 MB)".
7. **The multi-disc split is its own category and its own tick (added 2026-09-16).**
   `Report.DiscSplit` holds the `Reason: "disc"` moves, `Choices.DiscFolders` applies them,
   and neither the `Unorganized` count nor the re-copy line includes them — the report says
   "Multi-disc albums (N files) — off by default, `--disc-folders` to split into Disc N
   folders". `Options.DiscFolders` is what puts those moves in `Report.Moves` (the apply
   order lives there), so `Fix` refuses a `DiscFolders` tick on a report inspected without
   it rather than silently doing nothing to 43 files; `core fix --disc-folders` sets both.
   Against the real MC tree this is the difference between `Unorganized (85)` /
   `87 track(s) re-copied (2.3 GB)` and `Unorganized (33)` / `Multi-disc albums (43 files)` /
   `44 track(s) re-copied (1.4 GB)`.
8. **The CLI is `core fix`, not `core library inspect|fix|undo`.** One noun for the screen
   it mirrors, and the flags are the tick boxes: `--src`, `--names`, `--organize`, `--art`
   (the only flag that goes to the network without `--yes`), `--yes`, `--dry-run`,
   `--min-score`, `--json`, `--undo <journal>`. The `Event` type lives in `librarian` with a
   string `Job` — importing `internal/app` would link Gio into `core.exe` — and its `Kind`,
   `Text` and `Pct` are `app.Event`'s, so L6/L7's adapter is a switch and two copies.

**Interface.**
```go
type Report struct {
    Root string
    MissingArt []AlbumArt      // {Album library.Album; Candidates []artfetch.Candidate; Chosen int}
    Misnamed   []organizer.Move
    Unorganized []organizer.Move
    DiscSplit  []organizer.Move
    Attention  []organizer.Attention
    TracksRecopied int; Bytes int64
}
type Choices struct{ Art map[string]int /* album dir -> candidate index, -1 = skip */; Rename, Organize, DiscFolders bool }
func Inspect(ctx, Options{Root, Art *artfetch.Client, Progress}) (*Report, error)
func Fix(ctx, r *Report, ch Choices, o Options) (*Outcome, error)   // one journal; art first, then moves
func Undo(ctx, journal string) (*organizer.UndoReport, error)
```
`Inspect` = organizer.Scan + (for albums with no front cover) artfetch.Find, progress per
album. `Fix` embeds the chosen art (journaling each FLAC's pre-image? no — art embedding is
NOT undone: the journal records "art written to <files>" for the record, and Undo says so;
reversible-decisions list) then applies the moves. **syncer change (decision 11):** an
`ArtOp` is planned when a sidecar is missing/invalid OR its mtime < the art source's
mtime; test with a touched FLAC. **CLI.** `core library inspect|fix|undo` mirroring the
Fix screen's text. **Tests.** Fixture tree with one coverless album + the fake art server
→ report has one `MissingArt` with candidates; `Fix` with `Chosen` embeds and moves; the
subsequent `syncer.MakePlan` re-renders that album's art and copies the renamed files.
**Done when:** inspect → fix → sync-plan on the fixture does what the Fix screen promises.

### L5b — the iPod's name (≤ 300 lines)

**Status: DONE 2026-09-15** (`internal/disk/label.go`, `label_windows.go`, `label_other.go`,
`label_test.go`; `internal/cli/name.go`, `name_test.go`, one line in `info.go`, registered in
`root.go`; `internal/app` — `Device.Name/Label` + `DisplayName`, `Config.Names`,
`Backend.Rename`, the header's click-to-edit row, `rename_test.go`). Six corrections to what
is written below:

1. The calls live in **`internal/disk`**, not `internal/devicefs`: `disk` is already the
   package that owns `syscall`, `kernel32` (one lazy DLL for the package) and drive letters,
   and `devicefs` deliberately only ever touches files through `os`.
2. `VolumeLabel(root) (string, error)` — no `serialHex`. The FAT serial is not a key
   anything uses; the key the friendly name is filed under is the DISK's serial, so the
   third call is `VolumeDiskSerial(root)`, which asks
   `IOCTL_STORAGE_QUERY_PROPERTY` through a **zero-access handle to `\\.\D:`** and
   therefore answers the same string `FindIPods` reports **without Administrator**.
3. `LegalLabel(friendly) string` — no `changed` bool; the caller compares
   `LegalLabel(n) != n` when it wants to know, which is what the app's preview line does.
4. `SetVolumeLabelW` is handed **NULL**, not `""`, to clear a label (the documented way),
   and it upper-cases on FAT but *not* on NTFS — so the read-back compare is against
   `LegalLabel(...)` exactly, which is already upper-case.
5. `GetVolumeInformationW("D:")` does not mean drive D, it means the current directory ON
   drive D. Every entry point goes through `disk.VolumeRoot`, which adds the trailing
   backslash.
6. `core name` writes `config.json` **directly** (a `map[string]json.RawMessage` round-trip
   that preserves every other key) rather than importing `internal/app`, which would link
   Gio into `core.exe`.

The iPod's name is the FAT32 **volume label** of the music partition — what Windows shows
beside `D:`. Nothing else on the device carries a name the firmware or a host could agree on.

**Facts.** A FAT32 label is at most 11 bytes, stored upper-case (Windows upper-cases what
`SetVolumeLabelW` is given on FAT; the read-back is upper-case). Allowed bytes: `A–Z`,
`0–9`, space, and `! # $ % & ' ( ) - @ ^ _ \` { } ~`; not allowed: `* ? . , ; : / \ | + = < >
[ ] "` and anything non-ASCII (the OEM code page makes it host-dependent, so the app treats
it as disallowed). A label of only spaces or empty = "no label". Windows writes the label
into the root-directory volume entry and the boot sector's `BS_VolLab` field; the app never
writes FAT structures itself. The app runs elevated, so `SetVolumeLabelW` is not refused;
the volume must not be locked (the flasher's `Lock` dismounts `D:` — never rename during a
flash).

**Interface.** `internal/devicefs`: `VolumeLabel(root string) (label, serialHex string,
err error)` (`GetVolumeInformationW`), `SetVolumeLabel(root, label string) error`
(`SetVolumeLabelW`; both via `syscall.NewLazyDLL("kernel32.dll")`, stubs returning
`ErrUnsupported` off Windows), `LegalLabel(friendly string) (label string, changed bool)`
= upper-case ASCII letters kept, digits kept, allowed punctuation kept, everything else
dropped, runs of spaces collapsed, trimmed, cut to 11 bytes; `ValidLabel(label) error`.
`internal/app/config.json` gains `names: {"<disk serial>": "Brandon's iPod"}`.
**Name resolution** (`app.State.Device.Name`, used in the header, the top bar, dialogs,
the log): the friendly name from `config.json` for this serial, else the volume label as
read (shown as-is, upper-case, e.g. `BRANDONS IPO`), else `iPod Video 80 GB`. "Apple iPod"
(the SCSI model string) is never shown as the name again; it stays on the Details tab.
**Rename** = write the friendly name to `config.json` under the serial AND write
`LegalLabel(friendly)` to the volume; if the legal label differs from the friendly name the
UI says `Windows will show it as BRANDONS IPO` before applying, and applies both; the
label is read back and compared. An empty name removes the config entry and clears the
label.
**CLI.** `core name <drive-or-mount> [<name>]` — without a name prints the label, the
serial and the friendly name if any; with a name applies the rule above (prints the legal
label it wrote); `--label-only` skips the config entry. **App.** The name in the header is
a click-to-edit field (Enter applies, Esc cancels), disabled while a job runs.
**Tests.** `LegalLabel` table (`Brandon's iPod` → `BRANDON'S I`, `Musique été` →
`MUSIQUE T`, `***` → `` + error from `ValidLabel`, 11-byte cut on a byte boundary — it is
ASCII by then); the Windows calls behind an interface with a fake for the app/CLI tests;
config round-trip keyed by serial; header text for the three resolution cases; a snapshot
of the edit state. On this box: `core name D:` read-only against the real iPod in disk mode
(paste the output), then a real rename to `Brandon's iPod` and back to the original label,
verified by read-back — only with the user's go-ahead in the session.
**Later, firmware:** the device could read `BS_VolLab` from the boot sector (or the
root-directory volume entry) and show the name in About and on the boot screen; out of
this phase, noted in `STATUS.md` when L5b lands.
**Done when:** the header shows the friendly name after a rename, Windows Explorer shows
the legal label, and `core name D:` reads both back.

### L6 — app: launch, auto-detect, install

**Status: DONE 2026-09-15** (`internal/installer/{installer,installer_test}.go`;
`internal/cli/install.go` reduced to a caller; `internal/app/detect.go`,
`Phase`/`Device.Installed`/`JobInstall` in `state.go`, `Detect`/`Classify`/`Install`
on the Backend, `startInstall` + `confirmWrite` in `actions.go`, the two new panes in
`ui.go`, `LookingState`/`NotInstalledState`, `--no-detect` and two more `--state`
names; `detect_test.go`, `install_test.go`, snapshots). Rendered on Windows at
900×600 and 720×520. Five deviations from what is written below, all deliberate:

1. **`Phase` is derived, not stored**, and there are three of them, not four.
   `State.Phase()` reads `Device.Found` and `Device.Installed.Kind`, so a phase
   cannot disagree with the device it describes. "Working" is not a phase: a job
   runs ON a phase (an install on NotInstalled, a sync on Ready) and `State.Busy()`
   is what the buttons already read; a fourth value would have to be left and
   re-entered and the screen it left would be the one nobody could see. An
   UNCLASSIFIED device (`Kind == ""`) is Ready, not NotInstalled — every canned
   state and test that predates the classification describes an iPod that works.
2. **There is no `DetectResult` and the poll does not classify.** `Backend.Detect`
   is `FindIPods` alone and a change starts the ordinary Refresh job, which is
   where `Backend.Classify` is called (once, by `RealBackend.Refresh`). Two places
   that answer "what is installed" would eventually answer differently.
3. **The poll never touches State.** It publishes one `detectNews` under the
   mutex and the next frame's `drain()` acts on it on the UI goroutine — the rule
   the rest of the package already follows. `StartDetect(ctx, tick)` takes its
   clock, so a test's poll is a send and not a two-second wait, and it looks once
   immediately (an iPod already plugged in at launch should be found now).
4. **`internal/installer` exists after all**, as L1 predicted it might: `Install(ctx,
   Options, Deps)` with `Deps.Inspect` (the device seam), `ResolveImage` (the
   release download, which stays in the CLI because it needs cobra and the cache),
   `Flash`, `Volume` and `ChildArgs func(image string) []string` — a function, not a
   slice, because when the caller named no image there is no path to put in the
   child's command line until step 3 has run. `core install` is now 50 lines of
   wiring and every one of its tests still passes unchanged.
5. **Refresh is gone from the window, not hidden.** With the poll running there is
   nothing for the button to do, and a Refresh a user presses because the screen
   implied it was needed teaches them the app does not notice things by itself.
   The Looking screen has no button at all; `Cancel` stays in the iPod card.

Also: the install job ends by re-reading the device (`Backend.Refresh`) rather than
by believing what it wrote, so the phase only moves to Ready on a classification
that came back off the hardware. `JobInstall.Writes()` is true, so Cancel is not
offered during an install — the flasher does not check the context between the body
write and the directory write.

**Device check (not possible here):** the only iPod on hand runs Core, so the
NotInstalled screen has been seen only as a canned state. What a stock device will
exercise for the first time: the classification reaching `Phase`, the Apple backup
name in the dialog, and the volume step after the relaunch.

**State.** `Phase` enum on `State`: `Looking` (no iPod; the Select+Play hint), `NotInstalled`
(iPod found, `Installed.Kind == Other`: the Install card with the Apple-backup sentence and
one button), `Ready`, `Working`. `detect.go`: a goroutine ticking every 2 s while
`!Busy()`, calling `Backend.Detect(ctx) (DetectResult, error)` (`FindIPods` + `Classify`,
no volume walk), diffing by `Path+Serial`, and posting a `Refresh` job on change (or a
`DeviceGone` event on unplug, which returns the phase to `Looking` and clears the library).
`Backend.Install(ctx, emit)` wraps `installer.Install` with the same typed-confirmation
dialog as flash (the prompt shows "Apple firmware will be backed up to <path>"). The
Install card also offers "Install from a file…". `--no-detect` flag for tests/screenshots.
**Tests.** Fake backend with a scripted sequence (none → Apple iPod → Core iPod → none):
the phases follow; polling pauses during a job; Install runs the fake installer and the
phase becomes `Ready` on success; snapshot tests for `Looking`, `NotInstalled`, `Ready`.
**Done when:** launching the app with the iPod already in disk mode reaches `Ready` with
no click, and unplugging returns it to `Looking`.

### L7 — app: direction B (grid + Library tab)

**Status: DONE 2026-09-16** (`internal/app/{grid,library_tab,details_tab}.go`, the tab bar
and `body` in `ui.go`, `LibraryState` in `demo.go`, `--state library`;
`grid_test.go`, `library_test.go`, `rename_test.go`, `memory_test.go`, snapshots at
900×600 and 720×520). Rendered on Windows at 900×600 (`app_grid.png`, `app_library.png`)
and on Linux headless at both sizes.

**The bug this slice shipped and the guard that now stops it.** The Library tab rendered
without bound: `core-app.exe --screenshot --state library` reached 30 GB and 18 GB on
Windows, and the Linux test binary was OOM-killed at 40 GB, twice taking the session's
machine with it. **Root cause: `internal/app/ui.go`, `NewUI` — `u.grid`, `u.libList` and
`u.prevList` never had `Axis` set.** `widget.List`'s zero value is `layout.Horizontal`, and
a horizontal list lays its children out along an UNBOUNDED main axis (`layout.Inf`, which
Gio defines as 1e6 px). So the Library tab's problem rows and preview rows were laid out a
million pixels wide, and the album grid laid out one row of eight tiles 125,000 px each.
Nothing in the layout complained — the ops list stayed small and every pane still reported
the window size it was given, which is why `TestLayoutAtEverySizeAndState` and the plain
`go test ./internal/app/` passed throughout. It only detonated one layer down, in
`gioui.org/internal/stroke.flattenQuadBezier`, where Gio turns the `widget.Border` around
each row into a stroked path: a rounded rect a million pixels wide flattens into tens of
millions of segments, and `gpu.(*drawOps).buildVerts` asked the allocator for 1.4 GB
blocks. The fix is three lines: `u.grid.Axis = u.libList.Axis = u.prevList.Axis =
layout.Vertical`.

Two guards, in `internal/app/memory_test.go`:

- `TestEveryListScrollsVertically` walks the `UI` struct by reflection, finds every
  `widget.List` field (six today) and fails any whose `Axis` is not `Vertical`. It is the
  guard aimed at the cause, it runs with no GPU, and it catches the seventh pane somebody
  adds next year.
- `TestFrameAllocationIsBounded` renders every canned state at 900×600 and 720×520 through
  the same `Snapshot` the screenshot flag uses and fails if one frame allocates more than
  256 MiB of `runtime.MemStats.TotalAlloc`. Measured after the fix: demo 11.7 MiB, library
  20.6 MiB, details 35.3 MiB, notinstalled 16.3 MiB, empty 8.4 MiB, looking 8.7 MiB.
  `TestLayoutAllocationIsBounded` is the same question for the pure-Go ops recording
  (64 MiB budget; every state is under 270 KiB), so the cheap half runs on a machine with
  no GPU at all.

Also fixed while reviewing the rendered frames: the cost line above **Fix all** printed
`306800000 B (292.6 MiB)` inside its sentence, because `disk.HumanSize` is built for a
facts row and not for prose. `proseSize` in `library_tab.go` rounds once and says it once
(`307 MB`); `disk.HumanSize` is unchanged everywhere else.

Replaces the four cards. **Top bar:** the iPod line (name from the volume label or
"iPod Video 80 GB", `Core vX · up to date | vY available`, `used / total`), the state
sentence (`7 albums to sync · 101 tracks · 2.2 GB`, or `Everything is on the iPod`), the
ONE primary button (`Sync` / `Update` / `Install` / `Eject` by state — decision from the
design page: A's discipline), `Eject` secondary when Sync is primary. **Tabs:** Albums
(the grid), Library (the Fix screen), Playlists (list only, phase 3), Details (the old
log + doctor lines). **Grid:** `internal/thumbs` cache; tiles = albums from
`library.ScanTree(Source)` joined with the last `syncer.MakePlan` (dry run on every refresh
of the source): state chip `new` (album folder absent on the device), `changed` (any copy
op), none; dimmed when the source has no cover (a plain tile with the album name).
Clicking a tile shows the track list and the device state. **Library tab:** the three
problem rows with counts + the preview pane (`before → after` table; candidate art
thumbnails with accept/skip), `Fix all` (= `librarian.Fix` with the ticked choices; a
confirmation names the re-copy cost), `Undo` (enabled while the last journal is
un-archived). **Source folder:** chosen once, remembered in `config.json`; the Browse
button stays. Progress: the top bar's 2 px accent line (existing). Minimum window stays
720×520; the grid reflows columns from width. **Tests.** State/actions with the fake
backend (tile states from a fake plan; Fix all calls Fix with the chosen candidates; Undo
enabled/disabled); snapshots at 900×600 and 720×520 for `Ready` with 12 tiles and for the
Library tab; the demo state uses the same 928-song numbers.
**Device check:** on the real iPod in disk mode: launch → `Ready` without a click; Albums
shows 101 tiles, none `new`; Library shows the real counts for MC (expect: some misnamed
— MC files are `NN - Artist - Title` so most are canonical; report what it finds and DO
NOT apply anything to the real MC tree during the check — apply only against a copy).
**Done when:** the app opens to the grid with the iPod detected, and a `Fix all` on a copy
of MC followed by a Sync to the iPod plays on the device with the same library count.

---

> **Review 2026-09-15 (Fable) of L1–L4 + L5b:** all five accepted. Fixed in review: (1) `internal/app`
> `TestFlashDialogShowsThePlanAndDemandsTheDevicePath` hung the whole package for ten minutes — its
> dialog-answering goroutine was a 2000-iteration busy loop that missed the request once the job did
> a little more work before asking; it now waits on a deadline like `answerDialogs`; (2) an
> album-local `.m3u`/`.m3u8` now moves with its album (organizer sidecar rule; the MC-copy run had
> left `DOA - ericdoa.m3u` behind). Verified: L1 on both real dumps (Apple → other, pre-marker → core-old;
> `flash`/`install` over an Apple-shaped device write EntryOffset 0), the flasher's single `PlanWrite`
> call passes `CoreImage`, the FAT-side step runs only after a verified non-relaunched write; L2 on a copy of
> three MC albums (clean → empty plan; junked → 12 files back to byte-identical names; undo restores),
> journal created + fsynced before the first rename, undo refuses modified targets, symlinks skipped,
> quote fold is a subset of NormKey's, folder = artist source for both organizer and scanner; L3 in-place
> writes confined to [4, audioOff) with equal-length regions, ffprobe tests pass; L4 offline tests, limiter
> covers the CAA hop, `--yes` ≥ 0.9, art source written last; L5b NULL clears the label, read-back compared,
> rename serialised by the Runner. Known, not fixed: a crash between the two hops of a case-only rename
> leaves `<name>.core-tmp` for a human (undo refuses it, correctly); `core name` and the app both write
> config.json with temp+rename, so a simultaneous write is last-writer-wins.

**Reviewed 2026-09-16 (Fable): L6 accept, L7 accept with fixes.** Verified in code: Install Core
→ typed device path dialog → `installer.Install` once with `Install: true`, refusing anything
already Core, Apple backup named `apple-<serial>-<date>.bin`; the FAT-side files only after a
verified, non-relaunched, non-aborted write; Fix all → `librarian.Fix` once with exactly the
ticked choices, the re-copy line directly above the button, Undo only with a journal; the
primary-button table (Install / Sync / Update / Eject) decided in one place; every button
disabled while a job runs; the poll pauses during jobs and publishes under the mutex only. Grid:
`material.List` lays out visible rows only (1000 albums = 474 KiB per layout, 40 thumbnail loads,
new guard `TestLayoutScalesToAThousandAlbums`), decodes on 4 workers, one ImageOp per album.
Fixed: the hint under the Library buttons was clipped at the column's bottom edge (dropped; the
Fix dialog carries the undo rule, now worded honestly — covers written into files stay), and the
cost line + Fix all/Undo/Rescan are pinned under the scrolling problem list so they stay on screen
at the 720×520 minimum instead of sitting below the fold; a
rescan (Rescan, or ticking the multi-disc row) no longer throws away the art lookup and the
Take/Skip decisions for albums still coverless (`keepArt`); a nil guard on `res.Flash` in the
install job. Note: the detector's Busy check and its raw open are not atomic — a job starting in
between overlaps one read-only MBR read with the flasher's open (shared-read handle; harmless).

## 4. Reviewer checklist (per slice)

- L1: the planned row on the Apple-shaped fixture has entry 0 and `Addr/Version/LoadAddr2`
  unchanged; `--from-backup` never applies the policy; the Apple backup name is used only
  for `Other`; `VerifyWritten` still compares the whole row.
- L2: nothing outside `Root` is touched; non-FLAC files stay except the listed sidecars;
  never overwrite; journal written and fsynced BEFORE the first move; undo verifies size;
  the round-trip test (organize → `ScanTree` → `pos == track`) exists and passes.
- L3: audio bytes hashed before/after in every test; the in-place path leaves the file
  size unchanged; a rewrite goes through temp + fsync + rename.
- L4: no network in tests; User-Agent set; MusicBrainz ≤ 1 req/s; every image written only
  under the user's own files/cache.
- L5b: never rename while a volume is locked; the label written is `LegalLabel` of what the user typed and is read back; the friendly name lives only in config.json.
- L5–L7: one job at a time; `Fix all` cannot run without a preview having been shown; Undo
  refuses a modified target; the primary button's label always names what it will do.

## 5. Decisions the user may want to reverse

- **Names and folders follow the tags in place** (no managed copy of the library, no
  re-tagging). A "copy into a Core Library folder and never touch my files" mode is the
  alternative, at the cost of doubling the disk.
- **Source convention stays `Album - Artist` / `NN - Artist - Title.flac`**, because the
  device locator is derived from it. Changing it means changing `library` and the Python
  oracle together.
- **Renamed tracks are re-copied to the iPod** (the locator is the filename). The
  alternative — a device-side rename — needs a FAT writer the firmware and the app do not
  have.
- **Art embedding is not undone** by the journal (the pre-image is not kept); "remove
  pictures" is available as a manual action.
- **iTunes first, MusicBrainz second**; both are external services with their own terms.
- **Polling, not device notifications**, for detection.
- **Direction B** for the window; L6 is direction-independent, L7 is not.
