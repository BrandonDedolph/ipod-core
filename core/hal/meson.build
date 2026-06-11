# Hardware Abstraction Layer.
#
# hal.h is the contract; per-target implementations live in hw/ and sim/.
# The top-level meson.build picks the right subdir based on -Dtarget.

if target == 'sim'
  subdir('sim')
elif target == 'hw'
  subdir('hw')
endif
