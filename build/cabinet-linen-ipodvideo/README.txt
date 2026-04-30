Cabinet + Linen — for iPod Video (5G)
=====================================

A Rockbox theme + custom UI plugin pair.

Linen is a warm-light theme for Rockbox's WPS. Cabinet is a plugin that
replaces Rockbox's hardcoded main menu with a curated, design-driven UI:
custom main menu, Music browse chain (Artists/Albums/Songs with album art
hero), three Now Playing pages (default / big art / track info), volume
overlay, lock plate, real shuffle/repeat icons.

Installation
------------
1. Make sure your iPod Video is running Rockbox (https://rockbox.org).
2. Mount the iPod and copy the contents of this zip to the iPod's root.
   The .rockbox folder will merge with your existing one.
3. On the device, set Linen as your theme:
     Settings → Theme Settings → Browse Theme Files → linen
4. Initialize the database (one-time):
     Database → Initialize Now → wait for scan
5. Launch the Cabinet UI:
     Plugins → Apps → cabinet

Optional: set Cabinet as the auto-launched start screen so the iPod boots
straight into our UI:
     Settings → General Settings → System → Start Screen → Custom →
     /.rockbox/rocks/apps/cabinet.rock

Controls (in Cabinet)
---------------------
  Wheel up/down        scroll list / volume on Now Playing
  Left / Right         prev/next track on Now Playing
  Center (Select)      drill in / cycle NP info pages
  Play/Pause           toggle pause
  Menu                 back

Files
-----
  .rockbox/themes/linen.cfg            theme entry
  .rockbox/wps/linen.{wps,sbs}         WPS + status bar
  .rockbox/wps/linen/*.bmp             per-theme bitmap chrome
  .rockbox/fonts/nunito-*.fnt          Nunito at 9/11/13/13-bold/17-bold
  .rockbox/rocks/apps/cabinet.rock     custom UI plugin (ARM)

Licenses
--------
  Theme + plugin: GPL-2.0-or-later (matches Rockbox)
  Nunito font:    SIL Open Font License 1.1 (see OFL.txt)

Credits
-------
Designed and authored from scratch — no proprietary Apple or Rockbox-stock
assets used.
