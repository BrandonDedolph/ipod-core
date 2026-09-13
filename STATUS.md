# Status — picking up where we left off

The README is the canonical public story; this doc is the running list of
what works, what doesn't, and what to pick up next.

## 2026-09-13 — issue sweep landed on `main`, NOT yet flashed

Six read-only audits over the whole tree, then six fix branches merged
(28 commits, 53 host suites green, ARM `-Werror` + `verify-hw` clean). The
two user-reported problems turned out to be one chain: a PLAY-hold "off"
was suspend-to-RAM, which never escalated to PMU standby and drained the
cell; the resulting cold boot then rebuilt the queue as the album.

What changed (see `git log 054c722..`):
- **Power-down** — PLAY is arbitrated by press length (`ui/keyhold.c`: tap
  = pause on release, hold = sleep; the down-edge no longer pauses).
  Suspend now: FLUSH + STANDBY + **SLEEP** on the drive (reset-to-wake),
  `lcd_sleep()` behind `SUSPEND_PANEL_SLEEP` (default 1), 100 ms loop
  period, battery sampled on the 5 s cadence with the DISKSAFE/SHUTOFF
  policy live, and **escalation to PMU standby after
  `SUSPEND_TO_STANDBY_US` (30 min) off external power**. `enter_standby()`
  quiesces codec/drive/panel first and RETURNS -1 on a refused PMU command
  (repaint + carry on) instead of spinning bright. The `lcd_wake()` absorb
  window now lands after the panel-init update, not before.
- **Resume keeps the real queue** — the settings record carries queue kind
  (album / songs / artist / genre / shuffle-songs), cursor and two shuffle
  seeds (payload 24 → 44 B, version unchanged, old records still load).
  Shuffle deals are seed-reproducible; the Shuffle Songs double-deal is gone.
  Album remains the fallback.
- **Player** — the pump parked the drive on EVERY pass (hundreds of STANDBY
  commands a second, and the one-shot spin-up probe stayed latched so track
  opens ran without retries); the elapsed clock and seek anchor wrapped at
  71 min; a failed seek now restarts from 0; skips while paused no longer
  kick the DAC (no click); a dropped DMA completion no longer replays a
  buffer on resume.
- **Config/FS** — a slot read error at boot no longer regresses the seq and
  silently discards the session's settings; forced saves wake a parked
  drive first and stay dirty until a reported success (`kernel/cfg_commit.c`);
  a CORELIB.IDX read error is refused instead of reported as "Library too
  large"; an orphaned LFN fragment no longer eats the next file's name;
  `build_index.py` no longer turns "7 rings" into track 7.
- **HAL** — a NACKed battery ADC read is a failed read, not a 3300 mV cell
  (was a false SHUTOFF path); SHUTOFF cannot fire on external power; I2C
  waits for the bus before its one-time reset; ATA ready waits are timed
  (10 s / 31 s after SRST, spin-up 8 s); backlight ISR flags volatile.
- **UI** — low-battery modal survives a plug-in/unplug and the toast is
  timed from first paint; pending SELECT dropped on screen change / Hold;
  modals replace rather than overflow the screen stack; rail-pinned sliders
  don't write; lock plate no longer busy-spins; stale strip gauge repaints.

**First-flash checklist (all of the above is unverified on the device):**
1. Suspend → wake: panel comes back from `LCD_SLEEP` (white → set
   `SUSPEND_PANEL_SLEEP 0`); drive comes back from SLEEP via SRST (watch the
   UART for the spin-up wait); measure suspend draw before/after.
2. Hold PLAY 5 s → PMU standby; also let a suspend run past 30 min off the
   charger and confirm it powers down.
3. Resume into a Songs / Artist / Shuffle Songs context, press Next, check
   the order; confirm no click on a paused skip.
4. Battery line still reads sane values (the 2800–4600 mV plausibility band
   must not reject a real cell).
5. PLAY tap vs hold feel; a tap must never sleep, a hold must never pause.

