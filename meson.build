project(
  'n00b',
  'c',
  default_options: [
    'default_library=static',
  ],

  version: '0.1.0',
  license: 'MIT',
  meson_version: '>=1.6.0'
  ,
)

if (get_option('using_build_script') != true)
  error('Do not try to run meson directly. Run ./build.sh')
endif

target_os = host_machine.system()
target_arch = host_machine.cpu_family()

cc = meson.get_compiler('c')
vcpkg_inc = get_option('vcpkg_inc')
vcpkg_lib = get_option('vcpkg_lib')

incdir = [include_directories('include'), include_directories('include/internal'), include_directories(vcpkg_inc)]
using_macos = host_machine.system() == 'darwin'

conf_release_build = get_option('buildtype') == 'release'
conf_asan = get_option('use_asan').enabled()
conf_ubsan = get_option('use_ubsan').enabled()
conf_memcheck = get_option('use_memcheck') != 'off'
conf_gc_stats = get_option('show_gc_stats').enabled()
conf_lto = get_option('enable_lto')
conf_dev_mode = get_option('dev_mode')
conf_debug = get_option('enable_debug')

glibc_check = '''
#include <stdio.h>
#ifndef __GLIBC__
#error NO GLIBC
#endif
'''

if cc.compiles(glibc_check)
  using_glibc = true
else
  using_glibc = false
endif

posix_check = '''
#include <sys/mman.h>

int
test(void)
{
  return getpagesize() + MAP_ANON;
}
'''

c_args = [
  '-Wextra',
  '-g',
  '-std=c23',
  '-Wno-unused-parameter',
  '-Wno-unused-function',
  '-Wno-cast-function-type',
  '-Wno-parentheses-equality',
  '-Wno-initializer-overrides',
  '-Wno-unknown-pragmas',
  '-fno-omit-frame-pointer',
  '-ffunction-sections',
  # TODO only needed for musl
  # '-target', target_arch + '-' + target_os,
]

if target_os != 'windows'
  c_args += '-fPIC'
endif

if using_glibc
  c_args = c_args + ['-D_GNU_SOURCE']
endif

if target_os == 'darwin'
    link_args = ['-target', target_arch + '-' + target_os]
else
    link_args = []
endif

if target_os == 'darwin'
  c_args = c_args + ['-Wno-atomic-alignment']
endif

if conf_dev_mode
  c_args = c_args + ['-DN00B_DEV']
endif

if conf_debug
  c_args = c_args + ['-DN00B_DEBUG']
endif

if conf_asan
  c_args = c_args + [
    '-fsanitize=address',
    '-fsanitize-recover=all',
    '-DN00B_ASAN',
  ]
  link_args = link_args + [
    '-fsanitize=address',
    '-fsanitize-recover=all',
  ]
  if using_macos
    c_args = c_args + ['-fsanitize-blacklist=../.sanitize_ignore']
  endif
endif

if conf_ubsan
  c_args = c_args + [
    '-fsanitize=undefined',
    '-fno-sanitize=function',
    '-fsanitize-recover=all',
    '-DN00B_UBSAN',
  ]
  link_args = link_args + [
    '-fsanitize=undefined',
    '-fsanitize-recover=all',
  ]
endif

if (conf_memcheck)
  c_args = c_args + [
    '-DN00B_AUTO_MEMCHECK=3',
  ]
endif

if conf_gc_stats
  c_args = c_args + [
    '-DN00B_GC_STATS',
  ]
endif

if target_os != 'windows'
  if not cc.compiles(posix_check)
    c_args = c_args + ['-DPOSIX_C_SOURCE=200112L']
  endif
endif

# Detect __attribute__((aligned(N))) for word-at-a-time UTF-8 fast paths.
aligned_attr_check = '''
int test(void) {
  __attribute__((aligned(8))) char buf[16];
  return buf[0];
}
'''
if cc.compiles(aligned_attr_check, name: '__attribute__((aligned))')
  c_args = c_args + ['-DN00B_HAVE_ALIGNED_ATTR']
endif

vector_builtins_check = '''
typedef unsigned char v16u8 __attribute__((vector_size(16)));
int main(void) {
  v16u8 a = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
  return a[0];
}
'''
if cc.compiles(vector_builtins_check, name: 'vector_size(16)')
  c_args = c_args + ['-DN00B_HAVE_VECTOR_BUILTINS']
endif

exe_link_args = link_args
exe_c_args = c_args + [
  '-DN00B_HOST_OS=' + host_machine.system(),
  '-DN00B_HOST_CPU=' + host_machine.cpu_family(),
]

