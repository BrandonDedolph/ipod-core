# `core` — host CLI and `core-app` — desktop app

Two executables from one Go module, running the whole everyday-user
loop: put music on the iPod in the layout the firmware expects, bake the
album-art sidecars, write the index, flash firmware, fetch updates.

- **`core`** is the CLI. Console subsystem, cobra, every command below.
- **`core-app`** is the desktop window (Gio v0.8.0). It finds the iPod by
  itself — a 2-second poll, so plugging one in is the whole gesture — and
  stops for exactly one thing: an iPod still running Apple's firmware,
  which gets a single screen and an **Install Core** button. Once Core is
  on it: the album grid, a Library tab that says what is wrong with the
  music folder and offers to fix it, and a Details tab holding the
  device facts, Firmware (check / update / flash / backup) and the log.
  It links the same internal
  packages directly — no server, no IPC. On Windows its embedded manifest asks for Administrator at launch
  (one UAC prompt), so it reads the disk and flashes in-process; if it is
  somehow running unelevated it runs the `core` CLI beside it under UAC.

Users download `core-{os}-{arch}` and `core-app-{os}-{arch}` from GitHub
Releases and put them in the same folder.

*Why two executables and not `core ui`:* an exe linked with
`-H windowsgui` has no console at all, so `core sync` typed in a
terminal would print nothing; an exe linked without it flashes a black
console every time the app starts. Two files avoid both.

## Build

```bash
cd core/cli
go build -o core ./cmd/core
./core --help
```

The CLI is pure Go, so one machine cross-compiles every target:

```bash
CGO_ENABLED=0 GOOS=darwin  GOARCH=amd64 go build -o core-darwin-amd64  ./cmd/core
CGO_ENABLED=0 GOOS=darwin  GOARCH=arm64 go build -o core-darwin-arm64  ./cmd/core
CGO_ENABLED=0 GOOS=linux   GOARCH=amd64 go build -o core-linux-amd64   ./cmd/core
CGO_ENABLED=0 GOOS=linux   GOARCH=arm64 go build -o core-linux-arm64   ./cmd/core
CGO_ENABLED=0 GOOS=windows GOARCH=amd64 go build -o core-windows.exe   ./cmd/core
```

### Building `core-app`

Gio's **Windows** backend is pure Go, so the exe everyone actually
downloads cross-builds from anywhere. `-H windowsgui` is what stops a
console flashing up on launch:

```bash
CGO_ENABLED=0 GOOS=windows GOARCH=amd64 \
  go build -ldflags "-H windowsgui" -o core-app.exe ./cmd/core-app
```

Gio's **X11/Wayland** and **Cocoa** backends are cgo, so those builds
happen on their own platform:

```bash
# Linux (needs libwayland-dev libx11-dev libx11-xcb-dev libxkbcommon-dev
#        libxkbcommon-x11-dev libgles2-mesa-dev libegl1-mesa-dev
#        libffi-dev libxcursor-dev)
go build -tags novulkan -o core-app ./cmd/core-app

# macOS
CGO_ENABLED=1 go build -o core-app ./cmd/core-app
```

`core/cli/scripts/build-all.sh` does all of this: it builds the five
`core` binaries, `core-app-windows-amd64.exe`, and whichever `core-app`
builds the host can make natively — then **prints the ones it skipped**.
Nobody has three machines, and a release job that silently published
seven of eleven assets would be worse than one that names the four it
could not build. `.github/workflows/release.yml` runs it on
`ubuntu-latest`, `ubuntu-24.04-arm` and `macos-latest` for that reason.

#### `-tags novulkan`, and why a plain `go build ./...` still works here

