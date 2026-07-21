# Real-time MP3 on PP5022 (FPU-less ARM7TDMI) — fixed-point decoder dig

Research scouting note. Read-only investigation; no source was modified.
Date: 2026-07-20. Target: PortalPlayer PP5022, dual ARM7TDMI (ARMv4T), 80 MHz,
no FPU, no HW divide. Freestanding (no libc; bump-arena alloc + own mem ops).
Constraint: firmware tree is **Apache-2.0 + strictly cleanroom** — GPL/other
copyleft code cannot be vendored; GPL sources may be read for FACTS only.

---

## TL;DR / recommendation

The MP3 stutter is the soft-float back half of the decoder, not the bit parsing.
There is **no permissively-licensed, drop-in, fixed-point MP3 decoder** we can
vendor: the two great fixed-point decoders (libmad, Helix) are GPL and RPSL/RCSL
respectively — both incompatible with an Apache-2.0 cleanroom tree — and every
permissive decoder (minimp3/dr_mp3, pdmp3, kjmp3) is float-only. The
cleanroom-preserving path is therefore **to keep dr_mp3's public-domain integer
front-end (header parse, bit reservoir, Huffman) and replace only its float DSP
stages — requantization, IMDCT, antialias, stereo, and the polyphase synthesis
filterbank — with our own Q28 fixed-point implementation**, using libmad/Helix
only as an algorithm-facts reference, never copying. As a near-term unblock,
**decoding on the idle COP is realistic on this exact SoC (it's how Rockbox does
it) and may reach real-time even with today's soft-float**, buying time before
the fixed-point rewrite lands.

---

## Decoder options table

| Decoder | License | Apache-compatible? | Fixed-point? | Freestanding-friendly? | Notes |
|---|---|---|---|---|---|
| **dr_mp3** (current) | Public domain / MIT-0 (dual) | **Yes** (PD) | **No — float** | Yes (already wired: no libc/libm, soft-float via libgcc) | Float lives in the whole Layer-III back half. This is what stutters. |
| **minimp3** (lieff) | CC0 / public domain | Yes (PD) | **No — float only** (SSE/NEON SIMD, else scalar float) | Yes | dr_mp3 is derived from it; **offers nothing extra fixed-point**. Upstream itself notes Cortex-M perf needs a float→fixed rewrite. |
| **libmad** | **GPLv2+** | **No** (copyleft; would force whole firmware GPL) | **Yes** — 100% integer, `mad_fixed_t` = Q28 (32-bit, 28 frac bits) | Yes (integer, 24-bit PCM out) | High quality, the classic FPU-less choice. **Read for FACTS only** (Q-format, stage structure). Rockbox's fork is also GPL. |
| **Helix MP3** (RealNetworks) | **RPSL-1.0 / RCSL-1.0** (dual) | **No** for an Apache tree (see below) | **Yes** — fixed-point, ARM-tuned | **Very** — reentrant, statically linkable, C+asm, no C++, ~4 KB stack, **ARMv4 (ARM7TDMI) supported** | Reportedly decodes **128 kbps 44.1 kHz joint-stereo real-time on an ARM7** using only fixed-point — i.e. proven on our exact CPU class. Best perf, wrong license. |
| **pdmp3** (technosaurus) | Public domain | Yes (PD) | **No — float** | Mostly (libmpg123-subset API) | Port of Krister Lagerström's ISO reference; float synthesis. No fixed-point win. |
| **mpg123 / libmpg123** | **LGPLv2.1** | Marginal/no for a static freestanding firmware (LGPL static-link relink obligation is impractical here) | Has integer output but **float DSP core** | Poor (assumes hosted env) | Not a fixed-point win and licensing is awkward for static bare-metal. |
| **FFmpeg mp3** | **LGPL2.1+/GPL** (config-dependent) | No (same LGPL static issue; GPL if enabled) | float DSP | Poor (huge, hosted assumptions) | Not viable. |
| **kjmp3 / small hobby decoders** | mixed (often float, varying license) | varies | mostly float | varies | No credible permissive fixed-point option surfaced. |

### License precision (the deciding factor)

- **libmad — GPLv2+ (high confidence).** Copyleft. Vendoring it forces the whole
  firmware to GPL, which directly breaks the Apache-2.0 goal. Usable as a
  **facts reference only** (register-free pure algorithm: Q28 representation,
  which stages exist, block-scaling approach). Do not copy code.
