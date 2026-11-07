# Each entry is a gtest file <name>.cpp in this directory.
bitvm_tests = [
  'core',
  'dsp',
  'offline',
  'rt',
]

foreach name : bitvm_tests
  test(
    name,
    executable(
      name,
      name + '.cpp',
      dependencies: [bitvm_dep, gtest_dep],
      link_args: name == 'rt' ? [
        '-Wl,--wrap=malloc', '-Wl,--wrap=calloc', '-Wl,--wrap=realloc', '-Wl,--wrap=free',
      ] : [],
    ),
    suite: 'bitvm',
  )
endforeach
