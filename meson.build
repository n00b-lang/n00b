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
macos_sdk = ''
if target_os == 'darwin'
  macos_sdk = run_command('xcrun', '--sdk', 'macosx', '--show-sdk-path', check: true).stdout().strip()
endif

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

if target_os == 'darwin'
  n00b_static_object_entry_attr = '__attribute__((section("__DATA,n00b_stobj"),used))'
elif target_os == 'windows'
  n00b_static_object_entry_attr = '__attribute__((section(".n00bs$m"),used))'
else
  n00b_static_object_entry_attr = '__attribute__((section("n00b_stobj"),used,retain))'
endif

n00b_rstr_template_plain = '({static n00b_string_t $0={.u8_bytes=$1,.data=$2,.codepoints=$3,.styling=((void*)0)};$14 static const n00b_static_object_desc_t $6={.start=(const void*)&$0,.len=(uint64_t)sizeof($0),.tinfo=$4,.scan_kind=$10,.scan_cb=$11,.scan_user=$12,.object_id=$8,.file=__FILE__,.identity=$15,.flags=$9};static const n00b_static_object_desc_t * const $7 $13=&$6;&$0;})'
n00b_rstr_template_styled = '({$0 static n00b_string_t $1={.u8_bytes=$2,.data=$3,.codepoints=$4,.styling=$5};$16 static const n00b_static_object_desc_t $8={.start=(const void*)&$1,.len=(uint64_t)sizeof($1),.tinfo=$6,.scan_kind=$12,.scan_cb=$13,.scan_user=$14,.object_id=$10,.file=__FILE__,.identity=$17,.flags=$11};static const n00b_static_object_desc_t * const $9 $15=&$8;&$1;})'
n00b_rstr_static_ref_template_plain = 'static n00b_string_t $0={.u8_bytes=$1,.data=$2,.codepoints=$3,.styling=((void*)0)};$14 static const n00b_static_object_desc_t $6={.start=(const void*)&$0,.len=(uint64_t)sizeof($0),.tinfo=$4,.scan_kind=$10,.scan_cb=$11,.scan_user=$12,.object_id=$8,.file=__FILE__,.identity=$15,.flags=$9};static const n00b_static_object_desc_t * const $7 $13=&$6;'
n00b_rstr_static_ref_template_styled = '$0 static n00b_string_t $1={.u8_bytes=$2,.data=$3,.codepoints=$4,.styling=$5};$16 static const n00b_static_object_desc_t $8={.start=(const void*)&$1,.len=(uint64_t)sizeof($1),.tinfo=$6,.scan_kind=$12,.scan_cb=$13,.scan_user=$14,.object_id=$10,.file=__FILE__,.identity=$17,.flags=$11};static const n00b_static_object_desc_t * const $9 $15=&$8;'
n00b_array_literal_data_template = '$13 static $0 $1[]={$3};$20 static const n00b_static_object_desc_t $15={.start=(const void*)$1,.len=(uint64_t)sizeof($1),.tinfo=$4,.scan_kind=$6,.scan_cb=$7,.scan_user=$8,.object_id=$17,.file=__FILE__,.identity=$21,.flags=$18};static const n00b_static_object_desc_t * const $16 $19=&$15;'

