N00B_BUILD_TYPE=${N00B_BUILD_TYPE:-debug}
N00B_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
N00B_CLEAN=${N00B_CLEAN:-0}
N00B_TEST=${N00B_TEST:-0}
N00B_DOCS=${N00B_DOCS:-0}
N00B_CROSS=${N00B_CROSS:-}
N00B_JOBS=${N00B_JOBS:-}
N00B_NATIVE=${N00B_NATIVE:-0}

# Default to a C23-capable compiler if CC is not set.
if [[ -z "${CC}" ]] && [[ -x /usr/local/bin/clang ]] ; then
    export CC=/usr/local/bin/clang
fi

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

function ensure_ncc_subproject {
    local ncc_src="${N00B_ROOT}/subprojects/ncc"

    if [[ -f "${ncc_src}/meson.build" ]] ; then
        return 0
    fi

    echo "=== Cloning ncc into subprojects/ncc ==="
    git clone https://github.com/crashappsec/ncc.git "${ncc_src}"

    if [[ -n "${NCC_REV:-}" ]] ; then
        git -C "${ncc_src}" checkout "${NCC_REV}"
    fi
}

function build_ncc {
    local ncc_src="${N00B_ROOT}/subprojects/ncc"
    local ncc_build="${ncc_src}/build_release"
    local ncc_bin="${ncc_build}/ncc"

    if [[ -x "${ncc_bin}" ]] && [[ "${N00B_BUILD_BOOTSTRAP:-0}" -eq 0 ]] ; then
        echo "=== Using cached ncc build ==="
        export NCC_PATH="${ncc_bin}"
        return 0
    fi

    echo "=== Building ncc from subprojects/ncc ==="
    local ncc_cc="${CC:-clang}"
    # Avoid circular dependency: if CC is already ncc, fall back to clang.
    if [[ "$(basename "${ncc_cc}")" == "ncc" ]] ; then
        ncc_cc="clang"
    fi

    if [[ "${N00B_BUILD_BOOTSTRAP:-0}" -ne 0 ]] && [[ -d "${ncc_build}" ]] ; then
        rm -rf "${ncc_build}"
    fi

    if [[ ! -d "${ncc_build}" ]] ; then
        CC="${ncc_cc}" meson setup --buildtype=release "${ncc_build}" "${ncc_src}"
    fi

    meson compile -C "${ncc_build}"
    export NCC_PATH="${ncc_bin}"
}

function ensure_ncc {
    # 1. Explicit override via NCC_PATH env var.
    if [[ -n "${NCC_PATH:-}" ]] && [[ -x "${NCC_PATH}" ]] ; then
        export NCC_PATH
        return 0
    fi

    # 2. System ncc in PATH.
    local system_ncc
    system_ncc=$(which ncc 2>/dev/null || true)
    if [[ -n "${system_ncc}" ]] ; then
        export NCC_PATH="${system_ncc}"
        return 0
    fi

    # 3. Build from subproject.
    ensure_ncc_subproject
    build_ncc
}

function all_options {
    local s="-Dusing_build_script=true"

    if [[ ${N00B_BUILD_DEBUG:-0} -ne 0 ]] ; then
        s=${s} -Denable_debug=true
    fi

    if [[ ${N00B_BUILD_DEV:-0} -ne 0 ]] ; then
        s=${s} -Ddev_mode=true
    fi

    if [[ ${N00B_BUILD_LTO:-0} -ne 0 ]] ; then
        s=${s} -Denable_lto=true
    fi

    if [[ ${N00B_BUILD_GC_STATS:-0} -ne 0 ]] ; then
        s=${s} -Dshow_gc_stats=true
    fi

    if [[ ${N00B_BUILD_MEMCHECK:-0} -ne 0 ]] ; then
        s=${s} -Duse_memcheck=true
    fi

    if [[ ${N00B_BUILD_ASAN:-0} -ne 0 ]] ; then
        s=${s} -Duse_asan
    fi

    if [[ ${N00B_BUILD_UBSAN:-0} -ne 0 ]] ; then
        s=${s} -Duse_ubsan
    fi

    if [[ ${N00B_BUILD_MUSL:-0} -ne 0 ]] ; then
        s=${s} -Dusing_musl=true
    fi

    echo $s
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
       CC=${NCC_PATH} meson setup --buildtype=${N00B_BUILD_TYPE} $(all_options) ${build_dir} .
       if [[ $? -ne 0 ]] ; then
           echo "Build setup failed."
           exit 1
       fi
   fi

   meson compile -C ${build_dir} ${jobs_flag}

   if [[ ${N00B_TEST} -ne 0 ]] ; then
       local test_jobs_flag=""
       if [[ -n "${N00B_JOBS}" ]] ; then
           test_jobs_flag="--num-processes ${N00B_JOBS}"
       fi
       meson test -C ${build_dir} --print-errorlogs --timeout-multiplier 3 ${test_jobs_flag}
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
        CC=${NCC_PATH} \
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
