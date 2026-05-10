# Rockbox Theme — iPod Classic Video (5G)

A clean, original Rockbox theme designed at the iPod's native **320×240** resolution. Rounded geometric type (Nunito), warm-light + true-dark palettes, and a complete set of menu, system, and Now Playing screens.

This package is a design reference — the canonical layouts, tokens, and behaviors for an implementation in actual Rockbox WPS / SBS / FMS files.

---

## Open the design

Open `Rockbox Theme.html` in a browser. It loads a pan/zoom canvas with every screen + an interactive iPod prototype (drag the click wheel; toggle the orange Hold switch on top).

## Files

| File | Contains |
|---|---|
| `Rockbox Theme.html` | Entry point — wires the canvas + all screens |
| `themes.jsx` | The 4 Now Playing themes (Linen, Paper, Ink, Card) and shared atoms |
| `menus.jsx` | Main menu, music sub-menus, settings, list helpers, status strip |
| `collection-detail.jsx` | Album & Playlist detail views |
| `system-screens.jsx` | Boot, shutdown, file browser, EQ, theme picker, WPS info pages, charging, locked, unlocked |
| `interactive-ipod.jsx` | Live prototype — stack-based nav, click wheel, volume, hold |
| `ipod-frame.jsx` | iPod 5G hardware frame — bezel, click wheel, hold switch |
| `volume-demo.jsx` | Volume overlay + slider demo |
| `design-canvas.jsx` | Pan/zoom canvas component |

---

## Design tokens

### Type
- **Family:** `Nunito` (400, 500, 600, 700, 800)
- **Mono:** `JetBrains Mono` (file paths, track info, peak meter)

### Color — light (Linen / Paper / Card)
- Surface: `#f4f1ec` (Linen), `#faf8f4` (Paper), `#eeeae3` (Card)
- Ink: `#1a1714`
- Muted: `#5a5048`, `#7a7068`, `#9a8e80`
- Border: `rgba(26,23,20,0.08)`
- Accent: `oklch(0.7 0.12 40)` (warm terracotta)

### Color — dark (Ink)
- Surface: `#0e0d0c`
- Text: `#e8e4dd`, `#f4ede2`
- Muted: `#7a736a`, `#a89e92`, `#6a635a`
- Accent: `oklch(0.7 0.12 40)`

### Sizes (at 320×240 native)
- Status strip: 9px caps, 0.5 letter-spacing
- Header: 12.5px, weight 700
- List row: 12px primary, 10px secondary, height 22–24px
- Now Playing title: 17px (Linen), 14px (Paper), 16px (Ink/Card)
- Progress bar: 2px (thin) or 4px (Linen)

---

## Screen inventory

### Now Playing (4 directions)
- **Theme 1 — Linen.** Warm light, text-forward. Album art + title/artist/album block, generous progress bar. Status row: "Now Playing/Paused" + battery (with hold lock when engaged).
- **Theme 2 — Paper.** Minimal, art-centered. Big art top, title/artist below, slim inline progress. Tiny play/pause glyph + "PLAYING" caps top-left.
- **Theme 3 — Ink.** True dark with terracotta accent. Smaller art, text-rich, thin accent progress.
- **Theme 4 — Card.** Light with floating card around the metadata.

All themes share a status strip (battery, hold) and respect playing/paused state.

### WPS info pages (cycle on center button)
- Page 1: Big art
- Page 2: Peak meter (real-time L/R bars)
- Page 3: Track info (key/value: codec, sample rate, bit depth, path, etc.)

### Volume overlay
Centered transient overlay shown when the user spins the wheel during playback. Speaker icon + fill bar + percentage. Light + dark variants.

### Menus & browsing
- **Main menu:** Music, Playlists, Podcasts, Audiobooks, Settings, Now Playing
- **Music sub-menu:** Artists, Albums, Songs, Genres, Composers
- **Lists:** Artists, Albums (with art chips), Songs, Genres (with track count), Playlists, Podcasts (shows), Podcast Episodes, Audiobooks
- **Detail views:** Album (art header + tracklist), Playlist (summary + tracks). Now-playing track marked.

### Settings
- Settings main, Playback, Sound, About
- Theme picker (with current selection check)
- 5-band Equalizer (60Hz / 230Hz / 910Hz / 3.6k / 14k, ±12dB)

### System
- Boot splash with progress bar
- Shutdown / sleep
- File browser (raw filesystem)

### Power & Lock states
- Charging — full-screen battery, big %, time-to-full estimate (charging vs unplugged variants)
- Locked — dim Now Playing context + centered black plate ("LOCKED") — flashes ~1s then dismisses; persistent small lock indicator stays in status bar near battery
- Unlocked — light plate ("UNLOCKED") — flashes ~1s then dismisses; corner lock disappears

---

## Interaction model

The interactive prototype treats navigation as a stack of frames:

```
{ type: "main" | "music" | "artists" | ... | "playing", sel: number }
```

- **Center button:** activate selection / drill in
- **Menu button:** pop frame (back)
- **Prev/Next:** decrement/increment selection (or scrub on Now Playing)
- **Play/Pause:** toggle playback or jump to Now Playing
- **Wheel rotation:**
  - On Now Playing → volume (briefly shows overlay)
  - On lists → moves selection
- **Hold switch (top of device):** toggles a global lock. While locked, all wheel input is blocked and shows a 1s "LOCKED" plate. Status bars across all screens render a small lock glyph next to the battery.

Lists scroll automatically so the selection stays visible (~1/3 from the top of the viewport).

---

## Translating to Rockbox `.wps` / `.sbs`

Rockbox themes are configured via tag files. Key tags this design uses:

| Concept in this design | Rockbox tag(s) |
|---|---|
| Track title / artist / album | `%it`, `%ia`, `%id` |
| Track number / total | `%pl` / `%pe` |
| Elapsed / remaining / total | `%pc` / `%pr` / `%pt` |
| Progress bar | `%pb` |
| Volume | `%pv` |
| Battery percent | `%bl` |
| Charging | `%bc` |
| Time of day | `%cc:%cM` (12-hour) |
| Album art viewer | `%Cd` (display), `%Cl` (load) |
| Shuffle / repeat | `%mp` (playmode) |
| Hold | `%mh` |
| Codec / bitrate | `%fc`, `%fb` |
| Conditional formatting | `%?xx<true|false>` |

The 4 themes are intended to be 4 separate `.wps` files sharing one `.sbs` (status bar) and one menu skin. Settings live in `.cfg`.

### Suggested font

Nunito isn't bundled with Rockbox by default. Either:
- Convert Nunito to `.fnt` via `convbdf` (Rockbox's font tooling), or
- Use Rockbox's built-in `26-NimbusSans` as the closest rounded fallback.

A 16-pixel and 12-pixel weight would cover this design's full type scale.

---

## What to keep, what to invent

This design is a reference, not a one-to-one Rockbox port. Some flourishes here (animated peak meter, smooth progress, volume overlay placement) may need to be approximated using Rockbox's actual conditional-tag system. Stay faithful to:

- Type hierarchy and weights
- Color palette
- Spacing rhythm of status / header / list
- Lock + battery sitting together in the status row

…and approximate the rest within Rockbox's WPS expressiveness.

---

## Credits

Design built from scratch — no proprietary Apple or Rockbox-stock assets used. Free to adapt.
