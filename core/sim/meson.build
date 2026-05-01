# core-sim — the host-side simulator binary.
#
# Links the HAL (sim backend), audio engine + codecs, plus an entry
# point. Future PRs add the kernel and UI as additional dependencies.

cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required: false)

executable(
  'core-sim',
  sources: ['main.c'],
  dependencies: [hal_dep, audio_engine_dep, flac_dep, m_dep],
)
