# Vendored audio codecs.
#
# Each codec is a static_library wrapping its upstream source. The
# decoder.h ABI lives next to this file and is what the audio engine
# depends on; codecs/<name>/<name>.h re-exports the per-codec ops getter.

# Per-vendored-lib warning suppression. We don't own the upstream source,
# so silence its noise rather than maintain a fork.
vendored_codec_warning_suppress = [
  '-Wno-unused-parameter',
  '-Wno-unused-function',
  '-Wno-unused-variable',
  '-Wno-strict-prototypes',
  '-Wno-shadow',
  '-Wno-undef',
  '-Wno-cast-align',
  '-Wno-implicit-fallthrough',
  '-Wno-misleading-indentation',
]

# ---------- dr_flac (FLAC) -------------------------------------------

flac_lib = static_library(
  'core_codec_flac',
  sources: ['dr_flac/flac.c', 'dr_flac/tag_flac.c'],
  include_directories: include_directories('.', 'dr_flac'),
  c_args: vendored_codec_warning_suppress,
)

flac_dep = declare_dependency(
  link_with: flac_lib,
  include_directories: include_directories('.', 'dr_flac'),
)

# ---------- dr_mp3 (MP3) ---------------------------------------------

mp3_lib = static_library(
  'core_codec_mp3',
  sources: ['dr_mp3/mp3.c', 'dr_mp3/tag_mp3.c'],
  include_directories: include_directories('.', 'dr_mp3'),
  c_args: vendored_codec_warning_suppress + [
    # dr_mp3 has additional warnings under -Wall + -Werror that
    # don't affect functionality; suppress the extras here.
    '-Wno-sign-compare',
    '-Wno-type-limits',
  ],
)

mp3_dep = declare_dependency(
  link_with: mp3_lib,
  include_directories: include_directories('.', 'dr_mp3'),
)

# ---------- stb_image (JPEG, album art) -------------------------------

image_lib = static_library(
  'core_codec_image',
  sources: ['stb_image/image.c'],
  include_directories: include_directories('.', 'stb_image'),
  c_args: vendored_codec_warning_suppress + [
    # stb_image trips a few additional warnings under -Wall + -Werror
    # (sign comparisons, type limits in the JPEG IDCT, etc) that are
    # cosmetic. Suppress them here; our wrapper code is small and
    # easy to keep clean.
    '-Wno-sign-compare',
    '-Wno-type-limits',
  ],
)

image_dep = declare_dependency(
  link_with: image_lib,
  include_directories: include_directories('.', 'stb_image'),
)

# ---------- aggregate -------------------------------------------------

codecs_dep = declare_dependency(
  dependencies: [flac_dep, mp3_dep, image_dep],
  include_directories: include_directories('.'),
)
