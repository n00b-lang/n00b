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
  '-fno-omit-frame-pointer',
  '-ffunction-sections',
  '-fPIC',
  # TODO only needed for musl
  # '-target', target_arch + '-' + target_os,
]

link_args = ['-target', target_arch + '-' + target_os]

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

if not cc.compiles(posix_check)
  c_args = c_args + ['-DPOSIX_C_SOURCE=200112L']
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
    'src/core/lock_accounting.c',
    'src/core/mutex.c',
    'src/core/rwlock.c',
    'src/core/condition.c',
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

libdl_dep = cc.find_library('dl', required: false)

n00b_dll_src = n00b_core + n00b_unicode + n00b_strings + n00b_render + n00b_table

# Build generated unicode tables with --no-ncc (they are pure data,
# no ncc extensions, and parsing them is extremely expensive).
gen_unicode_lib = static_library('n00b_gen_unicode',
    gen_unicode_tables,
    include_directories: incdir,
    c_args: exe_c_args + ['--no-ncc'],
    link_args: link_args,
)

n00b = static_library('n00b',
    n00b_dll_src,
    include_directories: incdir,
    dependencies: deps + [libdl_dep],
    c_args: exe_c_args,
    link_args: link_args,
    link_whole: [gen_unicode_lib],
    install: true,
)

test_common_kwargs = {
    'include_directories': incdir,
    'c_args': exe_c_args,
    'link_with': [n00b],
}


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