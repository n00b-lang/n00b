source_root = meson.project_source_root()
golden = files('run_golden.py')
sanitize = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
compiler = meson.get_compiler('c').cmd_array()[0]
zlib = dependency('zlib')
add_test_setup(
    'lanternleaks',
    env: {'LSAN_OPTIONS': 'suppressions=' + source_root / 'tests' / 'lsan.supp' + ':print_suppressions=0'},
    is_default: true,
)
host_triple = host_machine.system() == 'darwin' ? host_machine.cpu_family() + '-apple-darwin' : host_machine.cpu_family() + '-linux-gnu'

test(
    'api-session',
    executable('api-session', 'api/session.c', dependencies: writ_dep, c_args: sanitize, link_args: sanitize),
    workdir: source_root,
)

test(
    'host-embedded',
    executable('host-embedded', 'host/embedded.c', dependencies: writ_dep, c_args: sanitize, link_args: sanitize),
    args: [source_root],
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

external_libs = lantern_external
if get_option('b_sanitize') != 'none'
    external_libs += '-fsanitize=' + get_option('b_sanitize')
endif

foreach library : [['host-isolation', libwrit], ['host-isolation-shared', libwrit_shared]]
    test(
        library[0],
        find_program('host/isolation.sh'),
        args: [compiler, library[1], source_root / 'include', meson.current_build_dir() / library[0]] + external_libs,
        depends: library[1],
    )
endforeach

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
    'spec-examples',
    python,
    args: [
        golden,
        source_root / 'tests' / 'spec' / 'examples.out',
        '--',
        python.full_path(),
        source_root / 'tests' / 'spec_examples.py',
        writ_exe.full_path(),
        'dsl.md',
        'call-plan.md',
    ],
    depends: writ_exe,
)

test(
    'fmt-roundtrip',
    python,
    args: [
        source_root / 'tests' / 'fmt_roundtrip.py',
        writ_exe.full_path(),
        source_root / 'dsl.md',
        source_root / 'call-plan.md',
        source_root / 'tests' / 'libc' / 'libc.writ',
        source_root / 'contracts' / 'zlib.writ',
        source_root / 'contracts' / 'zlib' / host_triple + '.writ',
        source_root / 'tests' / 'skeleton' / 'zlib.writ',
        source_root / 'ffi-spec.md',
        source_root / 'contracts' / 'resharp.writ',
        source_root / 'tests' / 'resharp' / 'resharp.derived.writ',
        source_root / 'tests' / 'spans' / 'spans.writ',
        source_root / 'profiles' / 'c.writ',
        source_root / 'profiles' / 'rust.writ',
        source_root / 'profiles' / 'n00b.writ',
        source_root / 'tests' / 'bind' / 'kv.export.writ',
        source_root / 'tests' / 'embedded' / 'zlib_search.writ',
        source_root / 'tests' / 'embedded' / 'export.writ',
        source_root / 'tests' / 'embedded' / 'refine.writ',
    ],
    depends: writ_exe,
)

fuzz_flags = ['-fsanitize=fuzzer-no-link,address,undefined', '-fno-omit-frame-pointer']
fuzz_writ = static_library(
    'writ_fuzz',
    core_sources,
    grammar_h,
    json_grammar_h,
    keywords_h,
    model_names_h,
    model_json_h,
    include_directories: writ_inc,
    dependencies: lantern_headers,
    c_args: fuzz_flags + core_args,
)
fuzz_seeds = []
foreach seed : ['libc/libc.writ', 'zlib/zlib.writ', 'skeleton/zlib.writ',
                'resharp/resharp.derived.writ', 'bind/kv.n00b.writ', 'embedded/zlib_search.writ',
                'embedded/export.writ', 'embedded/e0510.writ']
    fuzz_seeds += source_root / 'tests' / seed
