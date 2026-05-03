# core-sim — the host-side simulator binary.
#
# Links the HAL (sim backend), audio engine, codecs, UI, plus an entry
# point. Future PRs may add a kernel layer.

cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required: false)

core_sim_target = executable(
  'core-sim',
  sources: ['main.c'],
  dependencies: [hal_dep, audio_engine_dep, flac_dep, ui_dep, m_dep],
)
