# Audio engine and the bitvmd daemon. Knows nothing about sona:
# its only input is the protocol from shared/.
alsa_dep = dependency('alsa', required: get_option('alsa'))
sndfile_dep = dependency('sndfile', required: get_option('sndfile'))

bitvm_inc = include_directories('include')

# Header-only until the first .cpp lands (M2), same as shared/.
# ALSA backend sources join the library only when alsa_dep.found().
bitvm_dep = declare_dependency(
  include_directories: bitvm_inc,
  dependencies: [bitvm_proto_dep, threads_dep, alsa_dep, sndfile_dep],
)

executable('bitvmd', 'bitvmd/main.cpp', dependencies: bitvm_dep, install: true)

if gtest_dep.found()
  subdir('tests')
endif