endforeach
fuzz_seeds += source_root / 'contracts' / 'zlib' / host_triple + '.writ'
test(
    'fuzz-parse',
    executable(
        'fuzz-parse',
        'fuzz/parse.c',
        link_with: fuzz_writ,
        include_directories: include_directories('../include'),
        dependencies: lantern_links,
        c_args: ['-fsanitize=fuzzer,address,undefined'],
        link_args: ['-fsanitize=fuzzer,address,undefined'],
    ),
    args: ['-runs=3000', '-max_len=2048', '-seed_inputs=' + ','.join(fuzz_seeds)],
    timeout: 600,
)

test(
    'grammar-audit',
    executable(
        'grammar-audit',
        'core/grammar_audit.c',
        include_directories: writ_inc,
        dependencies: [writ_dep, lantern_headers],
    ),
)

foreach contract : ['zlib', 'libc']
    test(
        contract + '-plans',
        python,
        args: [golden, source_root / 'tests' / 'plan' / contract + '.full.plan', '--', writ_exe, '--platform',
               'aarch64-apple-darwin', 'plan', 'tests' / contract / contract + '.writ'],
    )
endforeach

test(
    'lower-golden',
    python,
    args: [golden, source_root / 'tests' / 'lower' / 'zlib.json', '--', writ_exe, '--platform', 'aarch64-apple-darwin',
           'lower', 'tests/skeleton/zlib.writ'],
)

test(
    'plan-golden',
    python,
    args: [golden, source_root / 'tests' / 'plan' / 'zlib.plan', '--', writ_exe, 'plan', 'tests/skeleton/zlib.writ'],
)

test(
    'fix-return-span',
    python,
    args: [golden, source_root / 'tests' / 'check' / 'return_span.fixed', '--fails', '--', writ_exe, '--fix', 'check',
           'tests/check/return_span.writ'],
)
test(
    'fix-binding-raw',
    python,
    args: [golden, source_root / 'tests' / 'bind' / 'e0508.fixed', '--fails', '--', writ_exe, '--fix', 'bind',
           'tests/bind/unknown.writ', 'tests/bind/e0508.writ', '--profile', 'profiles/n00b.writ'],
)

foreach name : ['unknown_type', 'not_pure', 'bad_phase', 'out_unbounded', 'syntax', 'duplicate', 'reinterpret',
                'view_domain', 'return_span', 'record_none']
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


zlib_only = []
foreach name : ['zlibVersion', 'crc32', 'adler32', 'compressBound', 'compress2', 'uncompress', 'deflate', 'deflateEnd',
                'deflateInit', 'deflateInit_', 'inflate', 'inflateEnd', 'inflateInit', 'inflateInit_']
    zlib_only += ['--only', name]
endforeach

test(
    'zlib-import',
    python,
    args: [golden, source_root / 'contracts' / 'zlib' / host_triple + '.writ', '--', writ_exe, 'import',
           '--library', 'zlib', '--header', 'zlib.h', '--link', 'z', '-o', '-'] + zlib_only,
)

test(
    'writ-contract',
    writ_exe,
    args: ['verify', 'contracts/writ.writ', '--binary', libwrit_shared.full_path()],
    depends: libwrit_shared,
    workdir: source_root,
)

test(
    'writ-contract-draft',
    python,
    args: [golden, source_root / 'contracts' / 'writ.draft.writ', '--', writ_exe, 'import', '--library', 'writ',
           '--header', 'writ/writ.h', '--header', 'writ/model.h', '--header', 'writ/gen.h', '--header',
           'writ/toolchain.h', '--link', 'writ', '-I', 'include', '-o', '-'],
)

foreach contract : ['zlib', 'libc']
    test(contract + '-verify', writ_exe, args: ['verify', source_root / 'tests' / contract / contract + '.writ'])
endforeach

generated_c = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=0']

