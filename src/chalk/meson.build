# Sources for the n00b chalk codec module: artifact insertion,
# deletion, extraction, and hashing for ELF / Mach-O / GGUF /
# SafeTensors / ZIP / Python .pyc / source / sidecar / certs.

n00b_chalk_src = files(
    'base32v.c',
    'normalize.c',
    'mark.c',
    'codec_table.c',
    'file_io.c',
    'pyc.c',
    'sidecar.c',
    'certs.c',
    'macos_wrap.c',
    'gguf_core.c',
    'gguf.c',
    'safetensors_core.c',
    'safetensors.c',
    'macho_core.c',
    'macho.c',
    'lang_table.c',
    'source.c',
    'elf.c',
    'elf_fallback.c',
    'zip.c',
    'pe.c',
    'resign_pe.c',
    'resign_identity.c',
    'resign_macho.c',
)

# macOS-only Objective-C bridge for the Mach-O re-sign body.
# Same pattern as src/net/quic/{acme_trust_macos,secret_keychain}.m:
# compile the .m through the system ObjC compiler, link as a
# static lib, fold into the top-level link_whole_libs after it
# exists.
n00b_chalk_link_whole_libs = []
if target_os == 'darwin'
    chalk_m_bridge_args = [
        '-g',
        '-Wno-unused-parameter',
        '-Wno-unused-function',
        '-Wno-unknown-pragmas',
        '-fno-omit-frame-pointer',
        '-fPIC',
        '-Wno-atomic-alignment',
        '-DN00B_HOST_OS=' + host_machine.system(),
        '-DN00B_HOST_CPU=' + host_machine.cpu_family(),
    ]
    n00b_chalk_link_whole_libs += static_library('n00b_chalk_resign_macho_darwin',
        ['resign_macho_darwin.m'],
        include_directories: incdir,
        dependencies: deps,
        objc_args: chalk_m_bridge_args,
        link_args: link_args,
    )
endif
