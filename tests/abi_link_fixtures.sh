#!/bin/sh
set -eu

# Some fixtures run from another directory, so a relative path to a tool
# is made absolute here. (A bare name is left to be found on PATH.)
absolute() {
    case $1 in
    /*) printf '%s' "$1" ;;
    */*) printf '%s/%s' "$(pwd)" "$1" ;;
    *) printf '%s' "$1" ;;
    esac
}

compiler=$(absolute "$1")
abi_record_object_test=$(absolute "$2")
abi_link_input_test=$(absolute "$3")
abi_register_object_test=$(absolute "$4")
script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
abi_programs="$script_dir/programs/abi"
records="$script_dir/programs/link_records"
interface_programs="$script_dir/programs/interfaces"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

# Builds INPUT... as one library with N00B_PATH set to PATH_DIR, writing every
# module's own ABI object as DIR/MODULE.n00bm. One build gives the objects of
# all its roots and everything they import, so a group of fixtures costs one
# compile.
library_dump() {
    path_dir=$1
    dir=$2
    shift 2
    mkdir -p "$dir"
    if ! N00B_PATH="$path_dir" "$compiler" library --dev-module-abi "$dir" \
        --output "$dir/linked.n00bm" "$@" >"$dir.err" 2>&1; then
        cat "$dir.err" >&2
        exit 1
    fi
}

# Every `library` run is a full compile, so the checks of a build's linked
# library use a build made for its objects where one has the right roots.
# Such a build keeps the root a check is about first, as it would be on its
# own (the first root's object is not the same as another root's), and its
# other roots only add modules the check doesn't look at.
library_dump "$abi_programs" "$tmp/abi" import_root c_foreign \
    abi_value_root foreign_enum_user cycles.cycle_first
# These builds' linked libraries are checked below as well.
library_dump "$abi_programs" "$tmp/cycle_long" cycles.cycle_long_a
library_dump "$abi_programs" "$tmp/cycle_self" cycles.cycle_self
# A module's object depends only on the module, not on what else the build
# has in it (the cache reuses it in other programs). This build's linked
# library is enum_export's, checked below.
library_dump "$abi_programs" "$tmp/alone" enum_export.__init \
    cycles.cycle_first
cmp "$tmp/abi/cycles.cycle_first.n00bm" \
    "$tmp/alone/cycles.cycle_first.n00bm" || {
    echo "cycles.cycle_first's ABI object differs with other roots" >&2
    exit 1
}
# A root in a cycle is built from source even when the cache holds its
# cycle, a library's roots as a program's one: built again with the cycle
# cached, every object is the same.
library_dump "$abi_programs" "$tmp/enum_cycle" enum_export.__init \
    cycles.cycle_first
library_dump "$abi_programs" "$tmp/enum_cycle_again" enum_export.__init \
    cycles.cycle_first
for object in "$tmp/enum_cycle"/*.n00bm; do
    cmp "$object" "$tmp/enum_cycle_again/${object##*/}" || {
        echo "${object##*/} differs when its library is built again" >&2
        exit 1
    }