Still open from the audit: `DEV_EN` peripheral gating and PLL-off in
suspend (moot once escalation lands), the BCM power gate, `PANEL_SLEEP_AT_IDLE`
(still 0), a tiebreak for same-hash files in one folder, `flac_meta.c`
stripping non-ASCII on the scan fallback, and `ata_identify()`'s ERR check.
(The reserved playlist queue kind has since been wired — see **Playlists**
under "What works".) Nothing has been pushed.

## Where we are right now (2026-07-28)

**A full music player on real hardware, and it is now the device's own
firmware.** `core` is written into the firmware partition as the OSOS
image: the Apple boot ROM hands straight to `crt0.S`, which does the MMAP0
SDRAM remap itself. There is no chainloader, no boot menu, and no Apple
firmware left on the device — `ipodpatcher -wf` replaces the whole OSOS.
Recovery is unchanged and unconditional (see below).

The whole bare-metal stack is proven end to end on an actual iPod 5.5G
80GB — boot ROM → `crt0.S` + MMAP0 remap → clock/PLL → timer/IRQ → LCD
(BCM present) → I²C/WM8758/I²S → DMA → ATA PIO → FAT32 — and on top of it
a real player: **streaming FLAC off the iPod's own disk** (read-ahead over
an 8 MB anti-skip buffer, not preload, so full-length tracks play), a
host-built library index (`CORELIB.IDX`) for instant Songs / Albums /
Artists / Genres, **settings that persist to disk**, **resume-on-boot**,
and the full Linen/Onyx UI. On-screen framebuffer console plus the new
Settings → Boot Details page are the cable-free debug channel; there is NO
serial cable (confirm hw state on screen instead).

**★ THE ROUTE BACK TO DISK MODE IS THE ONE THING NOTHING MAY COMPROMISE.**
Hold **Select + Play** at power-on. That lives in the boot ROM and runs
before any image loads, so nothing we flash can remove or pre-empt it —
proven on this device under the worst case (our firmware as the sole OSOS,
Apple's gone, black screen). Technique: let it go fully off, unplug, hold
Select + Play *first*, then plug USB in while still holding. The firmware
also offers a Disk Mode row under Settings, but that is convenience; the
ROM combo is the floor.

**Flashing changed with direct boot.** Copying `core.ipod` to the FAT32
data partition now does **nothing** — nothing loads it. The procedure is
`ipodpatcher <disk> -wf build-hw/core.ipod` with raw block-device access
(sudo on native Linux; an elevated shell on Windows, and pass `< NUL` or
ipodpatcher blocks on stdin), then **always verify**: `ipodpatcher <disk>
-rfb readback.bin` and `cmp` it against `core/build-hw/core.bin`.
Byte-identical or don't boot it. Back up the partition first (`-r`);
restoring it (`-w`) puts Apple's firmware back.

**Dev-environment note:** the clean flash environment is **native Linux**
(the iPod is a real `/dev/sdX`). The WSL path works too but goes through
Windows interop — `/mnt/d` writes silently do not persist (drvfs cache),
so file copies must be Windows-native + `Write-VolumeCache`. Toolchain on
Arch is all official `extra`: `pacman -S arm-none-eabi-gcc
arm-none-eabi-binutils arm-none-eabi-newlib meson ninja pkgconf`, then
`make hw` / `make ipod` / `make sim` from `core/`.

## What works on the device today

- **Direct boot** — our image is the OSOS; `crt0.S` sets up an IRAM stack,
  runs the remap stub from IRAM, copies `.data`, zeroes `.bss`, brings up
  cache, wakes the COP, and enters `kernel_main`. The backlight is lit
  *before* the LCD probe on purpose, so "backlight on" means our code ran.
- **Bring-up** — clock/PLL (30 MHz, refcounted 80 MHz boost), IRQ + 100 Hz
  timer, unified-cache management, real fault handlers + panic screen.
- **Display** — BCM framebuffer present path, including `bcm_init()` so a
  wedged BCM is no longer terminal (we can no longer assume a chainloader
  left it idle at frame one); on-screen console; the full RGB565 UI with
  damage-tracked partial presents.
- **Audio** — WM8758B bring-up over I²C, I²S transport, DMA-driven
  continuous playback fed by an SPSC PCM ring drained by the
  DMA-completion ISR.
- **Storage** — PIO ATA reader (aligned bulk reads straight into the caller
  buffer) + from-scratch read-only FAT32 (long names decoded to UTF-8),
  every chain walk bounded and every cluster validated.
- **Streaming decode** — `dr_flac` freestanding, fed by a read-ahead disk
  source over an 8 MB anti-skip buffer; a full-length track streams off the
  disk while the UI stays live. **FLAC only.** `dr_mp3` is built and linked
  and passes its host KAT, but MP3 is switched off on the device
  (`CORE_ENABLE_MP3 0`, `core/kernel/main.c:728`) and `classify_ext()` does
  not surface `.mp3` at all, so those files are invisible in the browser.
  Its float synthesis filter cannot hit real time on this FPU-less CPU —
  the PCM ring starves and playback stutters. Parked, not removed;
  re-enabling needs a fixed-point or second-core decoder.
- **FLAC seeking is O(log n)** — `DR_FLAC_NO_CRC` had silently compiled out
  dr_flac's binary-search seek (the search needs the header CRC to tell a
  real frame header from audio that looks like a sync code), and these
  files carry no SEEKTABLE, so every seek decoded from the start of the
  track. That was 5.1 s of an 8.4 s cold boot. Costs ~14 KB of text and
  some decode margin, which Boot Details now reports rather than assumes.