Gio compiles a Vulkan path on Linux, which needs `vulkan/vulkan.h`. That
header is not installed on every machine that can run the app perfectly
well through OpenGL ES (this project's WSL box among them), and a C
compiler error is not a useful thing for a Go build to fail with.

So the two files that import `gioui.org/app` and `gioui.org/gpu/headless`
— `internal/app/run.go` and `internal/app/snapshot.go` — carry
`//go:build !linux || novulkan`, with stubs returning `ErrNoGPU` behind
`//go:build linux && !novulkan`. The consequences:

- `go build ./...`, `go vet ./...` and `go test ./...` work on any Linux
  machine, with or without the Vulkan headers. The model, the job
  runner, the backend wiring and every layout function are compiled and
  tested either way — Gio records drawing into an `op.Ops` list, and
  nothing is rasterised until a frame is submitted.
- `go build -tags novulkan ./...` additionally compiles the window and
  the headless renderer, and `go test -tags novulkan ./...` runs
  `TestSnapshot`, which renders the window to PNGs. Without the tag that
  test **skips**; it never fails for want of a GPU.
- Windows and macOS builds never see any of this: their backends do not
  import the Vulkan headers, so the real files are always in.

CI runs both, so the tagged build is not a configuration only one laptop
ever compiles.

### Rendering the window without a display

```bash
go run -tags novulkan ./cmd/core-app --screenshot out.png --state demo
go run -tags novulkan ./cmd/core-app --screenshot out.png --state library
go run -tags novulkan ./cmd/core-app --screenshot out.png --state empty --size 720x520
go run -tags novulkan ./cmd/core-app --screenshot out.png --state looking
go run -tags novulkan ./cmd/core-app --screenshot out.png --state notinstalled
```

One frame, headless, then exit. `--state demo` is a canned State with
the bench device, the 928-track library and a sync half-finished, open on
the album grid; `--state library` is the Library tab with something in
every problem row; `--state empty` is the first run; `--state looking` is
the window with no iPod and the poll running, and `--state notinstalled`
is a stock iPod on the Install screen. `TestSnapshot`,
`TestSnapshotLaunchPhases` and `TestSnapshotTheTabs` use the same code
path and write to `$CORE_SNAPSHOT_DIR` (default:
`$TMPDIR/core-app-snapshots`).

Rendering is also a memory assertion. `TestFrameAllocationIsBounded`
renders every canned state at both sizes and fails if one frame allocates
more than 256 MiB; `TestEveryListScrollsVertically` checks the thing that
blew that budget once (see the `core-app` row below).

`core-app --no-detect` turns the poll off: the window then knows only
what a job told it, which is what a screenshot or a test wants and
nothing else does.

`core-app --log FILE` appends startup diagnostics to a file. On Windows
that is the only way to find out why a window did not appear: a
`-H windowsgui` process has no console, so its stderr goes nowhere.

## Test

```bash
cd core/cli
go test ./...
go vet ./...
gofmt -l .        # must print nothing; CI enforces this

# and again with the GUI files in (see -tags novulkan above):
go build -tags novulkan ./...
go vet   -tags novulkan ./...
go test  -tags novulkan ./...     # this run includes TestSnapshot
```

### Parity env vars

Several planned tests check Go output against the Python tools in
`tools/`, the C headers, or real data. They **skip** — never fail — when
the thing they need is absent, and they never touch the network.

| Variable | What it points at | Used by |
|---|---|---|
| `CORE_REPO` | repo root, for tests that grep C sources (caps in `core/kernel/main.c`, `settings_defaults()` in `core/ui/settings.c`, the art sizes in `core/ui/artcache.h`). Default: walk up from the test file. | S2, S3, S4a |
| `CORE_PARITY_SRC` | the source music tree (`…/Music/MC`) | S1, S2, S3, S4b |
| `CORE_PARITY_IDX` | an oracle `CORELIB.IDX` built by `tools/build_index.py` | S2 |
| `CORE_FWPART_DUMP` | a firmware-partition dump (`ipodpatcher -r`), or several separated by `:` | S5 |
| `CORE_SNAPSHOT_DIR` | where `TestSnapshot` writes the rendered window PNGs. Default: `$TMPDIR/core-app-snapshots` — a stable path, not `t.TempDir()`, because the point of the files is that somebody looks at them after the run. | S10 |

Partition dumps and music are never committed.

## Layout

```
cmd/core/         the CLI main (just calls cli.Root().Execute())
cmd/core-app/     the desktop app main: flags, --screenshot, app.Main
internal/
  cli/            cobra command tree (one file per subcommand)
  app/            core-app: State (the model), Runner (one job at a
                  time, events over a channel), Backend (an interface
                  with the real wiring and a fake), the Gio layout, the
                  Linen palette, and the headless snapshot renderer.
                  State is mutated only by the UI goroutine; jobs say
                  what happened by sending Events
  doctor/         the read-only device + volume checks, shared by
                  `core doctor` and core-app's Device card, so the two
                  cannot answer the same question differently
  eject/          flush + dismount + eject, per OS, shared by
                  `core eject` and core-app's Eject button
  firmware/       .ipod transport format (checksum, codec) + the
                  firmware-partition directory entry — scaffolding, see
                  the file comment in imageheader.go
  fwpart/         partition model: preamble, directory, OSOS entry,
                  capacity, write set, read-back verify. Reads and plans
                  only — nothing here writes. doc.go carries the
                  write-path SAFETY CHECKLIST.
  flasher/        the write sequence: parse the image, gate the
                  hardware, plan, print, confirm, elevate, back up,
                  write, verify. Every OS call is a field of Deps, so
                  the whole thing runs against an in-memory device
  disk/           the ONLY package that opens a raw block device:
                  enumerate (Windows IOCTLs / Linux sysfs / macOS
                  diskutil), open with aligned bounce-buffer I/O,
                  lock/dismount, elevation, and iPod identification
  organizer/      the source tree's names from its tags: Scan (a plan of
                  moves plus the files a human has to decide about), Apply
                  (journal first, fsynced, then the renames) and Undo (the
                  journal backwards, verifying size and mtime). Writes only
                  under the root it was given
  librarian/      the "things to fix" list and the one job that fixes it:
                  Inspect (one organizer.Scan plus a pass for front covers ->
                  Report), Candidates (the network step, on its own so it can
                  be cancelled), Fix (covers into the files, then the moves,
                  then the sidecars — one journal) and Undo
  installer/      the install sequence with no command line in it:
                  classify -> refuse a device already running Core ->
                  Apple's firmware to apple-<serial>-<date>.bin -> the
                  flasher -> CORECFG.DAT, CORELOG.BIN and Music\ on the
                  volume -> read the device back. `core install` and
                  core-app's Install button are both callers
  ghrelease/      GitHub Releases: latest / by tag, the firmware asset,
                  and a checksum-verified download cache under
                  <user cache dir>/core/<tag>/. stdlib net/http only
  artfetch/       cover-art lookup: the iTunes Search API, MusicBrainz +
                  the Cover Art Archive (mandatory User-Agent, 1 req/s,
                  injectable clock), the match score --yes acts on, and
                  the image + negative cache under
                  <user cache dir>/core/art. stdlib net/http only
  version/        build version info, stamped via -ldflags at release time
```

## Status

| Command | State |
|---|---|
| `firmware pack` | **works** — wraps a raw image in the `.ipod` format. `core/Makefile`'s `ipod` target runs it on every build, so this is the one command a broken change actually breaks. Validates its input, writes atomically, verifies by read-back. |
| `firmware unpack` | **works** — extracts and checksum-verifies an image. `--ignore-checksum` for recovery. |
| `build hw` / `build sim` | **works** — thin wrapper over `make -C core <target>`. |
| `info` | **works** (S6) — identifies the connected iPod and describes it: disk/model/serial/size, the logical sector size (from the OS, cross-checked against where the Apple preamble actually is), the partition table, the image directory with a fresh checksum per image, and the OSOS length/capacity/checksum, and a `firmware:` line naming the installed build from the `CORE-FW-VERSION:` marker in the OSOS body (S8; images before v0.1.3 carry none and report "unknown"). `--json` for machines, `--all-disks` to list every disk the OS can see. Read-only. |
| `index` | **works** (S2) — build `CORELIB.IDX` in Go, byte-identical to `tools/build_index.py` |
| `art` | **works** (S3) — `folder.art` (120×120) and `folder.thm` (28×28) CART sidecars, from a FLAC's embedded cover, one album or a whole tree. |
| `art --fetch` | **works** (L4) — finds cover art for albums that have none: the iTunes Search API first, then MusicBrainz + the Cover Art Archive (mandatory User-Agent, one request a second, the Cover Art Archive hop included). Candidates are printed with a match score — 1.00 artist and album equal, 0.90 equal once `(Deluxe)`/`- Single`/`[Remastered]` are stripped, 0.70 containment, under 0.50 a name mismatch — and the score is taken at the WEAKER of the two fields, so the right album by the wrong artist is never accepted. Nothing is written without `--write`, which asks per album unless `--yes`, and `--yes` accepts only at or above `--min-score` (0.9). An accepted cover is validated (JPEG or PNG, at least 300 px, at most 10 MB), embedded in EVERY FLAC of the album through `flac.WritePicture`, written beside them as `cover.jpg` and baked into the two sidecars. `--dry-run` decides and writes nothing. Images are cached under `<user cache dir>/core/art` (and a "no match" is remembered for a week); they belong to their rights-holders and are never written anywhere but your files and that cache. |
| `organize` | **works** (L2) — the source tree's names, made to say what the tags say: `Album - Artist/[Disc N/]NN - Artist - Title.flac`, which is the convention the device locator is derived from (`library.ScanTree` reads `NN. Title.flac` off the byte-sorted folder listing, so zero-padded track numbers put `pos` on the track number and `TrackTitle` hands the tag title back). Nothing is guessed: a file missing album, artist, title or a usable track number is listed under "needs attention" and left alone, an album whose artist tags cannot agree is a question, two files claiming one track number are both left, and a name already held by a different file is never overwritten. A difference only a curly apostrophe deep is NOT a rename — the locator hash folds it, so the rename would be a whole-album re-copy for nothing. `folder.art`/`folder.thm`/`cover.*` follow their album when the folder empties; every other file stays. The preview is the default and its last line is the cost in tracks the next sync will copy again. `--apply` fsyncs a full undo journal to `<user config dir>/core/journal/` BEFORE the first rename and stops at the first failure with that journal intact; `--undo <journal>` replays it backwards, refusing any file whose size or mtime changed since; `--list-journals` finds them. |
| `fix` | **works** (L5) — one command for everything wrong with a library, and the one job that fixes it. The report (the default; it writes nothing and asks no service anything) lists albums whose first track carries no cover, files misnamed inside the right folder, files that belong in another folder, and the files nobody can name from their tags — plus the cost: renamed tracks are re-copied on the next sync because the filename IS the device's locator, and the two byte totals are counted apart (already on the iPod vs never been there). `--art` is the flag that goes to the network: it prints the ranked candidates with the score `--yes` would act on. `--yes` does it — as ONE job with ONE undo journal: the covers go into every track of their album FIRST (embedding rewrites the file, and the journal records each file's size and mtime as it moves it, so a cover written after the rename would make that rename un-undoable), then the moves, then `folder.art` + `folder.thm` + `cover.jpg` in whatever folder the album ended up in. A cover that fails to download is one line in the summary, never a reason to abandon the renames. `--names`/`--organize`/`--art` narrow the job; `--dry-run` decides everything and writes nothing; `--json` is what the app reads; `--undo <journal>` puts the names back and says plainly that it does not remove embedded art. |
| `sync` | **works** (S4b) — source tree → device layout, art sidecars, playlists rewritten to device paths, `CORECFG.DAT`/`CORELOG.BIN` created only when absent or invalid, `CORELIB.IDX` written LAST. Plans first: `--dry-run` prints the whole plan with byte totals and writes nothing. Skips a track whose size matches and whose time is within 2 s (`--verify` hashes instead). Re-renders an album's sidecars when they are missing, invalid OR older than the file the cover is read from, so embedding art with `core fix` reaches the iPod on the next sync with no flag (`--art-refresh` is still the force). Deletes nothing without `--prune --yes`. Refuses a drvfs/9p destination. |
| `eject` | **works** (S4b) — flush, then dismount + eject. Windows: lock/dismount/allow-removal/eject through the volume handle, with Explorer's Shell verb as the fallback. Linux: `udisksctl unmount` + `power-off` (prints the commands when udisksctl is absent). macOS: `diskutil eject`. |
| `name` | **works** (L5b) — the iPod's name. `core name D:` prints the FAT volume label as it is stored, the disk serial and the friendly name core-app remembers for that serial; `core name D: "Brandon's iPod"` writes both halves — the label becomes `disk.LegalLabel(name)` (upper-cased, ASCII, at most the 11 bytes a FAT volume entry holds, so `Brandon's iPod` is stored as `BRANDON'S I` and the command says so), and the name itself goes into `<user config dir>/core/config.json` under `names`, keyed by the disk serial. The label is read back and compared; an empty name clears both; `--label-only` writes the volume and nothing else. Needs the same rights as any volume operation, and never during a flash — the flasher dismounts the drive letter. |
| `firmware inspect` | **works** — tells a partition dump, a `.ipod` file and a raw image apart and describes it: the image directory like `ipodpatcher -l` with a fresh checksum per entry, or the `.ipod` header, or the sum a raw image would need. Read-only. |
| `backup` | **works** (S6) — dumps the whole firmware partition (preamble, directory, every image) to a file, temp + fsync + rename, then re-parses the file it wrote to prove the backup is a backup. Never overwrites. Default path `<user config dir>/core/backups/fwpart-<bytes>-<timestamp>.bin`. Same bytes as `ipodpatcher -r`. |
| `firmware read` | **works** (S6) — writes exactly the OSOS entry's `Length` bytes (no padding to the 0x800 boundary), which is `ipodpatcher -rfb` byte for byte. |
| `flash` | **works** (S7) — replaces `ipodpatcher -wf`. Writes the OSOS body (zero-padded to 0x800) and the one directory sector holding its row, in that order; everything else on the partition is untouched. The row's entry point is set to 0 (this image starts at its first byte; Apple's starts 7.5 MB in, and a row left saying so is a device that boots to nothing) and `addr`/`vers`/`loadAddr2` are kept exactly as the device had them. Backs the WHOLE partition up first (temp + fsync + rename, then re-parsed off disk), locks the device, flushes, and re-opens it read-only to compare the read-back. `--dry-run` prints the plan and opens nothing for writing; without `--yes` you type the device path to confirm. `--from-backup <fwpart.bin>` is the recovery path: whole-partition restore, refused unless the file's preamble, directory and OSOS checksum all validate and its size matches the device. |
| `install` | **works** (L1) — one command for an iPod that is still running Apple's firmware: classifies the OSOS row and body (`fwpart.Classify`: a `CORE-FW-VERSION` marker means Core, no marker with entry point 0 and a body under a megabyte means a Core from before v0.1.3, anything else — Apple's `entry 0x736000` / 7.6 MB — means Core is not installed), refuses a device that is already ours with `core update` as the thing to run instead (`--force` overrides), downloads the latest release when no file is given, then runs the `flash` sequence unchanged. The whole-partition backup is named `apple-<serial>-<date>.bin` when what it holds is not Core, because that file is the only copy of that device's Apple firmware in existence — it is never pruned, and the install's last lines name it and the `core flash --from-backup` that puts it back. Afterwards it creates on the music volume what the firmware can overwrite but never create: `CORECFG.DAT`, `CORELOG.BIN` and an empty `Music\`. On Windows the elevated child is another `install` of the already-downloaded file, never a release tag. The sequence itself is `internal/installer` (L6), so the app's Install button runs the same one; what is left in the command is the flags, the download and the exit code. |
| `update` | **works** (S8) — compares the version marker in the installed image with the latest GitHub release, prints the release notes, downloads `core-<tag>.ipod` (or `core.ipod`) into `<user cache dir>/core/<tag>/`, verifies its `.ipod` transport checksum, and then runs the `flash` sequence unchanged. `--check` stops after the comparison and downloads nothing; `--tag vX.Y.Z` pins a release and is also how the installed version gets written again. On Windows the elevated child is re-invoked as `flash <cached file> --yes --no-relaunch`, never as `update`, so the process with Administrator rights does no network I/O. There is no signature checking: the checksum is an integrity check. |
| `doctor` | **works** (S8) — read-only walk printing one OK/WARN/FAIL line per check: device, tested hardware, the image directory, the OSOS checksum, the installed version, then the FAT volume — `Music/`, `CORELIB.IDX` (header, CRC, record count and the song/album/genre caps past which the device silently drops), `CORECFG.DAT`, `CORELOG.BIN` (header AND the block count against the file length, which is `evlog_mount()`'s own test) and the playlist folder. Exits 1 if any check FAILs. `--volume PATH` checks a volume with no iPod attached; the device lines become SKIPs. |
| `core-app` (separate executable, `cmd/core-app`) | **works** (S10, L5b, L6, L7) — native desktop window, Gio v0.8.0, minimum 720×520, with three launch phases and no Refresh button: **Looking** (no iPod — the disk-mode instructions and a poll of `disk.FindIPods` every 2 seconds, paused while a job runs, which also catches the unplug and empties the screen), **Core is not installed** (an iPod whose OSOS row classifies as something else — Apple's firmware — on one screen: what was found, the sentence saying Apple's firmware is copied to `apple-<serial>-<date>.bin` before a byte is written because it is the only copy there is, and **Install Core** / **Install from a file…**, which run `internal/installer` behind the same typed-device-path dialog as a flash), and **Ready**, which since L7 is direction B: a top bar (the iPod's NAME — the friendly name, else the volume label as FAT stores it, else "iPod Video 80 GB", never the "Apple iPod" SCSI string — click it to rename, which writes the volume label and remembers the name under the disk's serial; the installed/available version, the size, the state sentence and ONE primary button whose label is the decision: Install / Sync / Update / Eject), three tabs and a footer. **Albums** is the grid: one tile per album, the cover from the device's own `folder.art` sidecar when there is one and a coloured plate with the album's initial when there is not, a NEW/CHANGED chip from the last dry-run plan, columns reflowed from the window width. **Library** is the Fix screen: five problem rows with counts and tick boxes on the left, the before → after preview (and the cover-art candidates with Take/Skip) on the right, the re-copy cost in the sentence directly above **Fix all**, and **Undo**. **Details** holds what the four cards used to: device facts, **Firmware** (installed vs latest, Check, Update, Flash file…, Backup), the music folder with Browse… and Dry run / Sync + prune / Eject, and the log with Copy. One job at a time: a second click while something runs is refused, and every button but Cancel goes grey. `Sync + prune` is two dialogs with a dry run between them — the second lists the actual orphans and wants the word `prune` typed. The flash and install dialogs show the flasher's own plan text and want the device path typed exactly, like the CLI; on Windows the app's manifest (`cmd/core-app/winres/winres.json`, compiled into `rsrc_windows_*.syso`) requests Administrator at launch, so the window itself reads the raw disk and does the write; only an unelevated window falls back to running `core.exe` elevated through `disk.RelaunchElevatedExe`, and with no `core` beside it the Firmware card prints the command instead. `--screenshot`/`--state`/`--size` render one frame headless; `--no-detect` turns the poll off; `--log FILE` is how a window that never opened explains itself. **The L7 panes scroll vertically, and that is asserted.** `widget.List`'s zero value is a HORIZONTAL list, and the three lists L7 added (`grid`, `libList`, `prevList`) were built without setting `Axis`, so they handed their children `layout.Inf` — 1e6 px — on the X axis. The layout still reported the right window size, so every dimension test passed; a `widget.Border` a million pixels wide only exploded one layer down, in Gio's stroke flattener, where it became tens of millions of segments and gigabytes of allocation. `core-app.exe --screenshot --state library` reached 30 GB and the test binary was OOM-killed at 40 GB. The fix is three lines in `NewUI`; the guards are `TestEveryListScrollsVertically` (reflection over every `widget.List` on the UI struct — a new pane cannot forget it) and `TestFrameAllocationIsBounded` (256 MiB per rendered frame; the Library tab now costs 21 MiB). |

Commands are registered only once they have code behind them, so
`core --help` never lists something that would return "not yet
implemented". There are no stubs left: `info` was the last one.

**Flashing goes through `core flash` as of S7**, and `ipodpatcher`
stays documented as the fallback for two more releases (`ipodpatcher <n>
-wf core.ipod`, read back with `-rfb` and compared — which is also how
the first `core flash` on real hardware is independently confirmed).
`doflash.cmd` stays in the bring-up folder. Our firmware is
written **as the OSOS image itself** and boots directly from the
firmware partition: there is no chainloader to install and no bootloader
stage to place, so the write is one image with the Apple preamble, the
partition table and every other directory entry untouched.

**Device detection is `internal/disk`, not USB descriptors.** The old
`internal/ipod` package (a Generation/Mode/Device type system with a
`detect()` that returned an error on every platform) is gone. An iPod is
now identified from the disk itself, three independent checks, all
required: MBR partition 0 is type 0x00 with a real extent and partition
1 is FAT32 (0x0B/0x0C); `fwpart.CheckPreamble` passes on partition 0's
first 512 bytes (the Apple banner plus the `]ih[` marker at 0x100); and
the vendor/product strings the OS already has say Apple or iPod. Reading
the real USB VID/PID portably would mean libusb, which means cgo, and
the inquiry strings name the block device we are about to open, which a
descriptor does not.

The **logical sector size is read from the OS and then cross-checked**:
partition 0's start LBA times that size must land on the preamble, and
if it does not, 512 and 2048 are tried and `core info` says which one
matched. Every byte offset in the flash path is an LBA times this
number, so the USB bridge reporting 2048 where the driver says 512 is
the one way to be 96 KB wrong while every other number looks right.

Only the **5.5G 80 GB** is `Tested` (disk size within 74–80 GiB; the
real device is 80,026,361,856 B = 74.53 GiB, which is why the lower
bound is 74 and not 76). An untested iPod can still be read, inspected
and backed up; `core flash` refuses it without `--untested-hardware`,
and says so in a sentence that names the one model that has booted this
firmware.

**Raw disk access needs privilege.** Enumeration does not: on Windows a
zero-access handle to `\\.\PhysicalDriveN` answers the metadata
IOCTLs, so `core info --all-disks` prints the full list from an ordinary
console. Reading sector 0 does. When the open is refused, the error
prints the exact elevated command line — a `sudo …` line on Linux/macOS,
a `Start-Process … -Verb RunAs` line on Windows, where UAC cannot
elevate a running console.

**There is no signing or verification code.** There is no release key
and no embedded public key. `core update` verifies the `.ipod`
transport checksum, which is an integrity check and not a signature; any
flag that implies signature verification stays out of the help text
until the code exists. `GITHUB_TOKEN` is honoured if it is set, only to
get out of the unauthenticated rate limit — the releases being read are
public.

**The write path has a safety checklist, and the code follows it.**
The checklist lives in `internal/fwpart/doc.go`: prove the target is an
iPod three ways, back up the firmware partition first, verify by
read-back, and support `--dry-run` plus a typed confirmation.
`internal/fwpart` (S5) is the read and planning half — `PlanWrite`
returns the two byte ranges (the zero-padded body at `devOffset +
0x800`, then the one sector holding the re-encoded directory entry) and
`VerifyWritten` proves they landed. `internal/flasher` (S7) performs
them, in a fixed order with no flag that skips the backup or the
read-back, and every OS call it makes is a field of `Deps` so the whole
sequence — including a write that fails between the body and the
directory — is exercised against `disk.NewMemDevice`. The global
`--device` flag disambiguates when several iPods are connected.

**Elevation and the relaunch loop guard.** A write asks for elevation
whether or not the read worked (`disk.DecideElevation`): on Windows a
non-elevated handle can sometimes read a physical drive while every
write to it fails, and finding that out halfway through a flash is the
failure this exists to prevent. On Windows `core flash` starts an
elevated copy of itself with `--yes --no-relaunch --elevated-log <file>`
and prints the child's log, exiting with the child's code; the child
always carries `--no-relaunch`, so a child that somehow believes it is
not elevated refuses with the command line instead of spawning another.
The typed confirmation happens in the parent, where there is a console
to type into. On Linux/macOS there is no relaunch at all — `sudo`
re-runs in place, and the exact line is printed.

## Design notes

- **Cobra** for the command tree. We considered urfave/cli too — cobra
  won on better help-text rendering and richer flag types.
- **Minimal deps, no cgo.** cobra + pflag + mousetrap (cobra's
  transitive Windows-mode dep) is the whole dependency list today. The
  only addition is `golang.org/x/image` (Lanczos scaling for the art
  sidecars). `golang.org/x/sys` was budgeted for the raw-disk ioctls in
  S6 and turned out not to be needed: the stdlib `syscall` package has
  `DeviceIoControl`, `FlushFileBuffers`, `GetTokenInformation` and
  `EscapeArg` on Windows and `SYS_IOCTL` on Linux, and `ShellExecuteExW`
  comes from a `syscall.NewLazyDLL`. Anything more goes through a code
  review. No cgo means every target still cross-builds from one machine.
- **The Python tools stay.** `tools/build_index.py`, `coreart.py`,
  `make_config.py` and `make_log.py` remain as reference implementations
  and parity oracles for the Go code that replaces them in the daily
  flow.
- **Output files are written atomically.** `writeFileAtomic` in
  `internal/cli/firmware.go` writes to a temp file, fsyncs, verifies by
  read-back, and renames. `--force` must never destroy the previous
  artifact before the replacement is complete on disk — `make ipod`
  passes `--force` on every build, and a truncated `core.ipod` still
  parses as a valid header.
