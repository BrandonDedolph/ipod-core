# pvmp3 — the vendored MPEG-1/2/2.5 Layer III decoder

## Where it comes from

| | |
|---|---|
| Upstream | `platform/frameworks/av`, path `media/module/codecs/mp3dec/` |
| Browse | <https://android.googlesource.com/platform/frameworks/av/+/e2f098935447ca4945946de5cb69db843fe3f003/media/module/codecs/mp3dec/> |
| Commit | `e2f098935447ca4945946de5cb69db843fe3f003` (`refs/heads/main`, committed 2025-03-26) |
| Licence | Apache-2.0. `LICENSE` here is upstream's `NOTICE` verbatim; every source file keeps its "Copyright (C) 1998-2009 PacketVideo" header |
| Patents | `patent_disclaimer.txt`, vendored verbatim. It is a disclaimer, not a restriction; the last MP3 patents expired in 2017 |

`./vendor.sh` reproduces `upstream/` byte for byte from that tarball plus
`patches/`, and verifies the extracted tree against `SHA256SUMS.upstream`
first. A clean `git status` after running it is the check that what is
committed is what the pin says. Offline: `PVMP3_TARBALL=<cached .tar.gz>
./vendor.sh`.

Not vendored: `test/`, `fuzzer/`, `Android.bp`, `TEST_MAPPING` — Android build
and test scaffolding we neither compile nor ship.

## Why this decoder

