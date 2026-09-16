# 11 — Pause on headphone unplug

Repo: `/home/brando/Projects/ipod_theme` (main @ dbce3b8). All paths below are under `core/` unless
they start with `docs/`, `STATUS.md`, `README.md`.

## Summary

The hardware half of this feature already exists and is host-tested: `hal/hw/headphone.c` reads
GPIO A7, debounces it for 200 ms, and answers -1 while `HEADPHONE_DETECT_TRUSTED` is 0 (the
shipping default). What is missing is (a) the *policy* — today an inline edge detector in
`kernel/main.c` with a file-scope `g_hp_last`, untested, (b) a way to learn the pin's polarity
**without a serial cable** — the only probe that exists prints to SER0, and the owner ruled a
serial cable out on 2026-07-17 (memory `device_bringup.md`), so the UART probe can never run, and
(c) the docs.

The plan:

1. Pull the policy out of `main.c` into a pure module `ui/jackwatch.c` (`jackwatch_feed(level,
   playing, now_us) -> JACKWATCH_NONE | JACKWATCH_PAUSE`), unit-tested under `tests/ui/`, and
   leave `main.c` with thin wiring: feed it, act on PAUSE, narrate.
2. Put the probe **on the About screen**: the footer that already reads `ADC 2731 · LOG 6 on`
   gains a live `JACK 1` token (raw A7 level, plus `en`/`oe` when the pin is not configured as a
   GPIO input). It is drawn in every build, trusted or not, because `headphone_raw()` is one
   32-bit read of the same register the Hold switch reads every loop pass. The owner opens About,
   plugs and unplugs, reads the digit. One flash, no cable.
3. Keep **one switch**: `HEADPHONE_DETECT_TRUSTED` in `hal/hw/headphone.h` (already 0). Polarity
   stays the existing single `#define HEADPHONE_DETECT_ACTIVE_LOW`. The probe result sets those
   two lines and nothing else. No settings-record change (rationale under Design).
