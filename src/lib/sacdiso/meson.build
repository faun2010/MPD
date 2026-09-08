if is_windows
  # avoid the unused libwinpthread-1.dll dependency on Windows; MPD
  # doesn't use the pthread API on Windows, but this is what Meson
  # unhelpfully detects for us
  threads_dep = []
else
  threads_dep = dependency('threads')
endif

conf.set('HAVE_PTHREAD_SETNAME_NP', compiler.has_function('pthread_setname_np', dependencies: threads_dep))

sacdiso = static_library(
  'sacdiso',
  'sacd_disc.cpp',
  'sacd_dsdiff.cpp',
  'sacd_media.cpp',
  'sacd_metabase.cpp',
  'scarletbook.cpp',
  'log_printf.cpp',
  'libdstdec/binding/dst_decoder.cpp',
  'libdstdec/decoder/decoder.cpp',
  include_directories: inc,
  dependencies: [
    ixml_dep,
    gcrypt_dep,
    threads_dep,
  ],
)

sacdiso_dep = declare_dependency(
  link_with: sacdiso,
)