if conf_lto
  exe_c_args = exe_c_args + ['-flto', '-DN00B_LTO_ENABLED']
  exe_link_args = exe_link_args + ['-flto']
endif

if conf_release_build
  exe_c_args = exe_c_args + ['-O3', '-ffast-math', '-flto']
  exe_link_args = exe_link_args + ['-flto']
endif

pt_attr_np_code = '''
#include <pthread.h>

int main(void) {
  pthread_attr_t attrs;
  pthread_getattr_np(pthread_self(), &attrs);
  return 0;
}
'''

if cc.compiles(pt_attr_np_code)
  c_args = c_args + '-DN00B_HAVE_PT_GETATTR_NP'
endif

fpty_code = '''
#include <stddef.h>
#include <pty.h>

int main(void) { forkpty(nullptr, nullptr, nullptr, nullptr); return 0; }
'''

if cc.links(fpty_code, name: 'forkpty_check')
  exe_c_args = exe_c_args + ['-DHAVE_PTY_H']
endif

exe_link_args = exe_link_args + ['-w']

libatomic_dep = cc.find_library('atomic', required: false)
libmath_dep = cc.find_library('m', required: false)
libthreads_dep = dependency('threads', required: true)

deps = [
  libmath_dep,
  libthreads_dep,
]

libatomic_dep = cc.find_library('atomic', required: false)
libmath_dep = cc.find_library('m', required: false)
libthreads_dep = dependency('threads', required: true)
if libatomic_dep.found()
  deps = deps + [libatomic_dep]
endif

mir_proj = subproject('mir')
mir_dep  = mir_proj.get_variable('mir_dep')

n00b_core = [
    'src/core/init.c',
    'src/core/thread.c',
    'src/core/mmaps.c',
    'src/core/alloc.c',
    'src/core/arena.c',
    'src/core/pool.c',
    'src/core/memory_info.c',
    'src/core/hash.c',
    'src/core/stw.c',
    'src/core/gc.c',
    'src/core/buffer.c',
    'src/core/data_lock.c',
    'src/core/string.c',
    'src/core/lock_accounting.c',
    'src/core/mutex.c',
    'src/core/rwlock.c',
    'src/core/condition.c',
    'src/core/type_registry.c',
    'src/core/builtin_types.c',
]

# ============================================================================
# Abstract data types
# ============================================================================

n00b_adt = [
    'src/adt/dict_untyped.c',
    'src/adt/dict.c',
    'src/adt/interval_tree.c',
]

python3 = find_program('python3')

gen_unicode_file_names = [
    'gen_age.c',
    'gen_bidi_brackets.c',
    'gen_bidi_class.c',
    'gen_blocks.c',
    'gen_casemap.c',
    'gen_categories.c',
    'gen_collation.c',
    'gen_combining.c',
    'gen_composition.c',
    'gen_confusables.c',
    'gen_decomposition.c',
    'gen_eaw.c',
    'gen_emoji.c',
    'gen_grapheme.c',
    'gen_identifiers.c',
    'gen_idna.c',
    'gen_joining.c',
    'gen_linebreak.c',
    'gen_normprops.c',
    'gen_numeric.c',
    'gen_proplist.c',
    'gen_script_extensions.c',
    'gen_scripts.c',
    'gen_sentbreak.c',
    'gen_wordbreak.c',
]

gen_unicode_tables = custom_target('gen_unicode_tables',
    output: gen_unicode_file_names,
    command: [
        python3,
        meson.project_source_root() / 'src' / 'text' / 'unicode' / 'tools' / 'gen_tables.py',
        '--out-dir', '@OUTDIR@',
        '--cache-dir', meson.project_source_root() / 'src' / 'text' / 'unicode' / '.unicode_cache',
        '--test-data-dir', meson.project_source_root() / 'test' / 'data',
    ],
    depend_files: files('src/text/unicode/tools/gen_tables.py'),
)

# ============================================================================
# Text processing (unicode + strings)
# ============================================================================

n00b_text = [
    # Unicode algorithms
    'src/text/unicode/encoding.c',
    'src/text/unicode/properties.c',
    'src/text/unicode/casemap.c',
    'src/text/unicode/normalization.c',
    'src/text/unicode/segmentation.c',
    'src/text/unicode/linebreak.c',
    'src/text/unicode/bidi.c',
    'src/text/unicode/collation.c',
    'src/text/unicode/identifiers.c',
    'src/text/unicode/security.c',
    'src/text/unicode/idna.c',
    'src/text/unicode/emoji.c',
    'src/text/unicode/query.c',
    # String formatting, styling, rendering
    'src/text/strings/string_ops.c',
    'src/text/strings/string_convert.c',
    'src/text/strings/fptostr.c',
    'src/text/strings/fmt_numbers.c',
    'src/text/strings/ansi.c',
    'src/text/strings/markdown.c',
    'src/text/strings/style_ops.c',
    'src/text/strings/string_style.c',
    'src/text/strings/style_registry.c',
    'src/text/strings/rich_desc.c',
    'src/text/strings/format_spec.c',
    'src/text/strings/format.c',
    'src/text/strings/md_render.c',
    'src/text/strings/md_lines.c',
    'src/text/strings/theme.c',
]

