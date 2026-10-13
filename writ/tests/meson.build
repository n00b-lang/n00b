source_root = meson.project_source_root()
golden = files('run_golden.py')
sanitize = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
compiler = meson.get_compiler('c').cmd_array()[0]
zlib = dependency('zlib')

test(
    'api-session',
    executable('api-session', 'api/session.c', dependencies: writ_dep, c_args: sanitize, link_args: sanitize),
    workdir: source_root,
)

test(
    'host-embedded',
    executable('host-embedded', 'host/embedded.c', dependencies: writ_dep, c_args: sanitize, link_args: sanitize),
)

test(
    'core-internals',
    executable(
        'core-internals',
        'core/internals.c',
        include_directories: writ_inc,
        dependencies: [writ_dep, lantern_headers],
    ),
)

external_libs = []
if get_option('b_sanitize') != 'none'
    external_libs += '-fsanitize=' + get_option('b_sanitize')
endif
lantern_pc = run_command(
    find_program('pkg-config'),
    '--libs',
    'ir',
    env: {'PKG_CONFIG_PATH': lantern_prefix / 'lib' / 'pkgconfig'},
    check: true,
)
foreach arg : lantern_pc.stdout().strip().split()
    if arg != '-lir'
        external_libs += arg
    endif
endforeach

test(
    'host-isolation',
    find_program('host/isolation.sh'),
    args: [compiler, libwrit, source_root / 'include', meson.current_build_dir() / 'isolation'] + external_libs,
    depends: libwrit,
)

foreach standard : [['c11', 'c'], ['c++17', 'c++']]
    test(
        'headers-' + standard[0],
        find_program(compiler),
        args: [
            '-std=' + standard[0],
            '-pedantic-errors',
            '-Werror',
            '-fsyntax-only',
            '-x', standard[1],
            '-I' + source_root / 'include',
            source_root / 'tests' / 'host' / 'headers.h',
        ],
    )
endforeach

test(
    'model-header-current',
    python,
    args: [source_root / 'tools' / 'gen_model.py', source_root / 'defs' / 'model_fields.def', source_root / 'include' / 'writ' / 'model.h', '--check'],
)

keyword_corpus = custom_target(
    'keyword-corpus',
    input: source_root / 'grammar' / 'writ.bnf',
    output: 'keywords.writ',
    command: [python, source_root / 'tools' / 'keyword_corpus.py', '@INPUT@', '@OUTPUT@'],
)

test('keywords-are-identifiers', writ_exe, args: ['parse', keyword_corpus], depends: keyword_corpus)

test(
    'plan-golden',
    python,
    args: [golden, source_root / 'tests' / 'plan' / 'zlib.plan', '--', writ_exe, 'plan', 'tests/skeleton/zlib.writ'],
)

foreach name : ['unknown_type', 'not_pure', 'bad_phase', 'out_unbounded', 'syntax', 'duplicate']
    test(
        'check-' + name,
        python,
        args: [
            golden,
            source_root / 'tests' / 'check' / name + '.out',
            '--fails',
            '--',
            writ_exe,
            'check',
            'tests/check' / name + '.writ',
        ],
    )
endforeach

skeleton = custom_target(
    'skeleton-binding',
    input: source_root / 'tests' / 'skeleton' / 'zlib.writ',
    output: ['zlib_writ.h', 'zlib_writ.c', 'zlib_writ_mock.c', 'zlib_writ_conformance.c'],
    command: [writ_exe, 'gen', 'c', '@INPUT@', '-o', '@OUTDIR@', '--conformance'],
)

generated_c = ['-std=c11', '-Wall', '-Wextra', '-Werror']

test(
    'skeleton-conformance',
    executable(
        'skeleton-conformance',
        skeleton[1],
        skeleton[2],
        skeleton[3],
        c_args: generated_c + sanitize,
        link_args: sanitize,
        override_options: ['c_std=c11'],
    ),
)

test(
    'skeleton-roundtrip',
    executable(
        'skeleton-roundtrip',
        'skeleton/roundtrip.c',
        skeleton[0],
        skeleton[1],
        include_directories: include_directories('.'),
        c_args: generated_c + sanitize,
        link_args: sanitize,
        dependencies: zlib,
        override_options: ['c_std=c11'],
    ),
)

tools = source_root / 'tools'
foreach check : [
    ['check-alloc', 'check_alloc.py', [source_root / 'src'], false],
    ['check-alloc-planted', 'check_alloc.py', [source_root / 'tests' / 'enforce' / 'alloc'], true],
    ['check-containers', 'check_containers.py', [source_root / 'src'], false],
    ['check-containers-planted', 'check_containers.py', [source_root / 'tests' / 'enforce' / 'containers'], true],
    ['check-dupes', 'dupes.py', [source_root / 'src'], false],
    ['check-dupes-planted', 'dupes.py', [source_root / 'tests' / 'enforce' / 'dupes'], true],
    ['check-budget', 'loc_budget.py', [source_root / 'implementation.md', source_root / 'src'], false],
    ['check-budget-planted', 'loc_budget.py', [source_root / 'tests' / 'enforce' / 'budget' / 'plan.md', source_root / 'src'], true],
]
    test(check[0], python, args: [tools / check[1]] + check[2], should_fail: check[3])
endforeach
