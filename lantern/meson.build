project(
    'lantern (Language Tookit, er... N00bs)',
    'c',
    version: '0.1.0',
    license: 'MIT',
    meson_version: '>=1.1.0',
    default_options: [
        'buildtype=debugoptimized',
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

icu_uc_dep   = dependency('icu-uc', required: false)
icu_i18n_dep = dependency('icu-i18n', required: false)
have_icu     = icu_uc_dep.found() and icu_i18n_dep.found()

if not have_icu and host_machine.system() == 'darwin'
    brew = find_program('brew', required: false)
    if brew.found()
        icu_prefix_result = run_command(brew, '--prefix', 'icu4c',
                                        check: false)
        if icu_prefix_result.returncode() == 0
            icu_prefix = icu_prefix_result.stdout().strip()
            icu_include = include_directories(icu_prefix / 'include',
                                              is_system: true)
            icu_uc_lib = cc.find_library('icuuc',
                                         dirs: [icu_prefix / 'lib'],
                                         required: false)
            icu_i18n_lib = cc.find_library('icui18n',
                                           dirs: [icu_prefix / 'lib'],
                                           required: false)
            if icu_uc_lib.found() and icu_i18n_lib.found()
                icu_uc_dep = declare_dependency(
                    dependencies: icu_uc_lib,
                    include_directories: icu_include,
                )
                icu_i18n_dep = declare_dependency(
                    dependencies: icu_i18n_lib,
                    include_directories: icu_include,
                )
                have_icu = true
            endif
        endif
    endif
endif

if not have_icu
    error('ICU (icu-uc and icu-i18n) is required for Unicode identifier profiles')
endif

lantern_deps += [icu_uc_dep, icu_i18n_dep]
add_project_arguments('-DLANTERN_HAVE_ICU=1', language: 'c')

subdir('src')
subdir('tests')

summary(
    {
        'xxhash': 'vendored (include/vendor/xxhash.h)',
        'NFC via utf8proc': have_nfc,
        'Unicode identifier profiles via ICU': have_icu,
    },
    section: 'dependencies',
)

