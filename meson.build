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

default_c_std_flag = '-std=c23'
if not cc.has_argument(default_c_std_flag)
  if cc.has_argument('-std=c2x')
    default_c_std_flag = '-std=c2x'
  else
    error('Compiler must support -std=c23 or -std=c2x.')
  endif
endif

incdir = [include_directories('.'), include_directories('include'), include_directories('include/internal')]
if vcpkg_inc != ''
  incdir += [include_directories(vcpkg_inc)]
endif
using_macos = target_os == 'darwin'
using_windows = target_os == 'windows'

conf_release_build = get_option('buildtype') == 'release'
conf_asan = get_option('use_asan').enabled()
conf_ubsan = get_option('use_ubsan').enabled()
conf_memcheck = get_option('use_memcheck') != 'off'
conf_gc_stats = get_option('show_gc_stats').enabled()
conf_lto = get_option('enable_lto')
conf_dev_mode = get_option('dev_mode')
conf_debug = get_option('enable_debug')
toolchain_target = get_option('toolchain_target').strip()

project_c_args = [
  '-Wextra',
  '-g',
  default_c_std_flag,
  '-Wno-unused-parameter',
  '-Wno-unused-function',
  '-Wno-cast-function-type',
  '-Wno-parentheses-equality',
  '-Wno-initializer-overrides',
  '-fno-omit-frame-pointer',
  '-ffunction-sections',
]

project_link_args = []
compile_defines = []

if target_os == 'darwin'
  project_c_args += ['-Wno-atomic-alignment']
endif

if conf_dev_mode
  compile_defines += ['N00B_DEV']
endif

if conf_debug
  compile_defines += ['N00B_DEBUG']
endif

if conf_asan
  project_c_args += [
    '-fsanitize=address',
    '-fsanitize-recover=all',
  ]
  compile_defines += ['N00B_ASAN']
  project_link_args += [
    '-fsanitize=address',
    '-fsanitize-recover=all',
  ]
  if using_macos
    project_c_args += ['-fsanitize-blacklist=../.sanitize_ignore']
  endif
endif

if conf_ubsan
  project_c_args += [
    '-fsanitize=undefined',
    '-fno-sanitize=function',
    '-fsanitize-recover=all',
  ]
  compile_defines += ['N00B_UBSAN']
  project_link_args += [
    '-fsanitize=undefined',
    '-fsanitize-recover=all',
  ]
endif

if (conf_memcheck)
  compile_defines += ['N00B_AUTO_MEMCHECK=3']
endif

if conf_gc_stats
  compile_defines += ['N00B_GC_STATS']
endif

if target_os == 'linux'
  # Linux sources rely on GNU and modern POSIX APIs (e.g. syscall, dl_iterate_phdr).
  compile_defines += ['_GNU_SOURCE', '_POSIX_C_SOURCE=200809L']
endif

# Detect __attribute__((aligned(N))) for word-at-a-time UTF-8 fast paths.
aligned_attr_check = '''
int test(void) {
  __attribute__((aligned(8))) char buf[16];
  return buf[0];
}
'''
if cc.compiles(aligned_attr_check, args: project_c_args, name: '__attribute__((aligned))')
  compile_defines += ['N00B_HAVE_ALIGNED_ATTR']
endif

vector_builtins_check = '''
typedef unsigned char v16u8 __attribute__((vector_size(16)));
int main(void) {
  v16u8 a = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
  return a[0];
}
'''
if cc.compiles(vector_builtins_check, args: project_c_args, name: 'vector_size(16)')
  compile_defines += ['N00B_HAVE_VECTOR_BUILTINS']
endif

