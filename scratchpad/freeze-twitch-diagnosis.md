# Freeze + Twitch: root-cause diagnosis (PP5022, single ARM7TDMI)

Read-only analysis of HEAD `b6ff584` on `debug/lcd-multipresent`. Both bugs
arrived with the "background player" refactor (`d826fb6`) and survived three
fix attempts (`515b8d7`, `b6ff584`): guarding the `close()`, per-session vs
per-track `hal_audio_init`, and doubling the DMA ping-pong. This document
explains why none of those could work, ranks the real causes with code
evidence, and gives the definitive fixes.

The two bugs are **the same fault at two severities**: the LCD present runs
with the CPU IRQ line masked, and the audio DMA-completion handler is an IRQ.
Everything below follows from that.

---

## 0. Facts established from the code (grounding)

- **Audio completion is an ordinary IRQ (source 26), never FIQ.**
  `dma.c:dma_playback_init()` *clears* `CPU_INT_PRIORITY` bit 26 to force IRQ;
  `irq.c:irq_dispatch()` fans source 26 to `audio_dma_isr()`. (`DMA_IRQ = 26`,
  `DMA_MASK = 0x04000000`, `pp5022.h:645`.)
- **The LCD present masks the IRQ line for its whole duration.**
  `lcd.c` wraps `lcd_present_rect()` / `lcd_fill()` in
  `LCD_IRQ_ENTER()/EXIT()` = `arch_irq_save()/restore()`. The masked region
  covers the pixel stream **and** `bcm_frame_commit()`'s wait-for-idle spin.
- **`arch_irq_save()` only touches the CPSR I-bit (0x80).** `irq.h:52`
  ORs `CPSR_I_BIT` (`pp5022.h:172` = 0x80). It does **not** touch the F-bit
  (0x40). => **A present masks IRQ but not FIQ.** This is the linchpin of the
  10/10 fix.
- **FIQ is currently masked and unused.** `crt0.S` boots `CPSR=0xD3`
  (I+F masked); `arch_irq_enable()` clears only the I-bit, so F stays set
  forever. The FIQ vector `_v_fiq -> trap_hang` is a `b .` park.
- **The DMA-completion ISR must run within one ping-pong buffer time or the
  DAC stalls.** `audio.c:audio_dma_isr()` re-kicks the *other* buffer and
  refills the drained one. If it does not run, the channel is never re-kicked;
  the I2S FIFO underruns. Buffer depth is now `AUDIO_FRAMES_PER_BUF = 8192`
  = ~186 ms (`b6ff584`).
- **The ring's fullness does not protect the DAC during a masked present.**
  The ISR is the only thing that moves data ring -> DMA buffer. With the I-bit
  masked, an 87%-full ring is irrelevant — the starved actor is the *ISR*, not
  the decoder.
- **Stacks (rules out the "stack overflow" hypothesis, arithmetic below).**
  `linker.ld`: `_irq_stack_top = 0x40018000`, `_stack_top = 0x40017C00`.
  SVC stack grows **down** from `0x40017C00`; nothing else is linked into IRAM
  (all `.text/.data/.bss`, including `ring_storage` 256 KB, `audio_buf` 64 KB,
  `arena_buf` 128 KB, live in SDRAM). So SVC has ~95 KB of headroom down to
  `0x40000000`, and it grows *away* from the 1 KB IRQ stack. `drflac_open`
  works out of the 128 KB SDRAM arena, not the stack. **Stack overflow is not
  possible here** — the prompt's "1 KB below the IRQ region" reads the SVC top,
  not its extent.

---

## 1. Bug A — HARD FREEZE on song launch / switch

### The mechanism that is *newly and heavily* exposed

The old blocking `play_file()` only ever presented **while sitting in its own
now-playing loop**, and after the first paint it presented a *partial rect*
(the ~112-row animated strip). It **never did full-frame presents of the
browser or menu while audio was running** — because audio only existed inside
that loop.

