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

conf_cocoa = get_option('enable_cocoa') and target_os == 'darwin'
if conf_cocoa
  add_languages('objc', required: true, native: false)
endif

conf_notcurses = get_option('enable_notcurses')
conf_freetype  = get_option('enable_freetype')

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
  '--ncc-vargs-type=n00b_vargs_t',
  '--ncc-rstr-string-type=n00b_string_t',
  '--ncc-rstr-template-plain=({static n00b_string_t $0={.u8_bytes=$1,.data=$2,.codepoints=$3,.styling=((void*)0)};&$0;})',
  '--ncc-rstr-template-styled=({$0 static n00b_string_t $1={.u8_bytes=$2,.data=$3,.codepoints=$4,.styling=$5};&$1;})',
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
# ncc cannot tokenize the libgcc/clang SIMD intrinsic headers xxhash
# pulls in (compound-literal vector casts in mmintrin.h). Disable
# xxhash's intrinsic paths unconditionally — its scalar fallback is
# still fast.
c_args += '-DXXH_NO_INTRINSICS=1'

if using_glibc
  c_args = c_args + [
    '-D_GNU_SOURCE',
    '-DN00B_NCC_GLIBC_ASSERT_SHIM',
  ]
endif

if target_os == 'darwin'
    link_args = ['-target', target_arch + '-' + target_os]
elif target_os == 'windows'
    link_args = ['-static', '-Wl,--stack,16777216']
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
if target_os != 'windows' and cc.compiles(vector_builtins_check, name: 'vector_size(16)')
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

if conf_cocoa
  cocoa_dep = dependency('appleframeworks',
      modules: ['Cocoa', 'CoreText', 'CoreGraphics', 'QuartzCore'])
  deps += cocoa_dep
endif

if conf_notcurses
  notcurses_dep = dependency('notcurses', version: '>=3.0.10', required: false)
  conf_notcurses = notcurses_dep.found()
endif

if conf_freetype
  freetype_dep = dependency('freetype2', required: false)
  conf_freetype = freetype_dep.found()
endif

if conf_notcurses
  exe_c_args = exe_c_args + ['-DN00B_HAVE_NOTCURSES=1']
  deps += notcurses_dep
endif
if conf_freetype
  exe_c_args = exe_c_args + ['-DN00B_HAVE_FREETYPE=1']
  deps += freetype_dep
endif

mir_proj = subproject('mir')
mir_dep  = mir_proj.get_variable('mir_dep')

subdir('src/core')
subdir('src/util')

# ============================================================================
# Abstract data types
# ============================================================================

subdir('src/adt')

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

subdir('src/text')

# ============================================================================
# Display (render + table + layout + hexdump)
# ============================================================================

subdir('src/display')

# ============================================================================
# Conduit pub/sub system
# ============================================================================

subdir('src/conduit')

# ============================================================================
# Virtual Filesystem
# ============================================================================

subdir('src/vfs')

# ============================================================================
# Parsers
# ============================================================================

subdir('src/parsers')

# ============================================================================
# Slay grammar / parser toolkit
# ============================================================================

subdir('src/slay')

# ============================================================================
# N00b compiler (language-specific)
# ============================================================================

subdir('src/n00b')

libdl_dep = cc.find_library('dl', required: false)

# ============================================================================
# Logic / constraint solver
# ============================================================================

subdir('src/logic')

# ============================================================================
# Type checker
# ============================================================================

subdir('src/typecheck')

subdir('src/compiler/objfile')

# Aggregate per-module source lists into the libn00b source set. Each
# entry above (`subdir('src/<mod>')`) is responsible for declaring a
# `n00b_<mod>_src` variable; we just glue them together here.
n00b_dll_src = (
    n00b_core_src +
    n00b_util_src +
    n00b_adt_src +
    n00b_text_src +
    n00b_display_src +
    n00b_conduit_src +
    n00b_vfs_src +
    n00b_parsers_src +
    n00b_logic_src +
    n00b_slay_src +
    n00b_typecheck_src +
    n00b_compiler_src +
    n00b_objfile_src
)

# Build generated unicode tables with --no-ncc (they are pure data,
# no ncc extensions, and parsing them is extremely expensive).
gen_unicode_lib = static_library('n00b_gen_unicode',
    gen_unicode_tables,
    include_directories: incdir,
    c_args: exe_c_args + ['--no-ncc', '-UN00B_NCC_GLIBC_ASSERT_SHIM'],
    link_args: link_args,
)

