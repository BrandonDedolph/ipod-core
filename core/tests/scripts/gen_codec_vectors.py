#!/usr/bin/env python3
"""
Generate deterministic codec test vectors.

Output PCM is written to stdout as raw bytes (interleaved s16le).
The FLAC encoder ('flac' binary) is run separately by the shell wrapper.

Vectors match what the C-side KAT regenerates by formula in
core/tests/codec_kat.c, so the encode/decode round-trip is bit-exact.

Currently only one vector — a 440 Hz sine wave, 1 second, 44.1 kHz,
16-bit stereo. Add more here as we vendor more codecs.
"""

import math
import struct
import sys


def gen_sine_440hz_1s_44k_stereo() -> bytes:
    """1 second of 440 Hz sine, 44.1 kHz, 16-bit stereo, ~50% amplitude.

    NOTE: deterministic *on the machine that runs this script* —
    Python's math.sin() calls the host's libm, which is not bit-stable
    across glibc / musl / macOS libm / etc. The .pcm output is
    committed alongside the .flac, and the KAT memcmps against the
    committed bytes — so the cross-libm wobble doesn't affect tests
    once the fixture is generated. Regenerate only on a pinned host.
    """
    SAMPLE_RATE = 44100
    DURATION_S = 1
    FREQ_HZ = 440.0
    AMPLITUDE = 16000  # leaves headroom; FLAC is lossless either way

    pcm = bytearray(SAMPLE_RATE * DURATION_S * 2 * 2)  # 2ch, 2 bytes/sample
    out = 0
    for n in range(SAMPLE_RATE * DURATION_S):
        # sin in double, truncate to int16 with banker-style round-to-zero
        s = int(AMPLITUDE * math.sin(2.0 * math.pi * FREQ_HZ * n / SAMPLE_RATE))
        # s16le, identical L and R
        struct.pack_into("<hh", pcm, out, s, s)
        out += 4
    return bytes(pcm)


VECTORS = {
    "sine_440hz_1s_44k_s16_stereo": gen_sine_440hz_1s_44k_stereo,
}


def main(argv: list[str]) -> int:
    if len(argv) != 2 or argv[1] not in VECTORS:
        print(f"usage: {argv[0]} <vector-name>", file=sys.stderr)
        print(f"available vectors: {', '.join(VECTORS)}", file=sys.stderr)
        return 2
    sys.stdout.buffer.write(VECTORS[argv[1]]())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
