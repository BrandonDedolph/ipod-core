# Register-touching HAL backend for the iPod 5G/5.5G (PP5022).
#
# Freestanding ARM code only — no libc, no host dependencies. Grows one
# driver per PR: uart, lcd (this one), then clickwheel, ATA, audio.
hal_lib = static_library(
  'core_hal_hw',
  sources: ['uart.c', 'lcd.c', 'backlight.c', 'i2c.c', 'wm8758.c', 'i2s.c', 'dma.c', 'audio.c', 'ata.c', 'clickwheel.c', 'battery.c'],
  include_directories: include_directories('..'),
)

hal_dep = declare_dependency(
  link_with: hal_lib,
  include_directories: include_directories('..'),
)