# Build md4c vendor implementation with --no-ncc (it's 10K+ lines of
# pure C; parsing it through ncc's packrat parser OOMs in constrained
# environments like Docker cross-builds). The upstream linenoise source is
# POSIX-only, so Windows builds use a small non-interactive fallback.
vendor_sources = ['src/vendor/md4c.c']
if target_os == 'windows'
  vendor_sources += 'src/vendor/linenoise_windows.c'
else
  vendor_sources += 'src/vendor/linenoise.c'
endif

n00b_vendor_lib = static_library('n00b_vendor',
    vendor_sources,
    include_directories: incdir,
    c_args: exe_c_args + ['--no-ncc', '-UN00B_NCC_GLIBC_ASSERT_SHIM'],
    link_args: link_args,
)

link_whole_libs = [gen_unicode_lib, n00b_vendor_lib]

# Build the Cocoa backend as a separate ObjC static library (macOS only).
# ObjC files cannot go through ncc's C parser, so we compile them with
# the system ObjC compiler directly (similar to the vendor library pattern).
if conf_cocoa
  objc_comp = meson.get_compiler('objc')
  cocoa_objc_args = [
    '-Wextra',
    '-g',
    '-Wno-unused-parameter',
    '-Wno-unused-function',
    '-Wno-unknown-pragmas',
    '-fno-omit-frame-pointer',
    '-fPIC',
    '-Wno-atomic-alignment',
    '-fobjc-arc',
    '-DN00B_HOST_OS=' + host_machine.system(),
    '-DN00B_HOST_CPU=' + host_machine.cpu_family(),
  ]
  if conf_dev_mode
    cocoa_objc_args += '-DN00B_DEV'
  endif
  if conf_debug
    cocoa_objc_args += '-DN00B_DEBUG'
  endif

  n00b_cocoa_lib = static_library('n00b_cocoa',
      ['src/display/render/backend_cocoa.m'],
      include_directories: incdir,
      dependencies: deps,
      objc_args: cocoa_objc_args,
      link_args: link_args,
  )
  link_whole_libs += n00b_cocoa_lib
endif

# Build the notcurses backend as a separate static library.
# notcurses is a plain C library so this compiles through ncc normally,
# but we keep it separate to gate on the optional dependency.
if conf_notcurses
  n00b_notcurses_lib = static_library('n00b_notcurses',
      ['src/display/render/backend_notcurses.c'],
      include_directories: incdir,
      dependencies: deps,
      c_args: exe_c_args,
      link_args: link_args,
  )
  link_whole_libs += n00b_notcurses_lib
endif

n00b = static_library('n00b',
    n00b_dll_src,
    include_directories: incdir,
    dependencies: deps + [libdl_dep, mir_dep],
    c_args: exe_c_args,
    link_args: link_args,
    link_whole: link_whole_libs,
    install: true,
)

test_common_kwargs = {
    'include_directories': incdir,
    'c_args': exe_c_args,
    # `exe_link_args` carries the sanitizer link-line entries
    # (`-fsanitize=address` / `-fsanitize=undefined`) when those are
    # enabled at configure time, plus LTO flags.  Without this, tests
    # built off this kwarg block would link a libn00b that *was*
    # compiled with sanitizer hooks but no runtime, producing
    # `__ubsan_handle_*`-not-found at link time.  Phase 2 audit item
    # #9.
    'link_args': exe_link_args,
    'link_with': [n00b],
}

# QUIC echo demo — exercises the full transport end-to-end.

# Manifest-driven managed server (Phase 2 deliverable § 13.1).

# One-shot ACME cert acquisition demo (manual DNS-01 provider).
# Used by docs/quic/le_staging.md for manual interop verification.


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
# QUIC tests
# ============================================================================






# JWS encoder + ECDSA-P-256 sign + base64url + JWK thumbprint.

# ACME end-to-end (Phase 2.3a): directory + newNonce + newAccount.
# Skipped unless STEPCA_DIR_URL is in the env — start the fixture via
# `test/fixtures/stepca/start.sh` to populate it.

# CSR builder.  In-process structural test always runs; external
# `openssl req -verify` runs when an openssl binary is available.

# Orchestrator smoke test (argument validation; live cert acquisition
# is deferred to the Phase 2 deployment playbook because HTTP-01
# validation needs port 80).



# Endpoint hot-reload (cert + key swap).

# Sticky secret (stateless-reset / address-validation token rotation).

# LB-CID encoding (block-cipher mode, 16-byte fixed CID).

