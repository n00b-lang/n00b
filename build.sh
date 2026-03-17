N00B_BUILD_TYPE=${N00B_BUILD_TYPE:-debug}
N00B_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
N00B_CLEAN=${N00B_CLEAN:-0}
N00B_TEST=${N00B_TEST:-0}
N00B_DOCS=${N00B_DOCS:-0}
N00B_CROSS=${N00B_CROSS:-}
N00B_JOBS=${N00B_JOBS:-}
N00B_NATIVE=${N00B_NATIVE:-0}
N00B_UNICODE_ALLOW_DOWNLOADS=${N00B_UNICODE_ALLOW_DOWNLOADS:-0}
N00B_UNICODE_STRICT_CACHE=${N00B_UNICODE_STRICT_CACHE:-0}

function select_bootstrap_compiler {
    if [[ -n "${CC:-}" ]] ; then
        export CC
        return
    fi

    if command -v clang &>/dev/null ; then
        export CC=$(command -v clang)
        return
    fi

    if command -v cc &>/dev/null && cc --version 2>/dev/null | head -n 1 | grep -qi 'clang' ; then
        export CC=$(command -v cc)
        echo "Using cc because it resolves to a clang-compatible compiler."
        return
    fi

    echo "No clang-compatible bootstrap compiler found." >&2
    echo "Install clang or rerun with CC=/path/to/clang-compatible-compiler." >&2
    exit 1
}

select_bootstrap_compiler

# Ensure the macOS SDK root is set so the linker can find libSystem.
if [[ "$(uname -s)" == "Darwin" ]] && [[ -z "${SDKROOT}" ]] && command -v xcrun &>/dev/null; then
    export SDKROOT=$(xcrun --show-sdk-path 2>/dev/null)
fi

# ── Docker cross-compilation on macOS ────────────────────────────────────────
# On macOS, if Docker is available with an osxcross-enabled image, delegate
# to docker/cross-build.sh for cross-compilation. Set N00B_NATIVE=1 to
# skip this and build natively.
if [[ "$(uname -s)" == "Darwin" ]] && \
   [[ "${N00B_NATIVE}" == "0" ]] && \
   command -v docker &>/dev/null && \
   docker info &>/dev/null 2>&1 && \
   docker image inspect n00b-linux &>/dev/null 2>&1; then
    if docker run --rm n00b-linux test -d /usr/local/osxcross/bin 2>/dev/null; then
        echo "=== Docker cross-compilation (N00B_NATIVE=1 to override) ==="
        exec bash "${N00B_ROOT}/docker/cross-build.sh" macos-arm64
    fi
fi