The background engine changes this completely (`run_ui()`):
- Audio runs continuously across **Menu, Browser, and Now Playing**.
- Menu and Browser repaint with **full-frame** `lcd_present_fb()`
  (`main.c:918`), each an I-bit-masked ~150 KB stream + `bcm_frame_commit()`.
- **Launching a new song while one plays is the worst case**: Browser
  (audio + full presents) -> `player_play_queue()` -> `player_stop()` +
  `player_open_current()` (I2C/I2S/WM8758/DMA re-init + a full ring prime of
  1.49 s of FLAC + disk reads, a bus/cache storm) -> `scr_push(NOWPLAYING)`
  -> immediate **full-frame** present of Now Playing (`np_first`), all with the
  freshly re-kicked DMA contending.

During any full present the I-bit is masked, so `audio_dma_isr()` cannot fire.
If the masked window exceeds the ping-pong depth, the DMA channel goes idle
(never re-kicked) and audio dies. The window is dominated by
`bcm_frame_commit()`'s wait-for-idle:

```
spin = BCM_IDLE_SPIN_LIMIT (1<<16);  kick = BCM_REKICK_TRIPS (1<<12);
while ((stat==BCMCMD_LCD_UPDATE || stat==0xFFFF) && --spin) {
    if (--kick==0) { re-issue LCD_UPDATE; strobe; kick = BCM_REKICK_TRIPS; }
    stat = bcm_read32(BCMA_COMMAND);   // each is 2 handshakes, up to 1<<16 each
}
```

When back-to-back presents (scroll, or the launch sequence) hand the BCM a new
frame before the previous one retired, this loop spins toward its enormous
budget — up to `BCM_IDLE_SPIN_LIMIT * (2 * BCM_SPIN_LIMIT)` MMIO polls — with
IRQs masked the entire time. That is long enough that the **tick stops, audio
stops, the screen is frozen mid-present, and the device looks dead**, which is
exactly the reported symptom. Whether it self-recovers in ~1-2 s or the BCM
latches busy hard enough that repeated presents keep re-entering the spin, the
user experience is "hard freeze, physical reset."

### Why the three attempted fixes could not help
- **Guarding `close()`** removed a *real, separate* freeze (a genuine
  `b trap_hang` via a stale `g_dec.ops` — see below) but not this one.
- **Per-session vs per-track `hal_audio_init`** — both re-enter over an
  already-quiesced HAL (`player_stop()`/`player_advance()` mask IRQ 26 *before*
  the ring/decoder are touched), so neither variant is the freeze. Correctly,
  reverting made no difference.
- **Doubling the ping-pong (93->186 ms)** raises the underrun threshold but
  does nothing about a `bcm_frame_commit` spin that can exceed even 186 ms, and
  nothing about the fact that the ISR simply *cannot run* while the I-bit is
  masked.

### Ranked hypotheses for Bug A

1. **(PRIMARY) IRQ-masked full-frame present starves/stalls the audio path at
   launch; `bcm_frame_commit`'s wait-for-idle spins its budget with IRQs
   masked.** Highest-exposure new behavior; matches "screen locks + audio dies
   + reset"; explains why all three fixes missed. **Confidence: moderate-high**
   that this is the dominant launch/switch freeze.

2. **(REAL, PARTIALLY FIXED) `b trap_hang` via an indirect call through a
   stale/NULL `g_dec.ops`.** `flac_open_stream()` sets `d->ops` *last, only on
   success*; on failure `g_dec.ops` keeps a previous track's pointer (or NULL
   from `.bss` on the first-ever open). Calling through it branches into
   garbage -> undefined-instruction -> `_v_undef -> trap_hang` (`b .` =
   permanent, physical-reset-only). This was the original launch freeze; the
   `g_pl_active` guard (`b6ff584`) closes the paths in `main.c`. **Residual
   risk:** any *future* indirect call reached before `d->ops` is validated,
   and the general fragility of "ops set last." Keep as a monitored #2.

