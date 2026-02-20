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
  '-fno-omit-frame-pointer',
  '-ffunction-sections',
  '-fPIC',
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

deps = [
  libmath_dep,
  libthreads_dep,
]
if libatomic_dep.found()
  deps += [libatomic_dep]
endif

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
have_forkpty = false
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
    'src/core/stw.c',
    'src/core/gc.c',
    'src/core/interval_tree.c',
    'src/core/buffer.c',
    'src/core/string.c',
]

n00b_unicode_generated = [
    'src/unicode/generated/gen_age.c',
    'src/unicode/generated/gen_bidi_brackets.c',
    'src/unicode/generated/gen_bidi_class.c',
    'src/unicode/generated/gen_blocks.c',
    'src/unicode/generated/gen_casemap.c',
    'src/unicode/generated/gen_categories.c',
    'src/unicode/generated/gen_collation.c',
    'src/unicode/generated/gen_combining.c',
    'src/unicode/generated/gen_composition.c',
    'src/unicode/generated/gen_confusables.c',
    'src/unicode/generated/gen_decomposition.c',
    'src/unicode/generated/gen_eaw.c',
    'src/unicode/generated/gen_emoji.c',
    'src/unicode/generated/gen_grapheme.c',
    'src/unicode/generated/gen_identifiers.c',
    'src/unicode/generated/gen_idna.c',
    'src/unicode/generated/gen_joining.c',
    'src/unicode/generated/gen_linebreak.c',
    'src/unicode/generated/gen_normprops.c',
    'src/unicode/generated/gen_numeric.c',
    'src/unicode/generated/gen_proplist.c',
    'src/unicode/generated/gen_script_extensions.c',
    'src/unicode/generated/gen_scripts.c',
    'src/unicode/generated/gen_sentbreak.c',
    'src/unicode/generated/gen_wordbreak.c',
]

n00b_unicode = [
    'src/unicode/encoding.c',
    'src/unicode/properties.c',
    'src/unicode/casemap.c',
    'src/unicode/normalization.c',
    'src/unicode/segmentation.c',
    'src/unicode/string_ops.c',
    'src/unicode/linebreak.c',
    'src/unicode/bidi.c',
    'src/unicode/collation.c',
    'src/unicode/identifiers.c',
    'src/unicode/security.c',
    'src/unicode/idna.c',
    'src/unicode/emoji.c',
    'src/unicode/query.c',
]

n00b_dll_src = n00b_core + n00b_unicode_generated + n00b_unicode

n00b = static_library('n00b',
    n00b_dll_src,
    include_directories: incdir,
    dependencies: deps,
    c_args: project_compile_args,
    link_args: project_link_args,
    install: true,
)

test_common_kwargs = {
    'include_directories': incdir,
    'c_args': project_compile_args,
    'link_args': project_link_args,
    'link_with': [n00b],
}

testie = executable('testie',
    ['src/test/main.c'],
    kwargs: test_common_kwargs,
)

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

buffer_test = executable('test_buffer',
    ['test/unit/test_buffer.c'],
    kwargs: test_common_kwargs,
)
test('buffer', buffer_test, suite: 'unit')

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
    'properties', 'security', 'segmentation', 'string_ops',
]

foreach name : unicode_test_names
    exe = executable('test_unicode_' + name,
        'test/unit/test_unicode_' + name + '.c',
        kwargs: unicode_test_kwargs,
    )
    test('unicode_' + name, exe, suite: 'unit')
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
