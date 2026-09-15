project(
    'lantern (Language Tookit, er... N00bs)',
    'c',
    version: '0.1.0',
    license: 'MIT',
    meson_version: '>=1.1.0',
    default_options: [
        'c_std=c2x',
        'warning_level=2',
        'default_library=static',
    ],
)

cc = meson.get_compiler('c')

lantern_inc = [
    include_directories('include'),
    include_directories('include/vendor', is_system: true),
]

utf8proc_dep = dependency('libutf8proc', required: get_option('nfc'))
have_nfc     = utf8proc_dep.found()

lantern_deps = have_nfc ? [utf8proc_dep] : []

subdir('src')
subdir('tests')

summary(
    {
        'xxhash': 'vendored (include/vendor/xxhash.h)',
        'NFC via utf8proc': have_nfc,
    },
    section: 'dependencies',
)



