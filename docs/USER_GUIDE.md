# Using core

This guide describes Core v0.1.0. The device shows its version bottom-right on the boot screen and
in Settings → About; Boot Details shows the full build string.

This is the guide for the person holding the iPod. It covers the controls, every screen, putting
music on the device, power, and what to do if something goes wrong. Building and flashing the
firmware are in the [README](../README.md).

## Controls

The click wheel scrolls. The centre button is Select. Menu is the top of the wheel, Play at the
bottom, Left and Right at the sides. The Hold switch is on the top edge.

| Control | On lists and menus | On Now Playing |
|---|---|---|
| Wheel | Moves the selection | Volume. After a Select tap, seeks |
| Select | Opens the row. On a track, plays it | Tap: scrub mode. Hold: the queue |
| Menu | Back one screen | Back to the screen you came from |
| Play | Tap: pause or resume. Hold two seconds: sleep. Hold longer: power off | Same |
| Right | Jumps to Now Playing if a track is loaded, playing or paused | Next track |
| Left | Nothing but the click | Previous track |

Right on a list pushes Now Playing over it, so Menu brings you back to the exact row you left.
Left and Right skip tracks only on Now Playing and the queue. Play works everywhere.

On the Songs list only, spinning the wheel fast puts the selected row's first letter on screen so
you can aim. The letter stays for just over a second after you stop.

Hold Play for two seconds to sleep. Keep holding past five seconds and the device powers off
instead. Any button wakes a sleeping device where you left it. A powered-off device cold boots on
the next press.

<p align="center"><img src="screens/jump.gif" alt="Right jumps to Now Playing, Menu returns" width="360"></p>

## The Hold switch

Slide Hold on and the top of the screen inverts for a second with a padlock and Locked. Buttons
and the wheel do nothing while it is on; a press shows the banner again. A small padlock stays in
the status strip. Slide it off for the Unlocked banner. Touch the wheel or a button and the banner
goes away at once.

<p align="center"><img src="screens/lock.gif" alt="the Hold banner" width="360"></p>

## The status strip

The band at the top of every list and every Settings screen shows the playing track's name on the
left and the battery on the right, with the padlock between them when Hold is on. When nothing is
loaded the left side is blank. On Now Playing the band reads Now Playing or Paused, with SHUF and
RPT tokens when shuffle or repeat are on.

## Browsing

<table>
  <tr>
    <td><img src="screens/mainmenu.png" width="260" alt="Main menu"></td>
    <td><img src="screens/albums.png" width="260" alt="Albums"></td>
    <td><img src="screens/detail.png" width="260" alt="Album detail"></td>
  </tr>
</table>

**Main menu.** Music, Playlists, Settings, and Now Playing once something is loaded. Podcasts and
Audiobooks are greyed placeholders.

**Music.** Playlists, Artists, Albums, Songs, Shuffle Songs, Genres. Composers and Audiobooks are
greyed placeholders.

**Albums** lists every album with its cover chip and artist. Select opens the album: cover, title,
artist, track count and length, then the tracks with their numbers and durations. Select on a
track plays the album from there.

**Artists** lists artists; Select shows that artist's albums with an All Songs row at the top that
is the whole discography, album on the sub-line.

**Songs** is every song in the library. **Genres** lists genres with a count each. **Shuffle
Songs** deals the whole library and starts playing.

**Playlists** are `.m3u8` files you put in `Music/Playlists/` on the disk (see Putting music on
it). Select one to see its tracks; Select a track to play the playlist from there.

The header's right side shows where you are in the list, for example `6 / 17`. A long title
scrolls while it is selected.

## Now Playing

<table>
  <tr>
    <td><img src="screens/nowplaying.png" width="260" alt="Now Playing"></td>
    <td><img src="screens/volume.png" width="260" alt="Volume"></td>
  </tr>
</table>

The cover, then the eyebrow `TRACK 3 OF 12` (your place in the queue), the title, the artist, the
album. At the bottom the elapsed time, the time remaining, and the progress bar.