if using_windows
  compile_defines += [
    '_WIN32_WINNT=0x0602',
    'WIN32_LEAN_AND_MEAN',
    'NOMINMAX',
  ]
  # Emit Windows-native CodeView debug records when clang is the backend.
  # This gives WinDbg/CDB better symbol and line correlation than DWARF-only data.
  if cc.get_id() == 'clang' and cc.has_argument('-gcodeview')
    project_c_args += ['-gcodeview']
  endif
  # Avoid loader-relocated TLS local-exec addressing in PE binaries. Emulated TLS
  # keeps thread-local access stable under ASLR for the MinGW cross toolchain.
  if cc.has_argument('-femulated-tls')
    project_c_args += ['-femulated-tls']
  endif
  project_link_args += ['-static', '-mconsole']
else
  project_c_args += ['-fPIC']
endif
if conf_lto
  project_c_args += ['-flto']
  project_link_args += ['-flto']
  compile_defines += ['N00B_LTO_ENABLED']
endif

if conf_release_build
  project_c_args += ['-O3', '-ffast-math']
  project_link_args += ['-flto']
endif

if toolchain_target != ''
  target_arg = '--target=' + toolchain_target
  if cc.get_id() == 'clang'
    if not cc.compiles('int main(void) { return 0; }', args: [target_arg], name: 'toolchain target support check')
      error('Compiler does not accept ' + target_arg + '; clear -Dtoolchain_target or use a matching toolchain.')
    endif
    project_c_args += [target_arg]
    project_link_args += [target_arg]
  else
    warning('toolchain_target is currently only supported for clang; ignoring option for ' + cc.get_id())
  endif
endif

compile_define_args = []
foreach define : compile_defines
  compile_define_args += ['-D' + define]
endforeach

probe_args = project_c_args + compile_define_args
project_compile_args = probe_args
project_link_args += ['-w']

libatomic_dep = cc.find_library('atomic', required: false)
libmath_dep = cc.find_library('m', required: false)
libthreads_dep = dependency('threads', required: true)
libutil_dep = cc.find_library('util', required: false)
libbcrypt_dep = cc.find_library('bcrypt', required: false)
libpsapi_dep = cc.find_library('psapi', required: false)
libws2_32_dep = cc.find_library('ws2_32', required: false)

deps = [libthreads_dep]
if libmath_dep.found()
  deps += [libmath_dep]
endif
if libatomic_dep.found()
  deps += [libatomic_dep]
endif
if using_windows
  # Prefer explicit MinGW import libs for clang cross-linking.
  project_link_args += ['-lbcrypt', '-lpsapi', '-lsynchronization', '-lws2_32']
elif libws2_32_dep.found()
  deps += [libws2_32_dep]
endif

have_pthread_getattr_np = false
have_pthread_attr_get_np = false
have_pthread_get_stackaddr_np = false
have_pthread_get_stacksize_np = false
have_pty_h = false
have_forkpty = false
have_dl_iterate_phdr = false
have_arc4random_buf = false
have_getrandom = false
have_map_anonymous = false
have_map_anon = false

have_wait_on_address = false
have_wake_by_address_all = false
have_wake_by_address_single = false
have_virtual_query = false
have_get_current_thread_stack_limits = false
have_bcrypt_gen_random = false
have_enum_process_modules = false

if using_windows
  # NCC cannot parse MinGW system headers inside Meson probe snippets.
  # We treat these as target-contract capabilities and validate via
  # actual source compilation/linking in the cross build.
  have_wait_on_address = true
  have_wake_by_address_all = true
  have_wake_by_address_single = true
  have_virtual_query = true
  have_get_current_thread_stack_limits = true
  have_bcrypt_gen_random = true
  have_enum_process_modules = true