done
cmp "$tmp/alone/cycles.cycle_first.n00bm" \
    "$tmp/enum_cycle/cycles.cycle_first.n00bm" || {
    echo "cycles.cycle_first's ABI object differs beside enum_export" >&2
    exit 1
}
# Nor on where it sits among the roots: the first root is the first module
# to define the types the compiler gives every module.
library_dump "$abi_programs" "$tmp/first" cycles.cycle_first empty
library_dump "$abi_programs" "$tmp/empty_first" empty
for pair in alone:cycles.cycle_first empty_first:empty; do
    module=${pair#*:}
    cmp "$tmp/${pair%%:*}/$module.n00bm" "$tmp/first/$module.n00bm" || {
        echo "$module's ABI object differs as the first root" >&2
        exit 1
    }
done
# The linked library is record_root's, checked below. record_user is a
# second module that uses record_dep's record without declaring it.
library_dump "$records" "$tmp/records" record_root shared_link_first \
    record_user
library_dump "$interface_programs" "$tmp/interfaces" \
    "$abi_programs/foreign_flagset_user.n" \
    "$interface_programs/operator_enum_signed_user.n"
library_dump "" "$tmp/generic" "$abi_programs/generic_modify_parameter.n" \
    "$abi_programs/generic_local.n"
abi="$tmp/abi"
rec="$tmp/records"
cycle="$tmp/abi"
cycle_long="$tmp/cycle_long"
cycle_self="$tmp/cycle_self"

"$abi_record_object_test" "$abi/import_root.n00bm" import
"$abi_record_object_test" "$abi/__ffi_c_foreign___ffi_literal_1.n00bm" c-foreign
"$abi_record_object_test" "$rec/record_root.n00bm" import-record
"$abi_link_input_test" --field "$rec/record_dep.n00bm"

# The builtin library's modules, the ones its literals generate among them,
# come with every build.
set --
for object in "$rec"/*.n00bm; do
    case "$(basename "$object")" in
    linked.n00bm | record_* | shared_link_*) continue ;;
    esac
    set -- "$@" "$object"
done
"$abi_link_input_test" --records "$rec/record_root.n00bm" \
    "$rec/record_dep.n00bm" "$@"
"$abi_link_input_test" --using-rows "$rec/record_root.n00bm" \
    "$rec/record_user.n00bm" record_dep.SharedRecord
"$abi_register_object_test" "$rec/linked.n00bm" '>=2'
"$abi_link_input_test" --function-import "$rec/shared_link_first.n00bm" \
    "$rec/shared_link_dep.facade.n00bm" "$rec/shared_link_dep.impl.n00bm" "$@"
# A root named relative to another working directory. struct_order_root
# (a record built from a tuple in another module) only has to build.
mkdir -p "$tmp/nested"
(cd "$script_dir/programs" \
    && N00B_PATH="$records" "$compiler" library \
        --dev-module-abi "$tmp/nested" --output "$tmp/nested/linked.n00bm" \
        shared_link_first struct_order_root >"$tmp/nested.err" 2>&1) || {
    cat "$tmp/nested.err" >&2
    exit 1
}
[ -s "$tmp/nested/shared_link_first.n00bm" ]
[ -s "$tmp/nested/struct_order_root.n00bm" ]
"$abi_link_input_test" --value-import "$abi/abi_value_root.n00bm" \
    "$abi/abi_value_dep.n00bm" "$@"
N00B_PATH="$abi_programs" "$compiler" library \
    --output "$tmp/abi_value_library.n00bm" abi_value_root
"$abi_link_input_test" --linked-library "$tmp/abi_value_library.n00bm"
N00B_PATH="$abi_programs" "$compiler" library \
    --output "$tmp/abi_value_library_multi.n00bm" abi_value_root abi_value_dep
"$abi_link_input_test" --linked-library "$tmp/abi_value_library_multi.n00bm"
"$abi_record_object_test" "$abi/foreign_enum_user.n00bm" import-enum
"$abi_record_object_test" "$tmp/interfaces/foreign_flagset_user.n00bm" \
    import-flagset
[ -s "$tmp/interfaces/operator_enum_signed_user.n00bm" ]
"$abi_link_input_test" --linked-flagset "$tmp/interfaces/linked.n00bm"
"$abi_record_object_test" "$tmp/generic/generic_modify_parameter.n00bm" generic
"$abi_record_object_test" "$tmp/generic/generic_local.n00bm" generic-local
"$abi_record_object_test" "$cycle/cycles.cycle_first.n00bm" cycle \
    "$cycle/cycles.cycle_second.n00bm"
"$abi_record_object_test" "$cycle_long/cycles.cycle_long_a.n00bm" cycle \
    "$cycle_long/cycles.cycle_long_b.n00bm" \
    "$cycle_long/cycles.cycle_long_c.n00bm"
# The library objects a build's cycle requires (every cycle module imports
# `builtin`), which the link tests select along with it.
cycle_library() {
    for object in "$1"/*.n00bm; do
        case "$(basename "$object")" in
        linked.n00bm | cycles.*) continue ;;
        esac
        printf '%s\n' "$object"
    done
}
# (The temporary directory's paths have no spaces.)
# shellcheck disable=SC2046
"$abi_link_input_test" --cycle "$tmp/cycle_long_archive_linked.n00bm" \
    "$cycle_long/cycles.cycle_long_a.n00bm" \
    "$cycle_long/cycles.cycle_long_b.n00bm" \
    "$cycle_long/cycles.cycle_long_c.n00bm" $(cycle_library "$cycle_long")
"$abi_register_object_test" "$tmp/cycle_long_archive_linked.n00bm" '>=3'
"$abi_register_object_test" "$cycle_long/linked.n00bm" '>=3'
"$abi_record_object_test" "$cycle_self/cycles.cycle_self.n00bm" cycle
# shellcheck disable=SC2046
"$abi_link_input_test" --self "$cycle_self/cycles.cycle_self.n00bm" \
    $(cycle_library "$cycle_self")
"$abi_register_object_test" "$cycle_self/linked.n00bm" '>=1'
"$abi_register_object_test" "$tmp/alone/linked.n00bm" '>=2'
