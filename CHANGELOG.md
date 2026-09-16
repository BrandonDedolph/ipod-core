# Changelog

Each release lists what changed on the device since the previous one. Versions are git tags;
the boot screen and Settings → About show the one the device runs.

## Unreleased

- **MP3 plays.** It was compiled and switched off from the beginning because
  `dr_mp3`'s decoder is floating point and this CPU has no FPU — it needed 24
  times FLAC's work and the audio buffer starved. The decoder is now AOSP's
  fixed-point `pvmp3` (Apache-2.0), at about 1.7x FLAC, and `.mp3` files appear
  in the browser, in Songs, Genres and Shuffle, in the index and in `core sync`
  like any other track. MPEG-1, MPEG-2 and MPEG-2.5 Layer III, constant or
  variable bitrate; the low sample rates (8, 11.025, 12 and 16 kHz) are refused,
  because the iPod's DAC cannot be clocked there.
  **Not yet played on a device** — Settings → About → Boot Details shows the
  decode cost as a percentage of real time, and that reading is the gate.
- Titles, artists, albums, genres, track numbers and years come from an MP3's
  ID3 tags, including tags written in UTF-16 by Windows software — those used
  to be unreadable even when MP3 was enabled by hand. Album art can come from
  an MP3's embedded cover.
- Scrubbing and resume work on an MP3: the position maps through the file's own
  Xing seek table, so resuming a podcast comes back where it was instead of at
  the start.
- Boot Details' DECODE percentage is measured against the playing track's own
  sample rate. It had assumed 44.1 kHz, so a 48 kHz album read 8 % low.

- **Search.** Music → Search. The wheel moves along a strip of letters, digits and SPACE / DEL /
  DONE, and Select types; Right is a space and Left deletes. It matches song titles, artist names,
  album titles and playlist names as you type, showing the first four hits under the strip, and
  DONE opens the whole list. Names that start with what you typed come first, and artists before
  albums before playlists before songs, so one artist is never buried under two hundred tracks.
  Capitals, accents and curly apostrophes are all ignored: `elan` finds *Élan* and `its over`
  finds *It's Over* — which matters because the strip has no apostrophe key. Select or Play on a
  result does what the same row does anywhere else: the song plays in its album, the artist,
  album or playlist opens or plays. Above 200 matches it says how many there really are and asks
  for another letter. What you typed survives leaving the screen until the device is powered off.
- **The A–Z letter on every long list.** The big letter a fast spin puts on screen used to appear
  on Songs alone; it is now on any alphabetical list of about six screens or more — Artists,
  Albums, Playlists, Genres, an artist's All Songs, a genre's songs. It is the letter the list is
  *sorted by*, so The Kid LAROI is under K and an album is under its own title. While the letter
  is up a click moves a whole letter rather than a row, and it stays up — and stays in letter
  mode — for just over a second after you stop, so pausing to read it no longer drops you back to
  scrolling one row at a time.
- **Genres are in alphabetical order.** They used to be listed in whatever order the library
  happened to load them in.
- **Shuffle Albums.** Settings → Playback → Shuffle is now Off, Songs or Albums. Songs is what
  Shuffle always did. Albums plays the album you are on to its end in track order, then another
  album from the same list at random, until every album in that list has played; Repeat All starts
  again with a new album order, Repeat One is unchanged, and one album on its own just plays in
  order. Shuffle Songs on the Music menu still shuffles songs whichever setting is on. Now Playing
  reads SHUF·ALB while albums are being shuffled. The setting rides the byte the old on/off flag
  used, so a record written by an earlier build loads unchanged and an earlier build reads Albums
  as Songs.
- **The iPod knows what time it is.** The PMIC has always had a real-time clock in its always-on
  domain — it keeps counting while the device is off — and nothing read it. Settings → Date & Time
  sets it, chooses 12- or 24-hour, and can put the time in the title bar: on the top band while
  nothing is playing, and in the main menu's header always. The track name always wins the top
  band.
- **The computer sets the clock.** `core sync`, `core install` and `core eject` write your
  computer's time and time zone into `CORECFG.DAT`, and the iPod takes it at its next boot — it
  cannot be told the time while it is plugged in, because on the cable it is Apple's disk mode
  answering, not this firmware. `eject` stamps last, so the normal flow hands the device a time
  seconds old. A clock already running more than ten minutes ahead is left alone rather than
  dragged backwards by a stamp that might be days old; set that one by hand. `core doctor` prints
  the stamp and whether the device has taken it.

- **Volume Limit.** Settings → Sound has a ceiling on the volume, 10% to 100%, with 100% meaning
  no limit. Lower it below where the volume is and the volume comes down with it at once. On Now
  Playing a small triangle marks it on the volume bar and the wheel stops there; the bar keeps its
  full 0–100 scale. No combination — anyone who can reach Settings can move it back.
- **EQ presets.** Off plus seventeen: Acoustic, Bass Booster, Bass Reducer, Classical, Dance,
  Electronic, Hip-Hop, Jazz, Loudness, Pop, R&B, Rock, Small Speakers, Spoken Word, Treble
  Booster, Treble Reducer, Vocal Booster. Each is a full five-band curve on the WM8758B's own
  equaliser. While a preset is on, Bass and Treble show its bass and treble greyed and the wheel
  will not move them; set EQ to Off and your own values come straight back.
