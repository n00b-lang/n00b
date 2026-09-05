typehash_sources = files(
    'typehash.c',
    'th_parse.c',
    'th_access.c',
    'th_infer.c',
)

typehash_lib = library(
    'typehash',
    typehash_sources,
    dependencies: xxhash_dep,
    include_directories: include_directories('.'),
    install: true,
)

install_headers('typehash.h')

typehash_dep = declare_dependency(
    link_with: typehash_lib,
    include_directories: include_directories('.'),
)

pkg = import('pkgconfig')
pkg.generate(
    typehash_lib,
    description: 'Structural type identity hashing',
)

nfc_sources = have_nfc ? files('th_nfc_utf8proc.c') : files()
nfc_deps = have_nfc ? [utf8proc_dep] : []
nfc_args = have_nfc ? ['-DTH_TOOL_NFC'] : []

th_tool = executable(
    'th_tool',
    ['th_tool.c'] + nfc_sources,
    c_args: nfc_args,
    dependencies: [typehash_dep] + nfc_deps,
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

if have_nfc
    th_nfc_test = executable(
        'th_nfc_test',
        ['th_nfc_test.c'] + nfc_sources,
        dependencies: [typehash_dep] + nfc_deps,
    )

    test('nfc-normalizer', th_nfc_test)
endif