c_args = [
  '--ncc-vargs-type=n00b_vargs_t',
  '--ncc-rstr-string-type=n00b_string_t',
  '--ncc-rstr-text-style-type=n00b_text_style_t',
  '--ncc-rstr-style-record-type=n00b_style_record_t',
  '--ncc-static-object-entry-attr=' + n00b_static_object_entry_attr,
  '--ncc-rstr-template-plain=' + n00b_rstr_template_plain,
  '--ncc-rstr-template-styled=' + n00b_rstr_template_styled,
  '--ncc-rstr-static-ref-template-plain=' + n00b_rstr_static_ref_template_plain,
  '--ncc-rstr-static-ref-template-styled=' + n00b_rstr_static_ref_template_styled,
  '--ncc-rstr-static-ref-expr-plain=&$0',
  '--ncc-rstr-static-ref-expr-styled=&$1',
  '--ncc-array-literal-data-template=' + n00b_array_literal_data_template,
  '--ncc-array-literal-data-expr=$1',
  '-include',
  'n00b.h',
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
    link_args = ['-target', target_arch + '-apple-' + target_os,
                 '-isysroot', macos_sdk]
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
  exe_c_args = exe_c_args + ['-flto', '-DN00B_LTO_ENABLED',
                              '--ncc-lto-guard']
  exe_link_args = exe_link_args + ['-flto']
endif

if conf_release_build
  exe_c_args = exe_c_args + ['-O3', '-ffast-math', '-flto',
                              '--ncc-lto-guard']
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
libmath_dep = declare_dependency()
if host_machine.system() != 'darwin'
  libmath_dep = cc.find_library('m', required: false)
endif
libthreads_dep = dependency('threads', required: true)

deps = [
  libmath_dep,
  libthreads_dep,
]
if libatomic_dep.found()
  deps = deps + [libatomic_dep]
endif

if conf_cocoa
  cocoa_dep = declare_dependency(link_args: [
    '-framework', 'Cocoa',
    '-framework', 'CoreText',
    '-framework', 'CoreGraphics',
    '-framework', 'QuartzCore',
  ])
  deps = deps + [cocoa_dep]
endif

# macOS Security.framework + CoreFoundation: needed by the ACME trust
# verifier (acme_trust_macos.m) and Keychain secret bridge
# (secret_keychain.m).  Always-shipped Apple frameworks; unconditional
# on darwin.
if target_os == 'darwin'
  acme_trust_dep = declare_dependency(link_args: [
    '-framework', 'Security',
    '-framework', 'CoreFoundation',
  ])
  deps = deps + [acme_trust_dep]
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

# libzip — required by src/chalk/zip.c for the ZIP codec.
libzip_dep = dependency('libzip', required: false)
if libzip_dep.found()
  exe_c_args = exe_c_args + ['-DN00B_HAVE_LIBZIP=1']
  deps += libzip_dep
endif

# picotls + picoquic vendored subprojects.  Required by the QUIC stack
# (src/net/quic) and HTTP/3 client (src/net/http).  See
# docs/net/quic/vendored.md for the upstream-tracking pin file.
picotls_proj           = subproject('picotls')
picotls_minicrypto_dep = picotls_proj.get_variable('picotls_minicrypto_dep')

picoquic_proj = subproject('picoquic')
picoquic_dep  = picoquic_proj.get_variable('picoquic_core_dep')

# Monocypher (vendored, CC0 / BSD-2 dual): Ed25519 signing primitive
# for the n00b_attest signer vertical (WP-002).  In-tree copy at
# subprojects/monocypher/, sized down to the four files we actually
# use (monocypher{,-ed25519}.{h,c}).  See subprojects/monocypher/
# README.n00b.md for the integration shape rationale.
monocypher_proj = subproject('monocypher')
monocypher_dep  = monocypher_proj.get_variable('monocypher_dep')

# zlib for HTTP/1.1 + H3 Content-Encoding decompression (gzip/deflate).
# brotli + zstd are dlopen'd at runtime in src/net/http/http_compression.c.
if target_os == 'darwin'
  zlib_dep = declare_dependency(link_args: [macos_sdk / 'usr/lib/libz.tbd'])
else
  zlib_dep = dependency('zlib', required: true)
endif
deps = deps + [zlib_dep]


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
    'gen_names.c',
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

# Note: src/text/regex/ and src/util/simd/ (resharp-c port) are reached
# transitively via the src/text/ and src/util/ subdir() calls above.
# See ~/dd/n00b_regex_port_plan.md § 6 / § 7.

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

libdl_dep = declare_dependency()
if target_os != 'darwin'
  libdl_dep = cc.find_library('dl', required: false)
endif

# ============================================================================
# Logic / constraint solver
# ============================================================================

subdir('src/logic')

# ============================================================================
# Type checker
# ============================================================================

subdir('src/typecheck')

subdir('src/compiler/objfile')

subdir('src/chalk')

# ============================================================================
# Machine learning (feature-hashed binary logistic regression)
# ============================================================================

subdir('src/ml')

# ============================================================================
# Data formats: local landing sinks and readers
# ============================================================================

subdir('src/data')

# ============================================================================
# Networking: QUIC stack (phases 1-5) + HTTP/1.1+H3 client (phase 6)
# ============================================================================

subdir('src/net/quic')
subdir('src/net/http')

# ============================================================================
# Attestation (in-toto Statement + DSSE envelopes; signers / verifier
# arrive in later WPs — see .agents/work-plans/wp-001-* / WP-002 plan).
# ============================================================================

subdir('src/attest')
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
    n00b_objfile_src +
    n00b_chalk_src +
    n00b_ml_src +
    n00b_data_src +
    n00b_quic_src +
    n00b_http_src +
    n00b_attest_src
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

# QUIC's macOS ObjC bridges (ACME trust verifier, Keychain secret provider)
# are declared in src/net/quic/meson.build and exposed via the array below;
# fold them in once link_whole_libs exists.
link_whole_libs += n00b_quic_link_whole_libs

# libchalk's macOS ObjC bridge (Mach-O re-sign via codesign(1) shim) —
# same pattern as the QUIC bridges above.
link_whole_libs += n00b_chalk_link_whole_libs

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

# macOS ACME trust verifier + Keychain secret provider ObjC bridges
# are declared in src/net/quic/meson.build (per-subdir build files).
n00b = static_library('n00b',
    n00b_dll_src,
    include_directories: incdir,
    dependencies: deps + [libdl_dep, mir_dep, picoquic_dep, monocypher_dep],
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
    # libn00b now references picoquic symbols (endpoint, conn, chan).
    # Static-library deps don't propagate transitively in meson, so
    # every test that links libn00b also needs picoquic_dep on its
    # link line.  picoquic_dep brings the include path along too.
    # monocypher_dep is added for the same reason: libn00b's attest
    # sources (Phase 2+) will reference crypto_ed25519_* and the
    # attest_monocypher_smoke test calls those primitives directly.
    'dependencies': [picoquic_dep, monocypher_dep],
}

# Alias kept for clarity at QUIC test sites that drive picoquic
# explicitly; structurally identical to test_common_kwargs.
quic_test_kwargs = test_common_kwargs

n00b_static_init_helper = executable('n00b-static-init-helper',
    ['src/tools/n00b-static-init-helper.c'],
    include_directories: incdir,
    c_args: exe_c_args,
    link_args: exe_link_args,
    link_with: [n00b],
    dependencies: [picoquic_dep, monocypher_dep],
)

n00b_static_init_helper_ready = custom_target(
    'n00b-static-init-helper-ready',
    output: 'n00b_static_init_helper_ready.h',
    command: [
        python3,
        '-c',
        'import pathlib, sys; pathlib.Path(sys.argv[1]).write_text("/* generated */\\n")',
        '@OUTPUT@',
    ],
    depends: n00b_static_init_helper,
)

n00b_static_init_helper_flags = [
    '--ncc-static-init-helper=' + n00b_static_init_helper.full_path(),
]

# QUIC demo executables — each lives in its own per-subdir meson.build.
subdir('examples/quic_echo')
subdir('examples/quic_server_managed')
subdir('examples/quic_acme_demo')
subdir('examples/quic_acme_http01_demo')


array_test = executable('test_array',
    ['test/unit/test_array.c'],
    kwargs: test_common_kwargs,
)
test('array', array_test, suite: 'unit')

chalk_module_test = executable('test_chalk_module',
    ['test/unit/test_chalk_module.c'],
    kwargs: test_common_kwargs,
)
test('chalk_module', chalk_module_test, suite: 'unit')

attest_module_test = executable('test_attest_module',
    ['test/unit/test_attest_module.c'],
    kwargs: test_common_kwargs,
)
test('attest_module', attest_module_test, suite: 'unit')

attest_statement_roundtrip_test = executable('test_attest_statement_roundtrip',
    ['test/unit/test_attest_statement_roundtrip.c'],
    kwargs: test_common_kwargs,
)
test('attest_statement_roundtrip', attest_statement_roundtrip_test, suite: 'unit')

attest_dsse_roundtrip_test = executable('test_attest_dsse_roundtrip',
    ['test/unit/test_attest_dsse_roundtrip.c'],
    kwargs: test_common_kwargs,
)
test('attest_dsse_roundtrip', attest_dsse_roundtrip_test, suite: 'unit')

attest_arena_lifecycle_test = executable('test_attest_arena_lifecycle',
    ['test/unit/test_attest_arena_lifecycle.c'],
    kwargs: test_common_kwargs,
)
test('attest_arena_lifecycle', attest_arena_lifecycle_test, suite: 'unit')

attest_base64_test = executable('test_attest_base64',
    ['test/unit/test_attest_base64.c'],
    kwargs: test_common_kwargs,
)
test('attest_base64', attest_base64_test, suite: 'unit')

# n00b core util — `n00b_errno_str` POSIX errno-to-string helper.
# WP-004 Phase 5 closeout (DF-008 fix); PR-shaped libn00b lift
# matching the WP-002 wqw `n00b_stdin()/stdout()/stderr()` precedent
# (D-061).
errno_str_test = executable('test_errno_str',
    ['test/unit/test_errno_str.c'],
    kwargs: test_common_kwargs,
)
test('errno_str', errno_str_test, suite: 'unit')

# n00b core util — DER (X.690) encoder regression test.
# WP-005 Phase 3.
der_encode_test = executable('test_der_encode',
    ['test/unit/test_der_encode.c'],
    kwargs: test_common_kwargs,
)
test('der_encode', der_encode_test, suite: 'unit')

# n00b core util — PKCS#7 SignedData builder regression test.
# WP-005 Phase 3. Round-trips via picotls's read-side ASN.1
# validator. Post WP-005 P3-fixups sub-deliverable 4: loads
# committed PEM cert+key fixtures via picotls's PEM decoder, so
# the test needs `workdir: meson.project_source_root()` to
# resolve `test/unit/data/pkcs7_fixture_*.pem` relative to the
# source tree.
pkcs7_signed_data_test = executable('test_pkcs7_signed_data',
    ['test/unit/test_pkcs7_signed_data.c'],
    kwargs: test_common_kwargs,
)
test('pkcs7_signed_data', pkcs7_signed_data_test, suite: 'unit',
     workdir: meson.project_source_root())

# n00b core util — X.509 + PKCS#8 read-side walker regression test.
# WP-005 mid-stream cleanup lift. Loads the committed PEM cert+key
# fixtures and exercises both walkers. Needs `workdir:
# meson.project_source_root()` to resolve the relative PEM paths.
x509_walk_test = executable('test_x509_walk',
    ['test/unit/test_x509_walk.c'],
    kwargs: test_common_kwargs,
)
test('x509_walk', x509_walk_test, suite: 'unit',
     workdir: meson.project_source_root())

# libn00b core util — XDG Base Directory + typed-variadic path
# join + n00b_path_canonical regression. Exercises the spec matrix
# (var set / unset / empty) for each `_home()` variant, asserts
# `_runtime_dir()` returns nullptr when unset, exercises the
# variadic builder, and verifies the canonical combined resolver
# composes env-vars, tilde expansion, and absolute rooting.
path_xdg_test = executable('test_path_xdg',
    ['test/unit/test_path_xdg.c'],
    kwargs: test_common_kwargs,
)
test('path_xdg', path_xdg_test, suite: 'unit')

# libn00b core util — path-module anti-pattern sweep regression
# (pre-WP-005 P5). Exercises each of the seven swept functions
# (`split_on_slash` transitively via resolve_path, `n00b_resolve_path`,
# `n00b_path_walk`, `n00b_path_parts`, `n00b_find_file_in_program_path`,
# `n00b_list_directory`, `n00b_get_program_search_path`) and asserts
# push-after-return + readback works on each returned list — which
# forces `_n00b_list_ensure_cap` to read the list's scan-info /
# allocator fields. If a future commit reintroduces the
# `n00b_alloc(n00b_list_t(T)) + *p = n00b_list_new(T)` anti-pattern,
# scan-info is zero-initialized and the grow path either picks the
# wrong allocator or fails the readback identity check.
path_antipattern_sweep_test = executable('test_path_antipattern_sweep',
    ['test/unit/test_path_antipattern_sweep.c'],
    kwargs: test_common_kwargs,
)
test('path_antipattern_sweep', path_antipattern_sweep_test, suite: 'unit')

# n00b core PE — Authenticode certificate-table emit round-trip.
# WP-005 Phase 3 + P3-fixups sub-deliverable 3.
#
# Hybrid fixture wiring (per user kickoff Q3 disposition):
#   A. mingw-w64 + osslsigncode self-generated hello.exe.signed
#      (gated on toolchain availability; SKIPs cleanly when absent).
#   B. Sysinternals sigcheck64.exe (downloaded + SHA-256-verified at
#      configure time; NOT committed; SKIPs cleanly when network
#      unavailable). Previously pendmoves.exe — swapped 2026-05-21
#      because pendmoves ships only as PE32 (32-bit) and n00b_pe_parse
#      requires PE32+. sigcheck64.exe is the 64-bit variant from the
#      Sigcheck.zip distribution.
#   C. Always-runs synthetic in-memory PE blob cases as the primary.
#
# Each custom_target's helper script emits an EMPTY placeholder when
# its precondition fails (toolchain absent, network unavailable, SHA
# mismatch) so the meson rule still produces its declared output;
# the runtime test checks file size and SKIPs the case if empty.

mingw_gcc      = find_program('x86_64-w64-mingw32-gcc', required: false)
osslsigncode   = find_program('osslsigncode',           required: false)
pe_hello_exe = custom_target('pe_hello_exe',
    output: 'hello.exe',
    command: [
        meson.project_source_root() / 'test' / 'fixtures' / 'pe' / 'build_hello_exe.sh',
        meson.project_source_root() / 'test' / 'unit' / 'data' / 'hello.c',
        '@OUTPUT@',
    ],
    depend_files: files('test/unit/data/hello.c',
                        'test/fixtures/pe/build_hello_exe.sh'),
)
pe_hello_exe_signed = custom_target('pe_hello_exe_signed',
    output: 'hello.exe.signed',
    input: pe_hello_exe,
    command: [
        meson.project_source_root() / 'test' / 'fixtures' / 'pe' / 'sign_hello_exe.sh',
        '@INPUT@',
        meson.project_source_root() / 'test' / 'unit' / 'data' / 'pkcs7_fixture_cert.pem',
        meson.project_source_root() / 'test' / 'unit' / 'data' / 'pkcs7_fixture_key.pem',
        '@OUTPUT@',
    ],
    depend_files: files('test/unit/data/pkcs7_fixture_cert.pem',
                        'test/unit/data/pkcs7_fixture_key.pem',
                        'test/fixtures/pe/sign_hello_exe.sh'),
)
pe_sigcheck = custom_target('pe_sigcheck',
    output: 'sigcheck64.exe',
    command: [
        meson.project_source_root() / 'test' / 'fixtures' / 'pe' / 'fetch_sigcheck.sh',
        '@OUTPUT@',
    ],
    depend_files: files('test/fixtures/pe/fetch_sigcheck.sh'),
)
pe_cert_table_emit_test = executable('test_pe_cert_table_emit',
    ['test/unit/test_pe_cert_table_emit.c'],
    kwargs: test_common_kwargs,
)
test('pe_cert_table_emit', pe_cert_table_emit_test, suite: 'unit',
     depends: [pe_hello_exe, pe_hello_exe_signed, pe_sigcheck],
     workdir: meson.current_build_dir())

# WP-005 Phase 4 — libchalk PE re-sign end-to-end regression.
# Uses the same Sysinternals sigcheck64.exe fixture (downloaded
# into the build dir by the existing pe_sigcheck custom_target)
# and the committed pkcs7_fixture_{cert,key}.pem PEMs (resolved
# via MESON_SOURCE_ROOT, which meson sets for every test run).
# workdir = build dir so sigcheck64.exe resolves; the PEM
# fixtures come from the source tree.
chalk_pe_resign_test = executable('test_chalk_pe_resign',
    ['test/unit/test_chalk_pe_resign.c'],
    kwargs: test_common_kwargs,
)
test('chalk_pe_resign', chalk_pe_resign_test, suite: 'unit',
     depends: [pe_sigcheck],
     env: {'MESON_SOURCE_ROOT': meson.project_source_root()},
     workdir: meson.current_build_dir())

# WP-005 P5: Mach-O re-sign regression test. Always-runs; on macOS
# exercises the SecCodeSigner / codesign(1) bridge; on non-macOS
# exercises the strip-only fallback. Fixture: test/unit/data/hello.macho
# (committed). MESON_SOURCE_ROOT lets the test resolve the fixture path.
chalk_macho_resign_test = executable('test_chalk_macho_resign',
    ['test/unit/test_chalk_macho_resign.c'],
    kwargs: test_common_kwargs,
)
test('chalk_macho_resign', chalk_macho_resign_test, suite: 'unit',
     env: {'MESON_SOURCE_ROOT': meson.project_source_root()},
     workdir: meson.project_source_root())

attest_monocypher_smoke_test = executable('test_attest_monocypher_smoke',
    ['test/unit/test_attest_monocypher_smoke.c'],
    kwargs: test_common_kwargs,
)
test('attest_monocypher_smoke', attest_monocypher_smoke_test, suite: 'unit')

attest_signer_resolve_test = executable('test_attest_signer_resolve',
    ['test/unit/test_attest_signer_resolve.c'],
    kwargs: test_common_kwargs,
)
test('attest_signer_resolve', attest_signer_resolve_test, suite: 'unit')

attest_signer_release_test = executable('test_attest_signer_release',
    ['test/unit/test_attest_signer_release.c'],
    kwargs: test_common_kwargs,
)
test('attest_signer_release', attest_signer_release_test, suite: 'unit')

attest_signer_sign_test = executable('test_attest_signer_sign',
    ['test/unit/test_attest_signer_sign.c'],
    kwargs: test_common_kwargs,
)
test('attest_signer_sign', attest_signer_sign_test, suite: 'unit')

attest_signer_keyid_test = executable('test_attest_signer_keyid',
    ['test/unit/test_attest_signer_keyid.c'],
    kwargs: test_common_kwargs,
)
test('attest_signer_keyid', attest_signer_keyid_test, suite: 'unit')

attest_envelope_sign_test = executable('test_attest_envelope_sign',
    ['test/unit/test_attest_envelope_sign.c'],
    kwargs: test_common_kwargs,
)
test('attest_envelope_sign', attest_envelope_sign_test, suite: 'unit')

attest_envelope_signatures_roundtrip_test = executable(
    'test_attest_envelope_signatures_roundtrip',
    ['test/unit/test_attest_envelope_signatures_roundtrip.c'],
    kwargs: test_common_kwargs,
)
test('attest_envelope_signatures_roundtrip',
     attest_envelope_signatures_roundtrip_test,
     suite: 'unit')

attest_err_str_test = executable('test_attest_err_str',
    ['test/unit/test_attest_err_str.c'],
    kwargs: test_common_kwargs,
)
test('attest_err_str', attest_err_str_test, suite: 'unit')

attest_cli_sign_test = executable('test_attest_cli_sign',
    ['test/unit/test_attest_cli_sign.c'],
    kwargs: test_common_kwargs,
)
test('attest_cli_sign', attest_cli_sign_test, suite: 'unit')

attest_signer_arena_test = executable('test_attest_signer_arena',
    ['test/unit/test_attest_signer_arena.c'],
    kwargs: test_common_kwargs,
)
test('attest_signer_arena', attest_signer_arena_test, suite: 'unit')

attest_verifier_resolve_test = executable('test_attest_verifier_resolve',
    ['test/unit/test_attest_verifier_resolve.c'],
    kwargs: test_common_kwargs,
)
test('attest_verifier_resolve', attest_verifier_resolve_test, suite: 'unit')

attest_verifier_keyid_test = executable('test_attest_verifier_keyid',
    ['test/unit/test_attest_verifier_keyid.c'],
    kwargs: test_common_kwargs,
)
test('attest_verifier_keyid', attest_verifier_keyid_test, suite: 'unit')

attest_envelope_verify_signature_test = executable(
    'test_attest_envelope_verify_signature',
    ['test/unit/test_attest_envelope_verify_signature.c'],
    kwargs: test_common_kwargs,
)
test('attest_envelope_verify_signature',
     attest_envelope_verify_signature_test,
     suite: 'unit')

attest_envelope_verify_test = executable('test_attest_envelope_verify',
    ['test/unit/test_attest_envelope_verify.c'],
    kwargs: test_common_kwargs,
)
test('attest_envelope_verify', attest_envelope_verify_test, suite: 'unit')

attest_cli_verify_test = executable('test_attest_cli_verify',
    ['test/unit/test_attest_cli_verify.c'],
    kwargs: test_common_kwargs,
)
test('attest_cli_verify', attest_cli_verify_test, suite: 'unit')

attest_verifier_arena_test = executable('test_attest_verifier_arena',
    ['test/unit/test_attest_verifier_arena.c'],
    kwargs: test_common_kwargs,
)
test('attest_verifier_arena', attest_verifier_arena_test, suite: 'unit')

attest_oci_client_resolve_test = executable('test_attest_oci_client_resolve',
    ['test/unit/test_attest_oci_client_resolve.c'],
    kwargs: test_common_kwargs,
)
test('attest_oci_client_resolve', attest_oci_client_resolve_test, suite: 'unit')

# WP-004 Phase 4 hardening — FR-22 arena friendliness for the OCI
# client surface. Mirrors the WP-002 / WP-003 arena-test precedent
# (test_attest_signer_arena / test_attest_verifier_arena): N=50
# resolve + in-memory op + release through one arena, GC-collect,
# assert arena-used drops back. Always-runs (no Docker / no network).
attest_oci_arena_test = executable('test_attest_oci_arena',
    ['test/unit/test_attest_oci_arena.c'],
    kwargs: test_common_kwargs,
)
test('attest_oci_arena', attest_oci_arena_test, suite: 'unit')

# WP-004 Phase 4 hardening — redirect-policy delegation regression.
# Verifies the OCI client correctly stores per-call kwargs
# (`redirect_host_allowlist`, `allow_redirects`) on the handle so
# libn00b's dispatcher can pick them up once the matching surface
# lifts. Always-runs.
attest_oci_redirect_policy_test = executable(
    'test_attest_oci_redirect_policy',
    ['test/unit/test_attest_oci_redirect_policy.c'],
    kwargs: test_common_kwargs,
)
test('attest_oci_redirect_policy',
     attest_oci_redirect_policy_test,
     suite: 'unit')

# WP-004 Phase 4 hardening — per-call body-size cap surface. Asserts
# the size-cap error codes are wired and the fetcher kwarg blocks
# parse correctly with both `max_size` and `timeout_ms`. Always-runs.
attest_oci_size_caps_test = executable('test_attest_oci_size_caps',
    ['test/unit/test_attest_oci_size_caps.c'],
    kwargs: test_common_kwargs,
)
test('attest_oci_size_caps', attest_oci_size_caps_test, suite: 'unit')

attest_oci_manifest_test = executable('test_attest_oci_manifest',
    ['test/unit/test_attest_oci_manifest.c'],
    kwargs: test_common_kwargs,
)
test('attest_oci_manifest', attest_oci_manifest_test, suite: 'unit')

attest_oci_push_smoke_test = executable('test_attest_oci_push_smoke',
    ['test/unit/test_attest_oci_push_smoke.c'],
    kwargs: test_common_kwargs,
)
# WP-004 Phase 2 trailing addition (DF-012): Docker-gated full
# HEAD/POST/PUT/PUT push against the zot fixture in
# test/fixtures/zot/.  N00B_TEST_ZOT_SRC_ROOT carries the absolute
# repo-root path so the test can find start.sh / stop.sh without
# depending on the user-side `eval $(start.sh)` pre-step that the
# caddy/nginx-quic smokes use.  Skips cleanly when N00B_TEST_DOCKER
# is unset.
test('attest_oci_push_smoke', attest_oci_push_smoke_test,
     suite: 'unit',
     timeout: 90,
     env: {
         'N00B_TEST_ZOT_SRC_ROOT': meson.current_source_dir(),
     })

# WP-004 Phase 3: discover-side smoke against zot. Pushes two
# envelopes with distinct predicate-types and asserts list_referrers
# returns both. Skips cleanly when N00B_TEST_DOCKER is unset.
attest_oci_discover_smoke_test = executable('test_attest_oci_discover_smoke',
    ['test/unit/test_attest_oci_discover_smoke.c'],
    kwargs: test_common_kwargs,
)
test('attest_oci_discover_smoke', attest_oci_discover_smoke_test,
     suite: 'unit',
     timeout: 90,
     env: {
         'N00B_TEST_ZOT_SRC_ROOT': meson.current_source_dir(),
     })

# WP-004 Phase 3: pull-side smoke against zot. Pushes one envelope,
# pulls it back, asserts byte-identical; also verifies the post-
# filter rejects non-matching predicate-types.
attest_oci_pull_smoke_test = executable('test_attest_oci_pull_smoke',
    ['test/unit/test_attest_oci_pull_smoke.c'],
    kwargs: test_common_kwargs,
)
test('attest_oci_pull_smoke', attest_oci_pull_smoke_test,
     suite: 'unit',
     timeout: 90,
     env: {
         'N00B_TEST_ZOT_SRC_ROOT': meson.current_source_dir(),
     })

# WP-004 Phase 3: the headline cross-WP end-to-end smoke against
# zot. Chains sign -> push -> discover -> pull -> verify against
# a real OCI registry; passing this test means the entire
# n00b-attestation producer-to-consumer path is wired.
attest_oci_end_to_end_smoke_test = executable(
    'test_attest_oci_end_to_end_smoke',
    ['test/unit/test_attest_oci_end_to_end_smoke.c'],
    kwargs: test_common_kwargs,
)
test('attest_oci_end_to_end_smoke', attest_oci_end_to_end_smoke_test,
     suite: 'unit',
     timeout: 120,
     env: {
         'N00B_TEST_ZOT_SRC_ROOT': meson.current_source_dir(),
     })

# WP-005 Phase 1: ATTESTATION JSON builder byte-stability golden
# test. Three sub-tests (bundled+hint, lazy, no hint) cover the
# canonical-field-order disposition (D-056) + the per-mode shape
# transitions. Always-runs.
attest_mark_attestation_json_test = executable(
    'test_attest_mark_attestation_json',
    ['test/unit/test_attest_mark_attestation_json.c'],
    kwargs: test_common_kwargs,
)
test('attest_mark_attestation_json',
     attest_mark_attestation_json_test,
     suite: 'unit')

# WP-005 Phase 1: `n00b_attest_mark_artifact` IC-4 hash contract +
# registry_hint validation. Always-runs. Builds a tiny ELF fixture
# in-process via libn00b's ELF builder (no fixture binary checked
# in).
attest_mark_artifact_test = executable(
    'test_attest_mark_artifact',
    ['test/unit/test_attest_mark_artifact.c'],
    kwargs: test_common_kwargs,
)
test('attest_mark_artifact',
     attest_mark_artifact_test,
     suite: 'unit')

# WP-005 Phase 1: `n00b_attest_extract_from_artifact` IC-5 sentinel
# discrimination across all four cases (i..iv). Always-runs. Uses
# ELF + Mach-O fixtures built in-process plus a non-binary blob for
# the IC-5-(iv) case.
attest_extract_from_artifact_test = executable(
    'test_attest_extract_from_artifact',
    ['test/unit/test_attest_extract_from_artifact.c'],
    kwargs: test_common_kwargs,
)
test('attest_extract_from_artifact',
     attest_extract_from_artifact_test,
     suite: 'unit')

# WP-005 Phase 6: end-to-end mark + extract regression on the ELF
# codec (no resign step — the no-resign dispatch path on a
# signer_identity = nullptr call). Always-runs. Builds a tiny ELF
# fixture in-process via libn00b's ELF builder. IC-4 invariant
# verified end-to-end: the SHA-256 of the pre-mark artifact bytes
# equals the extracted envelope's subject.digest.sha256.
attest_mark_elf_e2e_test = executable(
    'test_attest_mark_elf_e2e',
    ['test/unit/test_attest_mark_elf_e2e.c'],
    kwargs: test_common_kwargs,
)
test('attest_mark_elf_e2e',
     attest_mark_elf_e2e_test,
     suite: 'unit')

# WP-005 Phase 6: end-to-end mark + Authenticode resign regression
# on the PE codec (cross-platform Authenticode signing — the P4
# resign primitive is deterministic and runs on any host). Always-
# runs. Uses an in-test synthetic PE32+ binary (`n00b_pe_binary_new`)
# and the committed PEM fixtures resolved via MESON_SOURCE_ROOT.
# IC-4 invariant verified end-to-end.
attest_mark_pe_e2e_test = executable(
    'test_attest_mark_pe_e2e',
    ['test/unit/test_attest_mark_pe_e2e.c'],
    kwargs: test_common_kwargs,
)
test('attest_mark_pe_e2e',
     attest_mark_pe_e2e_test,
     suite: 'unit',
     env: {'MESON_SOURCE_ROOT': meson.project_source_root()},
     workdir: meson.current_build_dir())

# WP-005 Phase 6: end-to-end mark + Mach-O resign regression,
# host-gated to macOS (the resign body relies on the
# Security-framework / codesign(1) bridge). Uses the committed
# test/unit/data/hello.macho fixture + the PEM fixtures resolved
# via MESON_SOURCE_ROOT. IC-4 invariant verified end-to-end; the
# post-resign bytes are additionally checked via
# /usr/bin/codesign --verify --deep --strict (non-fatal SKIP on
# tooling failure).
if host_machine.system() == 'darwin'
    attest_mark_macho_e2e_test = executable(
        'test_attest_mark_macho_e2e_macos',
        ['test/unit/test_attest_mark_macho_e2e_macos.c'],
        kwargs: test_common_kwargs,
    )
    test('attest_mark_macho_e2e_macos',
         attest_mark_macho_e2e_test,
         suite: 'unit',
         env: {'MESON_SOURCE_ROOT': meson.project_source_root()},
         workdir: meson.project_source_root())
endif

# WP-005 Phase 2: `n00b_attest_cli_mark` end-to-end regression
# (envelope-bytes → parse → mark dispatch). Always-runs; exercises
# the IC-4 unchalked-hash contract through the Phase-2 wrapper,
# --lazy mode, the CR-13 multi-envelope contract, and input
# validation.
attest_cli_mark_test = executable(
    'test_attest_cli_mark',
    ['test/unit/test_attest_cli_mark.c'],
    kwargs: test_common_kwargs,
)
test('attest_cli_mark',
     attest_cli_mark_test,
     suite: 'unit')

# WP-005 Phase 2: `n00b_attest_cli_unmark` mark→unmark→extract
# round-trip. Always-runs; post-unmark extract must surface the
# IC-5 (i) CHALK_NO_MARK sentinel.
attest_cli_unmark_test = executable(
    'test_attest_cli_unmark',
    ['test/unit/test_attest_cli_unmark.c'],
    kwargs: test_common_kwargs,
)
test('attest_cli_unmark',
     attest_cli_unmark_test,
     suite: 'unit')

# WP-005 Phase 2: `n00b_attest_cli_extract` mark→extract round-trip.
# Always-runs; covers bundled-mode round-trip, lazy-mode round-trip,
# no-mark sentinel, and input validation.
attest_cli_extract_test = executable(
    'test_attest_cli_extract',
    ['test/unit/test_attest_cli_extract.c'],
    kwargs: test_common_kwargs,
)
test('attest_cli_extract',
     attest_cli_extract_test,
     suite: 'unit')

# DF-013 regression: caller-supplied trust handle threaded through the
# h1 transport.  Same zot fixture as attest_oci_push_smoke, but the
# test exercises the libn00b-core path (n00b_http_request_sync ->
# n00b_http_h1_round_trip -> n00b_acme_tls_connect_ex) directly via a
# GET /v2/ rather than the attest-module OCI push surface.  Skips
# cleanly when N00B_TEST_DOCKER is unset.
h1_pinned_trust_test = executable('test_h1_pinned_trust',
    ['test/unit/test_h1_pinned_trust.c'],
    kwargs: test_common_kwargs,
)
test('h1_pinned_trust', h1_pinned_trust_test,
     suite: 'unit',
     timeout: 60,
     env: {
         'N00B_TEST_ZOT_SRC_ROOT': meson.current_source_dir(),
     })

list_test = executable('test_list',
    ['test/unit/test_list.c'],
    kwargs: test_common_kwargs,
)
test('list', list_test, suite: 'unit')

ring_test = executable('test_ring',
    ['test/unit/test_ring.c'],
    kwargs: test_common_kwargs,
)
test('ring', ring_test, suite: 'unit')

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

gc_scan_map_test = executable('test_gc_scan_map',
    ['test/unit/test_gc_scan_map.c'],
    kwargs: test_common_kwargs,
)
test('gc_scan_map', gc_scan_map_test, suite: 'unit')

gc_stack_test = executable('test_gc_stack',
    ['test/unit/test_gc_stack.c'],
    kwargs: test_common_kwargs,
)
test('gc_stack', gc_stack_test, suite: 'unit')

gc_stack_nonlocal_test = executable('test_gc_stack_nonlocal',
    ['test/unit/test_gc_stack_nonlocal.c'],
    kwargs: test_common_kwargs + {
        'c_args': exe_c_args + ['--ncc-gc-stack-maps'],
    },
)
test('gc_stack_nonlocal', gc_stack_nonlocal_test, suite: 'unit')

static_descriptor_test = executable('test_static_descriptor',
    ['test/unit/test_static_descriptor.c'],
    kwargs: test_common_kwargs,
)
test('static_descriptor', static_descriptor_test, suite: 'unit')

static_object_sections_test = executable('test_static_object_sections',
    [
      'test/unit/test_static_object_sections.c',
      'test/unit/test_static_object_sections_extra.c',
    ],
    kwargs: test_common_kwargs,
)
test('static_object_sections', static_object_sections_test, suite: 'unit')

ncc_static_objects_test = executable('test_ncc_static_objects',
    ['test/unit/test_ncc_static_objects.c'],
    kwargs: test_common_kwargs,
)
test('ncc_static_objects', ncc_static_objects_test, suite: 'unit')

ncc_static_images_test = executable('test_ncc_static_images',
    [
      'test/unit/test_ncc_static_images.c',
      n00b_static_init_helper_ready,
    ],
    kwargs: test_common_kwargs + {
        'c_args': exe_c_args + n00b_static_init_helper_flags,
    },
)
test('ncc_static_images', ncc_static_images_test, suite: 'unit')

gc_selective_scan_test = executable('test_gc_selective_scan',
    ['test/unit/test_gc_selective_scan.c'],
    kwargs: test_common_kwargs,
)
test('gc_selective_scan', gc_selective_scan_test, suite: 'unit')

container_scan_kind_test = executable('test_container_scan_kind',
    ['test/unit/test_container_scan_kind.c'],
    kwargs: test_common_kwargs,
)
test('container_scan_kind', container_scan_kind_test, suite: 'unit')

finalizer_test = executable('test_finalizer',
    ['test/unit/test_finalizer.c'],
    kwargs: test_common_kwargs,
)
test('finalizer', finalizer_test, suite: 'unit')

current_allocator_test = executable('test_current_allocator',
    ['test/unit/test_current_allocator.c'],
    kwargs: test_common_kwargs,
)
test('current_allocator', current_allocator_test, suite: 'unit')

buffer_test = executable('test_buffer',
    ['test/unit/test_buffer.c'],
    kwargs: test_common_kwargs,
)
test('buffer', buffer_test, suite: 'unit')

marshal_test = executable('test_marshal',
    ['test/unit/test_marshal.c'],
    kwargs: test_common_kwargs,
)
test('marshal', marshal_test, suite: 'unit')

file_io_test = executable('test_file_io',
    ['test/unit/test_file_io.c'],
    kwargs: test_common_kwargs,
)
test('file_io', file_io_test, suite: 'unit')

parquet_flat_test = executable('test_parquet_flat',
    ['test/unit/test_parquet_flat.c'],
    kwargs: test_common_kwargs,
)
test('parquet_flat', parquet_flat_test, suite: 'unit')

mutex_test = executable('test_mutex',
    ['test/unit/test_mutex.c'],
    kwargs: test_common_kwargs,
)
test('mutex', mutex_test, suite: 'unit')

stw_test = executable('test_stw',
    ['test/unit/test_stw.c'],
    kwargs: test_common_kwargs,
)
test('stw', stw_test, suite: 'unit')

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

type_static_layout_test = executable('test_type_static_layout',
    ['test/unit/test_type_static_layout.c'],
    kwargs: test_common_kwargs,
)
test('type_static_layout', type_static_layout_test, suite: 'unit')

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

xform_marshal_test = executable('test_xform_marshal',
    ['test/unit/test_xform_marshal.c'],
    kwargs: test_common_kwargs,
)
test('xform_marshal', xform_marshal_test, suite: 'unit')

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

std_streams_test = executable('test_std_streams',
    ['test/unit/test_std_streams.c'],
    kwargs: test_common_kwargs,
)
test('std_streams', std_streams_test, suite: 'unit')

fd_owner_read_all_test = executable('test_fd_owner_read_all',
    ['test/unit/test_fd_owner_read_all.c'],
    kwargs: test_common_kwargs,
)
test('fd_owner_read_all', fd_owner_read_all_test, suite: 'unit')

# ============================================================================
# Regex engine tests — registered via subdir during Phase 11 of the regex port.
# See ~/dd/n00b_regex_port_plan.md § 7 / Phase 11.
# ============================================================================

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

json_contract_test = executable('test_json_contract',
    ['test/unit/test_json_contract.c'],
    kwargs: test_common_kwargs,
)
test('json_contract', json_contract_test, suite: 'unit')

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

# ============================================================================
# Machine learning tests (heap + stats supporting primitives + ml)
# ============================================================================

heap_test = executable('test_heap',
    ['test/unit/test_heap.c'],
    kwargs: test_common_kwargs,
)
test('heap', heap_test, suite: 'unit')

stats_test = executable('test_stats',
    ['test/unit/test_stats.c'],
    kwargs: test_common_kwargs,
)
test('stats', stats_test, suite: 'unit')

ml_test = executable('test_ml',
    ['test/unit/test_ml.c'],
    kwargs: test_common_kwargs,
)
test('ml', ml_test, suite: 'unit')

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

# n00b-attest CLI binary (sign verb; other verbs are forward-roadmap
# stubs surfacing in --help and emitting "not yet implemented" + exit 1
# per DF-004 disposition (a)).
n00b_attest_tool = executable('n00b-attest',
    ['src/tools/n00b-attest.c'],
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

# ============================================================================
# Integration tests
# ============================================================================

# Interpreter integration tests (shell runner invokes the n00b binary).
if not meson.is_cross_build()
  bash_prog = find_program('bash', required: true)
  test('interp',
      bash_prog,
      args: [
          files('test/interp/run_tests.sh'),
          n00b_exe.full_path(),
      ],
      suite: ['interp', 'long'],
      workdir: meson.project_source_root(),
      timeout: 600,
  )
  test('n00b_compile_cache',
      bash_prog,
      args: [
          files('test/integration/run_compile_cache_tests.sh'),
          n00b_exe.full_path(),
      ],
      suite: ['integration', 'long'],
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

# Crayon ObjC/libxpc examples (Mac-only) live on the ML sibling branch
# along with examples/crayon_shared/.  They re-land with that branch.

# ============================================================================
# test_heap and test_stats live with the adt/heap.c source on a
# different feature branch; not built in this QUIC branch.

# ============================================================================
# QUIC tests
# ============================================================================

quic_framer_test = executable('test_quic_framer',
    ['test/unit/test_quic_framer.c'],
    kwargs: test_common_kwargs,
)
test('quic_framer', quic_framer_test, suite: 'unit')

quic_framer_fuzz_test = executable('test_quic_framer_fuzz',
    ['test/unit/test_quic_framer_fuzz.c'],
    kwargs: test_common_kwargs,
)
test('quic_framer_fuzz', quic_framer_fuzz_test, suite: 'unit', timeout: 60)

quic_trust_test = executable('test_quic_trust',
    ['test/unit/test_quic_trust.c'],
    kwargs: test_common_kwargs,
)
test('quic_trust', quic_trust_test, suite: 'unit')

quic_secret_test = executable('test_quic_secret',
    ['test/unit/test_quic_secret.c'],
    kwargs: test_common_kwargs,
)
test('quic_secret', quic_secret_test, suite: 'unit')

# Linux-only libsecret bridge.  Compiled and registered only when
# pkg-config locates libsecret-1; the test SKIPs (exit 77) when no
# Secret Service daemon is reachable at run-time, so the suite still
# passes on Linux hosts without gnome-keyring / kwallet.
if have_libsecret
    quic_secret_libsecret_kwargs = {}
    foreach _k, _v : test_common_kwargs
        if _k == 'dependencies'
            quic_secret_libsecret_kwargs += {_k: _v + [libsecret_dep]}
        else
            quic_secret_libsecret_kwargs += {_k: _v}
        endif
    endforeach
    quic_secret_libsecret_test = executable(
        'test_quic_secret_libsecret',
        ['test/unit/test_quic_secret_libsecret.c'],
        kwargs: quic_secret_libsecret_kwargs,
    )
    test('quic_secret_libsecret', quic_secret_libsecret_test, suite: 'unit')
endif

# test_quic_acme_http removed in Phase 6 chunk 11 — its coverage
# (URL parser, header bag, response parser) is replaced by
# test_http_url, test_http_h1, and test_http_compression.

# JWS encoder + ECDSA-P-256 sign + base64url + JWK thumbprint.
quic_jws_test = executable('test_quic_jws',
    ['test/unit/test_quic_jws.c'],
    kwargs: test_common_kwargs,
)
test('quic_jws', quic_jws_test, suite: 'unit')

# Phase 3 § 6 — JWT verifier: compact JWS parse, ES256 verify,
# claim validation, alg refusals.  RS256 verify (rsa_verify.c) is
# exercised inline through the JSON dispatch in jwt.c.
quic_jwt_test = executable('test_quic_jwt',
    ['test/unit/test_quic_jwt.c'],
    kwargs: test_common_kwargs,
)
test('quic_jwt', quic_jwt_test, suite: 'unit')

# Phase 3 § 7 — OIDC discovery + JWKS cache.  Offline-only;
# live-IdP tests live in the Phase 3.10 fixture suite.
quic_oidc_test = executable('test_quic_oidc',
    ['test/unit/test_quic_oidc.c'],
    kwargs: test_common_kwargs,
)
test('quic_oidc', quic_oidc_test, suite: 'unit')

# Phase 3 § 8 — DPoP issue + verify (RFC 9449).
quic_dpop_test = executable('test_quic_dpop',
    ['test/unit/test_quic_dpop.c'],
    kwargs: test_common_kwargs,
)
test('quic_dpop', quic_dpop_test, suite: 'unit')

# SHA-512 / SHA-384 — FIPS 180-4 vectors validate the new
# core/sha512.c that underpins RS384/RS512 JWS verification.
sha512_test = executable('test_sha512',
    ['test/unit/test_sha512.c'],
    kwargs: test_common_kwargs,
)
test('sha512', sha512_test, suite: 'unit')

# Phase 3 § 9 — mTLS-bound tokens (RFC 8705).
quic_mtls_token_test = executable('test_quic_mtls_token',
    ['test/unit/test_quic_mtls_token.c'],
    kwargs: test_common_kwargs,
)
test('quic_mtls_token', quic_mtls_token_test, suite: 'unit')

# Phase 3 § 10 — per-channel auth policy.
quic_auth_policy_test = executable('test_quic_auth_policy',
    ['test/unit/test_quic_auth_policy.c'],
    kwargs: test_common_kwargs,
)
test('quic_auth_policy', quic_auth_policy_test, suite: 'unit')

# Phase 3 § 12 / § 10.2 — audit log shape + JSONL sink.
quic_audit_test = executable('test_quic_audit',
    ['test/unit/test_quic_audit.c'],
    kwargs: test_common_kwargs,
)
test('quic_audit', quic_audit_test, suite: 'unit')

# Phase 5 § 5.1 — Prometheus metrics registry + listener.
quic_metrics_test = executable('test_quic_metrics',
    ['test/unit/test_quic_metrics.c'],
    kwargs: test_common_kwargs,
)
test('quic_metrics', quic_metrics_test, suite: 'unit')

# Phase 6 chunk 1 — HTTPS URL parser.
http_url_test = executable('test_http_url',
    ['test/unit/test_http_url.c'],
    kwargs: test_common_kwargs,
)
test('http_url', http_url_test, suite: 'unit')

# Phase 6 chunk 2 — HTTP/1.1 response parser + request builder
# (sub-chunks 2.1 + 2.2).
http_h1_test = executable('test_http_h1',
    ['test/unit/test_http_h1.c'],
    kwargs: test_common_kwargs,
)
test('http_h1', http_h1_test, suite: 'unit')

# Phase 6 chunk 2.3 — live-network smoke for the h1 round-trip
# path.  Skipped unless N00B_TEST_NET=1 (3 endpoints, 2-of-3
# tolerant gate per ~/dd/quic_6.md § 8.2).
http_h1_network_test = executable('test_http_h1_network',
    ['test/unit/test_http_h1_network.c'],
    kwargs: test_common_kwargs,
)
test('http_h1_network', http_h1_network_test,
     suite: 'unit', timeout: 60)

# Phase 6 chunk 3 — h3 round-trip primitive (argument validation
# + DNS failure path; full end-to-end exercises through the
# existing Caddy fixture when chunk 5's dispatcher lands).
http_h3_test = executable('test_http_h3',
    ['test/unit/test_http_h3.c'],
    kwargs: test_common_kwargs,
)
test('http_h3', http_h3_test, suite: 'unit', timeout: 30)

# Phase 6 chunk 3 — h3 informational network smoke (skipped unless
# N00B_TEST_NET=1).  Tolerant of trust-bridge gaps until chunk 9.
http_h3_network_test = executable('test_http_h3_network',
    ['test/unit/test_http_h3_network.c'],
    kwargs: test_common_kwargs,
)
test('http_h3_network', http_h3_network_test,
     suite: 'unit', timeout: 60)

# Phase 6 chunk 4 — connection pool.
http_pool_test = executable('test_http_pool',
    ['test/unit/test_http_pool.c'],
    kwargs: test_common_kwargs,
)
test('http_pool', http_pool_test, suite: 'unit')

# Minimal rwlock reader/writer contention repro.
rwlock_contention_test = executable('test_rwlock_contention',
    ['test/unit/test_rwlock_contention.c'],
    kwargs: test_common_kwargs,
)
test('rwlock_contention', rwlock_contention_test, suite: 'unit', timeout: 30)

# PoC for the picotls CertificateVerify gap (see audit).
tls_certverify_gap_test = executable('test_tls_certverify_gap',
    ['test/unit/test_tls_certverify_gap.c'],
    kwargs: test_common_kwargs,
)
test('tls_certverify_gap', tls_certverify_gap_test, suite: 'unit',
     timeout: 10)

# In-process mTLS client-auth handshake test (both matched-key
# and mismatched-key paths).
acme_tls_mtls_test = executable('test_acme_tls_mtls',
    ['test/unit/test_acme_tls_mtls.c'],
    kwargs: test_common_kwargs,
)
test('acme_tls_mtls', acme_tls_mtls_test, suite: 'unit', timeout: 10)

# RSA-PSS-SHA256 sign+verify roundtrip — math-level coverage for the
# new n00b_rsa_sign_pss_sha256 primitive.  Schoolbook bn_powmod with
# the full 2048-bit private exponent is slow under -O0; bump the
# timeout accordingly.
rsa_pss_roundtrip_test = executable('test_rsa_pss_roundtrip',
    ['test/unit/test_rsa_pss_roundtrip.c'],
    kwargs: test_common_kwargs,
)
test('rsa_pss_roundtrip', rsa_pss_roundtrip_test,
     suite: ['unit', 'long'], timeout: 60)

# End-to-end QUIC mTLS handshake (loopback) — covers the h3 / picoquic
# side of n00b_picotls_install_client_auth, including the server-side
# verify_sign installation that catches forged client CertificateVerify.
quic_mtls_handshake_test = executable('test_quic_mtls_handshake',
    ['test/unit/test_quic_mtls_handshake.c'],
    kwargs: test_common_kwargs,
)
test('quic_mtls_handshake', quic_mtls_handshake_test, suite: 'unit',
     timeout: 30)

# Phase 6 chunk 5 — public dispatcher (synchronous form;
# topic-shaped variant follows in a sub-chunk).
http_client_test = executable('test_http_client',
    ['test/unit/test_http_client.c'],
    kwargs: test_common_kwargs,
)
test('http_client', http_client_test, suite: 'unit', timeout: 30)

# DF-014 + DF-015 — per-call max_body_size + redirect_host_allowlist
# kwargs.  Substrate cases run unconditionally; integration leg
# requires N00B_TEST_DOCKER=1 (uses the zot fixture).
http_per_call_caps_test = executable('test_http_per_call_caps',
    ['test/unit/test_http_per_call_caps.c'],
    kwargs: test_common_kwargs,
)
test('http_per_call_caps', http_per_call_caps_test,
     suite: 'unit',
     timeout: 60,
     env: {
         'N00B_TEST_ZOT_SRC_ROOT': meson.current_source_dir(),
     })

# Phase 6 chunk 5 — live-network smoke for the dispatcher.
# Skipped unless N00B_TEST_NET=1.
http_client_network_test = executable('test_http_client_network',
    ['test/unit/test_http_client_network.c'],
    kwargs: test_common_kwargs,
)
test('http_client_network', http_client_network_test,
     suite: 'unit', timeout: 90)

# Phase 6 chunk 6 — Alt-Svc parser + cache.
http_alt_svc_test = executable('test_http_alt_svc',
    ['test/unit/test_http_alt_svc.c'],
    kwargs: test_common_kwargs,
)
test('http_alt_svc', http_alt_svc_test, suite: 'unit')

# Phase 6 chunk 8 — compression.
http_compression_test = executable('test_http_compression',
    ['test/unit/test_http_compression.c'],
    kwargs: test_common_kwargs,
)
test('http_compression', http_compression_test, suite: 'unit')

# Phase 6 chunk 9 — cookies.
http_cookies_test = executable('test_http_cookies',
    ['test/unit/test_http_cookies.c'],
    kwargs: test_common_kwargs,
)
test('http_cookies', http_cookies_test, suite: 'unit')

# Phase 6 chunk 10 — auth helper.
http_auth_test = executable('test_http_auth',
    ['test/unit/test_http_auth.c'],
    kwargs: test_common_kwargs,
)
test('http_auth', http_auth_test, suite: 'unit')

# Local HTTP/1 service/router.
http_service_test = executable('test_http_service',
    ['test/unit/test_http_service.c'],
    kwargs: test_common_kwargs,
)
test('http_service', http_service_test, suite: 'unit')

http_json_parquet_test = executable('test_http_json_parquet',
    ['test/unit/test_http_json_parquet.c'],
    kwargs: test_common_kwargs,
)
test('http_json_parquet', http_json_parquet_test, suite: 'unit')

# Phase 4 § 4.1 — CBOR encoder + decoder (RFC 8949).  Unit tests
# cover Appendix A vectors + n00b round-trips + decoder hardening +
# a light in-process fuzz smoke.  Dedicated libFuzzer harness lives
# at `test/fuzz/fuzz_quic_cbor.c`; built ad-hoc with Clang's
# `-fsanitize=fuzzer` rather than wired into the standard build.
quic_cbor_test = executable('test_quic_cbor',
    ['test/unit/test_quic_cbor.c'],
    kwargs: test_common_kwargs,
)
test('quic_cbor', quic_cbor_test, suite: 'unit', timeout: 60)

# Phase 4 § 4.2 — QPACK (RFC 9204): encoder, decoder, dynamic table,
# huffman, encoder/decoder stream protocol.  RFC examples + 1000-msg
# stress.  A libFuzzer harness lives at `test/fuzz/fuzz_quic_qpack.c`,
# built ad-hoc.
quic_qpack_test = executable('test_quic_qpack',
    ['test/unit/test_quic_qpack.c'],
    kwargs: test_common_kwargs,
)
test('quic_qpack', quic_qpack_test, suite: 'unit', timeout: 60)

# Phase 4 § 4.8 — RPC cancel/deadline ctx.  Tests local primitive only;
# wire signaling lands in 4.6 RPC unary.
quic_rpc_ctx_test = executable('test_quic_rpc_ctx',
    ['test/unit/test_quic_rpc_ctx.c'],
    kwargs: test_common_kwargs,
)
test('quic_rpc_ctx', quic_rpc_ctx_test, suite: 'unit')

# Phase 4 § 4.10 — RPC status enum: name lookup, HTTP-class
# mapping, QUIC-err → status bridge, gRPC numeric alignment.
quic_rpc_status_test = executable('test_quic_rpc_status',
    ['test/unit/test_quic_rpc_status.c'],
    kwargs: test_common_kwargs,
)
test('quic_rpc_status', quic_rpc_status_test, suite: 'unit')

# Phase 4 § 4.6 — RPC unary call: end-to-end in-process round-trip.
# Spins up an H3 client + server pair on UDP/lo, attaches the RPC
# dispatcher, and exercises register/lookup, success, application
# error, UNIMPLEMENTED, deadline-already-expired, and client-cancel
# mid-flight.
quic_rpc_unary_test = executable('test_quic_rpc_unary',
    ['test/unit/test_quic_rpc_unary.c'],
    kwargs: quic_test_kwargs,
)
test('quic_rpc_unary', quic_rpc_unary_test, suite: 'unit', timeout: 60)

# Phase 4 § 4.7 — Streaming RPC: server-stream / client-stream / bidi
# end-to-end loopback.  Mirrors the unary test scaffold but exercises
# the streaming wire shapes (multiple DATA frames + early-publish on
# the H3 server side + the n00b_rpc_stream_t(T) FIFO).
quic_rpc_streaming_test = executable('test_quic_rpc_streaming',
    ['test/unit/test_quic_rpc_streaming.c'],
    kwargs: quic_test_kwargs,
)
test('quic_rpc_streaming', quic_rpc_streaming_test,
     suite: 'unit', timeout: 90)

# Phase 4 § 4.9 — RPC auth wiring (per-channel + per-call).
# Server-side dispatcher consults manifest auth.policies[] +
# rpc.services[] to gate inbound RPCs; per-call n00b-rpc-policy
# header lets a request opt into a stricter policy (downgrades
# rejected).  Audit emits one event per dispatch.
quic_rpc_auth_test = executable('test_quic_rpc_auth',
    ['test/unit/test_quic_rpc_auth.c',
     'test/fixtures/synthetic_idp.c'],
    include_directories: incdir + [include_directories('test/fixtures')],
    c_args: exe_c_args,
    link_args: exe_link_args,
    link_with: [n00b],
    dependencies: [picoquic_dep],
)
test('quic_rpc_auth', quic_rpc_auth_test, suite: 'unit', timeout: 90)

# Phase 4 § 4.3 — H3 frame encoder + decoder (RFC 9114).
# Frame round-trip + truncated input + oversized + malformed.
quic_h3_frame_test = executable('test_quic_h3_frame',
    ['test/unit/test_quic_h3_frame.c'],
    kwargs: test_common_kwargs,
)
test('quic_h3_frame', quic_h3_frame_test, suite: 'unit')

# Phase 4 § 4.3 — Docker-gated H3 GET / smoke against the caddy
# fixture in `test/fixtures/caddy/`.  Run after:
#   eval "$(bash test/fixtures/caddy/start.sh)"
# Skips cleanly when N00B_TEST_DOCKER is unset.
quic_h3_caddy_smoke_test = executable('test_quic_h3_caddy_smoke',
    ['test/unit/test_quic_h3_caddy_smoke.c'],
    kwargs: quic_test_kwargs,
)
test('quic_h3_caddy_smoke', quic_h3_caddy_smoke_test,
     suite: 'interop', timeout: 60)

# Phase 4 § 4.4 — In-process H3 server loopback (client + server
# endpoint pair on UDP/lo).  Exercises GET, POST, 404, server reset,
# and GOAWAY emission on close.
quic_h3_server_loopback_test = executable('test_quic_h3_server_loopback',
    ['test/unit/test_quic_h3_server_loopback.c'],
    kwargs: quic_test_kwargs,
)
test('quic_h3_server_loopback', quic_h3_server_loopback_test,
     suite: 'unit', timeout: 60)

# WP-004 Phase 5 closeout — h3 mid-stream `max_body_size` enforcement
# regression (DF-014 lift).  Pins the receive-loop guard introduced on
# top of sub-agent 5 (`uq ae`)'s post-await check: a server that sends
# (or declares via `content-length`) a body larger than the caller's
# cap must trigger an `H3_EXCESSIVE_LOAD` stream reset before any
# excess bytes accumulate.  Uses the same in-process loopback fixture
# as `test_quic_h3_server_loopback`.
http_h3_max_body_cap_test = executable('test_http_h3_max_body_cap',
    ['test/unit/test_http_h3_max_body_cap.c'],
    kwargs: quic_test_kwargs,
)
test('http_h3_max_body_cap', http_h3_max_body_cap_test,
     suite: 'unit', timeout: 60)

# Phase 4 § 4.4 — Docker-gated H3 GET / + POST /echo smoke against
# the nginx-quic fixture in `test/fixtures/nginx-quic/`.  Run after:
#   eval "$(bash test/fixtures/nginx-quic/start.sh)"
# Skips cleanly when N00B_TEST_DOCKER is unset.
quic_h3_nginx_smoke_test = executable('test_quic_h3_nginx_smoke',
    ['test/unit/test_quic_h3_nginx_smoke.c'],
    kwargs: quic_test_kwargs,
)
test('quic_h3_nginx_smoke', quic_h3_nginx_smoke_test,
     suite: 'interop', timeout: 60)

# Phase 3 § 7 + § 10.10 — synthetic in-process IdP fixture wired
# end-to-end (OIDC + JWT + DPoP + mTLS-bound + policy + audit).
quic_synthetic_idp_test = executable('test_quic_synthetic_idp',
    ['test/unit/test_quic_synthetic_idp.c',
     'test/fixtures/synthetic_idp.c'],
    include_directories: incdir + [include_directories('test/fixtures')],
    c_args: exe_c_args,
    link_args: exe_link_args,
    link_with: [n00b],
    dependencies: [picoquic_dep],
)
test('quic_synthetic_idp', quic_synthetic_idp_test, suite: 'unit')

# Phase 3.10 manual interop — gated by KEYCLOAK_ISSUER + friends.
# Not run in CI (real-IdP interop is once-per-release per the
# Phase 2 LE-staging precedent).  Build it always so it doesn't
# bit-rot, but the test itself prints [SKIP] when the env isn't set.
quic_keycloak_interop_test = executable('test_quic_keycloak_interop',
    ['test/unit/test_quic_keycloak_interop.c'],
    kwargs: test_common_kwargs,
)
test('quic_keycloak_interop', quic_keycloak_interop_test,
     suite: 'unit', timeout: 60)

# Phase 3.11 — quic_oidc_demo example: manifest → OIDC + policy
# realization → eval against a synthetic-IdP-minted token.
quic_oidc_demo = executable('quic_oidc_demo',
    ['examples/quic_oidc_demo/main.c',
     'test/fixtures/synthetic_idp.c'],
    include_directories: incdir + [include_directories('test/fixtures')],
    c_args: exe_c_args,
    link_args: exe_link_args,
    link_with: [n00b],
    dependencies: [picoquic_dep],
)

# Phase 4 § 4.12 — quic_rpc_demo example: end-to-end H3+CBOR+@rpc demo
# with auth wired through the synthetic IdP.  Doubles as the smoke
# subject for `test_quic_rpc_demo_smoke`.
quic_rpc_demo = executable('quic_rpc_demo',
    ['examples/quic_rpc_demo/main.c',
     'examples/quic_rpc_demo/greet.c',
     'test/fixtures/synthetic_idp.c'],
    include_directories: incdir + [include_directories('test/fixtures')],
    c_args: exe_c_args,
    link_args: exe_link_args,
    link_with: [n00b],
    dependencies: [picoquic_dep],
)

# Phase 5 § 5.3 — quic_phase5_demo: multi-tenant + observability demo.
# Loopback exercises 5 RPCs across 2 synthetic IdPs through the tenant
# resolver, then scrapes /metrics to verify the registry is populated.
quic_phase5_demo = executable('quic_phase5_demo',
    ['examples/quic_phase5_demo/main.c',
     'examples/quic_phase5_demo/services.c',
     'examples/quic_phase5_demo/tenant_resolver.c',
     'test/fixtures/synthetic_idp.c'],
    include_directories: incdir + [include_directories('test/fixtures')],
    c_args: exe_c_args,
    link_args: exe_link_args,
    link_with: [n00b],
    dependencies: [picoquic_dep],
)

quic_phase5_demo_smoke_test = executable('test_quic_phase5_demo_smoke',
    ['test/unit/test_quic_phase5_demo_smoke.c'],
    kwargs: test_common_kwargs,
)
test('quic_phase5_demo_smoke', quic_phase5_demo_smoke_test,
     suite: ['unit', 'long'], timeout: 90,
     env: {'N00B_QUIC_PHASE5_DEMO_BIN': quic_phase5_demo.full_path()},
     depends: [quic_phase5_demo])

# Phase 5 § 5.4 — K8s playbook fixture smoke (kind + n00b deploy).
# Gated on N00B_TEST_KIND=1 + kind/kubectl/docker on PATH.
quic_phase5_k8s_test = executable('test_quic_phase5_k8s_smoke',
    ['test/unit/test_quic_phase5_k8s_smoke.c'],
    kwargs: test_common_kwargs,
)
test('quic_phase5_k8s_smoke', quic_phase5_k8s_test,
     suite: ['integration', 'long'], timeout: 1200,
     env: {'N00B_PHASE5_K8S_SCRIPT':
           meson.project_source_root() / 'test/integration/phase5_k8s/run.sh'})

# Phase 5 § 5.9 — bare-VM playbook fixture smoke (privileged
# container with systemd PID-1 + n00b unit).  Gated on
# N00B_TEST_VM=1 + docker on PATH.
quic_phase5_vm_test = executable('test_quic_phase5_vm_smoke',
    ['test/unit/test_quic_phase5_vm_smoke.c'],
    kwargs: test_common_kwargs,
)
test('quic_phase5_vm_smoke', quic_phase5_vm_test,
     suite: ['integration', 'long'], timeout: 1200,
     env: {'N00B_PHASE5_VM_SCRIPT':
           meson.project_source_root() / 'test/integration/phase5_vm/run.sh'})

# Phase 5 § 5.10 — bare-VM uacme + DNS-01 fixture smoke
# (privileged container with systemd PID-1 + Pebble +
# pebble-challtestsrv + uacme oneshot).  Same gate as 5.9.
quic_phase5_vm_uacme_test = executable('test_quic_phase5_vm_uacme_smoke',
    ['test/unit/test_quic_phase5_vm_uacme_smoke.c'],
    kwargs: test_common_kwargs,
)
test('quic_phase5_vm_uacme_smoke', quic_phase5_vm_uacme_test,
     suite: ['integration', 'long'], timeout: 1200,
     env: {'N00B_PHASE5_VM_UACME_SCRIPT':
           meson.project_source_root() / 'test/integration/phase5_vm/uacme/run.sh'})

# Phase 5 § 5.11 — ZITADEL alternate-IdP fixture smoke (kind +
# Helm + ZITADEL chart).  Gated on N00B_TEST_KIND_ZITADEL=1 +
# kind/kubectl/docker/helm on PATH.
quic_phase5_zitadel_test = executable('test_quic_phase5_zitadel_smoke',
    ['test/unit/test_quic_phase5_zitadel_smoke.c'],
    kwargs: test_common_kwargs,
)
test('quic_phase5_zitadel_smoke', quic_phase5_zitadel_test,
     suite: ['integration', 'long'], timeout: 1500,
     env: {'N00B_PHASE5_ZITADEL_SCRIPT':
           meson.project_source_root() / 'test/integration/phase5_k8s/zitadel/run.sh'})

# Smoke for the demo: forks `quic_rpc_demo --loopback` as a subprocess,
# asserts the canonical Hello + Stream output appears.  The demo binary
# path is passed via the N00B_QUIC_RPC_DEMO_BIN env var (set on the
# test invocation below).
quic_rpc_demo_smoke_test = executable('test_quic_rpc_demo_smoke',
    ['test/unit/test_quic_rpc_demo_smoke.c'],
    kwargs: test_common_kwargs,
)
test('quic_rpc_demo_smoke', quic_rpc_demo_smoke_test,
     suite: ['unit', 'long'], timeout: 90,
     env: {'N00B_QUIC_RPC_DEMO_BIN': quic_rpc_demo.full_path()},
     depends: [quic_rpc_demo])

# ACME end-to-end (Phase 2.3a): directory + newNonce + newAccount.
# Skipped unless STEPCA_DIR_URL is in the env — start the fixture via
# `test/fixtures/stepca/start.sh` to populate it.
quic_acme_session_test = executable('test_quic_acme_session',
    ['test/unit/test_quic_acme_session.c'],
    kwargs: test_common_kwargs,
)
test('quic_acme_session', quic_acme_session_test,
     suite: 'unit', timeout: 30)

# CSR builder.  In-process structural test always runs; external
# `openssl req -verify` runs when an openssl binary is available.
quic_acme_csr_test = executable('test_quic_acme_csr',
    ['test/unit/test_quic_acme_csr.c'],
    kwargs: test_common_kwargs,
)
test('quic_acme_csr', quic_acme_csr_test, suite: 'unit')

# Orchestrator smoke test (argument validation; live cert acquisition
# is deferred to the Phase 2 deployment playbook because HTTP-01
# validation needs port 80).
quic_acme_orchestrator_test = executable('test_quic_acme_orchestrator',
    ['test/unit/test_quic_acme_orchestrator.c'],
    kwargs: test_common_kwargs,
)
test('quic_acme_orchestrator', quic_acme_orchestrator_test, suite: 'unit')

quic_cert_provisioner_test = executable('test_quic_cert_provisioner',
    ['test/unit/test_quic_cert_provisioner.c'],
    kwargs: test_common_kwargs,
)
# Tests look up the fixture via N00B_SOURCE_ROOT; meson exposes this
# automatically via env when set in test_common_kwargs would propagate
# to children.  Easier: pass the project source root directly here.
test('quic_cert_provisioner', quic_cert_provisioner_test,
     suite: 'unit',
     env: {'N00B_SOURCE_ROOT': meson.project_source_root()})

quic_cert_store_test = executable('test_quic_cert_store',
    ['test/unit/test_quic_cert_store.c'],
    kwargs: test_common_kwargs,
)
test('quic_cert_store', quic_cert_store_test, suite: 'unit')

# Endpoint hot-reload (cert + key swap).
quic_endpoint_reload_test = executable('test_quic_endpoint_reload',
    ['test/unit/test_quic_endpoint_reload.c'],
    kwargs: test_common_kwargs,
)
test('quic_endpoint_reload', quic_endpoint_reload_test, suite: 'unit')

# Sticky secret (stateless-reset / address-validation token rotation).
quic_sticky_secret_test = executable('test_quic_sticky_secret',
    ['test/unit/test_quic_sticky_secret.c'],
    kwargs: test_common_kwargs,
)
test('quic_sticky_secret', quic_sticky_secret_test, suite: 'unit')

# LB-CID encoding (block-cipher mode, 16-byte fixed CID).
quic_lb_cid_test = executable('test_quic_lb_cid',
    ['test/unit/test_quic_lb_cid.c'],
    kwargs: test_common_kwargs,
)
test('quic_lb_cid', quic_lb_cid_test, suite: 'unit')

# Phase 2 fuzz tests — property tests across base64url, PEM, X.509
# validity walker, LB-CID decoder.  Same shape as the Phase 1
# framer fuzz: deterministic seed, ~5k iters per target.
quic_phase2_fuzz_test = executable('test_quic_phase2_fuzz',
    ['test/unit/test_quic_phase2_fuzz.c'],
    kwargs: test_common_kwargs,
)
test('quic_phase2_fuzz', quic_phase2_fuzz_test,
     suite: ['unit', 'long'], timeout: 120)

# Manifest + preflight (design doc § 9).
quic_manifest_test = executable('test_quic_manifest',
    ['test/unit/test_quic_manifest.c'],
    kwargs: test_common_kwargs,
)
test('quic_manifest', quic_manifest_test, suite: 'unit')

# DNS providers (manual + cloudflare) + DNS-01 challenge wrapper.
quic_dns_provider_test = executable('test_quic_dns_provider',
    ['test/unit/test_quic_dns_provider.c'],
    kwargs: test_common_kwargs,
)
test('quic_dns_provider', quic_dns_provider_test, suite: 'unit')

# Per-cnx SNI routing (cert store wired into picotls callbacks).
quic_sni_routing_test = executable('test_quic_sni_routing',
    ['test/unit/test_quic_sni_routing.c'],
    kwargs: test_common_kwargs,
)
test('quic_sni_routing', quic_sni_routing_test, suite: 'unit')

# Phase 3 § 5 — trust→picotls verify-cert bridge end-to-end.
quic_trust_bridge_test = executable('test_quic_trust_bridge',
    ['test/unit/test_quic_trust_bridge.c'],
    kwargs: test_common_kwargs,
)
test('quic_trust_bridge', quic_trust_bridge_test, suite: 'unit')

# Phase 3 § 5.3 tail — mid-handshake-reload no-tear property.
quic_reload_race_test = executable('test_quic_reload_race',
    ['test/unit/test_quic_reload_race.c'],
    kwargs: test_common_kwargs,
)
test('quic_reload_race', quic_reload_race_test, suite: 'unit')

# test_quic_acme_http_network removed in Phase 6 chunk 11 — its
# Let's-Encrypt-staging coverage runs through test_quic_acme_session
# (which now goes through n00b_http_request_sync) when the
# STEPCA_LEAF_FP env var is set; the basic h1 round-trip against
# public endpoints is exercised by test_http_h1_network.

quic_endpoint_test = executable('test_quic_endpoint',
    ['test/unit/test_quic_endpoint.c'],
    kwargs: quic_test_kwargs,
)
test('quic_endpoint', quic_endpoint_test, suite: 'unit')

quic_conn_test = executable('test_quic_conn',
    ['test/unit/test_quic_conn.c'],
    kwargs: test_common_kwargs,
)
test('quic_conn', quic_conn_test, suite: 'unit')

quic_chan_test = executable('test_quic_chan',
    ['test/unit/test_quic_chan.c'],
    kwargs: test_common_kwargs,
)
test('quic_chan', quic_chan_test, suite: 'unit')

quic_chan_recv_test = executable('test_quic_chan_recv',
    ['test/unit/test_quic_chan_recv.c'],
    kwargs: test_common_kwargs,
)
test('quic_chan_recv', quic_chan_recv_test, suite: 'unit')

quic_handshake_test = executable('test_quic_handshake',
    ['test/unit/test_quic_handshake.c'],
    kwargs: quic_test_kwargs,
)
test('quic_handshake', quic_handshake_test, suite: 'unit', timeout: 30)

quic_accept_test = executable('test_quic_accept',
    ['test/unit/test_quic_accept.c'],
    kwargs: quic_test_kwargs,
)
test('quic_accept', quic_accept_test, suite: 'unit', timeout: 30)

quic_qlog_test = executable('test_quic_qlog',
    ['test/unit/test_quic_qlog.c'],
    kwargs: quic_test_kwargs,
)
test('quic_qlog', quic_qlog_test, suite: 'unit', timeout: 30)

# Perf harness — built but not registered as a CI test (it's
# benchmarking, not assertion-based).  Run manually with
# `./build_debug/perf_quic_stream_scaling`.
perf_quic_stream_scaling = executable('perf_quic_stream_scaling',
    ['test/perf/perf_quic_stream_scaling.c'],
    kwargs: quic_test_kwargs,
)

# ============================================================================
# Conduit tests
# ============================================================================










if target_os == 'windows'
endif

conduit_udp_test = executable('test_conduit_udp',
    ['test/unit/test_conduit_udp.c'],
    kwargs: test_common_kwargs,
)
test('conduit_udp', conduit_udp_test, suite: 'unit')
















# ============================================================================
# Regex engine tests
# ============================================================================





regex_resharp_test = executable('test_regex_resharp',
    ['test/unit/test_regex_resharp.c'],
    kwargs: test_common_kwargs + {
        'c_args': exe_c_args + ['--ncc-no-gc-stack-maps'],
    },
)
test('regex_resharp', regex_resharp_test,
    suite: ['unit', 'long'],
    timeout: 300,
    env: {
        'N00B_RESHARP_TEST_DIR': meson.current_source_dir() / 'test/data/resharp/tests',
    })

# ============================================================================
# Logic / constraint solver tests
# ============================================================================

logic_test_kwargs = {
    'include_directories': incdir + [include_directories('test/unit')],
    'c_args': exe_c_args,
    'link_args': exe_link_args,
    'link_with': [n00b],
}









# ============================================================================
# ML tests live on the ML feature branch; not built in this QUIC branch.
# ============================================================================

# ============================================================================
# Parser tests
# ============================================================================





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

















# ============================================================================
# LSP framework tests
# ============================================================================

# LSP and nccd_* tests live on a different feature branch (the LSP
# initiative); not built in this QUIC branch.





# ============================================================================
# Type checker tests
# ============================================================================


# ============================================================================
# Analysis tests
# ============================================================================





# ============================================================================
# MIR JIT tests
# ============================================================================



# ============================================================================
# Render / layout tests
# ============================================================================







# Widget tests
















# Table tests





# cocoa_backend + notcurses_backend tests are declared earlier (from main's
# meson.build); the dedup pass missed the duplicate copies that came in with
# the QUIC-branch test section.  Drop the duplicates here.

# Table CLI tool

# Widget visual demo tool

# N00b dev tool (parse + annotate n00b source files, REPL)

# N00b main executable (compile, run, repl)

if target_os != 'windows'
    # VFS journaling depends on POSIX daemon, signal, and frontend behavior.
endif

# nccd (the ncc Language Server) and its lsp/ + nccd_* sources live
# on a different feature branch.  Not built in this QUIC branch.

# ============================================================================
# Integration tests
# ============================================================================

# Interpreter integration tests live in the `interp` suite registered
# earlier in this file (search for `test('interp', ...)`).  The older
# `find_program('bash')` registration here was a duplicate.





# ============================================================================
# VFS tests
# ============================================================================






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
endforeach

# ============================================================================
# Regex engine tests (Phase 11 / resharp-c port)
# ============================================================================

# `fuzz_compare` is a corpus-generator tool (writes regex-crate oracle
# JSON), not a correctness test, and depends on harness symbols
# (bounded thread runner, JSON serde, regex_build_multiline) that are
# not yet on the n00b side.  Skipped until phase 2; the .c/.h files
# remain in place for that work.
regex_test_names = [
    'accel_skip', 'auto_harden', 'deriv', 'engine',
    'parser', 'properties', 'rev_nulls', 'seek', 'smoke_unicode',
    'smoke_utf8seq', 'stream',
]

# Standalone parser tests.
exe = executable('test_parsers_toml',
    'test/unit/test_parsers_toml.c',
    kwargs: test_common_kwargs,
)
test('parsers_toml', exe, suite: 'unit')

# Tests that consume the resharp-c TOML / haystack fixtures need to know
# where those files live.  We point at the resharp-c project root checked
# out in the user's home so we can reuse the existing fixture tree until
# n00b-regex grows its own copies.
regex_engine_manifest_dir = '/Users/viega/resharp-c'
regex_engine_test_c_args  = exe_c_args + [
    '-DRESHARP_ENGINE_MANIFEST_DIR="' + regex_engine_manifest_dir + '"',
]

foreach name : regex_test_names
    if name == 'engine' or name == 'fuzz_compare' \
            or name == 'accel_skip' or name == 'auto_harden' \
            or name == 'deriv' or name == 'rev_nulls'
        exe = executable('test_regex_' + name,
            'test/unit/test_regex_' + name + '.c',
            include_directories: incdir,
            c_args: regex_engine_test_c_args,
            link_args: exe_link_args,
            link_with: [n00b],
        )
    else
        exe = executable('test_regex_' + name,
            'test/unit/test_regex_' + name + '.c',
            kwargs: test_common_kwargs,
        )
    endif
    # regex_engine runs derivative + fuzz comparisons across thousands of
    # patterns and legitimately takes ~60s in debug builds (often longer
    # under host load). The `engine` and `fuzz_compare` cases are tagged
    # `suite: 'long'` so build.sh's default `add_test_no_suite long` (when
    # `N00B_TEST_ALL=0` and no explicit suites/tests requested) excludes
    # them; opt in via `N00B_TEST_ALL=1` or `--all-tests`.
    if name == 'engine' or name == 'fuzz_compare'
        test_suite = ['unit', 'long']
        test_timeout = 300
    else
        test_suite = 'unit'
        test_timeout = 30
    endif
    test('regex_' + name, exe, suite: test_suite, timeout: test_timeout)
endforeach

# aarch64-gated SIMD regex tests
if target_arch == 'aarch64'
    foreach name : ['neon_simd', 'simd_neon_unit']
        # neon_simd consumes the same TOML fixture tree as the engine
        # tests for its lazy_*_toml cases; it needs the manifest dir.
        if name == 'neon_simd'
            exe = executable('test_regex_' + name,
                'test/unit/test_regex_' + name + '.c',
                include_directories: incdir,
                c_args: regex_engine_test_c_args,
                link_args: exe_link_args,
                link_with: [n00b],
            )
        else
            exe = executable('test_regex_' + name,
                'test/unit/test_regex_' + name + '.c',
                kwargs: test_common_kwargs,
            )
        endif
        test('regex_' + name, exe, suite: 'unit')
    endforeach
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