- **Helix — RPSL-1.0 or RCSL-1.0 (high confidence; confirmed from libhelix-mp3
  LICENSE.txt).** RPSL is OSI/FSF-approved *free software* but is **reciprocal
  copyleft**: derivative works must be re-licensed under RPSL, and *"Externally
  Deploying"* a modified version (shipping firmware to users counts) triggers a
  **source-disclosure obligation** under RPSL, plus a Seattle-venue clause. RPSL
  Exhibit B does name "Apache Software License" as a *Compatible Source License*
  — but that governs *combining separate files*, it does **not** let us relabel
  Helix as Apache. Net: Helix code stays RPSL forever; a Helix subdirectory is a
  **non-Apache, copyleft island** inside the tree. That conflicts with the
  stated "strictly Apache-2.0 cleanroom" rule. It is the best *technical* option
  and a legitimate one *if* the project later decides to accept an isolated
  RPSL-licensed component with its reciprocity + deploy-disclosure obligations.
- **dr_mp3 / minimp3 / pdmp3 — public domain / CC0 / MIT-0 (high confidence).**
  Fully Apache-compatible, but **all float**. No fixed-point path exists upstream.

---

## "Roll our own fixed-point" assessment

This is the recommended cleanroom path. dr_mp3 (public domain) is already vendored
and its **integer front-end is fine to keep**; only the float DSP needs replacing.

### What is float today (grounded in `core/codecs/dr_mp3/dr_mp3.h`)

The entire Layer-III synthesis back half is `float`. Confirmed functions:

- **Requantization** — `drmp3_L3_pow_43()` + `g_drmp3_pow43[]` table (the x^(4/3)
  power law) and `drmp3_L3_ldexp_q2()` (scalefactor gain). Float table lookup +
  float polynomial refine `(1 + frac*(4/3 + frac*2/9))` + float multiply.
  (`dr_mp3.h:1223, 1308–1331, ~1400–1435`)
- **Stereo processing** — `drmp3_L3_stereo_process()` (`:1533`), float.
- **Reorder / antialias** — `drmp3_L3_reorder()` (`:1587`), `drmp3_L3_antialias()`
  (`:1604`), float butterflies.
- **IMDCT** — `drmp3_L3_dct3_9()` (`:1639`), `drmp3_L3_imdct36/12/short/gr()`
  (`:1679–1801`), all float.
- **Polyphase synthesis filterbank (the hot loop)** — `drmp3d_DCT_II()` (`:1866`),
  `drmp3d_synth_pair/synth/synth_granule()` (`:2048–2240`); float, ending in the
  float→int16 clamp. This runs 32 subbands × 18 (or 12) samples per granule per
  channel every frame and dominates per-sample cost.

The sample type is selected at `dr_mp3.h:2022/2040` (`drmp3d_sample_t` = int16 or
float). Bit I/O, header sync, side-info, scalefactor and **Huffman decode are
already integer** — they are *not* the bottleneck (confirmed by the general
finding that soft-float DSP, not bit-parsing, is the cost on FPU-less cores).

**So: essentially the whole back half (requant → synthesis) is float.** Every
`float` mul/add there lowers to a libgcc soft-float call (`__aeabi_fmul`,
`__aeabi_fadd`, `__aeabi_f2iz`) — tens of cycles each — which is exactly the
headroom collapse observed (6% low-water on MP3 vs 93% on integer FLAC).

### Why fixed-point wins on ARM7TDMI

ARM7TDMI has 32×32→64 multiply with accumulate (`SMULL`/`SMLAL`) in a handful of
cycles. A Q28 fixed-point MAC replaces a ~20–40-cycle soft-float `fmul`+`fadd`
pair with a 1–7-cycle integer `SMLAL`. Across the thousands of MACs per frame in
IMDCT + synthesis that is the whole real-time gap.

### Effort / risk

- **Scope:** reimplement 5 stages — requant power law, IMDCT (36/12-point),
  antialias, stereo, polyphase synthesis DCT — plus a fixed float→s16 clamp.
  A few hundred lines; the algorithms are fully specified by ISO/IEC 11172-3, so
  this is a bounded, well-trodden problem (libmad and Helix are existence proofs).