# ============================================================================
# Display (render + table + layout + hexdump)
# ============================================================================

n00b_display = [
    'src/display/layout.c',
    'src/display/hexdump.c',
    # Render engine
    'src/display/render/cell.c',
    'src/display/render/border_themes.c',
    'src/display/render/plane.c',
    'src/display/render/box.c',
    'src/display/render/composite.c',
    'src/display/render/canvas.c',
    'src/display/render/backend_stream.c',
    'src/display/render/backend_ansi.c',
    'src/display/render/backend_ansi_inline.c',
    'src/display/render/backend_dumb.c',
    'src/display/render/backend_registry.c',
    # Table
    'src/display/table/table.c',
    'src/display/table/table_layout.c',
    'src/display/table/table_render.c',
    'src/display/table/table_styles.c',
]

# ============================================================================
# Conduit pub/sub system
# ============================================================================

n00b_conduit = [
    'src/conduit/conduit.c',
    'src/conduit/subscription.c',
    'src/conduit/thread_lifecycle.c',
    'src/conduit/service.c',
    # IO core
    'src/conduit/io.c',
    # Managed FDs
    'src/conduit/fd_managed.c',
    'src/conduit/fd_writer.c',
    'src/conduit/socket.c',
    'src/conduit/file.c',
    'src/conduit/timer.c',
    'src/conduit/user_event.c',
    # Pipeline / transforms
    'src/conduit/xform.c',
    'src/conduit/xform_linebuf.c',
    'src/conduit/xform_ansi_strip.c',
    'src/conduit/xform_render.c',
    'src/conduit/xform_hexdump.c',
    'src/conduit/xform_json.c',
    'src/conduit/xform_http.c',
    # Print API
    'src/conduit/print.c',
]

# Platform-conditional IO backends
if target_os != 'windows'
    n00b_conduit += [
        'src/conduit/io_poll.c',
        'src/conduit/signal.c',
        'src/conduit/proc_lifecycle.c',
        'src/conduit/file_change.c',
        'src/conduit/subproc.c',
    ]
endif

if target_os == 'darwin' or target_os in ['freebsd', 'openbsd', 'netbsd']
    n00b_conduit += 'src/conduit/io_kqueue.c'
endif
if target_os == 'linux'
    n00b_conduit += 'src/conduit/io_epoll.c'
    liburing_dep = dependency('liburing', required: false)
    if liburing_dep.found()
        n00b_conduit += 'src/conduit/io_uring.c'
        deps += liburing_dep
    endif
endif
if target_os == 'windows'
    n00b_conduit += 'src/conduit/io_wsa.c'
    ws2_dep = cc.find_library('ws2_32', required: true)
    sync_dep = cc.find_library('synchronization', required: true)
    deps += [ws2_dep, sync_dep]
endif

# ============================================================================
# Parsers
# ============================================================================

n00b_parsers = [
    'src/parsers/ijson.c',
    'src/parsers/json.c',
    'src/parsers/scanner.c',
    'src/parsers/token_stream.c',
    'src/parsers/scan_recipes.c',
]

# ============================================================================
# Slay grammar / parser toolkit
# ============================================================================

n00b_slay = [
    'src/slay/token.c',
    'src/slay/annotation.c',
    'src/slay/grammar.c',
    'src/slay/unicode_class.c',
    'src/slay/parse_tree.c',
    'src/slay/parse_forest.c',
    'src/slay/pwz.c',
    'src/slay/bnf.c',
    'src/slay/earley.c',
    'src/slay/earley_forest.c',
    'src/slay/n00b_parse.c',
    'src/slay/commander.c',
    'src/slay/commander_tok.c',
    'src/slay/commander_json.c',
    'src/slay/symtab.c',
    'src/slay/tree_util.c',
    'src/slay/annot_walk.c',
    'src/slay/annot_scope.c',
    'src/slay/annot_symtab.c',
    'src/slay/annot_cf.c',
    'src/slay/annot_types.c',
    'src/slay/infer_expr.c',
    'src/slay/cfg.c',
    'src/slay/cdg.c',
    'src/slay/dfg.c',
    'src/slay/diagnostic.c',
    'src/slay/analyze.c',
    'src/slay/debug.c',
    'src/slay/c_tokenizer.c',
    'src/slay/transform.c',
    'src/slay/codegen.c',
    'src/slay/codegen_mir.c',
]