foreach binding : [
    ['libc', 'libc/libc.writ', 'libc', []],
    ['zlib', 'zlib/zlib.writ', 'zlib', [zlib]],
]
    generated = custom_target(
        binding[0] + '-binding',
        input: source_root / 'tests' / binding[1],
        output: [
            binding[2] + '_writ.h',
            binding[2] + '_writ.c',
            binding[2] + '_writ_mock.c',
            binding[2] + '_writ_conformance.c',
        ],
        command: [writ_exe, 'gen', 'c', '@INPUT@', '-o', '@OUTDIR@', '--conformance'],
    )
    test(
        binding[0] + '-conformance',
        executable(
            binding[0] + '-conformance',
            generated[1],
            generated[2],
            generated[3],
            c_args: generated_c + sanitize,
            link_args: sanitize,
            override_options: ['c_std=c11'],
        ),
    )
    test(
        binding[0] + '-roundtrip',
        executable(
            binding[0] + '-roundtrip',
            binding[0] / 'roundtrip.c',
            generated[0],
            generated[1],
            include_directories: include_directories('.'),
            c_args: generated_c + sanitize,
            link_args: sanitize,
            dependencies: binding[3],
            override_options: ['c_std=c11'],
        ),
    )
endforeach

n00b_profile = ['--profile', 'profiles/n00b.writ']
foreach case : [
    ['zlib.n00b', 'tests/zlib/zlib.writ', 'zlib.n00b', n00b_profile + ['--language', 'n00b'], false],
    ['resharp.n00b', 'contracts/resharp.writ', 'resharp.n00b', n00b_profile, false],
    ['kv.n00b', 'tests/bind/kv.writ', 'kv.n00b', n00b_profile, false],
    ['kv.export', 'tests/bind/kv.writ', 'kv.export', n00b_profile, false],
    ['e0501', 'tests/zlib/zlib.writ', 'e0501', n00b_profile, true],
    ['e0502', 'tests/zlib/zlib.writ', 'e0502', n00b_profile, true],
    ['e0503', 'tests/zlib/zlib.writ', 'e0503', n00b_profile, true],
    ['e0504', 'tests/bind/kv.writ', 'e0504', n00b_profile, true],
    ['e0505', 'tests/zlib/zlib.writ', 'e0505', n00b_profile, true],
    ['e0506', 'tests/zlib/zlib.writ', 'e0506', n00b_profile, true],
    ['e0507', 'tests/zlib/zlib.writ', 'e0507', n00b_profile, true],
    ['e0508', 'tests/bind/unknown.writ', 'e0508', n00b_profile, true],
    ['e0509', 'tests/bind/kv.writ', 'e0509', n00b_profile, true],
    ['e0510', 'tests/zlib/zlib.writ', 'e0510', n00b_profile + ['--language', 'n00b'], true],
    ['e0511', 'tests/zlib/zlib.writ', 'e0511', n00b_profile, true],
    ['e0512', 'tests/zlib/zlib.writ', 'zlib.n00b', ['--profile', 'tests/bind/e0512.profile.writ'], true],
    ['e0513', 'tests/zlib/zlib.writ', 'e0513', n00b_profile, true],
]
    test(
        'bind-' + case[0],
        python,
        args: [golden, source_root / 'tests' / 'bind' / case[0] + '.out'] + (case[4] ? ['--fails'] : []) + ['--',
               writ_exe, '--platform', 'aarch64-apple-darwin', '--feature', 'experimental_capture_groups', 'bind',
               case[1], 'tests/bind' / case[2] + '.writ'] + case[3],
    )
endforeach