- **Q format:** Q28 in a 32-bit word (sign + 3 integer + 28 fractional) is the
  proven choice (libmad's `mad_fixed_t`). Accumulate MACs in 64-bit
  (`SMLAL`) and round/shift back to Q28 to avoid loss. The requant x^(4/3) stays
  a table + fixed-point polynomial refine; keep the table, do the math in Q28.
- **Precision:** MP3 is lossy and not bit-exact across decoders anyway, so the
  bar is "no audible artifacts," not bit-match. Risk areas are (a) headroom/
  overflow in the synthesis accumulator on loud full-scale content — needs
  guard bits or block scaling like Helix; (b) rounding bias in IMDCT. Both are
  manageable with 64-bit accumulation and careful shift points.
- **Cleanroom hazard:** implement from the ISO spec and our own math; use
  libmad/Helix only to cross-check *facts* (which Q format, where block scaling
  goes). Do not transcribe their code or constant tables verbatim beyond what
  the standard defines. The existing KAT (compares against dr_mp3's own captured
  PCM) will need a tolerance-based comparison instead of exact match, since the
  fixed-point output will differ slightly from the float reference.
- **Verdict:** medium effort (est. 1–2 focused weeks incl. tuning + a
  tolerance KAT), low-to-medium risk, and the **only** option that keeps the
  tree uniformly Apache-2.0. Recommended as the durable answer.

---

## COP / other mitigations

- **Decode on the second core (COP).** PP5022 is dual ARM7TDMI and **Rockbox runs
  the audio codec on the COP with the UI/playback engine on the main CPU** — so
  offloading MP3 decode to the currently-slept COP is architecturally proven on
  this exact SoC. Upside: it roughly doubles the compute budget and is the
  "authentic" iPod design; a whole dedicated core might make **even today's
  soft-float dr_mp3 hit real-time with zero decoder changes** — the cheapest way
  to "it plays cleanly" short-term. Downside: it needs an inter-core framework we
  don't have yet — shared-SDRAM mailbox/ring, COP wake from its sleep, an IRQ or
  polled handoff, and **manual cache coherency** (PP cache isn't coherent across
  cores; flush/invalidate around shared PCM/bitstream buffers). That's real
  infrastructure, but it's reusable for all codecs and is on the roadmap anyway
  (the earlier "unparked-COP bug" shows the COP path is already partly in view).
  It parallelizes work rather than reducing it, so it complements — not replaces —
  the fixed-point rewrite.
- **Is soft-float the whole cost?** Effectively yes for the hot path. Bit parsing
  and Huffman are already integer and cheap; the float IMDCT + synthesis
  filterbank is where the cycles go. This confirms the fixed-point conversion is
  aimed at the right stages.
- **Smaller angles:** raising the CPU clock further has little margin left (boot
  already boosts to 80 MHz); reducing quality (e.g. skipping the subband synthesis
  refinement) is off the table per the "no quality cut" requirement.

---

## Concrete next step (if we pursue MP3 for real)

1. **Short-term unblock (optional, high value):** stand up a minimal COP handoff
   (shared-SDRAM ring + cache flush/invalidate) and move the *existing* dr_mp3
   decode onto the COP; measure whether a dedicated core alone restores real-time
   low-water. This also builds the inter-core plumbing every future codec wants.
2. **Durable fix (recommended):** keep dr_mp3's public-domain integer front-end;
   write an Apache-2.0, Q28 fixed-point replacement for the five float DSP stages
   (requant → IMDCT → antialias → stereo → polyphase synthesis), authored from
   ISO/IEC 11172-3 with libmad/Helix consulted for facts only. Switch the KAT to
   a tolerance comparison. Land it behind a build flag so float and fixed paths
   can be A/B'd on device.
3. **Decision to make first:** confirm the hard constraint. If "strictly Apache
   cleanroom tree" is immovable, do #2. If the project will accept one isolated
   **RPSL-licensed** component with its reciprocity + deploy-source-disclosure
   obligations, vendoring **Helix** is the lowest-effort, highest-performance
   route (proven ARM7 real-time, ~4 KB stack, freestanding) — but it is a
   copyleft island, not Apache.

### Sources
- Helix / libhelix-mp3 (RPSL/RCSL, ARMv4, ~4 KB stack, ARM7 real-time): https://github.com/ultraembedded/libhelix-mp3 , https://github.com/ultraembedded/libhelix-mp3/blob/master/LICENSE.txt
- RPSL-1.0 terms (reciprocal, external-deploy source disclosure, Exhibit B compatibles): https://spdx.org/licenses/RPSL-1.0.html , https://opensource.org/license/rpsl-1.0
- libmad (GPL, 100% fixed-point Q28): https://www.underbit.com/products/mad/ , https://github.com/sezero/libmad
- minimp3 float-only / Cortex-M needs fixed-point rewrite: https://github.com/lieff/minimp3 , http://cmorgan.org/2023/10/05/mp3-decoding-on-embedded.html
- pdmp3 (public domain, float): https://github.com/technosaurus/PDMP3
- mpg123 (LGPL): https://www.mpg123.de/
- Rockbox (GPLv2+, runs codec on COP; uses libmad): https://en.wikipedia.org/wiki/Rockbox
