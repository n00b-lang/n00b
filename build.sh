#!/usr/bin/env bash

N00B_BUILD_TYPE=${N00B_BUILD_TYPE:-debug}
N00B_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
CC=${CC:-clang}
N00B_CLEAN=${N00B_CLEAN:-0}
N00B_TEST=${N00B_TEST:-0}
N00B_DOCS=${N00B_DOCS:-0}
N00B_MESON_NATIVE_FILE=${N00B_MESON_NATIVE_FILE:-}
N00B_MESON_CROSS_FILE=${N00B_MESON_CROSS_FILE:-}
N00B_NCC_COMPILER=${N00B_NCC_COMPILER:-}
N00B_TOOLCHAIN_TARGET=${N00B_TOOLCHAIN_TARGET:-}
N00B_BUILD_DIR=${N00B_BUILD_DIR:-}

declare -a N00B_BOOTSTRAP_MESON_FILE_ARGS=()
declare -a N00B_MAIN_MESON_FILE_ARGS=()

if [[ -n "${N00B_MESON_NATIVE_FILE}" ]] ; then
    N00B_BOOTSTRAP_MESON_FILE_ARGS+=(--native-file "${N00B_MESON_NATIVE_FILE}")
    N00B_MAIN_MESON_FILE_ARGS+=(--native-file "${N00B_MESON_NATIVE_FILE}")
fi

if [[ -n "${N00B_MESON_CROSS_FILE}" ]] ; then
    N00B_MAIN_MESON_FILE_ARGS+=(--cross-file "${N00B_MESON_CROSS_FILE}")
fi


function ensure_bootstrap {
    if [[ ! -f bin/ncc-bootstrap ]] || [[ ${N00B_CLEAN} -ne 0 ]] ; then
        N00B_BUILD_BOOTSTRAP=1
    else
        N00B_BUILD_BOOTSTRAP=${N00B_BUILD_BOOTSTRAP:-0}
    fi


    if [[ ${N00B_BUILD_BOOTSTRAP} -ne 0 ]] ; then
        cd "${N00B_ROOT}/bootstrap"
        if [[ ${N00B_CLEAN} -ne 0 ]] || [[ ! -d build_bootstrap ]] ; then
            if [[ -d build_bootstrap ]] ; then
                echo "Removing old bootstrap dir"
                rm -rf build_bootstrap
            fi

            local -a bootstrap_setup_cmd=(
                meson
                setup
                "--buildtype=${N00B_BUILD_TYPE}"
                "-Dcc_path=${CC}"
                "--prefix=${N00B_ROOT}"
                "--bindir=${N00B_ROOT}/bin"
            )
            bootstrap_setup_cmd+=("${N00B_BOOTSTRAP_MESON_FILE_ARGS[@]}")
            bootstrap_setup_cmd+=(build_bootstrap .)

            "${bootstrap_setup_cmd[@]}"
            if [[ $? -ne 0 ]] ; then
                echo "Bootstrap setup failed."
                exit 1
            fi
        fi

        meson compile -C build_bootstrap
        if [[ $? -ne 0 ]] ; then
            echo "Could not build bootstrap."
            exit 1
        fi
        meson install -C build_bootstrap
        if [[ $? -ne 0 ]] ; then
            echo "Bootstrap install failed."
            exit 1
        fi
        cd "${N00B_ROOT}"
    fi
}

function all_options {
    local -a opts=("-Dusing_build_script=true")

    if [[ ${N00B_BUILD_DEBUG:-0} -ne 0 ]] ; then
        opts+=("-Denable_debug=true")
    fi

    if [[ ${N00B_BUILD_DEV:-0} -ne 0 ]] ; then
        opts+=("-Ddev_mode=true")
    fi

    if [[ ${N00B_BUILD_LTO:-0} -ne 0 ]] ; then
        opts+=("-Denable_lto=true")
    fi

    if [[ ${N00B_BUILD_GC_STATS:-0} -ne 0 ]] ; then
        opts+=("-Dshow_gc_stats=true")
    fi

    if [[ ${N00B_BUILD_MEMCHECK:-0} -ne 0 ]] ; then
        opts+=("-Duse_memcheck=true")
    fi

    if [[ ${N00B_BUILD_ASAN:-0} -ne 0 ]] ; then
        opts+=("-Duse_asan")
    fi

    if [[ ${N00B_BUILD_UBSAN:-0} -ne 0 ]] ; then
        opts+=("-Duse_ubsan")
    fi

    if [[ ${N00B_BUILD_MUSL:-0} -ne 0 ]] ; then
        opts+=("-Dusing_musl=true")
    fi

    if [[ -n "${N00B_TOOLCHAIN_TARGET}" ]] ; then
        opts+=("-Dtoolchain_target=${N00B_TOOLCHAIN_TARGET}")
    fi

    printf '%s\n' "${opts[@]}"
}

function build_n00b {
   local build_dir="${1:-${N00B_BUILD_DIR:-build_${N00B_BUILD_TYPE}}}"
   local -a setup_opts
   mapfile -t setup_opts < <(all_options)

   if [[ ${N00B_CLEAN} -ne 0 ]] && [[ -d "${build_dir}" ]] ; then
       rm -rf "${build_dir}"
   fi

   if [[ ! -d "${build_dir}" ]] ; then
       local -a setup_cmd=(
           meson
           setup
           "--buildtype=${N00B_BUILD_TYPE}"
       )
       setup_cmd+=("${setup_opts[@]}")
       setup_cmd+=("${N00B_MAIN_MESON_FILE_ARGS[@]}")
       setup_cmd+=("${build_dir}" .)

       if [[ -n "${N00B_NCC_COMPILER}" ]] ; then
           CC="${N00B_ROOT}/bin/ncc-bootstrap" NCC_COMPILER="${N00B_NCC_COMPILER}" "${setup_cmd[@]}"
       else
           CC="${N00B_ROOT}/bin/ncc-bootstrap" "${setup_cmd[@]}"
       fi
       if [[ $? -ne 0 ]] ; then
           echo "Build setup failed."
           exit 1
       fi
   fi

   if [[ -n "${N00B_NCC_COMPILER}" ]] ; then
       NCC_COMPILER="${N00B_NCC_COMPILER}" meson compile -C "${build_dir}"
   else
       meson compile -C "${build_dir}"
   fi

   if [[ ${N00B_TEST} -ne 0 ]] ; then
       if [[ -n "${N00B_NCC_COMPILER}" ]] ; then
           NCC_COMPILER="${N00B_NCC_COMPILER}" meson test -C "${build_dir}" --print-errorlogs
       else
           meson test -C "${build_dir}" --print-errorlogs
       fi
       if [[ $? -ne 0 ]] ; then
           echo "Tests failed."
           exit 1
       fi
   fi

   if [[ ${N00B_DOCS} -ne 0 ]] ; then
       if [[ -n "${N00B_NCC_COMPILER}" ]] ; then
           NCC_COMPILER="${N00B_NCC_COMPILER}" meson compile -C "${build_dir}" docs
       else
           meson compile -C "${build_dir}" docs
       fi
       if [[ $? -ne 0 ]] ; then
           echo "Documentation generation failed."
           exit 1
       fi
       echo "Documentation generated in ${build_dir}/docs/html/"
   fi
}

ensure_bootstrap
build_n00b "$@"
