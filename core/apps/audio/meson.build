# Audio playback engine — bridges decoder_t to hal_audio_*.

audio_engine_lib = static_library(
  'core_audio_engine',
  sources: ['engine.c'],
  dependencies: [hal_dep, codecs_dep],
  include_directories: include_directories('.'),
)

audio_engine_dep = declare_dependency(
  link_with: audio_engine_lib,
  include_directories: include_directories('.'),
  dependencies: [hal_dep, codecs_dep],
)
