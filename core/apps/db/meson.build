# Tagcache (music library index).

tagcache_lib = static_library(
  'core_tagcache',
  sources: ['tagcache.c'],
  include_directories: include_directories('.'),
)

tagcache_dep = declare_dependency(
  link_with: tagcache_lib,
  include_directories: include_directories('.'),
)
