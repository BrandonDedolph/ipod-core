# Top-level Meson build for the core firmware.
#
# Two outputs from one source tree:
#   - core.ipod  : ARM cross-compile (the firmware that ships on device)
#   - core-sim   : host build, SDL-backed (for development on Linux/macOS)
#
# Pick the target with: meson setup build-hw  --cross-file cross/arm-none-eabi.txt -Dtarget=hw
#                       meson setup build-sim                                       -Dtarget=sim
#
# This file is intentionally minimal at the moment — subsystems land in
# subsequent PRs and will register themselves via subdir() calls below.

project(
  'core',
  'c',
  version: '0.0.1',
  license: 'Apache-2.0',
  default_options: [
    'c_std=gnu11',
    'warning_level=3',
    'werror=true',
    'b_lto=true',
    'optimization=s',          # -Os; size-optimized
  ],
)

target = get_option('target')

# Common warning/safety flags
# (warning_level=3 already implies -Wall -Wextra -Wpedantic; we add specifics.)
common_c_args = [
  '-ffunction-sections',
  '-fdata-sections',
  '-Wundef',
  '-Wshadow',
  '-Wno-unused-parameter',     # too noisy in driver stubs during scaffold
]

common_link_args = [
  '-Wl,--gc-sections',
]

if target == 'hw'
  # ARM cross-build. Toolchain + ARM-specific flags live in
  # cross/arm-none-eabi.txt under [built-in options]; we add only the
  # freestanding-correctness flags that don't belong in the cross file.
  common_c_args += [
    '-fno-builtin',
    '-fno-common',
    '-fno-stack-protector',
  ]
  add_project_arguments(common_c_args, language: 'c')
  add_project_link_arguments(common_link_args, language: 'c')
  message('Configured for hardware target (ARM, iPod 5G/5.5G)')

elif target == 'sim'
  # Host build. SDL2 provides LCD window + audio out + input.
  sdl2_dep = dependency('sdl2', required: true)
  add_project_arguments(common_c_args, language: 'c')
  add_project_link_arguments(common_link_args, language: 'c')
  message('Configured for sim target (host, SDL2)')

else
  error('Unknown target "@0@" (use -Dtarget=hw or -Dtarget=sim)'.format(target))
endif

subdir('hal')         # hardware abstraction layer (sim or hw backend)

if target == 'hw'
  # Bare-metal ARM build. boot/ + kernel/ are all that's freestanding-
  # clean today; codecs, audio engine, tagcache, and UI still pull in
  # host libc and land in subsequent PRs as they're brought up.
  subdir('boot')
  subdir('kernel')

  core_elf = executable(
    'core.elf',
    boot_sources + kernel_sources,
    dependencies: [hal_dep],     # hal/hw: SER0 debug UART
    link_args: [
      '-Wl,-T,' + (meson.current_source_dir() / 'boot' / 'linker.ld'),
      '-Wl,-Map=core.map',
    ],
    link_depends: boot_linker_script,
  )

  # Flat binary derived from the ELF — the host-side `core firmware pack`
  # wraps this in the .ipod transport format. Both products are byproducts
  # of `make hw`; the Makefile's `ipod` target does the .ipod wrap.
  objcopy = find_program('objcopy')
  custom_target(
    'core.bin',
    input: core_elf,
    output: 'core.bin',
    command: [objcopy, '-O', 'binary', '@INPUT@', '@OUTPUT@'],
    build_by_default: true,
  )

elif target == 'sim'
  subdir('codecs')      # vendored audio decoders (FLAC + MP3)
  subdir('apps/audio')  # playback engine: decoder_t -> ring -> hal_audio
  subdir('apps/db')     # tagcache (music library index)
  subdir('apps/ui')     # Cabinet shell, list view, font
  subdir('sim')         # core-sim entry point
  subdir('tests')       # codec KAT + .tcdb reader + audio playback
endif

# Future subsystems land here as they're written:
#
#   target == 'hw':  fs/ (FAT32), then codecs/apps once freestanding-clean
#   either:          kernel/ scheduler, IRQ table
