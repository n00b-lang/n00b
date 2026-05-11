project('mir', 'c',
  version: '0.0.0',
  default_options: ['warning_level=0', 'werror=false'],
)

# n00b's CC is `ncc` (a clang wrapper that performs source-to-source
# transforms). ncc's tokenizer cannot parse the libgcc/clang intrinsic
# headers MIR pulls in transitively on some platforms — notably the
# `__extension__ ((__m64)(__v2si){…})` compound-literal vector casts in
# mmintrin.h when targeting x86_64-w64-windows-gnu. Build MIR with the
# raw clang driver directly, regardless of host, so the project's CC
# does not need to be involved.

llvm_mingw = run_command(
  'sh', '-c', 'printf %s "${LLVM_MINGW:-}"', check: true,
).stdout().strip()

if llvm_mingw != ''
  # Cross-build to Windows (or any time the user has explicitly pointed
  # at an llvm-mingw root): use that clang directly.
  c_cmd = llvm_mingw / 'bin' / 'x86_64-w64-mingw32-clang'
else
  # Native build: use whichever clang is on PATH.
  c_cmd = find_program('clang', required: true).full_path()
endif

mir_c_args = ['-c', '-O2', '-DMIR_PARALLEL_GEN=0', '-w']
if host_machine.system() != 'windows'
  mir_c_args += '-fPIC'
endif

mir_o = custom_target('mir_o',
  input: 'mir.c',
  output: 'mir.c.o',
  command: [c_cmd, mir_c_args, '-o', '@OUTPUT@', '@INPUT@'],
)

mir_gen_o = custom_target('mir_gen_o',
  input: 'mir-gen.c',
  output: 'mir-gen.c.o',
  command: [c_cmd, mir_c_args, '-o', '@OUTPUT@', '@INPUT@'],
)

mir_lib = static_library('mir', objects: [mir_o, mir_gen_o])

mir_dep = declare_dependency(
  link_with: mir_lib,
  include_directories: include_directories('.'),
)