4. Behaviours, all encoded in the pure module and its tests: unplug while playing -> pause (resume
   position captured by the loop's existing pause-flip capture); plug in -> never auto-resume;
   unplug while paused/idle -> nothing; Hold on / charging modal / any screen -> still pauses; -1
   never pauses; a boot with nothing in the jack never pauses; the suspend/wake path re-primes
   the module from the live level so a pull during sleep is neither a phantom pause nor a resume.

Nothing here can be verified on the device in this job; every "on device" line is owed to the
bench and is marked UNFLASHED in STATUS.md.

## Current behaviour (file:line)

**HAL contract** — `hal/hal.h:333-356`. `hal_headphones_present()`: 1 seated / 0 absent / -1
unknown, debounced 200 ms, "poll it from the main loop (hw: two register reads)", "a caller must
treat -1 as do nothing". Sim: fixed at 1 unless `CORE_SIM_HEADPHONES=0`.

**hw driver** — `hal/hw/headphone.h`, `hal/hw/headphone.c`.
- Knobs (`headphone.h:35-55`): `HEADPHONE_DETECT_ADDR 0x6000D030`, `HEADPHONE_DETECT_BIT 0x80`,
  `HEADPHONE_DETECT_ACTIVE_LOW 0`, `HEADPHONE_DETECT_TRUSTED 0`, `HEADPHONE_PROBE 0`,
  `HEADPHONE_DEBOUNCE_US 200000`. All `#ifndef`-guarded, so `-D` overrides work.
- `headphone_debounce_feed()` (`headphone.c:41-70`): pure debouncer, first sample primes at once,
  return-to-old-level cancels the candidate, wrap-safe.
- `headphone_raw()` (`headphone.c:76-84`): one `mmio_read32` of GPIOA_INPUT_VAL, bit 7, polarity
  applied. Available in every build.
- `hal_headphones_present()` (`headphone.c:91-108`): `#if TRUSTED` -> USEC_TIMER read, then pin
  read, then debounce; `#else` -> `return -1` with **no bus traffic** (asserted by the untrusted
  test binary).
- The UART probe (`headphone.c:112-end`, `-DHEADPHONE_PROBE=1`): dumps all twelve GPIO ports to
  SER0 on change. **Unusable here: no serial cable.**

**sim** — `hal/sim/headphone_sim.c`: env-driven constant, `sim_headphones_set()` for tests, no
debounce. Not linked by any executable (`core/README.md`, "make sim is the test build").

**main.c wiring** (all `kernel/main.c`):
- `632-635`: `static int g_hp_last = -1;` "starts unknown so a boot with nothing in the jack
  cannot look like a pull-out".
- `5774-5797` (in `run_ui`, before the charging-modal edge, **outside** the Hold-locked branch):
  ```c
  int hp = hal_headphones_present();
  if (hp == 0 && g_hp_last == 1 && player_active() && !player_paused()) {
      player_pause();
  }
  if (hp >= 0) {
      g_hp_last = hp;
  }
  ```
  One-directional by design (comment explains the "switch closes before the audio contacts seat"
  reason for never auto-resuming). No narration, no evlog line, no test.
- `5366-5367` (suspend loop, every `SUSPEND_IDLE_MS` = 100 ms): `(void)hal_headphones_present();`
  "keep the jack debouncer fed so the answer at wake reflects the suspend".
- `5422-5433` (wake): `if (was_playing) { if (hal_headphones_present() != 0) player_resume(); }`
  — resume only into a seated plug; -1 keeps today's behaviour (resume). **`g_hp_last` is not
  updated on wake**, so a plug pulled and re-inserted during sleep leaves `g_hp_last == 1` (fine)
  but a plug pulled during sleep leaves it 1 while `hp` is 0 -> the next loop pass sees `hp==0 &&
  g_hp_last==1`, and pauses an already-paused (or never-resumed) player — harmless today only
  because `!player_paused()` guards it.
- `5700-5712`: resume-position capture fires on any pause flip (`paus != g_resume_was_paused`)
  -> a jack pause is captured to `CORECFG.DAT` with no extra call.
- `3237-3250` `settings_render_cur()`: About is drawn by `settings_about_render(...)` with live
  values; Boot Details by `settings_diag_render(...)`. Both repaint only when `dirty` (render gate
  at `6878`). Nothing sets `dirty` for a jack change.
- `5635`, `5892-5905`: the Hold switch is a per-pass `clickwheel_hold()` GPIO read
  (`hal/hw/clickwheel.c:141-145`, polled, no IRQ) with an edge detector seeded before the loop —
  the pattern to copy.
- Loop cadence: each pass ends in a 10 ms halt when idle/paused, ~200 µs when playing
  (`6740-6745`); during suspend the poll is every 100 ms (`5384`).

**About renderer** — `ui/screen_settings.c:398-548`; footer at baseline y=236,
`ui_text_centered(236, "ADC <raw> · LOG <seq> on", F_SMALL, S_MUTED)`. Cards end at y=222
(`AB_CARD_H 80`, `:386`), so rows 224..240 are the footer's. Signature `ui/settings.h:277-281`.
Placeholder call `screen_settings.c:294`. Gallery twin `docs/screens/render.py:1084-1098`
(values) and `:1166-1169` (footer).

**Tests** — `tests/hw_mmio/headphone_trace_test.c`: one source, three binaries
(`tests/meson.build:667-715`): `hw-headphone-trace` (`-DHEADPHONE_DETECT_TRUSTED=1`: grammar =
exactly one USEC_TIMER read + one GPIOA read; decode; debounce; wrap), `hw-headphone-untrusted`
(**passes only `-DMMIO_MOCK` and relies on the header default being 0** — see Risks),
`hw-headphone-probe`. Pure-module template: `tests/ui/keyhold_test.c` + `tests/meson.build:190-196`.

**GPIO interrupts** — `docs/hw/01-soc-pp5022.md:355-357` lists `GPIO0_IRQ` (#32, ports A–D) at the
controller level; no per-pin edge-enable register grammar is documented anywhere in `docs/hw/`,
the Hold switch on the same port is polled, and the clickwheel driver's own note
(`hal/hw/clickwheel.c:9-11`) records IRQ facts "for a later IRQ path but not exercised here".
**Decision: poll.** ≤10 ms loop period against a 200 ms debounce is ~20 samples per window;
during suspend 100 ms against 200 ms is still ≥2. An IRQ would need register discovery on the
device, which is exactly what this job cannot do.

**Docs** — `docs/hw/10-headphone-jack.md` is complete on the electrical question and describes
only the UART probe; `docs/USER_GUIDE.md` has no headphone paragraph; STATUS.md mentions the
jack only in the audio-noise (VMID) entry and the suspend "skips the resume if the jack is known
empty" note.

## Design

### The one switch, and where polarity lives

- **Switch:** `HEADPHONE_DETECT_TRUSTED` (`hal/hw/headphone.h`). 0 ships (inert: the HAL answers
  -1, the module never sees an edge, no bus traffic from the debounced path). 1 after the bench.
- **Polarity:** `HEADPHONE_DETECT_ACTIVE_LOW` (same header, already exists). The probe reading
  sets it. Both remain `#ifndef`-guarded so the trace tests keep overriding them with `-D`.
- **Why a `#define` and not a settings-record byte.** A record byte would need (1) a Settings
  row to set it (the "only rows that do something" rule means a real row with a real picker),
  (2) `tools/make_config.py` + the Go `core sync` config writer to learn the field, (3) a record
  layout bump with old-record compatibility, and (4) it would persist a *wrong* polarity across
  reflashes on the one disk the firmware writes to. The bench is a two-line edit either way, and
  the pin's polarity is a property of the board, not of the user. If a second device with a
  different board revision ever shows up, that is when a record byte earns its cost.
- **Why no `HEADPHONE_PAUSE_ENABLE` toggle.** Apple's firmware has none; TRUSTED is the gate.
  A user-facing Playback -> "Pause on unplug" row is a possible follow-up, not this job.

### `ui/jackwatch.{h,c}` — the pure policy module

Modelled on `ui/keyhold.{h,c}`: freestanding C11, integers only, no clock of its own, the SAME
source the ARM build links, unit-tested on the host.

```c
/* ui/jackwatch.h */
typedef struct {
    int8_t   last;        /* last believed debounced level: -1 unknown, 0 out, 1 in */
    int8_t   raw_last;    /* last raw level seen by jackwatch_note_raw, -1 = none    */
    uint8_t  paused_by;   /* 1 from a PAUSE until the next plug-in or a resume elsewhere */
    uint16_t raw_edges;   /* raw transitions counted since reset (About + evlog)       */
    uint16_t pauses;      /* PAUSE events emitted since reset                          */
    uint32_t edge_us;     /* when the last debounced edge was seen                     */
} jackwatch_t;

typedef enum { JACKWATCH_NONE = 0, JACKWATCH_PAUSE } jackwatch_action_t;

void jackwatch_reset(jackwatch_t *j);            /* zero == "no sample yet" */

/* Feed the DEBOUNCED level (hal_headphones_present(): -1/0/1) once per pass, with
 * whether the transport is currently playing. Returns PAUSE on exactly the
 * 1 -> 0 edge while playing; NONE otherwise. */
jackwatch_action_t jackwatch_feed(jackwatch_t *j, int level, int playing, uint32_t now_us);

/* Re-prime from the live level with no action — the wake path. A level of -1
 * leaves `last` alone. */
void jackwatch_prime(jackwatch_t *j, int level);

/* Count raw transitions for the About line / the evlog probe lines. Returns 1
 * on a transition (the caller narrates and dirties About). Pure bookkeeping,
 * never an action. */
int  jackwatch_note_raw(jackwatch_t *j, int raw);
```

Rules of `jackwatch_feed` (each is one test):

| `last` | `level` | `playing` | action | `last` after |
|---|---|---|---|---|
| any | -1 | any | NONE | unchanged (never primes from -1) |
| -1 | 0 | any | NONE (boot with jack empty) | 0 |
| -1 | 1 | any | NONE (boot with jack in) | 1 |
| 1 | 0 | 1 | **PAUSE**, `paused_by=1`, `pauses++`, `edge_us=now` | 0 |
| 1 | 0 | 0 | NONE (already paused/idle) | 0 |
| 0 | 1 | any | NONE (no auto-resume), `paused_by=0` | 1 |
| x | x | any | NONE | unchanged |

`now_us` is stored only for `edge_us` (the About line can say how long ago; the evlog line
carries it); it is not used for any timing decision — the HAL owns the 200 ms debounce and the
module must not add a second window, or the two would disagree about when a pull "happened".
`playing` is the caller's `player_active() && !player_paused()` (the transport predicate the
existing code uses, not `player_playing()`, which is the power gate).

### Wiring in `kernel/main.c` (thin)

1. Replace `g_hp_last` (`632-635`) with `static jackwatch_t g_jack;` (zero-init = unknown).
2. In `run_ui` at `5774-5797`, replace the block with:
   ```c
   {
       int raw = headphone_raw();                     /* one GPIOA read, every build */
       if (jackwatch_note_raw(&g_jack, raw)) {
           jack_narrate_raw(raw);                     /* budgeted, see below */
           if (scr_cur() == SCR_SETTINGS && g_set_screen == SETTINGS_ABOUT) dirty = 1;
       }
       int hp = hal_headphones_present();             /* -1 while untrusted */
       int playing = player_active() && !player_paused();
       if (jackwatch_feed(&g_jack, hp, playing, now_us) == JACKWATCH_PAUSE) {
           player_pause();
           uart_puts("core: jack out, pause\n");      /* -> CORELOG.BIN via the evlog tap */
       }
   }
   ```
   It stays where it is: before the charging-modal edge, outside the `g_locked` branch, so Hold
   and the charging modal cannot swallow it. `now_us` is the pass's existing USEC_TIMER sample.
   The resume capture needs no call — `5700-5712` already captures on the pause flip.
3. Narration: `jack_narrate_raw()` prints `core: jack raw=<0|1> n=<edges>\n` and stops after 64
   lines per boot (a static counter), so a flapping pin cannot fill the 16 KiB RAM ring — the
   same "cannot spam" promise the UART probe makes. Debounced edges (trusted builds only) print
   unconditionally: `core: jack in` / `core: jack out, pause` / `core: jack out` (not playing).
   Every byte of `uart_puts` is already captured by `evlog_capture()` (`kernel/evlog.h`, "THE
   TAP"), so the transcript lands in `CORELOG.BIN` and `tools/make_log.py --dump` shows it —
   the post-hoc record of the bench, no cable.
4. Suspend loop (`5366-5367`): keep the `(void)hal_headphones_present();` feed as is.
5. Wake (`5422-5433`): before the resume decision, `jackwatch_prime(&g_jack,
   hal_headphones_present());` then the existing `if (was_playing && hp != 0) player_resume();`.
   Priming with no action is what makes a pull-during-sleep neither a phantom pause on the first
   loop pass nor a resume; a re-insert during sleep leaves the player paused (Apple behaviour).
6. `settings_render_cur()` (`3237-3250`): pass the jack values to `settings_about_render`.
7. Before the loop (next to `hold_prev` at `5635`): nothing — `g_jack` zero-init is the "unknown"
   seed, and the first pass primes it from the HAL's own primed first sample. Boot with the jack
   empty therefore yields `last=0`, never an edge.

### The About line (the probe)

`settings_about_render` gains one parameter, `const about_jack_t *jack` (NULL draws today's
footer, so the placeholder call at `screen_settings.c:294` and `render.py` stay simple):

```c
typedef struct {
    int8_t   raw;        /* headphone_raw(): 0/1                                   */
    int8_t   debounced;  /* hal_headphones_present(): -1/0/1                       */
    uint8_t  pin_cfg;    /* bit0 ENABLE, bit1 OUTPUT_EN of A7 (headphone_pin_cfg())*/
    uint16_t edges;      /* jackwatch raw_edges                                    */
} about_jack_t;
```

Footer text, centred at y=236 as today:

- normal: `ADC 2731 · LOG 6 on · JACK 1 ×4` — raw level then the edge count (`×` is
  `UI_GLYPH_MULT` if the atlas has it, else `n4`; the implementer checks `ui/atlas.h`).
- trusted build: `… · JACK 1/1 ×4` (raw/debounced) so a bench can also see the debouncer agree.
- pin not a GPIO input: `… · JACK 1 en=0` / `oe=1` appended — the "if nothing flips" branch of
  the doc's probe, on screen.

Width budget: F_SMALL is the footer's own face; `"ADC 2731 · LOG 123 on · JACK 1/1 ×12"` is ~36
glyphs. `ui_text_centered` does not ellipsise; the implementer measures with `text_width()` and,
if over `LCD_WIDTH - 32`, drops the `ADC` token first (the least useful of the three). The
gallery renderer (`render.py`) mirrors the string with `AB_JACK_RAW = 1, AB_JACK_EDGES = 0`.

`headphone_pin_cfg()` is a new two-read accessor in `hal/hw/headphone.c` (GPIOA ENABLE
`0x6000D000` and OUTPUT_EN `0x6000D010`, constants already in `headphone.h`'s bank layout via
the probe's `PROBE_GRP_*` — promote those two to unconditional `#define`s). Reads only; the
shipping driver still never reconfigures a pin it has not been proven to own.

Repaint: About is only repainted on `dirty`; step 2 above sets it on a raw edge while About is
showing, so the digit changes within one loop pass (≤10 ms) of the switch settling. No other
screen pays anything.

### Behaviour matrix (what the tests pin)

| Situation | Result |
|---|---|
| Playing, plug pulled | pause after the 200 ms debounce; strip reads Paused; position captured on the next capture pass |
| Plug re-inserted | nothing (stays paused, one PLAY away) |
| Paused or nothing loaded, plug pulled | nothing |
| Hold switch on, plug pulled | pauses (poll is outside the locked branch) |
| Charging modal up, plug pulled | pauses (poll precedes the modal edge and has no screen input) |
| HAL answers -1 (untrusted build) | never pauses, `last` never primes |
| Boot with jack empty | `last` primes to 0; no edge, no pause |
| Boot with jack in, then pulled | pause (if playing) |
| Suspend with plug in, pulled during sleep, wake | stays paused; no phantom pause on the first pass |
| Suspend with plug in, pulled and re-inserted during sleep, wake | stays paused (no auto-resume even here) |
| Suspend with plug out, inserted during sleep, wake | resumes (existing rule: resume unless the jack is *known empty*) |
| Bouncing switch | exactly one edge (HAL debounce; pinned in `hw-headphone-trace`) |

### Debounce window

200 ms, unchanged (`HEADPHONE_DEBOUNCE_US`). The doc argues it; the loop samples it ≥20× per
window while awake and 2× during suspend. No second window in the module.

## Bench procedure

Prerequisite: the merged branch built as the **default** image (TRUSTED 0). Release flow from
memory `hold_banner_design.md`: commit -> `make ipod` -> stage -> flash (`ipodpatcher 1 -wf`
elevated `< NUL`, or `core flash build-hw/core.ipod`) -> read back and `cmp`. Select+Play is the
unconditional way back.

**Flash 1 — read the pin (one flash, no cable):**

1. Boot with **nothing in the jack**, no charger, Hold off. Wait for the menu.
2. Settings -> About. Read the footer's last token. Write down `JACK <d>` and whether it shows
   `en=`/`oe=`.
   - `JACK 0` with no `en=`/`oe=`: expected for "empty, active-high, pin is a GPIO input".
   - Any `en=0` or `oe=1`: the ROM did not leave A7 as a GPIO input — stop here; see step 6.
3. Push the headphone plug fully home. Within a second the token should change. Write it down
   (`JACK 1 ×1` expected).
4. Pull the plug. Write it down (`JACK 0 ×2`).
5. Repeat 3–4 twice more (`×6` at the end). Then, with the plug half-inserted, wiggle it for a
   few seconds and note the count: a jump of more than ~2 per wiggle is the bounce the debounce is
   there for (informational).
6. Interpretation:
   - **1 in / 0 out, count climbs by exactly one per motion:** polarity is the default.
     Edit `hal/hw/headphone.h`: `HEADPHONE_DETECT_TRUSTED 1`. Leave `ACTIVE_LOW 0`.
   - **0 in / 1 out:** edit both: `HEADPHONE_DETECT_ACTIVE_LOW 1`, `HEADPHONE_DETECT_TRUSTED 1`.
   - **Never changes, no `en=`/`oe=` shown:** A7 is a GPIO input but is not the jack. This job
     stops; the follow-up is an on-screen version of the probe's all-ports dump (a Boot Details
     sub-page), not a cable. Leave TRUSTED 0.
   - **Never changes, `en=0` or `oe=1` shown:** A7 is not configured as an input. Follow-up: a
     one-line forced-input config at boot (the probe's `probe_force_candidate_input()` already
     has the masked-write grammar) behind its own flash. Leave TRUSTED 0.
   - **Flaps with the plug untouched:** not the jack; leave TRUSTED 0.
7. Pull `CORELOG.BIN` in disk mode later and `tools/make_log.py --dump` it: the `core: jack
   raw=… n=…` lines are the written record of step 3–5 (≤64 lines per boot).

**Flash 2 — the feature (after the two-line edit):**

8. `make sim && meson test -C build-sim` (all suites, including the trusted/untrusted headphone
   binaries, which override the header with `-D` and must stay green regardless of the default).
   `make hw && make verify-hw`. Flash.
9. Play a track. Pull the plug: it pauses within ~0.3 s; the strip says Paused; About shows
   `JACK 0/0`. Re-insert: still paused. PLAY: resumes.
10. Pause with PLAY, pull, re-insert: nothing happens either way.
11. Hold on, playing, pull: pauses; Hold off; PLAY resumes.
12. Plug the charger (charging modal up), playing, pull the headphones: pauses.
13. Power-cycle with the jack **empty**, Resume on: boots paused as always; no `core: jack out`
    line in the log for that boot.
14. Playing, plug in, hold PLAY 2 s (sleep). Pull the plug while asleep. Press a button: wakes
    paused, no resume. Re-insert, PLAY: plays.
15. Playing, plug in, sleep, wake without touching the plug: resumes (unchanged behaviour).
16. Dump `CORELOG.BIN`: `core: jack out, pause` / `core: jack in` lines at each step.

## Files to change

| File | Change |
|---|---|
| `ui/jackwatch.h`, `ui/jackwatch.c` | NEW pure module (above). Header comment in the house voice: why it exists (the `g_hp_last` inline detector, untested, and the wake gap). |
| `kernel/main.c` | `632-635` delete `g_hp_last`, add `g_jack`; `5774-5797` the thin block + `jack_narrate_raw`; `5422-5433` `jackwatch_prime` before the resume decision; `3237-3250` build an `about_jack_t` and pass it; `#include "../ui/jackwatch.h"` and `"hw/headphone.h"` at `13-55`. |
| `meson.build` (firmware sources) | add `ui/jackwatch.c` where `ui/keyhold.c` is listed for the ARM build. |
| `hal/hw/headphone.h` / `.c` | `int headphone_pin_cfg(void)` (two reads); promote `PROBE_QUAD_AD`, `PROBE_GRP_ENABLE`, `PROBE_GRP_OUTPUT_EN` out of `#if HEADPHONE_PROBE`. Update the header comment: the UART probe is kept for anyone with a cable; the on-screen About token is the probe this device uses. No change to the knobs or the debounce. |
| `hal/hal.h` | none (raw is hw-only; `main.c` already includes hw headers). |
| `hal/sim/headphone_sim.c` | none. |
| `ui/settings.h` | `about_jack_t`; `settings_about_render(..., const about_jack_t *jack)`. |
| `ui/screen_settings.c` | footer composition (`~530-548`); placeholder call at `294` passes NULL. |
| `docs/screens/render.py` | `AB_JACK_*` values and the footer string (`1084-1098`, `1166-1169`); regenerate `about.png` (and any GIF that passes through About — check `settings.gif`). |
| `tests/ui/jackwatch_test.c` | NEW. |
| `tests/meson.build` | register `jackwatch` after `keyhold` (`190-196` pattern); **add `-DHEADPHONE_DETECT_TRUSTED=0` to `headphone_untrusted_test` (`~692-700`)** so flipping the header default in Flash 2 cannot silently turn that binary into a second trusted build and break the suite; extend the headphone binaries' sources if `headphone_pin_cfg` needs nothing new (it does not). |
| `tests/hw_mmio/headphone_trace_test.c` | `test_pin_cfg_grammar` in the trusted and untrusted branches: exactly two reads (`0x6000D000`, `0x6000D010`), no writes, bit decode. |
| `docs/USER_GUIDE.md`, `docs/hw/10-headphone-jack.md`, `STATUS.md`, `README.md` / `core/README.md` | see Docs. |

Not touched: `player/`, `ui/keyhold.c`, `ui/chrome.c`, `kernel/config.c` (no record change),
`kernel/evlog.c` (the tap already captures `uart_puts`), the `SUSPEND_*` switches.

## Tests to add

`tests/ui/jackwatch_test.c` (suite `unit`, name `jackwatch`; pure, links only `../ui/jackwatch.c`):

1. `unknown_never_primes`: 50 feeds of -1 with playing=1 -> NONE every time, `last` stays -1.
2. `boot_empty_no_pause`: reset; feed 0 playing=1 -> NONE, `last`=0; feed 0 ×100 -> NONE.
3. `boot_seated_then_pull`: feed 1 -> NONE; feed 0 playing=1 -> PAUSE, `pauses`=1,
   `paused_by`=1, `edge_us`=now; feed 0 ×100 -> NONE (no repeat).
4. `pull_while_paused`: 1 then 0 with playing=0 -> NONE, `pauses`=0.
5. `replug_never_resumes`: 1, 0(PAUSE), 1 -> NONE, `paused_by`=0; then 0 playing=1 -> PAUSE
   again (a second genuine pull pauses again).
6. `minus_one_mid_stream`: 1, -1, -1, 0 playing=1 -> the 0 is still an edge from 1 (PAUSE):
   -1 is "no answer", not a level.
7. `prime_on_wake`: 1; `jackwatch_prime(0)`; feed 0 playing=0 -> NONE (no phantom pause);
   `jackwatch_prime(-1)` leaves `last`; `jackwatch_prime(1)` after a pause clears `paused_by`.
8. `note_raw_counts_transitions`: -1 seed; 1,1,0,0,1 -> returns 1,0,1,0,1 and `raw_edges`=2 after
   the first (the first sample is a prime, not an edge) -> total 2 edges over the sequence.
9. `no_screen_input`: the struct has no screen/hold/charging field — a compile-time statement in
   the test comment plus a case that runs the pull table twice with no other state and gets the
   same answers.
10. `clock_wrap_is_irrelevant`: `now_us` = 0xFFFFFFF0 then 0x10 across a pull; PAUSE regardless
    (documents that the module does no timing).

`tests/hw_mmio/headphone_trace_test.c`: `test_pin_cfg_grammar` (both non-probe binaries):
mock `0x6000D000 = 0x80`, `0x6000D010 = 0x00` -> returns `en=1,oe=0`; trace is exactly two reads,
zero writes; the untrusted binary asserts this is the *only* bus traffic besides `headphone_raw`.

Existing `hw-headphone-*` suites must stay green with the header defaults unchanged AND with
`HEADPHONE_DETECT_TRUSTED 1` set locally (the Flash-2 state) — run both before merging; the
`-D` fix in `tests/meson.build` is what makes the second true.

`main.c` is not host-built; its five-line block is covered by the module tests plus the bench.
Say so in STATUS.

Suite count: 58 -> 59; update the two places that print it (`README.md` Build, `core/README.md`
Quick start).

## Docs to update

- **`docs/USER_GUIDE.md`**
  - Settings -> About bullet: add "…and the headphone jack's switch (`JACK 1` when a plug is
    seated) with a count of how many times it has changed since power-on."
  - New paragraph under Now Playing (after "When a track ends"): "**Headphones.** Pull the plug
    while a track plays and it pauses; plugging back in does not start it again — press Play.
    Nothing on the cable can control playback: the jack has no button line on this iPod." Mark
    it as arriving with a later release if TRUSTED is still 0 at the time the guide ships — the
    guide describes the released image.
  - Power -> Sleep bullet: "If the headphones were pulled while it slept, it wakes paused."
- **`core/docs/hw/10-headphone-jack.md`**
  - "Confirming it on the device": add the on-screen procedure (Flash 1 above) *before* the UART
    probe; state that this device has no serial cable and the About token is the probe that has
    actually been used; keep the UART section for a bench that has one.
  - "The driver": add the policy paragraph — `ui/jackwatch.c` owns the pause decision (table
    from Design); the HAL owns the debounce; the wake re-prime; the evlog lines and their 64-line
    budget.
  - Table row 3 "Polarity: unconfirmed" stays until Flash 1 is read; the implementer leaves a
    `<!-- bench result: -->` slot the owner fills.
- **`STATUS.md`** top entry, dated, **UNFLASHED**: what landed (module, About token, evlog
  lines, tests), that the feature ships inert (TRUSTED 0), and the two-flash bench list verbatim
  from above so the next device night has it.
- **`README.md`** Hardware table "Input" row: "Click wheel, five buttons, Hold switch, headphone
  insertion switch" (only once TRUSTED is 1 — otherwise leave). Suite count.
- **`CHANGELOG.md`**: nothing until a tag.
- Memory: `headphone_jack_verdict.md` gets the on-screen probe and the module name (the owner's
  memory, updated by the caller, not by this plan).

## Acceptance criteria

1. `make sim && meson test -C build-sim` green, 59 suites, including `jackwatch` and all three
   `hw-headphone-*`; green again with `HEADPHONE_DETECT_TRUSTED 1` set in the header (no test
   depends on the default).
2. `make hw && make verify-hw` clean under `-Werror` (gcc 16); `check_hw_consistency.py` still
   passes (no new register constants outside `pp5022.h` beyond the two promoted probe offsets,
   which the doc's bank-layout table already derives).
3. `kernel/main.c` diff is ≤ ~40 lines net and contains no policy: every branch that decides
   "pause or not" lives in `ui/jackwatch.c`.
4. In the default image `hal_headphones_present()` still returns -1 with no bus traffic
   (`hw-headphone-untrusted`), and the only new bus traffic per loop pass is one GPIOA read
   (`headphone_raw`) — plus two more only while About is on screen (`headphone_pin_cfg`).
5. About renders `JACK <d> ×<n>` in every theme without overflowing the 320 px row; `about.png`
   regenerated by `render.py` matches the firmware string.
6. The gallery/README/guide text matches the image that will ship (TRUSTED 0): the guide's
   headphone paragraph is worded as the behaviour of a trusted build and STATUS says it is inert.
7. Bench (owed, UNFLASHED): steps 1–16 above, results pasted into STATUS.md and the doc's table
   row 3 updated.

## Risks

- **Polarity wrong after the bench.** Only reachable after a human sets TRUSTED 1; the failure
  mode (pause on plug-*in*) is visible on the first try and reverts by one flash. The About
  token keeps showing raw/debounced side by side in trusted builds so it is diagnosable on the
  device.
- **A7 is not a GPIO input on this ROM** (`en=0`/`oe=1`): the token shows it; the fix (forced
  input at boot) is a separate flash with its own trace test. Not done blind.
- **A7 is the wrong pin.** The on-screen probe watches one bit; the all-ports dump needs a
  second sub-page (follow-up). Probability is low given the port-L cross-check in the doc.
- **`hw-headphone-untrusted` relies on the header default.** Flipping TRUSTED to 1 would turn it
  into a second trusted binary whose `main()` is the trusted branch — the suite would still pass
  but the "no bus traffic while untrusted" promise would silently stop being tested. Fixed by the
  explicit `-DHEADPHONE_DETECT_TRUSTED=0` in `tests/meson.build`; do it in this job.
- **USEC_TIMER during a PLL park.** The debouncer times on USEC_TIMER; if `SUSPEND_PARK_PLL`
  (default 0) is ever enabled and the counter slows/stops, a candidate started before the park
  is accepted late or early. Mitigated: the wake path re-primes from the *current* debounced
  answer and the debouncer's first-sample rule means at worst one 200 ms delay. Note in the doc.
- **Footer width.** `ui_text_centered` does not clip; the implementer must measure and drop the
  `ADC` token if needed, and `render.py` must do the same so the gallery does not drift.
- **Loop cost.** One extra 32-bit read per pass (same register the Hold read hits). Nil.
- **Evlog spam.** Raw-edge lines are budgeted at 64 per boot; debounced lines are at most one per
  genuine edge. A cable being chewed in a pocket costs 64 lines (~2 KB) then silence.
- **Nothing device-side is verifiable in this job**; the gap is stated in STATUS.

## Conflict surface

Other agents in this batch touching the same files should know:

- **`kernel/main.c`** — the merge hot spot. This plan edits exactly four places: `632-635`
  (globals), `3237-3250` (`settings_render_cur` About call), `5422-5433` (wake resume gate),
  `5774-5797` (the jack block in `run_ui`), plus two `#include` lines. Anyone re-shaping the
  suspend/wake path (`suspend_to_ram`, `5158-5436`) or the top of the `run_ui` per-pass section
  (battery refresh `5760-5772`, charging modal `5799-5810`) will collide on line numbers, not
  logic; rebase order does not matter.
- **`ui/settings.h` / `ui/screen_settings.c`** — `settings_about_render` gains a trailing
  parameter. Any plan adding another About value should add it to the same call in the same
  commit, or accept a trivial signature conflict. `settings_diag_render` is untouched.
- **`tests/meson.build`** — appends one suite after `keyhold` and edits the `headphone_untrusted`
  `c_args`. Any plan adding suites appends near the end; the untrusted edit is one line.
- **`docs/screens/render.py`** — the About footer string; regenerates `about.png` (and any
  Settings GIF that passes through About). A plan that changes About's layout for another
  reason (e.g. more counts) must merge the footer text.
- **`docs/USER_GUIDE.md`** — About bullet under Settings; new paragraph under Now Playing;
  Power -> Sleep bullet. **`STATUS.md`** — new dated top entry (every plan adds one; keep them
  as separate bullets under one date heading).
- **`hal/hw/headphone.{h,c}`** — additive only. **`hal/hal.h`** untouched, so a plan editing
  the HAL contract elsewhere does not conflict.
- **New files** (`ui/jackwatch.*`, `tests/ui/jackwatch_test.c`) conflict with nothing.