It is the only production-grade, **fixed-point**, MPEG-1/2/2.5 Layer III
decoder under an Apache-compatible licence that exists. Everything else is
either floating point (dr_mp3, minimp3, PDMP3), copyleft (libmad GPL-2.0,
mpg123 and ffmpeg's `mpegaudiodec_fixed` LGPL, Symphonia MPL-2.0), under the
RPSL/RCSL (Helix), or a prebuilt binary with no source (Espressif's
`esp_audio_codec`). This tree is Apache-2.0 and cleanroom, so the licence
filter came first and left exactly one candidate.

That mattered because float was the problem. On the FPU-less ARM7TDMI every
float multiply lowers to a libgcc soft-float call, and dr_mp3 needed 219
million ARMv4T instructions per second of audio — 24x the FLAC decoder — which
is why `.mp3` shipped invisible for as long as it did. pvmp3 needs 15.7 M at
128 kbps and 18.9 M at 320 kbps, about 1.7x FLAC, and links no
`__aeabi_f*`/`__aeabi_d*` helper at all: its Q-format constants are
compile-time folds. (The soft-float symbols still in `build-hw/core.map` are
dr_flac's and predate this decoder.) Measurement method and the full numbers
are in `core/docs/design/mp3-playback.md`.

## What we changed, and why

Three patches, applied in order by `vendor.sh`. Nothing touches a copyright
header.

### `patches/0001-c-port.patch` — build it as C

Upstream is C++ only incidentally: it is C with a handful of C++ spellings.
The firmware is freestanding C11 with no C++ runtime, so the `.cpp` files are
renamed to `.c` and these mechanical changes applied.

1. Nine `Qfmt`-style macros used C++ functional casts
   (`int32(double(0x7FFFFFFF)*(a))`) — rewritten as C casts. They are
   compile-time constant folds, so no float reaches the link.
2. `typedef struct huffcodetab huffcodetab;` in `s_huffcodetab.h`, which C++
   gets for free.
3. `#include <stdbool.h>` where `bool` is used (`pvmp3_getbits.h`,
   `pvmp3_framedecoder.c`).
4. `parseHeader()`'s default arguments removed; its one call site passes the
   `NULL`s explicitly.
5. `[[fallthrough]]` x3 → `__attribute__((fallthrough))`.
6. `__attribute__((no_sanitize("integer")))` is a clang spelling that gcc
   rejects under `-Werror=attributes`. It becomes `PV_NO_SANITIZE_INTEGER`,
   defined in `pvmp3_audio_type_defs.h` and empty on gcc. What it was saying
   is still true — see the `-fno-sanitize` note in `codecs/meson.build`.
7. Every `__inline` function DEFINED in a header becomes `static __inline`
   (`saturate16`, `pv_abs`, the `fxp_*` Q-format helpers). C99 inline
   semantics give a bare `inline` definition no external symbol, so at `-O0`
   — which is exactly what the CI sanitizer job builds at — any call the
   compiler chose not to inline was an undefined reference. Same fix for
   `fillDataBuf`, which was `__inline` in a `.c`.

### `patches/0002-armv4.patch` — the ARM7TDMI is v4, not v5

`pvmp3_normalize` under `PV_ARM_GCC_V4`/`V5` uses `clz`, which is ARMv5. The
guard is narrowed to V5 so our V4 build takes the portable C count instead.
This is the only v5 leak: the `smull`/`smlal` inline ops in
`pv_mp3dec_fxd_op_arm_gcc.h` and the four `asm/*_gcc.s` routines use only
ARMv4 "M" instructions the ARM7TDMI has (counted: 69 `smull`, 61 `smlal`, and
add/eor/mov/sub).

### `patches/0003-bounds-fixes.patch` — fuzzer-found correctness

Cherry-picked, with thanks, from **esphome-libs/micro-mp3**
(<https://github.com/esphome-libs/micro-mp3>, Apache-2.0, Open Home
Foundation), a fork of the OpenCORE copy of this same decoder whose
`src/opencore-mp3dec/CHANGES.md` documents each one. We take the four that
matter for a bare-metal player and leave the rest (that fork drops the ARM
assembly and targets Xtensa, so most of its changes do not apply here).

1. **count1 out-of-bounds write.** `pvmp3_huffman_quad_decoding()` writes
   `is[i..i+3]` but the second-chance guard only required `i < 576`, so
   `i == 574` wrote one `int32` past `work_buf_int32[576]` into the adjacent
   `circ_buffer`. The guard is now `i <= 576 - 4`. Not optional for us: gcc 16
   reports it as `-Warray-bounds` and our build is `-Werror`. It also makes
   upstream's `(i-2) >= 576` fixup unreachable, so that is gone.
2. **`mp3_sfBandIndex[].l[]` clamp.** `region0_count` is 4 bits and
   `region1_count` 3, so the long-block lookups can index 24 into a 23-entry
   table.
3. **Side-info bounds guard.** `pvmp3_get_side_info()` runs BEFORE the
   `predicted_frame_size` check, so nothing else bounded it: a degenerate
   sub-side-info frame (8 kbps MPEG-2 stereo at 24 kHz is 24 bytes against
   4 + 2 + 17 = 23 of header, CRC and side info) read past the frame.
4. **Muted frames clear the IMDCT overlap.** On a reservoir underflow pvmp3
   mutes the frame, but left `overlap[]` holding the previous frame's tail:
   the "muted" frame still emitted stale audio and the next real frame
   overlap-added against wrong history, the two stacking into a transient
   LOUDER than the surrounding signal. Cleared, a muted frame is exact silence
   and the next one fades in from clean state.

The same patch also drops **`BUFSIZE` from 8192 to 2048**. It is the
bit-reservoir ring: the live window is one frame's main data (at most 1441
bytes) plus the 511 bytes `main_data_begin` can reach back, so 2048 is the
smallest power of two that covers it and still exceeds any single frame we
hand in. Upstream's 8192 was 4x what is needed and its comment misdescribed it
("the biggest mp3 frame" is 1441 bytes compressed; 4608 is the decoded PCM
size). Saves 6 KB of decoder state, taking `pvmp3_decoderMemRequirements()`
from 28088 to 21944 bytes. Verified byte-identical output on every committed
vector and on 10 s of orchestral music at 64 / 128 / 320 kbps.

Three fixes from that fork's list are **already in AOSP `main`** and needed no
patch: the `bitrate_index == 15` rejection, `Scratch_mem[198]`, and the
`__builtin_clz` normalize. The rest of its changes are Xtensa-specific, are
about the C-only build it targets, or change decoded output (its rounding
change to the Q-format helpers), and are deliberately not taken: our reference
PCM is captured from THIS decoder and its accuracy is measured against ffmpeg
(`tests/codec-vectors/README.md`).

## Our layer on top

`upstream/` decodes one frame. It knows nothing about files, tags, bitrate
modes or seeking. That is these three, first-party and under `-Werror`:

| file | what |
|---|---|
| `mp3_frame.c/.h` | frame headers, ID3v2/ID3v1/APE skipping, Xing/Info/VBRI, duration, the seek map. Pure functions over bytes; `tests/codecs/mp3_frame_test.c` |
| `id3_meta.c/.h` | the ID3 tags themselves, into the shared `flac_meta_t`; `tests/codecs/id3_meta_test.c` |
| `mp3.c/.h` | the `decoder_ops_t` contract: open, decode, seek, close |

The one rule not to break: **hand `pvmp3_framedecoder()` exactly one frame per
call.** It begins with `validate_input()`, which walks the whole buffer it is
given parsing every header in it. One frame is one header; the 32 KB
read-ahead window would be O(window) on every frame.