- A boosting curve — a preset or Bass/Treble — is now played with the DAC turned down by exactly
  the size of the boost, so a loud track cannot distort. That is 3 to 7 dB depending on the preset
  (7 for Loudness, which is therefore the quietest of them), and up to 12 dB for Bass or Treble at
  their maximum. Bass +6 is 6 dB quieter overall than it was.
- The settings record grew from 44 to 48 payload bytes for those two fields, under the same
  version: a record written by any earlier build still loads, and loads as "no limit, EQ off".

## v0.1.3 — 2026-09-15

Since v0.1.2. The firmware change is one marker; the release is the host app.

- The image carries a `CORE-FW-VERSION:` marker so `core info` / `core update` can read the
  version off the device.
- The host app. `core` is one Go binary for Windows, macOS and Linux: `sync` puts a music tree on
  the iPod (files, `folder.art`/`folder.thm`, playlists, `CORECFG.DAT`/`CORELOG.BIN` once, the index
  last), `flash` writes a firmware image after a whole-partition backup and a read-back compare,
  `update` fetches the latest release, `info`/`backup`/`doctor`/`eject` do what they say. The
  Python tools in `tools/` stay as the parity oracles: the Go index is byte-identical on the whole
  library. `core-app` is the same thing as a desktop window (Gio). Both were run against the
  device: backup and image read identical to ipodpatcher's, a flash of the running image verified
  by both tools, a sync that left the index byte-identical to the Python one.
- The artist genre map is compiled into the binary, so a sync run away from the repository still
  applies it (the first on-device sync had written 919 empty genre fields).
- On Windows `core-app` asks for Administrator when it opens (an embedded manifest), so the
  window reads the iPod's disk and flashes in-process with no second prompt.

## v0.1.2 — 2026-09-14

Since v0.1.1.

- The Hold banner no longer inverts when locking. Locked and Unlocked both draw on the surface
  with the rule under the band; the closed or open padlock and the words carry the difference.
  The flip between an ink band and a surface band read as jarring on the device.
- The user guide's opening line names the running version again (it still said v0.1.0 in the
  v0.1.1 release). `tools/release.py <version>` now sets the version everywhere it is shown,
  regenerates the stamped stills and GIFs, and lists anything that still disagrees.

## v0.1.1 — 2026-09-14

Since v0.1.0.

- Settings → About draws its Core chip and both gauges in the theme's ink instead of the accent,
  so the page matches the theme. The theme picker keeps each swatch's accent dot.
- The Hold-banner gallery images are renamed (`hold.gif`, `hold_locked.png`, `hold_unlocked.png`,
  `hold_locked_list.png`). Same content; new paths so GitHub stops serving the old plates from
  its image cache.
- A `LICENSE` at the repository root, so GitHub detects Apache-2.0.

## v0.1.0 — 2026-09-14

First tagged release. Since the untagged firmware of 2026-09-13.

### Hold switch

- The centred lock plate is gone. A Hold edge inverts the top chrome for a second: an ink band
  with a closed padlock and "Locked", a surface band with the popped-open padlock and "Unlocked",
  HOLD ON / HOLD OFF where the header count sits. Nothing is covered. The battery stays in the band.
- A button press while locked is drained and re-shows the banner. Wheel motion while locked is
  dropped without lighting the screen. Presses no longer sit in the latch and fire on unlock.
- After unlocking, the first wheel move or press ends the banner and repaints at once.

### Controls

- Right and Left skip track only on Now Playing and the queue view. On every other screen Right
  jumps to Now Playing, pushed over the current screen, so Menu returns to the exact row. Left
  does nothing there.

### Screens

- Every Settings screen carries the status strip: track name, battery, padlock.
- The status strip shows nothing instead of CORE when nothing is playing. The main menu's Core
  header stays.
- Boot: one screen from power-on to the menu. Click-wheel mark, Core, IPOD VIDEO · 5.5 GEN, a
  2 px bar with the phase in small caps, the version stamped bottom-right. Settings are read and
  the theme applied before the library loads, so the loading bar is in your theme. The splash
  before the disk mounts stays Linen.
- Boot Details shows the boot-time disk addresses unless the drive is up, instead of spinning it
  up on entry.
- Settings → About shows the version. Boot Details shows the full build id.

### Behaviour

- Leaving Settings no longer spins up a parked drive. The exit write lands if the platters are
  turning; otherwise it waits for the next spin-up or a graceful exit. The idle path follows the
  same rule.

### Type

- Letter pairs with a recess or overhang on the left (Fa, Ta, LT, rT, Fz and the like) were
  tucked too tight by the atlas generator's optical pass. The pass now caps the tuck at the font's
  own or one pixel. Kern tables only; no glyph changed.

### Repository

- README rewritten as a product page. New `docs/USER_GUIDE.md`. Gallery renderer lays text out
  with the device's own kern tables and space advance; stills and GIFs regenerated.