3. **Teardown/re-init race between `pcm_ring_init` and a still-armed DMA ISR.**
   *Largely refuted.* Every re-open is preceded by `hal_audio_stop()` masking
   IRQ 26, and the ISR (`ring_source -> pcm_ring_read`) is **decoder-
   independent** — it only touches the always-valid static `g_ring`, never
   `g_dec`. So a late ISR during `player_stop()`'s `sleep_ms(60)` at worst
   drains stale tail PCM; it cannot deref freed/stale decoder state.

4. **`sleep_ms()` called with IRQs masked (permanent tick-starved spin).**
   *Refuted for current paths.* `sleep_ms` is only reached from foreground
   (`player_stop`, `disk_read` retry) with the I-bit enabled; `sched_yield()`
   returns immediately pre-`sched_start`, so it degrades to a tick busy-wait
   that always terminates. Would become a real freeze only if a `sleep_ms`/
   `disk_read` were ever called from inside an `arch_irq_save` section — a rule
   to enforce, not a current bug.

5. **Stack overflow.** *Refuted* by the arithmetic in §0.

6. **FIQ mis-route to `trap_hang`.** *Not currently reachable* — F-bit stays
   masked, so even a mis-routed source 26 is never delivered. (Becomes a
   live concern the moment we adopt the fix in §3; handled there.)

### Definitive fix for Bug A
The launch freeze and the twitch are the same disease; the cure is the same
(§3): **take the audio DMA completion off the I-bit-masked path (FIQ), and
stop masking IRQs around the *wait-for-idle***. Concretely for A:

- **Move `bcm_frame_commit()`'s wait-for-idle OUT of the masked section.**
  Only the *pixel stream* must be uninterrupted (an ISR mid-stream aborts the
  BCM frame). The trailing spin that waits for the *previous* update to retire
  does not need the I-bit masked. Restructure so `LCD_IRQ_ENTER/EXIT` wraps
  **only `bcm_stream_pixels`**, and the idle-wait runs with IRQs live. This
  alone bounds the masked window to the ~few-ms stream and lets the audio IRQ
  fire during the (previously masked) multi-poll spin.
- **Keep the `g_pl_active` guard** and, defensively, initialize `g_dec.ops = 0`
  in `player_stop()`/after close so hypothesis #2 can never resurface.
- With audio on FIQ (§3), the pixel stream itself no longer needs to block
  audio at all.

---

## 2. Bug B — audio twitch when scrolling fast during playback

### Root cause: ISR starvation by the IRQ-masked present (NOT decode starvation)

Do the arithmetic. While scrolling the Browser during playback the loop is:
`player_pump()` (one `decode_step()` of 1024 frames) -> poll wheel -> render +
**full present**, with presents rate-capped to ~14 fps (`main.c:915`, 70 ms).

- **Decode keeps up easily.** Between presents the loop spins freely (no idle
  throttle while `g_pl_active`), doing a `decode_step()` each pass. The ring
  stays ~87% full — confirmed by the BUF readout. So decode starvation is
  **not** the cause.
- **The DMA-completion ISR is what starves.** During each full present the
  I-bit is masked for the pixel stream **plus** `bcm_frame_commit`. Fast scroll
  keeps issuing full frames, so the BCM is often still busy when the next
  present's idle-wait begins, driving that spin long. If a single masked window
  reaches ~186 ms the channel underruns; even shorter windows, arriving every
  70 ms, jitter the completion cadence enough to click. **The 87% ring cannot
  help** — only the ISR moves ring -> DMA buffer, and it is exactly what is
  blocked.

That is precisely why deepening the ping-pong to 186 ms "did NOT fix it": it
raises the bar but does not remove the masked-window-exceeds-buffer tail, and
does nothing about the structural fact that the consumer is disabled during
the present.

