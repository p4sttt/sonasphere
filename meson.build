project(
  'sonasphere',
  'cpp',
  version: '0.0.1',
  meson_version: '>=1.3.0',
  default_options: [
    'cpp_std=c++23',
    'warning_level=3',
    'buildtype=debugoptimized',
    'b_ndebug=if-release',
  ],
)

cpp = meson.get_compiler('cpp')
add_project_arguments(
  cpp.get_supported_arguments(
    '-Wshadow',
    '-Wnon-virtual-dtor',
    '-Wold-style-cast',
    '-Woverloaded-virtual',
    '-Wnull-dereference',
    '-Wdouble-promotion',
    '-Wimplicit-fallthrough',
  ),
  language: 'cpp',
)

if get_option('fuzz')
  if cpp.get_id() != 'clang'
    error('-Dfuzz=true needs clang (libFuzzer); use CXX=clang++')
  endif
  # instrument the library; the fuzz targets link the libFuzzer main themselves
  add_project_arguments('-fsanitize=fuzzer-no-link', language: 'cpp')
  add_project_link_arguments('-fsanitize=fuzzer-no-link', language: 'cpp')
endif

threads_dep = dependency('threads')
gtest_dep = dependency('gtest', main: true, required: get_option('tests'))

# order matters (bitvm and sona consume shared/)
subdir('shared')
subdir('bitvm')
subdir('sona')