# Phase 2 fuzz tests — property tests across base64url, PEM, X.509
# validity walker, LB-CID decoder.  Same shape as the Phase 1
# framer fuzz: deterministic seed, ~5k iters per target.

# Manifest + preflight (design doc § 9).

# DNS providers (manual + cloudflare) + DNS-01 challenge wrapper.

# (removed: QUIC SNI routing — WIP, lives on another branch)

# (removed: QUIC trust→picotls bridge — WIP, lives on another branch)

# Phase 3 § 5.3 tail — mid-handshake-reload no-tear property.

# Live network integration test: skipped unless N00B_TEST_NETWORK=1.
# Hits Let's Encrypt's staging directory; verifies real TLS handshake
# through OS trust + HTTP/1.1 parse against a real ACME endpoint.








# Perf harness — built but not registered as a CI test (it's
# benchmarking, not assertion-based).  Run manually with
# `./build_debug/perf_quic_stream_scaling`.

# ============================================================================
# Conduit tests
# ============================================================================

type_registry_test = executable('test_type_registry',
    ['test/unit/test_type_registry.c'],
    kwargs: test_common_kwargs,
)
test('type_registry', type_registry_test, suite: 'unit')

type_hash_test = executable('test_type_hash',
    ['test/unit/test_type_hash.c'],
    kwargs: test_common_kwargs,
)
test('type_hash', type_hash_test, suite: 'unit')

conduit_test = executable('test_conduit',
    ['test/unit/test_conduit.c'],
    kwargs: test_common_kwargs,
)
test('conduit', conduit_test, suite: 'unit')

path_test = executable('test_path',
    ['test/unit/test_path.c'],
    kwargs: test_common_kwargs,
)
test('path', path_test, suite: 'unit')

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

socket_test = executable('test_socket',
    ['test/unit/test_socket.c'],
    kwargs: test_common_kwargs,
)
test('socket', socket_test, suite: 'unit')

fd_managed_test = executable('test_fd_managed',
    ['test/unit/test_fd_managed.c'],
    kwargs: test_common_kwargs,
)
test('fd_managed', fd_managed_test, suite: 'unit')

if target_os == 'windows'
    io_windows_test = executable('test_io_windows',
        ['test/unit/test_io_windows.c'],
        kwargs: test_common_kwargs,
    )
endif


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
test('subproc', subproc_test, suite: 'unit', timeout: 120)

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
# Regex engine tests
# ============================================================================

regex_charset_test = executable('test_regex_charset',
    ['test/unit/test_regex_charset.c'],
    kwargs: test_common_kwargs,
)
test('regex_charset', regex_charset_test, suite: 'unit')

regex_parse_test = executable('test_regex_parse',
    ['test/unit/test_regex_parse.c'],
    kwargs: test_common_kwargs,
)
test('regex_parse', regex_parse_test, suite: 'unit')

regex_match_test = executable('test_regex_match',
    ['test/unit/test_regex_match.c'],
    kwargs: test_common_kwargs,
)
test('regex_match', regex_match_test, suite: 'unit')

regex_api_test = executable('test_regex_api',
    ['test/unit/test_regex_api.c'],
    kwargs: test_common_kwargs,
)
test('regex_api', regex_api_test, suite: 'unit')

# ============================================================================
# Logic / constraint solver tests
# ============================================================================

