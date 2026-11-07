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

# fuzz targets: decode and the text parsers. each runs as a short smoke test over the
# seed corpus in tests/data/corpus; run them longer by hand, e.g.
#   build-fuzz/shared/fuzz_decode /tmp/corpus shared/tests/data/corpus/decode -max_total_time=60
# a crash input goes into the seed directory as a regression case
if get_option('fuzz')
  # libFuzzer writes new inputs into its first corpus directory, so that one is a scratch
  # directory in the build tree and the seeds in the source tree stay read-only
  mkdir = find_program('mkdir')
  foreach name : ['fuzz_decode', 'fuzz_text']
    fuzz_exe = executable(
      name,
      'tests/fuzz/' + name + '.cpp',
      dependencies: bitvm_proto_dep,
      link_args: ['-fsanitize=fuzzer'],
    )
    scratch = meson.current_build_dir() / (name + '_corpus')
    run_command(mkdir, '-p', scratch, check: true)
    test(
      name,
      fuzz_exe,
      args: [
        scratch,
        meson.current_source_dir() / 'tests/data/corpus' / name.replace('fuzz_', ''),
        '-runs=20000',
        '-error_exitcode=1',  # the default, 77, would read as "skipped" to meson
        '-artifact_prefix=' + meson.current_build_dir() + '/',
      ],
      suite: 'fuzz',
      timeout: 120,
    )
  endforeach
endif
