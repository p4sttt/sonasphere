# Each entry is a gtest file <name>.cpp in this directory.
proto_tests = [
  'catalog_hash',
  'codec_reject',
  'codec_roundtrip',
  'graphdef_codec',
  'graphdef_validate',
  'text_errors',
  'text_golden',
  'transport_path',
  'transport_socketpair',
  'types_names',
  'validate_rules',
]

test_args = ['-DBITVM_TEST_DATA_DIR="' + meson.current_source_dir() / 'data' + '"']

foreach name : proto_tests
  test(
    name,
    executable(
      name,
      [name + '.cpp', 'samples.cpp'],
      dependencies: [bitvm_proto_dep, gtest_dep],
      cpp_args: test_args,
    ),
    suite: 'shared',
  )
endforeach
