# Each entry is a gtest file <name>.cpp in this directory.
sona_tests = [
]

foreach name : sona_tests
  test(
    name,
    executable(name, name + '.cpp', dependencies: [sona_dep, gtest_dep]),
    suite: 'sona',
  )
endforeach
