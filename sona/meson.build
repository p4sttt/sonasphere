# Language, composition and REPL. Talks to bitvm only through
# bitvm_proto_dep — never add bitvm_dep here.
sona_inc = include_directories('include')

# header-only until the first library .cpp is added.
sona_dep = declare_dependency(
  include_directories: sona_inc,
  dependencies: [bitvm_proto_dep, threads_dep],
)

executable('sona', 'app/main.cpp', dependencies: sona_dep, install: true)

if gtest_dep.found()
  subdir('tests')
endif