embedded_flags = ['--platform', 'aarch64-apple-darwin', '--embedded', 'n00b']
contract_dir = ['--contract-dir', 'contracts']
foreach case : [
    ['zlib_path', 'bind', [], false],
    ['zlib_search', 'bind', contract_dir, false],
    ['defaults', 'bind', [], false],
    ['export', 'bind', [], false],
    ['refine', 'lower', contract_dir, false],
    ['e0510', 'bind', contract_dir, true],
    ['e0514_module', 'bind', contract_dir, true],
    ['e0514_none', 'bind', [], true],
    ['e0514_two', 'bind', contract_dir, true],
    ['e0003_path', 'bind', [], true],
    ['e0003_search', 'bind', contract_dir, true],
    ['use_broken', 'bind', [], true],
]
    test(
        'embedded-' + case[0],
        python,
        args: [golden, source_root / 'tests' / 'embedded' / case[0] + '.out'] + (case[3] ? ['--fails'] : [])
              + ['--', writ_exe] + embedded_flags + case[2] + [case[1], 'tests/embedded' / case[0] + '.writ']
              + (case[1] == 'bind' ? n00b_profile : []),
    )
endforeach
test(
    'embedded-zlib_env',
    python,
    args: [golden, source_root / 'tests' / 'embedded' / 'zlib_env.out', '--', writ_exe] + embedded_flags
          + ['bind', 'tests/embedded/zlib_env.writ'] + n00b_profile,
    env: {'WRIT_CONTRACT_PATH': source_root / 'tests' / 'missing:' + source_root / 'contracts'},
)

foreach case : ['zlib', 'resharp', 'kv', 'kv_export', 'float_record']
    test(
        'gen-n00b-' + case,
        python,
        args: [golden, source_root / 'tests' / 'gen_n00b' / case + '.out', '--stdout', '--', writ_exe]
              + ['--platform', 'aarch64-apple-darwin', '--feature', 'experimental_capture_groups'] + contract_dir
              + ['gen', 'n00b', '--embedded', 'tests/gen_n00b' / case + '.writ'] + n00b_profile + ['-o', '-'],
    )
endforeach
test(
    'gen-n00b-compile',
    python,
    args: [source_root / 'tests' / 'gen_n00b' / 'compile.py', writ_exe, compiler, host_triple, source_root,
           'zlib', 'resharp', 'kv_export', 'float_record'],
)

resharp_root = get_option('resharp_root')
resharp_features = ['--feature', 'experimental_capture_groups']
test(
    'resharp-import',
    python,
    args: [golden, source_root / 'contracts' / 'resharp.writ', '--stdout', '--', writ_exe] + resharp_features
          + ['import', '--library', 'resharp', '--crate', 'resharp', '--rust', resharp_root / 'Cargo.toml', '-o', '-'],
    env: {'CARGO_TARGET_DIR': meson.current_build_dir() / 'resharp-target'},
    is_parallel: false,
    timeout: 600,
)
test(
    'resharp-derive',
    python,
    args: [golden, source_root / 'tests' / 'resharp' / 'resharp.derived.writ', '--', writ_exe, 'derive',
           'contracts/resharp.writ'],
)
test(
    'resharp-plans',
    python,
    args: [golden, source_root / 'tests' / 'plan' / 'resharp.full.plan', '--', writ_exe, '--platform',
           'aarch64-apple-darwin'] + resharp_features + ['plan', 'contracts/resharp.writ'],
)
foreach variant : ['plain', 'features', 'miri']
    test(
        'resharp-transport-' + variant,
        python,
        args: [source_root / 'tests' / 'resharp' / 'transport.py', writ_exe.full_path(), source_root, resharp_root,
               meson.current_build_dir() / 'resharp-' + variant, compiler, variant],
        depends: writ_exe,
        is_parallel: false,
        timeout: 1800,
    )
endforeach

spans = custom_target(
    'spans-binding',
    input: source_root / 'tests' / 'spans' / 'spans.writ',
    output: ['spans_writ.h', 'spans_writ.c', 'spans_writ_mock.c', 'spans_writ_conformance.c'],
    command: [writ_exe, 'gen', 'c', '@INPUT@', '-o', '@OUTDIR@', '--conformance'],
)
test(
    'spans-conformance',
    executable(
        'spans-conformance',
        spans[1],
        spans[2],
        spans[3],
        c_args: generated_c + sanitize,
        link_args: sanitize,
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
