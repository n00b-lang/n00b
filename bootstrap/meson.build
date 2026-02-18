project('ncc_bootstrap', 'c',
    version: '0.1.0',
    meson_version: '>= 1.1',
    default_options: [
        'warning_level=2',
        'buildtype=debug',
        'c_std=none',
    ]
)

# include/ is two levels up from src/bootstrap/
inc_base = include_directories('./include')

# Generate sorted NT_LIST macro from parse.c declarations
# Shared sources are now merged into this directory
gen_nt_list = custom_target('gen_nt_list',
  input: ['src/parse.c', 'src/parse_support.c'],
  output: 'nt_list.inc',
  command: [find_program('bin/gen_nt_list.sh'), '@INPUT@'],
  capture: true,
)

bootstrap_files = [
  'src/ncc.c',
  'src/argv_parse.c',
  'src/buf.c',
  'src/compile_common.c',
  'src/lex.c',
  'src/output.c',
  'src/sha256.c',
  'src/dict.c',
  'src/st.c',
  'src/utils.c',
  'src/modernize.c',
  'src/mod_token_xforms.c',
  'src/mod_tree_xforms.c',
  'src/emit.c',
  'src/parse.c',
  'src/parse_support.c',
  'src/st_register.c',
  'src/rewrite.c',
  'src/type_normalize.c',
  'src/prefix_xform.c',
  'src/compile_packrat.c',
  'src/transform.c',
  'src/xforms.c',
  'src/xform_typeid.c',
  'src/xform_typestr.c',
  'src/xform_once.c',
  'src/xform_keyword.c',
  'src/xform_kw_call.c',
  'src/xform_bang.c',
  'src/xform_flatten.c',
  'src/xform_vargs.c',
  'src/xform_package.c',
  'src/xform_constexpr.c',
  'src/xform_constexpr_paste.c',
]

# Clang source-based code coverage
coverage_enabled = get_option('coverage')
coverage_args = []
coverage_link_args = []
if coverage_enabled
  if cc.get_id() != 'clang'
    error('Coverage requires Clang compiler')
  endif
  coverage_args = ['-fprofile-instr-generate', '-fcoverage-mapping']
  coverage_link_args = ['-fprofile-instr-generate']
endif


# Default prefix for identifiers in the source library.

extensions = get_option('extensions')
x_flag = '-DNCC_EXTENSIONS='
foreach item : extensions
  x_flag = x_flag + '.' + item
endforeach

fs = import('fs')

cc = meson.get_compiler('c')
default_compiler_path = cc.cmd_array()[0]

if not cc.compiles('int main(void) { return 0; }', args: ['-std=c23'], name: 'C23 support check')
  error('Compiler does not support -std=c23. Use clang 22+ (/usr/local/bin/clang).')
endif


c_args = [
  '-std=c23',
  '-DNCC_DEFAULT_CC="' + default_compiler_path + '"',
  '-DNCC_INTERNAL',  # Enable internal aliases for library types
  x_flag
]

if (host_machine.cpu_family() == 'x86_64' and cc.get_id() == 'clang')
  c_args = c_args + ['-Wno-atomic-alignment']
endif

ncc_exe = executable(
  'ncc-bootstrap',
  bootstrap_files,
  gen_nt_list,
  c_args: c_args,
  include_directories: [inc_base],
  dependencies: [],
  install : true,
)
