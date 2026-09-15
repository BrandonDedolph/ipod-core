# Changelog

Each release lists what changed on the device since the previous one. Versions are git tags;
the boot screen and Settings → About show the one the device runs.

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
