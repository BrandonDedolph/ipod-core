# MP3 playback

What was decided, what was measured, and what is still unknown.

Status: **on, and unproven on hardware.** The decoder is correct and inside
its budget on the host; nobody has yet watched a device play an MP3.

## The decision

MP3 shipped switched off from the beginning. `dr_mp3` was vendored, wrapped,
KAT'd and linked, and then hidden behind `CORE_ENABLE_MP3 0` because it could
not keep the PCM ring fed: its synthesis filter and IMDCT are plain `float`,
and on an ARM7TDMI with no FPU every multiply lowers to a libgcc soft-float
call. That is the "no floating point in core code" rule in PLAN.md arriving as
a concrete bill.

So the requirement was a **fixed-point** Layer III decoder, and since this tree
is Apache-2.0 and cleanroom, one under a compatible licence. That filter is
brutal:

| candidate | licence | fixed point? | verdict |
|---|---|---|---|
| Helix MP3 | RPSL / RCSL | yes | not Apache-compatible. PLAN.md named it for years |
| libmad | GPL-2.0 | yes | no |
| mpg123 | LGPL-2.1 | yes | a static freestanding link cannot satisfy the relinking clause |
| ffmpeg `mpegaudiodec_fixed` | LGPL | yes | same |
| Rockbox's libmad tree | GPL | yes | no, and cleanroom-forbidden here regardless |
| Symphonia | MPL-2.0 | no (float), Rust | no |
| dr_mp3, minimp3, PDMP3 | CC0 / Unlicense / PD | **no** | the 219 M instr/s problem |
| Espressif `esp_audio_codec` | binary `.a` | yes | no source |
| **pvmp3** | **Apache-2.0** | **yes** | **vendored** |

`pvmp3` is PacketVideo's decoder, carried in AOSP's `frameworks/av` and still
maintained there. It is the only production-grade permissive fixed-point
Layer III decoder that exists. The vendoring, the patches and the licence are
in [`core/codecs/pvmp3/README.md`](../../codecs/pvmp3/README.md).

## The numbers

Method, reproducible: each decoder was cross-compiled with the firmware's exact
flags (`-std=gnu11 -mcpu=arm7tdmi -ffreestanding -fno-builtin -Os`, pvmp3 with
`-DPV_ARM_GCC_V4` so the `smull` inline ops and the four `.s` routines are
used), linked bare-metal against the repo's `lib/mem.c` and libgcc with the
clip `.incbin`'d, and run under **unicorn 2.1.4** (a QEMU-derived ARM CPU
emulator) with chunked `emu_start(count=…)`, so the count is exact to ±250 k
instructions. The decoded PCM was read back out of emulated memory and
compared, so every count below is for a verified-correct decode.

Clip: 10 s of Beethoven's Symphony No. 3 (the Musopen recording, public
domain), 48 kHz stereo, at 128 and 320 kbps; for FLAC, the same 10 s of
ffmpeg-decoded PCM encoded with `flac -5`.

| binary | instructions | per second of audio |
|---|---|---|
| pvmp3 `-Os` V4, 128 kbps | 156.5 M | **15.65 M** |
| pvmp3 `-Os` V4, 320 kbps | 189.3 M | 18.93 M |
| pvmp3 `-Os` V4, the 1 s sine KAT vector | 14.75 M | 14.75 M |
| dr_flac `-Os` (CRC on), FLAC -5 of the same music | 91.5 M | 9.15 M |
| dr_mp3 `-Os` soft-float, 128 kbps | 2 190.8 M | 219.1 M |

`-O2` measured **identically** to `-Os` for pvmp3: the hot code is assembly and
inline assembly, so the optimiser has little left to do. `-Os` stays.

Reading it: pvmp3 costs about **1.7x FLAC** at 128 kbps and 2.1x at 320, and
dr_mp3 needed 14x pvmp3 — which is the whole story of why MP3 was parked. The
cost is almost flat with content (a sine is 360 k instructions per frame
against orchestral music's 376 k), because the fixed IMDCT and polyphase work
dominate, so 128 kbps is close to its own worst case.

### Accuracy

Against ffmpeg's decoder, aligned by best offset. ISO/IEC 11172-4 "full
accuracy" is an RMS error below 2^-15/sqrt(12) of full scale (a PSNR of about
101 dB) with a peak of at most 2 LSB.

| decode | PSNR | max diff |
|---|---|---|
| pvmp3, host C-equivalent ops, 128 kbps | 101.6 dB | 1 LSB |
| pvmp3, ARM `smull` assembly (emulated), 128 kbps | 103.4 dB | 2 LSB |
| pvmp3, ARM assembly, 320 kbps | 103.8 dB | 1 LSB |
| dr_mp3, soft float | 119.8 dB | 1 LSB |
| dr_flac | bit-exact | 0 |

Both pvmp3 paths clear the standard. They round differently from each other,
which is why the exact-match KAT reference is captured from the host build and
the ARM build is held only to the tolerance test — see
[`core/tests/codec-vectors/README.md`](../../tests/codec-vectors/README.md).

### Footprint