### The 10/10 fix: route audio DMA completion to FIQ
Because `arch_irq_save()` masks only the I-bit, an **FIQ preempts the LCD
present**. Put the audio completion on FIQ and it re-kicks the ping-pong on
time no matter how long a present holds the I-bit. This is also what Rockbox
does on this exact SoC, so it is proven tolerable to the BCM.

What it takes on PP5022:
1. **Route source 26 to FIQ:** in `dma_playback_init()` **set** (not clear)
   `CPU_INT_PRIORITY` bit 26 (`| DMA_MASK`). (Setting a bit = FIQ.)
2. **Real FIQ handler + stack in `crt0.S`:** replace `_v_fiq -> trap_hang`
   with an entry that uses the banked FIQ registers (r8-r14_fiq) so it needs
   little/no stacking, `sub lr,lr,#4`, calls a lean C `audio_fiq()` (or inlines
   the re-kick), returns with `subs pc, lr, #0` restoring CPSR from SPSR_fiq.
   Give FIQ its own small banked stack (carve another slot from top-of-IRAM;
   there is ~95 KB of unused IRAM below the SVC top — trivial).
3. **Unmask FIQ:** clear the CPSR F-bit once, after `dma_playback_init`, in a
   new `arch_fiq_enable()` (mirror `arch_irq_enable` with 0x40).
4. **Make `audio_dma_isr()` FIQ-safe:**
   - It must touch only state that is safe against a foreground it can preempt.
     The SPSC ring already assumes exactly one consumer; that consumer becoming
     an FIQ is fine (indices are the only shared words; keep the
     release/acquire pairs).
   - **`cache_commit()` from FIQ is the one real hazard.** It must never race a
     foreground `cache_commit`. Today the only foreground `cache_commit` is in
     `fill_buffer()` during priming (before the channel is kicked / FIQ armed),
     and `cache_init()` at boot — no overlap. **Enforce the invariant** that
     foreground never calls `cache_commit()` while audio is armed. (Better:
     give the FIQ a self-contained line-flush of just the 32 KB it filled.)
   - Keep the handler tiny: re-kick + fill + flush = tens of µs, every 186 ms.
5. **The completion must NOT also be delivered as an IRQ.** Remove source 26
   from the IRQ enable / `irq_dispatch` path so it is FIQ-only.

**Risk to weigh:** the present masks IRQs specifically because an ISR stalling
the pixel stream makes the BCM abort the frame. An FIQ *will* preempt the
stream. But the audio FIQ is short and rare, and Rockbox ships audio-at-FIQ
alongside BCM LCD streaming on this silicon, so brief FIQ gaps are tolerable
(the BCM aborts on *long* stalls / mid-transaction aborts, not tens-of-µs
bubbles). If a residual tear appears, pair FIQ with the §3 present changes.

### Cheaper mitigations (real, but not 10/10)
- **Move `bcm_frame_commit`'s idle-wait out of the masked section** (§1) — big
  win, low risk, do this regardless.
- **Partial-rect presents for Menu/Browser** (they already exist for Now
  Playing): repaint only changed rows instead of full frames, shrinking the
  masked window ~5-10x. Strong, cheap, no FIQ needed for a first pass.
- **Harder present cap / coalescing** during scroll — diminishing returns past
  14 fps.
- **Decode-ahead scheduling** — irrelevant to Bug B (decode is not the starved
  actor); do not spend effort here for this bug.
- **Shrinking the ping-pong** — wrong direction.

Recommendation: **FIQ is the correct 10/10 answer**; ship it together with the
"idle-wait unmasked" + partial-present changes, which also stand alone as a
strong, low-risk interim fix if FIQ bring-up needs device iteration.

---

## 3. The 10/10 target architecture for UI-vs-audio coexistence

Single in-order ARM7TDMI, no preemptive threads. Use the two hardware
priority levels as a fixed-priority real-time schedule:

