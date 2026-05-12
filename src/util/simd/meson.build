# SIMD port — Phase 10. Mirrors ~/resharp-c/src/simd/.

n00b_util_simd_src = files(
    'byte_freq.c',
    'mod.c',
)

if target_arch == 'aarch64'
    n00b_util_simd_src += files('neon.c')
endif

# WASM SIMD only when targeting WASM (not detected here; left for future).
# n00b_util_simd_src += files('wasm.c')
