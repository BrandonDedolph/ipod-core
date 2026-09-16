# Status — picking up where we left off

The README is the canonical public story; this doc is the running list of
what works, what doesn't, and what to pick up next.

## 2026-09-16 — MP3 is on, and UNFLASHED

`dr_mp3` is gone and AOSP's fixed-point **pvmp3** is vendored in its place
(`core/codecs/pvmp3/`, Apache-2.0, pinned commit, `vendor.sh` reproduces the
tree byte for byte). `CORE_ENABLE_MP3` is deleted with it: `.mp3` is a real
format now, in the browser, in Songs / Genres / Shuffle, in the index, in
`core sync`, and resume seeks one.

Why it could move: dr_mp3's float synthesis needed 219 M ARMv4T instructions
per second of audio, 24x FLAC, on a CPU with no FPU. pvmp3 is integer
throughout and needs 15.7 M at 128 kbps — about **1.7x FLAC**. It is also the
only permissive fixed-point Layer III decoder that exists; everything else is
float, GPL/LGPL, RPSL or a binary blob. The survey, the measurements and the
method are in `core/docs/design/mp3-playback.md`.

What is done, on the host:
- MPEG-1/2/2.5 Layer III, CBR and VBR. 8/11.025/12/16 kHz are refused at open
  (`DECODER_ERR_UNSUPPORTED`) — the WM8758B cannot be clocked there.
- ID3v2.2/2.3/2.4 tags into the same `flac_meta_t` FLAC fills, with REAL
  UTF-16 → UTF-8 (the old reader dropped the high byte and put `?` through the
  marquee of every Windows-tagged file), TCON's numeric genres, and the
  duration from Xing/Info/VBRI less the LAME delay and padding — ffprobe's
  rule, so the progress bar agrees with the index.
- Seeking through the Xing TOC: one frame plus one percent of the file, a
  couple of reads, so **resume cues an MP3 at its saved position** instead of
  at 0:00.
- Boot Details' DECODE % now divides by the PLAYING stream's rate instead of a
  hard-coded 44.1 kHz (it under-reported a 48 kHz track by 8 %).
- Seven host suites cover it, all green: `codec-kat` (re-pinned to pvmp3),
  `mp3-frame`, `id3-meta`, `mp3-accuracy` (vs ffmpeg, PSNR 101.7 dB / 1 LSB
  against a 96 dB / 2 LSB gate), `mp3-seek` (landing measured in the audio by
  correlation), `mp3-robust` (truncated, corrupted, junk-spliced, wrong layer,
  unclockable rate, lying ID3 size), `mp3-freestanding` (arena high-water
  28 360 B of the 128 KB budget). Five of them are new and `tag-mp3` is gone
  with dr_mp3, so the tree is at **70 suites**, green under the sanitizers
  too. `make hw && make verify-hw` clean; text 382 496 → 399 516 B of 1 MB.
  Of that +16.6 KB the decoder itself is +3.4 KB (36.4 KB against dr_mp3's
  33.0), and the rest is the framing / Xing / ID3 / seek layer pvmp3 has no
  notion of — 11.5 KB, including 2.8 KB of ID3v1 genre table. bss grows 520 B
  (12 216 384 → 12 216 904): `scan_file_t.fmt` across BROWSE_MAX and the
  decode rate in `player_stats_t`. The decoder's own ~28 KB comes out of the
  128 KB arena, which does not move.

**NOT FLASHED, and the one thing the host cannot answer is real time.** The
bound is 24–39 MHz of the 80 MHz core at 128 kbps, the same shape of budget
FLAC lives in, but that is arithmetic. Next bench, in order:

1. read FLAC's DECODE % on Boot Details FIRST, as the baseline — MP3 should
   land near 1.7x it;
2. 128 kbps CBR, 320 kbps CBR, a VBR file, a 22.05 kHz mono podcast;
3. underruns over a ten-minute album;
4. resume into an MP3; a scrub on a VBR file — and TIME the scrub. A seek
   backs off a fixed 6276 bytes and decodes every frame from there to the
   landing, which is six frame decodes at 320 kbps and about fifteen at 128.
   The constant is deliberate (nothing about the file can be trusted to size
   it — see `codecs/pvmp3/mp3.c`); if `g_boot_res_seek_ms` or a scrub reads
   slow, the tighter version reads the header AT the landing first and scales
   by that frame's size;
5. a FLAC → MP3 → FLAC hand-over, which must not stop the DAC;
6. **time-to-first-sound on an MP3 against a FLAC.** An MP3 open reads the
   file's TAIL twice — `id3_meta_read` through the metadata read-ahead, then
   `mp3_open_stream` through the disk buffer — where a FLAC open seeks to the
   end without reading. Each is a cluster walk plus a 32 KB block over PIO.
   The no-index library scan pays it once per file too. Nothing here is
   measurable on the host; if it shows, the fix is to hand the scan's result
   to the open instead of repeating it.

Over 80 %: IRAM the polyphase/IMDCT hot loops (96 KB free, `boot/linker.ld`),
then the parked COP. Not: shipping the low rates to dodge it.

Also unflashed from this change: `tools/build_index.py` and `core sync` admit
`.mp3` and keep the source extension (nothing transcodes), `folder.art` can
come from an ID3 APIC frame, and `core/cli/internal/id3` computes the duration
by ffprobe's rule with a parity test against it.

**One hole in the byte-identical-index guarantee, and it is only one.** An MP3
with no Xing or VBRI header states no length, so `build_index.py` (through
ffprobe) and `core sync` both ESTIMATE it — and the two estimates are the same
answer only when the file really is constant bitrate. For an untagged VBR file
they differ, sometimes by seconds, and so do the two indexes. `lame` writes a
Xing header unless told not to (`-t`), so such a file is rare rather than
impossible; the parity test names and skips those and ASSERTS every other file,
including untagged CBR. Said again in `core index --help` and in
`core/docs/design/mp3-playback.md`.

`internal/librarian` — the desktop app's organizer — still walks `*.flac` only;
that belongs with the library-manager work. `core art --fetch` is FLAC-only for
a different reason: it EMBEDS the cover it finds and there is no ID3 writer
here. Reading an MP3's existing cover works on every other art path.

## 2026-09-16 — Music › Search, UNFLASHED

The one thing this device could not do: find a track whose album you cannot
remember. Music grows a seventh active row, **Search**.

**How it works.** The wheel drives a 39-cell ring — A–Z, 0–9, SPACE, DEL,
DONE — one cell a detent, wrapping, with fifteen cells on screen centred on
the cursor. Select types; RIGHT is the space bar (it never jumps to Now
Playing while the picker is up, which is the one place the global RIGHT rule
is suspended) and LEFT is backspace, so the two dead keys on a list screen
become the two keys a text field needs. Every keystroke rescans and the top
four hits appear under the ring as you type; DONE hands the wheel to the full
list, MENU comes back to the ring with the query intact, MENU again leaves.

**The ranking is the feature.** A match anywhere in a name counts, but names
that START with the query come first, and within a rank the order is
few-to-many: artists, albums, playlists, songs. Without that, "sun" buries
Sunflower Bean under every track with the word in it. Artists rank on their
SORT key, past a leading "The", so "kid" puts The Kid LAROI at the top — the
same rule that files it under K on the Artists list.

**`library/fold.c`** is the new folding, and it is deliberately NOT
`name_hash`. name_hash is the on-disk locator: its bytes are recomputed by
`tools/build_index.py` and pinned by `tests/kernel/name_hash_vectors.h` on
both sides, so growing it a diacritic rule would silently unbind every
accented name in every library an older host tool built. fold.c keeps
name_hash's case and dash rules, adds a 192-entry Latin-1 + Latin Extended-A
table (`elan` finds Élan), and DROPS the quotes entirely — the ring has no
apostrophe key and nowhere to put one, so "its over" has to find "It's Over".
The invariant that matters is asserted through name_hash itself: every pair of
spellings it calls one name, this folds to one string.

**The scan is a plain linear walk** — every artist, album, playlist name and
song title folded and substring-matched per keystroke, ~150 KB of text
typical. A folded-title cache would cost 288 KB of .bss and is not bought
until a bench says the scan is felt, so every scan prints what it cost:
`core: search 3 chars 41 hits 27 ms`. The 25–55 ms estimate is a claim until
that line is read off a real device.

**Hits act like the rows they stand for.** A song plays in its ALBUM (not the
6000-row Songs queue): it is what "play this one" means, it resumes through
the existing `RESUME_KIND_ALBUM` path and it skips the LOADING SONGS bar. An
artist opens their albums; an album opens its tracklist, and MENU from there
returns to the results rather than revealing an album list that was never on
screen (`g_br_from_search`); a playlist opens its tracks. A song the index
lists but the disk no longer has is greyed and Select does nothing.

**It fits the gestures that landed beside it.** PLAY in the picker is the
transport it is everywhere else — a text field has no row under the cursor, and
that is said the way `gesture.h` already says it, with a row count of 0. PLAY
on a RESULT starts the row's queue exactly as PLAY on the list that row came
from does: an artist's whole discography, an album or a playlist from its first
track, a song in its album. The judgement lives in `ui/search.c`
(`search_play_rows`) where the host can assert it, not in main.c's switch.
RIGHT inside the picker is the space bar and never a skip. The drain's
`seekhold_void()` for a RIGHT press off the player screens stays
UNCONDITIONAL — it has to be, because the seek machines are fed from the live
button state at the top of a pass and this drain reads the tick-latched event
further down, so a press that begins between the two is invisible to the
machine until the pass AFTER the jump, by which time the screen is Now Playing
and `allowed` is 1. Without the void that press is a skip on release or a seek
on a hold, on every list, not just Search. `gesture_test` now models that
ordering and asserts both halves of it, the voided one and the one that would
skip. A MENU hold reaches the main menu from
Search like anywhere else — and `scr_pop_to_root` now resets the wheel gesture,
which it did not, and which matters because letter mode outlives a pause.