function ensure_ncc {
    if [[ ! -f bin/ncc ]] || [[ ${N00B_CLEAN} -ne 0 ]] ; then
        N00B_BUILD_NCC=1
    else
        N00B_BUILD_NCC=${N00B_BUILD_NCC:-0}
    fi


    if [[ ${N00B_BUILD_NCC} -ne 0 ]] ; then
        cd ${N00B_ROOT}/ncc
        if [[ ${N00B_CLEAN} -ne 0 ]] || [[ ! -d build_ncc ]] ; then
            if [[ -d build_ncc ]] ; then
                echo "Removing old ncc build dir"
                rm -rf build_ncc
            fi
            # n00b-specific rstr templates: wrap strings in n00b_static_header_t
            # so the GC can identify r"" strings as managed objects.
            # Styled slots: $0=style_decls $1=var $2=bytes $3=data $4=cp $5=styling $6=typehash $7=wrapper
            # Plain slots:  $0=var $1=bytes $2=data $3=cp $4=typehash $5=wrapper
            meson setup --buildtype=${N00B_BUILD_TYPE} -Dcc_path=${CC} --prefix=${N00B_ROOT} --bindir=${N00B_ROOT}/bin \
                '-Drstr_template_styled=({$0 static struct{n00b_static_header_t hdr;n00b_string_t obj;} $7={.hdr={.static_magic=0xcc653162303034ccULL,.tinfo=$6,.alloc_len=(unsigned)(sizeof(n00b_inline_hdr_t)+sizeof(n00b_string_t))},.obj={.u8_bytes=$2,.data=$3,.codepoints=$4,.styling=$5}};&$7.obj;})' \
                '-Drstr_template_plain=({static struct{n00b_static_header_t hdr;n00b_string_t obj;} $5={.hdr={.static_magic=0xcc653162303034ccULL,.tinfo=$4,.alloc_len=(unsigned)(sizeof(n00b_inline_hdr_t)+sizeof(n00b_string_t))},.obj={.u8_bytes=$1,.data=$2,.codepoints=$3,.styling=((void*)0)}};&$5.obj;})' \
                '-Dvargs_type=n00b_vargs_t' \
                '-Donce_prefix=__n00b_' \
                '-Drstr_string_type=n00b_string_t*' \
                build_ncc .
            if [[ $? -ne 0 ]] ; then
                echo "NCC setup failed."
                exit 1
            fi
        fi

        meson compile -C build_ncc
        if [[ $? -ne 0 ]] ; then
            echo "Could not build ncc."
            exit 1
        fi
        meson install -C build_ncc
        if [[ $? -ne 0 ]] ; then
            echo "NCC install failed."
            exit 1
        fi
        cd ${N00B_ROOT}
    fi
}

function all_options {
    local s="-Dusing_build_script=true"

    if [[ ${N00B_BUILD_DEBUG:-0} -ne 0 ]] ; then
        s="${s} -Denable_debug=true"
    fi

    if [[ ${N00B_BUILD_DEV:-0} -ne 0 ]] ; then
        s="${s} -Ddev_mode=true"
    fi

    if [[ ${N00B_BUILD_LTO:-0} -ne 0 ]] ; then
        s="${s} -Denable_lto=true"
    fi

    if [[ ${N00B_BUILD_GC_STATS:-0} -ne 0 ]] ; then
        s="${s} -Dshow_gc_stats=enabled"
    fi

    if [[ ${N00B_BUILD_MEMCHECK:-0} -ne 0 ]] ; then
        s="${s} -Duse_memcheck=on"
    fi

    if [[ ${N00B_BUILD_ASAN:-0} -ne 0 ]] ; then
        s="${s} -Duse_asan=enabled"
    fi

    if [[ ${N00B_BUILD_UBSAN:-0} -ne 0 ]] ; then
        s="${s} -Duse_ubsan=enabled"
    fi

    if [[ ${N00B_BUILD_MUSL:-0} -ne 0 ]] ; then
        s="${s} -Dusing_musl=true"
    fi

    if [[ ${N00B_UNICODE_ALLOW_DOWNLOADS} -ne 0 ]] ; then
        s="${s} -Dunicode_allow_downloads=true"
    fi

    if [[ ${N00B_UNICODE_STRICT_CACHE} -ne 0 ]] ; then
        s="${s} -Dunicode_strict_cache=true"
    fi

    echo "${s}"
}