else
  have_pthread_getattr_np = cc.has_function(
    'pthread_getattr_np',
    prefix: '#include <pthread.h>',
    args: probe_args,
    dependencies: [libthreads_dep],
  )
  have_pthread_attr_get_np = cc.has_function(
    'pthread_attr_get_np',
    prefix: '#include <pthread.h>',
    args: probe_args,
    dependencies: [libthreads_dep],
  )
  have_pthread_get_stackaddr_np = cc.has_function(
    'pthread_get_stackaddr_np',
    prefix: '#include <pthread.h>',
    args: probe_args,
    dependencies: [libthreads_dep],
  )
  have_pthread_get_stacksize_np = cc.has_function(
    'pthread_get_stacksize_np',
    prefix: '#include <pthread.h>',
    args: probe_args,
    dependencies: [libthreads_dep],
  )
  have_pty_h = cc.has_header('pty.h', args: probe_args)

  forkpty_deps = []
  if libutil_dep.found()
    forkpty_deps += [libutil_dep]
  endif
  if have_pty_h
    have_forkpty = cc.has_function(
      'forkpty',
      prefix: '#include <pty.h>',
      args: probe_args,
      dependencies: forkpty_deps,
    )
  endif

  have_dl_iterate_phdr = cc.has_function(
    'dl_iterate_phdr',
    prefix: '#include <link.h>',
    args: probe_args,
  )
  have_arc4random_buf = cc.has_function(
    'arc4random_buf',
    prefix: '#include <stdlib.h>',
    args: probe_args,
  )
  have_getrandom = cc.has_function(
    'getrandom',
    prefix: '#include <sys/random.h>',
    args: probe_args,
  )

  have_map_anonymous = cc.compiles(
    '''
    #include <sys/mman.h>
    int main(void) { return MAP_ANONYMOUS == 0; }
    ''',
    args: probe_args,
    name: 'MAP_ANONYMOUS availability',
  )

  have_map_anon = cc.compiles(
    '''
    #include <sys/mman.h>
    int main(void) { return MAP_ANON == 0; }
    ''',
    args: probe_args,
    name: 'MAP_ANON availability',
  )

  if not have_map_anonymous and not have_map_anon
    error('Platform must provide MAP_ANONYMOUS or MAP_ANON.')
  endif

  if not have_getrandom and not have_arc4random_buf
    error('Platform must provide getrandom() or arc4random_buf().')
  endif

  if not have_pthread_getattr_np and not have_pthread_attr_get_np and not (have_pthread_get_stackaddr_np and have_pthread_get_stacksize_np)
    error('No supported pthread stack-introspection API found for stack bound discovery.')
  endif
endif

build_conf = configuration_data()
build_conf.set_quoted('N00B_HOST_OS', target_os)
build_conf.set_quoted('N00B_HOST_CPU', target_arch)
build_conf.set10('N00B_HAVE_PTHREAD_GETATTR_NP', have_pthread_getattr_np)
build_conf.set10('N00B_HAVE_PTHREAD_ATTR_GET_NP', have_pthread_attr_get_np)
build_conf.set10('N00B_HAVE_PTHREAD_GET_STACKADDR_NP', have_pthread_get_stackaddr_np)
build_conf.set10('N00B_HAVE_PTHREAD_GET_STACKSIZE_NP', have_pthread_get_stacksize_np)
build_conf.set10('N00B_HAVE_PTY_H', have_pty_h)
build_conf.set10('N00B_HAVE_FORKPTY', have_forkpty)
build_conf.set10('N00B_HAVE_DL_ITERATE_PHDR', have_dl_iterate_phdr)
build_conf.set10('N00B_HAVE_ARC4RANDOM_BUF', have_arc4random_buf)
build_conf.set10('N00B_HAVE_GETRANDOM', have_getrandom)
build_conf.set10('N00B_HAVE_MAP_ANONYMOUS', have_map_anonymous)
build_conf.set10('N00B_HAVE_MAP_ANON', have_map_anon)
build_conf.set10('N00B_HAVE_WAIT_ON_ADDRESS', have_wait_on_address)
build_conf.set10('N00B_HAVE_WAKE_BY_ADDRESS_ALL', have_wake_by_address_all)
build_conf.set10('N00B_HAVE_WAKE_BY_ADDRESS_SINGLE', have_wake_by_address_single)
build_conf.set10('N00B_HAVE_VIRTUAL_QUERY', have_virtual_query)
build_conf.set10('N00B_HAVE_GET_CURRENT_THREAD_STACK_LIMITS', have_get_current_thread_stack_limits)
build_conf.set10('N00B_HAVE_BCRYPT_GEN_RANDOM', have_bcrypt_gen_random)
build_conf.set10('N00B_HAVE_ENUM_PROCESS_MODULES', have_enum_process_modules)

