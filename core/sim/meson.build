# core-sim — the host-side simulator binary.
#
# Links the HAL (sim backend) plus an entry point. Future PRs add the
# kernel, audio engine, codecs, and UI as additional dependencies.

executable(
  'core-sim',
  sources: ['main.c'],
  dependencies: [hal_dep],
)
