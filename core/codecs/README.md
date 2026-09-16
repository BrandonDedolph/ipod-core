# Codecs

Vendored audio decoders, plus the unified ABI they conform to.

## ABI

[`decoder.h`](decoder.h) defines `decoder_t` and `decoder_ops_t` — the
contract every codec wrapper implements. The audio engine doesn't know
or care which codec is playing; it only sees `decoder_t`.

Output format is always 16-bit signed interleaved PCM. Codecs that
decode at higher bit depth (e.g. 24-bit FLAC) downconvert in their
wrapper.

## Freestanding

Both shipped decoders build freestanding for the device
(`-DCORE_FREESTANDING`): **no libc, no malloc, no libm**. `DR_FLAC_NO_STDIO`
removes dr_flac's file paths; allocation always goes through
explicitly-passed callbacks backed by a static arena, so dr_flac's default
`MALLOC`/`REALLOC`/`FREE` are defined to NULL/no-op and are never reached;
assertions compile out; memory ops route to `lib/mem.c`. pvmp3 takes one
caller-supplied block (`pvmp3_decoderMemRequirements()`, 21.4 KB) and never
allocates again.

**Both decode paths are pure integer.** That is new for MP3: dr_mp3's
synthesis filter and IMDCT were plain `float`, which on this FPU-less
ARM7TDMI the compiler lowers to libgcc's soft-float runtime (`__aeabi_fmul`
and friends), and it cost 219 M instructions per second of audio — 24x FLAC.
pvmp3 is fixed point; its Q-format constants fold at compile time and it
references no soft-float helper at all. (`build-hw/core.map` still lists a
few `__aeabi_f*`/`__aeabi_d*` entries. They are dr_flac's and predate this
decoder: compile each object on its own and only `dr_flac/flac.c` has
undefined float symbols.)

## FLAC: CRC is ON, and that is a seek decision

`DR_FLAC_NO_CRC` is **not** defined. It used to be, on what looked like
sound reasoning: dr_flac CRCs every decoded frame on top of the decode, and
we act on the result nowhere — a failed frame comes back as zero frames,
which `flac_decode` reports as end-of-stream exactly like a genuine EOS. So
the check cost cycles and bought nothing.

The reasoning was sound and the conclusion was still wrong, because
`DR_FLAC_NO_CRC` **also compiles out dr_flac's binary-search seek**:

```c
#if !defined(DR_FLAC_NO_CRC)
    if (!wasSuccessful && ...) { ...binary_search(...) }
#endif
```

That guard is load-bearing, not incidental: a binary search lands on an
arbitrary byte and has to decide whether it is looking at a real frame
header or at audio data that merely resembles a sync code — the header CRC
is what settles it.

With CRC off, and a library whose files carry **no SEEKTABLE** (verified:
these FLACs hold STREAMINFO + PICTURE + VORBIS_COMMENT + PADDING and nothing
else), every seek fell through to brute force — decoding from the start of
the track. Measured on device 2026-07-27: resuming ~30 s into a track cost
5.1 s of an 8.4 s cold boot, and the same cost applied to every manual
scrub.