1. **Audio = FIQ = hard real time, top priority, preempts everything.**
   The DMA-completion FIQ re-kicks the ping-pong and refills from the SPSC
   ring. Nothing the foreground does (LCD present, disk PIO, decode) can delay
   it, because FIQ is never masked by `arch_irq_save`. Handler stays minimal
   and self-contained (own banked regs/stack, own cache flush).

2. **System tick = IRQ.** Timekeeping/`sleep_ms`. Fine to be briefly masked by
   the present; not real-time-critical.

3. **Decode = best-effort foreground job.** Keep the bounded one-chunk-per-pass
   `decode_step()` so the loop stays responsive; the ring (1.49 s) is the
   elastic buffer between decode jitter and the FIQ consumer. Target ring
   fill > one present-worth of audio at all times (already achieved).

4. **UI render + present = lowest priority, fully preemptible by audio.**
   - Only the pixel *stream* is a critical section (mask IRQ, **not** FIQ);
     the wait-for-idle runs unmasked.
   - Prefer partial-rect presents everywhere; never full-frame during playback
     if a dirty-rect will do.
   - Rate-cap presents while playing (keep the 14 fps cap).

5. **Input = polled in the loop** (already so); the shared I2C/clickwheel IRQ
   stays hard-masked (`irq.c` high-bank guard) — unaffected.

Invariants to enforce (so FIQ stays safe):
- Foreground never calls `cache_commit()` (or any non-reentrant HAL the FIQ
  uses) while audio is armed.
- Exactly one ring producer (decode) and one consumer (the FIQ).
- `g_dec.ops` is validated before any indirect call, and zeroed on close.

Net effect: audio is decoupled from *both* decode jitter (ring) and UI stalls
(FIQ over a masked present). The UI can present full frames, spin the BCM idle-
wait, or block on disk without ever clicking the DAC — which is the property
the background player was supposed to deliver.

---

## Executive summary

- **Most likely freeze cause (Bug A):** the background engine does full-frame
  `lcd_present_fb()` **with the CPU IRQ line masked** while audio runs — vastly
  more than the old player ever did — and `bcm_frame_commit()`'s wait-for-idle
  can spin toward a `1<<16 x 1<<16`-poll budget inside that masked window.
  Launching a new song is the worst case (bus/codec/cache re-init storm +
  immediate full present). Tick and audio ISR die; the screen is frozen mid-
  present -> "hard freeze, reset." The three prior fixes (guard `close`, audio-
  init location, bigger ping-pong) don't touch the masked present, so they
  couldn't help. (A second, *already-partially-fixed* freeze — `b trap_hang`
  via a stale `g_dec.ops` — is the real reason fix #1 helped once; keep the
  `g_pl_active` guard and zero `ops` on close.)
- **Concrete fix (Bug A):** move `bcm_frame_commit`'s idle-wait out of the
  IRQ-masked section (only the pixel stream needs masking), switch Menu/Browser
  to partial-rect presents, and adopt the FIQ change below.
- **Twitch (Bug B):** ISR starvation, not decode starvation — the ring is 87%
  full but the masked present disables the only actor that moves ring -> DMA
  buffer; 186 ms didn't help because the masked window can exceed it.
  **Recommended fix: route audio DMA completion (source 26) to FIQ** (set
  `CPU_INT_PRIORITY` bit 26, real FIQ vector + banked stack in `crt0.S`, clear
  the CPSR F-bit, make `audio_dma_isr` FIQ-safe w.r.t. `cache_commit`). FIQ is
  not masked by `arch_irq_save` (it only sets the I-bit), so it preempts the
  present — the same architecture Rockbox proves on this SoC. Cheaper interim:
  unmask the idle-wait + partial presents.
- **Confidence:** Bug B mechanism and fix — **high**. Bug A dominant cause —
  **moderate-high** (device UART instrumentation around the present and a PC-
  sample at freeze would confirm it lands in `bcm_frame_commit`/`bcm_read32`
  vs `trap_hang`).
