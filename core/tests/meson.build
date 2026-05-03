# Test harness: codec known-answer tests.
#
# Builds codec_kat as a host executable; the fixture directory is
# passed at runtime. `meson test` picks them up.

if target == 'sim'
  # libm needed only on platforms where it's a separate library;
  # macOS and most BSDs link math via libSystem and reject -lm.
  cc = meson.get_compiler('c')
  m_dep = cc.find_library('m', required: false)

  codec_kat = executable(
    'codec_kat',
    sources: ['codec_kat.c'],
    dependencies: [codecs_dep, m_dep],
  )

  vectors_dir = meson.current_source_dir() / 'codec-vectors'
  # codec_kat runs every vendored codec's KAT in one binary; one
  # meson test entry covers all of them. Failures print which codec
  # failed via the [codec] label in stderr.
  test('codec-kat', codec_kat,
       args: [vectors_dir],
       suite: 'unit')

  # End-to-end audio-playback test: spawns core-sim with the SDL2
  # disk audio driver, plays the FLAC fixture, then bit-compares
  # the captured PCM to the codec_kat reference. Exercises the
  # tagcache → cabinet → audio_engine_play → ring → HAL audio
  # callback path that codec_kat alone can't reach.
  #
  # The codec-vectors/ source dir doubles as the --music scan dir:
  # it contains exactly one .flac and one .mp3, so title-sort puts
  # the .flac at row 0 (the press sequence selects row 0). Capture
  # is written under the meson temp dir so multiple test runs in
  # parallel can't clobber each other.
  sim_audio_playback = executable(
    'sim_audio_playback',
    sources: ['sim_audio_playback.c'],
  )

  test('sim-audio-playback', sim_audio_playback,
       args: [
         core_sim_target.full_path(),
         vectors_dir,        # fixture-dir (sim --music)
         vectors_dir,        # vectors-dir (where the reference PCM lives)
       ],
       depends: [core_sim_target],
       suite:   'integration',
       timeout: 30)
endif