`.text` 24 308 B + `.rodata` 11 996 B for the whole decoder, `-Os
-mcpu=arm7tdmi`, V4 variant. Undefined symbols after a partial link:
`memcpy memmove memset __aeabi_idiv __aeabi_uidiv __aeabi_idivmod` — all
already in the image. No `__aeabi_f*`, no `__cxa_*`, no static constructors.
Against dr_mp3's 33.0 KB the decoder itself is +3.4 KB. The whole feature is
+16.6 KB (382 496 → 399 516 B measured on the linked image): the balance is the
11.5 KB framing, Xing, ID3 and seek layer pvmp3 has no notion of, of which
2.8 KB is the ID3v1 genre table TCON's numeric forms index into. bss grows
520 B (12 216 384 → 12 216 904): `scan_file_t.fmt` across BROWSE_MAX, and the
decode rate in `player_stats_t`.

Decoder state is one `pvmp3_decoderMemRequirements()` block: 28 088 bytes
upstream, **21 944** after the `BUFSIZE` reduction (see the vendoring README),
plus the wrapper's ~6.3 KB of staging. About 28 KB of the 128 KB arena, against
dr_flac's ~40 KB high-water — so `ARENA_BYTES` did not move, and
`mp3-freestanding` asserts the number from a real decode.

## What the host cannot answer

**Real time on the device.** Everything above is an instruction count; the
device runs at 80 MHz with an 8 KB cache in front of SDRAM. At a CPI of 1.5–2.5
— the honest bracket for codec code with a 36 KB footprint on a 3-stage
single-issue core — 128 kbps needs 24–39 MHz and 320 kbps 28–47 MHz, against
dr_flac's 14–23 MHz. That is the same shape of budget FLAC already lives in,
and it is a bound, not a measurement.

The measurement is **Settings → About → Boot Details, DECODE**: microseconds of
CPU per 1000 decoded frames, as a percentage of the 1e9/rate that real time
allows, drawn in red past 80 %. (That divisor is the playing stream's own rate
as of this change; it used to be a hard-coded 44.1 kHz, which under-reported a
48 kHz track by 8 %.) The bench, in order:

1. read FLAC's own DECODE % first, so the MP3 number has something to be
   compared against — the prediction is about 1.7x it;
2. 128 kbps CBR, then 320 kbps CBR, then a VBR file, then a 22.05 kHz mono
   podcast;
3. underruns over a ten-minute album (Boot Details reports them);
4. resume into an MP3, and a scrub on a VBR file;
5. a mixed FLAC → MP3 → FLAC hand-over, which must not stop the DAC.

If DECODE comes back over 80 %, the levers in order are: move
`pvmp3_polyphase_filter_window`, `pvmp3_mdct_18`, `pvmp3_dct_16/9` and the
Huffman tables into the 96 KB of unused IRAM (`boot/linker.ld`; a `.iram.text`
section plus a crt0 copy); then `-O2` on the bit-reader path only; then the
parked second core, which is a plan of its own. What is NOT a lever is shipping
the low sample rates to dodge the question — they are refused at open because
the WM8758B cannot be clocked there.

## Out of scope, deliberately

- **Gapless playback.** The LAME encoder delay and end padding are read, but
  only to correct the reported LENGTH so it matches ffprobe and the host index.
  The decoded stream is not trimmed, so a tagged file plays its few tens of
  milliseconds of padding, like any decoder without gapless.
- **ReplayGain for MP3.** `TXXX:REPLAYGAIN_*` is not read and no gain is
  applied; `player.c` keeps its `fmt != 1` gate and the FLAC path is unchanged.
- **MPEG-2.5 and 16 kHz MPEG-2.** Refused at open with
  `DECODER_ERR_UNSUPPORTED`. Resampling in software is exactly the cost that
  parked MP3 in the first place.
- **Free-format bitrate.** Refused: its frames carry no length.
- **Layer I and Layer II.** Refused. They are not MP3 and nothing produces them.

## The one hole in index parity

`core sync` and `tools/build_index.py` must stamp byte-identical indexes for
the same tree, and for MP3 they do — with one exception that is worth stating
rather than discovering.

An MP3 with **no Xing or VBRI header states no length.** Both tools then
estimate it. `build_index.py` takes ffprobe's estimate; `core sync` computes
its own, extrapolating the first frame's bitrate over the audio region. For a
constant-bitrate file those are the same number, exactly, and the parity test
asserts it. For a **variable**-bitrate file with no header they are not: the
first frame is not representative and ffmpeg samples instead, so the two
durations can differ by seconds and the two indexes differ in that record.

`lame` writes a Xing header unless asked not to (`-t`), and every other
encoder in circulation writes one too, so this is a rare file rather than an
impossible one. The consequences are also small: the device reads the duration
off the record, so the effect is one track showing the wrong time, not a track
that will not play.

`internal/id3/parity_test.go` walks an untagged file's frames, asserts the
comparison for the constant-bitrate ones like any tagged file, and SKIPS the
variable-bitrate ones by name with the two numbers printed. Making Go mimic
ffmpeg's sampling estimate would close it; it is not worth the fidelity risk
for a file class LAME does not produce by default.
