# SDL2-backed HAL implementation for host builds.
hal_lib = static_library(
  'core_hal_sim',
  sources: ['sim_hal.c'],
  dependencies: [sdl2_dep],
  include_directories: include_directories('..'),
)

hal_dep = declare_dependency(
  link_with: hal_lib,
  include_directories: include_directories('..'),
  dependencies: [sdl2_dep],
)
