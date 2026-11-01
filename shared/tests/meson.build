# Each entry is a gtest file <name>.cpp in this directory.
proto_tests = [
]

foreach name : proto_tests
  test(
    name,
    executable(name, name + '.cpp', dependencies: [bitvm_proto_dep, gtest_dep]),
    suite: 'shared',
  )
endforeach
