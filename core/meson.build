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
  subdir('fs')
  subdir('lib')

  # FLAC decoder (dr_flac) built freestanding for the target: no libc — a
  # static arena backs allocation and our lib/mem.c provides memcpy/memset
  # (see codecs/dr_flac/flac.c's CORE_FREESTANDING block). Warnings are
  # suppressed and werror disabled for the vendored single-header source,
  # which we don't own; arena.c + mem.c are ours and stay under -Werror.
  codec_warn_suppress = [
    '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-unused-variable',
    '-Wno-sign-compare', '-Wno-shadow', '-Wno-undef', '-Wno-cast-align',
    '-Wno-implicit-fallthrough', '-Wno-misleading-indentation',
  ]
  codec_inc = include_directories('codecs', 'codecs/dr_flac', 'codecs/dr_mp3', 'lib')
  flac_hw_lib = static_library(
    'core_codec_flac_hw',
    sources: ['codecs/dr_flac/flac.c'],
    c_args: ['-DCORE_FREESTANDING'] + codec_warn_suppress,
    include_directories: codec_inc,
    override_options: ['werror=false'],
  )

  # MP3 decoder (dr_mp3) built freestanding, same recipe as flac_hw_lib.
  # Unlike FLAC, MP3's synthesis filter is floating point, but dr_mp3 calls
  # no libm — the float ops lower to libgcc soft-float (__aeabi_*), already
  # linked via -lgcc below. Vendored source: warnings suppressed, werror off;
  # sign-compare/type-limits are extra for dr_mp3 (mirrors codecs/meson.build).
  mp3_hw_lib = static_library(
    'core_codec_mp3_hw',
    sources: ['codecs/dr_mp3/mp3.c'],
    c_args: ['-DCORE_FREESTANDING'] + codec_warn_suppress + [
      '-Wno-sign-compare', '-Wno-type-limits',
    ],
    include_directories: codec_inc,
    override_options: ['werror=false'],
  )

  # Read-ahead byte-source shim (ours, not vendored): collapses a decoder's
  # many tiny reads into a few large disk reads. Built freestanding so it pulls
  # lib/mem.h's memcpy (via CORE_FREESTANDING); stays under -Werror since we own
  # it and it's warning-clean.
  readahead_hw_lib = static_library(
    'core_readahead_hw',
    sources: ['codecs/readahead.c'],
    c_args: ['-DCORE_FREESTANDING'],
    include_directories: codec_inc,
  )

  # Freestanding antialiased text renderer (ours): draws Nunito type from the
  # pre-rasterized atlas/*.h data into an RGB565 framebuffer with a gamma-correct
  # integer blend — no libc/libm/malloc. Same text.c the host text_test compiles;
  # links into core.elf so the on-device UI can draw real type. Uses only relative
  # includes (ui/atlas.h -> ../hal/hal.h), so no extra include dirs; stays
  # under -Werror since we own it and it's warning-clean.
  text_hw_lib = static_library(
    'core_text_hw',
    sources: ['ui/text.c'],
    c_args: ['-DCORE_FREESTANDING'],
  )

  # Background streaming playback engine (ours): the FLAC/MP3 decode path, the
  # SPSC PCM ring feeding the DMA DAC, the fat32 byte-source + read-ahead glue,
  # album art, and the queue/auto-advance player — extracted out of kernel/main.c
  # into core/player/. Freestanding (its struct-copy lowers to lib/mem.c's memcpy,
  # resolved at the final link against mem_dep). Needs the hal include dir for
  # hw/*.h + hal.h; codec/kernel/fs headers are reached via relative includes.
  player_hw_lib = static_library(
    'core_player_hw',
    sources: ['player/player.c'],
    include_directories: include_directories('hal', 'codecs', 'codecs/dr_flac', 'codecs/dr_mp3'),
  )

  core_elf = executable(
    'core.elf',
    boot_sources + kernel_sources + fs_sources + files('codecs/arena.c'),
    dependencies: [hal_dep, mem_dep],   # hal/hw: SER0 UART; mem: freestanding memcpy/…
    link_with: [flac_hw_lib, mp3_hw_lib, readahead_hw_lib, text_hw_lib, player_hw_lib],
    include_directories: codec_inc,
    link_args: [
      '-Wl,-T,' + (meson.current_source_dir() / 'boot' / 'linker.ld'),
      '-Wl,-Map=core.map',
      # libgcc: the ARM7TDMI has no hardware divide, so integer division
      # (fat32 sector math, playback timing) needs the compiler runtime's
      # __aeabi_*divmod helpers. This is the compiler runtime, NOT libc.
      '-lgcc',
      # The pcm_ring SPSC indices use __atomic acquire/release ordering.
      # On ARMv4T (no barrier instruction) GCC lowers the fence to a
      # __sync_synchronize libcall; sync-none selects its no-op form. That
      # is correct HERE: the ring's producer (foreground pump) and consumer
      # (DMA ISR) run on the SAME core — ARM7TDMI is in-order and strongly
      # ordered, and the DMA engine reads the PCM buffer, never the indices,
      # so no cross-observer hardware fence is needed. The __atomic builtins
      # still emit the compiler-ordering barrier we actually rely on. Absent
      # this spec the stub links anyway but warns on every build.
      '-specs=sync-none.specs',
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
  subdir('tests')       # host unit tests for the freestanding firmware code
endif

# Future subsystems land here as they're written:
#
#   target == 'hw':  fs/ (FAT32), then codecs/apps once freestanding-clean
#   either:          kernel/ scheduler, IRQ table