# ============================================================================
# N00b compiler (language-specific)
# ============================================================================

n00b_compiler = [
    'src/n00b/n00b_tokenizer.c',
    'src/n00b/n00b_type_spec.c',
    'src/n00b/n00b_compile.c',
    'src/n00b/n00b_module_loader.c',
]

libdl_dep = cc.find_library('dl', required: false)

# ============================================================================
# Logic / constraint solver
# ============================================================================

n00b_logic = [
    'src/logic/clpfd_domain.c',
    'src/logic/clpfd_store.c',
    'src/logic/clpfd_label.c',
    'src/logic/asp_intern.c',
    'src/logic/asp_relation.c',
    'src/logic/asp_rule.c',
    'src/logic/asp_engine.c',
    'src/logic/logic_program.c',
    'src/logic/logic_dsl.c',
]

# ============================================================================
# Type checker
# ============================================================================

n00b_typecheck = [
    'src/typecheck/construct.c',
    'src/typecheck/context.c',
    'src/typecheck/unify.c',
    'src/typecheck/print.c',
]

n00b_dll_src = n00b_core + n00b_adt + n00b_text + n00b_display + n00b_conduit + n00b_parsers + n00b_logic + n00b_slay + n00b_typecheck + n00b_compiler

# Build generated unicode tables with --no-ncc (they are pure data,
# no ncc extensions, and parsing them is extremely expensive).
gen_unicode_lib = static_library('n00b_gen_unicode',
    gen_unicode_tables,
    include_directories: incdir,
    c_args: exe_c_args + ['--no-ncc'],
    link_args: link_args,
)

# Build md4c vendor implementation with --no-ncc (it's 10K+ lines of
# pure C; parsing it through ncc's packrat parser OOMs in constrained
# environments like Docker cross-builds).
n00b_vendor_lib = static_library('n00b_vendor',
    ['src/vendor/md4c.c', 'src/vendor/linenoise.c'],
    include_directories: incdir,
    c_args: exe_c_args + ['--no-ncc'],
    link_args: link_args,
)

n00b = static_library('n00b',
    n00b_dll_src,
    include_directories: incdir,
    dependencies: deps + [libdl_dep, mir_dep],
    c_args: exe_c_args,
    link_args: link_args,
    link_whole: [gen_unicode_lib, n00b_vendor_lib],
    install: true,
)

test_common_kwargs = {
    'include_directories': incdir,
    'c_args': exe_c_args,
    'link_with': [n00b],
}


array_test = executable('test_array',
    ['test/unit/test_array.c'],
    kwargs: test_common_kwargs,
)
test('array', array_test, suite: 'unit')

list_test = executable('test_list',
    ['test/unit/test_list.c'],
    kwargs: test_common_kwargs,
)
test('list', list_test, suite: 'unit')

stack_test = executable('test_stack',
    ['test/unit/test_stack.c'],
    kwargs: test_common_kwargs,
)
test('stack', stack_test, suite: 'unit')

result_test = executable('test_result',
    ['test/unit/test_result.c'],
    kwargs: test_common_kwargs,
)
test('result', result_test, suite: 'unit')

variant_test = executable('test_variant',
    ['test/unit/test_variant.c'],
    kwargs: test_common_kwargs,
)
test('variant', variant_test, suite: 'unit')

tuple_test = executable('test_tuple',
    ['test/unit/test_tuple.c'],
    kwargs: test_common_kwargs,
)
test('tuple', tuple_test, suite: 'unit')

tree_test = executable('test_tree',
    ['test/unit/test_tree.c'],
    kwargs: test_common_kwargs,
)
test('tree', tree_test, suite: 'unit')

gc_test = executable('test_gc',
    ['test/unit/test_gc.c'],
    kwargs: test_common_kwargs,
)
test('gc', gc_test, suite: 'unit')

finalizer_test = executable('test_finalizer',
    ['test/unit/test_finalizer.c'],
    kwargs: test_common_kwargs,
)
test('finalizer', finalizer_test, suite: 'unit')

buffer_test = executable('test_buffer',
    ['test/unit/test_buffer.c'],
    kwargs: test_common_kwargs,
)
test('buffer', buffer_test, suite: 'unit')

mutex_test = executable('test_mutex',
    ['test/unit/test_mutex.c'],
    kwargs: test_common_kwargs,
)
test('mutex', mutex_test, suite: 'unit')

