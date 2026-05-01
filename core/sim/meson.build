# core-sim — the host-side simulator binary.
#
# Links the HAL (sim backend) plus an entry point. Future PRs add the
# kernel, audio engine, codecs, and UI as additional dependencies.

cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required: false)   # libm for sinf() in the demo

executable(
  'core-sim',
  sources: ['main.c'],
  dependencies: [hal_dep, m_dep],
)