- **Library** — host-built `CORELIB.IDX` loads in one read → instant Songs /
  Albums / Artists / Genres; caps are 6000 songs / 1024 albums / 512
  artists / 128 genres, and hitting one sets a truncation warning rather
  than silently dropping tracks. Sorts are O(n log n). Records carry UTF-8
  fields + a normalized-name hash that binds each to its file independent
  of quote/case style (falls back to a per-file tag scan if the index is
  absent).
- **Settings persistence** — `CORECFG.DAT`, pre-allocated by
  `tools/make_config.py`, two alternating 1024 B slots (one whole *physical*
  sector each; this drive rejects sub-physical-sector access with IDNF),
  each record CRC-32 checked over its entire body including the header. The
  device never creates, grows, moves or deletes the file — it overwrites
  the bytes of the file's own first cluster, so zero FS metadata changes.
  The target LBA is re-resolved and re-validated through `fat32_file_lba()`
  before *every* write. `config_save()` is the only call to
  `ata_write_sectors()` in the firmware. **Proven on hardware 2026-07-27**;
  slot alternation confirmed from a raw disk dump, and `chkdsk` found no
  problems afterwards.
- **Resume on boot** — comes back on the track you left, **paused**, at the
  saved position, **in the queue you were playing it in**: Songs, an
  artist's songs, a genre, the same Shuffle Songs draw, or a playlist (the
  record's once-reserved context word now holds the playlist's name hash,
  `RESUME_KIND_PLAYLIST`), with the same shuffle order (Next is still the
  track that was coming next). Bound by
  the folded `name_hash()` of the filename (not an index or a cluster),
  cross-checked against duration ±2 s and required to be unique otherwise,
  so it survives a library rebuild. The queue is rebuilt from a kind byte
  plus two seeds (the record grew 24 → 44 bytes under the same version;
  old records still load and fall back to the album). Positions under 10 s
  aren't seeked. Any doubt at any step falls back to the track's album,
  and past that leaves the device exactly as if nothing had been saved.
  Parity between the C and host implementations is gated by
  `check_resume_parity.py` in `make verify-hw`. Not yet flashed.
