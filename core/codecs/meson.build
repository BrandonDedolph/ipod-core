# Vendored audio codecs.
#
# Each codec is a static_library wrapping its upstream source. The
# decoder.h ABI lives next to this file and is what the audio engine
# depends on; codecs/<name>/<name>.h re-exports the per-codec ops getter.

# dr_flac single-header wrapper.
flac_lib = static_library(
  'core_codec_flac',
  sources: ['dr_flac/flac.c'],
  include_directories: include_directories('.', 'dr_flac'),
  c_args: [
    # dr_flac throws lots of unused-parameter / strict-prototypes
    # warnings under -Wall/-Wextra; we don't own the source, so silence
    # the noise rather than maintain a fork.
    '-Wno-unused-parameter',
    '-Wno-unused-function',
    '-Wno-unused-variable',
    '-Wno-strict-prototypes',
    '-Wno-shadow',
    '-Wno-undef',
    '-Wno-cast-align',
    '-Wno-implicit-fallthrough',
    '-Wno-misleading-indentation',
  ],
)

flac_dep = declare_dependency(
  link_with: flac_lib,
  include_directories: include_directories('.', 'dr_flac'),
)

# Aggregate dependency for "all codecs," even if there's only one for now.
codecs_dep = declare_dependency(
  dependencies: [flac_dep],
  include_directories: include_directories('.'),
)