function build_n00b {
   local build_dir=${1:-build_${N00B_BUILD_TYPE}}
   local jobs_flag=""
   if [[ -n "${N00B_JOBS}" ]] ; then
       jobs_flag="-j ${N00B_JOBS}"
   fi

   if [[ ${N00B_CLEAN} -ne 0 ]] && [[ -d ${build_dir} ]] ; then
       rm -rf ${build_dir}
   fi
   if [[ ! -d ${build_dir} ]] ; then
       # OBJC must point to Apple's clang (with sysroot) for the Cocoa backend;
       # the LLVM clang at /usr/local/bin lacks macOS SDK paths.
       if [[ "$(uname -s)" == "Darwin" ]] && command -v xcrun &>/dev/null; then
           export OBJC=$(xcrun --find clang 2>/dev/null)
       fi
       CC=${N00B_ROOT}/bin/ncc meson setup --buildtype=${N00B_BUILD_TYPE} $(all_options) ${build_dir} .
       if [[ $? -ne 0 ]] ; then
           echo "Build setup failed."
           exit 1
       fi
   fi

   meson compile -C ${build_dir} ${jobs_flag}
   if [[ $? -ne 0 ]] ; then
       echo "Build compile failed."
       exit 1
   fi

   if [[ ${N00B_TEST} -ne 0 ]] ; then
       meson test -C ${build_dir} --print-errorlogs
       if [[ $? -ne 0 ]] ; then
           echo "Tests failed."
           exit 1
       fi
   fi

   if [[ ${N00B_DOCS} -ne 0 ]] ; then
       meson compile -C ${build_dir} ${jobs_flag} docs
       if [[ $? -ne 0 ]] ; then
           echo "Documentation generation failed."
           exit 1
       fi
       echo "Documentation generated in ${build_dir}/docs/html/"
   fi
}

# ============================================================================
# Cross-compilation support
#
# N00B_CROSS=all              — cross-compile for all targets with available toolchains
# N00B_CROSS=linux-x86_64     — cross-compile for one specific target
# ============================================================================

function cross_compile_target {
    local cross_file=$1
    local target_name=$(basename ${cross_file} .cross)
    local build_dir="build_cross_${target_name}"

    # Extract the C compiler path from the cross file.
    local cross_cc=$(python3 -c "
import configparser, pathlib, os
p = pathlib.Path('${cross_file}')
cp = configparser.ConfigParser()
cp.read(p)
tc = cp.get('constants', 'toolchain', fallback='/usr').strip().strip(\"'\")
c_val = cp.get('binaries', 'c', fallback='').strip().strip(\"'\")
# Handle 'toolchain / ...' path expressions
c_val = c_val.replace('toolchain / ', tc + '/')
print(c_val)
" 2>/dev/null)

    if [[ -z "${cross_cc}" ]] || [[ ! -x "${cross_cc}" ]] ; then
        echo "  [SKIP] ${target_name} — cross-compiler not found: ${cross_cc:-<empty>}"
        return 0
    fi

    echo "  [BUILD] ${target_name} (${cross_cc})"

    if [[ ${N00B_CLEAN} -ne 0 ]] && [[ -d ${build_dir} ]] ; then
        rm -rf ${build_dir}
    fi

    if [[ ! -d ${build_dir} ]] ; then
        CC=${N00B_ROOT}/bin/ncc \
        meson setup --cross-file ${cross_file} \
            --buildtype=${N00B_BUILD_TYPE} $(all_options) ${build_dir} .
        if [[ $? -ne 0 ]] ; then
            echo "  [FAIL] ${target_name} — meson setup failed"
            return 1
        fi
    fi

    meson compile -C ${build_dir}
    if [[ $? -ne 0 ]] ; then
        echo "  [FAIL] ${target_name} — compile failed"
        return 1
    fi

    echo "  [OK]   ${target_name}"
    return 0
}

function cross_compile {
    echo "Cross-compiling..."
    local failed=0

    if [[ "${N00B_CROSS}" == "all" ]] ; then
        for cf in ${N00B_ROOT}/cross/*.cross ; do
            cross_compile_target "${cf}" || failed=1
        done
    else
        local cf="${N00B_ROOT}/cross/${N00B_CROSS}.cross"
        if [[ ! -f "${cf}" ]] ; then
            echo "Cross file not found: ${cf}"
            echo "Available targets:"
            ls ${N00B_ROOT}/cross/*.cross 2>/dev/null | while read f; do
                echo "  $(basename $f .cross)"
            done
            exit 1
        fi
        cross_compile_target "${cf}" || failed=1
    fi

    if [[ ${failed} -ne 0 ]] ; then
        echo "Some cross-compilation targets failed."
        exit 1
    fi
}

ensure_ncc
build_n00b $*

if [[ -n "${N00B_CROSS}" ]] ; then
    cross_compile
fi
