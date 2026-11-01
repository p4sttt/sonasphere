# Each entry is a gtest file <name>.cpp in this directory.
bitvm_tests = [
]

foreach name : bitvm_tests
  test(
    name,
    executable(name, name + '.cpp', dependencies: [bitvm_dep, gtest_dep]),
    suite: 'bitvm',
  )
endforeach
