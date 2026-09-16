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

# ---------- meta_text (shared tag-text policy) ------------------------

# Ours, not vendored: the one rule for what bytes reach a display field,
# used by both the FLAC and the ID3 reader. Its own library so neither
# reader's archive can shadow the other's copy at link time.
meta_text_lib = static_library(
  'core_meta_text',
  sources: ['meta_text.c'],
  include_directories: include_directories('.'),
)

meta_text_dep = declare_dependency(
  link_with: meta_text_lib,
  include_directories: include_directories('.'),
)

# ---------- pvmp3 (MP3) -----------------------------------------------

# The vendored fixed-point decoder. The host build has no ARM assembly, so it
# compiles the four C translation units the hw build replaces with upstream/asm
# (pvmp3_common_c and friends are declared in ../meson.build, where both
# targets can see them). The C-equivalent fixed-point ops round a hair
# differently from the smull path — see codecs/pvmp3/README.md and the
# tolerance rule in tests/codec-vectors/README.md.
#
# The -fno-sanitize list is not a papering-over. A fixed-point codec computes
# in wrapped two's-complement on purpose: the Q-format multiplies shift
# negative values and the accumulators are meant to wrap, and upstream says so
# by marking them __attribute__((no_sanitize("integer"))) — a clang-only
# spelling our port turns into nothing on gcc. Under the CI sanitizer job
# (-Db_sanitize=address,undefined, halt_on_error=1) UBSan's arithmetic checks
# would abort on the first IMDCT butterfly of the first frame. ADDRESS
# sanitizing stays fully on for this library, which is the check that matters
# for a parser fed a user's disk, and the arithmetic ones stay on everywhere
# else in the tree.
pvmp3_upstream_lib = static_library(
  'core_codec_pvmp3',
  sources: pvmp3_common_c + pvmp3_asm_replaced_c,
  include_directories: include_directories('.', 'pvmp3'),
  c_args: vendored_codec_warning_suppress + [
    '-Wno-type-limits',
    '-fno-sanitize=shift-base,shift-exponent,signed-integer-overflow',
  ],
  override_options: ['werror=false'],
)

# Our layer on top: the decoder_ops wrapper, the framing/Xing/seek maths and
# the ID3 reader. Stays under -Werror.
mp3_lib = static_library(
  'core_codec_mp3',
  sources: pvmp3_wrapper_c,
  include_directories: include_directories('.', 'pvmp3'),
)

mp3_dep = declare_dependency(
  link_with: [mp3_lib, pvmp3_upstream_lib],
  dependencies: [meta_text_dep],
  include_directories: include_directories('.', 'pvmp3'),
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
  dependencies: [flac_dep, mp3_dep, image_dep, meta_text_dep],
  include_directories: include_directories('.'),
)
