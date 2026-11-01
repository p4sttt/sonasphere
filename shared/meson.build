# bitvm protocol: the only code both sona and bitvm see.
# No audio, DSP or sona grammar here.
bitvm_proto_inc = include_directories('include')

# Header-only until the first .cpp lands (M1). Then:
#   bitvm_proto_lib = static_library('bitvm-proto', files(...), include_directories: bitvm_proto_inc)
# and add `link_with: bitvm_proto_lib` below.
bitvm_proto_dep = declare_dependency(include_directories: bitvm_proto_inc)

if gtest_dep.found()
  subdir('tests')
endif