rwlock_test = executable('test_rwlock',
    ['test/unit/test_rwlock.c'],
    kwargs: test_common_kwargs,
)
test('rwlock', rwlock_test, suite: 'unit')

condition_test = executable('test_condition',
    ['test/unit/test_condition.c'],
    kwargs: test_common_kwargs,
)
test('condition', condition_test, suite: 'unit')

dict_test = executable('test_dict',
    ['test/unit/test_dict.c'],
    kwargs: test_common_kwargs,
)
test('dict', dict_test, suite: 'unit')

# ============================================================================
# Conduit tests
# ============================================================================

type_registry_test = executable('test_type_registry',
    ['test/unit/test_type_registry.c'],
    kwargs: test_common_kwargs,
)
test('type_registry', type_registry_test, suite: 'unit')

conduit_test = executable('test_conduit',
    ['test/unit/test_conduit.c'],
    kwargs: test_common_kwargs,
)
test('conduit', conduit_test, suite: 'unit')

publisher_test = executable('test_publisher',
    ['test/unit/test_publisher.c'],
    kwargs: test_common_kwargs,
)
test('publisher', publisher_test, suite: 'unit')

subscription_test = executable('test_subscription',
    ['test/unit/test_subscription.c'],
    kwargs: test_common_kwargs,
)
test('subscription', subscription_test, suite: 'unit')

io_test = executable('test_io',
    ['test/unit/test_io.c'],
    kwargs: test_common_kwargs,
)
test('io', io_test, suite: 'unit')

fd_managed_test = executable('test_fd_managed',
    ['test/unit/test_fd_managed.c'],
    kwargs: test_common_kwargs,
)
test('fd_managed', fd_managed_test, suite: 'unit')

socket_test = executable('test_socket',
    ['test/unit/test_socket.c'],
    kwargs: test_common_kwargs,
)
test('socket', socket_test, suite: 'unit')

if target_os != 'windows'
    signal_test = executable('test_signal',
        ['test/unit/test_signal.c'],
        kwargs: test_common_kwargs,
    )
    test('signal', signal_test, suite: 'unit')

    proc_lifecycle_test = executable('test_proc_lifecycle',
        ['test/unit/test_proc_lifecycle.c'],
        kwargs: test_common_kwargs,
    )
    test('proc_lifecycle', proc_lifecycle_test, suite: 'unit')

    file_change_test = executable('test_file_change',
        ['test/unit/test_file_change.c'],
        kwargs: test_common_kwargs,
    )
    test('file_change', file_change_test, suite: 'unit')

    subproc_test = executable('test_subproc',
        ['test/unit/test_subproc.c'],
        kwargs: test_common_kwargs,
    )
    test('subproc', subproc_test, suite: 'unit')
endif

timer_test = executable('test_timer',
    ['test/unit/test_timer.c'],
    kwargs: test_common_kwargs,
)
test('timer', timer_test, suite: 'unit')

service_test = executable('test_service',
    ['test/unit/test_service.c'],
    kwargs: test_common_kwargs,
)
test('service', service_test, suite: 'unit')

conduit_e2e_test = executable('test_conduit_e2e',
    ['test/unit/test_conduit_e2e.c'],
    kwargs: test_common_kwargs,
)
test('conduit_e2e', conduit_e2e_test, suite: 'unit')

dual_backend_test = executable('test_dual_backend',
    ['test/unit/test_dual_backend.c'],
    kwargs: test_common_kwargs,
)
test('dual_backend', dual_backend_test, suite: 'unit')

xform_test = executable('test_xform',
    ['test/unit/test_xform.c'],
    kwargs: test_common_kwargs,
)
test('xform', xform_test, suite: 'unit')

xform_linebuf_test = executable('test_xform_linebuf',
    ['test/unit/test_xform_linebuf.c'],
    kwargs: test_common_kwargs,
)
test('xform_linebuf', xform_linebuf_test, suite: 'unit')

xform_ansi_test = executable('test_xform_ansi',
    ['test/unit/test_xform_ansi.c'],
    kwargs: test_common_kwargs,
)
test('xform_ansi', xform_ansi_test, suite: 'unit')

xform_render_test = executable('test_xform_render',
    ['test/unit/test_xform_render.c'],
    kwargs: test_common_kwargs,
)
test('xform_render', xform_render_test, suite: 'unit')

hexdump_test = executable('test_hexdump',
    ['test/unit/test_hexdump.c'],
    kwargs: test_common_kwargs,
)
test('hexdump', hexdump_test, suite: 'unit')

xform_hexdump_test = executable('test_xform_hexdump',
    ['test/unit/test_xform_hexdump.c'],
    kwargs: test_common_kwargs,
)
test('xform_hexdump', xform_hexdump_test, suite: 'unit')

