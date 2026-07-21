# Freestanding C primitives (no libc). memcpy/memset/memmove/memcmp — needed
# by vendored codec code (dr_flac) and by any compiler-emitted calls.
#
# Built as a non-LTO static library on purpose: with whole-program LTO these
# are only *referenced* from the separately-compiled codec, so LTO would
# dead-strip them and the final link fails with "undefined reference to
# memcpy". A plain archive keeps them as real, always-linkable symbols.
mem_lib = static_library(
  'core_lib_mem',
  files('mem.c'),
  override_options: ['b_lto=false'],
)
mem_dep = declare_dependency(link_with: mem_lib)