Trading some per-frame decode margin for O(log n) seeks is the right way
round. The margin is visible on **Settings → Boot Details** (DECODE, against the
real-time budget of 1e9/rate µs per 1000 frames — 22676 at 44.1 kHz, 20833 at
48 — computed from the PLAYING stream's rate); if it ever gets tight the
fix is a seek table in the files, not brute-force seeking. The post-fix seek
timing has not yet been re-measured on the device.

## Codec status

| Format | Lib       | Status         | License |
|--------|-----------|----------------|---------|
| FLAC   | dr_flac   | ✅ shipping on device | Public domain (Unlicense) / MIT-0 |
| MP3    | pvmp3     | ✅ on; real time unproven on device (see below) | Apache-2.0 (PacketVideo / AOSP) |
| AAC    | (TBD)     | TODO           | TBD — Helix AAC (RPSL) is the leading candidate |
| ALAC   | Apple ALAC| TODO           | Apache-2.0 |
| Vorbis | Tremor    | TODO           | BSD-2 |
| Opus   | libopus   | TODO           | BSD-2 |
| WAV    | (own)     | TODO           | (Apache-2.0, ours) |

### Why pvmp3, and what is still unknown

dr_mp3 came first because single-header vendoring matched dr_flac's pattern
and exercised the ABI under a lossy shape cheaply. It could not make real
time on this CPU, and a float decoder never will, so the question became
which permissive FIXED-POINT Layer III decoder exists. The licence filter
answers it on its own:

| candidate | why not |
|---|---|
| Helix MP3 | RPSL/RCSL. PLAN.md named it for years; it is not Apache-compatible |
| libmad | GPL-2.0 |
| mpg123, ffmpeg `mpegaudiodec_fixed` | LGPL — a static freestanding link cannot satisfy the relinking clause |
| Symphonia | MPL-2.0, Rust, and float |
| dr_mp3, minimp3, PDMP3 | permissive, but float synthesis and IMDCT: the 219 M instr/s above |
| Espressif `esp_audio_codec` | prebuilt `.a`, no source |
| Rockbox's libmad tree | GPL, and cleanroom-forbidden here regardless |

**pvmp3** (PacketVideo, in AOSP's `frameworks/av`, Apache-2.0) is the only one
left, and it is a good one: 15.7 M ARMv4T instructions per second of audio at
128 kbps, 18.9 M at 320, against dr_flac's 9.15 M on the same music, and PSNR
103 dB against ffmpeg. Vendoring, the licence, the patent disclaimer and every
local patch are in [`pvmp3/README.md`](pvmp3/README.md); the measurements and
the method are in [`../docs/design/mp3-playback.md`](../docs/design/mp3-playback.md).

What the host cannot answer is real time. 1.7x FLAC at a CPI of 1.5–2.5 puts
128 kbps at 24–39 MHz of an 80 MHz core, which is the same shape of budget
FLAC already lives in — but that is a bound, not a measurement. **Settings →
About → Boot Details, DECODE** is the gate, and it has never been read on a
device playing an MP3. If it comes back over 80 %, the levers in order are
IRAM placement of the polyphase/IMDCT hot loops (there are 96 KB unused, see
`boot/linker.ld`) and then the parked second core.

### MP3: frames, Xing and seeking

pvmp3 decodes one frame and knows nothing about the file around it. The rest
is ours, in `pvmp3/`:

- `mp3_frame.c` — the 4-byte header and the frame length; two-header
  validation, because one sync word is worth nothing in a file full of cover
  art; ID3v2 (2.2/2.3/2.4, chained, with footers) skipped at the head and
  ID3v1/APE at the tail; Xing/Info/VBRI; and the seek map.
- `id3_meta.c` — the tags, into the same `flac_meta_t` every consumer already
  reads. Real UTF-16 to UTF-8, not the old high-byte drop that put `?` through
  the marquee of every Windows-tagged file.
- `mp3.c` — the `decoder_ops_t` contract. **One frame per
  `pvmp3_framedecoder()` call, always**: it begins with `validate_input()`,
  which walks the whole buffer it is handed.

**Duration** is the Xing/VBRI frame count times the samples per frame, less
the LAME encoder delay and end padding. That is ffprobe's rule, so the
progress bar agrees with what `build_index.py` and `core sync` stamped. With
no tag it is the CBR estimate from the first frame's bitrate — exact for CBR.

**Seeking** has no seek table to work with. The target frame maps to a byte
offset through the Xing TOC (100 entries, interpolated) or proportionally by
byte, the decoder backs off a FIXED 512 bytes plus four maximum-size frames
(6276 B), resyncs to a validated header, and decodes and discards every frame
up to the landing as warm-up: the bit reservoir reaches up to 511 bytes back,
and pvmp3 mutes a frame it cannot fill. The back-off is a constant on purpose.
Every per-file figure available at that point is untrustworthy for it — the
first frame's length is a quarter of the loud part on a VBR encode that opens
on silence, and without a Xing count the frame count is itself extrapolated
from that same first frame, so a "mean" derived from it is the first frame's
length restated. It costs four extra frame decodes at 320 kbps and about
fifteen at 128, paid once per scrub. Accuracy
is one frame (26 ms) plus the TOC's granularity, which is one percent of the
file: the same class of answer dr_flac's binary search gives. It costs one
disk-buffer window reset and a handful of frame decodes, which is why resume
seeks an MP3 now instead of cueing it at 0:00.

**Gapless and ReplayGain are out of scope.** The delay and padding correct the
reported length only; the decoded stream is not trimmed, so a tagged file plays
its few tens of milliseconds of encoder padding like any other decoder without
gapless. An MP3's `TXXX:REPLAYGAIN_*` frames are not read and no gain is
applied (`player.c` keeps its `fmt != 1` gate); the FLAC path is unchanged.

### Re-measuring the cost

The instruction counts above were produced by cross-compiling each decoder
with the firmware's exact flags, linking it bare-metal with `lib/mem.c` and
libgcc, and running it under **unicorn** (a QEMU-derived ARM CPU emulator)
with chunked `emu_start`, reading the decoded PCM back out of emulated memory
so every count is for a verified-correct decode. It is deliberately NOT in CI
— a pip dependency and a 12 s run for a number that only changes when a
decoder does. The method, the clips and the numbers are in
[`../docs/design/mp3-playback.md`](../docs/design/mp3-playback.md).

## Adding a new codec

1. Create `codecs/<codec>/` with:
   - `LICENSE` — preserve the upstream license verbatim.
   - `vendor.sh` — script that re-fetches the upstream source, with
     the URL and pinned version recorded.
   - `<lib>.h` / `<lib>.c` (or single-header) — the upstream source.
   - `<codec>.h` / `<codec>.c` — our wrapper, exposing
     `<codec>_decoder_ops()`.
   - If the source needs local changes — and anything that is not a
     single header will — keep them as an ordered `patches/` series that
     `vendor.sh` applies, and a `README.md` recording the pin and what every
     patch is for. `pvmp3/` is the worked example: re-running its `vendor.sh`
     leaves `git status` clean, which is the proof that what is committed is
     the pin plus the patches and nothing else.
2. Wire into `codecs/meson.build` as a new `static_library` and
   `declare_dependency`. Add to the `codecs_dep` aggregate.
3. Add a KAT to `tests/codec_kat.c` and a fixture to
   `tests/codec-vectors/` (regenerate via
   `tests/scripts/gen_codec_vectors.sh`).
4. Update this table.

## Running the KAT

The host build wires the test into Meson's runner:

```bash
cd core
make sim         # or: meson setup build-sim -Dtarget=sim && ninja -C build-sim
meson test -C build-sim       # runs every codec's KAT
```

Or directly:

```bash
./build-sim/tests/codec_kat ./tests/codec-vectors
# expected:
#   OK: flac decoded 44100 frames, 176400 bytes bit-exact
#   OK: mp3 decoded 46080 frames, 184320 bytes bit-exact
```

The MP3 count is the whole decoded stream, encoder delay and padding
included — 40 frames of 1152 samples, the Xing frame stepped over — not the
44100 the tag calls the track's length. See "Duration" above.

## Test vectors

All fixtures in `core/tests/codec-vectors/`. Generated by
`core/tests/scripts/gen_codec_vectors.sh`; regenerate only on a pinned
host (libm `sin()` is not bit-stable across platforms — the .pcm
output of the Python generator wobbles by ≤1 LSB across glibc / musl /
macOS, so the committed fixture is the source of truth).

| File                                            | Source                                              |
|-------------------------------------------------|-----------------------------------------------------|
| `sine_440hz_1s_44k_s16_stereo.pcm`              | Synthetic 440 Hz sine, 1 s, 44.1 kHz, s16, stereo, ±16000 |
| `sine_440hz_1s_44k_s16_stereo.flac`             | Above PCM piped through `flac` reference encoder    |
| `sine_440hz_1s_44k_s16_stereo_128k.mp3`         | Above PCM through LAME at 128 kbps CBR              |
| `sine_440hz_1s_44k_s16_stereo_128k.mp3.ref.pcm` | OUR decoder's output captured at fixture time       |
| `beethoven_2s_cbr128_44k_stereo.mp3`            | 2 s of public-domain music, LAME 128 kbps CBR       |
| `beethoven_2s_vbr_v5_44k_stereo.mp3`            | the same, LAME `-V 5` — a Xing header with a TOC    |
| `beethoven_2s_cbr32_22k_mono.mp3`               | the same as MPEG-2: 22.05 kHz mono, 32 kbps, no tag |
| `beethoven_*.ffmpeg.flac`                       | ffmpeg's decode of each, the accuracy reference     |

For a lossless codec the reference is the original input PCM — round-trip
must be byte-exact. A lossy codec has TWO references and they answer
different questions. `*.mp3.ref.pcm` is PCM captured from our own decoder, so
the KAT catches any regression in pvmp3 or the wrapper that moves a sample —
and says nothing about whether the decode is correct. `*.ffmpeg.flac` is
ffmpeg's decode of the same file, compared to a tolerance (PSNR ≥ 96 dB, peak
error ≤ 2 LSB after alignment), which is the test that would catch a decoder
that had been wrong from the start. **Do not "fix" a one-LSB difference**: the
ARM build's `smull` assembly and the host build's C ops round differently by
design, so the exact-match reference is only ever compared on the host. The
provenance of the music, the alignment rule and the whole argument are in
[`../tests/codec-vectors/README.md`](../tests/codec-vectors/README.md).