print_test = executable('test_print',
    ['test/unit/test_print.c'],
    kwargs: test_common_kwargs,
)
test('print', print_test, suite: 'unit')

# ============================================================================
# Logic / constraint solver tests
# ============================================================================

logic_test_kwargs = {
    'include_directories': incdir + [include_directories('test/unit')],
    'c_args': exe_c_args,
    'link_with': [n00b],
}

clpfd_domain_test = executable('test_clpfd_domain',
    ['test/unit/test_clpfd_domain.c'],
    kwargs: logic_test_kwargs,
)
test('clpfd_domain', clpfd_domain_test, suite: 'unit')

clpfd_store_test = executable('test_clpfd_store',
    ['test/unit/test_clpfd_store.c'],
    kwargs: logic_test_kwargs,
)
test('clpfd_store', clpfd_store_test, suite: 'unit')

asp_engine_test = executable('test_asp_engine',
    ['test/unit/test_asp_engine.c'],
    kwargs: logic_test_kwargs,
)
test('asp_engine', asp_engine_test, suite: 'unit')

asp_relation_test = executable('test_asp_relation',
    ['test/unit/test_asp_relation.c'],
    kwargs: logic_test_kwargs,
)
test('asp_relation', asp_relation_test, suite: 'unit')

logic_program_test = executable('test_logic_program',
    ['test/unit/test_logic_program.c'],
    kwargs: logic_test_kwargs,
)
test('logic_program', logic_program_test, suite: 'unit')

clpfd_label_test = executable('test_clpfd_label',
    ['test/unit/test_clpfd_label.c'],
    kwargs: logic_test_kwargs,
)
test('clpfd_label', clpfd_label_test, suite: 'unit')

logic_dsl_test = executable('test_logic_dsl',
    ['test/unit/test_logic_dsl.c'],
    kwargs: logic_test_kwargs,
)
test('logic_dsl', logic_dsl_test, suite: 'unit')

logic_easy_test = executable('test_logic_easy',
    ['test/unit/test_logic_easy.c'],
    kwargs: logic_test_kwargs,
)
test('logic_easy', logic_easy_test, suite: 'unit')

# ============================================================================
# Parser tests
# ============================================================================

ijson_test = executable('test_ijson',
    ['test/unit/test_ijson.c'],
    kwargs: test_common_kwargs,
)
test('ijson', ijson_test, suite: 'unit')

json_test = executable('test_json',
    ['test/unit/test_json.c'],
    kwargs: test_common_kwargs,
)
test('json', json_test, suite: 'unit')

xform_json_test = executable('test_xform_json',
    ['test/unit/test_xform_json.c'],
    kwargs: test_common_kwargs,
)
test('xform_json', xform_json_test, suite: 'unit')

xform_http_test = executable('test_xform_http',
    ['test/unit/test_xform_http.c'],
    kwargs: test_common_kwargs,
)
test('xform_http', xform_http_test, suite: 'unit')

# Slay tests commented out temporarily due to in-progress header changes
# scanner_test = executable('test_scanner',
#     ['test/unit/test_scanner.c'],
#     kwargs: test_common_kwargs,
# )
# test('scanner', scanner_test, suite: 'unit')
#
# ============================================================================
# Slay grammar tests
# ============================================================================

grammar_test = executable('test_grammar',
    ['test/unit/test_grammar.c'],
    kwargs: test_common_kwargs,
)
test('grammar', grammar_test, suite: 'unit')

pwz_test = executable('test_pwz',
    ['test/unit/test_pwz.c'],
    kwargs: test_common_kwargs,
)
test('pwz', pwz_test, suite: 'unit')

bnf_test = executable('test_bnf',
    ['test/unit/test_bnf.c'],
    kwargs: test_common_kwargs,
)
test('bnf', bnf_test, suite: 'unit')

bnf_json_test = executable('test_bnf_json',
    ['test/unit/test_bnf_json.c'],
    kwargs: test_common_kwargs,
)
test('bnf_json', bnf_json_test, suite: 'unit')

earley_test = executable('test_earley',
    ['test/unit/test_earley.c'],
    kwargs: test_common_kwargs,
)
test('earley', earley_test, suite: 'unit')

parse_api_test = executable('test_parse_api',
    ['test/unit/test_parse_api.c'],
    kwargs: test_common_kwargs,
)
test('parse_api', parse_api_test, suite: 'unit')

commander_test = executable('test_commander',
    ['test/unit/test_commander.c'],
    kwargs: test_common_kwargs,
)
test('commander', commander_test, suite: 'unit')

