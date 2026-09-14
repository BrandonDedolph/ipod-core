# docs/screens — the README gallery, and how it is made

Every PNG and GIF in this directory is drawn by `render.py`: a host-side
reproduction of the on-device UI in PIL, using the same Nunito faces, the
same palettes and the same pixel geometry as the firmware. They are **not
photographs and not framebuffer captures** — the firmware has no screenshot
path and the panel does not photograph cleanly. The README says so.

```bash
tools/.venv/bin/python3 docs/screens/render.py            # regenerate in place
CORE_SCREENS_OUT=/tmp/x tools/.venv/bin/python3 docs/screens/render.py   # elsewhere
```

PNGs are 320×240 upscaled ×3 with NEAREST (960×720). GIFs are ×2 (640×480),
96-colour adaptive palette, `disposal=2`. `render.py` reads the per-face
tracking, ascent and line height straight out of `core/ui/atlas/*.h`, and
refuses any character that is not in the atlas, so a screenshot cannot use
a glyph the device would draw as a box.

## The one rule: the firmware is the source of truth

A screenshot here is a claim about what the device draws. So:

- **Every label, row, count and number comes from the firmware source**, never
  from memory or from an older screenshot. The files and the functions:
  - list chrome (header, rows, selection bar, scrollbar, fonts):
    `core/ui/chrome.h` (geometry constants) and `core/ui/chrome.c`
    (`ui_header`, `ui_list_row`, `ui_scrollbar`);
  - palettes, all seven themes: `core/ui/palette.c` (`PAL_LINEN` …
    `PAL_MUSHROOM`, every slot is RGB565);
  - Settings screens: `core/ui/screen_settings.c` (root/Playback/Sound/
    Display/Clicker lists, the theme picker `theme_render`, About
    `settings_about_render`, Boot Details `settings_diag_render`) and the
    row tables in `core/ui/settings.c` (`ROOT_L`, `PLAY_L`, `SOUND_L`,
    `DISP_L`, `THEME_L`, `CLICK_L`);
  - low-battery screens and the charging screen: `core/ui/screen_battery.c`,
    `core/ui/screen_charging.c`;
  - everything else — status strip, main menu, Music menu, Artists/Albums/
    Songs/Genres/Playlists lists, album detail, Now Playing, the volume
    plate, the lock plate: `core/kernel/main.c` (`status_strip_render`,
    `main_menu_render`, `music_menu_render`, `g_main_menu`, `g_music_menu`,
    `albumlist_render`, `detail_render`, `artists_render`, `songs_render`,
    `genres_render`, `playlists_render`, `playlist_render`,
    `nowplaying_render`, `nowplaying_transport_render`,
    `volume_overlay_render`, `lock_plate_render`).
- **Where the host can build the real renderer, match it pixel for pixel.**
  `core/ui/screen_settings.c`, `screen_battery.c` and `screen_charging.c`
  compile on the host (the test suite does it). A small harness that links
  them and dumps the framebuffer produces the *actual* device output for
  those screens; when such reference images are provided, the PIL version
  must line up with them — same baselines, same x positions, same colours
  — and the check is done by reading both images, not by assumption.
- **Fonts.** `FONT_SMALL` = regular 9, `FONT_SUB` = regular 11, `FONT_ROW` =
  regular 12, `FONT_HEADER` = bold 13, `FONT_TITLE` = bold 18, and bold 12
  for list-row right values and the Now Playing state label. Those are the
  only six faces the device has.
- **Realistic data, no lorem ipsum.** The library shown is the one in
  `render.py` already (LANY, Post Malone, Rearrange…, AUSTIN); art comes
  from `art/`. Counts, times and LBAs should look like the device's
  (Boot Details on the device today: cold boot ≈ 3.3 s; a healthy decode
  margin is ~34 %; underruns 0; CONFIG LBAs 49236472 / 49236474; LOG LBAs
  49238456 / 49238464; About: 4127 songs / 318 albums / 142 artists,
  21.0 GB free of 74.5, battery 73 % at 3912 mV, ADC 2731, LOG 6 on).
- **GIFs tell one story each** and hold on the frames a reader needs to see.
  The frame lists live in `render.py` (`build_walkthrough_gif`, `gif_*`).
  Keep total sizes reasonable (the walkthrough is under 1 MB).

## What the gallery contains

| File | Screen | Drawn by |
|---|---|---|
| `boot.png` | boot splash | `screen_boot` |
| `mainmenu.png` `music.png` | main menu, Music menu | `screen_mainmenu`, `screen_music` |
| `artists.png` `albums.png` `songs.png` `genres.png` `allsongs.png` | library lists | `screen_*` |
| `playlists.png` | Music → Playlists list | `screen_playlists` |
| `detail.png` | album detail (tracklist) | `screen_detail` |
| `nowplaying.png` `volume.png` | Now Playing, volume plate | `screen_nowplaying`, `screen_volume` |
| `lock.png` `locked.png` | Hold-switch plates | `screen_lock`, `screen_locked` |
| `settings.png` `sound.png` `clicker.png` `theme.png` | Settings | `screen_settings`, `screen_sound`, `screen_clicker`, `screen_theme` |
| `about.png` `bootdetails.png` | About dashboard, Boot Details | `screen_about`, `screen_diag` |
| `nowplaying_onyx.png` `albums_onyx.png` | the Onyx theme | `with_palette(ONYX, …)` |
| `charging.png` `battery_low.png` | charging screen, low-battery warning | `screen_charging`, `screen_battery_low` |
| `demo.gif` | cold boot → browse → play walkthrough | `build_walkthrough_gif` |
| `browse.gif` `volume.gif` `themes.gif` `lock.gif` `settings.gif` | one feature each | `gif_*` |

## Keeping it honest

When a screen changes in the firmware, the screenshot changes in the same
PR. `git log --oneline -- core/ui core/kernel/main.c` since the last gallery
commit is the checklist. The image is wrong the moment it shows a row the
device does not have, a theme count it does not ship, or a layout it moved
on from — and the README's readers cannot tell.
