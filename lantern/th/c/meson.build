typehash_sources = files(
    'typehash.c',
    'th_access.c',
    'th_error.c',
    'th_infer.c',
    'th_print.c',
)

if have_nfc
    typehash_sources += files('th_nfc_utf8proc.c')
endif

install_headers('typehash.h')

th_inc  = [include_directories('.')]
th_deps = [xxhash_dep] + (have_nfc ? [utf8proc_dep] : [])

th_test_dir = meson.current_source_dir()