symtab_test = executable('test_symtab',
    ['test/unit/test_symtab.c'],
    kwargs: test_common_kwargs,
)
test('symtab', symtab_test, suite: 'unit')

c_parse_test = executable('test_c_parse',
    ['test/unit/test_c_parse.c'],
    kwargs: test_common_kwargs,
)
test('c_parse', c_parse_test, suite: 'unit')

c_tokenizer_test = executable('test_c_tokenizer',
    ['test/unit/test_c_tokenizer.c'],
    kwargs: test_common_kwargs,
)
test('c_tokenizer', c_tokenizer_test, suite: 'unit')

n00b_tokenizer_test = executable('test_n00b_tokenizer',
    ['test/unit/test_n00b_tokenizer.c'],
    kwargs: test_common_kwargs,
)
test('n00b_tokenizer', n00b_tokenizer_test, suite: 'unit')

cf_label_test = executable('test_cf_label',
    ['test/unit/test_cf_label.c'],
    kwargs: test_common_kwargs,
)
test('cf_label', cf_label_test, suite: 'unit')

cfg_test = executable('test_cfg',
    ['test/unit/test_cfg.c'],
    kwargs: test_common_kwargs,
)
test('cfg', cfg_test, suite: 'unit')

cdg_test = executable('test_cdg',
    ['test/unit/test_cdg.c'],
    kwargs: test_common_kwargs,
)
test('cdg', cdg_test, suite: 'unit')

dfg_test = executable('test_dfg',
    ['test/unit/test_dfg.c'],
    kwargs: test_common_kwargs,
)
test('dfg', dfg_test, suite: 'unit')

# ============================================================================
# Type checker tests
# ============================================================================

typecheck_test = executable('test_typecheck',
    ['test/unit/test_typecheck.c'],
    kwargs: test_common_kwargs,
)
test('typecheck', typecheck_test, suite: 'unit')

# ============================================================================
# Analysis tests
# ============================================================================

analyze_test = executable('test_analyze',
    ['test/unit/test_analyze.c'],
    kwargs: test_common_kwargs,
)
test('analyze', analyze_test, suite: 'unit')

n00b_programs_test = executable('test_n00b_programs',
    ['test/unit/test_n00b_programs.c'],
    kwargs: test_common_kwargs,
)
test('n00b_programs', n00b_programs_test, suite: 'unit')

infer_expr_test = executable('test_infer_expr',
    ['test/unit/test_infer_expr.c'],
    kwargs: test_common_kwargs,
)
test('infer_expr', infer_expr_test, suite: 'unit')

# ============================================================================
# MIR JIT tests
# ============================================================================

mir_test = executable('test_mir',
    ['test/unit/test_mir.c'],
    include_directories: incdir,
    c_args: exe_c_args,
    link_with: [n00b],
    dependencies: [mir_dep],
)
test('mir', mir_test, suite: 'unit')

codegen_test = executable('test_codegen',
    ['test/unit/test_codegen.c'],
    include_directories: incdir,
    c_args: exe_c_args,
    link_with: [n00b],
    dependencies: [mir_dep],
)
test('codegen', codegen_test, suite: 'unit')

# ============================================================================
# Render / layout tests
# ============================================================================

layout_test = executable('test_layout',
    ['test/unit/test_layout.c'],
    kwargs: test_common_kwargs,
)
test('layout', layout_test, suite: 'unit')

render_cell_test = executable('test_render_cell',
    ['test/unit/test_render_cell.c'],
    kwargs: test_common_kwargs,
)
test('render_cell', render_cell_test, suite: 'unit')

render_plane_test = executable('test_render_plane',
    ['test/unit/test_render_plane.c'],
    kwargs: test_common_kwargs,
)
test('render_plane', render_plane_test, suite: 'unit')

render_box_test = executable('test_render_box',
    ['test/unit/test_render_box.c'],
    kwargs: test_common_kwargs,
)
test('render_box', render_box_test, suite: 'unit')

render_canvas_test = executable('test_render_canvas',
    ['test/unit/test_render_canvas.c'],
    kwargs: test_common_kwargs,
)
test('render_canvas', render_canvas_test, suite: 'unit')

render_ansi_test = executable('test_render_ansi',
    ['test/unit/test_render_ansi.c'],
    kwargs: test_common_kwargs,
)
test('render_ansi', render_ansi_test, suite: 'unit')

# Table tests
table_build_test = executable('test_table_build',
    ['test/unit/test_table_build.c'],
    kwargs: test_common_kwargs,
)
test('table_build', table_build_test, suite: 'unit')

