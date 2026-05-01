# Codecs

Vendored audio decoders, plus the unified ABI they conform to.

## ABI

[`decoder.h`](decoder.h) defines `decoder_t` and `decoder_ops_t` — the
contract every codec wrapper implements. The audio engine doesn't know
or care which codec is playing; it only sees `decoder_t`.

Output format is always 16-bit signed interleaved PCM. Codecs that
decode at higher bit depth (e.g. 24-bit FLAC) downconvert in their
wrapper.

## Codec status

| Format | Lib       | Status        | License |
|--------|-----------|---------------|---------|
| FLAC   | dr_flac   | ✅ wrapped + KAT | Public domain (Unlicense) / MIT-0 |
| MP3    | Helix MP3 | TODO          | RPSL (GPL-compat) |
| AAC    | Helix AAC | TODO          | RPSL |
| ALAC   | Apple ALAC| TODO          | Apache-2.0 |
| Vorbis | Tremor    | TODO          | BSD-2 |
| Opus   | libopus   | TODO          | BSD-2 |
| WAV    | (own)     | TODO          | (Apache-2.0, ours) |

## Adding a new codec

1. Create `codecs/<codec>/` with:
   - `LICENSE` — preserve the upstream license verbatim.
   - `vendor.sh` — script that re-fetches the upstream source, with
     the URL and pinned version recorded.
   - `<lib>.h` / `<lib>.c` (or single-header) — the upstream source.
   - `<codec>.h` / `<codec>.c` — our wrapper, exposing
     `<codec>_decoder_ops()`.
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
meson test -C build-sim     # runs the KAT
```

Or directly:

```bash
./build-sim/tests/codec_kat ./tests/codec-vectors
# expected: OK: flac decoded 44100 frames, 88200 samples bit-exact
```

The fixture (`tests/codec-vectors/sine_440hz_1s_44k_s16_stereo.flac`)
was produced by piping deterministic synthetic PCM (440 Hz sine, 1 s,
44.1 kHz, 16-bit stereo, ±16000 amplitude) into the FLAC reference
encoder. The KAT regenerates the same PCM in C and bit-compares against
dr_flac's output.