logic_test_kwargs = {
    'include_directories': incdir + [include_directories('test/unit')],
    'c_args': exe_c_args,
    'link_args': exe_link_args,
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
# ML tests
# ============================================================================


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
test('bnf_json', bnf_json_test, suite: 'unit', workdir: meson.project_source_root())

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
test('c_parse', c_parse_test, suite: 'unit', workdir: meson.project_source_root())

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

tokenizer_registry_test = executable('test_tokenizer_registry',
    ['test/unit/test_tokenizer_registry.c'],
    kwargs: test_common_kwargs,
)
test('tokenizer_registry', tokenizer_registry_test, suite: 'unit')

text_tokenizer_test = executable('test_text_tokenizer',
    ['test/unit/test_text_tokenizer.c'],
    kwargs: test_common_kwargs,
)
test('text_tokenizer', text_tokenizer_test, suite: 'unit')

shell_tokenizer_test = executable('test_shell_tokenizer',
    ['test/unit/test_shell_tokenizer.c'],
    kwargs: test_common_kwargs,
)
test('shell_tokenizer', shell_tokenizer_test, suite: 'unit')

lisp_tokenizer_test = executable('test_lisp_tokenizer',
    ['test/unit/test_lisp_tokenizer.c'],
    kwargs: test_common_kwargs,
)
test('lisp_tokenizer', lisp_tokenizer_test, suite: 'unit')

json_tokenizer_test = executable('test_json_tokenizer',
    ['test/unit/test_json_tokenizer.c'],
    kwargs: test_common_kwargs,
)
test('json_tokenizer', json_tokenizer_test, suite: 'unit')

# ============================================================================
# LSP framework tests
# ============================================================================






# nccd backend lib (defined here so the test target below can link it).
nccd_backend_lib = static_library('nccd_backend',
    [
    ],
    include_directories: incdir + [include_directories('src/tools')],
    c_args: exe_c_args,
)


cf_label_test = executable('test_cf_label',
    ['test/unit/test_cf_label.c'],
    kwargs: test_common_kwargs,
)
test('cf_label', cf_label_test, suite: 'unit', workdir: meson.project_source_root())

cfg_test = executable('test_cfg',
    ['test/unit/test_cfg.c'],
    kwargs: test_common_kwargs,
)
test('cfg', cfg_test, suite: 'unit', workdir: meson.project_source_root())

cdg_test = executable('test_cdg',
    ['test/unit/test_cdg.c'],
    kwargs: test_common_kwargs,
)
test('cdg', cdg_test, suite: 'unit', workdir: meson.project_source_root())

dfg_test = executable('test_dfg',
    ['test/unit/test_dfg.c'],
    kwargs: test_common_kwargs,
)
test('dfg', dfg_test, suite: 'unit', workdir: meson.project_source_root())

# ============================================================================
# Type checker tests
# ============================================================================

typecheck_test = executable('test_typecheck',
    ['test/unit/test_typecheck.c'],
    kwargs: test_common_kwargs,
)
test('typecheck', typecheck_test, suite: 'unit', workdir: meson.project_source_root())

# ============================================================================
# Analysis tests
# ============================================================================

analyze_test = executable('test_analyze',
    ['test/unit/test_analyze.c'],
    kwargs: test_common_kwargs,
)
test('analyze', analyze_test, suite: 'unit', workdir: meson.project_source_root())

n00b_programs_test = executable('test_n00b_programs',
    ['test/unit/test_n00b_programs.c'],
    kwargs: test_common_kwargs,
)
test('n00b_programs', n00b_programs_test, suite: 'unit', workdir: meson.project_source_root())

n00b_types_test = executable('test_n00b_types',
    ['test/unit/test_n00b_types.c'],
    kwargs: test_common_kwargs,
)
test('n00b_types', n00b_types_test, suite: 'unit', workdir: meson.project_source_root())

infer_expr_test = executable('test_infer_expr',
    ['test/unit/test_infer_expr.c'],
    kwargs: test_common_kwargs,
)
test('infer_expr', infer_expr_test, suite: 'unit', workdir: meson.project_source_root())

# ============================================================================
# MIR JIT tests
# ============================================================================

mir_test = executable('test_mir',
    ['test/unit/test_mir.c'],
    include_directories: incdir,
    c_args: exe_c_args,
    link_args: exe_link_args,
    link_with: [n00b],
    dependencies: [mir_dep],
)
test('mir', mir_test, suite: 'unit')

codegen_test = executable('test_codegen',
    ['test/unit/test_codegen.c'],
    include_directories: incdir,
    c_args: exe_c_args,
    link_args: exe_link_args,
    link_with: [n00b],
    dependencies: [mir_dep],
)
test('codegen', codegen_test, suite: 'unit', workdir: meson.project_source_root())

codegen_coverage_test = executable('test_codegen_coverage',
    ['test/unit/test_codegen_coverage.c'],
    include_directories: incdir,
    c_args: exe_c_args,
    link_args: exe_link_args,
    link_with: [n00b],
    dependencies: [mir_dep],
)
test('codegen_coverage', codegen_coverage_test,
    suite: 'unit',
    workdir: meson.project_source_root(),
)

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

# Widget tests
label_test = executable('test_label',
    ['test/unit/test_label.c'],
    kwargs: test_common_kwargs,
)
test('label', label_test, suite: 'unit')

focus_test = executable('test_focus',
    ['test/unit/test_focus.c'],
    kwargs: test_common_kwargs,
)
test('focus', focus_test, suite: 'unit')

divider_test = executable('test_divider',
    ['test/unit/test_divider.c'],
    kwargs: test_common_kwargs,
)
test('divider', divider_test, suite: 'unit')

spacer_test = executable('test_spacer',
    ['test/unit/test_spacer.c'],
    kwargs: test_common_kwargs,
)
test('spacer', spacer_test, suite: 'unit')

progress_test = executable('test_progress',
    ['test/unit/test_progress.c'],
    kwargs: test_common_kwargs,
)
test('progress', progress_test, suite: 'unit')

button_test = executable('test_button',
    ['test/unit/test_button.c'],
    kwargs: test_common_kwargs,
)
test('button', button_test, suite: 'unit')

mouse_test = executable('test_mouse',
    ['test/unit/test_mouse.c'],
    kwargs: test_common_kwargs,
)
test('mouse', mouse_test, suite: 'unit')

checkbox_test = executable('test_checkbox',
    ['test/unit/test_checkbox.c'],
    kwargs: test_common_kwargs,
)
test('checkbox', checkbox_test, suite: 'unit')

input_test = executable('test_input',
    ['test/unit/test_input.c'],
    kwargs: test_common_kwargs,
)
test('input', input_test, suite: 'unit')

switch_test = executable('test_switch',
    ['test/unit/test_switch.c'],
    kwargs: test_common_kwargs,
)
test('switch', switch_test, suite: 'unit')

radio_test = executable('test_radio',
    ['test/unit/test_radio.c'],
    kwargs: test_common_kwargs,
)
test('radio', radio_test, suite: 'unit')

link_test = executable('test_link',
    ['test/unit/test_link.c'],
    kwargs: test_common_kwargs,
)
test('link', link_test, suite: 'unit')

list_widget_test = executable('test_list_widget',
    ['test/unit/test_list_widget.c'],
    kwargs: test_common_kwargs,
)
test('list_widget', list_widget_test, suite: 'unit')

selectionlist_test = executable('test_selectionlist',
    ['test/unit/test_selectionlist.c'],
    kwargs: test_common_kwargs,
)
test('selectionlist', selectionlist_test, suite: 'unit')

draw_cmd_test = executable('test_draw_cmd',
    ['test/unit/test_draw_cmd.c'],
    kwargs: test_common_kwargs,
)
test('draw_cmd', draw_cmd_test, suite: 'unit')

breadcrumb_test = executable('test_breadcrumb',
    ['test/unit/test_breadcrumb.c'],
    kwargs: test_common_kwargs,
)
test('breadcrumb', breadcrumb_test, suite: 'unit')

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

event_normalize_test = executable('test_event_normalize',
    ['test/unit/test_event_normalize.c'],
    kwargs: test_common_kwargs,
)
test('event_normalize', event_normalize_test, suite: 'unit')

if conf_cocoa
  cocoa_test = executable('test_cocoa_backend',
      ['test/unit/test_cocoa_backend.m'],
      include_directories: incdir,
      objc_args: cocoa_objc_args,
      link_with: [n00b],
  )
endif

if conf_notcurses
  notcurses_test = executable('test_notcurses_backend',
      ['test/unit/test_notcurses_backend.c'],
      kwargs: test_common_kwargs,
  )
endif

# Table CLI tool
table_tool = executable('n00b_table',
    ['src/tools/table.c'],
    kwargs: test_common_kwargs,
    install: true,
)

# Widget visual demo tool
widget_demo = executable('widget_demo',
    ['src/tools/widget_demo.c'],
    kwargs: test_common_kwargs,
    install: false,
)

# N00b dev tool (parse + annotate n00b source files, REPL)
n00b_dev = executable('n00b_dev',
    ['src/tools/n00b_dev.c', 'src/tools/n00b_repl.c'],
    include_directories: incdir,
    c_args: exe_c_args,
    link_args: exe_link_args,
    link_whole: [n00b],
    dependencies: [mir_dep],
    install: true,
)

# N00b main executable (compile, run, repl)
n00b_exe = executable('n00b',
    ['src/tools/n00b.c', 'src/tools/n00b_repl.c'],
    include_directories: incdir,
    c_args: exe_c_args,
    link_args: exe_link_args,
    link_whole: [n00b],
    dependencies: [mir_dep],
    install: true,
)

if target_os != 'windows'
    # VFS journaling depends on POSIX daemon, signal, and frontend behavior.
    vfs_journal = executable('vfs_journal',
        ['src/tools/vfs_journal.c'],
        kwargs: test_common_kwargs,
        install: false,
    )
endif

# nccd — the ncc Language Server.
# Phase 2: lexical-only ncc backend (semantic tokens, outline,
# bracket-balance diagnostics).  The backend lib is defined earlier
# (alongside the test target that links it).

# ============================================================================
# Integration tests
# ============================================================================

# Interpreter integration tests (shell runner invokes the n00b binary).
if not meson.is_cross_build()
  bash_prog = find_program('bash', required: true)
  test('n00b_interp',
      bash_prog,
      args: [
          files('test/interp/run_tests.sh'),
          n00b_exe.full_path(),
      ],
      suite: 'integration',
      workdir: meson.project_source_root(),
      timeout: 600,
  )
  test('n00b_compile_cache',
      bash_prog,
      args: [
          files('test/integration/run_compile_cache_tests.sh'),
          n00b_exe.full_path(),
      ],
      suite: 'integration',
      workdir: meson.project_source_root(),
      timeout: 120,
  )
endif

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
# VFS tests
# ============================================================================

vfs_backend_test = executable('test_vfs_backend',
    ['test/unit/test_vfs_backend.c'],
    kwargs: test_common_kwargs,
)
test('vfs_backend', vfs_backend_test, suite: 'unit')

vfs_core_test = executable('test_vfs_core',
    ['test/unit/test_vfs_core.c'],
    kwargs: test_common_kwargs,
)
test('vfs_core', vfs_core_test, suite: 'unit')

vfs_cache_test = executable('test_vfs_cache',
    ['test/unit/test_vfs_cache.c'],
    kwargs: test_common_kwargs,
)
test('vfs_cache', vfs_cache_test, suite: 'unit')

vfs_local_test = executable('test_vfs_local',
    ['test/unit/test_vfs_local.c'],
    kwargs: test_common_kwargs,
)
test('vfs_local', vfs_local_test, suite: 'unit')

vfs_journal_test = executable('test_vfs_journal',
    ['test/unit/test_vfs_journal.c'],
    kwargs: test_common_kwargs,
)
test('vfs_journal', vfs_journal_test, suite: 'unit')

# ============================================================================
# Unicode tests
# ============================================================================

unicode_test_kwargs = {
    'include_directories': incdir + [include_directories('test/unit')],
    'c_args': exe_c_args,
    'link_args': exe_link_args,
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
    'link_args': exe_link_args,
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
# Object file (LIEF) tests
# ============================================================================

objfile_test_names = [
    'types', 'bstream', 'writer', 'detect', 'elf', 'elf_build',
    'macho', 'macho_build', 'pe', 'pe_build', 'demangle',
    'abstract', 'dwarf',
]

foreach name : objfile_test_names
    exe = executable('test_objfile_' + name,
        'test/unit/test_objfile_' + name + '.c',
        kwargs: test_common_kwargs,
    )
    test('objfile_' + name, exe, suite: 'unit')
endforeach

# ============================================================================
# Examples (Mac-only — ObjC + libxpc)
# ============================================================================

if target_os == 'darwin'
    example_objc_args = [
        '-g',
        '-Wno-unused-parameter',
        '-Wno-unused-function',
        '-Wno-unknown-pragmas',
        '-fno-omit-frame-pointer',
        '-Wno-atomic-alignment',
        '-fobjc-arc',
    ]

    example_inc = [include_directories('examples/crayon_shared'), incdir]

    # ObjC libxpc subscriber — no n00b types, plain Apple toolchain.
    # Also hosts the xpc_dictionary_apply shim that hides Apple's block
    # syntax behind a plain-C function pointer.
    n00b_crayon_xpc = static_library('n00b_crayon_xpc',
        ['examples/crayon_shared/crayon_subscriber.m',
         'examples/crayon_shared/crayon_xpc_apply.m'],
        include_directories: example_inc,
        objc_args: example_objc_args,
    )

    # C bridges + features — touch n00b's _generic_struct macros, must
    # compile via ncc with the same flags as the rest of the project.
    n00b_crayon_helpers = static_library('n00b_crayon_helpers',
        ['examples/crayon_shared/crayon_xpc_to_json.c',
         'examples/crayon_shared/crayon_features.c'],
        include_directories: example_inc,
        c_args: exe_c_args + ['-fblocks'],
    )

    crayon_dump = executable('crayon_dump',
        ['examples/crayon_dump/main.m'],
        include_directories: example_inc,
        link_with: n00b_crayon_xpc,
        objc_args: example_objc_args,
        link_args: ['-framework', 'Foundation'],
    )



endif

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
