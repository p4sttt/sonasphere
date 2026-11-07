# bitvm protocol: the only code both sona and bitvm see.
# No audio, DSP or sona grammar here.
bitvm_proto_inc = include_directories('include')

bitvm_proto_sources = files(
  'src/catalog.cpp',
  'src/codec.cpp',
  'src/graphdef.cpp',
  'src/messages.cpp',
  'src/text.cpp',
  'src/text_graph.cpp',
  'src/text_print.cpp',
  'src/text_util.cpp',
  'src/transport.cpp',
  'src/types.cpp',
  'src/validate.cpp',
)

bitvm_proto_lib = static_library(
  'bitvm-proto',
  bitvm_proto_sources,
  include_directories: bitvm_proto_inc,
)

bitvm_proto_dep = declare_dependency(
  include_directories: bitvm_proto_inc,
  link_with: bitvm_proto_lib,
)

if gtest_dep.found()
  subdir('tests')
endif
