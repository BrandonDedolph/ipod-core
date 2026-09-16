# Shared brief for every agent on this job

Repo: /home/brando/Projects/ipod_theme (git, branch main at dbce3b8). Firmware lives in core/.
Project: "core", a from-scratch Apache-2.0 firmware for the iPod 5.5G (PP5022, two ARM7TDMI, no FPU,
WM8758B codec, 320x240 BCM framebuffer, ATA PIO, FAT32). Cleanroom: NO Rockbox code, no GPL code,
no RPSL/RCSL (Helix) code. Anything pulled in must be Apache-2.0 compatible (Apache/MIT/BSD/CC0/Unlicense/public domain).

Read first, in this order: README.md, docs/USER_GUIDE.md, STATUS.md (top 150 lines), core/README.md,
then the source that matters for your feature. Design references are design_reference/*.jsx.

Ground rules that already shape the code (do not fight them):
- Freestanding C11, integer-only in the firmware, no libc/malloc. -Werror under arm-none-eabi-gcc 16.
- The firmware never creates, grows or moves a file on disk. It writes only into pre-allocated files
  (CORECFG.DAT settings/resume record via kernel/config.c + kernel/cfg_commit.c; CORELOG.BIN event log).
  Anything new that must persist rides in the settings record (bump carefully: old records must load) or a
  new pre-allocated file created by the host tools (tools/make_config.py style, and core/cli Go `core sync`).
- UI model code (ui/settings.c, ui/wheel.c, ui/keyhold.c, ui/chrome.c, player/player.c, library/*) is
  host-built and unit-tested under core/tests (meson). New logic goes in a host-testable module, NOT in
  kernel/main.c, which is 7300 lines and the merge hot spot. main.c changes should be thin wiring.
- Button model: ui/keyhold.c arbitrates tap vs hold (PLAY: tap=pause, hold=sleep). RIGHT/LEFT skip only on
  Now Playing and the queue view; elsewhere RIGHT pushes Now Playing. Hold switch locks input.
- Settings screens are data-driven tables in ui/settings.c (ROOT_L/PLAY_L/SOUND_L/DISP_L). Only rows that
  do something may appear.
- The status strip (ui/chrome.c status_strip_render) shows playing track + battery + padlock, SHUF/RPT tokens
  on Now Playing.
- Player queue kinds: album/songs/artist/genre/shuffle/playlist; resume record carries kind+seeds (ui/settings.h).
- Verification commands (run from core/):
    make sim && meson test -C build-sim          # host suites, must stay green
    make hw && make verify-hw                    # ARM image, -Werror, layout/size checks
  Nothing can be tested on the device in this job; say so explicitly for anything device-only.
- docs/screens/render.py renders the gallery from the atlases; docs/USER_GUIDE.md documents every control.
  A user-visible change updates the guide (and STATUS.md with an UNFLASHED note).

Commit style: imperative subject like the log (`ui: ...`, `player: ...`), body explains why. End every
commit message with the line:
Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
