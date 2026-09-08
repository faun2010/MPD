dvdaiso = static_library(
  'dvdaiso',
  'audio_stream.cpp',
  'audio_stream_info.cpp',
  'mlp_audio_stream.cpp',
  'pcm_audio_stream.cpp',
  'audio_track.cpp',
  'dvda_block.cpp',
  'dvda_disc.cpp',
  'dvda_filesystem.cpp',
  'dvda_media.cpp',
  'dvda_metabase.cpp',
  'dvda_zone.cpp',
  'log_trunk.cpp',
  'udf/dvd_input.cpp',
  'udf/dvd_reader.cpp',
  'udf/dvd_udf.cpp',
  include_directories: inc,
  dependencies: [
    ixml_dep,
    gcrypt_dep,
  ],
)

dvdaiso_dep = declare_dependency(
  link_with: dvdaiso,
)
