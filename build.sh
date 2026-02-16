N00B_BUILD_TYPE=${N00B_BUILD_TYPE:-debug}
N00B_ROOT=$(realpath ${BASH_SOURCE[0]}/..)
N00B_CLEAN=${N00B_CLEAN:-0}


function ensure_bootstrap {
    if [[ ! -f bin/ncc-bootstrap ]] || [[ ${N00B_CLEAN} -ne 0 ]] ; then
        N00B_BUILD_BOOTSTRAP=1
    else
        N00B_BUILD_BOOTSTRAP=${N00B_BUILD_BOOTSTRAP:-0}
    fi


    if [[ ${N00B_BUILD_BOOTSTRAP} -ne 0 ]] ; then
        cd ${N00B_ROOT}/bootstrap
        if [[ ${N00B_CLEAN} -ne 0 ]] || [[ ! -d build_bootstrap ]] ; then
            if [[ -d build_bootstrap ]] ; then
                echo "Removing old bootstrap dir"
                rm -rf build_bootstrap
            fi
            meson setup --buildtype=${N00B_BUILD_TYPE} -Dcc_path=${CC} --prefix=${N00B_ROOT} --bindir=${N00B_ROOT}/bin build_bootstrap .
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
        cd ${N00B_ROOT}
    fi
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

   if [[ ${N00B_CLEAN} -ne 0 ]] && [[ -d ${build_dir} ]] ; then
       rm -rf ${build_dir}
   fi
   if [[ ! -d ${build_dir} ]] ; then
       CC=${N00B_ROOT}/bin/ncc-bootstrap meson setup --buildtype=${N00B_BUILD_TYPE} $(all_options) ${build_dir} .
       if [[ $? -ne 0 ]] ; then
           echo "Build setup failed."
           exit 1
       fi
   fi

   meson compile -C ${build_dir}
}

ensure_bootstrap
build_n00b $*
