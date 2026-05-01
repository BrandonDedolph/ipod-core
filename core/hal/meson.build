# Hardware Abstraction Layer.
#
# hal.h is the contract; per-target implementations live in hw/ and sim/.
# The top-level meson.build picks the right subdir based on -Dtarget.

if target == 'sim'
  subdir('sim')
elif target == 'hw'
  # subdir('hw')   # lands in PR introducing register-touching drivers
  message('hal/hw not yet populated — declaring stub hal_dep so subdirs can link')
  hal_dep = declare_dependency(
    include_directories: include_directories('.'),
  )
endif