**Volume.** Turn the wheel. A bar appears for a moment; the speaker icon grows sound waves as it
goes up and shows a cross at zero.

**Scrubbing.** Tap Select. The wheel now moves the play position five seconds per click, more per
click the faster you turn. The left time shows where you are aiming and the right time shows how
far that is from where playback is, as a signed figure. Stop turning and it seeks after about four
tenths of a second. Leave it alone for four seconds and the wheel goes back to volume.

**The queue.** Hold Select for about half a second. The queue view lists what is playing and what
is next, with the playing row marked. Select a row to jump to it; Menu returns to Now Playing.
Left and Right skip here as well.

**Shuffle and repeat** are in Settings, Playback. They show as SHUF and RPT (or RPT1) in the top
band.

**When a track ends** while you are browsing, the name in the status strip changes. Nothing pops
up.

## Settings

<table>
  <tr>
    <td><img src="screens/settings.png" width="260" alt="Settings"></td>
    <td><img src="screens/about.png" width="260" alt="About"></td>
    <td><img src="screens/bootdetails.png" width="260" alt="Boot Details"></td>
  </tr>
</table>

Select a row to open it. On a slider, Select starts editing, the wheel changes the value, and
Select or Menu finishes. Every change is saved to the disk; if the drive happens to be asleep the
save waits until it next spins, or until the device sleeps or powers off, so leaving Settings
never makes you wait.

- **Playback.** Shuffle on or off. Repeat Off, All, or One. Resume on or off: with Resume on, the
  next boot comes back on the track you were on, paused at the same position, in the same queue.
  Turning Resume off also forgets the stored position.
- **Sound.** Volume, Bass and Treble (plus or minus 12 dB), and Balance. These drive the WM8758B
  directly.
- **Theme.** Seven palettes. The current one is marked; Select switches at once.
- **Display.** Backlight: Never, or 5, 10, 15, 30 or 60 seconds after the last input. At the
  timeout the light drops to a quarter of your brightness, and fifteen seconds later it goes off.
  Brightness: 1 to 32, shown as a percentage.
- **Clicker.** The click the wheel makes: Off, Tick, Click, Pop, Blip, Tock, Double, Chirp.
- **About.** The firmware version, model, song, album and artist counts, storage free, battery
  percentage and voltage, and the event log's state.
- **Boot Details.** The full build string, how long the last cold boot took and where it went
  (LCD, disk, library, resume, other), the FLAC decode cost against real time, audio underruns,
  and the disk addresses of the settings file and the log. Reading it never wakes a sleeping drive.
- **Disk Mode.** Saves everything and reboots into Apple's USB disk mode.
- **Reset Settings.** Back to the defaults, saved.

## Themes

<table>
  <tr>
    <td><img src="screens/nowplaying.png" width="260" alt="Linen"></td>
    <td><img src="screens/nowplaying_onyx.png" width="260" alt="Onyx"></td>
    <td><img src="screens/nowplaying_sage.png" width="260" alt="Sage"></td>
  </tr>
</table>

Linen (warm light), Onyx (warm dark), Sage (dark green-grey), Plaster (pink-beige), Olive
(greige-olive), Umber (espresso), Mushroom (warm greige). Every screen uses the theme's ink and
surface, so the selected row and the Hold banner invert with it. The low-battery red never changes.

## Putting music on it

The firmware reads what a host tool prepares. All of this happens on a computer with the iPod in
disk mode, on the iPod's FAT32 volume.

1. **Folders.** Put each album in its own folder under `Music/`, one FLAC per track. The firmware
   plays FLAC. MP3 files are ignored: the decoder is built but switched off because it cannot keep
   up on this CPU, so `.mp3` files never enter the library.
2. **The index.** Run `tools/build_index.py --src <your Music folder> --out <iPod>/Music/CORELIB.IDX`.
   The firmware looks for `CORELIB.IDX` in `Music/`, or in the volume root if there is no `Music/`
   folder. There is no default output path; pass `--out` yourself. The firmware loads this in one
   read at boot. Rebuild it whenever you add or remove music. Titles, artists, albums, genres and
   durations come from the files' tags through `ffprobe`.
