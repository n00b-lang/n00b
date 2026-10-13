#!/bin/sh
set -eu

compiler=$1
abi_record_object_test=$2
abi_link_input_test=$3
abi_register_object_test=$4
script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
interface_programs="$script_dir/programs/interfaces"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

N00B_PATH="$script_dir/programs/abi" "$compiler" compile \
    --output "$tmp/import_abi.n00bm" import_root
[ -s "$tmp/import_abi.n00bm" ]
"$abi_record_object_test" "$tmp/import_abi.n00bm" import
N00B_PATH="$script_dir/programs/link_records" "$compiler" compile \
    --output "$tmp/import_record_abi.n00bm" record_root
[ -s "$tmp/import_record_abi.n00bm" ]
"$abi_record_object_test" "$tmp/import_record_abi.n00bm" import-record
N00B_PATH="$script_dir/programs/link_records" "$compiler" compile \
    --output "$tmp/record_dep_abi.n00bm" record_dep
[ -s "$tmp/record_dep_abi.n00bm" ]
"$abi_link_input_test" --field "$tmp/record_dep_abi.n00bm"
set --
for module in builtin.io.debug builtin.io.raw \
    builtin.containers.list builtin.containers.traits \
    builtin.containers.view builtin.cursor builtin.containers.dict \
    builtin.hash builtin.containers.hash_index builtin.sort \
    builtin.containers.set; do
    N00B_PATH="$script_dir/programs/link_records" "$compiler" compile \
        --output "$tmp/$module.n00bm" "$module" 2>"$tmp/$module.err" || {
        cat "$tmp/$module.err"
        exit 1
    }
    set -- "$@" "$tmp/$module.n00bm"
done
"$abi_link_input_test" --records "$tmp/import_record_abi.n00bm" \
    "$tmp/record_dep_abi.n00bm" "$@"
N00B_PATH="$script_dir/programs/link_records" "$compiler" library \
    --output "$tmp/import_record_linked.n00bm" record_root
"$abi_register_object_test" "$tmp/import_record_linked.n00bm" '>=2'
N00B_PATH="$script_dir/programs/link_records" "$compiler" compile \
    --output "$tmp/shared_link_first_abi.n00bm" shared_link_first
N00B_PATH="$script_dir/programs/link_records" "$compiler" compile \
    --output "$tmp/shared_link_facade_abi.n00bm" shared_link_dep.facade
N00B_PATH="$script_dir/programs/link_records" "$compiler" compile \
    --output "$tmp/shared_link_impl_abi.n00bm" shared_link_dep.impl
"$abi_link_input_test" --function-import "$tmp/shared_link_first_abi.n00bm" \
    "$tmp/shared_link_facade_abi.n00bm" "$tmp/shared_link_impl_abi.n00bm" "$@"
N00B_PATH="$script_dir/programs/abi" "$compiler" compile \
    --output "$tmp/abi_value_root.n00bm" abi_value_root
N00B_PATH="$script_dir/programs/abi" "$compiler" compile \
    --output "$tmp/abi_value_dep.n00bm" abi_value_dep
"$abi_link_input_test" --value-import "$tmp/abi_value_root.n00bm" \
    "$tmp/abi_value_dep.n00bm" "$@"
N00B_PATH="$script_dir/programs/abi" "$compiler" library \
    --output "$tmp/abi_value_library.n00bm" abi_value_root
"$abi_link_input_test" --linked-library "$tmp/abi_value_library.n00bm"
N00B_PATH="$script_dir/programs/abi" "$compiler" library \
    --output "$tmp/abi_value_library_multi.n00bm" abi_value_root abi_value_dep
"$abi_link_input_test" --linked-library "$tmp/abi_value_library_multi.n00bm"
N00B_PATH="$script_dir/programs/abi" "$compiler" compile \
    --output "$tmp/import_enum_abi.n00bm" foreign_enum_user
[ -s "$tmp/import_enum_abi.n00bm" ]
"$abi_record_object_test" "$tmp/import_enum_abi.n00bm" import-enum
N00B_PATH="$interface_programs" "$compiler" compile \
    --output "$tmp/import_flagset_abi.n00bm" \
    "$script_dir/programs/abi/foreign_flagset_user.n00b"
"$abi_record_object_test" "$tmp/import_flagset_abi.n00bm" import-flagset
N00B_PATH="$interface_programs" "$compiler" compile \
    --output "$tmp/import_enum_facade_abi.n00bm" \
    "$interface_programs/operator_enum_signed_user.n00b"
[ -s "$tmp/import_enum_facade_abi.n00bm" ]
N00B_PATH="$interface_programs" "$compiler" library \
    --output "$tmp/linked_flagset_abi.n00bm" \
    "$script_dir/programs/abi/foreign_flagset_user.n00b"
"$abi_link_input_test" --linked-flagset "$tmp/linked_flagset_abi.n00bm"
"$compiler" compile --output "$tmp/generic_abi.n00bm" \
    "$script_dir/programs/abi/generic_modify_parameter.n00b"
[ -s "$tmp/generic_abi.n00bm" ]
"$abi_record_object_test" "$tmp/generic_abi.n00bm" generic
"$compiler" compile --output "$tmp/generic_local_abi.n00bm" \
    "$script_dir/programs/abi/generic_local.n00b"
[ -s "$tmp/generic_local_abi.n00bm" ]
"$abi_record_object_test" "$tmp/generic_local_abi.n00bm" generic-local
N00B_PATH="$script_dir/programs/abi" "$compiler" compile \
    --output "$tmp/cycle_first.n00bm" builtin.cycle_first
N00B_PATH="$script_dir/programs/abi" "$compiler" compile \
    --output "$tmp/cycle_second.n00bm" builtin.cycle_second
"$abi_record_object_test" "$tmp/cycle_first.n00bm" cycle \
    "$tmp/cycle_second.n00bm"
N00B_PATH="$script_dir/programs/abi" "$compiler" compile \
    --output "$tmp/cycle_long_a.n00bm" builtin.cycle_long_a
N00B_PATH="$script_dir/programs/abi" "$compiler" compile \
    --output "$tmp/cycle_long_b.n00bm" builtin.cycle_long_b
N00B_PATH="$script_dir/programs/abi" "$compiler" compile \
    --output "$tmp/cycle_long_c.n00bm" builtin.cycle_long_c
"$abi_record_object_test" "$tmp/cycle_long_a.n00bm" cycle \
    "$tmp/cycle_long_b.n00bm" "$tmp/cycle_long_c.n00bm"
"$abi_link_input_test" "$tmp/cycle_long_a.n00bm" \
    "$tmp/cycle_long_b.n00bm" "$tmp/cycle_long_c.n00bm" \
    "$tmp/cycle_long_archive_linked.n00bm"
"$abi_register_object_test" "$tmp/cycle_long_archive_linked.n00bm" '>=3'
N00B_PATH="$script_dir/programs/abi" "$compiler" library \
    --output "$tmp/cycle_long_linked.n00bm" builtin.cycle_long_a
"$abi_register_object_test" "$tmp/cycle_long_linked.n00bm" '>=3'
N00B_PATH="$script_dir/programs/abi" "$compiler" compile \
    --output "$tmp/cycle_self.n00bm" builtin.cycle_self
"$abi_record_object_test" "$tmp/cycle_self.n00bm" cycle
"$abi_link_input_test" --self "$tmp/cycle_self.n00bm"
N00B_PATH="$script_dir/programs/abi" "$compiler" library \
    --output "$tmp/cycle_self_linked.n00bm" builtin.cycle_self
"$abi_register_object_test" "$tmp/cycle_self_linked.n00bm" '>=1'
