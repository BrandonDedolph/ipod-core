# Tagcache (music library index).
#
# Depends on the codec tag readers (tag_flac, tag_mp3) so library_load
# can populate per-song titles/artists/albums during the scan. The
# audio decoder paths aren't pulled in here — only the tag readers.
# At link time `flac_dep` / `mp3_dep` bring in their full static
# libraries, which is fine; LTO + --gc-sections drops the audio path
# from any binary that doesn't use it.

tagcache_lib = static_library(
  'core_tagcache',
  sources: ['tagcache.c'],
  include_directories: include_directories('.'),
  dependencies: [flac_dep, mp3_dep],
)

tagcache_dep = declare_dependency(
  link_with: tagcache_lib,
  include_directories: include_directories('.'),
)
