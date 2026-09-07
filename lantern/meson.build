project(
    'ir',
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

ir_inc = include_directories('include')

xxhash_dep = dependency('libxxhash', required: false)

if not xxhash_dep.found()
    xxhash_include = get_option('xxhash_include')

    if xxhash_include != ''
        xxhash_dep = declare_dependency(
            compile_args: ['-isystem', xxhash_include],
        )
    elif cc.has_header('xxhash.h')
        xxhash_dep = declare_dependency()
    else
        error(
            'xxhash.h not found. Install libxxhash, or configure with\n' +
            '  meson setup build -Dxxhash_include=/path/to/dir',
        )
    endif
endif

utf8proc_dep = dependency('libutf8proc', required: get_option('nfc'))
have_nfc     = utf8proc_dep.found()

ir_deps = [xxhash_dep] + (have_nfc ? [utf8proc_dep] : [])

subdir('src')
subdir('tests')

summary(
    {
        'xxhash': xxhash_dep.found(),
        'NFC via utf8proc': have_nfc,
    },
    section: 'dependencies',
)
