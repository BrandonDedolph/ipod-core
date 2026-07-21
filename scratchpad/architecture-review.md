# Core firmware — architecture & code-quality review

Principal-eng review of the from-scratch iPod 5.5G (PP5022, ARM7TDMI, no MMU/FPU,
freestanding) player. Read broadly across `core/`; ranked by impact. Citations are
`file:function`. Verdict up front: **the driver layer (hal/hw/*) and the FS/codec
plumbing are genuinely strong — 8-9/10 craftsmanship, well-commented, host-trace
tested.** The thing keeping this off a 10/10 is not the drivers; it is the
**application architecture above the HAL**, which has bifurcated into two programs.

Footprint answer up front (reviewer asked): `.bss` on the shipped image is
**~671 KB** (`size build-hw/core.elf` → bss 686688) on a 64 MB device — ~1%. The
big static buffers (ring 256 KB + arena 128 KB + 2×32 KB DMA + fb 150 KB + art
29 KB) are **rounding error**. Do not shrink anything for memory reasons; if
anything the ring could grow. Memory is a non-problem — spend the review budget on
structure and the present/ISR interaction instead.

---

## 1. Top structural issues (ranked)

### S1 — MUST FIX (#1). The tested/beautiful code and the shipped code are two different programs.
**What.** `core/meson.build` compiles `apps/audio/engine.c`, `apps/ui/*`
(cabinet, now_playing, list, chrome, search), `apps/db/tagcache.c` **only under
`target == 'sim'`** (meson.build:174-181). The device image (`core.elf`,
meson.build:135-160) is `boot + kernel/main.c + fs + lib + codecs + ui/text.c +
hal/hw`. Confirmed: nothing in `hal/hw` implements the `hal.h` LCD/input/clock
contract (only `hal/sim/sim_hal.c` does), and `kernel/main.c` calls **none** of
`lcd_present()/lcd_framebuffer()/button_get()/clock_ms()`.

Consequences, all real:
- The 4-page Cabinet "Linen" Now-Playing (`apps/ui/now_playing.c`), tagcache,
  album-art cache, peak meters, `atlas`-rendered chrome — **none of it runs on the
  iPod.** The device Now-Playing is `main.c:nowplaying_render`, a bare art blit +
  title + progress bar drawn straight into `console.c`'s framebuffer.
- The **tested** playback engine (`apps/audio/engine.c`, exercised by
  `sim-audio-playback` + the ring reasoning) is **not** the device engine. The
  device engine is the hand-rolled `player_*` in `main.c`. Worse, `engine.c` is
  structurally *unshippable*: `audio_engine_play` does `malloc(len)` +
  `memcpy` of the **entire compressed file** into RAM (engine.c:128-131). That is
  the exact model the device correctly rejected (streaming via `fat_src`), so
  `engine.c` can never be promoted to hw as-is.
- Two SPSC rings maintained in parallel with copy-pasted ordering helpers:
  `kernel/pcm_ring.c` (device) and the inline ring in `engine.c` (sim). Two
  time-formatters (`main.c:fmt_time`, `now_playing.c:format_time`). Two
  now-playing renderers. The `21/21` green suite validates the branch that does
  not ship.

**Why it matters.** Correctness (device path has ~no integration test coverage),
maintainability (every UI/engine change must be done twice or is done once in the
wrong place), and morale/waste (the largest, prettiest module — `apps/ui` — is
dead weight on device).

**Refactor.** Collapse to one application that both targets compile. Concretely:
promote the *device* engine/UI to be *the* engine/UI, and make the sim a HAL
backend under them — not a parallel app. That requires S4 (a real portable
render/input seam). `apps/audio/engine.c` should be **deleted** and its role
filled by the streaming `player_*` (moved out of main.c per S2); the good ideas in
`engine.c` (mono→stereo splat, release/acquire discipline) fold into the survivor.

---

### S2 — MUST FIX. `kernel/main.c` is a 1085-line god-file.
**What.** One file holds: boot/`kernel_main`, MBR parse + FAT mount, the
`fat_src` decoder-source adapter, `disk_read` retry, the streaming player engine
(`player_open_current/stop/advance/play_queue/pump`, `decode_pump`,
`decode_step`, `ring_source`), **all** UI rendering (`browse_render`,
`menu_render`, `nowplaying_render`, `boot_splash`, `draw_row`, `fmt_time`), the
screen stack, the `run_ui` event loop, the backlight timeout state machine, and
wheel acceleration.

**Why.** Impossible to unit-test any piece in isolation (hence S1's coverage
gap); every concern touches the same file-static globals; the control flow is hard
to reason about (the freeze-on-launch and glitch bugs live here).

**Refactor.** Split into headers + modules, and put them where sim can share them:
- `player/` — `player.c/.h` (queue, open/stop/advance/pump), `pump.c` (decode
  pump/step), `fat_source.c` (`fat_src_*`). Owns `g_ring`, `g_dec`, decoder
  lifecycle. Pure of UI and of MMIO.
- `ui/` — `screen.c` (the stack), `browser.c`, `menu.c`, `nowplaying.c`. Draw via
  a small gfx port (S4), not `console_*` directly.
- `boot/mount` stays in `kernel_main`; `run_ui` becomes a thin dispatcher.

---

### S3 — MUST FIX. Duplicate playback engines / rings (the concrete half of S1).
Already covered under S1; called out separately because it is the single most
copy-pasted subsystem. Target state: **one** SPSC ring (`pcm_ring.c`), **one**
engine (streaming), consumed by both targets. Delete `engine.c`.

---

### S4 — MUST FIX. The HAL contract is only half-honoured on device, which is *why* the UI got written twice.
**What.** `hal.h` bills itself as "the only header the kernel + apps include to
talk to the hardware," exposing `lcd_framebuffer/lcd_present/button_get/clock_ms`.
The **sim** honours all of it. The **device** honours only the *audio* slice
(`hal/hw/audio.c` implements `hal_audio_*`); for video it calls `hw/lcd.h`
(`lcd_present_fb/_rect`) and `kernel/console.c` directly, for input `hw/clickwheel.h`
(`clickwheel_poll`), for time the kernel tick. So there is **no portable
render/input/clock seam** that both backends implement — which is the structural
reason the UI exists twice.

**Why.** Without this seam, S1/S3 can't actually be unified — there's nothing for a
single UI to draw through.

**Refactor.** Define a tiny portable surface both targets implement:
`gfx_framebuffer() / gfx_present_rect(x,y,w,h) / input_poll(event*) / time_us()`.
Device wires them to `console_fb`+`lcd_present_rect`+`clickwheel_poll`+`USEC_TIMER`;
sim wires them to SDL. The one shared UI draws through it. (Note the two targets
even disagree on the input model — device is `wheel_event_t` poll, sim is
`button_t button_get(timeout)`; unify on the poll form.)

---

### S5 — SHOULD FIX (decision, not code). Cooperative scheduler vs hand-rolled superloop — you maintain both, ship neither cleanly.
**What.** `kernel/sched.c` + `switch.S` is a real, tested, round-robin cooperative
scheduler. But `run_ui` (main.c:775) is a single-threaded superloop
(`player_pump` → poll wheel → render → repeat, with a manual `for(volatile…)`
idle delay at main.c:926-929). The scheduler is reached **only** on the dead-end
fallback (`kernel_main` → `idle_task` when there's no LCD / mount fails,
main.c:1077-1084). So in the shipping path the scheduler is dead weight and the
superloop hand-codes cooperative scheduling inline.

**Judgment.** On a single core with DMA-driven audio, **the superloop is the right
call** — it's simpler and the ISR already provides the real-time guarantee. So the
recommendation is *not* "move playback to a scheduler task." It is: **pick one.**
Either (a) delete `sched.c`/`switch.S` from the ship image and own the superloop as
the design, or (b) if you keep the scheduler for a future COP/second-core split,
document it as explicitly not-yet-used and stop routing the fallback through it as
if it were load-bearing. Right now it reads as an unfinished intention.

---

## 2. Efficiency wins (concrete, ranked by payoff on an 80 MHz FPU-less core)

### E1 — BIGGEST LEVER. Browser/menu do a full-frame present on every change.
`run_ui` (main.c:908-921) calls `lcd_present_fb(console_framebuffer())` for any
`dirty` menu/browser repaint — a full 320×240 = **38 400 packed stores streamed
with IRQs masked** (`lcd.c:lcd_present_rect`, `LCD_IRQ_ENTER`). You already have
`lcd_present_rect` and use it for the NP band — but the list, the most-scrolled
screen, always full-presents, and the code even admits it by capping to ~14 fps
while playing (main.c:915) to avoid starving the audio ISR. **This is the primary
mechanism behind "audio glitches under UI load."** Fix: present only the two rows
that changed on a scroll (old-selected + new-selected band), or the list band
`LIST_Y0..bottom`. That shrinks the masked critical section by ~10× and largely
removes the need for the fps cap. High payoff, localized change.

### E2 — Full codec bring-up is re-run on every track.
`player_open_current` (main.c:618) calls `hal_audio_init(44100,2)` per track, which
runs `i2c_init + i2s_init + wm8758_init + dma_playback_init` (`audio.c:92`).
`wm8758_init` (wm8758.c:105) replays a **30-write reset → PLL relock → VMID ramp →
unmute** sequence *every track change*. That adds latency and likely an audible
pop/click at each transition, and re-locks the PLL needlessly. Fix: init the codec
+ I2S once at startup; per track only `dma_playback_stop/kick` (and mute/unmute if
needed). Split `hal_audio_init` (one-time) from `hal_audio_configure_stream`
(per-track, rate/channels only).

### E3 — `cache_commit()` flushes the *entire* cache inside the DMA ISR.
`fill_buffer` (audio.c:76-90) calls `cache_commit()` after every buffer refill,
and `cache.c:cache_commit` issues a global `CACHE_OP_FLUSH` and **spins on
`CACHE_CTL_BUSY`** — flushing *all* dirty lines (incl. the UI framebuffer), from
IRQ context. That lengthens the ISR and thrashes lines the UI just wrote. Fix:
flush only the just-filled 32 KB buffer by address range (Rockbox
`commit_discard_dcache_range` analog). Shorter ISR, less cache churn, less
interaction with the E1 present window.

### E4 — Now-Playing re-blits the album art every second for a band-only present.
`nowplaying_render` (main.c:515) does `console_clear` of the whole framebuffer and
re-blits the 120×120 art (main.c:521-525) once per second, but the steady-state
path only *presents* `NP_ANIM_Y=128..240` (main.c:901-903). So the art re-blit and
the clear of rows `<128` are wasted CPU every second. Fix: on the per-second tick,
clear+redraw only the band below `NP_ANIM_Y`; blit art once when the screen is
first shown or on `dirty`.

### E5 — `ata_read_sectors` bounces every 2-sector physical read, even for large aligned bulk reads.
`fat32.c:fat32_stream_read` deliberately issues one big aligned `fs->read` for many
whole sectors (the throughput optimization, fat32.c:484-503). But
`ata.c:ata_read_sectors` (ata.c:191) then loops `ATA_PHYS_LOG` (2) sectors at a
time through `ata_bounce` with a per-iteration `READ SECTORS` command + memcpy — so
a 64-sector request becomes **32 commands + 32 copies**, defeating the FS-layer
bulk read. Fix: when `off==0` and `count>=2` and the destination is halfword-safe,
issue a single multi-sector `ata_read_raw(phys, min(count&~1, 256), out)` straight
into the caller's buffer; only bounce the unaligned head/tail. Meaningful for
disk-bound spin-up and reduces PIO command overhead.

### E6 — Memory footprint: nothing to do.
Answered above — `.bss` ~671 KB / 64 MB. The buffers are sized sanely
(ring ~1.49 s, arena sized for MP3 high-water ~96 KB). Leave them. If you later
want more glitch headroom, growing the ring is free.

---

## 3. Correctness / robustness smells

### C1 — The HAL "quiescence guarantee" is documented but not implemented on hw.
`hal.h` promises (lines 194-200) that when `hal_audio_set_source` returns, the
previous callback is no longer in flight, so `userdata` is safe to free. The hw
impl (`audio.c:110-114`) is **two plain stores** — no barrier, no wait for an
in-flight `audio_dma_isr`. On device this isn't currently *exercised* at NULL (the
`player_*` path masks the DMA IRQ via `hal_audio_stop` before closing, and never
calls `set_source(NULL)`), so it's latent — but it's a documented contract the code
doesn't honour, and the sim engine *does* rely on it (`engine.c:198`). Either
implement it (mask the DMA IRQ around the swap, or spin until the current
completion count advances) or downgrade the comment to the truth.

### C2 — Per-track HAL/decoder lifecycle is fragile.
`player_advance` (main.c:644) carries a scar comment: an unconditional
`g_dec.ops->close()` after a *failed* open dereferenced stale ops and hard-froze
the device; the fix is the `g_pl_active` guard. That the "decoder open" and "HAL
running" states are tracked by one shared flag across three functions
(`open_current`/`stop`/`advance`) is the underlying fragility. Combined with E2
(re-init per track), track transitions are the riskiest control flow in the
firmware. A single `player_state` enum with one owner (the player module from S2)
and explicit open→playing→stopping transitions would make the freeze class
unreachable by construction.

### C3 — The wheel accumulator is shared across screens.
`run_ui` passes `&g_br_accum` (the *browser's* accumulator) to `wheel_move` for
**both** the menu (main.c:818) and the browser (main.c:835). A residual accumulator
from scrolling a list can carry into menu navigation. Harmless today (small counts)
but it's cross-screen state bleed; give each screen its own accum, or reset on
`scr_push/pop`.

### C4 — `disk_read` retry semantics contradict their own comment.
`disk_read` (main.c:216) claims immediate retries "with no audible stall" and that
"a sleep here would freeze the decode," yet `attempt >= 2` does `sleep_ms(60)`
**unconditionally**, including on mid-playback reads (this is the FS block callback
for `next_cluster` FAT walks too, not just open). It's survivable (the ring has
~1.5 s and DMA keeps draining) but the code and comment disagree; make the
mid-playback vs open-time policy explicit.

### C5 — `buf_phys` SDRAM alias is the self-identified #1 device risk.
`audio.c:62` hands the DMA `SDRAM_NATIVE_BASE + logical` on the assumption the DMA
ignores the MMAP0 remap. Well-documented, correctly flagged; note only — it's a
known device-gated unknown, not a code defect.

### C6 — FLAC decode can't distinguish EOS from a mid-stream error.
`flac.c:flac_decode` returns dr_flac's `0` for both EOS and corruption, so a
truncated/corrupt file silently ends the track (and `player_pump` auto-advances).
Acceptable for a player; document that broken files truncate rather than surface an
error, so it isn't mistaken for a bug later.

---

## 4. The 10/10 target + migration path

**Target shape.** One application, two HAL backends, a clean seam between them.

```
boot/crt0.S ─▶ kernel_main ──(clock,cache,timer,IRQ,mount)──▶ app_run()
                                                                  │
   ┌──────────────────────── portable app (shared by hw + sim) ──┴───────────┐
   │  player/        ui/                    core loop (superloop)             │
   │  ├ player.c     ├ screen.c (stack)     app_run():                        │
   │  ├ pump.c       ├ browser.c              player_pump();                  │
   │  ├ fat_source.c ├ menu.c                 if (input_poll(&ev)) dispatch;  │
   │  └ owns g_ring, ├ nowplaying.c           if (dirty) gfx_present_rect(…); │
   │    decoder      └ draw via gfx port                                      │
   └───────────────────────────────┬─────────────────────────────────────────┘
             portable ports:  gfx_present_rect / input_poll / time_us / hal_audio_*
        ┌──────────────────────────┴──────────────────────────┐
   hal/hw (device)                                        hal/sim (SDL)
   console_fb + lcd_present_rect,                         SDL fb + texture,
   clickwheel_poll, USEC_TIMER,                           SDL events/clock,
   audio.c (DMA ping-pong ISR)                            SDL audio callback
```

**Ownership & data flow (the discipline that earns the 10):**
- **One ring, one owner.** `pcm_ring.c` is the *only* SPSC boundary. Producer =
  foreground `pump`, consumer = `audio_dma_isr` → `ring_source`. This is already
  correct on device; make it the only one.
- **Player module owns all decoder/HAL lifecycle** behind a state enum; UI never
  touches `g_dec`/`g_ring`. Codec configured once, streams per track (E2).
- **UI is pure over a gfx port**, draws minimal dirty rects (E1/E4), never
  full-frame except on screen change.
- **Audio ISR stays short**: range-flush only (E3), no full-cache spin.
- **Scheduler decision made** (S5): superloop is the design; scheduler either
  deleted or explicitly future-work.
- Both targets build the same `player/` + `ui/`, so `meson test` finally covers
  the shipped path — the sim becomes a test/dev *backend*, not a separate app.

**Migration path (incremental, each step shippable, no big-bang):**
1. **Seam first (S4).** Add `gfx_present_rect/input_poll/time_us`; implement on
   device (trivial wrappers over existing `hw/`), on sim over SDL. No behavior
   change.
2. **Extract player (S2/S3).** Move `player_*`/`fat_src`/pump out of `main.c` into
   `player/`. `main.c` shrinks to boot + `app_run`. Add a host test for
   `player_advance`/EOS/skip — the coverage gap S1 leaves.
3. **Extract UI (S2).** Move `browse/menu/nowplaying` into `ui/`, drawing through
   the gfx port. Now the device UI is portable.
4. **Point sim at the shared app.** Replace `sim/main.c`'s
   `cabinet + audio_engine` with `app_run` over the sim HAL. **Delete
   `apps/audio/engine.c`.** Port/retire the parts of `apps/ui` you actually want
   (the Cabinet Now-Playing design is nicer than the device's — bring it across
   *as the shared* nowplaying.c, don't leave it stranded in sim).
5. **Efficiency pass** once unified: E1 (rect presents in the list), E2 (one-time
   codec init), E3 (range flush), E4 (band-only NP redraw), E5 (bulk ATA).
6. **Retire dead weight:** `apps/db/tagcache` and `apps/ui/*` either promoted into
   the shared app or removed; scheduler per S5.

Net: same features, one codebase, the shipped path tested, the masked-present
window shrunk ~10×, and the track-transition/codec-reinit fragility designed out.
The drivers are already there; this is entirely an above-the-HAL restructuring.