- **Boot Details** (Settings → Boot Details) — a live per-phase breakdown of
  the last cold boot: LCD/BCM, disk + mount, library, resume (split into
  dir / open / seek), and OTHER derived as total-minus-named so unmeasured
  time shows up instead of vanishing. TOTAL is measured independently, not
  summed. Plus FLAC decode cost as a % of the 22676 µs/kframe real-time
  budget, the underrun count, and the CFG seq + slot LBAs. **Cold boot
  measured 8.4 s, of which 5.1 s was one FLAC seek** (since fixed). The
  post-fix total has NOT been read off the device — Boot Details shows it
  live; take the number from there rather than quoting arithmetic.
- **Power management** — CPU scaled to 30 MHz and halted at idle; the HDD
  spun down after 20 s idle, including while paused (and the parked flag
  now survives a settings write, so it re-parks afterwards); the codec
  powered down and the audio clocks gated at stop; the lock/unlock plate
  repainted on every Hold edge. **The power button** (`ui/keyhold.c`,
  host-tested): PLAY decided by press length — a tap toggles pause on
  release, a 2 s hold suspends, 5 s escalates to PMU standby; a
  hold-to-sleep no longer pauses first, so wake resumes. **Suspend**
  (`suspend_to_ram`) now actually powers things down: drive to ATA SLEEP
  (flush, standby, `0xE6`; a reset wakes it), panel to `LCD_SLEEP`
  (`SUSPEND_PANEL_SLEEP 1`, rollback documented beside it), clock
  unboosted, codec off via the pump; it samples the battery on the 5 s
  cadence and runs the DISKSAFE/SHUTOFF policy while asleep, escalates to
  real standby after `SUSPEND_TO_STANDBY_US` (30 min) on battery, and on
  wake does `lcd_wake` -> present -> backlight (the present retires the
  panel's wake-init before returning — `bcm_frame_commit` used to absorb
  BEFORE the update, i.e. never) and skips the resume if the jack is
  known empty. **Standby** (`enter_standby`) quiesces first (codec,
  settings, drive, panel) and is no longer `_Noreturn`: a PMU that refuses
  the write gets a relit, repainted, running device instead of a dark
  dead one. **UNVERIFIED ON THE DEVICE, all of it** — none of this has
  been flashed: whether the panel comes back from `LCD_SLEEP` (if it wakes
  white, `SUSPEND_PANEL_SLEEP 0`), the drive's post-SLEEP reset wake, the
  suspend draw before/after, and the refused-standby recovery. **LCD panel
  sleep at idle stays DISABLED** (`PANEL_SLEEP_AT_IDLE 0`) — two of its
  three preconditions are now genuinely met (the post-wake absorb order,
  the deferred relight); `bcm_init()` exists but is compiled out. Let the
  suspend path prove panel sleep on the device first.
- **Charging** — `charger_set_max_current(500)` asserts HPWR so the LTC4066
  uses the 500 mA input cap instead of the 100 mA one we had been
  inheriting from the boot ROM. **UNVERIFIED ON HARDWARE** — needs an
  inline USB current meter. (Also out of USB spec without enumeration,
  which we can't do; safe on wall chargers and essentially all root ports.)
- **Full UTF-8 names** — atlas covers Latin-1 + smart punctuation, the text
  renderer decodes UTF-8, and display sources tag text so FAT-illegal
  characters (`?,*,:,/`) show correctly.
- **Text rendering** — the pen is 26.6 fixed point and carries the
  fractional advance across a whole string instead of rounding per glyph;
  each of the six shipped faces (Nunito regular 9/11/13, bold 11/13/17)
  carries its own kerning table (1,742–3,657 pairs — we previously applied
  none) and its own tracking value, solved from measured ink-to-ink
  daylight rather than guessed. Two host tools back this:
  `tools/text_preview.c` links `core/ui/text.c` and the shipped atlases
  unmodified and renders the real stack to a PPM, and
  `tools/text_metrics.py` reproduces the device's pen arithmetic against
  the baked alpha bitmaps and reports per-pair spacing (`--target` solves a
  tracking value). *Type still doesn't read right at 9–13 px; the remaining
  lever is the face, not the spacing — Nunito ships no TrueType hinting.*
- **Browsing UI** — main menu, Music submenu, Artists / Albums / Songs /
  Genres, an artist's **All Songs** (whole discography in one list, album on
  the sub-line), album detail (hero art + tracklist with per-disc sections +
  durations), scrolling marquee for long titles, two-line rows with 28 px
  album-art chips, letter-stepping on a sustained fast spin (Songs only).