configure_file(
  input: 'include/internal/n00b_build_config.h.in',
  output: 'n00b_build_config.h',
  configuration: build_conf,
)


n00b_core = [
    'src/core/init.c',
    'src/core/thread.c',
    'src/core/mmaps.c',
    'src/core/alloc.c',
    'src/core/arena.c',
    'src/core/pool.c',
    'src/core/memory_info.c',
    'src/core/hash.c',
    'src/core/dict_untyped.c',
    'src/core/dict.c',
    'src/core/stw.c',
    'src/core/gc.c',
    'src/core/interval_tree.c',
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
        meson.project_source_root() / 'src' / 'unicode' / 'tools' / 'gen_tables.py',
        '--out-dir', '@OUTDIR@',
        '--cache-dir', meson.project_source_root() / 'src' / 'unicode' / '.unicode_cache',
        '--test-data-dir', meson.project_source_root() / 'test' / 'data',
    ],
    depend_files: files('src/unicode/tools/gen_tables.py'),
)

n00b_unicode = [
    'src/unicode/encoding.c',
    'src/unicode/properties.c',
    'src/unicode/casemap.c',
    'src/unicode/normalization.c',
    'src/unicode/segmentation.c',
    'src/unicode/linebreak.c',
    'src/unicode/bidi.c',
    'src/unicode/collation.c',
    'src/unicode/identifiers.c',
    'src/unicode/security.c',
    'src/unicode/idna.c',
    'src/unicode/emoji.c',
    'src/unicode/query.c',
]

n00b_strings = [
    'src/strings/string_ops.c',
    'src/strings/string_convert.c',
    'src/strings/fptostr.c',
    'src/strings/fmt_numbers.c',
    'src/strings/ansi.c',
    'src/strings/markdown.c',
    'src/strings/style_ops.c',
    'src/strings/string_style.c',
    'src/strings/style_registry.c',
    'src/strings/rich_desc.c',
    'src/strings/format_spec.c',
    'src/strings/format.c',
    'src/strings/md_render.c',
    'src/strings/md_lines.c',
    'src/strings/theme.c',
]

n00b_render = [
    'src/core/layout.c',
    'src/render/cell.c',
    'src/render/border_themes.c',
    'src/render/plane.c',
    'src/render/box.c',
    'src/render/composite.c',
    'src/render/canvas.c',
    'src/render/backend_stream.c',
    'src/render/backend_ansi.c',
    'src/render/backend_ansi_inline.c',
    'src/render/backend_dumb.c',
    'src/render/backend_registry.c',
]

n00b_table = [
    'src/table/table.c',
    'src/table/table_layout.c',
    'src/table/table_render.c',
    'src/table/table_styles.c',
]

# ============================================================================
# Conduit pub/sub system
# ============================================================================

n00b_conduit = [
    'src/conduit/conduit.c',
    'src/conduit/subscription.c',
    'src/conduit/thread_lifecycle.c',
    'src/conduit/service.c',
]

# IO backends (platform-conditional)
n00b_conduit_io = [
    'src/conduit/io.c',
]

if target_os != 'windows'
    n00b_conduit_io += 'src/conduit/io_poll.c'
endif

if target_os == 'darwin' or target_os in ['freebsd', 'openbsd', 'netbsd']
    n00b_conduit_io += 'src/conduit/io_kqueue.c'
