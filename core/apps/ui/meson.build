# Cabinet UI — atlas-based text, chrome primitives, list view, shell.
#
# Atlases (Nunito at fixed sizes) are pre-rasterized at build-time by
# tools/atlas_gen.sh. The generated headers in atlas/ are committed
# to the repo so the C build doesn't need Pillow.

ui_lib = static_library(
  'core_ui',
  sources: [
    'atlas.c',
    'chrome.c',
    'list.c',
    'theme.c',
    'now_playing.c',
    'search.c',
    'cabinet.c',
  ],
  dependencies: [hal_dep, audio_engine_dep, flac_dep, image_dep, tagcache_dep],
  include_directories: include_directories('.'),
)

ui_dep = declare_dependency(
  link_with: ui_lib,
  include_directories: include_directories('.'),
  dependencies: [hal_dep, audio_engine_dep, flac_dep, image_dep, tagcache_dep],
)