- **Playlists — read path** (2026-09-13, **not yet flashed**) — Music →
  Playlists (and the main menu's Playlists row) lists
  `Music/Playlists/*.m3u8` (`.m3u` too) by filename, A–Z, re-read on every
  entry. Open one and it is a tracklist in the Songs shape (tag title /
  artist / duration for tracks the index knows, the filename for ones it
  doesn't); SELECT plays the whole playlist from that row, each track with
  its own album's cover. Entries may be volume-root-absolute
  (`/Music/Artist - Album/01 Song.flac`) or relative to `Music/Playlists/`
  (`../Artist - Album/01 Song.flac`); `\` separators and a drive letter are
  tolerated. A missing, non-audio or unreadable entry is skipped and
  counted, never fatal, and an empty result says which nothing it is.
  Caps: **64 playlists, 128 tracks per playlist** (the first that many;
  the overflow is reported, not silent). Rows bind to the library by the
  same full-name locator hash the album tracklist uses. Lives in
  `core/library/playlist.c` over `fs/m3u.c` + the new
  `fat32_resolve_path()`, host-tested end to end on an in-RAM volume with
  real VFAT long names. On the no-`Music/` layout the folder is
  `Playlists/` at the root. Writing playlists is still item 1 below.
- **Now Playing** — 120×120 cover, title/artist/album, TRACK N OF M,
  elapsed / −remaining, a rounded progress bar, shuffle/repeat tokens,
  battery, and wheel seek.
- **Gapless hand-over** — at decoder EOS the next track is opened over the
  same arena, disk buffer and ring while several seconds of the old track
  are still queued; when the two share a sample rate the DAC is never
  stopped. Presentation (title/clock/art) is deferred until playback
  crosses the boundary. **Not yet confirmed by ear on the device.**
- **Overlays** — volume (skinny-wave speaker icon + fill bar + %),
  lock/unlock padlock modals, charging screen ("CHARGED" when done), boot
  splash. Anti-aliased modal/progress corners.
- **Settings** — nine rows: Playback (shuffle / repeat / **resume**), Sound
  (volume / bass / treble / balance via the WM8758 EQ), Theme (Linen ↔ Onyx
  live palette swap), Display (backlight timeout + brightness), Clicker
  (7 piezo profiles + Off), About (dashboard: song/album/artist counts,
  storage, battery), Boot Details, Disk Mode, Reset. The list scrolls and
  has a scrollbar. Placeholder rows that did nothing were removed.
- **Battery gauge** — proportional fill, turns red at ≤20%.

## What's NOT done (pick up next)

1. **Playlists — write path.** Reading is done (above, unflashed). Saving
   or editing a playlist means creating and growing a file, which means
   **FAT32 cluster allocation**, which does not exist. The only write we
   have is an in-place overwrite of one pre-allocated file's first cluster
   (`config.c`). The read path was a day; the write path is a filesystem
   project.
2. **Search** — not implemented.
3. **A screen-tuned font face.** Advances, kerning and tracking are all
   fixed and measured, and the type still reads wrong at 9–13 px. Nunito
   ships no hinting bytecode, so the next lever is swapping the face for
   one designed for small sizes — not more spacing tuning.
4. **Flash and measure the power work** — the suspend/standby changes
   above are entirely device-unverified; the first flash should check the
   panel wakes from `LCD_SLEEP`, the drive wakes from SLEEP, and read the
   suspend draw. Then **re-enable panel sleep at idle** — see above; the
   blockers it was disabled for have since been fixed, and the suspend path
   exercises the same wake sequence.
5. **Podcasts / Audiobooks / Composers** — greyed placeholders in the
   menus; no backing implementation.
6. **More codecs** — AAC / ALAC / Vorbis / Opus / WAV are stubbed in
   `codecs/README.md`; only FLAC is wired (MP3 builds but is disabled).
7. **On-device screenshot capture** — `lcd_screenshot_bmp()` is declared in
   `hal.h` and implemented only in the sim HAL; nothing calls it. The
   README shots come from `docs/screens/render.py`, which is a **standalone
   Python/PIL reimplementation** of the UI (real Nunito faces, real
   palette, real layout — but not a single pixel from firmware code). It
   can drift from the device silently. A device capture path would end that.
8. **Library sync is manual** — build the index on the host
   (`tools/build_index.py`), convert art (`tools/coreart.py`), pre-create
   `CORECFG.DAT` (`tools/make_config.py`), and copy to the device.
9. **Host CLI install/flash/recover are stubs** —
    `core/cli/internal/cli/install.go` says so outright; flashing is
    `ipodpatcher` by hand today.

## Testing

`meson test -C build-sim` from `core/` (**54/54** green):

- **Codec KAT** — FLAC + MP3 decoders bit-exact against reference PCM.
- **MMIO golden traces** — each freestanding hw driver is host-compiled against a
  recording mock bus (`-DMMIO_MOCK`) and asserted to emit its exact ordered
  register grammar (I²C, WM8758 bring-up, I²S, DMA, LCD present, BCM init,
  UART, clock, timer, battery, volume, backlight, and the PMU standby write).
- **Audio ping-pong** — the buffer state machine in `hal/hw/audio.c`: buffers
  alternate, no audio is repeated or dropped across DMA completions, a short
  source read zero-pads exactly the tail.
- **Player queue** — `core/player/player.c`'s queue, auto-advance, repeat,
  shuffle, next/prev, pause/resume, the gapless hand-over and the incremental
  queue builder, with the real player compiled against fake disk/codec/DAC.
- **Library locator hash** — golden vectors asserted from BOTH the C side and
  `tools/build_index.py`, plus a diff proving the testable copy still matches
  `kernel/main.c`. The hash is the only thing binding an index record to its
  file; if the implementations drift the track silently stops resolving.
- **Resume** — the matcher's name/duration/uniqueness rules, plus a parity
  check against the host side.
- **Settings persistence** — the config record layout, CRC, slot alternation,
  LBA resolution and the ATA write bus grammar.
- **FAT32** — the happy path, path resolution (`fat32_resolve_path`: every
  separator/case form, every refusal, EIO vs ENOENT), plus corrupt images:
  cyclic FAT chains, out-of-range clusters, a FAT16 boot sector, orphaned
  LFN runs, a truncated volume.
- **M3U8** — the reader against the playlists that actually break parsers.
- **Playlists** — `library/playlist.c` end to end on an in-RAM volume with
  real VFAT long names: the listing, every entry shape, every failure as a
  count, both caps, an unreadable album folder vs an unreadable playlist.
- **readahead / diskbuf / scheduler / console / clickwheel / pcm-ring / text /
  thumb / FLAC metadata / MP3 tags** — unit tests.

Some of those assert the *documented* contract rather than current behaviour and
are marked as expected failures where the fix belongs to another file. Run
`CORE_TEST_STRICT_XFAIL=1 meson test -C build-sim` to turn every such marker
into a hard failure and see what is still outstanding.

Note the suite only exists in a **sim** configure (`if target == 'sim'`); a hw
build registers zero tests. Seven of the 54 need `python3` (the generated
FAT32 images and the four parity/consistency scripts) — without it you get
47.

**Read this before trusting a green suite.** For a long stretch this section
said "25/25 green" and that number came from *stale gcc-14 binaries* — `meson
test --no-rebuild` happily runs whatever is already in the build directory. A
real rebuild failed: gcc had rolled 14 → 16 underneath the tree, gcc 15 had
added `-Wunterminated-string-initialization`, and the `-Werror` hw build no
longer compiled. `.github/workflows/ci.yml` now builds everything from scratch
on every push for exactly this reason, on both a pinned toolchain and the
current rolling one, and runs the suite again under ASan + UBSan.

Also gated by `make verify-hw` (and in CI) — five static checks against the
linked ARM image:

- `tests/scripts/check_hw_layout.sh` — crt0 / linker layout asserts.
- `tests/scripts/check_size.sh` — size budget; prints text/data/bss and fails
  past the documented ceiling.
- `tests/scripts/check_hw_consistency.py` — `hal/hw/pp5022.h` addresses vs
  `docs/hw/`.
- `tests/scripts/check_name_hash_parity.py` — the host tool's name hash
  against the golden vectors the C side (`library/names.c`) is tested on.
- `tests/scripts/check_resume_parity.py` — the resume matcher across C and host.

Current image: **~298 KB text, 260 B data, ~11.45 MB bss** (budgets 1 MB /
64 KB / 14 MB, image ceiling 30 MB inside the 32 MB SDRAM window). bss is at
81% of its ceiling — there is ~2.5 MB of headroom, not the "room for another
MB-scale buffer" the script's inline comment still claims. The FLAC CRC tables
and binary search are ~14 KB of the text growth.

The device build (`make hw` / `make ipod`) is the authoritative compile for the
firmware; the clang lints about `hw/pp5022.h` / `LCD_WIDTH` / `mmio_*` are
include-path noise from editor tooling, not build errors.

## Known stale/incorrect notes elsewhere in the tree

Found while writing this doc. Worth fixing when you next touch these files
(they are not this doc's to edit):

- `core/cli/internal/firmware/checksum.go:7` still says the partition checksum
  is `sum(bytes) + ModelNum`; `08-boot-dock.md:64` corrects that from a real
  device (no `MODEL_NUM` seed). Line 21 also still calls ipodloader2 "the
  shipping path".
- `core/docs/hw/08-boot-dock.md` still describes preserving Apple's
  `entryOffset` so we can chain back to it, and calls disk mode
  bootloader-mediated in one place and ROM-level in another. The ROM answer is
  the proven one.
- `core/docs/design/settings-persistence.md:5` says the write path is "not yet
  verified on hardware" — it was, on 2026-07-27.
- `core/fs/fat32.c:96` says the ATA write primitive "is not wired to anything
  yet" — `kernel/config.c` uses it.
- `core/player/player.c:64` comments "Sizing (4 MB)" next to an 8 MB define,
  and derives ~37 s of audio from it (the define's own comment says ~73 s).
- `core/tests/scripts/check_size.sh:17-24`'s inline "currently ~" figures are a
  release behind (226 KB → 298 KB text, 10.1 MB → 11.45 MB bss).
- `core/ui/atlas/nunito_bold_9.h` is generated by `tools/atlas_gen.sh` and
  committed, but no source includes it and `atlas.h` doesn't declare it —
  ~171 KB of dead source.
- Fixed while this was being written: `core/boot/crt0.S`, `core/hal/hw/power.h`
  (both said a bootloader hands off to us) and `tools/README.md` (now documents
  `text_preview.c` and `text_metrics.py`).

## Repository

GitHub: https://github.com/BrandonDedolph/ipod-core.

**`main` is NOT necessarily current.** This line used to claim it was; work
lands on local branches first and several commits have sat unpushed at a time,
so treat the remote as a floor, not the truth. Check before assuming:

```bash
git log --oneline origin/main..HEAD    # commits you have that the remote doesn't
git status -sb                          # ahead/behind for the current branch
```

See the README for the overview, `core/README.md` for the firmware build, and
`PLAN.md` for the roadmap.
