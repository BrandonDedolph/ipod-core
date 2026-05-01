# Test harness: codec known-answer tests.
#
# Builds codec_kat as a host executable; the fixture directory is
# passed at runtime. `meson test` picks them up.

if target == 'sim'
  codec_kat = executable(
    'codec_kat',
    sources: ['codec_kat.c'],
    dependencies: [codecs_dep],
    # libm for sin() in the reference signal.
    link_args: ['-lm'],
  )

  vectors_dir = meson.current_source_dir() / 'codec-vectors'
  test('codec-kat-flac', codec_kat,
       args: [vectors_dir],
       suite: 'unit')
endif