3. **Album art.** Run `tools/coreart.py --thumb <album folder>` (or `--batch <root>` on the tree).
   It writes `folder.art` (120 px, the Now Playing cover) and `folder.thm` (28 px, the list chip)
   next to the tracks from the FLAC's embedded cover. No art file, no chip: the row shows a
   placeholder.
4. **Playlists.** Put `.m3u8` files in `Music/Playlists/`. A path inside that starts with a slash
   is read from the volume root; anything else is read relative to that folder. The firmware reads
   playlists; it cannot write them.
5. **Two files it writes to.** `CORECFG.DAT` (settings and resume) and `CORELOG.BIN` (the event
   log) must exist in the volume root before the first boot: `tools/make_config.py --create
   <iPod>` and `tools/make_log.py --create <iPod>`. The firmware never creates, grows or moves a
   file, so it can only write into space that is already there.

Library limits: 6000 songs, 1024 albums, 512 artists, 128 genres. Past a limit the About page says
"Library too large · some items not shown" in red.

## Battery and charging

The battery in the status strip is an estimate from the cell's voltage. An old cell sags when the
drive spins and rebounds after, so the percentage can move several points in a minute; the
voltage on the About page is steadier.

- **Charging.** Plug in and the charging screen shows the percentage; any button dismisses it, and
  that press does nothing else. The gauge reads the cell, not the charger, so it does not jump to
  full when you plug in.
- **Low.** At 3.7 V a small toast says the battery is low.
- **Very low.** At 3.5 V a full-screen warning: the drive parks whenever playback does not need it,
  and the device stops writing settings and the log until you plug in. Plug in now to keep listening.
- **Empty.** At 3.3 V a goodbye screen, then power off, so the disk is never cut mid-write.

<table>
  <tr>
    <td><img src="screens/charging.png" width="260" alt="Charging"></td>
    <td><img src="screens/battery_low.png" width="260" alt="Very low"></td>
  </tr>
</table>

## Power

- **Backlight.** After the timeout in Display the light dims, then goes off fifteen seconds later.
  Any input brings it back; the press that wakes it from off is not acted on.
- **The drive** spins down after twenty seconds without input as long as nothing is playing, so it
  parks while paused too. It spins up again when the next read needs it; a resume over a parked
  drive spins it up before the music starts.
- **Sleep.** Hold Play two seconds. Playback pauses and the position is saved, the drive parks, the
  screen and backlight go off, the codec powers down. Any button wakes it back where it was.
- **Power off.** Hold Play past five seconds, or leave a sleeping device on battery for thirty
  minutes: it powers itself off. The next press cold boots; with Resume on you come back on the
  same track, paused.

## Disk mode and the files on the drive

Settings, Disk Mode saves everything and reboots into Apple's USB disk mode. From power-on, hold
Select + Play for the same thing from the boot ROM; that works even if the firmware will not boot.

On the volume you will find, besides your music: `Music/CORELIB.IDX` the library index,
`Music/Playlists/` your playlists, `CORECFG.DAT` the settings and resume record in the root, and
`CORELOG.BIN` the 4 MiB event log, also in the root. `tools/make_log.py --dump CORELOG.BIN` prints
the log: every diagnostic line the firmware wrote, with boot boundaries, battery samples and the
audio counters. If you report a problem, that dump is what explains it.

## If something is wrong

- **It will not boot, or the screen is blank.** Hold Select + Play at power-on. That is Apple's disk
  mode, in the boot ROM; the firmware cannot remove it. Then reflash, or restore the backup
  (README, Flash).
- **The library is missing or stale.** Rebuild `CORELIB.IDX` and copy it over.
- **Settings do not stick.** `CORECFG.DAT` must exist in the volume root; create it with
  `tools/make_config.py --create <iPod>`.
- **Something else.** Pull `CORELOG.BIN` in disk mode and dump it. Known issues and what is being
  worked on are in [STATUS.md](../STATUS.md).
