# Audio engine and the bitvmd daemon. Knows nothing about sona:
# its only input is the protocol from shared/.
alsa_dep = dependency('alsa', required: get_option('alsa'))
sndfile_dep = dependency('sndfile', required: get_option('sndfile'))

bitvm_inc = include_directories('include')

bitvm_lib = static_library(
  'bitvm',
  'src/buses.cpp',
  'src/engine.cpp',
  'src/graph.cpp',
  'src/node_tree.cpp',
  'src/time_map.cpp',
  include_directories: bitvm_inc,
  dependencies: [bitvm_proto_dep, threads_dep, sndfile_dep],
  cpp_args: sndfile_dep.found() ? ['-DBITVM_HAVE_SNDFILE=1'] : [],
)

bitvm_dep = declare_dependency(
  include_directories: bitvm_inc,
  link_with: bitvm_lib,
  dependencies: [bitvm_proto_dep, threads_dep, sndfile_dep],
)

executable('bitvmd', 'bitvmd/main.cpp', dependencies: bitvm_dep, install: true)

if gtest_dep.found()
  subdir('tests')
endif