table_layout_test = executable('test_table_layout',
    ['test/unit/test_table_layout.c'],
    kwargs: test_common_kwargs,
)
test('table_layout', table_layout_test, suite: 'unit')

table_render_test = executable('test_table_render',
    ['test/unit/test_table_render.c'],
    kwargs: test_common_kwargs,
)
test('table_render', table_render_test, suite: 'unit')

table_stream_test = executable('test_table_stream',
    ['test/unit/test_table_stream.c'],
    kwargs: test_common_kwargs,
)
test('table_stream', table_stream_test, suite: 'unit')

# Table CLI tool
table_tool = executable('n00b_table',
    ['src/tools/table.c'],
    kwargs: test_common_kwargs,
    install: true,
)

# N00b dev tool (parse + annotate n00b source files, REPL)
n00b_dev = executable('n00b_dev',
    ['src/tools/n00b_dev.c', 'src/tools/n00b_repl.c'],
    include_directories: incdir,
    c_args: exe_c_args,
    link_with: [n00b],
    dependencies: [mir_dep],
    install: true,
)

# ============================================================================
# Integration tests
# ============================================================================

hash_test = executable('test_hash',
    ['test/integration/test_hash.c'],
    kwargs: test_common_kwargs,
)
test('hash', hash_test, suite: 'integration')

interval_tree_test = executable('test_interval_tree',
    ['test/integration/test_interval_tree.c'],
    kwargs: test_common_kwargs,
)
test('interval_tree', interval_tree_test, suite: 'integration')

dict_untyped_test = executable('test_dict_untyped',
    ['test/integration/test_dict_untyped.c'],
    kwargs: test_common_kwargs,
)
test('dict_untyped', dict_untyped_test, suite: 'integration')

arena_alloc_test = executable('test_arena_alloc',
    ['test/integration/test_arena_alloc.c'],
    kwargs: test_common_kwargs,
)
test('arena_alloc', arena_alloc_test, suite: 'integration')

pool_alloc_test = executable('test_pool_alloc',
    ['test/integration/test_pool_alloc.c'],
    kwargs: test_common_kwargs,
)
test('pool_alloc', pool_alloc_test, suite: 'integration')

mmaps_test = executable('test_mmaps',
    ['test/integration/test_mmaps.c'],
    kwargs: test_common_kwargs,
)
test('mmaps', mmaps_test, suite: 'integration')

alloc_metadata_test = executable('test_alloc_metadata',
    ['test/integration/test_alloc_metadata.c'],
    kwargs: test_common_kwargs,
)
test('alloc_metadata', alloc_metadata_test, suite: 'integration')

# ============================================================================
# Unicode tests
# ============================================================================

unicode_test_kwargs = {
    'include_directories': incdir + [include_directories('test/unit')],
    'c_args': exe_c_args,
    'link_with': [n00b],
}

unicode_test_names = [
    'bidi', 'casemap', 'collation', 'emoji', 'encoding',
    'identifiers', 'idna', 'iter', 'linebreak', 'normalization',
    'properties', 'security', 'segmentation',
]

foreach name : unicode_test_names
    exe = executable('test_unicode_' + name,
        'test/unit/test_unicode_' + name + '.c',
        kwargs: unicode_test_kwargs,
    )
    test('unicode_' + name, exe, suite: 'unit')
endforeach

# ============================================================================
# String tests
# ============================================================================

str_test_kwargs = {
    'include_directories': incdir + [include_directories('test/unit')],
    'c_args': exe_c_args,
    'link_with': [n00b],
}

str_test_names = [
    'string_ops', 'string_convert', 'fptostr', 'fmt_numbers',
    'ansi', 'markdown', 'style_ops', 'string_style',
    'style_registry', 'rich_desc', 'format_spec',
    'format', 'md_render', 'md_lines',
]

foreach name : str_test_names
    exe = executable('test_str_' + name,
        'test/unit/test_str_' + name + '.c',
        kwargs: str_test_kwargs,
    )
    test('str_' + name, exe, suite: 'unit')
endforeach

# ============================================================================
# Documentation (Doxygen)
# ============================================================================

doxygen = find_program('doxygen', required: false)

if doxygen.found()
  doxyfile = configure_file(
    input: 'Doxyfile.in',
    output: 'Doxyfile',
    configuration: {
      'PROJECT_NAME': meson.project_name(),
      'PROJECT_VERSION': meson.project_version(),
      'TOP_SRCDIR': meson.project_source_root(),
      'TOP_BUILDDIR': meson.project_build_root(),
    },
  )

  custom_target('docs',
    input: doxyfile,
    output: 'html',
    command: [doxygen, '@INPUT@'],
    build_by_default: false,
    install: false,
  )
endif