endif
if target_os == 'linux'
    n00b_conduit_io += 'src/conduit/io_epoll.c'
    liburing_dep = dependency('liburing', required: false)
    if liburing_dep.found()
        n00b_conduit_io += 'src/conduit/io_uring.c'
        deps += liburing_dep
    endif
endif
if target_os == 'windows'
    n00b_conduit_io += 'src/conduit/io_wsa.c'
    ws2_dep = cc.find_library('ws2_32', required: true)
    sync_dep = cc.find_library('synchronization', required: true)
    deps += [ws2_dep, sync_dep]
endif

n00b_conduit += n00b_conduit_io

# Managed IO (Phase 3)
n00b_conduit_managed = [
    'src/conduit/fd_managed.c',
    'src/conduit/fd_writer.c',
    'src/conduit/socket.c',
    'src/conduit/file.c',
    'src/conduit/timer.c',
    'src/conduit/user_event.c',
]

if target_os != 'windows'
    n00b_conduit_managed += 'src/conduit/signal.c'
    n00b_conduit_managed += 'src/conduit/proc_lifecycle.c'
    n00b_conduit_managed += 'src/conduit/file_change.c'
endif

n00b_conduit += n00b_conduit_managed

# IO layer (subprocess management)
n00b_io = []
if target_os != 'windows'
    n00b_io += 'src/io/subproc.c'
endif

n00b_conduit += n00b_io

# Pipeline framework (Phase 4)
n00b_conduit_pipeline = [
    'src/conduit/xform.c',
    'src/conduit/xform_linebuf.c',
    'src/conduit/xform_ansi_strip.c',
    'src/conduit/xform_render.c',
    'src/conduit/xform_hexdump.c',
    'src/conduit/xform_json.c',
    'src/conduit/xform_http.c',
]

n00b_conduit += n00b_conduit_pipeline

# Print API (Phase 4)
n00b_conduit += ['src/conduit/print.c']

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
    'src/slay/hashset.c',
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
    'src/slay/annot_walk.c',
    'src/slay/cfg.c',
]

# ============================================================================
# Hexdump library
# ============================================================================

n00b_hexdump = [
    'src/hexdump/hexdump.c',
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
]

n00b_dll_src = n00b_core + n00b_unicode + n00b_strings + n00b_render + n00b_table + n00b_conduit + n00b_parsers + n00b_hexdump + n00b_logic + n00b_slay + n00b_typecheck

# Build generated unicode tables with --no-ncc (they are pure data,
# no ncc extensions, and parsing them is extremely expensive).
gen_unicode_lib = static_library('n00b_gen_unicode',
    gen_unicode_tables,
    include_directories: incdir,
    c_args: project_compile_args + ['--no-ncc'],
    link_args: project_link_args,
)

# Build md4c vendor implementation with --no-ncc (it's 10K+ lines of
# pure C; parsing it through ncc's packrat parser OOMs in constrained
# environments like Docker cross-builds).
n00b_vendor_lib = static_library('n00b_vendor',
    ['src/vendor/md4c.c'],
    include_directories: incdir,
    c_args: exe_c_args + ['--no-ncc'],
    link_args: link_args,
)

n00b = static_library('n00b',
    n00b_dll_src,
    include_directories: incdir,
    dependencies: deps + [libdl_dep],
    c_args: project_compile_args,
    link_args: project_link_args,
    link_whole: [gen_unicode_lib],
    install: true,
)

test_common_kwargs = {
    'include_directories': incdir,
    'c_args': project_compile_args,
    'link_args': project_link_args,
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

# ============================================================================
# Type checker tests
# ============================================================================

typecheck_test = executable('test_typecheck',
    ['test/unit/test_typecheck.c'],
    kwargs: test_common_kwargs,
)
test('typecheck', typecheck_test, suite: 'unit')

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
    'c_args': project_compile_args,
    'link_args': project_link_args,
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
    'c_args': project_compile_args,
    'link_args': project_link_args,
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