Also: `menu_render_list` finally scrolls, because Music is nine rows in an
eight-row window and a menu that silently drops its last row is the worst way
to find out it grew. Playlists are read once a session for the search rather
than on every entry.

66 host suites green (new: `fold`, `search` — the latter with a pixel-oracle
painter test in `chrome_test.c`'s style — plus the Search shapes added to
`gesture`), ARM `-Werror` + `verify-hw` clean. The three stages cost about
7.8 KB of text (7,756–7,792 B depending on the build-stamp string in the
baseline), 2,120 B of .bss and 40 B of .data against main (the .data is
`g_letters_for`'s initialiser and `g_search_src`) — 36%, 83% and under 1% of
their budgets, and no new MB-scale buffer. `docs/screens/render.py` gained `letter.png`, `search.png` and
`search_results.png`.

**Nothing here has run on the device.** The bench:
1. Music shows nine rows with a scrollbar; Genres and Search are both
   reachable and the last row is not cut off.
2. Search opens with the cursor on A, an empty plate and the hint.
3. Type S-U-N: hits appear as you type, artist first. RIGHT types a space,
   LEFT deletes. DONE opens the results; MENU returns to the ring with SUN
   still there; MENU again is Music.
4. DONE on a query with no matches does nothing (and the rows say No matches).
5. The four actions: a song plays in its album and MENU from Now Playing is
   the results; an artist opens its albums; an album opens its tracklist and
   MENU is the results, NOT the Albums list; a playlist opens its tracks.
6. An accented or apostrophed title in the library is found by its plain
   spelling.
7. Read `core: search … ms` off `CORELOG.BIN` for a one-, two- and
   three-character query on the full library — that is the real scan cost, and
   the number this entry is missing.
8. The first Search of a session after the drive has parked: the playlist
   folder read spins it up once. Time it; it should be the only stall.
9. PLAY in the picker pauses and resumes; PLAY on each of the four result
   kinds starts the right queue, and MENU from Now Playing is the results.
   Hold RIGHT in the picker: one space, no seek. Hold MENU: the main menu.
10. Onyx: the ring's cursor pill and the query plate in a dark palette.

## 2026-09-16 — Steering by letter, UNFLASHED

Two changes to `ui/wheel.c`, one of which alters how an existing device
behaviour feels.

**The latch and the plate now share one clock.** `wheel_accel_step` treated
any gap over `WHEEL_IDLE_US` (200 ms) as a new gesture and dropped letter
mode, while the plate stayed up for `WHEEL_AZ_HOLD_LETTER` (1.2 s). So for a
whole second the letter could be on screen with the wheel already back on
rows, and the next detent both moved one row and took the plate down — the
control changing meaning under a thumb that had only paused to read it. The
latch now outlives a pause shorter than the plate's hold, which is the rule
the guide already described ("the letter stays for just over a second"). The
SPEED still resets at 200 ms, so a pause never leaves eight rows per detent
armed: pause, and the next detent is exactly one letter.

This is the one change here a user could dislike, and it is deliberately a
single `if` so the rollback is a one-liner.

**The detent is answered by the index, not by a walk.**
`wheel_set_letter_step` is a fourth seam beside the clock, the row-initial
source and the click; `kernel_main` registers `ui/letterindex.c`'s run index
over it. The old `list_letter_step` walk is O(rows in this letter) — up to a
few hundred `initial_at` calls per detent on a 6000-song library, each one a
screen-stack lookup — and it stays as the fallback, so an unregistered seam is
slower, never different. The suite proves which one runs, with what arguments,
and that a screen with no letters never reaches either.

`wheel` suite extended (§4 split three ways, new §9 over the seam); the
`letterindex` step cases are deliberately §6's, because this is the swap.
ARM `-Werror` + `verify-hw` clean.

**Nothing here has run on the device.** The bench:
- Spin Songs fast, stop half a second, one detent → the NEXT letter, plate
  still up.
- Stop two seconds, one detent → one row, no plate.
- Select while the plate is up opens the row under the bar, which is the run
  head the last detent landed on.
- A letter detent back from mid-letter lands on that letter's first row; a
  second one goes to the previous letter.
- At either end of the alphabet a further detent does nothing and does not
  click.
- A fast spin that ends in a SELECT does not carry the plate onto the screen
  that SELECT opened — nor does one that ends in a MENU hold.

## 2026-09-16 — The A-Z letter on every long list, UNFLASHED

The locator plate and the letter-stepping wheel were **Songs only**, and the
comment in `kernel/main.c` said so on purpose: the only tool it had was a
linear walk of the rows, which is affordable exactly once. Every other long
list on the device is alphabetised too — Artists, Albums (all of them and one
artist's), Playlists, a genre's songs, an artist's All Songs — and on each of
them a fast spin showed nothing and moved rows.

`ui/letterindex.c` is the new tool: one walk per list builds an index of
**runs** (`#`, `A`…`Z`, a trailing `#`), and the letter for a row and the head
of the next/previous letter then come out of it by binary search. Runs rather
than a 27-slot table because the library's collation (`title_cmp`) is a
byte-wise ASCII compare: everything below `'A'` sorts before A and everything
above `'Z'` — `[`, `_`, and every UTF-8 lead byte — sorts after it, so `#` is
two groups, one at each end of the list, and a table keyed by letter would
make the one at the bottom unreachable. The run cap doubles as the test for a
list that is not sorted at all.

`main.c` answers one question per screen — the initial of the key that list is
**sorted by** — and that is the part with teeth: Artists sort past a leading
"The" (The Kid LAROI is under K), an album is under its own title, and the
synthetic "All Songs" row is under nothing. A list needs 48 rows and 4 distinct
runs before it gets a plate at all; under that, aiming is slower than
scrolling (at 8 rows a detent, 48 rows is six detents end to end, and the
alphabet is 27). Both numbers are one `#define` in `ui/letterindex.h`, pinned
by value in the suite so a bench can retune them in one line.

**Two library bugs the index walked straight into.**

- `g_album_key[]` — the parsed album titles the album sort compares — was
  never permuted with `g_albums[]` when the sort applied its permutation. It
  had never mattered, because after that loop nothing read the array again; it
  existed only to keep the comparator cheap. The locator reads it (the album
  initial is the album title's, and re-splitting a folder name per row under a
  spinning wheel is the string scan the array was introduced to stop), so the
  stale keys would have been wrong letters on the plate. The key now travels
  with its album.
- **Genres were not sorted.** `genre_intern()` appends in first-seen order, so
  Music › Genres listed them in whatever order the index records arrived —
  no order to read down and nothing to locate within. They are sorted A→Z in
  `library_finish` now, which means remapping every song's genre field through
  the inverse permutation; `library_finish` is the one point where that is
  safe, because it runs before any screen or the resume restore can capture a
  genre index, and nothing on disk holds one (the resume record stores the
  song and rebuilds the view from the song's own field).

`scr_push`/`scr_pop` now call `wheel_accel_reset()`, which `ui/wheel.h` has
claimed they do since the extraction and they never did.

`letterindex` is a new host suite; ARM `-Werror` + `verify-hw` clean.
bss +456 B, text +1.2 KB.

**Nothing here has run on the device.** The bench:
- Artists, Albums, Playlists and Genres: a fast spin puts a plate up; the
  letter matches the row under the bar.
- The Kid LAROI sits under K on Artists, and the plate says K there.
- A filtered artist's album list: the All Songs row never shows a letter, and
  a letter step never lands on it.
- A list under 48 rows (a small Playlists folder, a short artist) never shows
  a plate however fast it is spun.
- Genres reads A→Z, each genre's count is what it was, and About's genre count
  is unchanged.
- The Onyx theme's plate colours.

## 2026-09-16 — UNFLASHED

- **Shuffle Albums.** Settings > Playback > Shuffle is a three-way select now,
  Off / Songs / Albums, like the original iPod's. Songs is exactly today's
  shuffle. Albums plays the album you are on to its end in the index's track
  order, then another album from the same queue at random, until every album
  in that queue has played once; Repeat All deals a fresh album order (with a
  different first album whenever the queue holds more than one), Repeat One is
  untouched, and a one-album queue simply plays — and, under Repeat All,
  loops — in order.

  It is one new branch of the player's existing seeded deal, not a second
  mechanism: `album_deal()` sorts the playable queue indices by
  `(album, order_key, index)`, shuffles the resulting runs with the same LCG
  from the same seed, and expands them back in place. So an album order is a
  pure function of (queue, seed, keep, mode) exactly as a song order is, and
  it rides the resume record's existing `resume_order_seed` /
  `resume_order_keep` pair with **no record change**. The two keys come from
  two new `browse_entry_t` fields that main.c fills at every queue builder:
  `album` (the library's album id + 1, 0 = unknown, which makes an entry its
  own group) and `order_key` (the `(disc << 16) | track` that `browse_bind()`
  already sorts a tracklist by, so an album plays in the order it lists in).

  **On disk:** the same payload byte 0, widened from a flag to the mode
  (0 off, 1 songs, 2 albums). No version bump, no length change, no offset
  moved — a v0.1.3 record loads unchanged, and a v0.1.3 build reading an
  albums record sees "shuffle on", which is Songs. An unknown value reads as
  Off (the theme-id precedent, not a clamp). A SELECT that changes a Settings
  row now also captures the resume context, so the saved
  (seed, keep) can never lag a mode change by a capture window.

  Now Playing shows `SHUF·ALB` instead of `SHUF` — except over a Shuffle Songs
  queue, which is its own song order (`PLAYER_KEEP_QUEUE`) whatever the
  setting says, and still reads `SHUF`. bss grows 36 KB (`g_queue` +24 KB for
  the two fields, `g_sorted` +12 KB of deal scratch); the size gate is
  unmoved at 83% of 14 MB. New host suite `player-album` (35 assertions over a
  fixture of three interleaved albums with scrambled track numbers) plus
  extensions to `settings` and `config`; 63 suites green, `make verify-hw`
  clean.

  **UNFLASHED.** Bench list:
  (a) Songs > play a track with Albums on: the album finishes in tracklist
      order and the next album starts at its track 1;
  (b) Repeat All wraps to a different album;
  (c) power off mid-album, boot: Next is that album's next track;
  (d) Settings > Shuffle cycles Off / Songs / Albums and the token reads
      `SHUF·ALB`;
  (e) Shuffle Songs from the Music menu still shuffles songs with the setting
      on Albums, and shows `SHUF`;
  (f) a v0.1.3 record (byte 0 = 0 or 1) loads with every setting intact;
  (g) PLAY on an artist / genre / album row (the gesture) with Albums on:
      the queue it builds groups by album too — it goes through the same four
      builders, which is the whole reason the keys live on the entry.
- **Clock: the foundation half is in, the UI and the record are not.** The
  device has an RTC — the PCF50605 PMIC keeps a BCD calendar in the same
  always-on domain the standby machine lives in — and nothing has ever read it.
  This lands everything under the screens: `hal/hw/rtc.c` (`hal_rtc_get` /
  `hal_rtc_set` over the existing I²C path, a torn-read retry, and a validity
  gate that makes every wrong answer read as "no time known"), the integer
  calendar `kernel/datetime.c`, the boot decision `kernel/timesync.c`, the
  software clock `kernel/wallclock.c`, the editor model `ui/settime.c` with its
  painter, and the sim's own RTC. Four new host suites (`datetime`, `timesync`,
  `settime`, `hw-rtc`), one of which walks all 36 525 days of 2000..2099.

  **Nothing calls any of it yet.** There is no Date & Time row, no clock on the
  status strip, and no host stamp in `CORECFG.DAT` — those are the second half
  (the settings record's time block at payload 48..63, the Go/Python writers,
  the Settings screens and the boot apply), landing on top of this.

  **The register map is unconfirmed.** Every RTC address is derived from the
  public PCF50606 datasheet and cross-checked against the six PMU registers
  `docs/hw/06-power.md` already documented; the doc now carries the table with a
  confidence column, the `RTCWAK` conflict, and the bench procedure that settles
  it. Nothing here has been on a device.

- **Pause on headphone unplug — the policy, and a probe that needs no cable.**
  The pause decision moved out of `kernel/main.c` (five untested inline lines
  over a file-scope `g_hp_last`) into `ui/jackwatch.c`, a pure module with its
  own host suite — the same extraction `ui/keyhold.c` got. It closes a real
  hole: the suspend path never updated `g_hp_last`, so a plug pulled while the
  device slept left the first pass back looking at an edge the wake had already
  accounted for. The suspend loop now feeds the module as it sleeps (with
  `playing` = the transport state the sleep interrupted), so a pull is seen
  while it happens and the wake simply declines to resume — which is also what
  makes "pulled and plugged back in before waking it" stay paused. The wake
  re-primes as a backstop for the paths that leave that loop early. main.c
  keeps the wiring only: sample, act, narrate — the module reports `IN`/`OUT`
  alongside `PAUSE` so there is exactly one edge detector, and it is the
  tested one.

  **The feature ships INERT.** `HEADPHONE_DETECT_TRUSTED` is still 0, so
  `hal_headphones_present()` answers -1 with no bus traffic and the module
  never sees an edge. What is live in this image is the *probe*: Settings >
  About's footer now reads `ADC 2731 · LOG 6 on · JACK 1 n0` — the raw A7
  level and how many times it has moved since power-on (plus `en=0`/`oe=1` if
  the boot ROM did not leave A7 a GPIO input, and `/1` for the debounced level
  once trusted). Reading it is one 32-bit read of the register the Hold switch
  is already read from, so it costs nothing and is drawn in every build. Every
  edge is also narrated to the UART, which means `CORELOG.BIN`; raw lines are
  budgeted at 64 per boot.

  Why on screen: the only probe that existed prints to SER0, and this device
  has no serial cable (ruled out 2026-07-17), so the pin's polarity was
  unreadable. Now it is a digit on the About page.

  Also: `headphone_pin_cfg()` (two reads, no writes, trace-tested in both
  non-probe binaries); `tests/meson.build` spells out
  `-DHEADPHONE_DETECT_TRUSTED=0` for `hw-headphone-untrusted` so flipping the
  header default at the bench cannot silently turn it into a second trusted
  binary; 58 → 59 host suites (60 with the sleep timer merged beside it). `make sim && meson test -C build-sim` green
  (also green with the header set to TRUSTED 1), `make hw && make verify-hw`
  clean under gcc-16 `-Werror` (also with TRUSTED 1).

  **Nothing here was run on the device.** `kernel/main.c` is not host-built, so
  its wiring is covered by the module's tests plus the bench below — and every
  "on device" line is owed. The bench, in order:

  *Flash 1 — read the pin (the default image, TRUSTED 0):*
  1. Boot with **nothing in the jack**, no charger, Hold off; wait for the menu.
  2. Settings > About. Write down the footer's `JACK <d>` and whether it shows
     `en=`/`oe=`. Any `en=0`/`oe=1` means A7 is not a GPIO input — stop at 6.
  3. Push the plug fully home; the token should change within a second
     (`JACK 1 n1`). Write it down.
  4. Pull it (`JACK 0 n2`). Write it down.
  5. Repeat 3–4 twice more (`n6`). Then wiggle a half-inserted plug for a few
     seconds and note the count (bounce; informational).
  6. Interpret: 1 in / 0 out and one count per motion → set
     `HEADPHONE_DETECT_TRUSTED` 1, leave `ACTIVE_LOW` 0. 0 in / 1 out → also
     set `HEADPHONE_DETECT_ACTIVE_LOW` 1. Never changes with no `en=`/`oe=` →
     wrong pin, leave TRUSTED 0 (follow-up: an on-screen all-ports dump).
     Never changes with `en=0`/`oe=1` → follow-up is a forced-input config at
     boot, its own flash. Flaps untouched → not the jack, leave TRUSTED 0.
  7. Pull `CORELOG.BIN` in disk mode and `tools/make_log.py --dump` it: the
     `core: jack raw=… n=…` lines are the written record of steps 3–5.

  *Flash 2 — the feature (after that two-line edit):*
  8. `make sim && meson test -C build-sim`, `make hw && make verify-hw`, flash.
  9. Play a track, pull the plug: pauses within ~0.3 s, strip says Paused,
     About reads `JACK 0/0`. Re-insert: still paused. PLAY: resumes.
  10. Pause with PLAY, pull, re-insert: nothing either way.
  11. Hold on, playing, pull: pauses. Hold off, PLAY: resumes.
  12. Charger in (charging modal up), playing, pull the headphones: pauses.
  13. Power-cycle with the jack **empty**, Resume on: boots paused as always,
      and no `core: jack out` line in the log for that boot.
  14. Playing with the plug in, hold PLAY 2 s (sleep), pull the plug while
      asleep, press a button: wakes paused, no resume, and the log carries
      `core: jack out during suspend, staying paused`. Re-insert, PLAY: plays.
  14b. Same again, but plug the headphones back IN before waking it: still
      wakes paused (the suspend loop saw the pull as it happened).
  15. Playing, plug in, sleep, wake without touching the plug: resumes
      (unchanged).
  16. Dump `CORELOG.BIN`: `core: jack out, pause` / `core: jack in` at each step.

  Paste the results here and fill the `<!-- bench result: -->` slot in
  `core/docs/hw/10-headphone-jack.md`'s summary table.
## 2026-09-16 — Sleep timer, UNFLASHED

Settings > Playback has a fourth row, **Sleep Timer**: Off / 15 / 30 / 60 /
90 / 120 min. SELECT cycles it and arms a countdown at once; while it runs,
`SLEEP <minutes left>` sits in the right-hand cluster of every list and
Settings strip and after SHUF / RPT on the Now Playing band, dropping once a
minute. At expiry the player is paused and the device takes the existing
`suspend_to_ram()` path — the same one a two-second PLAY hold takes: position
force-committed, drive parked, panel dark, and on battery the 30-minute
escalation to PMU standby. The wake comes back **paused**, unlike a PLAY-hold
wake, because the person fell asleep and the next press is "where was I".

The countdown is a pure module, `ui/sleeptimer.c`, because the arithmetic is
the whole problem: the 1 MHz USEC_TIMER wraps every ~71.6 minutes, so a
120-minute timer crosses it twice. Deltas are taken between consecutive feeds
(so a wrap cancels) and the sub-minute remainder is carried, never truncated.
The host suite runs the full two hours — 72 000 feeds starting 30 s before the
wrap — and asserts a TICK on every minute boundary and the FIRE on the 120th,
not the 119th, not the 121st, not twice.

`kernel/main.c` got thin wiring only: one feed call, one action-code branch,
the token at the three strip paint sites, one more term in `chrome_key()`, and
the PLAY-hold suspend refactored into flags plus ONE shared block, which is
where the disarm-on-any-suspend rule now lives. `suspend_to_ram` itself is
unchanged. The shared block also clears `cpu_idled` and the two transient UI
windows (Hold banner, volume plate) on the way in: this is the first suspend
that can be entered from the dark, idled state, and the first that can be
entered while a banner is up without a button press behind it.

Paths that leave the loop WITHOUT disarming — disk mode, the low-battery
shut-off — never return to it; they come back through a boot, where both sides
start at 0 (`.bss` and `config_decode`).

**The invariant.** `sleeptimer_total_min(&g_sleep) == g_settings.sleep_timer_min`
at every loop top: the settings field is what the ROW shows, `g_sleep` is what
RUNS, and only `sleep_timer_apply()` (plus the shared suspend block and the
FIRE branch, which zero both) ever moves them.

**Never on disk.** `sleep_timer_min` is a runtime-only field in `settings_t`:
`config_encode` ignores it, `config_decode` writes 0 into it (`config_load`
copies the whole decoded struct, so a field decode skipped would arrive
holding stack garbage), and arming returns the new `SETTINGS_ACTION_SLEEPTIMER`
so main.c applies without a `settings_touch()`. Bedtime is the one moment the
drive is parked for the night; arming must not spin it up for a byte-identical
record. Payload length stays 44 and `CONFIG_VERSION` stays 2, so the host
tools and the `verify-hw` resume-parity check are untouched.

59 host suites green, ARM `-Werror` + `verify-hw` clean, `docs/screens/render.py`
regenerated (one new still, `playback.png`).

**Bench list when this is flashed** — none of it has run on the device:
- Arm 15 on Now Playing → the token counts down once a minute → the panel goes
  dark at 0 → a press wakes it paused, on the same track and screen, with the
  row reading Off and no token.
- PLAY-hold while a timer is armed → the wake shows the row at Off.
- Reset Settings while armed → Off, and no token.
- Hold switch on with a timer armed → it still sleeps; waking needs Hold off
  and then a press.
- Leave a timer-initiated sleep on battery for thirty minutes → it escalates to
  PMU standby like any other sleep.
- **The idle-boost case.** Arm 30, let the album END (or pause) and the
  backlight time off, so the loop has dropped the core to 30 MHz
  (`cpu_idled`), then let the timer fire. On wake, the core must still drop
  back to 30 MHz the next time the screen goes off with nothing playing — this
  is the one path that can enter `suspend_to_ram` from the dark, idled state
  (a PLAY hold cannot: the press relights and re-boosts a pass earlier), and
  an unbalanced boost refcount would silently pin it at 80 MHz until a reboot.
  The shared re-seed clears `cpu_idled` for exactly this.
- Boot Details' CFG seq must NOT move when the row is cycled.
- How the token reads on the panel next to a long track name (the name's clip
  loses 28–42 px while the timer is armed; if it reads cramped, the fallback is
  to drop the token from the list strip and keep it on Now Playing, which is
  one call site).

If a fired timer wakes badly, the first bisect is to enter the suspend with a
synthetic 2 s "hold" origin (`suspend_origin = nowp - PLAY_HOLD_US`) — it
changes nothing in the loop and proves whether the origin matters.

## 2026-09-13 — issue sweep landed on `main`, NOT yet flashed

Six read-only audits over the whole tree, then six fix branches merged;
a second wave the same day added Playlists (read path + resume kind),
Now Playing partial repaints, `hal_audio_frames_played()`, the audit
leftovers, and two rounds of cross-branch review fixes (52 commits in all,
54 host suites green, ARM `-Werror` + `verify-hw` clean, nothing pushed). The
two user-reported problems turned out to be one chain: a PLAY-hold "off"
was suspend-to-RAM, which never escalated to PMU standby and drained the
cell; the resulting cold boot then rebuilt the queue as the album.

What changed (see `git log 054c722..`):
- **Three iPod gestures: Play a list, hold to seek, hold for home
  (2026-09-16)** — the button model grew the three press-length gestures the
  original has and this did not. (a) **PLAY on a list title starts it**: on
  Albums (an album row or the artist's All Songs row), Artists, Genres,
  Playlists, Songs, inside an album or a playlist, and on Shuffle Songs, a
  PLAY *tap* builds the queue the row names and pushes Now Playing exactly as
  SELECT would, replacing whatever was playing; a playlist that cannot be
  played shows its tracklist screen with the reason instead of silently
  pausing. Elsewhere PLAY is still pause/resume. (b) **Hold RIGHT/LEFT seeks**
  on Now Playing and the queue view, aiming the way the wheel scrubber does
  (target on the left, signed delta on the right, playhead on the bar) at 5 s
  a quarter-second, then 15 / 30 / 60 as the hold passes 2 s / 5 s / 10 s,
  clamped to the track; one `player_seek_to()` on release, so a long
  fast-forward costs one seek. A hold that moved nothing (let go before its
  first tick, or pinned against an end its whole life) seeks nothing, the way
  the wheel scrubber refuses an unmoved commit; a track change under the aim
  drops it, so a hold across an auto-advance cannot land the old track's
  target in the new one. A tap still skips — decided on the release, like
  PLAY's pause, and a tap whose whole press fell inside one blocked pass is
  recovered from the latched down-edge, so the transport keeps the wheel
  latch's "no tap is ever lost" promise. (c) **Hold MENU one second jumps to the main menu** from
  anywhere; the tap still backs one screen at the down-edge, and a jump out of
  Settings runs the same SOFT commit the tap exit does. The logic is a new
  host-testable `ui/gesture.c` (the skip-vs-seek machine, the ramp, the
  per-screen PLAY policy) plus `keyhold_void()` / `keyhold_held()`;
  `kernel/main.c` only wires it. 61 host suites green (new `gesture` suite
  beside the sleep timer's and the jack watch's, `keyhold` extended), ARM
  `-Werror` + `verify-hw` clean. **UNFLASHED.**
  Bench list: (a) Albums: PLAY on an album row → Now Playing at track 1, MENU
  → the same album row, not the tracklist; (b) Artists → PLAY on an artist →
  its All Songs queue, `TRACK 1 OF n`, and the resume kind after a reboot is
  the artist's songs; (c) the artist's All Songs row, a genre, a playlist, a
  track inside an album and inside a playlist all play from PLAY; a playlist
  whose files are all gone shows "No tracks found on disk" and the music keeps
  playing; (d) Music menu: PLAY on Shuffle Songs deals and plays, PLAY on
  Artists/Albums pauses; (e) main menu / Settings / Now Playing / queue: PLAY
  still pauses, hold still sleeps at 2 s and powers off past 5 s; (f) from a
  dark backlight one PLAY press on a list only lights the screen; (g) Now
  Playing: RIGHT tap → next on release, hold → the band advances and speeds up
  at 2/5/10 s, release lands one seek there, a hold to the end pins and ends
  the track normally, LEFT hold pins at 0; (h) while paused a RIGHT hold moves
  the position and stays paused; (i) scrubber on, then RIGHT hold → the wheel
  goes back to volume; (j) queue view: taps skip, a hold seeks (visible after
  MENU back to Now Playing); (k) Albums list: RIGHT tap jumps to Now Playing,
  RIGHT held 3 s from the list does NOT skip or seek, LEFT hold does nothing;
  (l) MENU hold from inside an album → one pop at the press, main menu at 1 s;
  from Settings → Sound mid-edit the edit closes, then home, and the changed
  value survives a power-off; (m) MENU hold at the main menu does nothing,
  MENU hold with Hold switched on mid-press does nothing, MENU from a dark
  screen only lights it; (n) charging screen: MENU/RIGHT/LEFT dismiss it and,
  held, do nothing more, while PLAY held from it still sleeps; (o) Hold on,
  then RIGHT/LEFT/MENU: banner only; (p) hold RIGHT through the end of a track
  — the next one starts at 0:00 and does NOT jump to where the aim had got to;
  (q) a deliberate ~600 ms RIGHT press (long enough to be a hold, let go before
  its first quarter-second tick) is silent: no hiccup, no jump backwards.
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
- **Hold banner (2026-09-14)** — the centred 180x110 lock plate is gone. A
  Hold edge now inverts the TOP CHROME for a second instead: a dot-keyhole
  padlock that pops open, "Locked" / "Unlocked", and HOLD ON / HOLD OFF where
  the header count sits — 22 px on Now Playing, strip + header through the
  divider on lists and Settings. The title and the art are never covered. A
  button press while locked is drained from the wheel latch and re-shows the
  banner; wheel motion is dropped silently. The present is the band alone
  when nothing else is pending (a full frame when the strip is dirty or the
  panel is stale). Gallery redrawn to match:
  `docs/screens/lock.png`, `locked.png`, the new `locked_list.png`,
  `lock.gif`. **UNFLASHED.**
- **Transport confined to the player screens (2026-09-14)** — RIGHT/LEFT now
  skip ONLY on Now Playing and the queue view: a skip from a list you were
  merely browsing changed the music under you. On every other screen RIGHT
  PUSHES Now Playing (MENU returns to the exact row) and LEFT does nothing
  (MENU is already "back"); the wheel delta is cleared so the push does not
  arrive with a stale detent. `top_banner_render` stayed the reusable
  primitive it became, and ellipsises its label to the room left of the
  token. **UNFLASHED.** Bench list: (a) RIGHT on Albums → Now Playing,
  MENU → same row; same from inside a tracklist; (b) RIGHT/LEFT still skip on
  Now Playing and the queue; (c) LEFT on a list does nothing.
  *A track-change banner was built the same day and then dropped: the status
  strip already renames itself when the queue advances, and that is enough —
  no band, nothing to dismiss, one less thing composed over every list paint.*
- **Status strip on the Settings screens (2026-09-14)** — Settings used to be
  the one place the top band went blank: `ui/screen_settings.c` follows the
  jsx, which draws no strip. Crossing from a list into Settings dropped the
  battery and the playing track off the screen. `settings_render_cur()` now
  paints `status_strip_render()` over the painters' clear band (verified: no
  settings painter draws in rows 0..14 — the header's bold-13 ink starts at
  y 16), and the Hold banner's recoloured strip row no longer skips Settings.
  Gallery redrawn. **UNFLASHED.**
- **The strip is blank when nothing is playing (2026-09-14)** — it used to
  fall back to the `CORE` wordmark. It is a now-playing readout, not a
  wordmark: idle now shows only the battery (and the Hold padlock). The main
  menu's header still says `Core`, as does About's chip. Both C sites
  (`status_strip_render`, `top_banner_render`'s strip row) and the gallery
  renderer changed together. **UNFLASHED.**
- **Leaving Settings no longer spins the drive up (2026-09-14)** — the stall
  on every Settings back-out was a forced commit: the exit calls
  `resume_capture()` (which dirties the record whenever a track is loaded,
  because the position moved) and then `settings_commit(CFG_COMMIT_FORCE)`,
  and FORCE with parked platters means `ata_wakeup()` — 1-3 s — BEFORE the pop
  is rendered. New `CFG_COMMIT_SOFT`: no debounce, like FORCE, but a parked
  drive is never woken; the change stays pending. The IDLE rule tightened the
  same way (`parked` alone defers now, not `parked && player_active`) so the
  deferred write cannot ambush the user with a spin-up three seconds later
  mid-browse. The pending write lands the next time the platters turn for any
  reason, or at suspend / power-off / disk mode / the DISKSAFE flush, all of
  which still force. Trade-off, stated in `cfg_commit.h`: a change made while
  the drive is parked is lost only on a HARD power cut before any of those,
  and the suspend timeout forces a commit within 30 minutes of idle.
  `cfg_commit_test.c` grew the SOFT and parked-IDLE cases (40 → 50 checks).
  **UNFLASHED.** Bench: Settings → MENU with the drive parked is instant and
  silent; a setting changed there survives a suspend/wake and a power-off.
- **The boot is one screen now (2026-09-14)** — the "Core Player / loading"
  splash and the separate titled progress bar are gone; `boot_screen_render`
  draws both. The click-wheel mark (an AA ring + dot, `fill_disc_aa` — the
  plates' corner mask stops at r=16 and the ring is 19), "Core" under it, the
  device line under that, and along the bottom a 2 px ink bar with the phase in
  small caps — `LOADING`, then `LOADING LIBRARY` / `LOADING SONGS` /
  `SHUFFLING SONGS`. Bottom right, in the border colour, a build stamp:
  `git describe --always --dirty --abbrev=7`, baked in by a meson `vcs_tag`
  header (`kernel/core_version.h.in`), screen-only — never on the UART, so the
  clicky boot golden is untouched. **The saved theme is applied BEFORE the
  library load**: the settings read (which needs only `fs`) moved ahead of
  `library_ensure`, so a dark-theme user gets the Linen splash for the disk
  spin-up — unavoidable, the theme is *on* that disk — then one flip, and the
  bar, the placeholder album chips (`chip_placeholder_init` moved after
  `theme_set`) and the menu are all Onyx. Boot Details' OTHER bucket (total
  minus the named phases) absorbs the moved config read; `g_lib_load_ms` is
  measured inside `library_ensure` and is unchanged. **UNFLASHED.**
- **Semantic versions in the UI (2026-09-14)** — releases are git tags
  `vMAJOR.MINOR.PATCH`, and the firmware now reads them instead of carrying a
  version number nobody updates. Two meson `vcs_tag` headers, both regenerated
  on every `ninja` (a `run_command` would only refresh at reconfigure):
  `CORE_BUILD_ID` = `git describe --tags --always --dirty --abbrev=7` (the full
  stamp — **`--tags` is new**, without it a *lightweight* `git tag v0.1.0`
  would have been invisible), and `CORE_VERSION` = `git describe --tags
  --abbrev=0` (the nearest tag alone, `kernel/core_version_tag.h.in`). An
  untagged tree — which is what this one still is — makes the second command
  exit non-zero; the `vcs_tag` fallback turns that into `v0.0.0` and the build
  succeeds, so nothing has to be tagged before it compiles. Today's tree
  generates `CORE_BUILD_ID "7617196-dirty"` and `CORE_VERSION "v0.0.0"`.
  Where they show: the boot screen keeps the build id bottom right (its own
  row, baseline 236, clear of the phase label at 224 — the longest plausible
  stamp is 145 px of 320, so nothing is clipped or overlapped); Settings →
  About's firmware chip reads **"Core v0.1.0"** instead of "Core" (the chip is
  sized from `text_width`, and "iPod 5.5G" ends at x=101 while even "Core
  v0.10.12" starts at x=211); Boot Details puts the **full build id in the
  header's right-hand slot**, the only free text row on a page whose bars,
  legend and LBA rows run to y=230. `screen_settings.c` is host-built and must
  not see a generated header, so both strings arrive as parameters
  (`settings_about_render(..., version)`, `settings_diag_render(...,
  build_id)`); the placeholder call inside `settings_render` passes `"v0.0.0"`.
  `docs/screens/render.py` runs the same two `git describe` commands with the
  same fallbacks at render time, so the stills stop drifting from the
  firmware. **UNFLASHED.**
- **Boot Details no longer spins the drive up (2026-09-14)** — the page probed
  CORECFG.DAT's two slot LBAs and the log's header/next LBAs on EVERY paint,
  and each probe walks the FAT chain, so opening it with the platters parked
  cost a 1-3 s wake before the first pixel. The same four values are already
  probed at boot with the drive spinning; they are kept, the page re-probes
  only `if (!ata_is_parked())`, and a failed probe cannot blank a good
  boot-time value. Nothing about the page changes when the drive is up.
  **UNFLASHED.**
- **The unlock banner no longer eats a second of input (2026-09-14, device
  bug)** — while the Hold banner was up, the main loop `continue`d past the
  render for the whole ~1 s window, so every scroll and press after an UNLOCK
  was *applied* but nothing was drawn until it expired. The banner is
  confirmation, not a modal: the first event after Hold comes off disarms the
  window (`g_lock_flash.armed = 0`) and falls through to the normal render in
  that same pass. The LOCKED banner is unaffected — input while locked is
  swallowed earlier and re-arms the window. **UNFLASHED.**
- **`Fa` was tucked under the F's arm (2026-09-14)** — the atlas generator
  re-solves every letter pair optically, putting each pair's MEAN daylight
  over the x-height band on the face's target. A mean cannot see a recess:
  `F`'s right edge in that band is its open stem on most rows and its middle
  arm on one or two, so the solver kept pulling the next letter left until
  the arm row bottomed out on the 1 px no-touch floor. `Fa` at regular 12
  shipped at a 3 px pen step against the design's 6, with the bounding boxes
  4 px into each other where the font overlaps them 1 — and the same for
  every pair whose left glyph overhangs or recesses (`Ta Ya Va LT AV Fz rT`
  …). Fixed in `tools/atlas_gen.py` only, with a cap
  (`OPTICAL_MAX_BOX_OVERLAP_PX = 1`): a pair's boxes may overlap by 1 px, or
  by as much as the font's own kern for that pair already overlaps them,
  whichever is more. It only ever loosens, it moves 45–166 pairs per face out
  of ~4 200–6 000, and every other pair keeps the step the rhythm solve chose
  (the lowercase rhythm sd over `tools/ui_strings.txt` is unchanged to two
  decimals in all six faces). Regenerated: all six `core/ui/atlas/*.h` — kern
  tables only, no bitmap and no advance moved. regular 12 `Fa` 3→6 px,
  `Ta` 5→6, `LT` 4→5, `rT` 2→4; bold 18 `Fa` 8→9, `Ta` 8→10, `Fz` 6→9.
  `docs/screens/render.py` now reads the atlas `_KERN[]` table instead of
  re-deriving the FONT's kerning, and rounds the pen once per pair like
  `text.c`'s `pen_step` — so the gallery shows the device's spacing for the
  first time, and stills with `T/Y/L/V/F` pairs shifted by a pixel or two.
  **UNFLASHED.** Bench: Albums (`F-1 Trillion`, `Taylor Swift`), the Now
  Playing title in bold 18, About — read `Fa`, `Ta`, `LT` at arm's length.

- **Volume Limit and EQ presets (2026-09-16)** — Settings → Sound grew from
  four rows to six. **Volume Limit** is a 10..100 ceiling (100 = no limit);
  `settings_volume_clamp()` in `ui/settings.c` is the ONE statement of the
  rule, called by both the Settings slider and the Now Playing wheel, so the
  two cannot disagree, and lowering the ceiling under the current volume
  pulls the volume down in the same call. The volume plate keeps its 0..100
  scale and draws a 5x3 ink triangle over the bar at the limit (the 5G
  Features Guide's own marker) — inside `VOL_PLATE_*`, so the pinned
  3200-word present still covers it. A wheel tick that cannot move the
  volume still raises the plate (the marker is the explanation) but no
  longer earns a disk write, which also ends the rail-pinned touches the
  old code did at 0 and 100. **EQ** is a SELECT cycling Off + 17 presets from
  a new host-built table (`ui/eq.c`, 18 names, five band gains and five
  centre codes each — ours, nothing copied). A preset is a full five-band
  curve on the codec's own EQ through the new `hal_eq_set()`, which is now
  the single register encoder: it pre-attenuates `LDACVOL/RDACVOL` by the
  curve's largest boost BEFORE writing the gains, so a full-scale FLAC
  cannot clip the digital path, and `hal_codec_restore()` replays the whole
  cached curve after the per-track codec reset. The DACVOL pair and the band
  gains are ordered by the SIGN of the pre-cut change — cut the DAC first
  going up, take the gains off first coming down — so the attenuation is
  never smaller than the boost that is live, including mid-call; both
  directions are pinned as literal bus bytes. `hal_tone_set` is gone: at EQ
  Off the curve `ui/eq.c` hands over IS the tone control (flat mids on centre
  code 00, the shelves on 105 Hz / 6.9 kHz), so it emits the same words it
  always did — plus that pre-cut, which means **Bass +6 now plays a touch
  quieter overall than it did**. That is the correction, not a regression. While a preset is on, Bass and Treble are
  locked: they render greyed showing the PRESET's shelf gains, the wheel is
  refused and SELECT does not enter edit mode, while the user's own tone
  sits untouched underneath and returns at Off. Record: payload 44 → 48
  under the same version 2 (`volume_limit` u8 at 44, `eq` u8 at 45, u16
  reserved) — `config.c`, `tools/make_config.py`, `core/cli`'s `config.go`
  and all three goldens moved in one commit. A 44-byte record (every device
  in the field) decodes as limit 100 / EQ Off with its volume untouched; a
  limit byte of 0 means UNSET → 100, never the 10% floor. New host coverage:
  `tests/ui/eq_test.c` (the whole table against an independently typed
  fixture), the Sound half of `settings_test`, the sound tail in
  `config_test`, and `volume_trace_test`'s mock-bus pins for the flat curve
  (byte-identical to the shipped tone path), Bass Booster and the restore
  replay. Gallery: `sound.png` redrawn with EQ Rock active, new
  `volume_limit.png`, `settings.gif` regenerated. **UNFLASHED.**
  **Device-only, not observed here:** the EQ2–EQ4 centre-frequency table and
  the `EQxBW` polarity now in `wm8758.h` / `05-audio.md` were written down
  from MEMORY of the WM8758B datasheet — not read out of the PDF and not
  heard on the device — and both are marked "verify". A wrong centre code is
  a band centred somewhere else; a wrong `EQ_BW_NARROW` polarity (this build
  assumes 0 = wide) makes every preset's three peaks narrow instead of wide.
  Either is a differently shaped preset, never a fault. The two SHELF corners
  are not in doubt — the shipped tone control has been using them; whether the EQ saturates
  before or after the DAC-volume stage (if the digital volume sits AFTER the
  EQ, the pre-cut protects the DAC input but not the EQ accumulator); and
  pop/zipper on the `EQ3DMODE` ADC→DAC flip, since none of these registers
  has a zero-cross latch (fallback if it is audible: soft-mute around
  `hal_eq_set`, `DACCTRL_SOFTMUTE`, already used by `wm8758_mute`).
  Bench list:
  (a) set Volume Limit 40 while playing at 70 — the volume drops to 40 at
  once, the plate shows 40 with the triangle at 40%, the wheel cannot pass
  it; Menu out and back, and a power cycle, keep the limit;
  (b) limit back to 100 — triangle gone, the wheel reaches 100;
  (c) EQ Bass Booster on a bass-heavy full-scale FLAC — audibly more bass,
  **no clipping or crackle**, overall level a touch below Off; Bass/Treble
  greyed at +6 / 0 and the wheel will not move them; Off restores the
  user's own tone;
  (d) cycle every preset while playing — no pop, no dropout at the path
  switch; Bass Reducer and Treble Reducer are audibly cuts;
  (e) track change and pause/resume keep the preset (the restore hook);
  (f) Reset Settings — limit 100, EQ Off, saved;
  (g) **datasheet check, off the device:** read R18–R22 in the WM8758B PDF and
  confirm the EQ2/EQ3/EQ4 centre-frequency rows and that `EQxBW` is 0 = wide.
  If the BW polarity is inverted, one constant (`EQ_BW_NARROW`) and nothing
  else changes — every preset writes bit 8 clear, so the whole table flips
  from wide peaks to narrow ones with no code change.

**Device verdicts so far (2026-09-13 evening):** audio noise fixed
(VMID); panel sleep white on wake (off); suspend wakes with the PLL park
off; PMU power-off + wake OK; gauge, scrollbar, seven themes, white blank
flashed. Next bench items: pull CORELOG.BIN after a suspend and `--dump` it,
`chkdsk`, then re-try the 10 Hz tick and the gates ONE AT A TIME with the
log capturing the loop.

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
6. Panel sleep at idle (`PANEL_SLEEP_AT_IDLE 1`, since 2026-09-13): let the
   backlight time out, wait 10 s, press a button — the screen must come
   back with the right content, not white. If white, set
   `PANEL_SLEEP_AT_IDLE 0` in `kernel/main.c`. Then hold PLAY from dark:
   the panel must wake once, sleep into suspend, and come back once. The
   `core: ui` UART line's `bcm_timeouts`/`refused` should read 0/0.
7. Suspend low power (since 2026-09-13, later): while asleep the SoC runs
   on the 24 MHz crystal with the PLL off, SER0/PWM/I²C gated, and a 10 Hz
   tick. Check: the `core: batt` line still prints sane values 10 s and
   60 s into a suspend (not -1 — the I²C re-gate must bring the controller
   back), the wheel wakes it from a 10 Hz sample, a hold to 5 s still
   reaches PMU standby from the crystal, and measure the draw. Rollback is
   per-park: drop `clock_suspend` from `suspend_lowpower_enter` first,
   then the gates.
8. Paused skips: press Next ×5 while paused, then Play — one bring-up,
   no pops on the skips; a 44.1 → 48 kHz skip while paused then Play
   must come up at 48 kHz.
9. **Event log — the write path's second caller; qualify it before it
   flushes.** `python3 tools/make_log.py --create` (Windows-native copy +
   `Write-VolumeCache`), then `sudo python3 tools/make_log.py --verify
   /dev/sdX`: note the header LBA and the next-write LBA. Boot; the UART
   line `core: evlog on seq .. boot .. lba <hdr>/<next>` MUST show the same
   two numbers. If not, power off before a flush. Then hold PLAY (suspend
   → forced flush, `core: evlog blk 00000000 final`), wake, disk mode,
   `--verify` again: block 1 valid, seq 0, FINAL, and `--dump` prints the
   boot narration; `chkdsk D:` (read-only, no `/f`) clean. Then let it run
   with the log ON until a `core: evlog blk` idle write lands (a full block
   is ~40 battery lines, ~4 min) and `chkdsk` once more. About must read
   `LOG <n> on`; `LOG off` means the file was not found or did not
   validate, and nothing is written.

Still open from the audit: the BCM power gate, the ROM's undocumented
`DEV_EN` bits (USB/FireWire/IDE) and a 32 kHz suspend point — `DEV_EN`
gating of SER0/PWM/I2C and PLL-off in suspend have since landed, see
"suspend power" below. `PANEL_SLEEP_AT_IDLE` is now 1 (unflashed; see
"What works" and checklist item 6).
The reserved playlist queue kind has since been wired — see
**Playlists** under "What works". Closed since: the same-hash
tiebreak (`name_bind_exact`, exact on-disk name wins when a bucket has two
candidates), `flac_meta.c` keeping UTF-8 on the scan fallback, and
`ata_identify()` waiting for !BSY before it reads ERR/DF. Also closed: a
skip while paused ran the full codec bring-up (WM_RESET + VMID charge) only
for the 5 s pause timer to power it down again — the bring-up is now owed to
the resume (`g_pl_bringup_pending` in `player.c`, with the player/HAL state
table; `player-queue` 13e'', `player-clock` 11), so ten paused Nexts cost
zero I²C. Add to the first-flash checklist: Next ×N while paused, then Play —
no pop, sound at the last track's rate. Nothing has been pushed.

### 2026-09-13, later — suspend power: PLL parked, peripherals gated, tick at 10 Hz

`suspend_to_ram` used to idle the SoC at the 30 MHz PLL point with the
PLL running, the 100 Hz tick waking the core to read the wheel, and the
UART, PWM and I2C clocks on. Now, after the drive/panel/backlight are
down (`suspend_lowpower_enter`, `kernel/main.c`):

- `clock_gate_suspend` — `DEV_EN` (`0x6000600C`) bits 6 (`DEV_SER0`),
  17 (`DEV_PWM`), 12 (`DEV_I2C`) cleared, one masked RMW each through
  the owning driver; each block re-gates itself on its next use, so the
  codec-off write, the 5 s battery sample and the PMU standby command
  work unchanged. `DEV_OPTO` (wake source) and the ROM's `0xC2000124`
  bits are not touched.
- `timer_set_rate(10)` — `TIMER1_CFG` (`0x60005000`) = `0xC0000000 |
  99999`; the tick counter keeps its 10 ms unit via the `USEC_TIMER`
  reconcile, the wheel is sampled at 10 Hz ("hold any button" wakes),
  and the loop also wakes on a latched button down-edge.
- `clock_suspend` — `CLOCK_SOURCE` (`0x60006020`) = `0x20002222`,
  `DEV_TIMING1` (`0x70000034`) = `0x0303`, `PLL_CONTROL` (`0x60006034`)
  `&= ~0x88000000`, `DEV_INIT2` (`0x70000020`) `&= ~0x40000000`. The
  24 MHz crystal, not the doc's 32 kHz point: that one is for a core
  that has stopped, and nothing in the doc says which peripheral clocks
  follow the core there.

The battery sample, every path into `enter_standby`, and the wake path
restore all three first (resume = the `clock_init` grammar, `TIMER1_CFG`
back to `9999`, the `DEV_EN` bits set again). Trace-tested host-side
(`hw-clock`, new `hw-clock-gate`, `hw-timer`; 55 suites), ARM `-Werror`
+ `verify-hw` clean. **Unverified on the device**: the saving itself
(nothing in `docs/hw/` gives a current figure for any of it — measure
suspend draw before/after at the bench), whether the I2C controller
needs a reset pulse after a gate, and the wheel/UART at the crystal
rate. If a suspend misbehaves, the three parks are independent — start
by leaving `clock_suspend` out of `suspend_lowpower_enter`.

### 2026-09-13, evening — first flash: what the device said

- **Panel sleep: WHITE on wake.** The backlight timed out during playback,
  the panel slept, and the first press came back solid white. Both
  `PANEL_SLEEP_AT_IDLE` and `SUSPEND_PANEL_SLEEP` are back to 0; the
  `lcd_sleep` before the PMU standby command is gated on the same flag.
- **Suspend: SOLVED, late.** Every short-hold suspend went dark and never
  woke; the 5 s hold (PMU power-off) came back from the third image on.
  Three bisect flashes of the low-power switches all "failed" because the
  review-round re-park guard in the idle loop ran UNCONDITIONALLY — the
  PLL park (CLOCK_SOURCE → crystal, PLL off) executed on every image,
  "all off" included, and the wake side never un-parked it. With the guard
  gated, the short-hold suspend wakes and resumes. So: **the PLL park
  breaks the wake** (wheel or tick does not survive the crystal source as
  written); the 10 Hz tick and the SER0/PWM/I2C gates were never actually
  tested alone. All three stay 0 (`SUSPEND_PARK_PLL/SLOW_TICK/GATE_CLOCKS`);
  the drive parks with STANDBY (`SUSPEND_ATA_SLEEP 0`) — SLEEP was swapped
  out mid-bisect and is untested on its own too.
- **Event log** (`CORELOG.BIN`, 4 MiB, on the device since the evening):
  Boot Details reports header/next LBAs 49238456 / 49238464 — header ~1 MB
  past CORECFG.DAT's slot, next block inside the header's cluster. Raw-disk
  `make_log.py --verify` still owed for the record.
- **Audio: FIXED and CONFIRMED.** Hiss with nothing playing, the wheel click
  on the jack and the drive's spin-up on the jack were all one thing: the
  codec played through VMIDSEL 0x2, the datasheet's low-power STANDBY
  divider (worst supply rejection). Playback now uses the normal 0x1
  divider, and the codec is forced cold at boot (a Menu+Select reset keeps
  its rails up). Second flash: jack silent idle, silent paused, no click
  bleed, no drive noise — owner confirmed all four.

### 2026-09-13, later — cross-branch review, two rounds

Five reviewers went over the merged result looking for what each parallel
author could not see. Fixed: a slow suspend entry (spin-up for a forced
save) pushed the PLAY hold past the 5 s escalation and turned a requested
sleep into a power-off — escalation is now also timed 2.5 s from when the
screen went dark (`SUSPEND_ESCALATE_DARK_US`); an in-suspend DISKSAFE
write left the drive in STANDBY instead of SLEEP (`ata_is_slept()` gate);
`ata_sleep()` no longer flushes an already-parked drive; a wake reset that
times out fails fast instead of stacking three budgets; the keyhold swallow
works before the sampler shows the press but only for PLAY and only for a
couple of feeds; a wedged I2C controller is reset on re-init; Next takes the
prefetched hand-over (Repeat-All + Shuffle re-deal) unless it is the same
track (Repeat-One); playlist resolution walks one folder per album instead
of the root per track, stops on a failing disk, and reports unusable
entries. Add to the first-flash checklist: open a 100-track playlist and
time it; hold PLAY on a dirty settings record and confirm the device
sleeps rather than powers off.

### 2026-09-13, later — the DAC's position is asked, not guessed

`hal_audio_frames_played()` (hal.h) reports what the converter has actually
clocked out. Hw: completed DMA buffers plus the timed part of the current
one (kick timestamp x rate — the late-kick detector's own figures), so the
residual is the 16-frame I2S FIFO (~0.4 ms) plus however late the pump pass
that reads it lands; frozen across stop/start, unmoved by a flush, zeroed by
init. Sim: SDL's pulls less the 1024-frame buffer in flight, interpolated
the same way, so the residual is the host mixer's own latency rather than a
guessed 350 ms (the sim does not link the player today; the backend is
there for when it does). `player.c`'s `frames_heard()` now pairs that count
with the ring's (`heard_rebase()` at every `hal_audio_init` and
`hal_audio_flush`) instead of subtracting a hardcoded 2 x 8192, so the
gapless hand-over — and the clock it anchors — lands on the true frame
where it was 0..186 ms late. The elapsed clock's accumulator/anchor design
is unchanged. Covered by `hw-audio-pingpong` (section 11) and
`player-clock` (sections 6–10: FIFO/SDL/device depths, count wrap, a
codec-cold pause, a seek, a 44.1 → 48 kHz hand-over). Unflashed: on the
device, listen for the title and clock flipping exactly as the next track
starts, and watch `audio_late_kicks()` — a late completion is now also a
late position, capped at the buffer.

### 2026-09-13, later — an on-disk event log (unflashed, device-gated)

Every `core:` line the UART carries is now also captured into an 8 KiB RAM
ring (`kernel/evlog.c`, a weak-symbol tap in `hal/hw/uart.c`; the wire bytes
are unchanged, the clicky golden still matches) and flushed one 2048 B block
at a time into `CORELOG.BIN`, a 4 MiB ring `tools/make_log.py --create`
pre-allocates next to `CORECFG.DAT`. Same rules as the settings file: the
device never creates or grows it, block 0 (header: magic/version/block
size/count/id/CRC) is validated at mount and never written, every block's
LBA is re-resolved through the cluster chain (`fat32_file_lba_at`, new)
before every write, whole physical sectors only, and the write goes
through `cfg_commit_gate` — idle: one full block, debounced, never wakes a
parked drive; forced at suspend entry and standby (FINAL block, wakes the
drive first); the DISKSAFE last write is exempt; nothing below it. The
cursor is found at boot by a binary search over the ring's seq numbers
(~11 reads for 2047 slots), not a sweep. Boot line:
`core: evlog on|off seq .. boot .. prev none|final|unflushed lba <hdr>/<next>`
— `prev` is the boot reason as far as the log knows it. About shows
`LOG <seq> on` / `LOG off` / `LOG <seq> err`. New narration: suspend
entering / idle loop / wake, standby entering, each flush's result.
Host: `evlog` (150 cases: format, the host tool's fixture decoded by the
firmware, every mount refusal, the whole flush policy, ring wrap, torn
blocks, the O(log n) scan on 64 slots) and `make-log` (the `--dump` round
trip), plus `fat32_file_lba_at` cases in `fat32`. 57 suites green, ARM
`-Werror` + `verify-hw` clean, image +14 KB. **Not flashed**: checklist
item 9 is the qualification `kernel/config.c`'s banner demands of a second
caller of `ata_write_sectors()` — this is that caller.

**To pull the log:** disk mode → `powershell.exe Copy-Item D:\CORELOG.BIN
C:\Users\<you>\CORELOG.BIN` (Windows-native; a drvfs read can serve a stale
copy) → `python3 tools/make_log.py --dump /mnt/c/Users/<you>/CORELOG.BIN`
(`--blocks` for the per-block headers).

### 2026-09-13, night — stutter solved, About redesigned, both confirmed on the device

The event log paid for itself on its first pull: all 12 "audio underrun"
counts came at one moment, playback start, coincident with "lcd: BCM
idle-wait timed out" and a 20 ms present gap — a partial present handed to
the BCM while it was still retiring the previous full frame stalled the main
loop past the PCM ring. One more timeout after every "suspend: wake" was the
resume stutter. Fix (1a7067b): only FULL presents set `g_present_cost_us`,
the transport partial is paced by `present_gap_us()` since `last_present`,
and `last_present` is re-stamped after `suspend_to_ram` returns.
`EVLOG_RING_BYTES` 8 → 16 KiB (the log was dropping bytes at boot).

About (bcb7bca) is now a dashboard: name + Core chip on one row, the three
stat columns, STORAGE and BATTERY on side-by-side `PAL_PLATE` cards each
with a big value, its own gauge and a caption ("53.5 of 74.5 GB", "3912
mV"), and one muted footer "ADC n · LOG n on". The "library too large"
warning is drawn by the renderer (`lib_truncated` argument), not painted
over from main.c. Rendered on the host for every theme before flashing.

**Device verdicts (owner, all three):** About layout good; play from a cold
boot → Boot Details underruns 0; short-hold suspend → wake → resume with no
stutter, underruns still 0. 58 suites green, ARM `-Werror` + `verify-hw`
clean, readback-verified flash.

**CORRECTION, same night (second log pull):** the pacing fix was not the
whole story. A run that ended in a power-off, then a cold boot that
restored the track PAUSED, then Play ~2 min later: 11 underruns, ring_low
8 %, decode 61 ms/kframe for 25 s, 20 BCM timeouts. The buffer under a
paused-at-boot track held only the 1.49 s ring prime's worth of file; the
main loop's 20 s idle spin-down parked the drive; Play drained the ring
into diskbuf's synchronous fallback, which blocked on the spin-up. Fix
(d9197e4): the paused pump stocks the anti-skip buffer to DISK_LOW while
the platters are up (never on a parked drive), the playing pump honours
`ata_is_parked()` alongside its own flag, and `player_resume` pre-pays
the spin-up before the DAC starts when the buffer is under DISK_LOW/2.
Five host cases (player-queue 13b). **Owner: "no more underruns" on all
three sequences** (paused-at-boot → park → Play; pause mid-song → park →
resume; suspend while playing → wake). Third log pull: the one flushed
window of that boot has a pause, a park and a resume with underruns 0 and
bcm_timeouts 0 — so the BCM timeouts were a side effect of the stalls,
not a separate fault. Most of that session was lost, though: the log's
last flush was a suspend entry and the rest sat in RAM through the
reboot into disk mode. 1879fa0: entering Disk Mode from Settings now
forces a settings commit and a FINAL log block first (narrated
"core: disk mode: entering"). The ROM Select+Play route still loses RAM
— unavoidable.

**Pushed 2026-09-14 (054c722..353f2de, 108 commits), CI green on every
job.** README and core/README rewritten for the device as it is; the
docs/screens gallery re-rendered against the firmware (render.py repaired —
it had not run since the atlas set changed; docs/screens/README.md holds
the rules; Settings-side screens checked against host renders of the real
C, library-side against main.c). Next bench: the three suspend switches
one at a time with the log capturing; playlists with real .m3u8 files.

**End of night, device image = 1879fa0.** Confirmed on the device today:
silent jack (VMID), steady charge gauge, scrollbar under fast scroll,
white blank at sleep, seven themes on a five-row picker, short-hold
suspend + wake, 5 s power-off + wake, event log end to end, About
dashboard, zero underruns on every resume path. Open: SUSPEND_SLOW_TICK
and SUSPEND_GATE_CLOCKS untested alone, clock_suspend (PLL park) needs a
rework before it comes back, ATA SLEEP vs STANDBY untested alone, USB
ground-loop noise is the cable. Nothing pushed.

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
and the full themed UI (Linen/Onyx plus five more palettes, see Settings
below). On-screen framebuffer console plus the new
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
- **Streaming decode** — `dr_flac` and `pvmp3` freestanding, fed by a
  read-ahead disk source over an 8 MB anti-skip buffer; a full-length track
  streams off the disk while the UI stays live. FLAC is proven on the device.
  MP3 is switched ON as of the entry at the top of this file, but has never
  been played on hardware — see that entry for what to bench.
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
  before *every* write. `config_save()` was the only call to
  `ata_write_sectors()` in the firmware until the event log below became
  the second. **Proven on hardware 2026-07-27**; slot alternation
  confirmed from a raw disk dump, and `chkdsk` found no problems
  afterwards.
- **Event log** (unflashed, device-gated — checklist item 9) —
  `CORELOG.BIN`, pre-allocated by `tools/make_log.py` (4 MiB: a header
  block + 2047 ring blocks of 2048 B = two physical sectors each). The
  firmware taps every UART byte into an 8 KiB RAM ring and flushes it one
  block at a time (16 B header: magic, seq, boot id, length + FINAL bit,
  CRC-32; 2032 B of text) at block `1 + seq % 2047`, LBA re-resolved
  through the file's cluster chain before every write, through the same
  commit gate as the settings save (idle: full block, debounced, drive
  already up; forced at suspend/standby; the DISKSAFE last write; never
  below it). Block 0 is never written; if it does not validate the log is
  off and About says `LOG off`. Pull it: disk mode → `powershell.exe
  Copy-Item D:\CORELOG.BIN C:\...` → `python3 tools/make_log.py --dump`.
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
  unboosted and then **parked** — `DEV_EN` SER0/PWM/I2C gated, `TIMER1`
  at 10 Hz, PLL disabled + unpowered on the 24 MHz crystal (registers
  listed under "suspend power" above; **unverified on the device**,
  draw unmeasured) — codec off via the pump; it samples the battery on
  the 5 s cadence (clocks restored around the sample) and runs the
  DISKSAFE/SHUTOFF policy while asleep, escalates to
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
  sleep at idle is ON** (`PANEL_SLEEP_AT_IDLE 1`, 2026-09-13, rollback
  documented beside it; **equally unflashed**): at backlight-off the loop
  now issues `LCD_SLEEP`; the wake is one block in the main loop that does
  `lcd_wake` -> paint -> full present (retires the panel init) -> backlight,
  the input sites no longer light the LED over a slept panel, and a present
  refused while slept is counted (`lcd_presents_refused`, host-tested) so
  the wake frame is full by construction. Idle sleep and suspend compose
  (sleep twice, wake once — idempotent, host-tested). `panic()` now wakes
  and lights the panel before it draws. `bcm_init()` exists but is compiled
  out. The traced idle path is in the commit message of the flip.
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
  album-art chips, and — on every long alphabetical list — the A-Z plate and
  letter-stepping on a sustained fast spin.
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
- **Settings** — nine rows: Playback (shuffle **Off/Songs/Albums** / repeat /
  **resume** / sleep timer), Sound
  (volume / bass / treble / balance via the WM8758 EQ), Theme (seven live
  palette swaps — Linen, Onyx, Sage, Plaster, Olive, Umber, Mushroom; the
  picker scrolls, an unknown stored id lands on Linen), Display (backlight
  timeout + brightness), Clicker
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
2. **Search** — built (2026-09-16), **not yet flashed**; see that entry's
   bench list. What is still not there: searching by genre or composer, art
   chips on album hits, and any narrowing of the scan (a full library walk
   per keystroke, timed onto the UART).
3. **A screen-tuned font face.** Advances, kerning and tracking are all
   fixed and measured, and the type still reads wrong at 9–13 px. Nunito
   ships no hinting bytecode, so the next lever is swapping the face for
   one designed for small sizes — not more spacing tuning.
4. **Flash and measure the power work** — the suspend/standby changes
   above are entirely device-unverified; the first flash should check the
   panel wakes from `LCD_SLEEP` (both after a suspend AND after a backlight
   timeout — panel sleep at idle is now on, checklist item 6), the drive
   wakes from SLEEP, and read the suspend draw. Panel sleep at idle is the
   one with the wider exposure: it fires on every backlight timeout, so a
   white wake there is the first thing to look for; the rollback is
   `PANEL_SLEEP_AT_IDLE 0`.
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
   `CORECFG.DAT` (`tools/make_config.py`) and `CORELOG.BIN`
   (`tools/make_log.py`), and copy to the device.
9. **Host CLI install/flash/recover are stubs** —
    `core/cli/internal/cli/install.go` says so outright; flashing is
    `ipodpatcher` by hand today.
10. **The firmware does not read the iPod's name.** As of L5b the host
    owns it: `core name D: "Brandon's iPod"` writes the FAT volume label
    (`BRANDON'S I` — 11 upper-case ASCII bytes is the whole field) and
    core-app shows the friendly name in its header. The device could read
    the same name off the boot sector's `BS_VolLab` (or the root
    directory's volume entry, which is what Windows really shows) and put
    it in About and on the boot screen. Nothing in the firmware does yet;
    it is a read of bytes we already mount, not a filesystem project.

## Testing

`meson test -C build-sim` from `core/` (**59/59** green):

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
- **Sleep timer** — `core/ui/sleeptimer.c`'s minute accumulator, including the
  full 120-minute countdown across TWO 32-bit microsecond wraps (72 000 feeds),
  that it fires exactly once and disarms itself, and that coarse or irregular
  feeds carry their remainder instead of drifting. None of that can be run on
  the device: the clock wrap is ~71.6 minutes apart.
- **Event log** — `kernel/evlog.c` on a RAM disk with a deliberately
  fragmented `CORELOG.BIN`: the block format and every rejection, the host
  tool's `--emit` fixture decoded by the firmware, every mount refusal, the
  flush policy through the real `cfg_commit` gate, ring wrap and remount,
  torn blocks at and off the cursor, an unreadable slot, the O(log n) boot
  scan; a recorder refuses any write outside the file's own clusters.
  `make-log` round-trips a synthesized ring through `--dump`.
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

## Performance notes (moved from README 2026-09-14)

Verbatim from the README, which no longer carries it.

## Performance — real-time on a 2006 SoC

The PP5022 is a pair of ~80 MHz ARM7TDMI cores with **no FPU, no hardware
divide**, a small unified cache, and a **PIO** disk (no DMA to the drive,
~170 KB/s). Decoding FLAC in real time *and* driving a smooth, animated,
antialiased UI on that budget took deliberate work — the interesting part
of the project is how little the hardware gives you.

- **Clock + cache first.** Enabling the PP5022 unified cache and holding an
  80 MHz boost across the whole open/decode path is the line between
  stuttering and real-time FLAC.
- **Measure the boot, don't guess at it.** "Boot takes ten seconds" had
  nowhere to aim, so the boot path got instrumented per phase and the
  numbers got their own screen. A cold boot measured **8.4 s**, of which
  **5.1 s was a single FLAC seek**; removing that should leave roughly 3 s,
  but the post-fix total has deliberately not been written down here because
  nobody has yet read it off the device. Boot Details reports it live, on
  every boot rather than on the one day someone times it — which is the
  point of the screen.
- **A `#define` that cost five seconds.** `DR_FLAC_NO_CRC` was set purely to
  save per-frame decode cost — but `dr_flac` guards its *binary-search seek*
  behind that same flag, because landing on an arbitrary byte means proving
  a candidate frame header is real rather than audio that happens to look
  like a sync code, and the CRC is the proof. These files also carry no
  SEEKTABLE, so with both paths gone every seek fell through to decoding
  from the start of the track: 5.1 s of that 8.4 s boot was one seek.
  Seeking is O(log n) now, at a cost of ~14 KB of text and some per-frame
  decode margin — which is exactly why Boot Details reports the decode
  margin instead of assuming it.
- **No divides in the hot path.** The gamma-correct text blend runs entirely
  in integers off pre-baked sRGB↔linear LUTs (never touches `<math.h>`), and
  the per-pixel alpha composite replaces three soft-divides with an exact
  `floor(x/255)` add-shift — the divide-less ARM7 never pays for a divide
  while painting glyphs.
- **Draw only what changed.** The marquee scrolls through a tiny partial
  present (just the title band), not a full-frame blit, and clips per pixel to
  its row — so continuous animation costs almost nothing.
- **Instant library.** The song database is built on the host into a single
  index the firmware loads in *one read*; Songs / Albums / Genres open with no
  per-file tag scan at boot, and per-genre counts are precomputed. Records bind
  to files by a hash, not a directory-walking string compare.
- **Streaming without skips.** A read-ahead disk buffer does bursty reads so
  the drive head parks between them (anti-skip), feeding a lock-free SPSC PCM
  ring drained by the DMA-completion ISR — audio never waits on the UI. Bulk
  ATA reads land straight in the caller's buffer, with a one-sector bounce only
  for unaligned tails.
- **Album art that never stalls audio.** Covers are pre-converted on the host
  to raw RGB565 sidecars (no on-device JPEG decode); the list-chip cache loads
  at most one thumbnail per main-loop pass so scrolling can't starve the audio
  DMA, and the 28 px chip is an exact-size file — a 1:1 copy, no resample.
- **No allocator in the render path.** The Nunito glyph atlases are `const`
  `.rodata` resolved at link time — no FreeType, no malloc, no init step.
- **Idle costs something, so spend less of it.** At idle the CPU drops to
  30 MHz and halts, and the drive spins down after 20 s — including while
  paused. At stop, the codec is powered down and the audio clocks are gated.
  Asleep (Play held), the drive is parked, the panel and backlight are off,
  the codec is down and the CPU idles between wheel samples.
- **A pause stocks the buffer.** While a track sits paused and the platters
  are still up, the player quietly reads ~9 s of the file ahead, and a
  resume over a parked drive with a shallow buffer spins the drive up
  *before* the DAC starts — so waking the device never stalls a second into
  the music. Found on the device with the event log: eleven underruns,
  every one of them a spin-up the ring could not cover.
- **Give the display controller time.** The BCM retires a full frame after
  the present returns; a partial update sent too soon behind it stalls the
  main loop past the audio ring. Presents are paced from the measured cost
  of the last full frame, on every path including the wake from sleep.

