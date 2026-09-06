typehash_sources = files(
    'typehash.c',
    'th_access.c',
    'th_infer.c',
    'th_print.c',
)

if have_nfc
    typehash_sources += files('th_nfc_utf8proc.c')
endif

typehash_lib = library(
    'typehash',
    typehash_sources,
    dependencies: [xxhash_dep] + (have_nfc ? [utf8proc_dep] : []),
    include_directories: include_directories('.'),
    install: true,
)

install_headers('typehash.h')

typehash_dep = declare_dependency(
    link_with: typehash_lib,
    include_directories: include_directories('.'),
    dependencies: have_nfc ? [utf8proc_dep] : [],
)

pkg = import('pkgconfig')
pkg.generate(
    typehash_lib,
    description: 'Structural type identity hashing',
)

th_cycle_test = executable(
    'th_cycle_test',
    'th_cycle_test.c',
    dependencies: typehash_dep,
)

th_infer_cycle_test = executable(
    'th_infer_cycle_test',
    'th_infer_cycle_test.c',
    dependencies: typehash_dep,
)

test('inference-cycles', th_infer_cycle_test)
