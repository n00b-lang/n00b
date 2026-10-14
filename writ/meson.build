project(
    'writ',
    'c',
    version: '0.1.0',
    meson_version: '>=1.1.0',
    default_options: [
        'buildtype=debugoptimized',
        'c_std=c2x',
        'warning_level=2',
        'default_library=static',
    ],
)

lantern_prefix = get_option('lantern_prefix')
lantern_root = get_option('lantern_root')
lantern = dependency('ir', method: 'pkg-config')
lantern_headers = lantern.partial_dependency(compile_args: true, includes: true)
lantern_links = lantern.partial_dependency(link_args: true, links: true)

python = find_program('python3')
embed = lantern_root / 'tools' / 'embed_text.py'

grammar_h = custom_target(
    'writ_grammar.h',
    input: 'grammar/writ.bnf',
    output: 'writ_grammar.h',
    command: [python, embed, '@INPUT@', '@OUTPUT@', 'writ_grammar_text'],
)

keywords_h = custom_target(
    'writ_keywords.h',
    input: 'grammar/writ.bnf',
    output: 'writ_keywords.h',
    command: [python, files('tools/soft_keywords.py'), '@INPUT@', '@OUTPUT@'],
)

model_names_h = custom_target(
    'model_names.h',
    input: 'defs/model_fields.def',
    output: 'model_names.h',
    command: [python, files('tools/gen_model.py'), '@INPUT@', '/dev/null', '--names', '@OUTPUT@'],
)

model_json_h = custom_target(
    'model_json.h',
    input: 'defs/model_fields.def',
    output: 'model_json.h',
    command: [python, files('tools/gen_model.py'), '@INPUT@', '/dev/null', '--json', '@OUTPUT@'],
)

writ_inc = include_directories('include', 'defs', 'src/core')

core_sources = files(
    'src/core/session.c',
    'src/core/intern.c',
    'src/core/loc.c',
    'src/core/diag.c',
    'src/core/output.c',
    'src/core/runner.c',
    'src/core/emit.c',
    'src/core/syntax/ast.c',
    'src/core/syntax/parse.c',
    'src/core/syntax/fmt.c',
    'src/core/model/defaults.c',
    'src/core/model/platform.c',
    'src/core/model/types.c',
    'src/core/model/expr.c',
    'src/core/model/resolve.c',
    'src/core/model/checks.c',
    'src/core/model/views.c',
    'src/core/plan/plan.c',
    'src/core/plan/render.c',
    'src/core/lower/json.c',
    'src/gen/c.c',
    'src/gen/conformance.c',
    'src/toolchain/probe.c',
    'src/toolchain/exports.c',
)

writ_raw = static_library(
    'writ_raw',
    core_sources,
    grammar_h,
    keywords_h,
    model_names_h,
    model_json_h,
    include_directories: writ_inc,
    dependencies: lantern_headers,
)

libwrit = custom_target(
    'libwrit.a',
    input: writ_raw,
    output: 'libwrit.a',
    command: [
        files('tools/isolate.sh'),
        meson.get_compiler('c').cmd_array()[0],
        '@OUTPUT@',
        '@INPUT@',
        lantern_prefix / 'lib' / 'libir.a',
        meson.current_source_dir() / 'exports' / 'writ.exp',
    ],
)

writ_dep = declare_dependency(
    link_with: libwrit,
    include_directories: include_directories('include'),
    dependencies: lantern_links,
)

llvm_prefix = get_option('llvm_prefix')
libclang = declare_dependency(
    compile_args: ['-I' + llvm_prefix / 'include'],
    link_args: ['-L' + llvm_prefix / 'lib', '-lclang', '-Wl,-rpath,' + llvm_prefix / 'lib'],
)

libwrit_clang = static_library(
    'writ_clang',
    files('src/clang/tu.c', 'src/clang/importer.c', 'src/clang/layout.c'),
    model_names_h,
    include_directories: writ_inc,
    dependencies: [lantern_headers, libclang],
)

writ_clang_dep = declare_dependency(link_with: libwrit_clang, dependencies: [writ_dep])

writ_exe = executable(
    'writ',
    'src/cli/main.c',
    dependencies: writ_clang_dep,
    c_args: ['-DWRIT_DEFAULT_LLVM_PREFIX="' + llvm_prefix + '"'],
)

subdir('tests')
