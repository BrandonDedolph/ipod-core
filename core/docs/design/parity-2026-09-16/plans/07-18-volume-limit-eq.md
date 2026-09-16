# 07-18 — Sound: Volume Limit + EQ presets

Plan only. Tree at `dbce3b8` (main), read 2026-09-16. Nothing here has been on the device.

## Summary

Two rows join Settings > Sound, both persisted in the settings record as a 4-byte tail appended
under the existing length-gated scheme (payload 44 → 48, `CONFIG_VERSION` stays 2, every record on
every device still loads):

1. **Volume Limit** — a slider 10..100, default 100 (= no limit). The Now Playing wheel and the
   Volume slider clamp to it; lowering it below the current volume pulls the volume down at once.
   The Now Playing volume plate keeps its 0..100 scale and draws a small ink triangle above the
   bar at the limit (the original iPod's marker, per the 5G Features Guide p.27: "A triangle on
   the volume bar indicates the maximum volume limit"). No combination lock.
2. **EQ** — a SELECT row cycling Off + 17 presets (Acoustic, Bass Booster, Bass Reducer,
   Classical, Dance, Electronic, Hip-Hop, Jazz, Loudness, Pop, R&B, Rock, Small Speakers, Spoken
   Word, Treble Booster, Treble Reducer, Vocal Booster). **Option (a)**: each preset is a full
   5-band curve on the WM8758B's own EQ (R18–R22) plus a DAC-volume pre-cut equal to the curve's
   largest boost so a full-scale FLAC cannot clip the digital path. "Flat" is dropped — on this
   hardware it would be audibly identical to Off (both write 0 dB in every band), and the brief
   says only rows that do something may appear. While a preset is active the Bass and Treble
   sliders are **locked**: they display the preset's low- and high-shelf gains, greyed, and the
   wheel does not move them; the user's own bass/treble values are untouched underneath and come
   back the moment EQ returns to Off.

All new logic is host-testable: the model in `ui/settings.c`, the preset table in a new
`ui/eq.c`, the register grammar in `hal/hw/volume.c` (mock-bus test), the record in
`kernel/config.c`. `kernel/main.c` changes are wiring: one clamp call on the wheel path, one
argument on the overlay painter, one `hal_eq_set` in `settings_apply`, one "locked" check before
entering slider edit mode.

## Current behaviour (file:line)

**Settings model** — `core/ui/settings.h:46-107` `settings_t` (`volume` 0..100 at :51, `bass,
treble` −12..12 at :52, flagged COSMETIC in the comment though they are live). `core/ui/settings.c`:
- `:135` `SOUND_L[4] = { "Volume", "Bass", "Treble", "Balance" }`; `:190` `settings_count(SOUND) = 4`.
- `:243-244` `settings_kind(SOUND, *)` = SLIDER for every row.
- `:293-302` `settings_value`: Volume `fmt_pct` num/den = volume/100; Bass/Treble `fmt_db`,
  (v+12)/24; Balance `fmt_balance`.
- `:389-392` `settings_activate(SOUND, *)` → NOOP (sliders).
- `:406-417` `settings_adjust(SOUND)`: Volume `clampi(old+delta, 0, 100)`, Bass/Treble ±12,
  Balance ±100; returns "moved".
- `:157-183` `settings_defaults`: volume 70, bass/treble/balance 0.

**Record** — `core/kernel/config.h:73-95` versioning rule (append fields, `length` not `version`
gates them; `CONFIG_VERSION 2`). `core/kernel/config.c:160-203` payload offsets: v1 bytes 0..11,
locator 12..23, queue context 24..43, `CFG_PAYLOAD_V2Q = 44`. `:296-352` `config_encode` writes
length 44; `:354-450` `config_decode` clamps every field, gates the tails on `len >=`.

**Volume paths** — `core/kernel/main.c`:
- `:2871-2880` `settings_apply()`: `g_volume = g_settings.volume; hal_volume_set(g_volume);
  hal_balance_set(...); hal_tone_set(g_settings.bass, g_settings.treble); theme_set(...)`.
- `:6302-6327` Now Playing wheel: detent quantisation → `step` (±1, ±4, ±6 … capped ±12),
  `g_volume += step` clamped 0..100 inline, `hal_volume_set`, `g_settings.volume = g_volume`,
  `settings_touch()`, `ui_window_arm(&g_vol_show)`, `np_vol_dirty = 1`. The touch happens even
  when pinned at a rail.
- `:3337-3358` `volume_overlay_render(int vol)`: plate `VOL_PLATE_*` (`ui/chrome.h:118-121`,
  60,101,200×32), speaker at PX+16, bar `bx = PX+34, by = PY+13, bw = 124, bh = 6`, fill
  `bw*vol/100`, percent right-aligned. Called at `:3531` with `g_volume`. The plate rect is pinned
  by `tests/hw_mmio/lcd_present_test.c:759-771` (3200 words) — anything drawn must stay inside it.
- `:6368-6397` Settings wheel: `if (g_set_editing && slider)` → `settings_adjust` → on "moved"
  `settings_apply(); settings_touch()`. `:6399-6405` SELECT on a slider row toggles
  `g_set_editing`; a non-slider row goes through `settings_activate`.
- `:6455-6458` Reset: `settings_defaults → settings_apply → settings_touch`.
- `:5529-5530` boot: defaults then `config_load`; `:5570` `g_volume = hal_volume_get()` (codec
  default 70); `:5608` `settings_apply()` overrides it from the record. `:4627/4666/4689` the
  resume restore mutes with `hal_volume_set(0)` and puts `g_volume` back.
- Every other `hal_volume_set(g_volume)` (`:6020, 6025, 6107, 6164, 6217, 6261, 6354`) re-applies
  the same `g_volume` over a codec re-init — all inherit the clamp automatically.

**HAL** — `core/hal/hw/volume.c:74-77` cached `g_percent/g_balance/g_bass_db/g_treble_db`;
`:180-206` `eq_gain_code` (code = 12 − dB) and `hal_tone_set`: writes EQ1 = `dac | 0x20 | code`,
EQ2..EQ4 = `0x00C`, EQ5 = `0x20 | code`, with `EQ_DAC_MODE` (bit 8 of EQ1) only when bass or
treble ≠ 0 so flat is bit-identical to no EQ; `:232-236` `hal_codec_restore` = `volume_latch()` +
`hal_tone_set(cached)`, run from `wm8758_init` via the hook (`hal/hw/wm8758.c:283-285`,
registered at `hal/hw/audio.c:332,718`). `hal/hw/wm8758.c:230-231` `init_seq_c` writes
`LDACVOL = 0xFF`, `RDACVOL = 0x1FF` (full scale, DACVU on the right write).
`core/hal/hw/wm8758.h:42-46, 148-156` EQ register numbers 0x12..0x16 and the constants
`EQ_GAIN_MASK 0x1F`, `EQ_GAIN_0DB 0x0C`, `EQ_DAC_MODE 0x100`, `EQ1_CUTOFF_105HZ 0x20`,
`EQ5_CUTOFF_6K9 0x20`. `core/docs/hw/05-audio.md:351-392` documents EQ1C/EQ5C and the 12−dB
code; the mid-band centres and the BW bit are **not** documented yet. `:205-221` DAC digital
volume: 8-bit, 0xFF = 0 dB, latched by DACVU on the right write.

**Renderer** — `core/ui/screen_settings.c:116-129` `draw_slider`; `:167-215` `list_render`
(mixed kinds already handled — Display has SELECT + SLIDER); `LIST_ROWS 8`, `ROW_H 24`,
`LIST_Y0 42` (`ui/chrome.h:80-82`), so 6 Sound rows fit without scrolling.

**Tests today** — `tests/ui/settings_test.c:90-111, 223, 286-289, 366-383` address Sound rows by
index (Bass 1, Treble 2, Balance 3, count 4). `tests/kernel/config_test.c:473-476` `T_LEN_V2Q 44`,
`:1279-1287` the host fixture must say length 44. `tests/hw_mmio/volume_trace_test.c` covers the
percent map and the OUT1 pair; **no tone/EQ coverage exists**. `tests/hw_audio/audio_test_stubs.c:42`
stubs `hal_codec_restore`, so the audio golden does not see the EQ writes.

**Host encoders** — `tools/make_config.py:79-135` fields, `PAYLOAD_LEN = 44`; the meson
`corecfg-fixture` (`tests/meson.build:527-533`) regenerates it. `core/cli/internal/devicefs/
config.go:44-46` `cfgPayloadLen = 44`, `:83-96` `DefaultSettings`, `config_test.go:31-40` a 56-byte
golden head + CRC, `settings_defaults_test.go:45-62` a want-list grepped from the C.

**Gallery** — `docs/screens/render.py:914-928` `volume_overlay(sc, vol)`, `:932-933`
`screen_volume` (vol 78), `:1719-1745` `SOUND_ROWS`/`screen_sound` (four slider rows),
`:1389-1405` `gif_settings` (drives `rows[0]`), outputs at `:2015` (`volume.png`) and `:2029`
(`sound.png`).

**Design reference** — `design_reference/system-screens.jsx:133-200` has a 5-slider `Equalizer`
screen (a *custom* EQ editor). Out of scope: presets need no editor; the row-value pattern of
`menus.jsx` SettingsSound is what the Sound screen follows.

## Design

### Sound screen rows (final order)

| idx | label | kind | value text | bar |
|---|---|---|---|---|
| 0 | Volume | SLIDER | `NN%` | volume/100 (clamped to the limit) |
| 1 | Volume Limit | SLIDER | `NN%` | limit/100 |
| 2 | EQ | SELECT | preset name | — |
| 3 | Bass | SLIDER (locked while EQ ≠ Off) | `±N dB` (preset's band-1 gain when locked) | (v+12)/24 |
| 4 | Treble | SLIDER (locked while EQ ≠ Off) | `±N dB` (preset's band-5 gain when locked) | (v+12)/24 |
| 5 | Balance | SLIDER | as today | as today |

Volume Limit sits under Volume because it is the same bar; EQ sits above Bass/Treble because it
owns them. Existing tests that index Bass/Treble/Balance as 1/2/3 move to 3/4/5.

### Volume Limit

- `settings_t` gains `int volume_limit; /* 10..100, 100 = no limit */`. Default 100.
- New pure helper in `ui/settings.c`:
  `int settings_volume_clamp(const settings_t *s, int v)` → `clampi(v, 0, s->volume_limit)`.
  Single source of the rule; `settings_adjust` (Volume row) and main.c's wheel path both call it.
- `settings_adjust(SOUND, 1, delta)`: `nv = clampi(old+delta, 10, 100)`; `s->volume_limit = nv`;
  **if `s->volume > nv` then `s->volume = nv`** (the "clamped down at once"); return `nv != old`.
  main.c's existing `settings_apply()` on "moved" pushes the lowered volume to the codec.
- Floor is 10, not 0: a limit of 0 would be a mute switch nobody can find. Reset Settings →
  defaults → 100.
- Now Playing wheel (`main.c:6318-6321` becomes):
  `int nv = settings_volume_clamp(&g_settings, g_volume + step);` — the overlay is still armed
  and presented even when `nv == g_volume` (so the marker explains why the wheel stopped), but
  `hal_volume_set` / `g_settings.volume` / `settings_touch()` only happen when it moved (this also
  ends the rail-pinned touches that `:6323` does today; a change, but strictly fewer disk writes).
- Codec side: no change. The limit is policy above the HAL. Note for the guide: the firmware never
  enables OUT2 (line-out, `PWRMGMT3` has no `LOUT2EN/ROUT2EN` in `init_seq_a`), so there is no
  unlimited path around the headphone amp.
- Overlay marker — `volume_overlay_render(int vol, int limit)`; after the fill, when `limit < 100`:
  ```c
  int mx = bx + bw * limit / 100;              /* the ceiling on the 0..100 track */
  console_fill_rect(mx - 2, by - 4, 5, 1, LINEN_INK);   /* 5x3 triangle, apex down */
  console_fill_rect(mx - 1, by - 3, 3, 1, LINEN_INK);
  console_fill_rect(mx,     by - 2, 1, 1, LINEN_INK);
  ```
  `by = PY+13`, so the rows are PY+9..PY+11 — inside the 32-px plate; x ∈ [bx−2, bx+bw+2] ⊂ plate.
  The fill can never pass the marker because `vol <= limit` by construction. The lcd_present rect
  test is untouched. `render.py` `volume_overlay(sc, vol, limit=100)` mirrors the three rects.
- Record: `P_VOL_LIMIT = CFG_PAYLOAD_V2Q + 0` (payload 44, u8). Encode `clampi(limit, 10, 100)`.
  Decode (gated on `len >= CFG_PAYLOAD_V2S`): **0 → 100** (unset — a file whose new bytes are
  zero must not pin a user at 10%), else `clampi(v, 10, 100)`; then `if (s->volume >
  s->volume_limit) s->volume = s->volume_limit;`. Records shorter than 48 → 100.

### EQ presets — option (a), the codec's 5-band EQ

**Why (a).** `hal_tone_set` already writes all five EQ registers once per track through the
restore hook; a preset is the same five writes with different values plus two DAC-volume
writes whose grammar `init_seq_c` already uses. The extension is contained in `volume.c`, is
asserted byte-for-byte by the mock bus, and needs no new bus path or power state. Option (b)
(bass/treble pairs) would give 17 names for what is really 3–4 distinguishable sounds.

**Register facts** (WM8758B R18–R22; the two shelves are already in `05-audio.md:367-378`):

| reg | addr | bit 8 | bits 6:5 (`EQxC`) | bits 4:0 |
|---|---|---|---|---|
| EQ1 | 0x12 | `EQ3DMODE` 1 = EQ on the DAC path | low shelf: 00=80, 01=105, 10=135, 11=175 Hz | gain code 12−dB |
| EQ2 | 0x13 | `EQ2BW` 0 wide / 1 narrow | peak: 00=230, 01=300, 10=385, 11=500 Hz | gain code |
| EQ3 | 0x14 | `EQ3BW` | peak: 00=650, 01=850, 10=1100, 11=1400 Hz | gain code |
| EQ4 | 0x15 | `EQ4BW` | peak: 00=1800, 01=2400, 10=3200, 11=4100 Hz | gain code |
| EQ5 | 0x16 | unused | high shelf: 00=5300, 01=6900, 10=9000, 11=11700 Hz | gain code |

Gain code = `12 − dB`, 0x00 = +12 … 0x0C = 0 … 0x18 = −12; 0x19..0x1F reserved. The EQ2–EQ4
centre tables and the BW polarity are **transcribed from memory of the datasheet, not from the
repo** — they must be checked against the WM8758B PDF before they land in `wm8758.h` and
`05-audio.md` (Risks).

**Pre-cut.** A boost in the digital EQ can clip a full-scale track. Every curve is applied with
the DAC digital volume set to `0 dB − max(0, max band gain)`: `LDACVOL = 0xFF − 2·precut`,
`RDACVOL = DACVU | (0xFF − 2·precut)` (0.5 dB/step, 0xFF = 0 dB; `05-audio.md:210-214`). This
applies uniformly, **including the Off + Bass/Treble path** (today a +12 dB bass has no pre-cut;
that is an existing exposure this closes — audible as "Bass +6 is a little quieter overall",
which is correct). A flat curve pre-cuts 0 dB → `0xFF/0x1FF`, byte-identical to `init_seq_c`.

**Path routing.** `EQ_DAC_MODE` is set on EQ1 iff any band ≠ 0 (today's rule kept), so Off with
flat tone still leaves the EQ on the inert ADC path and playback is bit-identical to today.

**Default centres** (used by every preset unless noted): B1 shelf 105 Hz (code 01, the tone
control's corner), B2 300 Hz (01), B3 1.1 kHz (10), B4 3.2 kHz (10), B5 shelf 6.9 kHz (01); all
peaks wide (BW 0). The Off curve writes EQ2..EQ4 with centre code 00 so its words are exactly the
ones `hal_tone_set` emits today (`0x00C`) — a regression pin, inaudible either way at 0 dB.

**Preset table** — gains in dB (B1..B5), then the resolved 9-bit words and the DACVOL pair.
These numbers are this project's own; nothing is copied from iTunes or any other firmware.

| # | name | B1 | B2 | B3 | B4 | B5 | EQ1 | EQ2 | EQ3 | EQ4 | EQ5 | precut | LDACVOL / RDACVOL |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | Off (tone b/t) | b | 0 | 0 | 0 | t | `(b\|\|t ? 0x100:0)\|0x20\|(12−b)` | 0x00C | 0x00C | 0x00C | `0x20\|(12−t)` | max(0,b,t) | 0xFF−2p / 0x100\|… |
| 1 | Acoustic | +4 | +2 | 0 | +2 | +3 | 0x128 | 0x02A | 0x04C | 0x04A | 0x029 | 4 | 0xF7 / 0x1F7 |
| 2 | Bass Booster | +6 | +3 | 0 | 0 | 0 | 0x126 | 0x029 | 0x04C | 0x04C | 0x02C | 6 | 0xF3 / 0x1F3 |
| 3 | Bass Reducer | −6 | −3 | 0 | 0 | 0 | 0x132 | 0x02F | 0x04C | 0x04C | 0x02C | 0 | 0xFF / 0x1FF |
| 4 | Classical | +4 | +2 | −2 | −2 | +3 | 0x128 | 0x02A | 0x04E | 0x04E | 0x029 | 4 | 0xF7 / 0x1F7 |
| 5 | Dance | +5 | +2 | 0 | +3 | +4 | 0x127 | 0x02A | 0x04C | 0x049 | 0x028 | 5 | 0xF5 / 0x1F5 |
| 6 | Electronic | +5 | +1 | −2 | +2 | +4 | 0x127 | 0x02B | 0x04E | 0x04A | 0x028 | 5 | 0xF5 / 0x1F5 |
| 7 | Hip-Hop | +6 | +3 | 0 | +1 | +3 | 0x126 | 0x029 | 0x04C | 0x04B | 0x029 | 6 | 0xF3 / 0x1F3 |
| 8 | Jazz | +3 | 0 | −1 | +2 | +3 | 0x129 | 0x02C | 0x04D | 0x04A | 0x029 | 3 | 0xF9 / 0x1F9 |
| 9 | Loudness | +7 | +2 | −2 | 0 | +5 | 0x125 | 0x02A | 0x04E | 0x04C | 0x027 | 7 | 0xF1 / 0x1F1 |
| 10 | Pop | −1 | +2 | +4 | +2 | −1 | 0x12D | 0x02A | 0x048 | 0x04A | 0x02D | 4 | 0xF7 / 0x1F7 |
| 11 | R&B | +5 | +3 | −1 | +1 | +3 | 0x127 | 0x029 | 0x04D | 0x04B | 0x029 | 5 | 0xF5 / 0x1F5 |
| 12 | Rock | +5 | +2 | −1 | +2 | +4 | 0x127 | 0x02A | 0x04D | 0x04A | 0x028 | 5 | 0xF5 / 0x1F5 |
| 13 | Small Speakers (B1 @ 175 Hz, code 11) | +5 | +2 | 0 | +2 | +3 | 0x167 | 0x02A | 0x04C | 0x04A | 0x029 | 5 | 0xF5 / 0x1F5 |
| 14 | Spoken Word (B4 @ 2.4 kHz, code 01) | −3 | +1 | +3 | +4 | +1 | 0x12F | 0x02B | 0x049 | 0x028 | 0x02B | 4 | 0xF7 / 0x1F7 |
| 15 | Treble Booster | 0 | 0 | 0 | +2 | +6 | 0x12C | 0x02C | 0x04C | 0x04A | 0x026 | 6 | 0xF3 / 0x1F3 |
| 16 | Treble Reducer | 0 | 0 | 0 | −2 | −6 | 0x12C | 0x02C | 0x04C | 0x04E | 0x032 | 0 | 0xFF / 0x1FF |
| 17 | Vocal Booster | −2 | +1 | +4 | +4 | 0 | 0x12E | 0x02B | 0x048 | 0x048 | 0x02C | 4 | 0xF7 / 0x1F7 |

Word formulas (what the test derives independently): `EQ1 = 0x100 | (C1<<5) | (12−g1)` (the
0x100 only when any band ≠ 0), `EQ2..4 = (BW<<8) | (C<<5) | (12−g)`, `EQ5 = (C5<<5) | (12−g5)`.
I2C framing per `wm8758_write`: byte0 = `(reg<<1) | bit8`, byte1 = low byte — e.g. Bass Booster
EQ1 0x126 → `0x25 0x26`; LDACVOL 0xF3 → `0x16 0xF3`; RDACVOL 0x1F3 → `0x19 0xF3`.

Write order in `hal_eq_set`: LDACVOL, RDACVOL(+DACVU) **first** (attenuate before boosting), then
EQ1..EQ5. Nothing here has a zero-cross latch; see Risks.

**Locked Bass/Treble — decision and justification.** The silicon has exactly one low shelf and
one high shelf; a preset owns both. Of the two options offered, "disabled while a preset is
active" was chosen over "sliders become the preset's values and editing them switches EQ to Off":
- no state is lost — `s->bass/s->treble` keep the user's own tone and return with Off; the other
  option either overwrites them or needs a "Custom" preset carrying three mid gains the UI has no
  control for;
- no audible surprise — nudging Bass under "Rock" would otherwise drop the mid-band curve
  mid-song;
- it keeps `settings_activate` a one-field mutation and the record a one-byte addition.
A summing model (preset + user shelf) was also rejected: the register saturates at ±12 while the
slider would keep moving, and the shown number would not be the shelf's gain.
Implementation: `int settings_row_locked(int screen, const settings_t *s, int idx)` — 1 for Sound
rows 3/4 while `s->eq != 0`. `settings_adjust` returns 0 for a locked row; `settings_value` shows
the preset's band-1/band-5 gain there; `list_render` paints a locked row's label and value in
`S_MUTED2` (`S_SEL_SUB` when selected) and its fill in `S_MUTED2`; main.c refuses edit mode on a
locked row (SELECT does nothing but the click). That is the one place a visible row "does
nothing" — it is explaining state, and the guide says so.

**EQ row** is a SELECT: SELECT cycles `s->eq = (s->eq + 1) % EQ_PRESET_COUNT`, always a change
(NONE). 18 values is a long cycle; a Clicker-style picker screen is the better UI but needs the
MENU-from-a-nested-screen handling in main.c that does not exist (`:6471-6480` only knows
ROOT/non-ROOT). Follow-up, not this plan.

**Where the table lives.** New pure module `core/ui/eq.h/.c` (host-built, no hardware):
```c
#define EQ_BANDS         5
#define EQ_PRESET_COUNT  18                /* Off + 17 */
#define EQ_OFF           0
typedef struct { int8_t gain_db[5]; uint8_t cutoff[5]; uint8_t narrow[5]; } eq_curve_t;
const char *eq_preset_name(int i);                       /* "Off" for out of range */
void eq_preset_curve(int i, eq_curve_t *out);            /* Off = flat, tone corners */
void eq_effective_curve(int preset, int bass_db, int treble_db, eq_curve_t *out);
                                                          /* preset != Off ? preset : tone */
int  eq_precut_db(const eq_curve_t *c);                   /* max(0, max gain) */
```
`settings.c` includes `eq.h` for names and the locked rows' displayed gains (both pure). The HAL
takes plain arrays so no ui type crosses into hal/:
```c
/* hal/hw/volume.h */
void hal_eq_set(const int8_t gain_db[5], const uint8_t cutoff[5], const uint8_t narrow[5]);
```
`hal_tone_set(b, t)` stays as a two-line wrapper (flat mids, code-00 centres) so its documented
words are unchanged; `hal_codec_restore` replays the cached 15 bytes + pre-cut through
`hal_eq_set`. `settings_apply` becomes:
```c
eq_curve_t c;
eq_effective_curve(g_settings.eq, g_settings.bass, g_settings.treble, &c);
hal_eq_set(c.gain_db, c.cutoff, c.narrow);
```
- Record: `P_EQ = CFG_PAYLOAD_V2Q + 1` (payload 45, u8), `P_SND_PAD = +2` (u16, 0),
  `CFG_PAYLOAD_V2S = 48`. Encode: `eq < EQ_PRESET_COUNT ? eq : 0`. Decode: same rule (an unknown
  preset from a newer build → Off, the theme's "unknown → Linen" precedent, `config.c:395-400`).

### Record layout after this plan

```
payload off  size  field           rule
44           u8    volume_limit    10..100; 0 = unset → 100; volume clamped to it on decode
45           u8    eq              0..17; unknown → 0 (Off)
46           u16   reserved        0
length 44 → 48, CONFIG_VERSION unchanged (2)
```

## Files to change

Firmware (`core/`):
- `ui/settings.h` — `volume_limit`, `eq` fields (+ comments); prototypes `settings_volume_clamp`,
  `settings_row_locked`, `settings_eq_name`; fix the stale "COSMETIC" note on bass/treble.
- `ui/settings.c` — `SOUND_L[6]`, count 6, kind (idx 2 SELECT), value, activate (idx 2 cycles),
  adjust (rows 0/1/3/4/5 with the limit coupling and the lock), defaults (100 / 0), the two
  helpers; `#include "eq.h"`.
- `ui/eq.h`, `ui/eq.c` — NEW: names, the preset table, `eq_effective_curve`, `eq_precut_db`.
- `ui/screen_settings.c` — `list_render`: locked look for slider rows (`settings_row_locked`).
- `kernel/config.h` — versioning comment (44 → 48 under v2); `kernel/config.c` — offsets,
  encode, decode (+ the volume ≤ limit coupling).
- `kernel/main.c` — `settings_apply` (`hal_eq_set`), wheel clamp (`:6318-6323`),
  `volume_overlay_render(vol, limit)` + call at `:3531`, locked check before `g_set_editing = 1`
  (`:6399-6402`). `#include "../ui/eq.h"`.
- `hal/hw/volume.h`, `hal/hw/volume.c` — `hal_eq_set`, cached curve + precut, `hal_codec_restore`
  replays it, `hal_tone_set` as wrapper.
- `hal/hw/wm8758.h` — `EQ_BW_NARROW 0x100`, `EQ_CUTOFF_SHIFT 5`, `EQ_CUTOFF_MASK 0x60`,
  `DACVOL_0DB 0xFF` (name the existing 0xFF in `init_seq_c` with it), comments for the centre
  tables.
- `meson.build` (device sources: `ui/eq.c`) and `tests/meson.build` (settings_test + a new
  eq_test link `../ui/eq.c`; volume_trace_test unchanged sources).
Host side:
- `tools/make_config.py` — `SOUND2_FIELDS` at 44/45/46 (`volume_limit` 100, `eq` 0, pad),
  `PAYLOAD_LEN = 48`, decode/verify print the new fields.
- `core/cli/internal/devicefs/config.go` — `cfgPayloadLen = 48`, `Settings.VolumeLimit/EQ`,
  `DefaultSettings` 100/0, encode/decode the two bytes; `config_test.go` golden head → 60 bytes
  with `3000` length and `64 00 00 00` at record offsets 56..59, CRC regenerated with the
  documented `make_config.py --emit` command; `settings_defaults_test.go` want-list + the two
  fields.
Docs/gallery: see below.

## Tests to add

- `tests/ui/settings_test.c` (extend): defaults `volume_limit == 100 && eq == 0`;
  `count-sound == 6`; labels by index (Volume Limit at 1, EQ at 2, Balance at 5) so a reorder
  cannot pass by renumbering; kind: idx 2 SELECT, others SLIDER; Volume adjust clamps at the limit
  (`limit 40, volume 38, +5 → 40`, rail returns 0); lowering the limit below the volume pulls
  volume down in the same call; limit rails 10/100; `settings_volume_clamp` for v < 0, v > limit,
  limit 100; EQ activate cycles 0→1→…→17→0 returning NONE each time and mutating exactly `eq`
  (memcmp against a copy with only `eq` patched); `settings_value(SOUND, 2)` names match
  `eq_preset_name`; lock: with `eq = 2` (Bass Booster) `settings_adjust(SOUND, 3, +1) == 0`,
  `s->bass` untouched, value text on row 3 is `+6 dB` and num/den (18, 24), row 4 `0 dB`;
  `settings_row_locked` 0 for every row at Off and 1 only for rows 3/4 otherwise; back to Off the
  user's bass text returns; NOOP/NONE invariant for the new row; re-index the existing 1..3 loop
  to 3..5.
- `tests/ui/eq_test.c` (NEW, links `ui/eq.c`): count 18; every name non-empty and unique; Off
  curve all-zero gains with cutoffs {1,0,0,0,1}; every gain in [−12, 12]; every cutoff ≤ 3;
  `narrow` only ever set on bands 1..3 (never the shelves); `eq_precut_db` = max positive gain for
  every preset and 0 for Bass Reducer / Treble Reducer / Off; `eq_effective_curve(Off, +3, −2)`
  → gains {3,0,0,0,−2}; `eq_effective_curve(Rock, +3, −2)` ignores the tone args; out-of-range
  preset → Off. The table of the plan (gains) is the fixture.
- `tests/hw_mmio/volume_trace_test.c` (extend): `hal_eq_set` for a flat curve emits exactly the
  seven writes today's `hal_tone_set(0,0)` + `init_seq_c`'s DACVOL pair imply — `0x16 0xFF`,
  `0x19 0xFF`, then EQ1..EQ5 `0x24 0x2C`, `0x26 0x0C`, `0x28 0x0C`, `0x2A 0x0C`, `0x2C 0x2C`
  (regression pin: Off is bit-identical to the shipped tone path); Bass Booster emits
  `0x16 0xF3`, `0x19 0xF3`, `0x25 0x26`, `0x26 0x29`, `0x28 0x4C`, `0x2A 0x4C`, `0x2C 0x2C`;
  a ±12 clamp on an out-of-range gain; `EQ3DMODE` set iff any band ≠ 0; `hal_codec_restore` after
  `hal_eq_set(Rock)` replays the same seven values; `hal_tone_set(+3, −2)` still equals the old
  words plus a 3 dB precut. Values are derived in the test from the formulas above, not pasted.
- `tests/kernel/config_test.c` (extend): `T_LEN_V2S 48`; encode declares 48; a 44-byte v2 record
  decodes with limit 100 / eq 0 and its volume untouched; a 48-byte record with volume 90 and
  limit 60 decodes to volume 60; limit byte 0 → 100, 5 → 10, 250 → 100; eq byte 17 → 17,
  18 → 0, 200 → 0; host fixture `blob[6] == 48`, `s.volume_limit == 100`, `s.eq == 0`, and the
  "across the board" defaults check gains the two fields.
- Go: `config_test.go` golden (regenerated, documented command) and a round-trip of
  `VolumeLimit: 40, EQ: 12`; `settings_defaults_test.go` want-list.
- `docs/screens/render.py` is exercised by the release stills (no assertion; the diff is the
  review).

Suites that must stay green untouched: `hw-audio` (restore is stubbed), `lcd-present` (plate rect
unchanged), `clicky` (no UART line changes), `cfg_commit`, `player`.

## Docs to update

- `docs/USER_GUIDE.md` — Settings "Sound." bullet: Volume, Volume Limit (what it does, the
  triangle, the 10% floor, no combination), EQ (the 18 values, "Bass and Treble are greyed while
  a preset is on; set EQ to Off to use them"), Bass/Treble/Balance; Now Playing "Volume."
  paragraph: the marker; add `screens/volume_limit.png` beside `volume.png` in that table.
- `README.md` — Settings paragraph: "Playback, Sound (volume limit, EQ presets, tone), …" — one
  clause; the gallery table can stay.
- `core/docs/hw/05-audio.md` — "WM8758 tone controls" section: EQ2–EQ4 centre tables, the BW bit,
  `EQ_BW_NARROW`, the DACVOL pre-cut rule and the write order; the constants table for the new
  names; a line that the preset gains are `ui/eq.c`'s and the register policy is `hal_eq_set`'s.
  Mark the mid-band tables "verify against the datasheet" until someone has.
- `core/docs/design/settings-persistence.md` — layout table +2 fields, length 48 under v2.
- `core/README.md` "Settings and resume" — one sentence: v2 now 48 bytes (locator, context,
  sound tail).
- `STATUS.md` — a dated entry under the 2026-09-13 section in the house style, ending
  **UNFLASHED**, with the bench list from Acceptance criteria.
- `CHANGELOG.md` — an Unreleased section (or the next tag's) with the two features.
- `docs/screens/render.py` — `SOUND_ROWS` → six rows with kind + locked flag (Sound still shows a
  preset active, e.g. `EQ Rock` with Bass/Treble greyed at +5/+4), `volume_overlay(limit=)`,
  new `screen_volume_limit()` → `volume_limit.png` (vol 60 at limit 60), `gif_settings` keeps
  driving `rows[0]`. Regenerate `sound.png`, `settings.gif`, `volume_limit.png`.
- `tools/make_config.py` docstring/field comments; `core/cli/internal/devicefs/config.go` header
  comment (length 48).

## Acceptance criteria

Host (`cd core && make sim && meson test -C build-sim`, `make hw && make verify-hw`, `cd core/cli
&& go test ./...`): all green, ARM `-Werror` clean, plus every test listed above.

Behavioural (host-provable):
1. `settings_adjust` can never leave `volume > volume_limit`, from either row.
2. A 44-byte record on a device today decodes with limit 100 / EQ Off and no other field changed.
3. The Off + flat path emits byte-identical codec writes to the shipped build (the mock-bus pin).
4. Every preset's resolved words match the table in this plan.
5. The volume plate paints nothing outside `VOL_PLATE_*` (lcd_present test unchanged and green).

Device bench list (cannot be run in this job; goes to STATUS.md as UNFLASHED):
- (a) Set Volume Limit 40 while playing at 70: volume drops to 40 at once, plate shows 40 with the
  triangle at 40%; the wheel cannot pass it; Menu out and back, power-cycle: limit persists.
- (b) Limit back to 100: triangle gone, wheel reaches 100.
- (c) EQ Bass Booster on a bass-heavy full-scale FLAC: audibly more bass, **no clipping/crackle**
  (the pre-cut's job), overall level a touch lower than Off; Bass/Treble rows greyed at +6/0 and
  the wheel does not move them; Off restores the user's own tone.
- (d) Cycle every preset while playing: no pop, no dropout at the EQ path switch (Off→preset is
  the `EQ3DMODE` ADC→DAC flip); Treble Reducer / Bass Reducer are audibly cuts.
- (e) Track change and pause/resume keep the preset (the restore hook replays it).
- (f) Reset Settings: limit 100, EQ Off, saved.

## Risks

- **EQ register writes are device-only.** The host test proves the I2C byte sequence
  (`volume_trace_test` over the mock bus) and that the restore hook replays it; it cannot prove
  the chip sounds right. Specifically unverifiable off-device: (1) the EQ2–EQ4 centre-frequency
  and BW-bit tables above come from memory of the WM8758B datasheet — check the PDF before they
  enter `wm8758.h`/`05-audio.md`; a wrong code is a wrong centre, not a fault; (2) whether the
  EQ's internal arithmetic saturates before the DAC-volume stage — if the datasheet's signal
  order puts the digital volume *after* the EQ, the pre-cut protects the DAC input but not the EQ
  accumulator, and (c) above is the test; (3) pop/zipper on the `EQ3DMODE` ADC→DAC flip and on
  gain steps (no zero-cross on these registers) — mitigated by writing DACVOL first; if audible,
  soft-mute around `hal_eq_set` (DACCTRL SOFTMUTE, already in `wm8758_mute`) is the fallback,
  costing a brief dip on each preset change.
- **Loudness drop from the pre-cut on the existing tone path.** Bass +6 today plays 6 dB hotter
  in the lows than it will after this — a deliberate correction, but a change the guide/STATUS
  must state.
- **Record tail contention.** Offsets 44..47 are the next free payload bytes; any other plan
  appending under v2 collides (see Conflict surface). Offsets are expressed relative to
  `CFG_PAYLOAD_V2Q` so a rebase is one constant per file — but the **Go golden and the Python
  fixture both change bytes** and must be regenerated together, in one commit, with the C.
- **Three encoders.** `config.c`, `make_config.py`, `config.go` and their three goldens must move
  in one commit; a partial landing makes `core sync`-created files disagree with the firmware's
  defaults (the exact 2026-07-27 Resume bug class).
- **Volume Limit is not a safety certification.** It caps the OUT1 amp code the firmware writes;
  a pre-cut, a different headphone, or an EQ boost all move the actual SPL. Say so in the guide
  in one sentence.
- **Cycle length.** 18 SELECT presses round-trip is clumsy; noted as a picker follow-up, not a
  blocker.

## Conflict surface

- `core/ui/settings.h` (`settings_t` fields) and `core/ui/settings.c` (`SOUND_L`, count/kind/
  value/activate/adjust/defaults) — any plan adding a Settings row or field touches the same
  tables; Sound indices 1..3 → 3..5 change every test that names them.
- `core/kernel/config.c` payload tail at 44 and `config.h`'s versioning comment; `tools/
  make_config.py` `PAYLOAD_LEN`; `core/cli/internal/devicefs/config.go` + both goldens +
  `settings_defaults_test.go` — anything else persisting a new byte collides here; first to land
  takes 44..47, the other rebases to `CFG_PAYLOAD_V2S`.
- `core/kernel/main.c` `settings_apply` (`:2871-2880`), the Now Playing wheel block
  (`:6302-6327`), `volume_overlay_render` (`:3337-3358`) and its call (`:3531`), the Settings
  SELECT edit-mode toggle (`:6399-6405`) — the merge hot spot; the pause-on-unplug and any
  Now Playing plan will be near `:6300`.
- `core/hal/hw/volume.c/.h` (cached state, `hal_codec_restore`), `wm8758.h` constants — a
  headphone/line-out or suspend plan that touches codec state collides.
- `core/ui/screen_settings.c` `list_render` — a Settings visual plan collides.
- `tests/hw_mmio/volume_trace_test.c`, `tests/kernel/config_test.c`, `tests/ui/settings_test.c`,
  `tests/meson.build` (new `eq_test`, settings_test sources).
- `docs/screens/render.py` Sound/volume painters and outputs; `docs/USER_GUIDE.md` Settings +
  Now Playing sections; `STATUS.md` top section; `CHANGELOG.md`; `core/docs/hw/05-audio.md` EQ
  section.
