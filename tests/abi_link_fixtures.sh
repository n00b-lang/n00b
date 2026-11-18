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

library_dump "$abi_programs" "$tmp/abi" import_root c_foreign \
    abi_value_root foreign_enum_user cycles.cycle_first
# These builds' linked libraries are checked below as well.
library_dump "$abi_programs" "$tmp/cycle_long" cycles.cycle_long_a
library_dump "$abi_programs" "$tmp/cycle_self" cycles.cycle_self
# A module's object depends only on the module, not on what else the build
# has in it (the cache reuses it in other programs). The other root is
# cheap on purpose: this build is only here for the comparison.
library_dump "$abi_programs" "$tmp/alone" empty cycles.cycle_first
cmp "$tmp/abi/cycles.cycle_first.n00bm" \
    "$tmp/alone/cycles.cycle_first.n00bm" || {
    echo "cycles.cycle_first's ABI object differs with other roots" >&2
    exit 1
}
library_dump "$records" "$tmp/records" record_root shared_link_first
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
N00B_PATH="$records" "$compiler" library \
    --output "$tmp/import_record_linked.n00bm" record_root
"$abi_register_object_test" "$tmp/import_record_linked.n00bm" '>=2'
"$abi_link_input_test" --function-import "$rec/shared_link_first.n00bm" \
    "$rec/shared_link_dep.facade.n00bm" "$rec/shared_link_dep.impl.n00bm" "$@"
# A root named relative to another working directory.
mkdir -p "$tmp/nested"
(cd "$script_dir/programs" \
    && N00B_PATH="$records" "$compiler" library \
        --dev-module-abi "$tmp/nested" --output "$tmp/nested/linked.n00bm" \
        shared_link_first >"$tmp/nested.err" 2>&1) || {
    cat "$tmp/nested.err" >&2
    exit 1
}
[ -s "$tmp/nested/shared_link_first.n00bm" ]
N00B_PATH="$records" "$compiler" library \
    --output "$tmp/struct_order.n00bm" struct_order_root
[ -s "$tmp/struct_order.n00bm" ]
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
N00B_PATH="$interface_programs" "$compiler" library \
    --output "$tmp/linked_flagset_abi.n00bm" \
    "$abi_programs/foreign_flagset_user.n"
"$abi_link_input_test" --linked-flagset "$tmp/linked_flagset_abi.n00bm"
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
N00B_PATH="$abi_programs" "$compiler" library \
    --output "$tmp/enum_export_linked.n00bm" enum_export.__init
"$abi_register_object_test" "$tmp/enum_export_linked.n00bm" '>=2'
