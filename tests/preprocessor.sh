set -eu

compiler_dir="$(CDPATH= cd -- "$(dirname -- "$1")" && pwd)"
compiler="$compiler_dir/$(basename -- "$1")"
storage_interface_writer="$2"
script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
program_dir="$script_dir/programs/preprocessor"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

interface_programs="$script_dir/programs/interfaces"

if "$compiler" lower "$program_dir/invalid-module-name.n00b" \
    >"$tmp/invalid_module_name.out" 2>"$tmp/invalid_module_name.err"; then
    echo 'invalid module filename unexpectedly lowered' >&2
    exit 1
fi
grep -q 'invalid module or file name' "$tmp/invalid_module_name.err"

reject_shape_record() {
    archive=$1
    label=$2
    record_offset=$3
    field_offset=$4
    replacement=$5
    consumer=$6
    owner_pattern=$7
    corrupted="$tmp/$label.n00bi"
    cp "$archive" "$corrupted"
    printf '%b' "$replacement" | dd of="$corrupted" bs=1 \
        seek="$((record_offset + field_offset))" conv=notrunc 2>/dev/null
    if "$compiler" lower --link-interface "$corrupted" "$consumer" \
        >"$tmp/$label.out" 2>"$tmp/$label.err"; then
        echo "malformed array-shape record '$label' unexpectedly loaded" >&2
        exit 1
    fi
    grep -q 'fatal\[N00BB-L0046\]' "$tmp/$label.err"
    grep -Eq "$owner_pattern" "$tmp/$label.err"
}

"$storage_interface_writer" "$tmp/absent_failure.interface" \
    --absent-failure
"$compiler" preprocess --link-interface "$tmp/absent_failure.interface" \
    --dump-symbols "$interface_programs/generic_absent_user.n00b" \
    >"$tmp/generic_absent_user.out" \
    2>"$tmp/generic_absent_user.err" || {
    cat "$tmp/generic_absent_user.err"
    cat "$tmp/generic_absent_user.out"
    exit 1
}
grep -q 'answer: function, mutable, type=() -> int fail: nil' \
    "$tmp/generic_absent_user.out"

"$storage_interface_writer" "$tmp/required_storage.interface"
"$compiler" lower --link-interface "$tmp/required_storage.interface" \
    --dump-ir --dump-link-unit --dump-symbols \
    "$interface_programs/required_storage_user.n00b" \
    >"$tmp/required_storage_user.out" \
    2>"$tmp/required_storage_user.err" || {
    cat "$tmp/required_storage_user.err"
    cat "$tmp/required_storage_user.out"
    exit 1
}
grep -q 'slot: variable, mutable, type=int, visibility=public, phases=runtime, mutable-in=runtime, initializer-in=comptime+runtime, storage-duration=module, .*requested-storage=vendor.pool/opaque, selected-storage=vendor.pool/opaque, storage-class-required' \
    "$tmp/required_storage_user.out"
grep -q '@required_storage_dep.slot: int .*ir.storage_requested("vendor.pool/opaque") .*ir.storage_selected("vendor.pool/opaque") .*ir.storage_required(true)' \
    "$tmp/required_storage_user.out"
grep -q 'n00bb.finalization v1 unavailable' \
    "$tmp/required_storage_user.out"
grep -q 'cleanup-range\[slot,phase=runtime,storage=module,release=unavailable,.*finalizer=unspecified\]' \
    "$tmp/required_storage_user.out"
"$compiler" library --emit-interface "$tmp/required_storage_roundtrip.interface" \
    --link-interface "$tmp/required_storage.interface" \
    "$interface_programs/required_storage_user.n00b" \
    >"$tmp/required_storage_library.out" \
    2>"$tmp/required_storage_library.err" || {
    cat "$tmp/required_storage_library.err"
    exit 1
}
"$compiler" lower \
    --link-interface "$tmp/required_storage_roundtrip.interface" \
    --dump-ir --dump-symbols \
    "$interface_programs/required_storage_roundtrip_user.n00b" \
    >"$tmp/required_storage_roundtrip_user.out" \
    2>"$tmp/required_storage_roundtrip_user.err" || {
    cat "$tmp/required_storage_roundtrip_user.err"
    cat "$tmp/required_storage_roundtrip_user.out"
    exit 1
}
grep -q 'requested-storage=vendor.pool/opaque, selected-storage=vendor.pool/opaque, storage-class-required' \
    "$tmp/required_storage_roundtrip_user.out"
grep -q '@required_storage_dep.slot: int .*ir.storage_requested("vendor.pool/opaque") .*ir.storage_selected("vendor.pool/opaque") .*ir.storage_required(true)' \
    "$tmp/required_storage_roundtrip_user.out"
"$storage_interface_writer" "$tmp/missing_storage_class.interface" \
    --missing-class
if "$compiler" lower \
    --link-interface "$tmp/missing_storage_class.interface" \
    "$interface_programs/required_storage_user.n00b" \
    >"$tmp/missing_storage_class.out" \
    2>"$tmp/missing_storage_class.err"; then
    echo 'required storage request without a class unexpectedly passed' >&2
    exit 1
fi
grep -q 'fatal\[N00BB-L0046\].*required dynamic storage class has no name' \
    "$tmp/missing_storage_class.err"

N00B_PATH="$interface_programs" "$compiler" library \
    --emit-interface "$tmp/export_origin.interface" \
    export_origin.facade \
    >"$tmp/export_origin_library.out" \
    2>"$tmp/export_origin_library.err" || {
    cat "$tmp/export_origin_library.err"
    exit 1
}
"$compiler" lower --link-interface "$tmp/export_origin.interface" \
    --dump-ir --dump-symbols \
    "$interface_programs/export_origin_consumer.n00b" \
    >"$tmp/export_origin_consumer.out" \
    2>"$tmp/export_origin_consumer.err" || {
    cat "$tmp/export_origin_consumer.err"
    cat "$tmp/export_origin_consumer.out"
    exit 1
}
grep -q 'preserve_identity: function, mutable, type=(export_origin.impl.Token) -> export_origin.impl.Token fail: nil' \
    "$tmp/export_origin_consumer.out"
test "$(grep -c 'Token: type, mutable, type=export_origin.impl.Token' \
    "$tmp/export_origin_consumer.out")" -eq 2
"$storage_interface_writer" "$tmp/export_origin_conflict.interface" \
    --origin-conflict
if "$compiler" lower \
    --link-interface "$tmp/export_origin_conflict.interface" \
    "$interface_programs/export_origin_conflict_consumer.n00b" \
    >"$tmp/export_origin_conflict.out" \
    2>"$tmp/export_origin_conflict.err"; then
    echo 'conflicting canonical type representations unexpectedly passed' >&2
    exit 1
fi
grep -q 'fatal\[N00BB-L0046\].*interface aliases disagree on a canonical type representation' \
    "$tmp/export_origin_conflict.err"

"$compiler" lower --dump-ir --dump-link-unit \
    "$program_dir/builtin_option_operations.n00b" \
    >"$tmp/builtin_option_operations.out" \
    2>"$tmp/builtin_option_operations.err" || {
    cat "$tmp/builtin_option_operations.err"
    cat "$tmp/builtin_option_operations.out"
    exit 1
}
grep -q 'option.get_or_else' "$tmp/builtin_option_operations.out"
grep -q 'option.is_nil' "$tmp/builtin_option_operations.out"
grep -q 'resolved builtin_option_operations -> builtins:1:get_or_else' \
    "$tmp/builtin_option_operations.out"
grep -q 'resolved builtin_option_operations -> builtins:1:is_nil' \
    "$tmp/builtin_option_operations.out"
! grep -Eq 'trait-dispatch\[unavailable\].*builtin_option_operations' \
    "$tmp/builtin_option_operations.out"
! grep -q 'function-global-unknown.*builtin_option_operations' \
    "$tmp/builtin_option_operations.out"

"$compiler" lower --dump-ir \
    "$program_dir/builtin_atomic_locking_types.n00b" \
    >"$tmp/builtin_atomic_locking_types.out" \
    2>"$tmp/builtin_atomic_locking_types.err" || {
    cat "$tmp/builtin_atomic_locking_types.err"
    cat "$tmp/builtin_atomic_locking_types.out"
    exit 1
}
grep -q 'atomic\[int\]' "$tmp/builtin_atomic_locking_types.out"
grep -q 'array_locking\[int\]' "$tmp/builtin_atomic_locking_types.out"
grep -q 'list_locking\[int\]' "$tmp/builtin_atomic_locking_types.out"
grep -q 'set_locking\[string\]' "$tmp/builtin_atomic_locking_types.out"
grep -q 'dict_locking\[string, int\]' "$tmp/builtin_atomic_locking_types.out"

"$compiler" lower --dump-link-unit \
    "$program_dir/builtin_locking_open_trait.n00b" \
    >"$tmp/builtin_locking_open_trait.out" \
    2>"$tmp/builtin_locking_open_trait.err" || {
    cat "$tmp/builtin_locking_open_trait.err"
    cat "$tmp/builtin_locking_open_trait.out"
    exit 1
}
for method in inspect_array inspect_list inspect_set inspect_dict; do
    grep -q "trait-dispatch\\[resolved\\] builtin_locking_open_trait:1:use_locking_collections -> builtin_locking_open_trait:1:${method}" \
        "$tmp/builtin_locking_open_trait.out"
done

"$compiler" lower --dump-ir --dump-layout \
    "$program_dir/builtin_atomic_access.n00b" \
    >"$tmp/builtin_atomic_access.out" \
    2>"$tmp/builtin_atomic_access.err" || {
    cat "$tmp/builtin_atomic_access.err"
    cat "$tmp/builtin_atomic_access.out"
    exit 1
}
grep -q 'type atomic\[int\] = int' "$tmp/builtin_atomic_access.out"
test "$(grep -c '= load atomic ' "$tmp/builtin_atomic_access.out")" -eq 3
test "$(grep -c 'store atomic ' "$tmp/builtin_atomic_access.out")" -eq 2
! grep -Eq '= load %__n00bb_storage_' "$tmp/builtin_atomic_access.out"

"$compiler" lower --dump-ir \
    "$program_dir/builtin_atomic_coercion.n00b" \
    >"$tmp/builtin_atomic_coercion.out" \
    2>"$tmp/builtin_atomic_coercion.err" || {
    cat "$tmp/builtin_atomic_coercion.err"
    cat "$tmp/builtin_atomic_coercion.out"
    exit 1
}
grep -q '= load atomic ' "$tmp/builtin_atomic_coercion.out"
grep -q 'store atomic ' "$tmp/builtin_atomic_coercion.out"
grep -q '= atomic.wrap %' "$tmp/builtin_atomic_coercion.out"
grep -q '= call @add_one,' "$tmp/builtin_atomic_coercion.out"

"$compiler" lower --dump-ir \
    "$program_dir/builtin_atomic_global.n00b" \
    >"$tmp/builtin_atomic_global.out" \
    2>"$tmp/builtin_atomic_global.err" || {
    cat "$tmp/builtin_atomic_global.err"
    cat "$tmp/builtin_atomic_global.out"
    exit 1
}
grep -q 'address.of @builtin_atomic_global.counter' \
    "$tmp/builtin_atomic_global.out"
grep -q 'store atomic ' "$tmp/builtin_atomic_global.out"
grep -q '= load atomic ' "$tmp/builtin_atomic_global.out"

if "$compiler" lower --dump-ir \
    "$program_dir/builtin_atomic_partial_update.n00b" \
    >"$tmp/builtin_atomic_partial_update.out" \
    2>"$tmp/builtin_atomic_partial_update.err"; then
    echo 'atomic field update unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'fatal\[N00BB-T0017\].*cannot be updated through a field' \
    "$tmp/builtin_atomic_partial_update.err"
test "$(grep -c 'fatal\[N00BB-T0017\]' \
    "$tmp/builtin_atomic_partial_update.err")" -eq 1

if "$compiler" lower --dump-ir \
    "$program_dir/builtin_atomic_unsupported.n00b" \
    >"$tmp/builtin_atomic_unsupported.out" \
    2>"$tmp/builtin_atomic_unsupported.err"; then
    echo 'unsupported native atomic type unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'fatal\[N00BB-T0018\].*not supported by the selected target' \
    "$tmp/builtin_atomic_unsupported.err"

if "$compiler" lower --dump-ir \
    "$program_dir/builtin_atomic_bad_arity.n00b" \
    >"$tmp/builtin_atomic_bad_arity.out" \
    2>"$tmp/builtin_atomic_bad_arity.err"; then
    echo 'atomic type with the wrong number of arguments unexpectedly passed' >&2
    exit 1
fi
grep -q 'fatal\[N00BB-T0016\].*Atomic.*requires 1 type argument' \
    "$tmp/builtin_atomic_bad_arity.err"

for type in array_locking list_locking set_locking dict_locking; do
    if "$compiler" lower --dump-ir \
        "$program_dir/builtin_${type}_bad_arity.n00b" \
        >"$tmp/builtin_${type}_bad_arity.out" \
        2>"$tmp/builtin_${type}_bad_arity.err"; then
        echo "$type with the wrong number of type arguments unexpectedly passed" >&2
        exit 1
    fi
    expected_arity=1
    test "$type" = dict_locking && expected_arity=2
    case "$type" in
        array_locking) display_type=Array_Locking ;;
        list_locking) display_type=List_Locking ;;
        set_locking) display_type=Set_Locking ;;
        dict_locking) display_type=Dict_Locking ;;
    esac
    grep -q "fatal\\[N00BB-T0016\\].*${display_type}.*requires ${expected_arity} type argument" \
        "$tmp/builtin_${type}_bad_arity.err"
done

for fixture in lowercase_builtin_type_rejected lowercase_ref_type_rejected; do
    if "$compiler" preprocess "$program_dir/${fixture}.n00b" \
        >"$tmp/${fixture}.out" 2>"$tmp/${fixture}.err"; then
        echo "$fixture unexpectedly accepted an obsolete lowercase type" >&2
        exit 1
    fi
    grep -Eq 'N00BB-(P0001|L0014|T0007)' "$tmp/${fixture}.err"
done

"$compiler" lower --dump-ir --dump-control-flow --dump-symbols \
    "$program_dir/builtin_option_narrowing.n00b" \
    >"$tmp/builtin_option_narrowing.out" \
    2>"$tmp/builtin_option_narrowing.err" || {
    cat "$tmp/builtin_option_narrowing.err"
    cat "$tmp/builtin_option_narrowing.out"
    exit 1
}
grep -q 'nil = nil' "$tmp/builtin_option_narrowing.out"
grep -q 'option.is_nil' "$tmp/builtin_option_narrowing.out"
grep -q 'option.get ' "$tmp/builtin_option_narrowing.out"
grep -q 'branch .*__n00bb_if_then_' "$tmp/builtin_option_narrowing.out"

"$compiler" lower --dump-ir \
    "$program_dir/builtin_option_assignment.n00b" \
    >"$tmp/builtin_option_assignment.out" \
    2>"$tmp/builtin_option_assignment.err" || {
    cat "$tmp/builtin_option_assignment.err"
    cat "$tmp/builtin_option_assignment.out"
    exit 1
}
test "$(grep -c ': option\[int\] = option.none' \
    "$tmp/builtin_option_assignment.out")" -eq 2
grep -q ': option\[int\] = option.some %input' \
    "$tmp/builtin_option_assignment.out"
test "$(grep -c 'store .*%__n00bb_[0-9][0-9]* !loc' \
    "$tmp/builtin_option_assignment.out")" -eq 3

if "$compiler" lower --dump-ir \
    "$program_dir/builtin_option_get_unchecked.n00b" \
    >"$tmp/builtin_option_get_unchecked.out" \
    2>"$tmp/builtin_option_get_unchecked.err"; then
    echo 'option.get without a set-value proof unexpectedly passed' >&2
    exit 1
fi
grep -q 'option.get requires a value proven non-nil' \
    "$tmp/builtin_option_get_unchecked.err"

N00B_PATH="$interface_programs" "$compiler" library \
    --emit-interface "$tmp/render.interface" \
    render_contract.facade \
    >"$tmp/render_interface.out" 2>"$tmp/render_interface.err" || {
    cat "$tmp/render_interface.err"
    exit 1
}
test -s "$tmp/render.interface"
"$compiler" preprocess --link-interface "$tmp/render.interface" \
    --dump-link-unit --dump-symbols "$interface_programs/render_plugin.n00b" \
    >"$tmp/render_plugin.out" 2>"$tmp/render_plugin.err" || {
    cat "$tmp/render_plugin.err"
    cat "$tmp/render_plugin.out"
    exit 1
}
grep -q 'trait-dispatch\[resolved\].*render_plugin:1:use_render -> render_plugin:1:render' \
    "$tmp/render_plugin.out"
grep -q 'resolved render_plugin -> render_contract.impl:[0-9]*:external_value' \
    "$tmp/render_plugin.out"
grep -q 'resolved render_plugin -> render_contract.impl:[0-9]*:ArchivedPoint.x' \
    "$tmp/render_plugin.out"
grep -q 'resolved render_plugin -> render_contract.impl:[0-9]*:ArchivedValue.number' \
    "$tmp/render_plugin.out"
grep -q 'resolved render_plugin -> render_contract.impl:[0-9]*:ArchivedValue.text' \
    "$tmp/render_plugin.out"
grep -q 'render_contract.impl:[0-9]*:ArchivedColor kind=' \
    "$tmp/render_plugin.out"
grep -q 'red: enum-constant, const, type=render_contract.impl.ArchivedColor, owner-type=ArchivedColor, value=0x00000000000000000000000000000000, phases=comptime+runtime, mutable-in=none, initializer-in=comptime, storage-duration=comptime' \
    "$tmp/render_plugin.out"
grep -q 'green: enum-constant, const, type=render_contract.impl.ArchivedColor, owner-type=ArchivedColor, value=0x00000000000000000000000000000002, phases=comptime+runtime, mutable-in=none, initializer-in=comptime, storage-duration=comptime' \
    "$tmp/render_plugin.out"
"$compiler" lower --link-interface "$tmp/render.interface" --dump-ir \
    --dump-layout \
    --dump-link-unit "$interface_programs/render_plugin.n00b" \
    >"$tmp/render_plugin.ir" 2>"$tmp/render_plugin.lower.err" || {
    cat "$tmp/render_plugin.lower.err"
    cat "$tmp/render_plugin.ir"
    exit 1
}
grep -q '= extract .* "x"' "$tmp/render_plugin.ir"
grep -Eq 'layout render_contract.impl.ArchivedPoint target=(x86_64-linux|arm64-macos|x86_64-windows) size=8 align=8' \
    "$tmp/render_plugin.ir"
grep -q 'member x bit-offset=0 bit-width=64' "$tmp/render_plugin.ir"
grep -q 'call @external_value' "$tmp/render_plugin.ir"
if "$compiler" preprocess --link-interface "$tmp/render.interface" \
    "$interface_programs/render_private_field.n00b" \
    >"$tmp/render_private_field.out" 2>&1; then
    echo 'private archive-backed field access unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'N00BB-L0021.*record field is not visible' \
    "$tmp/render_private_field.out"
if "$compiler" preprocess \
    --link-interface "$tmp/render.interface" \
    --link-interface "$tmp/render.interface" \
    "$interface_programs/render_plugin.n00b" \
    >"$tmp/conflicting_interface.out" 2>&1; then
    echo 'duplicate linked trait interface unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'error\[TH-L0003\]: conflicting trait declaration: render_contract.impl.Render' \
    "$tmp/conflicting_interface.out"

"$compiler" parse --jobs 4 "$program_dir/parallel_first.n00b" \
    "$program_dir/parallel_second.n00b" >"$tmp/parallel_roots.out"
grep -q 'parallel_first' "$tmp/parallel_roots.out"
grep -q 'parallel_second' "$tmp/parallel_roots.out"

"$compiler" parse --dump-tree "$script_dir/programs/array_shape_types.n00b" \
    >"$tmp/array_shape_types.tree"
grep -q '<array-dimensions>' "$tmp/array_shape_types.tree"
grep -q '<tspec-application>' "$tmp/array_shape_types.tree"
grep -q 'rectangular_field' "$tmp/array_shape_types.tree"
grep -q 'shape_parameter' "$tmp/array_shape_types.tree"
grep -q 'shape_return' "$tmp/array_shape_types.tree"
"$compiler" lower --dump-symbols --dump-link-unit --dump-ir \
    --dump-control-flow \
    "$script_dir/programs/array_shape_types.n00b" \
    >"$tmp/array_shape_types.lower" 2>"$tmp/array_shape_types.err" || {
    cat "$tmp/array_shape_types.err"
    cat "$tmp/array_shape_types.lower"
    exit 1
}
grep -Fq 'rectangular: variable, mutable, type=Array[Array[int]]' \
    "$tmp/array_shape_types.lower"
grep -Fq 'rectangular_field: variable, mutable, type=Array[Array[int]]' \
    "$tmp/array_shape_types.lower"
grep -Eq 'rectangular_field:.*array-shape=rank:2' \
    "$tmp/array_shape_types.lower"
grep -Eq 'input:.*array-shape=rank:2' \
    "$tmp/array_shape_types.lower"
grep -Eq 'array-shape\[rank=2,flat-prefix=2,dim0=comptime(:producer=[^,]+)?,dim1=comptime(:producer=[^,]+)?\]' \
    "$tmp/array_shape_types.lower"
grep -Eq 'array-shape\[rank=2,flat-prefix=1,dim0=comptime(:producer=[^,]+)?,dim1=unresolved\]' \
    "$tmp/array_shape_types.lower"
grep -Eq 'array-shape\[rank=2,flat-prefix=0,dim0=unresolved,dim1=unresolved\]' \
    "$tmp/array_shape_types.lower"
grep -Eq 'array-element-count=comptime:producer=__n00bb_array_product_' \
    "$tmp/array_shape_types.lower"
grep -Eq 'func @__n00bb_array_product_.*\(\) -> uint fail: uint' \
    "$tmp/array_shape_types.lower"
grep -Eq '= mul trap ' "$tmp/array_shape_types.lower"
grep -Eq 'control-flow @__n00bb_array_product_' \
    "$tmp/array_shape_types.lower"
grep -Eq 'array_shape_types:0:ShapeHolder\.rectangular_field .*array-element-count=comptime:producer=__n00bb_array_product_' \
    "$tmp/array_shape_types.lower"
grep -Eq 'func @shape_parameter\(%input: array\[array\[int\]\].*\) -> array\[array\[int\]\]' \
    "$tmp/array_shape_types.lower"
grep -Fq 'func @shape_return() -> array[int]' \
    "$tmp/array_shape_types.lower"
grep -q 'warning\[N00BB-W0004\].*mixes explicit array syntax with dimension shorthand' \
    "$tmp/array_shape_types.err"

"$compiler" lower --dump-symbols --dump-link-unit \
    "$script_dir/programs/preprocessor/grouped_array_declarations.n00b" \
    >"$tmp/grouped_array_declarations.out" \
    2>"$tmp/grouped_array_declarations.err" || {
    cat "$tmp/grouped_array_declarations.err"
    cat "$tmp/grouped_array_declarations.out"
    exit 1
}
grep -Eq 'first: variable, .*array-shape=rank:1, dim0=comptime' \
    "$tmp/grouped_array_declarations.out"
grep -Eq 'second: variable, .*array-shape=rank:1, dim0=comptime' \
    "$tmp/grouped_array_declarations.out"
! grep -Eq 'first: variable, .*array-shape=rank:1, dim0=unresolved|second: variable, .*array-shape=rank:1, dim0=unresolved' \
    "$tmp/grouped_array_declarations.out"

"$compiler" lower --dump-symbols --dump-link-unit --dump-ir \
    "$script_dir/programs/array_inferred_declaration.n00b" \
    >"$tmp/array_inferred_declaration.out" \
    2>"$tmp/array_inferred_declaration.err" || {
    cat "$tmp/array_inferred_declaration.err"
    cat "$tmp/array_inferred_declaration.out"
    exit 1
}
grep -Eq 'inferred_values: variable, mutable, type=Array\[int\], array-shape=rank:1, dim0=unresolved' \
    "$tmp/array_inferred_declaration.out"
grep -Eq 'static global @array_inferred_declaration.inferred_values: array\[int\]' \
    "$tmp/array_inferred_declaration.out"
grep -Eq 'array_inferred_declaration:0:inferred_values .*binding ir.array-shape' \
    "$tmp/array_inferred_declaration.out"
grep -Eq 'array\[int\] = aggregate ' \
    "$tmp/array_inferred_declaration.out"
grep -Eq ': int = const 1' "$tmp/array_inferred_declaration.out"
grep -Eq ': int = const 2' "$tmp/array_inferred_declaration.out"

N00B_PATH="$script_dir/programs" "$compiler" lower --dump-ir \
    "$script_dir/programs/array_runtime_shape_checks.n00b" \
    >"$tmp/array_runtime_shape_checks.out" 2>&1 || {
    cat "$tmp/array_runtime_shape_checks.out"
    exit 1
}
test "$(grep -c 'sequence.shape.check' \
    "$tmp/array_runtime_shape_checks.out")" -ge 17
grep -q '!ir.shape_constraint(rank=1,dim0=comptime:producer=' \
    "$tmp/array_runtime_shape_checks.out"
grep -Eq 'func @store_shaped_row\(%matrix: array\[array\[int\]\].*!ir.alias_count\(1\).*ir.alias0\(%__lowered_[0-9]+\).*ir.alias0_escapes\(true\)' \
    "$tmp/array_runtime_shape_checks.out"
grep -q '__n00bb_index_store_is_ok_' \
    "$tmp/array_runtime_shape_checks.out"
grep -q 'variant.payload .*"error"' \
    "$tmp/array_runtime_shape_checks.out"
grep -q 'jump __n00bb_try_handler_' \
    "$tmp/array_runtime_shape_checks.out"

N00B_PATH="$script_dir/programs" "$compiler" lower --dump-ir \
    "$script_dir/programs/array_runtime_shape_import.n00b" \
    >"$tmp/array_runtime_shape_import.out" 2>&1 || {
    cat "$tmp/array_runtime_shape_import.out"
    exit 1
}
grep -q 'call @remote_return' "$tmp/array_runtime_shape_import.out"
grep -q 'sequence.shape.check.*!ir.shape_constraint(rank=1,dim0=comptime:producer=' \
    "$tmp/array_runtime_shape_import.out"

if "$compiler" lower "$program_dir/array_dimension_non_integer.n00b" \
    >"$tmp/array_dimension_non_integer.out" 2>&1; then
    echo 'non-integer array dimension unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'N00BB-A0002.*array dimension expression must have an integer type' \
    "$tmp/array_dimension_non_integer.out"

if "$compiler" lower "$program_dir/array_dimension_bad_order.n00b" \
    >"$tmp/array_dimension_bad_order.out" 2>&1; then
    echo 'sized array dimension after an unsized dimension unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'N00BB-A0001.*sized array dimensions must precede unsized dimensions' \
    "$tmp/array_dimension_bad_order.out"

"$compiler" lower --dump-ir --dump-control-flow --dump-link-unit \
    "$script_dir/programs/array_dimension_comptime_call.n00b" \
    >"$tmp/array_dimension_comptime_call.out" 2>&1 || {
    cat "$tmp/array_dimension_comptime_call.out"
    exit 1
}
grep -q 'func @__n00bb_array_extent_.*fail: nil' \
    "$tmp/array_dimension_comptime_call.out"
grep -q 'call @__n00bb_comptime_calculate_extent' \
    "$tmp/array_dimension_comptime_call.out"
grep -q 'control-flow @__n00bb_array_extent_' \
    "$tmp/array_dimension_comptime_call.out"
grep -q 'func @__n00bb_comptime_calculate_extent' \
    "$tmp/array_dimension_comptime_call.out"
grep -Eq 'array-shape\[rank=1,flat-prefix=1,dim0=comptime:producer=__n00bb_array_extent_' \
    "$tmp/array_dimension_comptime_call.out"
grep -Eq ': uint = cast trap %__n00bb_' \
    "$tmp/array_dimension_comptime_call.out"
grep -Eq ': uint = sub trap %__n00bb_extent_.*, %__n00bb_extent_one_' \
    "$tmp/array_dimension_comptime_call.out"
grep -Eq 'array-element-count=comptime:producer=__n00bb_array_product_' \
    "$tmp/array_dimension_comptime_call.out"

"$compiler" lower --dump-ir --dump-control-flow --dump-link-unit \
    "$script_dir/programs/array_initializer_extent_deferred.n00b" \
    >"$tmp/array_initializer_extent_deferred.out" 2>&1 || {
    cat "$tmp/array_initializer_extent_deferred.out"
    exit 1
}
grep -Eq 'func @__n00bb_array_product_.*\(\) -> uint fail: uint' \
    "$tmp/array_initializer_extent_deferred.out"
grep -Eq '= eq .*__n00bb_extent_load_0.*__n00bb_initializer_extent_' \
    "$tmp/array_initializer_extent_deferred.out"
grep -Eq 'fail %__n00bb_initializer_failure_' \
    "$tmp/array_initializer_extent_deferred.out"
grep -Eq 'outcome=failure' "$tmp/array_initializer_extent_deferred.out"

"$compiler" lower --dump-ir \
    "$script_dir/programs/array_dimension_range_proven.n00b" \
    >"$tmp/array_dimension_range_proven.out" 2>&1 || {
    cat "$tmp/array_dimension_range_proven.out"
    exit 1
}
grep -Eq ': uint = cast wrap %__n00bb_' \
    "$tmp/array_dimension_range_proven.out"
! grep -Eq ': uint = sub trap .*__n00bb_extent_one_' \
    "$tmp/array_dimension_range_proven.out"

"$compiler" lower --dump-ir \
    "$script_dir/programs/array_dimension_range_assignment.n00b" \
    >"$tmp/array_dimension_range_assignment.out" 2>&1 || {
    cat "$tmp/array_dimension_range_assignment.out"
    exit 1
}
grep -Eq ': uint = cast wrap @array_dimension_range_assignment.extent' \
    "$tmp/array_dimension_range_assignment.out"
! grep -Eq ': uint = sub trap .*__n00bb_extent_one_' \
    "$tmp/array_dimension_range_assignment.out"

"$compiler" lower --dump-ir \
    "$script_dir/programs/array_dimension_range_branch_join.n00b" \
    >"$tmp/array_dimension_range_branch_join.out" 2>&1 || {
    cat "$tmp/array_dimension_range_branch_join.out"
    exit 1
}
grep -Eq ': bool = gt ' "$tmp/array_dimension_range_branch_join.out"
grep -Eq ': uint = cast wrap @array_dimension_range_branch_join.extent' \
    "$tmp/array_dimension_range_branch_join.out"
! grep -Eq ': uint = sub trap .*__n00bb_extent_one_' \
    "$tmp/array_dimension_range_branch_join.out"

"$compiler" lower --dump-ir \
    "$script_dir/programs/array_dimension_range_uncertain_join.n00b" \
    >"$tmp/array_dimension_range_uncertain_join.out" 2>&1 || {
    cat "$tmp/array_dimension_range_uncertain_join.out"
    exit 1
}
grep -Eq ': uint = sub trap .*__n00bb_extent_one_' \
    "$tmp/array_dimension_range_uncertain_join.out"

"$compiler" lower --dump-ir \
    "$script_dir/programs/array_dimension_range_complex_condition_unproven.n00b" \
    >"$tmp/array_dimension_range_complex_condition_unproven.out" 2>&1 || {
    cat "$tmp/array_dimension_range_complex_condition_unproven.out"
    exit 1
}
grep -Eq ': uint = sub trap .*__n00bb_extent_one_' \
    "$tmp/array_dimension_range_complex_condition_unproven.out"

"$compiler" lower --dump-ir \
    "$script_dir/programs/array_dimension_range_loop_unproven.n00b" \
    >"$tmp/array_dimension_range_loop_unproven.out" 2>&1 || {
    cat "$tmp/array_dimension_range_loop_unproven.out"
    exit 1
}
grep -Eq ': uint = sub trap .*__n00bb_extent_one_' \
    "$tmp/array_dimension_range_loop_unproven.out"

"$compiler" lower --dump-ir \
    "$script_dir/programs/comparison_string_equality.n00b" \
    >"$tmp/comparison_string_equality.out" 2>&1 || {
    cat "$tmp/comparison_string_equality.out"
    exit 1
}
grep -Eq ': bool = eq ' "$tmp/comparison_string_equality.out"

"$compiler" lower --dump-ir \
    "$script_dir/programs/array_dimension_range_identity.n00b" \
    >"$tmp/array_dimension_range_identity.out" 2>&1 || {
    cat "$tmp/array_dimension_range_identity.out"
    exit 1
}
grep -Eq ': uint = cast wrap %__n00bb_' \
    "$tmp/array_dimension_range_identity.out"
! grep -Eq ': uint = sub trap .*__n00bb_extent_one_' \
    "$tmp/array_dimension_range_identity.out"

N00B_PATH="$script_dir/programs" "$compiler" lower --dump-ir \
    "$script_dir/programs/array_dimension_range_import.n00b" \
    >"$tmp/array_dimension_range_import.out" 2>&1 || {
    cat "$tmp/array_dimension_range_import.out"
    exit 1
}
grep -q 'copy @array_dimension_range_dep.impl.extent' \
    "$tmp/array_dimension_range_import.out"
grep -Eq ': uint = cast wrap %__n00bb_' \
    "$tmp/array_dimension_range_import.out"
! grep -Eq ': uint = sub trap .*__n00bb_extent_one_' \
    "$tmp/array_dimension_range_import.out"

"$compiler" lower --dump-ir \
    "$script_dir/programs/array_dimension_range_product.n00b" \
    >"$tmp/array_dimension_range_product.out" 2>&1 || {
    cat "$tmp/array_dimension_range_product.out"
    exit 1
}
grep -Eq ': uint = cast wrap %__n00bb_' \
    "$tmp/array_dimension_range_product.out"
! grep -Eq ': uint = sub trap .*__n00bb_extent_one_' \
    "$tmp/array_dimension_range_product.out"

"$compiler" lower --dump-ir --dump-control-flow \
    "$script_dir/programs/array_dimension_failure_paths.n00b" \
    >"$tmp/array_dimension_failure_paths.out" 2>&1 || {
    cat "$tmp/array_dimension_failure_paths.out"
    exit 1
}
grep -Eq 'func @__n00bb_array_extent_.*fail: nil' \
    "$tmp/array_dimension_failure_paths.out"
grep -Eq ': uint = cast trap .*' \
    "$tmp/array_dimension_failure_paths.out"
grep -Eq ': uint = sub trap .*__n00bb_extent_one_' \
    "$tmp/array_dimension_failure_paths.out"
grep -Eq ': uint = mul trap .*' \
    "$tmp/array_dimension_failure_paths.out"
grep -Eq 'control-flow @__n00bb_array_product_' \
    "$tmp/array_dimension_failure_paths.out"
grep -q 'func @__n00bb_comptime_too_wide() -> i128' \
    "$tmp/array_dimension_failure_paths.out"
grep -q 'call @__n00bb_comptime_too_wide' \
    "$tmp/array_dimension_failure_paths.out"
grep -q 'cast trap %__n00bb_0' \
    "$tmp/array_dimension_failure_paths.out"
grep -q 'const -170141183460469231731687303715884105728' \
    "$tmp/array_dimension_failure_paths.out"
! grep -q 'const 170141183460469231731687303715884105728' \
    "$tmp/array_dimension_failure_paths.out"

if "$compiler" lower \
    "$script_dir/programs/array_dimension_literal_overflow.n00b" \
    >"$tmp/array_dimension_literal_overflow.out" 2>&1; then
    echo 'an integer literal outside i128 unexpectedly lowered' >&2
    exit 1
fi
grep -q 'IR-E0015].*does not fit in int' \
    "$tmp/array_dimension_literal_overflow.out"

"$compiler" lower --dump-ir --dump-link-unit \
    "$script_dir/programs/array_tagged_initializer.n00b" \
    >"$tmp/array_tagged_initializer.out" 2>&1 || {
    cat "$tmp/array_tagged_initializer.out"
    exit 1
}
grep -q 'array_tagged_initializer:0:rectangular_literal' \
    "$tmp/array_tagged_initializer.out"
grep -Eq 'array-shape\[rank=2,flat-prefix=2,dim0=comptime:producer=.*dim1=comptime:producer=' \
    "$tmp/array_tagged_initializer.out"
grep -Eq ': array\[array\[int\]\] = aggregate %__n00bb_' \
    "$tmp/array_tagged_initializer.out"

"$compiler" lower --dump-ir "$script_dir/programs/array_tagged_initializer_mismatch.n00b" \
    >"$tmp/array_tagged_initializer_mismatch.out" 2>&1 || {
    cat "$tmp/array_tagged_initializer_mismatch.out"
    exit 1
}
grep -Eq '__n00bb_initializer_extent_[0-9]+: uint = const 1' \
    "$tmp/array_tagged_initializer_mismatch.out"
grep -q 'fail %__n00bb_initializer_failure_' \
    "$tmp/array_tagged_initializer_mismatch.out"

if "$compiler" lower \
    "$script_dir/programs/array_initializer_rank_mismatch.n00b" \
    >"$tmp/array_initializer_rank_mismatch.out" 2>&1; then
    echo 'array initializer with the wrong rank unexpectedly lowered' >&2
    exit 1
fi
grep -Eq 'IR-E0011].*storing array\[int\].*array\[array\[int\]\]' \
    "$tmp/array_initializer_rank_mismatch.out"

"$compiler" parse --dump-tree \
    "$script_dir/programs/array_new_rectangular.n00b" \
    >"$tmp/array_new_rectangular.tree" 2>&1 || {
    cat "$tmp/array_new_rectangular.tree"
    exit 1
}
[ "$(grep -c '<new-expr>' "$tmp/array_new_rectangular.tree")" -eq 2 ]
if grep -A2 '<new-expr>' "$tmp/array_new_rectangular.tree" | grep -q '<tagged-lit>'; then
    echo 'new[T](...) was parsed as a literal modifier' >&2
    exit 1
fi

"$compiler" lower --dump-ir --dump-link-unit \
    "$script_dir/programs/array_new_sized.n00b" \
    >"$tmp/array_new_sized.out" 2>&1 || {
    cat "$tmp/array_new_sized.out"
    exit 1
}
grep -Eq 'array-bound:[0-9]+:.*:0' "$tmp/array_new_sized.out"
grep -Eq 'array.new .*%__n00bb_array_bound_result_' "$tmp/array_new_sized.out"
grep -Eq 'array.new %__n00bb_array_bound_result_[0-9]+, %__n00bb_array_extent_[0-9]+' \
    "$tmp/array_new_sized.out"
grep -Eq 'array.new %__n00bb_array_extent_[0-9]+, %__n00bb_array_extent_[0-9]+' \
    "$tmp/array_new_sized.out"
awk '
    /^module array_new_sized\$comptime \{/ { phase = "comptime"; next }
    /^module array_new_sized \{/ { phase = "runtime"; next }
    /= array.new/ { count[phase]++ }
    END { exit !(count["comptime"] == 5 && count["runtime"] == 4) }
' "$tmp/array_new_sized.out"
test "$(grep -c '!ir.array.element_initialization("language_zero")' \
    "$tmp/array_new_sized.out")" -eq 9
grep -Eq 'fixed .*array-shape\[rank=1,flat-prefix=1,dim0=comptime:producer=' \
    "$tmp/array_new_sized.out"
grep -Eq 'rectangular .*array-shape\[rank=2,flat-prefix=1,dim0=comptime:producer=.*dim1=unresolved' \
    "$tmp/array_new_sized.out"

"$compiler" lower --dump-ir \
    "$script_dir/programs/array_new_runtime_extent.n00b" \
    >"$tmp/array_new_runtime_extent.ir" \
    2>"$tmp/array_new_runtime_extent.err" || {
    cat "$tmp/array_new_runtime_extent.err"
    exit 1
}
grep -Eq ': uint = cast trap %count' "$tmp/array_new_runtime_extent.ir"
grep -Eq ': uint = sub trap %__n00bb_array_extent_[0-9]+,' \
    "$tmp/array_new_runtime_extent.ir"
grep -Eq '= array.new %__n00bb_array_extent_[0-9]+' \
    "$tmp/array_new_runtime_extent.ir"

N00B_PATH="$program_dir" "$compiler" lower \
    --dump-symbols --dump-link-unit \
    "$program_dir/array_shape_cycle_a.n00b" \
    >"$tmp/array_shape_cycle.out" 2>"$tmp/array_shape_cycle.err" || {
    cat "$tmp/array_shape_cycle.err"
    cat "$tmp/array_shape_cycle.out"
    exit 1
}
grep -Eq 'outer_grid:.*array-shape=rank:2' "$tmp/array_shape_cycle.out"
grep -Eq 'ragged_rows:.*array-shape=rank:2' "$tmp/array_shape_cycle.out"
grep -Eq 'array_shape_cycle_a:0:outer_grid .*binding ir.array-shape.*facts\[dimensions=2' \
    "$tmp/array_shape_cycle.out"
grep -Eq 'array_shape_cycle_b:0:ragged_rows .*binding ir.array-shape.*facts\[dimensions=2' \
    "$tmp/array_shape_cycle.out"

link_records="$script_dir/programs/link_records"
if N00B_PATH="$link_records" "$compiler" library \
    --dump-ir --dump-link-unit --jobs 4 \
    "$link_records/shared_link_first.n00b" \
    "$link_records/shared_link_second.n00b" \
    >"$tmp/shared_link.ir" 2>"$tmp/shared_link.err"; then
    :
else
    grep -q "'library' processing is not implemented yet" \
        "$tmp/shared_link.err" || {
        cat "$tmp/shared_link.err"
        exit 1
    }
fi
grep -q 'call @shared_link_value' "$tmp/shared_link.ir"
grep -q 'first_root' "$tmp/shared_link.ir"
grep -q 'second_root' "$tmp/shared_link.ir"
grep -q '^module shared_link_dep.impl\$comptime {' "$tmp/shared_link.ir"
grep -q '^module shared_link_dep.impl {' "$tmp/shared_link.ir"
test "$(grep -c 'func @shared_link_value' "$tmp/shared_link.ir")" -eq 1
test "$(grep -c 'func @__n00bb_comptime_shared_link_value' \
    "$tmp/shared_link.ir")" -eq 1
grep -q 'n00bb.call-graph v1 complete' "$tmp/shared_link.ir"
grep -q 'resolved shared_link_first -> shared_link_dep.impl:1:shared_link_value' \
    "$tmp/shared_link.ir"
grep -q 'shared_link_dep.impl:0:SharedLinkContext kind=' "$tmp/shared_link.ir"
grep -q 'call\[resolved\] .*shared_link_first:1:first_root -> shared_link_dep.impl:1:shared_link_value' \
    "$tmp/shared_link.ir"
grep -q 'call\[resolved\] .*shared_link_second:1:second_root -> shared_link_dep.impl:1:shared_link_value' \
    "$tmp/shared_link.ir"
grep -q 'binding n00bb.ir.function' "$tmp/shared_link.ir"
test "$(grep -c '^    initializer-use\[resolved\] shared_link_second:0:shared_link_copy -> shared_link_dep.impl:1:shared_link_value' \
    "$tmp/shared_link.ir")" -eq 1
grep -q 'initializer-use\[resolved\] shared_link_second:0:shared_link_copy -> shared_link_dep.impl:0:shared_link_global' \
    "$tmp/shared_link.ir"
! grep -q 'builtins:has_type' "$tmp/shared_link.ir"

N00B_PATH="$link_records" "$compiler" lower --dump-ir --dump-link-unit \
    "$link_records/shared_context_user.n00b" \
    >"$tmp/shared_context_user.out"
grep -q 'shared_link_dep.impl:0:SharedLinkContext kind=' \
    "$tmp/shared_context_user.out"
grep -q 'resolved shared_context_user -> shared_link_dep.impl:0:SharedLinkContext' \
    "$tmp/shared_context_user.out"
grep -q 'shared_link_dep.impl:0:SharedLinkContext kind=.*binding n00bb.ir.context.enter' \
    "$tmp/shared_context_user.out"
grep -q 'shared_link_dep.impl:0:SharedLinkContext kind=.*binding n00bb.ir.context.ensure' \
    "$tmp/shared_context_user.out"
grep -q 'context-call\[resolved\] .*shared_context_user -> shared_link_dep.impl:0:SharedLinkContext' \
    "$tmp/shared_context_user.out"
grep -q 'n00bb.call-graph v1 complete' "$tmp/shared_context_user.out"
grep -q 'call @__n00bb_context_SharedLinkContext_enter' \
    "$tmp/shared_context_user.out"
grep -q 'function-global-read-summary\[resolved\] shared_context_user:1:shared_context_user -> shared_link_dep.impl:0:shared_context_global' \
    "$tmp/shared_context_user.out"
grep -q 'function-global-write-summary\[resolved\] shared_context_user:1:shared_context_user -> shared_link_dep.impl:0:shared_context_global' \
    "$tmp/shared_context_user.out"

"$compiler" preprocess --dump-symbols --dump-link-unit \
    "$script_dir/programs/link_records/builtin_has_type.n00b" \
    >"$tmp/builtin_has_type.out"
grep -q 'matches: variable, mutable, type=bool' \
    "$tmp/builtin_has_type.out"
grep -q 'namespace=builtin' "$tmp/builtin_has_type.out"
grep -q 'builtins:has_type kind=1' "$tmp/builtin_has_type.out"
grep -q 'resolved builtin_has_type -> builtins:has_type' \
    "$tmp/builtin_has_type.out"
grep -q 'call\[external\].*builtins:has_type' \
    "$tmp/builtin_has_type.out"
! grep -q 'function-global-read\[resolved\].*SharedLinkContext' \
    "$tmp/shared_context_user.out"

"$compiler" preprocess --dump-link-unit "$program_dir/local_link_refs.n00b" \
    >"$tmp/local_link_refs.out"
grep -q 'resolved local_link_refs -> local_link_refs:1:local_link_target' \
    "$tmp/local_link_refs.out"

if N00B_PATH="$program_dir" "$compiler" parse --jobs 129 \
    "$program_dir/parallel_first.n00b" >"$tmp/parallel_bad_jobs.out" 2>&1; then
    echo 'out-of-range worker count unexpectedly succeeded' >&2
    exit 1
fi
grep -q -- '--jobs must be between 0 and 128' "$tmp/parallel_bad_jobs.out"

"$compiler" preprocess --dump-symbols "$program_dir/typecase_narrowing.n00b" \
    >"$tmp/typecase_narrowing.out" 2>"$tmp/typecase_narrowing.err" || {
    cat "$tmp/typecase_narrowing.err"
    exit 1
}
"$compiler" lower --dump-ir "$program_dir/typecase_narrowing.n00b" \
    >"$tmp/typecase_narrowing.ir" 2>"$tmp/typecase_narrowing.lower.err" || {
    cat "$tmp/typecase_narrowing.lower.err"
    exit 1
}
grep -q 'type.test %item, int' "$tmp/typecase_narrowing.ir"
grep -q 'type.narrow %item, int' "$tmp/typecase_narrowing.ir"
grep -q 'add trap %__n00bb_narrow_' "$tmp/typecase_narrowing.ir"

"$compiler" lower --dump-ir "$program_dir/typecase_multiple_types.n00b" \
    >"$tmp/typecase_multiple_types.ir" 2>"$tmp/typecase_multiple_types.err" || {
    cat "$tmp/typecase_multiple_types.err"
    exit 1
}
grep -q 'type.test %item, (int | string)' \
    "$tmp/typecase_multiple_types.ir"

"$compiler" lower --dump-ir "$program_dir/typecase_compound_types.n00b" \
    >"$tmp/typecase_compound_types.ir" \
    2>"$tmp/typecase_compound_types.err" || {
    cat "$tmp/typecase_compound_types.err"
    cat "$tmp/typecase_compound_types.ir"
    exit 1
}
grep -q 'type.test .*atomic\[int\]' \
    "$tmp/typecase_compound_types.ir"
grep -q 'type.test .*array\[int\]' \
    "$tmp/typecase_compound_types.ir"

"$compiler" lower --dump-ir "$program_dir/typecase_function_type.n00b" \
    >"$tmp/typecase_function_type.ir" \
    2>"$tmp/typecase_function_type.err" || {
    cat "$tmp/typecase_function_type.err"
    cat "$tmp/typecase_function_type.ir"
    exit 1
}
grep -q 'type.test .*[(]int, string[)] -> bool fail: string' \
    "$tmp/typecase_function_type.ir"

"$compiler" lower --dump-ir "$program_dir/typecase_function_params.n00b" \
    >"$tmp/typecase_function_params.ir" \
    2>"$tmp/typecase_function_params.err" || {
    cat "$tmp/typecase_function_params.err"
    cat "$tmp/typecase_function_params.ir"
    exit 1
}
grep -Fq '(int, *string, flags: array[int], options: bool) -> nil' \
    "$tmp/typecase_function_params.ir"
grep -Fq '() -> bool' \
    "$tmp/typecase_function_params.ir"

N00B_PATH="$program_dir" "$compiler" lower --dump-ir --dump-control-flow \
    "$program_dir/typecase_else_and_break.n00b" \
    >"$tmp/typecase_else_and_break.out" \
    2>"$tmp/typecase_else_and_break.err" || {
    cat "$tmp/typecase_else_and_break.out"
    cat "$tmp/typecase_else_and_break.err"
    exit 1
}
grep -q 'type.test %item, int' "$tmp/typecase_else_and_break.out"
grep -q 'type.narrow %item, int' "$tmp/typecase_else_and_break.out"
grep -q 'typecase_join' "$tmp/typecase_else_and_break.out"

if "$compiler" preprocess "$program_dir/typecase_nonvariable.n00b" \
    >"$tmp/typecase_nonvariable.out" 2>&1; then
    echo 'typecase with a non-variable subject unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'typecase requires a variable subject' \
    "$tmp/typecase_nonvariable.out"

if "$compiler" lower "$program_dir/typecase_nonexhaustive.n00b" \
    >"$tmp/typecase_nonexhaustive.out" 2>&1; then
    echo 'non-exhaustive typecase unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'typecase without else must cover every subject type' \
    "$tmp/typecase_nonexhaustive.out"

"$compiler" lower --dump-ir "$program_dir/typecase_value.n00b" \
    >"$tmp/typecase_value.ir" 2>"$tmp/typecase_value.err" || {
    cat "$tmp/typecase_value.err"
    exit 1
}
grep -q 'func @bare_typecase' "$tmp/typecase_value.ir"
grep -q 'type.narrow %item, int' "$tmp/typecase_value.ir"
grep -q '__n00bb_typecase_value_' "$tmp/typecase_value.ir"
grep -q '__n00bb_typecase_else_' "$tmp/typecase_value.ir"

if "$compiler" lower "$program_dir/typecase_value_nonexhaustive.n00b" \
    >"$tmp/typecase_value_nonexhaustive.out" 2>&1; then
    echo 'non-exhaustive value typecase unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'typecase without else must cover every subject type' \
    "$tmp/typecase_value_nonexhaustive.out"

"$compiler" lower "$program_dir/typecase_redundant.n00b" \
    >"$tmp/typecase_redundant.out" 2>&1
test "$(grep -c 'N00BB-W0006' "$tmp/typecase_redundant.out")" -eq 1

"$compiler" parse --dump-symbols "$program_dir/context_signature.n00b" \
    >"$tmp/context_signature.symbols" 2>"$tmp/context_signature.err" || {
    cat "$tmp/context_signature.err"
    exit 1
}
grep -q 'TestContext: variable, mutable, type=(int) -> int fail: int' \
    "$tmp/context_signature.symbols"
"$compiler" lower "$program_dir/context_signature.n00b" \
    >"$tmp/context_signature.ir" 2>"$tmp/context_signature.lower.err" || {
    cat "$tmp/context_signature.lower.err"
    exit 1
}
grep -E -q '@__n00bb_context_TestContext_enter\(%resource: int !ir.binding_phases\([0-9]+\).*\) -> int fail: int' \
    "$tmp/context_signature.ir"
grep -E -q '@__n00bb_context_TestContext_exit\(%resource: int !ir.binding_phases\([0-9]+\).*%guard: int !ir.binding_phases\([0-9]+\)' \
    "$tmp/context_signature.ir"
grep -E -q '@__n00bb_context_TestContext_error\(%resource: int !ir.binding_phases\([0-9]+\).*%err: int !ir.binding_phases\([0-9]+\)' \
    "$tmp/context_signature.ir"
grep -E -q '@__n00bb_context_TestContext_ensure\(%resource: int !ir.binding_phases\([0-9]+\).*%guard: int !ir.binding_phases\([0-9]+\)' \
    "$tmp/context_signature.ir"

"$compiler" lower --dump-ir "$program_dir/context_variadic.n00b" \
    >"$tmp/context_variadic.ir" 2>"$tmp/context_variadic.err" || {
    cat "$tmp/context_variadic.err"
    exit 1
}
grep -E -q '@__n00bb_context_Batch_enter\(%values: array\[int\] !ir.binding_phases\([0-9]+\).*\) -> array\[int\]' \
    "$tmp/context_variadic.ir"
grep -q '= aggregate %__lowered_' "$tmp/context_variadic.ir"
grep -q 'call @__n00bb_context_Batch_enter, %__n00bb_context_rest_' \
    "$tmp/context_variadic.ir"

"$compiler" lower --dump-ir "$program_dir/lower_context_with.n00b" \
    >"$tmp/lower_context_with.ir" 2>"$tmp/lower_context_with.err" || {
    cat "$tmp/lower_context_with.err"
    exit 1
}
grep -q 'call @__n00bb_context_Recorder_enter' \
    "$tmp/lower_context_with.ir"
grep -q 'call @__n00bb_context_Recorder_exit' \
    "$tmp/lower_context_with.ir"
grep -q 'call @__n00bb_context_Recorder_ensure' \
    "$tmp/lower_context_with.ir"

"$compiler" lower --dump-ir "$program_dir/context_defaults.n00b" \
    >"$tmp/context_defaults.ir" 2>"$tmp/context_defaults.err" || {
    cat "$tmp/context_defaults.err"
    exit 1
}
grep -q 'call @__n00bb_context_DefaultedContext_enter' \
    "$tmp/context_defaults.ir"
awk '
    /^  (static )?func @__n00bb_context_DefaultedContext_default_[12]\(/ {
        in_default = 1
    }
    /^  (static )?func @/ && in_default \
        && !/DefaultedContext_default_[12]\(/ { in_default = 0 }
    in_default && /= add trap/ { adds++ }
    END { exit adds != 2 }
' "$tmp/context_defaults.ir"
test "$(grep -c 'call @__n00bb_context_DefaultedContext_enter' \
    "$tmp/context_defaults.ir")" -eq 2
awk '
    /^  (static )?func @context_default_order\(/ { in_call = 1; next }
    /^  (static )?func @/ && in_call { exit }
    in_call && /call @__n00bb_context_OrderedContext_default_0/ { a = NR }
    in_call && /call @__n00bb_context_OrderedContext_default_1/ { b = NR }
    in_call && /call @__n00bb_context_OrderedContext_default_2/ { c = NR }
    END { exit !(b && a && c && b < a && a < c) }
' "$tmp/context_defaults.ir"
"$compiler" lower --dump-ir \
    "$program_dir/context_default_depth_first.n00b" \
    >"$tmp/context_default_depth_first.ir" \
    2>"$tmp/context_default_depth_first.err" || {
    cat "$tmp/context_default_depth_first.err"
    exit 1
}
awk '
    /^  (static )?func @context_default_depth_first\(/ { in_call = 1; next }
    /^  (static )?func @/ && in_call { exit }
    in_call && /call @__n00bb_context_DepthFirstContext_default_3/ && !d { d = NR }
    in_call && /call @__n00bb_context_DepthFirstContext_default_2/ && !c { c = NR }
    in_call && /call @__n00bb_context_DepthFirstContext_default_0/ && !a { a = NR }
    in_call && /call @__n00bb_context_DepthFirstContext_default_1/ && !b { b = NR }
    END { exit !(d && c && a && b && d < c && c < a && a < b) }
' "$tmp/context_default_depth_first.ir"
grep -q 'call @__n00bb_context_QuietRecorder_ensure' \
    "$tmp/lower_context_with.ir"
! grep -q '@__n00bb_context_QuietRecorder_error' \
    "$tmp/lower_context_with.ir"
grep -q '^  func @context_failure() -> int fail: int' \
    "$tmp/lower_context_with.ir"

"$compiler" lower --dump-ir --dump-link-unit \
    "$program_dir/lower_branch_liveness.n00b" \
    >"$tmp/lower_branch_liveness.out" 2>"$tmp/lower_branch_liveness.err" || {
    cat "$tmp/lower_branch_liveness.err"
    exit 1
}
grep -q 'n00bb.cfg-liveness v1 complete' "$tmp/lower_branch_liveness.out"
grep -q 'cfg-liveness\[symbols=2,ranges=2,complete-ranges=2,starts=2,end-points=[1-9][0-9]*,end-edges=[1-9][0-9]*,missing-starts=0,edges=.*direct-live=[1-9][0-9]*,direct-dead=[1-9][0-9]*,.*range-ends=[1-9][0-9]*,' \
    "$tmp/lower_branch_liveness.out"
grep -q 'n00bb.storage-lifetimes v1 complete' \
    "$tmp/lower_branch_liveness.out"
grep -q 'n00bb.finalization v1 complete' \
    "$tmp/lower_branch_liveness.out"
grep -q 'cleanup\[ranges=2,complete-ranges=2,cfg-edges=[1-9][0-9]*,endpoints=.*safe-exits=4,live-exits=0,unknown-exits=0,.*complete\]' \
    "$tmp/lower_branch_liveness.out"
if grep -E -q '^[[:space:]]*(finalize|dealloc) ' \
    "$tmp/lower_branch_liveness.out"; then
    echo 'cleanup analysis inserted an unspecified finalizer or raw release' >&2
    exit 1
fi

"$compiler" lower --dump-link-unit \
    "$program_dir/lower_context_liveness.n00b" \
    >"$tmp/lower_context_liveness.out" \
    2>"$tmp/lower_context_liveness.err" || {
    cat "$tmp/lower_context_liveness.err"
    exit 1
}
grep -q 'n00bb.cfg-liveness v1 complete' \
    "$tmp/lower_context_liveness.out"
grep -E -q 'LocalHookValues kind=.*cfg-liveness\[symbols=6,ranges=6,complete-ranges=6,starts=6,end-points=[1-9][0-9]*,end-edges=0,missing-starts=0,edges=6,.*complete\]' \
    "$tmp/lower_context_liveness.out"
grep -q 'n00bb.storage-lifetimes v1 complete' \
    "$tmp/lower_context_liveness.out"
grep -q 'n00bb.finalization v1 complete' \
    "$tmp/lower_context_liveness.out"
grep -E -q 'LocalHookValues kind=.*cleanup\[ranges=6,complete-ranges=6,cfg-edges=6,endpoints=6,exits=6,safe-exits=6,live-exits=0,unknown-exits=0,.*complete\]' \
    "$tmp/lower_context_liveness.out"

"$compiler" lower --dump-link-unit \
    "$program_dir/lower_storage_empty.n00b" \
    >"$tmp/lower_storage_empty.out" \
    2>"$tmp/lower_storage_empty.err" || {
    cat "$tmp/lower_storage_empty.err"
    cat "$tmp/lower_storage_empty.out"
    exit 1
}
grep -q 'n00bb.storage-lifetimes v1 complete' \
    "$tmp/lower_storage_empty.out"

"$compiler" lower --dump-link-unit \
    "$program_dir/incomplete_storage_lifetime.n00b" \
    >"$tmp/local_storage_lifetime.out" \
    2>"$tmp/local_storage_lifetime.err" || {
    cat "$tmp/local_storage_lifetime.err"
    cat "$tmp/local_storage_lifetime.out"
    exit 1
}
grep -q 'n00bb.storage-lifetimes v1 complete' \
    "$tmp/local_storage_lifetime.out"

"$compiler" lower --dump-symbols \
    "$program_dir/lower_context_liveness.n00b" \
    >"$tmp/lower_context_liveness.symbols" \
    2>"$tmp/lower_context_liveness.symbols.err" || {
    cat "$tmp/lower_context_liveness.symbols.err"
    exit 1
}
grep -E -q 'entered: variable, mutable, .*storage-duration=automatic' \
    "$tmp/lower_context_liveness.symbols"
grep -E -q 'released: variable, mutable, .*storage-duration=automatic' \
    "$tmp/lower_context_liveness.symbols"
grep -E -q 'extra: variable, mutable, .*storage-duration=automatic' \
    "$tmp/lower_context_liveness.symbols"

"$compiler" lower --dump-link-unit \
    "$program_dir/lower_reference_values.n00b" \
    >"$tmp/lower_reference_values.out" \
    2>"$tmp/lower_reference_values.err" || {
    cat "$tmp/lower_reference_values.err"
    exit 1
}
grep -E -q 'borrow_local kind=.*cfg-liveness\[.*alias-unknown=[1-9][0-9]*,' \
    "$tmp/lower_reference_values.out"
grep -E -q 'borrow_local kind=.*cfg-liveness\[.*end-points=0,end-edges=0,.*alias-unknown=[1-9][0-9]*,' \
    "$tmp/lower_reference_values.out"
grep -q 'n00bb.finalization v1 unavailable' \
    "$tmp/lower_reference_values.out"
grep -E -q 'borrow_local kind=.*cleanup\[.*unknown-exits=[1-9][0-9]*,.*incomplete\]' \
    "$tmp/lower_reference_values.out"

"$compiler" lower --dump-symbols --dump-link-unit \
    "$program_dir/lower_local_alias.n00b" \
    >"$tmp/lower_local_alias.out" 2>"$tmp/lower_local_alias.err" || {
    cat "$tmp/lower_local_alias.err"
    exit 1
}
grep -E -q 'target: variable, mutable, .*aliases=0, aliased-by=1, .*alias-analysis=complete' \
    "$tmp/lower_local_alias.out"
grep -E -q 'borrowed: variable, mutable, .*aliases=1, aliased-by=0, .*alias-analysis=complete' \
    "$tmp/lower_local_alias.out"
grep -E -q 'local_alias kind=.*cfg-liveness\[symbols=2,ranges=2,complete-ranges=2,starts=2,end-points=[0-9][0-9]*,end-edges=[0-9][0-9]*,missing-starts=0,.*alias-dead=[1-9][0-9]*,alias-unknown=0,' \
    "$tmp/lower_local_alias.out"

"$compiler" lower --dump-symbols \
    "$program_dir/forward_type_alias.n00b" \
    >"$tmp/forward_type_alias.out" 2>"$tmp/forward_type_alias.err" || {
    cat "$tmp/forward_type_alias.err"
    cat "$tmp/forward_type_alias.out"
    exit 1
}
grep -E -q 'First: type, .*type=int' "$tmp/forward_type_alias.out"
grep -E -q 'Later: type, .*type=int' "$tmp/forward_type_alias.out"
grep -E -q 'input: parameter, .*type=int' "$tmp/forward_type_alias.out"

"$compiler" lower --dump-symbols \
    "$program_dir/deferred_type_shapes.n00b" \
    >"$tmp/deferred_type_shapes.out" 2>"$tmp/deferred_type_shapes.err" || {
    cat "$tmp/deferred_type_shapes.err"
    cat "$tmp/deferred_type_shapes.out"
    exit 1
}
grep -E -q 'item: variable, .*type=int' "$tmp/deferred_type_shapes.out"
grep -E -q 'LaterList: type, .*type=List\[int\]' \
    "$tmp/deferred_type_shapes.out"
grep -E -q 'input: parameter, .*type=List\[int\]' \
    "$tmp/deferred_type_shapes.out"

if "$compiler" lower "$program_dir/unknown_type_reference.n00b" \
    >"$tmp/unknown_type_reference.out" 2>&1; then
    echo 'unknown local type name unexpectedly lowered' >&2
    exit 1
fi
grep -Fq "error[N00BB-T0007]: no visible type named 'DoesNotExist'" \
    "$tmp/unknown_type_reference.out"

awk '
    /^  func @context_enter_failure/ { in_enter_failure = 1 }
    /^  static func @__n00bb_context_/ { in_enter_failure = 0 }
    in_enter_failure && /^    block __n00bb_enter_error_/ { in_enter_error = 1 }
    in_enter_failure && /^    block / && !/__n00bb_enter_error_/ { in_enter_error = 0 }
    in_enter_error && /call @__n00bb_context_QuietRecorder_ensure/ { prior_ensure = 1 }
    in_enter_error && /call @__n00bb_context_FailingEnter_(error|ensure)/ { failed_cleanup = 1 }
    in_enter_error && /fail %/ { propagated = 1 }
    END { exit !(prior_ensure && !failed_cleanup && propagated) }
' "$tmp/lower_context_with.ir"
awk '
    /^  func @context_return/ { in_return = 1 }
    /^  func @context_fallthrough/ { in_return = 0 }
    in_return && /call @__n00bb_context_Recorder_exit/ { exits++ }
    in_return && /return %/ { returned = 1 }
    END { exit !(exits == 2 && returned) }
' "$tmp/lower_context_with.ir"

"$compiler" lower --dump-ir --dump-link-unit \
    "$program_dir/lower_context_hook_failures.n00b" \
    >"$tmp/lower_context_hook_failures.ir" \
    2>"$tmp/lower_context_hook_failures.err" || {
    cat "$tmp/lower_context_hook_failures.err"
    exit 1
}
grep -q 'n00bb.finalization v1 complete' \
    "$tmp/lower_context_hook_failures.ir"
grep -Eq 'cleanup\[.*unknown-exits=0,.*complete\]' \
    "$tmp/lower_context_hook_failures.ir"
awk '
    /^  func @recover_exit_error/ { in_recover = 1 }
    /^  static func @__n00bb_context_/ { in_recover = 0 }
    in_recover && /call @__n00bb_context_RecoverExit_error/ { handled++ }
    in_recover && /call @__n00bb_context_RecoverExit_ensure/ { ensured++ }
    in_recover && /fail %/ { propagated++ }
    END { exit !(handled == 1 && ensured == 3 && propagated == 3) }
' "$tmp/lower_context_hook_failures.ir" || {
    cat "$tmp/lower_context_hook_failures.ir"
    exit 1
}
awk '
    /^  func @propagate_error_hook_failure/ { in_handler = 1 }
    /^  func @recover_exit_error/ { in_handler = 0 }
    in_handler && /call @__n00bb_context_PropagatingExit_error/ { handled++ }
    in_handler && /call @__n00bb_context_PropagatingExit_ensure/ { ensured++ }
    in_handler && /fail %/ { propagated++ }
    END { exit !(handled == 1 && ensured == 3 && propagated == 3) }
' "$tmp/lower_context_hook_failures.ir"
awk '
    /^  func @unwind_outer_after_inner_exit_failure/ { in_nested = 1 }
    /^  func @propagate_unhandled_exit_failure/ { in_nested = 0 }
    in_nested && /call @__n00bb_context_RecoverExit_ensure/ { inner_ensures++ }
    in_nested && /call @__n00bb_context_OuterObserver_ensure/ { outer_ensures++ }
    in_nested && /call @__n00bb_context_OuterObserver_exit/ { outer_exits++ }
    END { exit !(inner_ensures == 3 && outer_ensures == 4 && outer_exits == 1) }
' "$tmp/lower_context_hook_failures.ir"
awk '
    /^  func @propagate_unhandled_exit_failure/ { in_unhandled = 1 }
    /^  func @propagate_error_hook_failure/ { in_unhandled = 0 }
    in_unhandled && /call @__n00bb_context_UnhandledExit_ensure/ { ensured++ }
    in_unhandled && /fail %/ { propagated++ }
    END { exit !(ensured == 2 && propagated == 2) }
' "$tmp/lower_context_hook_failures.ir"

"$compiler" lower --dump-ir "$program_dir/lower_context_body_failure.n00b" \
    >"$tmp/lower_context_body_failure.ir" \
    2>"$tmp/lower_context_body_failure.err" || {
    cat "$tmp/lower_context_body_failure.err"
    exit 1
}
awk '
    /^  func @propagate_body_failure/ { in_body = 1 }
    /^  static func @__n00bb_context_/ { in_body = 0 }
    in_body && /call @__n00bb_context_BodyFailure_error/ { error_calls++ }
    in_body && /call @__n00bb_context_BodyFailure_ensure/ { ensure_calls++ }
    in_body && /const 23/ { original_failure = 1 }
    END { exit !(error_calls == 0 && ensure_calls == 1 && original_failure) }
' "$tmp/lower_context_body_failure.ir"

if "$compiler" lower "$program_dir/lower_context_failing_ensure.n00b" \
    >"$tmp/lower_context_failing_ensure.out" 2>&1; then
    echo 'a fallible ensure hook unexpectedly lowered' >&2
    exit 1
fi
grep -q 'fail statement requires a non-nil function failure type' \
    "$tmp/lower_context_failing_ensure.out"

"$compiler" lower "$program_dir/lower_arithmetic.n00b" \
    >"$tmp/lower_arithmetic.ir" 2>"$tmp/lower_arithmetic.err" || {
    cat "$tmp/lower_arithmetic.err"
    exit 1
}
grep -q '@add' "$tmp/lower_arithmetic.ir"
grep -q '= add trap' "$tmp/lower_arithmetic.ir"
grep -q '= div trap trap' "$tmp/lower_arithmetic.ir"
grep -q '= rem trap trap' "$tmp/lower_arithmetic.ir"
grep -q 'return %' "$tmp/lower_arithmetic.ir"
"$compiler" lower "$program_dir/lower_float_negate.n00b" \
    >"$tmp/lower_float_negate.ir" 2>"$tmp/lower_float_negate.err" || {
    cat "$tmp/lower_float_negate.err"
    exit 1
}
grep -q ': int = neg trap ' "$tmp/lower_float_negate.ir"
grep -q ': f64 = neg %' "$tmp/lower_float_negate.ir"
"$compiler" lower --dump-control-flow --dump-pdg \
    "$program_dir/lower_arithmetic.n00b" >"$tmp/lower_arithmetic.analysis"
grep -q '^control-flow @add$' "$tmp/lower_arithmetic.analysis"
grep -q '^pdg @add ' "$tmp/lower_arithmetic.analysis"
grep -q 'data .* =>' "$tmp/lower_arithmetic.analysis"
! grep -q '^module lower_arithmetic' "$tmp/lower_arithmetic.analysis"
"$compiler" lower --dump-ir \
    "$program_dir/lower_float_arithmetic.n00b" \
    >"$tmp/lower_float_arithmetic.ir" \
    2>"$tmp/lower_float_arithmetic.err" || {
    cat "$tmp/lower_float_arithmetic.err"
    cat "$tmp/lower_float_arithmetic.ir"
    exit 1
}
grep -Eq 'func @add_float\(%left: f64 .*%right: f64 .*\) -> f64' \
    "$tmp/lower_float_arithmetic.ir"
grep -Eq 'func @add_f64\(%left: f64 .*%right: f64 .*\) -> f64' \
    "$tmp/lower_float_arithmetic.ir"
for function in add_float add_f64 add_float_literals; do
    awk -v fname="$function" '
        $0 ~ "^  func @" fname "\\(" { in_function = 1 }
        in_function && / = add / { add_seen = 1; if ($0 ~ / add trap /) trapped = 1 }
        in_function && /^  }/ { in_function = 0 }
        END { exit !(add_seen && !trapped) }
    ' "$tmp/lower_float_arithmetic.ir"
done
N00B_PATH="$interface_programs" "$compiler" library \
    --emit-interface "$tmp/float_alias_archive.n00bi" \
    float_alias_archive_dep.facade \
    >"$tmp/float_alias_archive_library.out" \
    2>"$tmp/float_alias_archive_library.err" || {
    cat "$tmp/float_alias_archive_library.err"
    exit 1
}
"$compiler" lower --link-interface "$tmp/float_alias_archive.n00bi" \
    --dump-ir --dump-link-unit \
    "$interface_programs/float_alias_archive_user.n00b" \
    >"$tmp/float_alias_archive_user.out" \
    2>"$tmp/float_alias_archive_user.err" || {
    cat "$tmp/float_alias_archive_user.err"
    cat "$tmp/float_alias_archive_user.out"
    exit 1
}
grep -Eq 'func @use_archive_add\(%left: f64 .*%right: f64 .*\) -> f64' \
    "$tmp/float_alias_archive_user.out"
grep -q 'call @archive_add' "$tmp/float_alias_archive_user.out"
"$compiler" lower --dump-ir --dump-pdg \
    "$program_dir/lower_arithmetic.n00b" >"$tmp/lower_arithmetic.ir_and_pdg"
grep -q '^module lower_arithmetic' "$tmp/lower_arithmetic.ir_and_pdg"
grep -q '^pdg @add ' "$tmp/lower_arithmetic.ir_and_pdg"
for command in build library compile; do
    if "$compiler" "$command" --dump-ir --dump-control-flow --dump-pdg \
        "$program_dir/lower_arithmetic.n00b" >"$tmp/$command.dumps" 2>&1; then
        echo "$command unexpectedly completed its unimplemented backend stage" >&2
        exit 1
    fi
    grep -q '^module lower_arithmetic' "$tmp/$command.dumps"
    grep -q '^control-flow @add$' "$tmp/$command.dumps"
    grep -q '^execution-graph:$' "$tmp/$command.dumps"
    grep -q '^pdg @add ' "$tmp/$command.dumps"
    grep -q "'$command' processing is not implemented yet" \
        "$tmp/$command.dumps"
done

"$compiler" lower "$program_dir/lower_value_return.n00b" \
    >"$tmp/lower_value_return.ir" 2>"$tmp/lower_value_return.err" || {
    cat "$tmp/lower_value_return.err"
    exit 1
}
grep -q 'return %' "$tmp/lower_value_return.ir"

if "$compiler" parse "$program_dir/lower_value_keyword_rejected.n00b" \
    >"$tmp/lower_value_keyword_rejected.out" \
    2>"$tmp/lower_value_keyword_rejected.err"; then
    echo 'retired value statement unexpectedly parsed' >&2
    exit 1
fi

"$compiler" lower "$program_dir/lower_failure_exit.n00b" \
    >"$tmp/lower_failure_exit.ir" 2>"$tmp/lower_failure_exit.err" || {
    cat "$tmp/lower_failure_exit.err"
    exit 1
}
grep -q 'fail: int' "$tmp/lower_failure_exit.ir"
grep -q 'fail %' "$tmp/lower_failure_exit.ir"

"$compiler" lower "$program_dir/lower_error_propagation.n00b" \
    >"$tmp/lower_error_propagation.ir" \
    2>"$tmp/lower_error_propagation.err" || {
    cat "$tmp/lower_error_propagation.err"
    exit 1
}
grep -q 'variant.tag' "$tmp/lower_error_propagation.ir"
grep -q 'type variant __lantern_call_outcome_' \
    "$tmp/lower_error_propagation.ir"
grep -q 'branch %' "$tmp/lower_error_propagation.ir"
grep -q 'variant.payload' "$tmp/lower_error_propagation.ir"
grep -q 'fail %' "$tmp/lower_error_propagation.ir"

if "$compiler" lower --dump-ir --dump-control-flow --dump-pdg \
    "$program_dir/lower_unhandled_error.n00b" \
    >"$tmp/lower_unhandled_error.out" \
    2>"$tmp/lower_unhandled_error.err"; then
    echo 'a fallible call without postfix ! unexpectedly lowered' >&2
    exit 1
fi
grep -q 'must be used with postfix !' "$tmp/lower_unhandled_error.err"
! grep -Eq '^(module |control-flow |pdg )' \
    "$tmp/lower_unhandled_error.out"

"$compiler" lower "$program_dir/lower_caught_error.n00b" \
    >"$tmp/lower_caught_error.ir" 2>"$tmp/lower_caught_error.err" || {
    cat "$tmp/lower_caught_error.err"
    exit 1
}
grep -q 'variant.tag' "$tmp/lower_caught_error.ir"
grep -q 'try_handler' "$tmp/lower_caught_error.ir"
! grep -q 'fail %' "$tmp/lower_caught_error.ir"
"$compiler" preprocess --dump-symbols \
    "$program_dir/lower_caught_error.n00b" \
    >"$tmp/lower_caught_error.symbols" 2>&1
grep -q 'err: variable, .*declaration=3:37, definitions=\[3:37\], use-sites=\[3:55\]' \
    "$tmp/lower_caught_error.symbols"

"$compiler" lower "$program_dir/lower_caught_fail.n00b" \
    >"$tmp/lower_caught_fail.ir" 2>"$tmp/lower_caught_fail.err" || {
    cat "$tmp/lower_caught_fail.err"
    exit 1
}
grep -q 'try_handler' "$tmp/lower_caught_fail.ir"
! grep -q 'fail %' "$tmp/lower_caught_fail.ir"

"$compiler" lower "$program_dir/lower_try_value.n00b" \
    >"$tmp/lower_try_value.ir" 2>"$tmp/lower_try_value.err" || {
    cat "$tmp/lower_try_value.err"
    exit 1
}
grep -q 'try_handler' "$tmp/lower_try_value.ir"
grep -q 'try_value_' "$tmp/lower_try_value.ir"
grep -q 'fail %' "$tmp/lower_try_value.ir"

"$compiler" lower "$program_dir/lower_nested_try.n00b" \
    >"$tmp/lower_nested_try.ir" 2>"$tmp/lower_nested_try.err" || {
    cat "$tmp/lower_nested_try.err"
    exit 1
}
[ "$(grep -c 'block __n00bb_try_handler' \
    "$tmp/lower_nested_try.ir")" -eq 2 ]

if "$compiler" lower "$program_dir/lower_mismatched_error.n00b" \
    >"$tmp/lower_mismatched_error.out" \
    2>"$tmp/lower_mismatched_error.err"; then
    echo 'error propagation across mismatched channels unexpectedly passed' >&2
    exit 1
fi
grep -q "must match the enclosing function's failure channel" \
    "$tmp/lower_mismatched_error.err"

"$compiler" lower "$program_dir/lower_value_block.n00b" \
    >"$tmp/lower_value_block.ir" 2>"$tmp/lower_value_block.err" || {
    cat "$tmp/lower_value_block.err"
    exit 1
}
grep -q '__n00bb_block_value_' "$tmp/lower_value_block.ir"
grep -q 'block_value_cont' "$tmp/lower_value_block.ir"

"$compiler" lower "$program_dir/lower_local_assignment.n00b" \
    >"$tmp/lower_local_assignment.ir" \
    2>"$tmp/lower_local_assignment.err" || {
    cat "$tmp/lower_local_assignment.err"
    exit 1
}
grep -q '__n00bb_local_' "$tmp/lower_local_assignment.ir"
grep -q '\$__n00bb_local_.* = copy' "$tmp/lower_local_assignment.ir"

"$compiler" lower "$program_dir/lower_compound_assignment.n00b" \
    >"$tmp/lower_compound_assignment.ir" \
    2>"$tmp/lower_compound_assignment.err" || {
    cat "$tmp/lower_compound_assignment.err"
    exit 1
}
grep -q 'add trap' "$tmp/lower_compound_assignment.ir"
grep -q 'sub trap' "$tmp/lower_compound_assignment.ir"
grep -q 'mul trap' "$tmp/lower_compound_assignment.ir"
grep -Eq 'div trap trap ' "$tmp/lower_compound_assignment.ir"
grep -Eq 'rem trap trap ' "$tmp/lower_compound_assignment.ir"
grep -q 'shl' "$tmp/lower_compound_assignment.ir"

"$compiler" lower "$program_dir/lower_bitwise_shifts.n00b" \
    >"$tmp/lower_bitwise_shifts.ir" \
    2>"$tmp/lower_bitwise_shifts.err" || {
    cat "$tmp/lower_bitwise_shifts.err"
    exit 1
}
grep -q ' and ' "$tmp/lower_bitwise_shifts.ir"
grep -q ' or ' "$tmp/lower_bitwise_shifts.ir"
grep -q ' xor ' "$tmp/lower_bitwise_shifts.ir"
grep -q ' shl trap ' "$tmp/lower_bitwise_shifts.ir"
grep -q ' shr trap ' "$tmp/lower_bitwise_shifts.ir"

"$compiler" lower "$program_dir/lower_bool_not.n00b" \
    >"$tmp/lower_bool_not.ir" 2>"$tmp/lower_bool_not.err" || {
    cat "$tmp/lower_bool_not.err"
    exit 1
}
grep -q ' eq %value, %' "$tmp/lower_bool_not.ir"

"$compiler" lower "$program_dir/lower_numeric_cast.n00b" \
    >"$tmp/lower_numeric_cast.ir" 2>"$tmp/lower_numeric_cast.err" || {
    cat "$tmp/lower_numeric_cast.err"
    exit 1
}
grep -q ': i128 = cast %value' "$tmp/lower_numeric_cast.ir"
grep -q ': i64 = cast trap %value' "$tmp/lower_numeric_cast.ir"

"$compiler" lower --dump-ir "$program_dir/lower_enum_cast.n00b" \
    >"$tmp/lower_enum_cast.ir" 2>"$tmp/lower_enum_cast.err" || {
    cat "$tmp/lower_enum_cast.err"
    exit 1
}
grep -q ': u8 = enum.value %' "$tmp/lower_enum_cast.ir"
grep -q ': ubitint\[1\] = enum.value trap %' "$tmp/lower_enum_cast.ir"
grep -q ': u8 = enum.value trap %' "$tmp/lower_enum_cast.ir"
grep -q 'shr' "$tmp/lower_compound_assignment.ir"
grep -q 'and' "$tmp/lower_compound_assignment.ir"
grep -q 'or' "$tmp/lower_compound_assignment.ir"
grep -q 'xor' "$tmp/lower_compound_assignment.ir"

"$compiler" lower --dump-ir \
    "$program_dir/lower_global_compound_assignment.n00b" \
    >"$tmp/lower_global_compound_assignment.ir" \
    2>"$tmp/lower_global_compound_assignment.err" || {
    cat "$tmp/lower_global_compound_assignment.err"
    exit 1
}
grep -q 'copy @lower_global_compound_assignment.total' \
    "$tmp/lower_global_compound_assignment.ir"
grep -q 'add trap %' \
    "$tmp/lower_global_compound_assignment.ir"
grep -q 'global.store @lower_global_compound_assignment.total' \
    "$tmp/lower_global_compound_assignment.ir"

"$compiler" lower --dump-ir "$program_dir/lower_compound_storage.n00b" \
    >"$tmp/lower_compound_storage.ir" \
    2>"$tmp/lower_compound_storage.err" || {
    cat "$tmp/lower_compound_storage.err"
    exit 1
}
grep -q 'field .*"value"' "$tmp/lower_compound_storage.ir"
grep -q 'add trap ' "$tmp/lower_compound_storage.ir"
grep -q '= call @__read,' "$tmp/lower_compound_storage.ir"
grep -q '= call @__write,' "$tmp/lower_compound_storage.ir"

"$compiler" lower --dump-ir "$program_dir/lower_tuple_assignment.n00b" \
    >"$tmp/lower_tuple_assignment.ir" \
    2>"$tmp/lower_tuple_assignment.err" || {
    cat "$tmp/lower_tuple_assignment.err"
    cat "$tmp/lower_tuple_assignment.ir"
    exit 1
}
test "$(grep -c '= aggregate' "$tmp/lower_tuple_assignment.ir")" -eq 2
grep -q 'store .*ir.binding_initialization(true)' \
    "$tmp/lower_tuple_assignment.ir"

if "$compiler" lower "$program_dir/lower_immutable_assignment.n00b" \
    >"$tmp/lower_immutable_assignment.ir" \
    2>"$tmp/lower_immutable_assignment.err"; then
    echo 'assignment to an immutable local unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'cannot assign to this binding during runtime execution' \
    "$tmp/lower_immutable_assignment.err"

"$compiler" lower "$program_dir/lower_phase_access.n00b" \
    >"$tmp/lower_phase_access.ir" \
    2>"$tmp/lower_phase_access.err" || {
    cat "$tmp/lower_phase_access.err"
    exit 1
}
grep -q '__n00bb_comptime_init' "$tmp/lower_phase_access.ir"

if "$compiler" lower "$program_dir/lower_runtime_read_in_comptime.n00b" \
    >"$tmp/lower_runtime_read_in_comptime.ir" \
    2>"$tmp/lower_runtime_read_in_comptime.err"; then
    echo 'compile-time read of a runtime-only binding unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'this binding is not available during compile-time evaluation' \
    "$tmp/lower_runtime_read_in_comptime.err"

"$compiler" lower --dump-ir \
    "$program_dir/lower_comptime_control.n00b" \
    >"$tmp/lower_comptime_control.ir" \
    2>"$tmp/lower_comptime_control.err" || {
    cat "$tmp/lower_comptime_control.err"
    exit 1
}
awk '
    /^  static func @__n00bb_comptime_init/ { in_comptime = 1 }
    /^  static func @__n00bb_runtime_init/ { in_comptime = 0 }
    in_comptime && /branch / { comptime_branch = 1 }
    !in_comptime && /branch / { runtime_branch = 1 }
    END { exit !(comptime_branch && !runtime_branch) }
' "$tmp/lower_comptime_control.ir"

"$compiler" lower "$program_dir/lower_const_comptime_write.n00b" \
    >"$tmp/lower_const_comptime_write.ir" \
    2>"$tmp/lower_const_comptime_write.err" || {
    cat "$tmp/lower_const_comptime_write.err"
    exit 1
}
grep -q '__n00bb_comptime_init' "$tmp/lower_const_comptime_write.ir"

if ! N00B_PATH="$program_dir" "$compiler" lower --dump-ir \
    --dump-control-flow --dump-link-unit \
    initializer_comptime_const_client \
    >"$tmp/initializer_comptime_const.out" \
    2>"$tmp/initializer_comptime_const.err"; then
    cat "$tmp/initializer_comptime_const.err"
    cat "$tmp/initializer_comptime_const.out"
    exit 1
fi
grep -Fq 'const global @initializer_comptime_const_dep.scalar_value: int = $"' \
    "$tmp/initializer_comptime_const.out"
grep -Fq ':global-state:scalar_value"' \
    "$tmp/initializer_comptime_const.out"
grep -Fq 'const global @initializer_comptime_const_dep.pair_value:' \
    "$tmp/initializer_comptime_const.out"
grep -Fq 'global-state:scalar_value" phase="comptime" source=@initializer_comptime_const_dep$comptime' \
    "$tmp/initializer_comptime_const.out"
grep -Fq 'global-state:pair_value" phase="comptime" source=@initializer_comptime_const_dep$comptime' \
    "$tmp/initializer_comptime_const.out"
grep -Fq '= copy @initializer_comptime_const_dep.scalar_value' \
    "$tmp/initializer_comptime_const.out"
grep -Fq 'global.store @initializer_comptime_const_client.runtime_scalar, %__n00bb_' \
    "$tmp/initializer_comptime_const.out"
grep -Fq '= copy @initializer_comptime_const_dep.pair_value' \
    "$tmp/initializer_comptime_const.out"
grep -Fq 'global.store @initializer_comptime_const_client.runtime_pair, %__n00bb_' \
    "$tmp/initializer_comptime_const.out"
if "$compiler" lower \
    "$program_dir/initializer_comptime_const_runtime_write.n00b" \
    >"$tmp/initializer_comptime_const_runtime_write.out" 2>&1; then
    echo 'a runtime write to a const binding unexpectedly lowered' >&2
    exit 1
fi
grep -q 'cannot assign to this binding' \
    "$tmp/initializer_comptime_const_runtime_write.out" || {
    cat "$tmp/initializer_comptime_const_runtime_write.out"
    exit 1
}

if ! "$compiler" lower --dump-ir --dump-link-unit "$program_dir/lower_global.n00b" \
    >"$tmp/lower_global.ir" 2>"$tmp/lower_global.err"; then
    cat "$tmp/lower_global.err"
    cat "$tmp/lower_global.ir"
    exit 1
fi
grep -q 'static global @lower_global.global_value: int = 1' \
    "$tmp/lower_global.ir" || {
    cat "$tmp/lower_global.ir"
    exit 1
}
grep -q 'global.store @lower_global.global_value' "$tmp/lower_global.ir"
grep -q 'add trap @lower_global.global_value' "$tmp/lower_global.ir"
grep -q 'binding n00bb.ir.global' "$tmp/lower_global.ir"

"$compiler" lower --dump-link-unit "$program_dir/initializer_uses.n00b" \
    >"$tmp/initializer_uses.out" 2>"$tmp/initializer_uses.err" || {
    cat "$tmp/initializer_uses.err"
    cat "$tmp/initializer_uses.out"
    exit 1
}
grep -q 'n00bb.initializer-direct-uses v1 complete' \
    "$tmp/initializer_uses.out" || {
    cat "$tmp/initializer_uses.err"
    cat "$tmp/initializer_uses.out"
    exit 1
}
grep -q 'n00bb.initializer-phase-uses v1 complete' \
    "$tmp/initializer_uses.out"
grep -q 'n00bb.initializer-cycle-check v1 complete' \
    "$tmp/initializer_uses.out"
grep -q 'n00bb.initializer-dependencies v1 complete' \
    "$tmp/initializer_uses.out"
grep -q 'n00bb.comptime-module-initializer-order v1 complete' \
    "$tmp/initializer_uses.out"
grep -q 'n00bb.runtime-module-initializer-order v1 complete' \
    "$tmp/initializer_uses.out"
grep -q 'initializer-use\[resolved\] initializer_uses:0:dependency_target -> initializer_uses:0:dependency_source' \
    "$tmp/initializer_uses.out" || {
    cat "$tmp/initializer_uses.out"
    exit 1
}
grep -q 'runtime-initializer-use\[resolved\] initializer_uses:0:dependency_target -> initializer_uses:0:dependency_source' \
    "$tmp/initializer_uses.out"
grep -q 'comptime-initializer-use\[resolved\] <initializer>: -> initializer_uses:0:comptime_source' \
    "$tmp/initializer_uses.out"

"$compiler" preprocess --dump-link-unit \
    "$program_dir/link_declaration_facts.n00b" \
    >"$tmp/link_declaration_facts.out" \
    2>"$tmp/link_declaration_facts.err" || {
    cat "$tmp/link_declaration_facts.err"
    cat "$tmp/link_declaration_facts.out"
    exit 1
}
grep -q 'n00bb.phase-values v1 unavailable' \
    "$tmp/link_declaration_facts.out"
grep -q 'n00bb.semantic-ready v1 complete (n00bb.link-unit-semantic-analysis)' \
    "$tmp/link_declaration_facts.out"
grep -q 'n00bb.array-shapes v1 complete' \
    "$tmp/link_declaration_facts.out"
grep -q 'binding ir.array-shape' "$tmp/link_declaration_facts.out"
grep -q 'n00bb.storage-lifetimes v1 unavailable' \
    "$tmp/link_declaration_facts.out"
grep -q 'link_declaration_facts:0:matrix .*facts\[dimensions=2, initializers=0' \
    "$tmp/link_declaration_facts.out"
grep -q 'link_declaration_facts:0:values .*facts\[dimensions=1, initializers=1' \
    "$tmp/link_declaration_facts.out"
grep -q 'link_declaration_facts:0:values .*static-value=sequence:\[00000000000000000000000000000001,00000000000000000000000000000002\]' \
    "$tmp/link_declaration_facts.out"
"$compiler" preprocess --dump-link-unit \
    "$program_dir/sequence_computed_const.n00b" \
    >"$tmp/sequence_computed_const.out"
! grep -q 'values .*static-value=' "$tmp/sequence_computed_const.out"
grep -q 'link_declaration_facts:0:limit .*static-value=int:00000000000000000000000000000029' \
    "$tmp/link_declaration_facts.out"
grep -q 'link_declaration_facts:0:label .*static-value=string:13 bytes' \
    "$tmp/link_declaration_facts.out"
grep -q 'link_declaration_facts:0:empty_label .*static-value=string:0 bytes' \
    "$tmp/link_declaration_facts.out"
grep -q 'link_declaration_facts:0:computed .*facts\[dimensions=0, initializers=1' \
    "$tmp/link_declaration_facts.out"
! grep -q 'link_declaration_facts:0:computed .*static-value=' \
    "$tmp/link_declaration_facts.out"
grep -q 'initializer-phases=\[comptime\]' \
    "$tmp/link_declaration_facts.out"

"$compiler" preprocess --dump-link-unit \
    "$program_dir/initializer_cross_phase_read.n00b" \
    >"$tmp/initializer_cross_phase_read.out" \
    2>"$tmp/initializer_cross_phase_read.err" || {
    cat "$tmp/initializer_cross_phase_read.err"
    cat "$tmp/initializer_cross_phase_read.out"
    exit 1
}
grep -q 'n00bb.initializer-dependencies v1 unavailable' \
    "$tmp/initializer_cross_phase_read.out" || {
    cat "$tmp/initializer_cross_phase_read.out"
    exit 1
}

if "$compiler" preprocess --dump-link-unit \
    "$program_dir/initializer_cycle.n00b" \
    >"$tmp/initializer_cycle.out" 2>&1; then
    echo 'a dynamic global initializer cycle unexpectedly passed analysis' >&2
    exit 1
fi
grep -q 'runtime initializer dependency cycle requires reading an uninitialized global' \
    "$tmp/initializer_cycle.out" || {
    cat "$tmp/initializer_cycle.out"
    exit 1
}

if N00B_PATH="$program_dir" "$compiler" preprocess \
    initializer_cycle_import_a \
    >"$tmp/initializer_cycle_import.out" 2>&1; then
    echo 'a cross-module initializer cycle unexpectedly passed analysis' >&2
    exit 1
fi
grep -q 'runtime initializer dependency cycle requires reading an uninitialized global' \
    "$tmp/initializer_cycle_import.out" || {
    cat "$tmp/initializer_cycle_import.out"
    exit 1
}

if N00B_PATH="$program_dir" "$compiler" lower --dump-ir --dump-link-unit \
    initializer_uninitialized_import_a \
    >"$tmp/initializer_uninitialized_import.out" 2>&1; then
    echo 'an uninitialized imported global in an import cycle unexpectedly passed' >&2
    exit 1
fi
grep -q 'initialization in an import cycle reads a global without an explicit initializer' \
    "$tmp/initializer_uninitialized_import.out" || {
    cat "$tmp/initializer_uninitialized_import.out"
    exit 1
}
if grep -q 'n00bb.ir.module' "$tmp/initializer_uninitialized_import.out"; then
    echo 'IR binding ran after a fatal uninitialized import-cycle diagnostic' >&2
    cat "$tmp/initializer_uninitialized_import.out"
    exit 1
fi

if "$compiler" lower --dump-ir --dump-link-unit \
    "$program_dir/initializer_forward_read.n00b" \
    >"$tmp/initializer_forward_read.out" 2>&1; then
    echo 'a forward read of a dynamic global initializer unexpectedly lowered' >&2
    exit 1
fi
grep -q 'N00BB-L0043.*initializer reads a global before its initializer runs' \
    "$tmp/initializer_forward_read.out" || {
    cat "$tmp/initializer_forward_read.out"
    exit 1
}

if "$compiler" lower --dump-ir --dump-link-unit \
    "$program_dir/initializer_uninitialized_read.n00b" \
    >"$tmp/initializer_uninitialized_read.out" 2>&1; then
    echo 'a read of a global without an initializer unexpectedly lowered' >&2
    exit 1
fi
grep -q 'N00BB-L0044.*initializer reads a global without an explicit initializer' \
    "$tmp/initializer_uninitialized_read.out" || {
    cat "$tmp/initializer_uninitialized_read.out"
    exit 1
}

if ! N00B_PATH="$program_dir" "$compiler" lower --dump-ir \
    --dump-control-flow \
    initializer_comptime_write_a \
    >"$tmp/initializer_comptime_write.out" 2>&1; then
    cat "$tmp/initializer_comptime_write.out"
    exit 1
fi
grep -Fq 'global-result "'"$(printf '%s' "$program_dir")"':initializer_comptime_write_b:global-state:shared_value" phase="comptime" source=@initializer_comptime_write_b$comptime.initializer_comptime_write_b.shared_value target=@initializer_comptime_write_b.initializer_comptime_write_b.shared_value type=int' \
    "$tmp/initializer_comptime_write.out" || {
    cat "$tmp/initializer_comptime_write.out"
    exit 1
}
grep -q 'global.store @initializer_comptime_write_b.shared_value' \
    "$tmp/initializer_comptime_write.out" || {
    cat "$tmp/initializer_comptime_write.out"
    exit 1
}
grep -q 'global.store @initializer_comptime_write_a.copied_value' \
    "$tmp/initializer_comptime_write.out" || {
    cat "$tmp/initializer_comptime_write.out"
    exit 1
}
N00B_PATH="$program_dir" "$compiler" preprocess --dump-link-unit \
    initializer_comptime_write_a \
    >"$tmp/initializer_comptime_write_link.out" \
    2>"$tmp/initializer_comptime_write_link.err" || {
    cat "$tmp/initializer_comptime_write_link.err"
    cat "$tmp/initializer_comptime_write_link.out"
    exit 1
}
grep -q 'n00bb.initializer-dependencies v1 complete' \
    "$tmp/initializer_comptime_write_link.out" || {
    cat "$tmp/initializer_comptime_write_link.out"
    exit 1
}

if N00B_PATH="$program_dir" "$compiler" lower --dump-ir \
    initializer_comptime_conditional_write_a \
    >"$tmp/initializer_comptime_conditional_write.out" 2>&1; then
    echo 'a runtime read after a conditional compile-time write unexpectedly lowered' >&2
    exit 1
fi
grep -q 'N00BB-L0044.*without a guaranteed compile-time initialization' \
    "$tmp/initializer_comptime_conditional_write.out" || {
    cat "$tmp/initializer_comptime_conditional_write.out"
    exit 1
}
if grep -q 'n00bb.ir.module' \
    "$tmp/initializer_comptime_conditional_write.out"; then
    echo 'IR binding ran after an unproven compile-time initialization' >&2
    cat "$tmp/initializer_comptime_conditional_write.out"
    exit 1
fi

for program in \
    initializer_comptime_called_write \
    initializer_comptime_called_branch_write \
    initializer_comptime_context_hook_write \
    initializer_comptime_out_write \
    initializer_comptime_out_failure_path \
    initializer_comptime_reference_write \
    initializer_comptime_returned_reference_write \
    initializer_comptime_parameter_reference_write; do
    if ! "$compiler" lower --dump-ir --dump-control-flow \
        "$program_dir/$program.n00b" >"$tmp/$program.out" 2>&1; then
        cat "$tmp/$program.out"
        exit 1
    fi
    grep -q "global-result .*global-state:" \
        "$tmp/$program.out" || {
        cat "$tmp/$program.out"
        exit 1
    }
done
grep -q 'global-result .*global-state:first_value' \
    "$tmp/initializer_comptime_parameter_reference_write.out" || {
    cat "$tmp/initializer_comptime_parameter_reference_write.out"
    exit 1
}
grep -q 'global-result .*global-state:second_value' \
    "$tmp/initializer_comptime_parameter_reference_write.out" || {
    cat "$tmp/initializer_comptime_parameter_reference_write.out"
    exit 1
}
if ! N00B_PATH="$program_dir" "$compiler" lower --dump-ir \
    --dump-control-flow \
    initializer_comptime_imported_reference_write \
    >"$tmp/initializer_comptime_imported_reference_write.out" 2>&1; then
    cat "$tmp/initializer_comptime_imported_reference_write.out"
    exit 1
fi
grep -q 'global-result .*global-state:shared_value' \
    "$tmp/initializer_comptime_imported_reference_write.out" || {
    cat "$tmp/initializer_comptime_imported_reference_write.out"
    exit 1
}
if "$compiler" lower --dump-ir \
    "$program_dir/initializer_comptime_reference_conditional_write.n00b" \
    >"$tmp/initializer_comptime_reference_conditional_write.out" 2>&1; then
    echo 'a runtime read after a conditional reference write unexpectedly lowered' >&2
    exit 1
fi
grep -q 'N00BB-L0044.*without a guaranteed compile-time initialization' \
    "$tmp/initializer_comptime_reference_conditional_write.out" || {
    cat "$tmp/initializer_comptime_reference_conditional_write.out"
    exit 1
}
if ! awk '
    /block __n00bb_unwrap_ok_/ { in_ok = 1 }
    in_ok && /global.store @initializer_comptime_out_failure_path.shared_value/ {
        success_store = 1
    }
    /block __n00bb_unwrap_error_/ { in_ok = 0; in_error = 1 }
    in_error && /global.store @initializer_comptime_out_failure_path.shared_value/ {
        error_store = 1
    }
    END { exit !(success_store && !error_store) }
' "$tmp/initializer_comptime_out_failure_path.out"; then
    echo 'an out argument must copy back only on the successful call path' >&2
    cat "$tmp/initializer_comptime_out_failure_path.out"
    exit 1
fi

if ! "$compiler" lower --dump-ir --dump-control-flow \
    "$program_dir/initializer_comptime_replace_literal.n00b" \
    >"$tmp/initializer_comptime_replace_literal.out" 2>&1; then
    cat "$tmp/initializer_comptime_replace_literal.out"
    exit 1
fi
grep -Fq 'phase_value: int = $"default:initializer_comptime_replace_literal:global-state:phase_value"' \
    "$tmp/initializer_comptime_replace_literal.out" || {
    cat "$tmp/initializer_comptime_replace_literal.out"
    exit 1
}
grep -q 'global-result .*global-state:phase_value' \
    "$tmp/initializer_comptime_replace_literal.out" || {
    cat "$tmp/initializer_comptime_replace_literal.out"
    exit 1
}

"$compiler" lower --dump-ir "$program_dir/lower_modify_global.n00b" \
    >"$tmp/lower_modify_global.out" 2>&1 || {
    cat "$tmp/lower_modify_global.out"
    exit 1
}
grep -Fq 'global.store @lower_modify_global.shared_value' \
    "$tmp/lower_modify_global.out" || {
    cat "$tmp/lower_modify_global.out"
    exit 1
}

if "$compiler" lower --dump-ir \
    "$program_dir/initializer_comptime_called_conditional_write.n00b" \
    >"$tmp/initializer_comptime_called_conditional_write.out" 2>&1; then
    echo 'a runtime read after a conditional helper write unexpectedly lowered' >&2
    exit 1
fi
grep -q 'N00BB-L0044.*without a guaranteed compile-time initialization' \
    "$tmp/initializer_comptime_called_conditional_write.out" || {
    cat "$tmp/initializer_comptime_called_conditional_write.out"
    exit 1
}

if "$compiler" lower --dump-ir --dump-link-unit \
    "$program_dir/comptime_initializer_cycle.n00b" \
    >"$tmp/comptime_initializer_cycle.out" 2>&1; then
    echo 'a compile-time initializer cycle unexpectedly passed analysis' >&2
    exit 1
fi
grep -q 'compile-time initializer dependency cycle requires reading an uninitialized global' \
    "$tmp/comptime_initializer_cycle.out" || {
    cat "$tmp/comptime_initializer_cycle.out"
    exit 1
}
if grep -q 'n00bb.ir.module' "$tmp/comptime_initializer_cycle.out"; then
    echo 'lowering ran after a fatal compile-time initializer cycle' >&2
    cat "$tmp/comptime_initializer_cycle.out"
    exit 1
fi

"$compiler" preprocess --dump-link-unit \
    "$program_dir/initializer_indirect_effect.n00b" \
    >"$tmp/initializer_indirect_effect.out" \
    2>"$tmp/initializer_indirect_effect.err" || {
    cat "$tmp/initializer_indirect_effect.err"
    cat "$tmp/initializer_indirect_effect.out"
    exit 1
}
grep -q 'n00bb.initializer-direct-uses v1 complete' \
    "$tmp/initializer_indirect_effect.out" || {
    cat "$tmp/initializer_indirect_effect.out"
    exit 1
}
grep -q 'initializer-use\[resolved\] initializer_indirect_effect:0:reference_holder -> initializer_indirect_effect:0:referenced_global' \
    "$tmp/initializer_indirect_effect.out" || {
    cat "$tmp/initializer_indirect_effect.out"
    exit 1
}
grep -q 'function-global-alias\[resolved\] initializer_indirect_effect:1:reference_global -> initializer_indirect_effect:0:referenced_global' \
    "$tmp/initializer_indirect_effect.out" || {
    cat "$tmp/initializer_indirect_effect.out"
    exit 1
}
grep -q 'function-global-alias-summary\[resolved\] initializer_indirect_effect:1:reference_wrapper -> initializer_indirect_effect:0:referenced_global' \
    "$tmp/initializer_indirect_effect.out" || {
    cat "$tmp/initializer_indirect_effect.out"
    exit 1
}
grep -q 'function-global-alias-summary\[resolved\] initializer_indirect_effect:1:recursive_reference_relay -> initializer_indirect_effect:0:referenced_global' \
    "$tmp/initializer_indirect_effect.out" || {
    cat "$tmp/initializer_indirect_effect.out"
    exit 1
}
grep -q 'initializer-use\[resolved\] initializer_indirect_effect:0:recursive_reference_holder -> initializer_indirect_effect:0:referenced_global' \
    "$tmp/initializer_indirect_effect.out" || {
    cat "$tmp/initializer_indirect_effect.out"
    exit 1
}

"$compiler" preprocess --dump-link-unit "$program_dir/function_global_effects.n00b" \
    >"$tmp/function_global_effects.out" \
    2>"$tmp/function_global_effects.err" || {
    cat "$tmp/function_global_effects.err"
    cat "$tmp/function_global_effects.out"
    exit 1
}
grep -q 'n00bb.function-direct-global-effects v1 complete' \
    "$tmp/function_global_effects.out"
grep -q 'function-global-read\[resolved\] function_global_effects:1:read_effect -> function_global_effects:0:effect_global' \
    "$tmp/function_global_effects.out"
grep -q 'function-global-write\[resolved\] function_global_effects:1:write_effect -> function_global_effects:0:effect_global' \
    "$tmp/function_global_effects.out"
grep -q 'function-global-read-write\[resolved\] function_global_effects:1:update_effect -> function_global_effects:0:effect_global' \
    "$tmp/function_global_effects.out"
grep -q 'n00bb.call-effects v1 complete' "$tmp/function_global_effects.out"
grep -q 'function-global-read-summary\[resolved\] function_global_effects:1:forward_read -> function_global_effects:0:effect_global' \
    "$tmp/function_global_effects.out"
grep -q 'function-global-write-summary\[resolved\] function_global_effects:1:forward_write -> function_global_effects:0:effect_global' \
    "$tmp/function_global_effects.out"

"$compiler" lower --dump-ir --dump-link-unit \
    "$program_dir/lower_record_type.n00b" \
    >"$tmp/lower_record_type.ir" 2>"$tmp/lower_record_type.err" || {
    cat "$tmp/lower_record_type.err"
    exit 1
}
grep -q 'type lower_record_type.LoweredPoint = (x: int, y: f64)' \
    "$tmp/lower_record_type.ir"
grep -E -q 'func @identity_point\(%pt: (lower_record_type\.)?LoweredPoint.*\) -> (lower_record_type\.)?LoweredPoint' \
    "$tmp/lower_record_type.ir"
grep -q 'lower_record_type:2:LoweredPoint kind=2 binding n00bb.semantic.type' \
    "$tmp/lower_record_type.ir"
grep -q 'lower_record_type:0:LoweredPoint.x kind=0 binding n00bb.semantic.type' \
    "$tmp/lower_record_type.ir"
grep -q 'lower_record_type:0:LoweredPoint.y kind=0 binding n00bb.semantic.type' \
    "$tmp/lower_record_type.ir"

record_programs="$script_dir/programs/link_records"
N00B_PATH="$record_programs" "$compiler" lower --dump-ir --dump-link-unit \
    record_root \
    >"$tmp/record_root.ir" 2>"$tmp/record_root.err" || {
    cat "$tmp/record_root.err"
    exit 1
}
grep -q 'type record_root.SharedRecord = (local: f64)' \
    "$tmp/record_root.ir"
grep -q 'type record_dep.SharedRecord = (remote: int)' \
    "$tmp/record_root.ir"
test "$(grep -c 'type record_dep.SharedRecord =' "$tmp/record_root.ir")" -eq 2
grep -E -q 'func @use_remote\(%item: record_dep\.SharedRecord !ir.binding_phases\([0-9]+\).*\) -> record_dep\.SharedRecord' \
    "$tmp/record_root.ir"
grep -E -q 'func @use_local\(%item: record_root\.SharedRecord !ir.binding_phases\([0-9]+\).*\) -> record_root\.SharedRecord' \
    "$tmp/record_root.ir"
grep -q 'resolved record_root -> record_root:2:SharedRecord' \
    "$tmp/record_root.ir"
grep -q 'resolved record_root -> record_dep:2:SharedRecord' \
    "$tmp/record_root.ir"

N00B_PATH="$record_programs" "$compiler" preprocess --dump-symbols --dump-link-unit \
    record_field_root \
    >"$tmp/record_field_root.out" 2>"$tmp/record_field_root.err" || {
    cat "$tmp/record_field_root.err"
    cat "$tmp/record_field_root.out"
    exit 1
}
grep -q 'amount: variable, .*uses=1, declaration=1:28, definitions=\[\], use-sites=\[3:12\]' \
    "$tmp/record_field_root.out"
grep -q 'amount: variable, .*node-targets=\[3:12->self\]' \
    "$tmp/record_field_root.out"
grep -q 'RemoteRecord: type, .*member-scope=#' \
    "$tmp/record_field_root.out"
grep -q 'amount: variable, .*owner-type=RemoteRecord' \
    "$tmp/record_field_root.out"
grep -q 'resolved record_field_root -> record_field_dep:0:RemoteRecord.amount' \
    "$tmp/record_field_root.out"

if N00B_PATH="$record_programs" "$compiler" preprocess \
    record_missing_imported_field \
    >"$tmp/record_missing_imported_field.out" 2>&1; then
    echo 'missing imported record field unexpectedly passed semantic resolution' >&2
    exit 1
fi
grep -q "record type has no field named 'absent'" \
    "$tmp/record_missing_imported_field.out"

N00B_PATH="$record_programs" "$compiler" preprocess --dump-symbols --dump-link-unit \
    record_identity_root \
    >"$tmp/record_identity.out" 2>"$tmp/record_identity.err" || {
    cat "$tmp/record_identity.err"
    cat "$tmp/record_identity.out"
    exit 1
}
grep -q 'resolved record_identity_root -> record_identity_root:0:SharedRecord.local' \
    "$tmp/record_identity.out"
grep -q 'resolved record_identity_root -> record_identity_dep:0:SharedRecord.remote' \
    "$tmp/record_identity.out"

if "$compiler" preprocess \
    "$script_dir/programs/type_namespace/name_collision.n00b" \
    >"$tmp/type_name_collision.out" 2>"$tmp/type_name_collision.err"; then
    echo "type/value name collision unexpectedly compiled"
    exit 1
fi
grep -q 'N00BB-T0006' "$tmp/type_name_collision.err"
grep -q 'conflicts with a value name in the same scope' \
    "$tmp/type_name_collision.err"

if "$compiler" preprocess \
    "$program_dir/unresolved_reference.n00b" \
    >"$tmp/unresolved_reference.out" 2>&1; then
    echo 'preprocessing unexpectedly accepted an unresolved identifier' >&2
    exit 1
fi
grep -q "unresolved name 'missing_value'" \
    "$tmp/unresolved_reference.out"

if "$compiler" preprocess \
    "$program_dir/record_missing_field.n00b" \
    >"$tmp/record_missing_field.out" 2>&1; then
    echo 'missing record field unexpectedly passed semantic resolution' >&2
    exit 1
fi
grep -q "record type has no field named 'absent'" \
    "$tmp/record_missing_field.out"

nested_module_dir="$script_dir/programs/nested_modules"
N00B_PATH="$nested_module_dir" "$compiler" preprocess \
    --dump-symbols --dump-link-unit \
    nested_consumer \
    >"$tmp/nested_module_paths.out"
grep -q 'resolved nested_consumer -> deep.tools:2:Answer' \
    "$tmp/nested_module_paths.out"
grep -q 'resolved nested_consumer -> deep.tools:1:answer' \
    "$tmp/nested_module_paths.out"
grep -E -q 'payload: variable, .*node-targets=\[5:[0-9]+->self\]' \
    "$tmp/nested_module_paths.out"
N00B_PATH="$nested_module_dir" "$compiler" preprocess \
    --dump-symbols --dump-link-unit \
    deep_consumer \
    >"$tmp/deep_module_paths.out" 2>"$tmp/deep_module_paths.err" || {
    cat "$tmp/deep_module_paths.err"
    exit 1
}
grep -q 'resolved deep_consumer -> deep.tools.math.core:2:DeepAnswer' \
    "$tmp/deep_module_paths.out"
grep -q 'resolved deep_consumer -> deep.tools.math.core:1:answer' \
    "$tmp/deep_module_paths.out"
grep -E -q 'amount: variable, .*node-targets=\[6:[0-9]+->self\]' \
    "$tmp/deep_module_paths.out"

if "$compiler" preprocess \
    "$program_dir/member_unknown_receiver.n00b" \
    >"$tmp/member_unknown_receiver.out" 2>&1; then
    echo 'member access with an unresolved receiver type unexpectedly passed' >&2
    exit 1
fi
grep -q 'N00BB-L0019' "$tmp/member_unknown_receiver.out"

if "$compiler" preprocess \
    "$program_dir/member_nonrecord_receiver.n00b" \
    >"$tmp/member_nonrecord_receiver.out" 2>&1; then
    echo 'member access on a primitive unexpectedly passed' >&2
    exit 1
fi
grep -q 'fatal\[LANTERN-T0001\]' "$tmp/member_nonrecord_receiver.out"

if "$compiler" preprocess \
    "$script_dir/programs/type_namespace/duplicate_type.n00b" \
    >"$tmp/duplicate_type.out" 2>"$tmp/duplicate_type.err"; then
    echo "duplicate type definition unexpectedly compiled"
    exit 1
fi
grep -q 'N00BB-T0005' "$tmp/duplicate_type.err"
grep -q "conflicting record definitions for 'DuplicateType'" \
    "$tmp/duplicate_type.err"

if "$compiler" preprocess \
    "$script_dir/programs/link_validation/duplicate_function.n00b" \
    >"$tmp/duplicate_function.out" 2>"$tmp/duplicate_function.err"; then
    echo "duplicate function definition unexpectedly compiled"
    exit 1
fi
grep -q 'LANTERN-L0003' "$tmp/duplicate_function.err"
grep -q 'duplicate definitions for one module declaration identity' \
    "$tmp/duplicate_function.err"

visibility_dir="$script_dir/programs/type_visibility"
N00B_PATH="$visibility_dir" "$compiler" preprocess \
    "$visibility_dir/public_consumer.n00b" \
    >"$tmp/public_type_visibility.out" \
    2>"$tmp/public_type_visibility.err"
N00B_PATH="$visibility_dir" "$compiler" preprocess \
    "$program_dir/public_field_consumer.n00b" \
    >"$tmp/public_field_visibility.out" \
    2>"$tmp/public_field_visibility.err"
if N00B_PATH="$visibility_dir" "$compiler" preprocess \
    "$program_dir/private_field_consumer.n00b" \
    >"$tmp/private_field_visibility.out" \
    2>"$tmp/private_field_visibility.err"; then
    echo 'private field unexpectedly visible across modules'
    exit 1
fi
grep -q 'N00BB-L0021' "$tmp/private_field_visibility.err"
N00B_PATH="$visibility_dir" "$compiler" preprocess \
    "$visibility_dir/opaque_consumer.n00b" \
    >"$tmp/opaque_type_visibility.out" \
    2>"$tmp/opaque_type_visibility.err"
(cd "$visibility_dir" && "$compiler" preprocess \
    function_same_package_consumer.n00b) \
    >"$tmp/same_package_function.out" \
    2>"$tmp/same_package_function.err"
if (cd "$visibility_dir" && "$compiler" preprocess \
    function_private_same_package_consumer.n00b) \
    >"$tmp/private_same_package_function.out" \
    2>"$tmp/private_same_package_function.err"; then
    echo 'private function unexpectedly visible within its package'
    exit 1
fi
grep -q 'N00BB-T0008' "$tmp/private_same_package_function.err"
N00B_PATH="$visibility_dir" "$compiler" preprocess \
    "$visibility_dir/function_public_consumer.n00b" \
    >"$tmp/public_function_visibility.out" \
    2>"$tmp/public_function_visibility.err"
if N00B_PATH="$visibility_dir" "$compiler" preprocess \
    "$visibility_dir/function_missing_consumer.n00b" \
    >"$tmp/missing_function_visibility.out" \
    2>"$tmp/missing_function_visibility.err"; then
    echo 'missing module function unexpectedly resolved'
    exit 1
fi
grep -q 'module .* has no visible function named' \
    "$tmp/missing_function_visibility.err"
if N00B_PATH="$visibility_dir" "$compiler" preprocess \
    "$visibility_dir/variable_private_consumer.n00b" \
    >"$tmp/private_global_visibility.out" \
    2>"$tmp/private_global_visibility.err"; then
    echo 'module-private global unexpectedly visible through an import'
    exit 1
fi
grep -q "has no visible member named 'default_private_global'" \
    "$tmp/private_global_visibility.err"
if N00B_PATH="$visibility_dir" "$compiler" preprocess \
    "$program_dir/opaque_record_consumer.n00b" \
    >"$tmp/opaque_record_access.out" 2>&1; then
    echo 'opaque record field unexpectedly accessible outside its package'
    exit 1
fi
grep -q 'N00BB-L0021.*record field is not visible' \
    "$tmp/opaque_record_access.out"
N00B_PATH="$visibility_dir" "$compiler" preprocess \
    "$visibility_dir/function_package_consumer.n00b" \
    >"$tmp/package_function_visibility.out" \
    2>"$tmp/package_function_visibility.err"
if N00B_PATH="$visibility_dir" "$compiler" preprocess \
    "$visibility_dir/function_private_consumer.n00b" \
    >"$tmp/private_function_visibility.out" \
    2>"$tmp/private_function_visibility.err"; then
    echo 'private function unexpectedly visible across packages'
    exit 1
fi
grep -q 'N00BB-T0009' "$tmp/private_function_visibility.err"
(cd "$visibility_dir" && "$compiler" preprocess default_consumer.n00b) \
    >"$tmp/default_type_visibility.out" \
    2>"$tmp/default_type_visibility.err"
for visibility in default package private; do
    consumer="${visibility}_consumer"
    [ "$visibility" = default ] && consumer="default_cross_package"
    if N00B_PATH="$visibility_dir" "$compiler" preprocess \
        "$visibility_dir/${consumer}.n00b" \
        >"$tmp/${visibility}_type_visibility.out" \
        2>"$tmp/${visibility}_type_visibility.err"; then
        echo "$visibility type unexpectedly visible across packages"
        exit 1
    fi
    grep -q 'N00BB-T0007' "$tmp/${visibility}_type_visibility.err"
    grep -q 'has no visible type' \
        "$tmp/${visibility}_type_visibility.err"
done

"$compiler" preprocess "$program_dir/unused_members.n00b" \
    >"$tmp/unused_members.out" 2>"$tmp/unused_members.err"
grep -q "N00BB-W0001.*never_set.*is never set" "$tmp/unused_members.err"
grep -q "N00BB-W0001.*read_only.*is never set" "$tmp/unused_members.err"
grep -q "N00BB-W0002.*write_only.*is set but never read" \
    "$tmp/unused_members.err"
grep -q "N00BB-W0003.*orphan.*is never used" "$tmp/unused_members.err"
! grep -q "N00BB-W0003.*write_fields" "$tmp/unused_members.err"
! grep -q "N00BB-W0003.*read_used" "$tmp/unused_members.err"
! grep -q "N00BB-W0003.*read_only_field" "$tmp/unused_members.err"
! grep -q "N00BB-W000[12].*field 'used'" "$tmp/unused_members.err"

N00B_PATH="$program_dir" "$compiler" preprocess \
    "$program_dir/unused_members_root.n00b" \
    >"$tmp/unused_members_closure.out" \
    2>"$tmp/unused_members_closure.err"
grep -q "N00BB-W0001.*never_set.*is never set" \
    "$tmp/unused_members_closure.err"
grep -q "N00BB-W0002.*write_only.*is set but never read" \
    "$tmp/unused_members_closure.err"
grep -A2 "warning\[N00BB-W0001\].*never_set.*is never set" \
    "$tmp/unused_members_closure.err" | grep -q 'module unused_members_dep'
grep -A2 "warning\[N00BB-W0002\].*write_only.*is set but never read" \
    "$tmp/unused_members_closure.err" | grep -q 'module unused_members_dep'
grep -q "N00BB-W0003.*imported_orphan.*is never used" \
    "$tmp/unused_members_closure.err"

N00B_PATH="$program_dir" "$compiler" lower --dump-ir --dump-link-unit \
    "$program_dir/variant_root.n00b" \
    >"$tmp/variant_root.ir" 2>"$tmp/variant_root.err" || {
    cat "$tmp/variant_root.err"
    exit 1
}
grep -q 'variant_dep.impl.RemoteValue' "$tmp/variant_root.ir"
grep -E -q 'variant.tag (%item|%__lowered_[0-9]+)' \
    "$tmp/variant_root.ir"
grep -q 'variant.payload' "$tmp/variant_root.ir"
grep -q 'variant_dep.impl:0:RemoteValue.number kind=' \
    "$tmp/variant_root.ir"
grep -q 'variant_dep.impl:0:RemoteValue.text kind=' \
    "$tmp/variant_root.ir"

"$compiler" lower --dump-link-unit "$program_dir/link_nested_declarations.n00b" \
    >"$tmp/link_nested_declarations.out" \
    2>"$tmp/link_nested_declarations.err" || {
    cat "$tmp/link_nested_declarations.err"
    exit 1
}
test "$(grep -c 'N00BB-W0001.*field '\''x'\'' is never set' \
    "$tmp/link_nested_declarations.err")" -eq 1
grep -q 'link_nested_declarations:4:Color.red kind=' \
    "$tmp/link_nested_declarations.out"
grep -q 'link_nested_declarations:0:Value.some kind=' \
    "$tmp/link_nested_declarations.out"
grep -q 'link_nested_declarations:1:Render.render kind=' \
    "$tmp/link_nested_declarations.out"
grep -q 'link_nested_declarations:0:Point.x kind=' \
    "$tmp/link_nested_declarations.out"
grep -q 'link_nested_declarations:2:Color kind=2 binding n00bb.semantic.type' \
    "$tmp/link_nested_declarations.out"
grep -q 'link_nested_declarations:4:Color.red kind=4 binding n00bb.semantic.type' \
    "$tmp/link_nested_declarations.out"
grep -q 'link_nested_declarations:4:Color.green kind=4 binding n00bb.semantic.type' \
    "$tmp/link_nested_declarations.out"
grep -q 'link_nested_declarations:2:Value kind=2 binding n00bb.semantic.type' \
    "$tmp/link_nested_declarations.out"
grep -q 'link_nested_declarations:0:Value.some kind=0 binding n00bb.semantic.type' \
    "$tmp/link_nested_declarations.out"
grep -q 'link_nested_declarations:2:Render kind=2 binding n00bb.semantic.type' \
    "$tmp/link_nested_declarations.out"

"$compiler" lower "$program_dir/lower_out_parameter.n00b" \
    >"$tmp/lower_out_parameter.ir" \
    2>"$tmp/lower_out_parameter.err" || {
    cat "$tmp/lower_out_parameter.err"
    cat "$tmp/lower_out_parameter.ir"
    exit 1
}
grep -q '%arg0: int' "$tmp/lower_out_parameter.ir" || {
    cat "$tmp/lower_out_parameter.ir"
    exit 1
}
grep -q 'copy \$__n00bb_param_' "$tmp/lower_out_parameter.ir" || {
    cat "$tmp/lower_out_parameter.ir"
    exit 1
}
grep -q 'return %' "$tmp/lower_out_parameter.ir" || {
    cat "$tmp/lower_out_parameter.ir"
    exit 1
}

"$compiler" lower "$program_dir/lower_if.n00b" \
    >"$tmp/lower_if.ir" 2>"$tmp/lower_if.err" || {
    cat "$tmp/lower_if.err"
    exit 1
}
grep -Eq 'branch %flag, __n00bb_if_then_[0-9]+, __n00bb_if_join_[0-9]+' \
    "$tmp/lower_if.ir"
grep -q 'return %' "$tmp/lower_if.ir"

"$compiler" lower "$program_dir/lower_if_else.n00b" \
    >"$tmp/lower_if_else.ir" 2>"$tmp/lower_if_else.err" || {
    cat "$tmp/lower_if_else.err"
    exit 1
}
grep -Eq 'branch %flag, __n00bb_if_then_[0-9]+, __n00bb_if_else_[0-9]+' \
    "$tmp/lower_if_else.ir"
grep -q 'unreachable' "$tmp/lower_if_else.ir"

"$compiler" lower "$program_dir/lower_if_elif.n00b" \
    >"$tmp/lower_if_elif.ir" 2>"$tmp/lower_if_elif.err" || {
    cat "$tmp/lower_if_elif.err"
    exit 1
}
grep -q '__n00bb_if_test_' "$tmp/lower_if_elif.ir"
grep -q '__n00bb_if_else_' "$tmp/lower_if_elif.ir"
grep -q 'unreachable' "$tmp/lower_if_elif.ir"

"$compiler" lower "$program_dir/lower_if_elif_no_else.n00b" \
    >"$tmp/lower_if_elif_no_else.ir" 2>"$tmp/lower_if_elif_no_else.err" || {
    cat "$tmp/lower_if_elif_no_else.err"
    exit 1
}
grep -q '__n00bb_if_test_' "$tmp/lower_if_elif_no_else.ir"
grep -q '__n00bb_if_join_' "$tmp/lower_if_elif_no_else.ir"

"$compiler" lower "$program_dir/lower_if_value.n00b" \
    >"$tmp/lower_if_value.ir" 2>"$tmp/lower_if_value.err" || {
    cat "$tmp/lower_if_value.err"
    exit 1
}
grep -q '__n00bb_if_test_' "$tmp/lower_if_value.ir"
grep -q '__n00bb_if_value_' "$tmp/lower_if_value.ir"

"$compiler" lower "$program_dir/lower_while_break.n00b" \
    >"$tmp/lower_while_break.ir" 2>"$tmp/lower_while_break.err" || {
    cat "$tmp/lower_while_break.err"
    exit 1
}
grep -q '__n00bb_while_header_' "$tmp/lower_while_break.ir"
grep -q '__n00bb_while_exit_' "$tmp/lower_while_break.ir"

"$compiler" lower "$program_dir/lower_while_continue.n00b" \
    >"$tmp/lower_while_continue.ir" 2>"$tmp/lower_while_continue.err" || {
    cat "$tmp/lower_while_continue.err"
    exit 1
}
grep -q '__n00bb_while_header_' "$tmp/lower_while_continue.ir"
grep -q '__n00bb_while_exit_' "$tmp/lower_while_continue.ir"

"$compiler" lower --dump-ir --dump-link-unit \
    "$program_dir/lower_loop_local_initialization.n00b" \
    >"$tmp/lower_loop_local_initialization.ir" \
    2>"$tmp/lower_loop_local_initialization.err" || {
    cat "$tmp/lower_loop_local_initialization.err"
    exit 1
}
grep -q 'ir.binding_reentry_header' \
    "$tmp/lower_loop_local_initialization.ir"
grep -q 'n00bb.finalization v1 complete' \
    "$tmp/lower_loop_local_initialization.ir"
grep -Eq 'cleanup\[ranges=[1-9][0-9]*,complete-ranges=[1-9][0-9]*,cfg-edges=[1-9][0-9]*,.*complete\]' \
    "$tmp/lower_loop_local_initialization.ir"

"$compiler" lower --dump-ir --dump-link-unit \
    "$program_dir/cleanup_error_exit.n00b" \
    >"$tmp/cleanup_error_exit.out" 2>"$tmp/cleanup_error_exit.err" || {
    cat "$tmp/cleanup_error_exit.err"
    exit 1
}
grep -q 'n00bb.finalization v1 complete' "$tmp/cleanup_error_exit.out"
grep -Eq 'cleanup\[ranges=1,complete-ranges=1,cfg-edges=[1-9][0-9]*,endpoints=[1-9][0-9]*,exits=1,safe-exits=1,live-exits=0,unknown-exits=0,.*complete\]' \
    "$tmp/cleanup_error_exit.out"

"$compiler" lower "$program_dir/lower_labeled_break.n00b" \
    >"$tmp/lower_labeled_break.ir" 2>"$tmp/lower_labeled_break.err" || {
    cat "$tmp/lower_labeled_break.err"
    exit 1
}
grep -q 'jump __n00bb_while_exit_' "$tmp/lower_labeled_break.ir"

"$compiler" lower "$program_dir/lower_labeled_continue.n00b" \
    >"$tmp/lower_labeled_continue.ir" \
    2>"$tmp/lower_labeled_continue.err" || {
    cat "$tmp/lower_labeled_continue.err"
    exit 1
}
grep -q 'jump __n00bb_while_header_0' "$tmp/lower_labeled_continue.ir"

"$compiler" lower "$program_dir/lower_range_for.n00b" \
    >"$tmp/lower_range_for.ir" 2>"$tmp/lower_range_for.err" || {
    cat "$tmp/lower_range_for.err"
    exit 1
}
grep -q 'le %' "$tmp/lower_range_for.ir"
grep -q 'lt %' "$tmp/lower_range_for.ir"
grep -q '__n00bb_for_latch_' "$tmp/lower_range_for.ir"
"$compiler" preprocess --dump-symbols \
    "$program_dir/lower_range_for.n00b" \
    >"$tmp/lower_range_for.symbols" 2>&1
grep -q 'index: variable, .*declaration=2:9, definitions=\[2:9\], use-sites=\[2:36\]' \
    "$tmp/lower_range_for.symbols"
"$compiler" lower --dump-ir \
    "$program_dir/lower_range_for_step.n00b" \
    >"$tmp/lower_range_for_step.ir" \
    2>"$tmp/lower_range_for_step.err" || {
    cat "$tmp/lower_range_for_step.err"
    cat "$tmp/lower_range_for_step.ir"
    exit 1
}
grep -q '= select %' "$tmp/lower_range_for_step.ir"
grep -q 'add trap .*%stride' "$tmp/lower_range_for_step.ir"
grep -q 'for_zero_step' "$tmp/lower_range_for_step.ir"
grep -q '^      trap ' "$tmp/lower_range_for_step.ir"
if "$compiler" lower \
    "$program_dir/lower_range_for_zero_step.n00b" \
    >"$tmp/lower_range_for_zero_step.out" 2>&1; then
    echo 'a range loop with a statically zero step unexpectedly compiled' >&2
    exit 1
fi
grep -q 'range step must not be zero' "$tmp/lower_range_for_zero_step.out"
if "$compiler" lower \
    "$program_dir/lower_range_for_multiple_bindings.n00b" \
    >"$tmp/lower_range_for_multiple_bindings.out" 2>&1; then
    echo 'a range loop with multiple bindings unexpectedly compiled' >&2
    exit 1
fi
grep -q 'error\[N00BB-P0001\].*syntax error' \
    "$tmp/lower_range_for_multiple_bindings.out"

"$compiler" lower "$program_dir/lower_switch.n00b" \
    >"$tmp/lower_switch.ir" 2>"$tmp/lower_switch.err" || {
    cat "$tmp/lower_switch.err"
    exit 1
}
grep -q 'switch %code, default: __n00bb_switch_else_' "$tmp/lower_switch.ir"
grep -q '\[1: __n00bb_switch_case_' "$tmp/lower_switch.ir"
grep -q '\[2: __n00bb_switch_case_' "$tmp/lower_switch.ir"
grep -q '\[-1: __n00bb_switch_case_' "$tmp/lower_switch.ir"
grep -q '__n00bb_switch_else_' "$tmp/lower_switch.ir"

"$compiler" lower "$program_dir/lower_labeled_switch_break.n00b" \
    >"$tmp/lower_labeled_switch_break.ir" \
    2>"$tmp/lower_labeled_switch_break.err" || {
    cat "$tmp/lower_labeled_switch_break.err"
    exit 1
}
grep -q 'jump __n00bb_switch_join_' "$tmp/lower_labeled_switch_break.ir"

"$compiler" lower "$program_dir/lower_switch_no_else.n00b" \
    >"$tmp/lower_switch_no_else.ir" 2>"$tmp/lower_switch_no_else.err" || {
    cat "$tmp/lower_switch_no_else.err"
    exit 1
}
grep -q 'switch %flag, default: __n00bb_switch_join_' \
    "$tmp/lower_switch_no_else.ir"

"$compiler" lower "$program_dir/lower_switch_value.n00b" \
    >"$tmp/lower_switch_value.ir" \
    2>"$tmp/lower_switch_value.err" || {
    cat "$tmp/lower_switch_value.err"
    exit 1
}
grep -q 'switch %code' "$tmp/lower_switch_value.ir"
grep -q '__n00bb_switch_result_' "$tmp/lower_switch_value.ir"

"$compiler" lower "$program_dir/lower_switch_ranges.n00b" \
    >"$tmp/lower_switch_ranges.ir" \
    2>"$tmp/lower_switch_ranges.err" || {
    cat "$tmp/lower_switch_ranges.err"
    exit 1
}
grep -q 'ge %' "$tmp/lower_switch_ranges.ir"
grep -q 'le %' "$tmp/lower_switch_ranges.ir"
grep -q 'lt %' "$tmp/lower_switch_ranges.ir"
grep -q '__n00bb_switch_test_' "$tmp/lower_switch_ranges.ir"

"$compiler" lower --dump-ir "$program_dir/lower_switch_expression_pattern.n00b" \
    >"$tmp/lower_switch_expression_pattern.ir" \
    2>"$tmp/lower_switch_expression_pattern.err" || {
    cat "$tmp/lower_switch_expression_pattern.err"
    exit 1
}
grep -q 'eq %code, %wanted' "$tmp/lower_switch_expression_pattern.ir"
grep -q '__n00bb_switch_test_' "$tmp/lower_switch_expression_pattern.ir"

"$compiler" lower --dump-ir --dump-link-unit \
    "$program_dir/lower_variant_switch.n00b" \
    >"$tmp/lower_variant_switch.ir" \
    2>"$tmp/lower_variant_switch.err" || {
    cat "$tmp/lower_variant_switch.err"
    exit 1
}
grep -E -q 'variant.tag (%item|%__lowered_[0-9]+)' \
    "$tmp/lower_variant_switch.ir"
grep -q 'switch %__n00bb_variant_tag_' "$tmp/lower_variant_switch.ir"
grep -q 'variant.payload' "$tmp/lower_variant_switch.ir"
grep -q 'switch_else' "$tmp/lower_variant_switch.ir"
grep -q 'lower_variant_switch:0:SwitchValue.number kind=0 binding n00bb.semantic.type' \
    "$tmp/lower_variant_switch.ir"

"$compiler" preprocess --dump-symbols --dump-link-unit \
    "$program_dir/lower_variant_switch.n00b" \
    >"$tmp/lower_variant_switch.symbols" \
    2>"$tmp/lower_variant_switch.symbols.err" || {
    cat "$tmp/lower_variant_switch.symbols.err"
    cat "$tmp/lower_variant_switch.symbols"
    exit 1
}
grep -q 'number: variable, .*uses=2, declaration=2:5, definitions=\[\], use-sites=\[7:30, 7:14\]' \
    "$tmp/lower_variant_switch.symbols"
grep -q 'resolved lower_variant_switch -> lower_variant_switch:0:SwitchValue.number at 7:30' \
    "$tmp/lower_variant_switch.symbols"
grep -q 'resolved lower_variant_switch -> lower_variant_switch:0:SwitchValue.number at 7:14' \
    "$tmp/lower_variant_switch.symbols"

"$compiler" preprocess --dump-symbols \
    "$program_dir/type_name_shadow.n00b" \
    >"$tmp/type_name_shadow.symbols" \
    2>"$tmp/type_name_shadow.symbols.err" || {
    cat "$tmp/type_name_shadow.symbols.err"
    cat "$tmp/type_name_shadow.symbols"
    exit 1
}
grep -q 'Count: variable, .*uses=0, declaration=4:9' \
    "$tmp/type_name_shadow.symbols"
grep -q 'Count: type, .*uses=3, declaration=1:1, definitions=\[\], use-sites=\[3:35, 3:45, 5:17\]' \
    "$tmp/type_name_shadow.symbols"

"$compiler" preprocess --dump-symbols --dump-link-unit \
    "$program_dir/record_field_reference.n00b" \
    >"$tmp/record_field_reference.out" \
    2>"$tmp/record_field_reference.err" || {
    cat "$tmp/record_field_reference.err"
    cat "$tmp/record_field_reference.out"
    exit 1
}
grep -q 'amount: variable, .*uses=1, declaration=2:5, definitions=\[\], use-sites=\[6:12\]' \
    "$tmp/record_field_reference.out"
grep -q 'amount: variable, .*node-targets=\[6:12->self\]' \
    "$tmp/record_field_reference.out"
grep -q 'FieldRecord: type, .*member-scope=#' \
    "$tmp/record_field_reference.out"
grep -q 'amount: variable, .*owner-type=FieldRecord' \
    "$tmp/record_field_reference.out"
grep -q 'resolved record_field_reference -> record_field_reference:0:FieldRecord.amount' \
    "$tmp/record_field_reference.out"

"$compiler" preprocess --dump-symbols \
    "$program_dir/anonymous_record_field.n00b" \
    >"$tmp/anonymous_record_field.out" 2>&1 || {
    cat "$tmp/anonymous_record_field.out"
    exit 1
}
grep -q 'item: variable, .*type=(answer: int)' \
    "$tmp/anonymous_record_field.out"
! grep -q '^    answer: type,' "$tmp/anonymous_record_field.out"

"$compiler" preprocess --dump-symbols \
    "$program_dir/multi_name_loop.n00b" \
    >"$tmp/multi_name_loop.out" 2>&1 || {
    cat "$tmp/multi_name_loop.out"
    exit 1
}
grep -Eq 'number: variable, .*type=int.*declaration=[0-9]+:9, definitions=\[[0-9]+:9\], use-sites=\[[0-9]+:9\]' \
    "$tmp/multi_name_loop.out"
grep -Eq 'label: variable, .*type=string.*declaration=[0-9]+:17, definitions=\[[0-9]+:17\], use-sites=\[[0-9]+:9\]' \
    "$tmp/multi_name_loop.out"

if "$compiler" preprocess \
    "$program_dir/multi_name_non_tuple_loop.n00b" \
    >"$tmp/multi_name_non_tuple_loop.out" 2>&1; then
    echo 'multi-binding loop unexpectedly accepted a non-tuple item' >&2
    exit 1
fi
grep -q 'loop bindings do not match a named-field tuple item' \
    "$tmp/multi_name_non_tuple_loop.out"

"$compiler" lower --dump-ir "$program_dir/lower_variant_exhaustive.n00b" \
    >"$tmp/lower_variant_exhaustive.ir" \
    2>"$tmp/lower_variant_exhaustive.err" || {
    cat "$tmp/lower_variant_exhaustive.err"
    exit 1
}
grep -q 'switch_unreachable' "$tmp/lower_variant_exhaustive.ir"

"$compiler" lower --dump-ir \
    "$program_dir/lower_variant_arm_assignment.n00b" \
    >"$tmp/lower_variant_arm_assignment.ir" \
    2>"$tmp/lower_variant_arm_assignment.err" || {
    cat "$tmp/lower_variant_arm_assignment.err"
    exit 1
}
grep -q 'variant.make' "$tmp/lower_variant_arm_assignment.ir"
grep -q 'number' "$tmp/lower_variant_arm_assignment.ir"
test "$(grep -c 'variant.make' "$tmp/lower_variant_arm_assignment.ir")" -eq 3

N00B_PATH="$program_dir" "$compiler" lower --dump-ir --dump-link-unit \
    "$program_dir/lower_variant_payload_specificity.n00b" \
    >"$tmp/lower_variant_payload_specificity.out" \
    2>"$tmp/lower_variant_payload_specificity.err" || {
    cat "$tmp/lower_variant_payload_specificity.err"
    exit 1
}
grep -q 'variant.make "payload"' \
    "$tmp/lower_variant_payload_specificity.out"
grep -q 'resolved lower_variant_payload_specificity -> constructor_specific:1:__init' \
    "$tmp/lower_variant_payload_specificity.out"
if grep -q 'resolved lower_variant_payload_specificity -> constructor_general:1:__init' \
    "$tmp/lower_variant_payload_specificity.out"; then
    echo 'generic payload constructor was selected over the exact specialization' >&2
    exit 1
fi

if "$compiler" lower "$program_dir/lower_variant_invalid_arm.n00b" \
    >"$tmp/lower_variant_invalid_arm.out" 2>&1; then
    echo 'assignment to an undeclared variant arm unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'N00BB-L0016.*no field named.*missing' \
    "$tmp/lower_variant_invalid_arm.out"

if "$compiler" lower \
    "$program_dir/lower_variant_mismatched_payload.n00b" \
    >"$tmp/lower_variant_mismatched_payload.out" 2>&1; then
    echo 'mismatched variant payload unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'IR-E0011.*payload type int, but got string' \
    "$tmp/lower_variant_mismatched_payload.out"

if "$compiler" lower \
    "$program_dir/lower_variant_incomplete_payload.n00b" \
    >"$tmp/lower_variant_incomplete_payload.out" 2>&1; then
    echo 'incomplete named-field variant payload unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'IR-E0011.*CompletePayload, but got (first: int)' \
    "$tmp/lower_variant_incomplete_payload.out"

if "$compiler" lower \
    "$program_dir/lower_variant_invalid_constructor_args.n00b" \
    >"$tmp/lower_variant_invalid_constructor_args.out" 2>&1; then
    echo 'variant payload constructor with missing arguments unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'N00BB-T0020.*no visible __init matches this constructed type and its arguments' \
    "$tmp/lower_variant_invalid_constructor_args.out"

if "$compiler" lower "$program_dir/lower_variant_nonexhaustive.n00b" \
    >"$tmp/lower_variant_nonexhaustive.out" 2>&1; then
    echo 'non-exhaustive variant switch unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'variant switch without else must cover every arm exactly once' \
    "$tmp/lower_variant_nonexhaustive.out"

if "$compiler" lower "$program_dir/lower_variant_nonvariable_subject.n00b" \
    >"$tmp/lower_variant_nonvariable_subject.out" 2>&1; then
    echo 'non-variable variant switch subject unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'variant switch subject must be a variable' \
    "$tmp/lower_variant_nonvariable_subject.out"

if "$compiler" lower "$program_dir/lower_variant_wrong_arm_payload.n00b" \
    >"$tmp/lower_variant_wrong_arm_payload.out" 2>&1; then
    echo 'payload access for an unmatched variant arm unexpectedly succeeded' >&2
    exit 1
fi

if "$compiler" lower "$program_dir/lower_variant_nil_payload.n00b" \
    >"$tmp/lower_variant_nil_payload.out" 2>&1; then
    echo 'reading a nil variant arm unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'nil variant arm has no readable payload' \
    "$tmp/lower_variant_nil_payload.out" || {
    cat "$tmp/lower_variant_nil_payload.out"
    exit 1
}

if "$compiler" lower "$program_dir/lower_variant_duplicate_arm.n00b" \
    >"$tmp/lower_variant_duplicate_arm.out" 2>&1; then
    echo 'duplicate variant switch arm unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'variant switch arm appears more than once' \
    "$tmp/lower_variant_duplicate_arm.out"

if "$compiler" lower "$program_dir/lower_variant_else_payload.n00b" \
    >"$tmp/lower_variant_else_payload.out" 2>&1; then
    echo 'variant payload access in else unexpectedly succeeded' >&2
    exit 1
fi

if "$compiler" preprocess "$program_dir/lower_variant_unknown_arm.n00b" \
    >"$tmp/lower_variant_unknown_arm.preprocess.out" 2>&1; then
    echo 'unknown variant switch arm passed semantic resolution' >&2
    exit 1
fi
grep -q 'variant switch cases must name variant arms' \
    "$tmp/lower_variant_unknown_arm.preprocess.out"

if "$compiler" lower "$program_dir/lower_variant_unknown_arm.n00b" \
    >"$tmp/lower_variant_unknown_arm.out" 2>&1; then
    echo 'unknown variant switch arm unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'variant switch cases must name variant arms' \
    "$tmp/lower_variant_unknown_arm.out"

if "$compiler" lower "$program_dir/lower_variant_compound_arm.n00b" \
    >"$tmp/lower_variant_compound_arm.out" 2>&1; then
    echo 'compound variant switch label unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'variant switch cases must name variant arms' \
    "$tmp/lower_variant_compound_arm.out"

if "$compiler" lower "$program_dir/lower_variant_grouped_arms.n00b" \
    >"$tmp/lower_variant_grouped_arms.out" 2>&1; then
    echo 'grouped variant switch arms unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'variant switch cases must name exactly one arm' \
    "$tmp/lower_variant_grouped_arms.out"

"$compiler" lower "$program_dir/lower_call.n00b" \
    >"$tmp/lower_call.ir" 2>"$tmp/lower_call.err" || {
    cat "$tmp/lower_call.err"
    exit 1
}
grep -q '= call @identity, %item' "$tmp/lower_call.ir"
"$compiler" preprocess --dump-symbols "$program_dir/lower_call.n00b" \
    >"$tmp/lower_call.symbols"
grep -q 'identity: function, .*node-targets=\[2:47->self\]' \
    "$tmp/lower_call.symbols"

"$compiler" lower --dump-ir "$program_dir/lower_variadic.n00b" \
    >"$tmp/lower_variadic.ir" 2>"$tmp/lower_variadic.err" || {
    cat "$tmp/lower_variadic.err"
    cat "$tmp/lower_variadic.ir"
    exit 1
}
grep -E -q 'func @collect\(%prefix: int .*\*%values: int\).*-> array\[int\]' \
    "$tmp/lower_variadic.ir"
grep -q '= rest.collect %values' "$tmp/lower_variadic.ir"
test "$(grep -c '= rest.collect %values' "$tmp/lower_variadic.ir")" -eq 2
grep -q 'call @collect, .*,' "$tmp/lower_variadic.ir"
grep -q 'call @collect' "$tmp/lower_variadic.ir"

"$compiler" lower --dump-ir "$program_dir/lower_array_literals.n00b" \
    >"$tmp/lower_array_literals.ir" 2>"$tmp/lower_array_literals.err" || {
    cat "$tmp/lower_array_literals.err"
    cat "$tmp/lower_array_literals.ir"
    exit 1
}
test "$(grep -c '= aggregate' "$tmp/lower_array_literals.ir")" -eq 4
grep -q '= const 10' "$tmp/lower_array_literals.ir"
grep -q '= aggregate %' "$tmp/lower_array_literals.ir"
grep -q 'func @lower_tagged_array() -> array\[int\]' \
    "$tmp/lower_array_literals.ir"
grep -q 'func @lower_short_tagged_array() -> array\[int\]' \
    "$tmp/lower_array_literals.ir"

"$compiler" lower --dump-ir "$script_dir/programs/lower_list_literals.n00b" \
    >"$tmp/lower_list_literals.ir" 2>"$tmp/lower_list_literals.err" || {
    cat "$tmp/lower_list_literals.err"
    cat "$tmp/lower_list_literals.ir"
    exit 1
}
test "$(grep -c '!ir.aggregate.form("elements")' \
    "$tmp/lower_list_literals.ir")" -eq 3
grep -q 'func @lower_list_values.*-> list\[int\]' \
    "$tmp/lower_list_literals.ir"
grep -q 'func @lower_tagged_list_values.*-> list\[int\]' \
    "$tmp/lower_list_literals.ir"
grep -q 'func @lower_short_tagged_list_values.*-> list\[int\]' \
    "$tmp/lower_list_literals.ir"

"$compiler" lower --dump-ir "$script_dir/programs/lower_set_literals.n00b" \
    >"$tmp/lower_set_literals.ir" 2>"$tmp/lower_set_literals.err" || {
    cat "$tmp/lower_set_literals.err"
    cat "$tmp/lower_set_literals.ir"
    exit 1
}
test "$(grep -c '!ir.aggregate.form("elements")' \
    "$tmp/lower_set_literals.ir")" -eq 3
grep -q 'func @lower_set_values() -> set\[int\]' \
    "$tmp/lower_set_literals.ir"
grep -q 'func @lower_tagged_set_values() -> set\[int\]' \
    "$tmp/lower_set_literals.ir"

"$compiler" lower --dump-ir "$script_dir/programs/lower_dict_literals.n00b" \
    >"$tmp/lower_dict_literals.ir" 2>"$tmp/lower_dict_literals.err" || {
    cat "$tmp/lower_dict_literals.err"
    cat "$tmp/lower_dict_literals.ir"
    exit 1
}
test "$(grep -c '!ir.aggregate.form("pairs")' \
    "$tmp/lower_dict_literals.ir")" -eq 3
grep -q 'func @lower_dict_values() -> dict\[string, int\]' \
    "$tmp/lower_dict_literals.ir"
grep -q 'func @lower_tagged_dict_values() -> dict\[string, int\]' \
    "$tmp/lower_dict_literals.ir"

"$compiler" lower --dump-ir "$script_dir/programs/lower_string_literals.n00b" \
    >"$tmp/lower_string_literals.ir" 2>"$tmp/lower_string_literals.err" || {
    cat "$tmp/lower_string_literals.err"
    cat "$tmp/lower_string_literals.ir"
    exit 1
}
grep -Fq '!ir.const.bytes("410042")' "$tmp/lower_string_literals.ir"
grep -Fq '!ir.const.bytes("62696e617279")' "$tmp/lower_string_literals.ir"
grep -Fq '!ir.const.bytes("6c6f6e672062696e617279")' \
    "$tmp/lower_string_literals.ir"
grep -q 'func @lower_binary_string() -> string' \
    "$tmp/lower_string_literals.ir"
grep -q 'func @lower_long_binary_string() -> string' \
    "$tmp/lower_string_literals.ir"
grep -q 'func @lower_rich_string() -> string' \
    "$tmp/lower_string_literals.ir"
grep -q 'func @lower_rich_auto_string() -> string' \
    "$tmp/lower_string_literals.ir"
grep -q 'func @lower_rich_plain_string() -> string' \
    "$tmp/lower_string_literals.ir"
grep -Fq '!ir.const.bytes("706c61696e20726963682074657874")' \
    "$tmp/lower_string_literals.ir"
! sed -n '/func @lower_rich_plain_string()/,/^  }/p' \
    "$tmp/lower_string_literals.ir" | grep -q '!n00bb.rich.metadata'
grep -Fq '\"tag\":\"b\",\"start\":0,\"end\":0' \
    "$tmp/lower_string_literals.ir"
grep -Fq '\"position\":1,\"format\":\"s\"' \
    "$tmp/lower_string_literals.ir"
grep -Fq '\"tag\":\"u\",\"start\":0,\"end\":2' \
    "$tmp/lower_string_literals.ir"
grep -Fq '\"tag\":\"b\",\"start\":1,\"end\":3' \
    "$tmp/lower_string_literals.ir"
grep -Fq '!n00bb.literal.modifier("binary")' \
    "$tmp/lower_string_literals.ir"
grep -Fq '!n00bb.literal.modifier("rich")' \
    "$tmp/lower_string_literals.ir"
grep -Fq '!ir.const.bytes("7465787420")' \
    "$tmp/lower_string_literals.ir"
grep -Fq '\"tag\":\"bold\",\"start\":0,\"end\":4' \
    "$tmp/lower_string_literals.ir"
grep -Fq '\"position\":3,\"format\":\"04d\"' \
    "$tmp/lower_string_literals.ir"
grep -Fq '\"name\":\"name\",\"format\":\"s\"' \
    "$tmp/lower_string_literals.ir"
grep -Fq '\"offset\":1,\"position\":0' \
    "$tmp/lower_string_literals.ir"
grep -Fq '!ir.const.bytes("c2ab6c69746572616cc2bb205b7c74657874")' \
    "$tmp/lower_string_literals.ir"
grep -Fq '\"tag\":\"bold\",\"start\":0,\"end\":null' \
    "$tmp/lower_string_literals.ir"
grep -Fq 'func @lower_u32_string() -> array[char]' \
    "$tmp/lower_string_literals.ir"
grep -Fq 'char = const 65' "$tmp/lower_string_literals.ir"
grep -Fq 'char = const 955' "$tmp/lower_string_literals.ir"
grep -Fq 'char = const 0' "$tmp/lower_string_literals.ir"
grep -Fq 'array[char] = aggregate' "$tmp/lower_string_literals.ir"
grep -Fq '!n00bb.literal.modifier("u32")' \
    "$tmp/lower_string_literals.ir"

for legacy in codepoint f32; do
    if "$compiler" lower "$program_dir/legacy_${legacy}_type.n00b" \
        >"$tmp/legacy_${legacy}.out" 2>"$tmp/legacy_${legacy}.err"; then
        echo "legacy type '$legacy' unexpectedly compiled" >&2
        exit 1
    fi
    grep -Fq "no visible type named '$legacy'" \
        "$tmp/legacy_${legacy}.err"
done

"$compiler" lower --dump-ir "$program_dir/char_scalar.n00b" \
    >"$tmp/char_scalar.ir" 2>"$tmp/char_scalar.err" || {
    cat "$tmp/char_scalar.err"
    exit 1
}
for value in 65 10 0 955; do
    grep -Fq "char = const $value" "$tmp/char_scalar.ir"
done
if "$compiler" lower "$program_dir/empty_char_literal.n00b" \
    >"$tmp/empty_char.out" 2>"$tmp/empty_char.err"; then
    echo 'empty character literal unexpectedly compiled' >&2
    exit 1
fi
grep -Fq 'character literal must contain exactly one scalar' \
    "$tmp/empty_char.err"

if "$compiler" lower --dump-ir \
    "$script_dir/programs/lower_rich_mixed_substitutions.n00b" \
    >"$tmp/lower_rich_mixed_substitutions.out" 2>&1; then
    echo 'mixed automatic and explicit rich substitutions unexpectedly compiled' >&2
    exit 1
fi
grep -q 'cannot mix automatic and explicit positional substitutions' \
    "$tmp/lower_rich_mixed_substitutions.out"

if "$compiler" lower --dump-ir \
    "$script_dir/programs/lower_rich_unclosed_ascii_tag.n00b" \
    >"$tmp/lower_rich_unclosed_ascii_tag.out" 2>&1; then
    echo 'unclosed rich ASCII tag unexpectedly compiled' >&2
    exit 1
fi
grep -q 'unclosed ASCII tag' "$tmp/lower_rich_unclosed_ascii_tag.out"

if "$compiler" lower --dump-ir \
    "$script_dir/programs/lower_unregistered_string_modifier.n00b" \
    >"$tmp/lower_unregistered_string_modifier.out" 2>&1; then
    echo 'unregistered string modifier unexpectedly compiled' >&2
    exit 1
fi
grep -q 'no literal handler is registered for this string payload shape' \
    "$tmp/lower_unregistered_string_modifier.out"

if "$compiler" lower --dump-ir \
    "$script_dir/programs/lower_unregistered_fence_modifier.n00b" \
    >"$tmp/lower_unregistered_fence_modifier.out" 2>&1; then
    echo 'unregistered fence modifier unexpectedly compiled' >&2
    exit 1
fi
grep -q 'no literal handler is registered for this modifier and payload shape' \
    "$tmp/lower_unregistered_fence_modifier.out"

if "$compiler" lower --dump-ir \
    "$script_dir/programs/lower_unsupported_string_shape.n00b" \
    >"$tmp/lower_unsupported_string_shape.out" 2>&1; then
    echo 'string modifier on a non-string literal shape unexpectedly compiled' >&2
    exit 1
fi
grep -q 'literal modifier is not supported for this payload shape' \
    "$tmp/lower_unsupported_string_shape.out"

"$compiler" lower --dump-ir "$program_dir/lower_module_initializers.n00b" \
    >"$tmp/lower_module_initializers.ir" \
    2>"$tmp/lower_module_initializers.err" || {
    cat "$tmp/lower_module_initializers.err"
    exit 1
}
grep -Eq '@__n00bb_runtime_init\(\) -> nil .*!n00bb.initializer\("runtime"\)' \
    "$tmp/lower_module_initializers.ir"
grep -Eq '@__n00bb_comptime_init\(\) -> nil .*!ir.execution_phase\(1\).*!n00bb.initializer\("comptime"\)' \
    "$tmp/lower_module_initializers.ir"
awk '
    /^  static func @__n00bb_runtime_init/ { in_runtime = 1 }
    /^  static func @__n00bb_comptime_init/ { in_runtime = 0 }
    in_runtime && /call @initial_value/ { calls++ }
    in_runtime && /global.store @.*dynamic/ { stores++ }
    in_runtime && /global.store @.*static_value/ { static_effect_leaked = 1 }
    in_runtime && /const 4/ { comptime_leaked = 1 }
    END { exit !(calls == 2 && stores == 2 && !static_effect_leaked && !comptime_leaked) }
' "$tmp/lower_module_initializers.ir"
awk '
    /^  static func @__n00bb_comptime_init/ { in_comptime = 1 }
    /^  static func @__n00bb_runtime_init/ { in_comptime = 0 }
    in_comptime && /const 4/ { compile_time_value = 1 }
    in_comptime && /global.store @.*static_value/ { compile_time_global_effect = 1 }
    END { exit !(compile_time_value && compile_time_global_effect) }
' "$tmp/lower_module_initializers.ir"

"$compiler" lower --dump-control-flow \
    "$program_dir/lower_module_initializers.n00b" \
    >"$tmp/lower_module_initializers.graph" \
    2>"$tmp/lower_module_initializers.graph.err" || {
    cat "$tmp/lower_module_initializers.graph.err"
    exit 1
}
grep -q '^execution-graph:$' "$tmp/lower_module_initializers.graph"
grep -q 'phase #0 "comptime"' "$tmp/lower_module_initializers.graph"
grep -q 'phase #1 "runtime"' "$tmp/lower_module_initializers.graph"
grep -q 'function=lower_module_initializers\$comptime.__n00bb_comptime_init' \
    "$tmp/lower_module_initializers.graph"
grep -q 'edge requires #1 -> #3' "$tmp/lower_module_initializers.graph"
"$compiler" lower --dump-control-flow \
    "$program_dir/lower_module_initializers.n00b" \
    >"$tmp/lower_module_initializers.graph.repeat" \
    2>"$tmp/lower_module_initializers.graph.repeat.err" || {
    cat "$tmp/lower_module_initializers.graph.repeat.err"
    exit 1
}
cmp "$tmp/lower_module_initializers.graph" \
    "$tmp/lower_module_initializers.graph.repeat"

"$compiler" lower --dump-ir --dump-control-flow --dump-pdg \
    "$program_dir/lower_module_initializers.n00b" \
    >"$tmp/lower_module_initializers.all_dumps" \
    2>"$tmp/lower_module_initializers.all_dumps.err" || {
    cat "$tmp/lower_module_initializers.all_dumps.err"
    exit 1
}
grep -q '^module lower_module_initializers\$comptime' \
    "$tmp/lower_module_initializers.all_dumps"
grep -Fq 'module lower_module_initializers {' \
    "$tmp/lower_module_initializers.all_dumps"
grep -q '^control-flow @__n00bb_comptime_init$' \
    "$tmp/lower_module_initializers.all_dumps"
grep -q '^pdg @__n00bb_runtime_init ' \
    "$tmp/lower_module_initializers.all_dumps"
grep -q '^execution-graph:$' "$tmp/lower_module_initializers.all_dumps"
grep -q 'phase #0 "comptime"' "$tmp/lower_module_initializers.all_dumps"
grep -q 'phase #1 "runtime"' "$tmp/lower_module_initializers.all_dumps"
grep -q 'edge requires #1 -> #3' \
    "$tmp/lower_module_initializers.all_dumps"
"$compiler" lower --dump-ir --dump-control-flow --dump-pdg \
    "$program_dir/lower_module_initializers.n00b" \
    >"$tmp/lower_module_initializers.all_dumps.repeat" \
    2>"$tmp/lower_module_initializers.all_dumps.repeat.err" || {
    cat "$tmp/lower_module_initializers.all_dumps.repeat.err"
    exit 1
}
cmp "$tmp/lower_module_initializers.all_dumps" \
    "$tmp/lower_module_initializers.all_dumps.repeat"

"$compiler" lower --dump-ir --dump-control-flow \
    "$program_dir/lower_comptime_initializer_failure.n00b" \
    >"$tmp/lower_comptime_initializer_failure.out" \
    2>"$tmp/lower_comptime_initializer_failure.err" || {
    cat "$tmp/lower_comptime_initializer_failure.err"
    exit 1
}
grep -Eq '@__n00bb_comptime_init\(\) -> nil fail: int .*ir.execution_phase\(1\)' \
    "$tmp/lower_comptime_initializer_failure.out"
grep -q 'outcome=failure' "$tmp/lower_comptime_initializer_failure.out"
grep -q 'edge requires #1 -> #3' \
    "$tmp/lower_comptime_initializer_failure.out"
comptime_initializer_failure_exit=$(sed -E -n \
    's/^  node #([0-9]+) function-exit phase="comptime" function=lower_comptime_initializer_failure\$comptime\.__n00bb_comptime_init .*outcome=failure/\1/p' \
    "$tmp/lower_comptime_initializer_failure.out")
test -n "$comptime_initializer_failure_exit"
if grep -q "edge requires #1 -> #$comptime_initializer_failure_exit" \
    "$tmp/lower_comptime_initializer_failure.out"; then
    echo 'a failed compile-time initializer exit unexpectedly gates runtime' >&2
    exit 1
fi

"$compiler" lower --dump-ir \
    "$program_dir/lower_comptime_function_call.n00b" \
    >"$tmp/lower_comptime_function_call.ir" \
    2>"$tmp/lower_comptime_function_call.err" || {
    cat "$tmp/lower_comptime_function_call.err"
    exit 1
}
grep -q 'module lower_comptime_function_call\$comptime {' \
    "$tmp/lower_comptime_function_call.ir"
grep -q 'func @__n00bb_comptime_compile_value().*!ir.execution_phase(1)' \
    "$tmp/lower_comptime_function_call.ir"
awk '
    /^module / { in_comptime = /\$comptime/ }
    in_comptime && /call @__n00bb_comptime_compile_value/ { compile_call = 1 }
    !in_comptime && /call @__n00bb_comptime_compile_value/ { leaked_call = 1 }
    END { exit !(compile_call && !leaked_call) }
' "$tmp/lower_comptime_function_call.ir"

"$compiler" lower --dump-ir \
    "$program_dir/lower_comptime_runtime_dependency.n00b" \
    >"$tmp/lower_comptime_runtime_dependency.out" 2>&1 || {
    cat "$tmp/lower_comptime_runtime_dependency.out"
    exit 1
}
grep -q 'call @__n00bb_comptime_compile_value' \
    "$tmp/lower_comptime_runtime_dependency.out"
grep -q 'return @lower_comptime_runtime_dependency.runtime_value' \
    "$tmp/lower_comptime_runtime_dependency.out"

for fixture in lower_nested_comptime lower_comptime_value_body; do
    if "$compiler" parse "$program_dir/$fixture.n00b" \
        >"$tmp/$fixture.rejected" 2>&1; then
        echo "comptime expression unexpectedly parsed: $fixture" >&2
        exit 1
    fi
    grep -q 'syntax error.*near comptime' "$tmp/$fixture.rejected"
done

N00B_PATH="$program_dir" "$compiler" lower --dump-ir --dump-control-flow \
    lower_comptime_import.root \
    >"$tmp/lower_comptime_import_root.out" \
    2>"$tmp/lower_comptime_import_root.err" || {
    cat "$tmp/lower_comptime_import_root.err"
    exit 1
}
grep -q 'call @__n00bb_comptime_compile_value' \
    "$tmp/lower_comptime_import_root.out"
grep -q 'call @__n00bb_comptime_compile_constant' \
    "$tmp/lower_comptime_import_root.out"
leaf_exit=$(sed -n \
    's/^  node #\([0-9][0-9]*\) function-exit phase="comptime" function=lower_comptime_import\.leaf\$comptime\.__n00bb_comptime_init .*outcome=success/\1/p' \
    "$tmp/lower_comptime_import_root.out")
middle_entry=$(sed -n \
    's/^  node #\([0-9][0-9]*\) function-entry phase="comptime" function=lower_comptime_import\.middle\$comptime\.__n00bb_comptime_init .*/\1/p' \
    "$tmp/lower_comptime_import_root.out")
middle_exit=$(sed -n \
    's/^  node #\([0-9][0-9]*\) function-exit phase="comptime" function=lower_comptime_import\.middle\$comptime\.__n00bb_comptime_init .*outcome=success/\1/p' \
    "$tmp/lower_comptime_import_root.out")
root_entry=$(sed -n \
    's/^  node #\([0-9][0-9]*\) function-entry phase="comptime" function=lower_comptime_import\.root\$comptime\.__n00bb_comptime_init .*/\1/p' \
    "$tmp/lower_comptime_import_root.out")
test -n "$leaf_exit" && test -n "$middle_entry" \
    && test -n "$middle_exit" && test -n "$root_entry"
grep -q "edge sequence #$leaf_exit -> #$middle_entry" \
    "$tmp/lower_comptime_import_root.out"
grep -q "edge sequence #$middle_exit -> #$root_entry" \
    "$tmp/lower_comptime_import_root.out"

"$compiler" lower --dump-ir --dump-link-unit \
    "$program_dir/lower_comptime_trait_call.n00b" \
    >"$tmp/lower_comptime_trait_call.out" \
    2>"$tmp/lower_comptime_trait_call.err" || {
    cat "$tmp/lower_comptime_trait_call.err"
    exit 1
}
grep -q 'call @__n00bb_comptime_render' \
    "$tmp/lower_comptime_trait_call.out"
grep -q 'trait-dispatch\[resolved\].*-> lower_comptime_trait_call:1:render' \
    "$tmp/lower_comptime_trait_call.out"

"$compiler" lower --dump-ir \
    "$program_dir/lower_comptime_context_hooks.n00b" \
    >"$tmp/lower_comptime_context_hooks.out" \
    2>"$tmp/lower_comptime_context_hooks.err" || {
    cat "$tmp/lower_comptime_context_hooks.err"
    exit 1
}
enter_line=$(grep -n 'call @__n00bb_context_ValueContext_enter' \
    "$tmp/lower_comptime_context_hooks.out" | head -1 | cut -d: -f1)
exit_line=$(grep -n 'call @__n00bb_context_ValueContext_exit' \
    "$tmp/lower_comptime_context_hooks.out" | head -1 | cut -d: -f1)
ensure_line=$(grep -n 'call @__n00bb_context_ValueContext_ensure' \
    "$tmp/lower_comptime_context_hooks.out" | head -1 | cut -d: -f1)
test -n "$enter_line" && test -n "$exit_line" && test -n "$ensure_line"
test "$enter_line" -lt "$exit_line" && test "$exit_line" -lt "$ensure_line"

"$compiler" lower --dump-control-flow "$program_dir/lower_global.n00b" \
    >"$tmp/lower_global.graph" 2>"$tmp/lower_global.graph.err" || {
    cat "$tmp/lower_global.graph.err"
    exit 1
}
grep -q 'label="empty-initialization-phase" outcome=success' \
    "$tmp/lower_global.graph"
grep -q 'edge requires #1 -> #2' "$tmp/lower_global.graph"

if "$compiler" lower "$program_dir/lower_module_initializer_failure.n00b" \
    >"$tmp/lower_module_initializer_failure.out" 2>&1; then
    echo 'a failing top-level initializer unexpectedly lowered' >&2
    exit 1
fi
grep -q 'a runtime module initializer has no failure channel' \
    "$tmp/lower_module_initializer_failure.out" || {
    cat "$tmp/lower_module_initializer_failure.out"
    exit 1
}

"$compiler" lower --dump-ir "$program_dir/lower_record_new.n00b" \
    >"$tmp/lower_record_new.ir" 2>"$tmp/lower_record_new.err" || {
    cat "$tmp/lower_record_new.err"
    exit 1
}
grep -q 'alloc lower_record_new.ConstructedRecord' \
    "$tmp/lower_record_new.ir"
grep -q 'call @__init' "$tmp/lower_record_new.ir"
grep -q 'field .*"payload"' "$tmp/lower_record_new.ir"

"$compiler" lower --dump-ir "$program_dir/unicode_type_nfc.n00b" \
    >"$tmp/unicode_type_nfc.ir" 2>"$tmp/unicode_type_nfc.err" || {
    cat "$tmp/unicode_type_nfc.err"
    exit 1
}
grep -q 'type unicode_type_nfc.Café = (value: int)' \
    "$tmp/unicode_type_nfc.ir"
grep -q 'func @read_cafe(%value: unicode_type_nfc.Café' \
    "$tmp/unicode_type_nfc.ir"

N00B_PATH="$program_dir" "$compiler" lower --dump-ir --dump-link-unit \
    "$program_dir/constructor_imported.n00b" \
    >"$tmp/constructor_imported.ir" 2>"$tmp/constructor_imported.err" || {
    cat "$tmp/constructor_imported.err"
    exit 1
}
grep -q 'type constructor_dep.impl.ImportedRecord = (payload: int)' \
    "$tmp/constructor_imported.ir"
grep -Eq 'call @constructor_dep\.impl\.__init|call @__init' \
    "$tmp/constructor_imported.ir"
grep -q 'constructor_dep.impl:.*__init' "$tmp/constructor_imported.ir"

N00B_PATH="$program_dir" "$compiler" lower --dump-ir --dump-link-unit \
    "$program_dir/constructor_specificity.n00b" \
    >"$tmp/constructor_specificity.ir" \
    2>"$tmp/constructor_specificity.err" || {
    cat "$tmp/constructor_specificity.err"
    exit 1
}
awk '
    /^  references:/ { in_references = 1; next }
    /^  dependencies:/ { in_references = 0 }
    in_references { print }
' "$tmp/constructor_specificity.ir" >"$tmp/constructor_specificity.refs"
grep -q 'constructor_specific:1:__init' \
    "$tmp/constructor_specificity.refs"
if grep -q 'constructor_general:1:__init' \
    "$tmp/constructor_specificity.refs"; then
    echo 'generic constructor was selected over the exact specialization' >&2
    exit 1
fi

if N00B_PATH="$program_dir" "$compiler" lower \
    "$program_dir/constructor_ambiguous.n00b" \
    >"$tmp/constructor_ambiguous.out" 2>&1; then
    echo 'equally specific imported constructors unexpectedly resolved' >&2
    exit 1
fi
grep -q 'N00BB-T0021.*multiple equally specific __init functions' \
    "$tmp/constructor_ambiguous.out" || {
    cat "$tmp/constructor_ambiguous.out"
    exit 1
}

if "$compiler" lower "$program_dir/constructor_missing_init.n00b" \
    >"$tmp/constructor_missing_init.out" 2>&1; then
    echo 'new without an applicable __init unexpectedly lowered' >&2
    exit 1
fi
grep -q 'N00BB-T0020.*no visible __init matches this constructed type' \
    "$tmp/constructor_missing_init.out" || {
    cat "$tmp/constructor_missing_init.out"
    exit 1
}

if "$compiler" lower "$program_dir/constructor_failing_init.n00b" \
    >"$tmp/constructor_failing_init.out" 2>&1; then
    echo '__init with a failure channel unexpectedly lowered' >&2
    exit 1
fi
grep -q 'N00BB-T0018.*__init cannot declare or infer a failure channel' \
    "$tmp/constructor_failing_init.out" || {
    cat "$tmp/constructor_failing_init.out"
    exit 1
}

if "$compiler" lower "$program_dir/constructor_wrong_receiver_mode.n00b" \
    >"$tmp/constructor_wrong_receiver_mode.out" 2>&1; then
    echo '__init without a modify receiver unexpectedly lowered' >&2
    exit 1
fi
grep -q 'N00BB-T0017.*first modify parameter' \
    "$tmp/constructor_wrong_receiver_mode.out" || {
    cat "$tmp/constructor_wrong_receiver_mode.out"
    exit 1
}

if "$compiler" lower "$program_dir/constructor_returning_init.n00b" \
    >"$tmp/constructor_returning_init.out" 2>&1; then
    echo '__init with a return value unexpectedly lowered' >&2
    exit 1
fi
grep -q 'N00BB-T0019.*__init must return no value' \
    "$tmp/constructor_returning_init.out" || {
    cat "$tmp/constructor_returning_init.out"
    exit 1
}

if "$compiler" lower \
    "$program_dir/constructor_variant_init_rejected.n00b" \
    >"$tmp/constructor_variant_init_rejected.out" 2>&1; then
    echo 'variant-owned __init unexpectedly lowered' >&2
    exit 1
fi
grep -q 'N00BB-T0022.*variant types cannot define __init' \
    "$tmp/constructor_variant_init_rejected.out" || {
    cat "$tmp/constructor_variant_init_rejected.out"
    exit 1
}

"$compiler" lower --dump-ir \
    "$program_dir/generic_modify_parameter.n00b" \
    >"$tmp/generic_modify_parameter.ir" \
    2>"$tmp/generic_modify_parameter.err" || {
    cat "$tmp/generic_modify_parameter.err"
    exit 1
}
grep -Fq 'func @assign_same(inout %item: `a) -> nil fail: nil' \
    "$tmp/generic_modify_parameter.ir"
if grep -Fq 'forall[' "$tmp/generic_modify_parameter.ir"; then
    echo 'generic function lowering retained a forall binder' >&2
    exit 1
fi
grep -q 'call @assign_same' "$tmp/generic_modify_parameter.ir"
grep -q '!ir.call_signature((inout int) -> nil fail: nil)' \
    "$tmp/generic_modify_parameter.ir"
grep -q '= call.output .* 0' \
    "$tmp/generic_modify_parameter.ir"

N00B_PATH="$program_dir" "$compiler" lower --dump-ir init_root \
    >"$tmp/init_order.ir" 2>"$tmp/init_order.err" || {
    cat "$tmp/init_order.err"
    exit 1
}
awk '
    /^module init_dependency/ { current = "dependency" }
    /^module init_root/ { current = "root" }
    current == "dependency" && /__n00bb_runtime_init/ && /initializer_order\(0\)/ { dep_order = 1 }
    current == "root" && /__n00bb_runtime_init/ && /initializer_order\(1\)/ { root_order = 1 }
    END { exit !(dep_order && root_order) }
' "$tmp/init_order.ir"

N00B_PATH="$program_dir" "$compiler" lower --dump-control-flow \
    init_root >"$tmp/init_order.graph" \
    2>"$tmp/init_order.graph.err" || {
    cat "$tmp/init_order.graph.err"
    exit 1
}
dependency_entry=$(sed -E -n \
    's/^  node #([0-9]+) function-entry .*function=init_dependency\.__n00bb_runtime_init.*/\1/p' \
    "$tmp/init_order.graph")
dependency_exit=$(sed -E -n \
    's/^  node #([0-9]+) function-exit .*function=init_dependency\.__n00bb_runtime_init.*outcome=success/\1/p' \
    "$tmp/init_order.graph")
root_entry=$(sed -E -n \
    's/^  node #([0-9]+) function-entry .*function=init_root\.__n00bb_runtime_init.*/\1/p' \
    "$tmp/init_order.graph")
runtime_entry=$(sed -E -n \
    's/^  node #([0-9]+) phase-entry .*phase="runtime".*/\1/p' \
    "$tmp/init_order.graph")
test -n "$dependency_entry" && test -n "$dependency_exit" \
    && test -n "$root_entry" && test -n "$runtime_entry"
grep -q "edge sequence #$runtime_entry -> #$dependency_entry" \
    "$tmp/init_order.graph"
grep -q "edge sequence #$dependency_exit -> #$root_entry" \
    "$tmp/init_order.graph"

N00B_PATH="$program_dir" "$compiler" lower --dump-ir init_cycle_a \
    >"$tmp/init_cycle_order.ir" 2>"$tmp/init_cycle_order.err" || {
    cat "$tmp/init_cycle_order.err"
    exit 1
}
awk '
    /^module init_cycle_a/ { current = "a" }
    /^module init_cycle_b/ { current = "b" }
    current == "a" && /__n00bb_comptime_init/ { a_comptime = 1 }
    current == "a" && /__n00bb_runtime_init/ { a_runtime = 1 }
    current == "b" && /__n00bb_comptime_init/ { b_comptime = 1 }
    current == "b" && /__n00bb_runtime_init/ { b_runtime = 1 }
    current == "a" && /__n00bb_comptime_init/ && /initializer_order\(0\)/ { a_comptime_order = 1 }
    current == "a" && /__n00bb_runtime_init/ && /initializer_order\(0\)/ { a_runtime_order = 1 }
    current == "b" && /__n00bb_comptime_init/ && /initializer_order\(1\)/ { b_comptime_order = 1 }
    current == "b" && /__n00bb_runtime_init/ && /initializer_order\(1\)/ { b_runtime_order = 1 }
    END {
        exit !(a_comptime && a_runtime && b_comptime && b_runtime &&
               a_comptime_order && a_runtime_order &&
               b_comptime_order && b_runtime_order)
    }
' "$tmp/init_cycle_order.ir"

N00B_PATH="$program_dir" "$compiler" lower --dump-control-flow \
    init_cycle_a >"$tmp/init_cycle_order.graph" \
    2>"$tmp/init_cycle_order.graph.err" || {
    cat "$tmp/init_cycle_order.graph.err"
    exit 1
}
awk '
    /^execution-graph:/ { in_graph = 1 }
    in_graph && /function-entry .*\.__n00bb_comptime_init/ { comptime++ }
    in_graph && /function-entry .*\.__n00bb_runtime_init/ { runtime++ }
    END { exit !(comptime == 2 && runtime == 2) }
' "$tmp/init_cycle_order.graph"
cycle_comptime_a_exit=$(sed -E -n \
    's/^  node #([0-9]+) function-exit phase="comptime" function=init_cycle_a\$comptime\.__n00bb_comptime_init .*outcome=success/\1/p' \
    "$tmp/init_cycle_order.graph")
cycle_comptime_b_entry=$(sed -E -n \
    's/^  node #([0-9]+) function-entry phase="comptime" function=init_cycle_b\$comptime\.__n00bb_comptime_init .*/\1/p' \
    "$tmp/init_cycle_order.graph")
cycle_runtime_a_exit=$(sed -E -n \
    's/^  node #([0-9]+) function-exit phase="runtime" function=init_cycle_a\.__n00bb_runtime_init .*outcome=success/\1/p' \
    "$tmp/init_cycle_order.graph")
cycle_runtime_b_entry=$(sed -E -n \
    's/^  node #([0-9]+) function-entry phase="runtime" function=init_cycle_b\.__n00bb_runtime_init .*/\1/p' \
    "$tmp/init_cycle_order.graph")
cycle_comptime_b_exit=$(sed -E -n \
    's/^  node #([0-9]+) function-exit phase="comptime" function=init_cycle_b\$comptime\.__n00bb_comptime_init .*outcome=success/\1/p' \
    "$tmp/init_cycle_order.graph")
test -n "$cycle_comptime_a_exit" && test -n "$cycle_comptime_b_entry" \
    && test -n "$cycle_runtime_a_exit" && test -n "$cycle_runtime_b_entry" \
    && test -n "$cycle_comptime_b_exit"
grep -q "edge sequence #$cycle_comptime_a_exit -> #$cycle_comptime_b_entry" \
    "$tmp/init_cycle_order.graph"
grep -q "edge sequence #$cycle_runtime_a_exit -> #$cycle_runtime_b_entry" \
    "$tmp/init_cycle_order.graph"
grep -q "edge requires #1 -> #$cycle_comptime_b_exit" \
    "$tmp/init_cycle_order.graph"

if "$compiler" parse "$program_dir/lower_function_comptime_block.n00b" \
    >"$tmp/lower_function_comptime_block.out" 2>&1; then
    echo 'comptime block inside a function unexpectedly parsed' >&2
    exit 1
fi
grep -q 'syntax error.*near comptime' \
    "$tmp/lower_function_comptime_block.out"

"$compiler" lower --dump-ir --dump-control-flow \
    "$program_dir/lower_comptime_global_final_state.n00b" \
    >"$tmp/lower_comptime_global_final_state.out" \
    2>"$tmp/lower_comptime_global_final_state.err" || {
    cat "$tmp/lower_comptime_global_final_state.err"
    exit 1
}
awk '
    /^module / { in_comptime = $0 ~ /\$comptime/ }
    in_comptime && /global.store @lower_comptime_global_final_state.phase_value/ {
        phase_writes++
    }
    in_comptime && /global.store @lower_comptime_global_final_state.dynamic_value/ {
        comptime_dynamic_write = 1
    }
    !in_comptime && /global.store @lower_comptime_global_final_state.dynamic_value/ {
        runtime_dynamic_init = 1
    }
    !in_comptime && /global.store @lower_comptime_global_final_state.phase_value/ {
        runtime_phase_write = 1
    }
    END {
        exit !(phase_writes >= 3 && comptime_dynamic_write \
            && runtime_dynamic_init && !runtime_phase_write)
    }
' "$tmp/lower_comptime_global_final_state.out"
grep -Fq 'global @lower_comptime_global_final_state.phase_value: int = $"default:lower_comptime_global_final_state:global-state:phase_value"' \
    "$tmp/lower_comptime_global_final_state.out"
grep -Fq 'global @lower_comptime_global_final_state.dynamic_value: int = $"default:lower_comptime_global_final_state:global-state:dynamic_value"' \
    "$tmp/lower_comptime_global_final_state.out"
grep -Fq 'global-result "default:lower_comptime_global_final_state:global-state:phase_value" phase="comptime"' \
    "$tmp/lower_comptime_global_final_state.out"
grep -Fq 'global-result "default:lower_comptime_global_final_state:global-state:dynamic_value" phase="comptime"' \
    "$tmp/lower_comptime_global_final_state.out"

"$compiler" lower --dump-ir --dump-control-flow \
    "$program_dir/lower_comptime_unreachable_global_write.n00b" \
    >"$tmp/lower_comptime_unreachable_global_write.out" \
    2>"$tmp/lower_comptime_unreachable_global_write.err" || {
    cat "$tmp/lower_comptime_unreachable_global_write.err"
    cat "$tmp/lower_comptime_unreachable_global_write.out"
    exit 1
}
grep -Fq 'global @lower_comptime_unreachable_global_write.result: int = 0' \
    "$tmp/lower_comptime_unreachable_global_write.out" || {
    cat "$tmp/lower_comptime_unreachable_global_write.out"
    exit 1
}
if grep -Fq 'global-result "default:lower_comptime_unreachable_global_write:global-state:result"' \
    "$tmp/lower_comptime_unreachable_global_write.out"; then
    echo 'an unreachable compile-time write unexpectedly produced a phase result' >&2
    cat "$tmp/lower_comptime_unreachable_global_write.out"
    exit 1
fi

"$compiler" lower --dump-ir "$program_dir/lower_default_arguments.n00b" \
    >"$tmp/lower_default_arguments.ir" \
    2>"$tmp/lower_default_arguments.err" || {
    cat "$tmp/lower_default_arguments.err"
    exit 1
}
test "$(grep -c 'call @defaulted,' "$tmp/lower_default_arguments.ir")" -eq 2
grep -Fq 'call @defaulted, missing, missing,' \
    "$tmp/lower_default_arguments.ir"
grep -Eq 'call @defaulted, %__lowered_[0-9]+, missing,' \
    "$tmp/lower_default_arguments.ir"
grep -Eq '= param\.present %__n00bb_default_param_[0-9]+' \
    "$tmp/lower_default_arguments.ir"
grep -Eq 'call @defaulted, .*!ir.call_signature\(\(int, int, int\)' \
    "$tmp/lower_default_arguments.ir"
grep -Eq 'call @named, %__lowered_[0-9]+, %__lowered_[0-9]+' \
    "$tmp/lower_default_arguments.ir"
if grep -Eq '__n00bb_default_arg_|extract .*"present"' \
    "$tmp/lower_default_arguments.ir"; then
    echo 'default arguments retained a presence/value wrapper' >&2
    exit 1
fi
grep -q 'add trap' "$tmp/lower_default_arguments.ir"
"$compiler" lower --dump-ir \
    "$program_dir/lower_default_declaration_scope.n00b" \
    >"$tmp/lower_default_declaration_scope.ir" \
    2>"$tmp/lower_default_declaration_scope.err" || {
    cat "$tmp/lower_default_declaration_scope.err"
    exit 1
}
awk '
    /^  func @default_from_declaration\(/ { in_default = 1 }
    /^  func @/ && !/default_from_declaration/ && in_default { exit }
    in_default && /copy @lower_default_declaration_scope\.default_seed/ {
        found = 1
    }
    END { exit !found }
' "$tmp/lower_default_declaration_scope.ir"
awk '
    /^  (static )?func @use_named_evaluation_order/ { in_call = 1 }
    /^  (static )?func @/ && !/use_named_evaluation_order/ && in_call { exit }
    in_call && /call @second_value/ { second = NR }
    in_call && /call @first_value/ { first = NR }
    END { exit !(second && first && second < first) }
' "$tmp/lower_default_arguments.ir"
awk '
    /^  (static )?func @dependency_order\(/ { in_default = 1; next }
    /^  (static )?func @/ && in_default { exit }
    in_default && /call @default_a/ && !a { a = NR }
    in_default && /call @default_b/ && !b { b = NR }
    in_default && /call @default_c/ && !c { c = NR }
    END { exit !(b && a && c && b < a && a < c) }
' "$tmp/lower_default_arguments.ir"

"$compiler" lower --dump-ir \
    "$program_dir/lower_default_depth_first.n00b" \
    >"$tmp/lower_default_depth_first.ir" \
    2>"$tmp/lower_default_depth_first.err" || {
    cat "$tmp/lower_default_depth_first.err"
    exit 1
}
awk '
    /^  func @depth_first_defaults\(/ { in_default = 1 }
    /^  func @/ && !/depth_first_defaults/ && in_default { exit }
    in_default && /call @default_order_d/ && !d { d = NR }
    in_default && /call @default_order_c/ && !c { c = NR }
    in_default && /call @default_order_pair/ && !a { a = NR }
    in_default && /call @default_order_independent/ && !b { b = NR }
    END { exit !(d && c && a && b && d < c && c < a && a < b) }
' "$tmp/lower_default_depth_first.ir"

if "$compiler" lower "$program_dir/lower_missing_required_argument.n00b" \
    >"$tmp/lower_missing_required_argument.out" 2>&1; then
    echo 'a missing required argument unexpectedly lowered' >&2
    exit 1
fi
grep -q 'call omits a required parameter' \
    "$tmp/lower_missing_required_argument.out"

if "$compiler" lower "$program_dir/lower_cyclic_defaults.n00b" \
    >"$tmp/lower_cyclic_defaults.out" 2>&1; then
    echo 'cyclic default dependencies unexpectedly lowered' >&2
    exit 1
fi
grep -q 'default parameter dependencies are cyclic' \
    "$tmp/lower_cyclic_defaults.out"

"$compiler" lower "$program_dir/lower_parameter_modes.n00b" \
    >"$tmp/lower_parameter_modes.ir" \
    2>"$tmp/lower_parameter_modes.err" || {
    cat "$tmp/lower_parameter_modes.err"
    exit 1
}
grep -q '!ir.call_signature((out int, int) -> nil fail: nil)' \
    "$tmp/lower_parameter_modes.ir"
grep -q '!ir.call_signature((inout int, int) -> nil fail: nil)' \
    "$tmp/lower_parameter_modes.ir"
grep -q 'call.output .* 0' "$tmp/lower_parameter_modes.ir"
test "$(grep -c 'call.output .* 0' "$tmp/lower_parameter_modes.ir")" -eq 2
grep -q '= call @copy_modify' "$tmp/lower_parameter_modes.ir"
grep -q '= call @write_out' "$tmp/lower_parameter_modes.ir"

"$compiler" lower --dump-ir --dump-symbols \
    "$program_dir/consume_parameter_modes.n00b" \
    >"$tmp/consume_parameter_modes.out" \
    2>"$tmp/consume_parameter_modes.err" || {
    cat "$tmp/consume_parameter_modes.err"
    exit 1
}
grep -q 'item: parameter.*direction=consume' "$tmp/consume_parameter_modes.out"
grep -q 'fallback: parameter.*optional.*direction=consume' \
    "$tmp/consume_parameter_modes.out"
grep -q 'items: parameter.*direction=consume.*variadic' \
    "$tmp/consume_parameter_modes.out"
grep -q 'func @take(consume %item: int.*consume %__n00bb_default_param_.*consume \*%items: int)' \
    "$tmp/consume_parameter_modes.out"
grep -Fq '!ir.call_signature((consume int, consume int, consume *int) -> nil fail: nil)' \
    "$tmp/consume_parameter_modes.out"
! grep -q 'call.output' "$tmp/consume_parameter_modes.out"
test "$(grep -c '= consume ' "$tmp/consume_parameter_modes.out")" -eq 5

for fixture in consume_use_after consume_double consume_branch_join \
               consume_loop consume_alias consume_global; do
    if "$compiler" lower --dump-ir "$program_dir/$fixture.n00b" \
        >"$tmp/$fixture.out" 2>&1; then
        echo "$fixture unexpectedly accepted an unavailable consumed binding" >&2
        exit 1
    fi
    grep -Eq 'used after consume|consumed more than once' \
        "$tmp/$fixture.out" || {
        cat "$tmp/$fixture.out"
        exit 1
    }
done

if "$compiler" lower --dump-ir \
    "$program_dir/consume_escaping_alias.n00b" \
    >"$tmp/consume_escaping_alias.out" 2>&1; then
    echo 'consume unexpectedly accepted an escaping alias' >&2
    exit 1
fi
grep -q 'aliases to this binding are tracked and non-escaping' \
    "$tmp/consume_escaping_alias.out"

if "$compiler" lower --dump-ir \
    "$program_dir/consume_old_value_alias.n00b" \
    >"$tmp/consume_old_value_alias.out" 2>&1; then
    echo 'consume unexpectedly revived an alias into the old value' >&2
    exit 1
fi
grep -q "binding 'local' is used after consume" \
    "$tmp/consume_old_value_alias.out"

for fixture in consume_subobject consume_index_subobject; do
    if "$compiler" lower --dump-ir "$program_dir/$fixture.n00b" \
        >"$tmp/$fixture.out" 2>&1; then
        echo "$fixture unexpectedly consumed a subobject" >&2
        exit 1
    fi
    grep -q 'consume requires a whole binding or a temporary value' \
        "$tmp/$fixture.out" || {
        cat "$tmp/$fixture.out"
        exit 1
    }
done

"$compiler" lower --dump-ir "$program_dir/consume_reassigned.n00b" \
    >"$tmp/consume_reassigned.out" 2>&1 || {
    cat "$tmp/consume_reassigned.out"
    exit 1
}
test "$(grep -c '= consume ' "$tmp/consume_reassigned.out")" -eq 2

"$compiler" lower --dump-ir "$program_dir/consume_alias_reassigned.n00b" \
    >"$tmp/consume_alias_reassigned.out" 2>&1 || {
    cat "$tmp/consume_alias_reassigned.out"
    exit 1
}
grep -q '= consume ' "$tmp/consume_alias_reassigned.out"
grep -q '= load ' "$tmp/consume_alias_reassigned.out"

"$compiler" lower --dump-ir "$program_dir/consume_call_forms.n00b" \
    >"$tmp/consume_call_forms.out" 2>&1 || {
    cat "$tmp/consume_call_forms.out"
    exit 1
}
grep -q 'call @generic_take' "$tmp/consume_call_forms.out"
grep -q 'call @recursive_take' "$tmp/consume_call_forms.out"
grep -q 'call @failing_take' "$tmp/consume_call_forms.out"
grep -q '!ir.call_signature((consume int) -> nil fail: int)' \
    "$tmp/consume_call_forms.out"
awk '
    /= consume %failed_value/ { consume_line = NR }
    /call @failing_take/ { call_line = NR }
    /branch .*__n00bb_unwrap_ok_/ { branch_line = NR }
    END { exit !(consume_line < call_line && call_line < branch_line) }
' "$tmp/consume_call_forms.out"

"$compiler" lower --dump-ir --dump-link-unit \
    "$script_dir/programs/trait_dispatch_single.n00b" \
    >"$tmp/lower_trait_dispatch.out" \
    2>"$tmp/lower_trait_dispatch.err" || {
    cat "$tmp/lower_trait_dispatch.err"
    exit 1
}
grep -q '= call @render, %item' "$tmp/lower_trait_dispatch.out"
grep -q 'resolved trait_dispatch_single -> trait_dispatch_single:1:render' \
    "$tmp/lower_trait_dispatch.out"

N00B_PATH="$program_dir/lower_import" "$compiler" lower "$program_dir/lower_import/lower_import_call.n00b" \
    >"$tmp/lower_import_call.ir" 2>"$tmp/lower_import_call.err" || {
    cat "$tmp/lower_import_call.err"
    exit 1
}
grep -q '= call @imported, %item' "$tmp/lower_import_call.ir"

"$compiler" parse "$program_dir/spaced_index.n00b" >"$tmp/spaced_index.tree"
grep -q 'postfix-expr' "$tmp/spaced_index.tree"
! grep -q '<tagged-lit>' "$tmp/spaced_index.tree"

"$compiler" parse "$program_dir/adjacent_prefix.n00b" >"$tmp/adjacent_prefix.tree"
grep -q '<tagged-lit>' "$tmp/adjacent_prefix.tree"

"$compiler" preprocess --dump-symbols "$program_dir/literal_shapes.n00b" \
    >"$tmp/literal_shapes.out"
grep -q 'Bracket_Literal' "$tmp/literal_shapes.out"
grep -q 'Brace_Literal' "$tmp/literal_shapes.out"

if "$compiler" preprocess "$program_dir/wrong_literal_shape.n00b" \
    >"$tmp/wrong_literal_shape.out" 2>&1; then
    echo 'brace literal unexpectedly satisfied an unrelated scalar type' >&2
    exit 1
fi
grep -q 'Brace_Literal' "$tmp/wrong_literal_shape.out"

"$compiler" preprocess --dump-symbols --dump-link-unit \
    "$script_dir/programs/index_load_dispatch.n00b" \
    >"$tmp/index_load.out" 2>&1 || {
    cat "$tmp/index_load.out"
    exit 1
}
grep 'get_item: function' "$tmp/index_load.out" | grep -q 'type=(Array\[int\]) -> string' || {
    cat "$tmp/index_load.out"
    echo 'index load result was not inferred from the Index_Read contract' >&2
    exit 1
}
grep -q 'resolved index_load_dispatch -> index_load_dispatch:1:__read' \
    "$tmp/index_load.out"
grep -q 'trait-dispatch\[resolved\] index_load_dispatch:1:get_item -> index_load_dispatch:1:__read' \
    "$tmp/index_load.out"

"$compiler" lower --dump-ir "$script_dir/programs/array_index_read_lowering.n00b" \
    >"$tmp/array_index_read.ir" 2>"$tmp/array_index_read.err" || {
    cat "$tmp/array_index_read.err"
    exit 1
}
grep -Eq '= call @__read, %items, %__lowered_[0-9]+ .*ir.call_signature\(\(array\[int\], int\) -> int fail: nil\)' \
    "$tmp/array_index_read.ir"

"$compiler" lower --dump-ir \
    "$script_dir/programs/array_index_nested_read.n00b" \
    >"$tmp/array_index_nested_read.ir" \
    2>"$tmp/array_index_nested_read.err" || {
    cat "$tmp/array_index_nested_read.err"
    exit 1
}
grep -Eq '= call @__read, %rows, %__lowered_[0-9]+ .*ir.call_signature\(\(array\[array\[int\]\], int\) -> array\[int\] fail: int\)' \
    "$tmp/array_index_nested_read.ir"
grep -q 'variant.payload .*"ok"' "$tmp/array_index_nested_read.ir"
grep -Eq '= call @__read, .*!ir.alias_count\(1\).*ir.alias0\(%rows\)' \
    "$tmp/array_index_nested_read.ir"
grep -Eq '= variant.payload .*"ok" .*!ir.alias_count\(1\).*ir.alias0\(%rows\)' \
    "$tmp/array_index_nested_read.ir"

"$compiler" lower --dump-ir "$script_dir/programs/array_index_store_lowering.n00b" \
    >"$tmp/array_index_store.ir" 2>"$tmp/array_index_store.err" || {
    cat "$tmp/array_index_store.err"
    exit 1
}
grep -Eq '= call @__write, %items, %__lowered_[0-9]+, %__lowered_[0-9]+' \
    "$tmp/array_index_store.ir"
if grep -q '= call @__read,' "$tmp/array_index_store.ir"; then
    echo 'indexed assignment unexpectedly emitted a read before its store' >&2
    exit 1
fi

"$compiler" lower --dump-ir "$script_dir/programs/array_ragged_lowering.n00b" \
    >"$tmp/array_ragged.ir" 2>"$tmp/array_ragged.err" || {
    cat "$tmp/array_ragged.err"
    exit 1
}
test "$(grep -c '= array.new ' "$tmp/array_ragged.ir")" -eq 3
test "$(grep -c '!ir.array.element_initialization("language_zero")' \
    "$tmp/array_ragged.ir")" -eq 3
test "$(grep -c '= call @__write,' "$tmp/array_ragged.ir")" -eq 2

"$compiler" lower --dump-ir "$script_dir/programs/array_slice_lowering.n00b" \
    >"$tmp/array_slice.ir" 2>"$tmp/array_slice.err" || {
    cat "$tmp/array_slice.err"
    exit 1
}
grep -Eq 'call @__slice, %items, %__lowered_[0-9]+, %' \
    "$tmp/array_slice.ir"
if grep -q '__n00bb_default_arg_' "$tmp/array_slice.ir"; then
    echo 'slice defaults retained a presence/value wrapper' >&2
    exit 1
fi
grep -Eq 'func @full_view\(%items: array\[int\].*\) -> view\[int\] fail: int' \
    "$tmp/array_slice.ir"
grep -q 'neg trap' "$tmp/array_slice.ir"
test "$(grep -c 'variant.tag %__n00bb_' "$tmp/array_slice.ir")" -eq 3
test "$(grep -c 'fail %__n00bb_outcome_' "$tmp/array_slice.ir")" -eq 3
test "$(grep -Ec '= call @__slice, .*!ir.alias_count\(1\).*!ir.alias0\(' \
    "$tmp/array_slice.ir")" -eq 3
test "$(grep -Ec ': view\[int\] = variant.payload .*"ok" .*!ir.alias_count\(1\).*!ir.alias0\(' \
    "$tmp/array_slice.ir")" -eq 3

"$compiler" lower --dump-symbols --dump-link-unit --dump-ir \
    "$script_dir/programs/array_view_lifetime.n00b" \
    >"$tmp/array_view_lifetime.out" \
    2>"$tmp/array_view_lifetime.err" || {
    cat "$tmp/array_view_lifetime.err"
    cat "$tmp/array_view_lifetime.out"
    exit 1
}
grep -Eq 'local_view: variable, .*type=View\[int\].*aliases=1, aliased-by=0,.*alias-analysis=complete, escaping-aliases=0' \
    "$tmp/array_view_lifetime.out"
grep -Eq 'local_view: variable, .*type=View\[int\].*aliases=1, aliased-by=0,.*escaping-aliases=1,.*may-escape' \
    "$tmp/array_view_lifetime.out"
grep -Eq '= call @__slice, .*!ir.alias0_escapes\(false\).*!ir.lifetime_escapes\(false\)' \
    "$tmp/array_view_lifetime.out"
grep -Eq '= call @__slice, .*!ir.alias0_escapes\(true\).*!ir.lifetime_escapes\(true\)' \
    "$tmp/array_view_lifetime.out"
grep -Eq 'cleanup-range\[local_view,.*evidence=incomplete' \
    "$tmp/array_view_lifetime.out"

N00B_PATH="$script_dir/programs" "$compiler" lower --dump-symbols \
    --dump-link-unit --dump-ir \
    "$script_dir/programs/array_nested_slice_lifetime.n00b" \
    >"$tmp/array_nested_slice_lifetime.out" \
    2>"$tmp/array_nested_slice_lifetime.err" || {
    cat "$tmp/array_nested_slice_lifetime.err"
    cat "$tmp/array_nested_slice_lifetime.out"
    exit 1
}
grep -Eq 'first_view: variable, .*type=View\[int\].*aliases=1, aliased-by=0,.*alias-analysis=complete, escaping-aliases=0' \
    "$tmp/array_nested_slice_lifetime.out"
grep -q 'trait-dispatch\[resolved\] array_nested_slice_lifetime:1:nested_view -> array_nested_slice.array_impl:1:__slice' \
    "$tmp/array_nested_slice_lifetime.out"
grep -q 'trait-dispatch\[resolved\] array_nested_slice_lifetime:1:nested_view -> array_nested_slice.view_impl:1:__slice' \
    "$tmp/array_nested_slice_lifetime.out"
grep -Eq '= call @__slice, .*!ir.alias0_escapes\(false\)' \
    "$tmp/array_nested_slice_lifetime.out"
grep -Eq '= call @__slice, .*!ir.alias0_escapes\(true\)' \
    "$tmp/array_nested_slice_lifetime.out"
grep -Eq 'cleanup-range\[first_view,.*safe-exits=1,live-exits=0,unknown-exits=0,evidence=complete' \
    "$tmp/array_nested_slice_lifetime.out"

"$compiler" lower --dump-ir --dump-control-flow --dump-link-unit \
    "$script_dir/programs/array_slice_failure.n00b" \
    >"$tmp/array_slice_failure.out" 2>"$tmp/array_slice_failure.err" || {
    cat "$tmp/array_slice_failure.err"
    cat "$tmp/array_slice_failure.out"
    exit 1
}
grep -q 'func @__slice(.*-> view\[int\] fail: int' \
    "$tmp/array_slice_failure.out"
grep -q 'func @propagate_slice(.*-> view\[int\] fail: int' \
    "$tmp/array_slice_failure.out"
grep -q 'func @catch_slice(.*-> int fail: nil' \
    "$tmp/array_slice_failure.out"
test "$(grep -c ': variant __lantern_call_outcome_.*= call @__slice,' \
    "$tmp/array_slice_failure.out")" -eq 3
test "$(grep -c 'variant.payload .*"error"' \
    "$tmp/array_slice_failure.out")" -eq 3
grep -q 'fail %__n00bb_outcome_' "$tmp/array_slice_failure.out"
grep -q 'jump __n00bb_try_handler_' "$tmp/array_slice_failure.out"

"$compiler" preprocess --dump-symbols --dump-link-unit \
    "$script_dir/programs/index_write_slice_dispatch.n00b" \
    >"$tmp/index_write_slice.out" 2>&1 || {
    cat "$tmp/index_write_slice.out"
    exit 1
}
grep -q 'resolved index_write_slice_dispatch -> index_write_slice_dispatch:1:__write' \
    "$tmp/index_write_slice.out"
grep -q 'resolved index_write_slice_dispatch -> index_write_slice_dispatch:1:__slice' \
    "$tmp/index_write_slice.out"
grep -q 'trait-dispatch\[resolved\] index_write_slice_dispatch:1:set_item -> index_write_slice_dispatch:1:__write' \
    "$tmp/index_write_slice.out"
grep -q 'trait-dispatch\[resolved\] index_write_slice_dispatch:1:get_view -> index_write_slice_dispatch:1:__slice' \
    "$tmp/index_write_slice.out"

"$compiler" preprocess --dump-symbols --dump-link-unit \
    "$script_dir/programs/generic_index_dispatch.n00b" \
    >"$tmp/generic_index.out" 2>&1 || {
    cat "$tmp/generic_index.out"
    exit 1
}
grep -q 'deferred generic_index_dispatch -> <pending>:<pending>.*generic trait dispatch awaits specialization' \
    "$tmp/generic_index.out"
grep -q 'resolved generic_index_dispatch -> generic_index_dispatch:2:Index_Read' \
    "$tmp/generic_index.out"
grep -q 'Index_Read: type, .*uses=1, declaration=1:1, definitions=\[\], use-sites=\[7:61\]' \
    "$tmp/generic_index.out"
grep -q 'trait-dispatch\[deferred\] generic_index_dispatch:1:generic_get' \
    "$tmp/generic_index.out"

if "$compiler" preprocess "$program_dir/index_load_bad_result.n00b" \
    >"$tmp/index_load_bad_result.out" 2>&1; then
    cat "$tmp/index_load_bad_result.out"
    echo 'index load with a mismatched result type unexpectedly passed' >&2
    exit 1
fi
grep -q 'N00BB-T0001' "$tmp/index_load_bad_result.out" || {
    cat "$tmp/index_load_bad_result.out"
    exit 1
}

"$compiler" preprocess "$program_dir/index_store.n00b" \
    >"$tmp/index_store.out" 2>&1 || {
    cat "$tmp/index_store.out"
    exit 1
}

if "$compiler" preprocess "$program_dir/index_store_bad_value.n00b" \
    >"$tmp/index_store_bad_value.out" 2>&1; then
    cat "$tmp/index_store_bad_value.out"
    echo 'index store with a mismatched value type unexpectedly passed' >&2
    exit 1
fi
grep -q 'N00BB-T0003' "$tmp/index_store_bad_value.out"

"$compiler" preprocess --dump-symbols "$program_dir/index_slice.n00b" \
    >"$tmp/index_slice.out" 2>&1 || {
    cat "$tmp/index_slice.out"
    exit 1
}
for function in full bounded no_start no_end; do
    grep "$function: function" "$tmp/index_slice.out" | \
        grep -q -- '-> View\[int\]' || {
        cat "$tmp/index_slice.out"
        echo "$function did not infer the Index_Slice view result" >&2
        exit 1
    }
done

if "$compiler" preprocess "$program_dir/index_slice_bad_bound.n00b" \
    >"$tmp/index_slice_bad_bound.out" 2>&1; then
    cat "$tmp/index_slice_bad_bound.out"
    echo 'slice indexing unexpectedly accepted a non-integer bound' >&2
    exit 1
fi
grep -q 'LANTERN-T0001' "$tmp/index_slice_bad_bound.out"

"$compiler" preprocess --dump-symbols --dump-link-unit \
    "$program_dir/ordering_trait.n00b" \
    >"$tmp/ordering_trait.out" 2>&1 || {
    cat "$tmp/ordering_trait.out"
    exit 1
}
grep 'less: function' "$tmp/ordering_trait.out" | grep -q -- '-> bool' || {
    cat "$tmp/ordering_trait.out"
    echo 'ordering trait result was not inferred as bool' >&2
    exit 1
}
for operation in less_equal greater greater_equal; do
    grep "$operation: function" "$tmp/ordering_trait.out" | grep -q -- '-> bool' || {
        cat "$tmp/ordering_trait.out"
        echo "$operation did not infer bool through Comparable.__cmp" >&2
        exit 1
    }
done
test "$(grep -c 'trait-dispatch\[resolved\] ordering_trait:1:.* -> ordering_trait:1:__cmp' \
    "$tmp/ordering_trait.out")" -eq 4
"$compiler" lower --dump-ir "$program_dir/ordering_trait.n00b" \
    >"$tmp/ordering_trait.ir" 2>"$tmp/ordering_trait.err" || {
    cat "$tmp/ordering_trait.err"
    exit 1
}
test "$(grep -c '= call @__cmp,' "$tmp/ordering_trait.ir")" -eq 4
grep -q 'enum.const "less", 0' "$tmp/ordering_trait.ir"
grep -q 'enum.const "equal", 1' "$tmp/ordering_trait.ir"
grep -q 'enum.const "greater", 2' "$tmp/ordering_trait.ir"

"$compiler" preprocess --dump-link-unit "$program_dir/equality_trait.n00b" \
    >"$tmp/equality_trait.link" 2>"$tmp/equality_trait.err" || {
    cat "$tmp/equality_trait.err"
    exit 1
}
test "$(grep -c 'trait-dispatch\[resolved\] equality_trait:1:.* -> equality_trait:1:__equal' \
    "$tmp/equality_trait.link")" -eq 2
test "$(grep -c 'trait-dispatch\[resolved\] equality_trait:1:.* -> equality_trait:1:__cmp' \
    "$tmp/equality_trait.link")" -eq 2
"$compiler" lower --dump-ir "$program_dir/equality_trait.n00b" \
    >"$tmp/equality_trait.ir" 2>"$tmp/equality_trait.err" || {
    cat "$tmp/equality_trait.err"
    exit 1
}
test "$(grep -c '= call @__equal,' "$tmp/equality_trait.ir")" -eq 2
test "$(grep -c '= call @__cmp,' "$tmp/equality_trait.ir")" -eq 2
grep -q 'enum.const "equal", 1' "$tmp/equality_trait.ir"
"$compiler" lower --dump-ir "$program_dir/equality_trait_failure.n00b" \
    >"$tmp/equality_trait_failure.ir" \
    2>"$tmp/equality_trait_failure.err" || {
    cat "$tmp/equality_trait_failure.err"
    exit 1
}
grep -q '= call @__cmp,' "$tmp/equality_trait_failure.ir"
grep -q 'variant.tag ' "$tmp/equality_trait_failure.ir"
grep -q 'enum.const "equal", 1' "$tmp/equality_trait_failure.ir"
grep -q 'fail %__n00bb_' "$tmp/equality_trait_failure.ir"

"$compiler" lower --dump-ir "$program_dir/ordering_trait_failure.n00b" \
    >"$tmp/ordering_trait_failure.ir" \
    2>"$tmp/ordering_trait_failure.err" || {
    cat "$tmp/ordering_trait_failure.err"
    exit 1
}
grep -q '= call @__cmp,' "$tmp/ordering_trait_failure.ir"
grep -q 'variant.tag ' "$tmp/ordering_trait_failure.ir"
grep -q 'fail %__n00bb_' "$tmp/ordering_trait_failure.ir"
if "$compiler" lower "$program_dir/ordering_trait_failure_unhandled.n00b" \
    >"$tmp/ordering_trait_failure_unhandled.out" \
    2>"$tmp/ordering_trait_failure_unhandled.err"; then
    echo 'unhandled comparison failure unexpectedly lowered' >&2
    exit 1
fi
grep -q 'uncaught call error must match' \
    "$tmp/ordering_trait_failure_unhandled.err"

"$compiler" lower --dump-ir "$program_dir/enum_constant_lower.n00b" \
    >"$tmp/enum_constant_lower.ir" 2>"$tmp/enum_constant_lower.err" || {
    cat "$tmp/enum_constant_lower.err"
    exit 1
}
grep -q 'enum.const "equal", 1' "$tmp/enum_constant_lower.ir"

"$compiler" preprocess --dump-symbols "$program_dir/sized_integer_types.n00b" \
    >"$tmp/sized_integer_types.out" 2>&1 || {
    cat "$tmp/sized_integer_types.out"
    exit 1
}
grep -q 'widths: function' "$tmp/sized_integer_types.out"
grep -q 'widths: function.*bits:4' "$tmp/sized_integer_types.out"
"$compiler" lower --dump-layout "$program_dir/scalar_layout_alias.n00b" \
    >"$tmp/scalar_layout_alias.out" 2>"$tmp/scalar_layout_alias.err" || {
    cat "$tmp/scalar_layout_alias.err"
    exit 1
}
grep -Eq 'layout scalar_layout_alias.Wide target=(x86_64-linux|arm64-macos|x86_64-windows) size=16 align=16' \
    "$tmp/scalar_layout_alias.out"
"$compiler" lower --dump-layout "$program_dir/scalar_layout_ir_only.n00b" \
    >"$tmp/scalar_layout_ir_only.out" 2>"$tmp/scalar_layout_ir_only.err" || {
    cat "$tmp/scalar_layout_ir_only.err"
    exit 1
}
test "$(grep -c '^  layout bool {$' "$tmp/scalar_layout_ir_only.out")" -eq 2
"$compiler" lower --dump-layout "$program_dir/atomic_layout.n00b" \
    >"$tmp/atomic_layout.out" 2>"$tmp/atomic_layout.err" || {
    cat "$tmp/atomic_layout.err"
    exit 1
}
test "$(grep -Fc '  layout atomic[bits:17] {' "$tmp/atomic_layout.out")" -eq 2
"$compiler" lower --dump-layout "$program_dir/lower_enum_cast.n00b" \
    >"$tmp/lower_enum_layout.out" 2>"$tmp/lower_enum_layout.err" || {
    cat "$tmp/lower_enum_layout.err"
    exit 1
}
grep -Eq 'layout lower_enum_cast.Signed target=(x86_64-linux|arm64-macos|x86_64-windows) size=1 align=1' \
    "$tmp/lower_enum_layout.out"
grep -Eq 'layout lower_enum_cast.Small target=(x86_64-linux|arm64-macos|x86_64-windows) size=1 align=1' \
    "$tmp/lower_enum_layout.out"
"$compiler" lower --dump-layout "$script_dir/programs/flagset_operators.n00b" \
    >"$tmp/flagset_layout.out" 2>"$tmp/flagset_layout.err" || {
    cat "$tmp/flagset_layout.err"
    exit 1
}
grep -Eq 'layout flagset_operators.FilePerms target=(x86_64-linux|arm64-macos|x86_64-windows) size=1 align=1' \
    "$tmp/flagset_layout.out"
if "$compiler" lower "$program_dir/recursive_record_layout.n00b" \
    >"$tmp/recursive_record_layout.out" 2>&1; then
    echo 'inline recursive record unexpectedly lowered' >&2
    exit 1
fi
grep -q 'inline layout cycle: recursive_record_layout.Node.next -> recursive_record_layout.Node' \
    "$tmp/recursive_record_layout.out"
"$compiler" lower "$program_dir/recursive_record_ref.n00b" \
    >"$tmp/recursive_record_ref.out" 2>&1 || {
    cat "$tmp/recursive_record_ref.out"
    exit 1
}
if "$compiler" lower "$program_dir/mutual_record_inline.n00b" \
    >"$tmp/mutual_record_inline.out" 2>&1; then
    echo 'mutually recursive inline records unexpectedly lowered' >&2
    exit 1
fi
grep -q 'inline layout cycle: mutual_record_inline.Left.right -> mutual_record_inline.Right.left -> mutual_record_inline.Left' \
    "$tmp/mutual_record_inline.out"
"$compiler" lower "$program_dir/mutual_record_refs.n00b" \
    >"$tmp/mutual_record_refs.out" 2>&1 || {
    cat "$tmp/mutual_record_refs.out"
    exit 1
}
N00B_PATH="$program_dir" "$compiler" lower --dump-link-unit \
    record_cycle_import.a >"$tmp/record_cycle_import.out" 2>&1 || {
    cat "$tmp/record_cycle_import.out"
    exit 1
}
grep -q 'resolved record_cycle_import.a -> record_cycle_import.b:.*:Right' \
    "$tmp/record_cycle_import.out"
grep -q 'resolved record_cycle_import.b -> record_cycle_import.a:.*:Left' \
    "$tmp/record_cycle_import.out"
if N00B_PATH="$program_dir" "$compiler" lower \
    record_cycle_missing.a >"$tmp/record_cycle_missing.out" 2>&1; then
    echo 'missing type in recursive import unexpectedly lowered' >&2
    exit 1
fi
grep -q "module 'record_cycle_missing.b' has no visible type named 'Absent'" \
    "$tmp/record_cycle_missing.out"
if N00B_PATH="$program_dir" "$compiler" lower \
    record_cycle_inline.a >"$tmp/record_cycle_inline.out" 2>&1; then
    echo 'cross-module inline record cycle unexpectedly lowered' >&2
    exit 1
fi
grep -q 'inline layout cycle: record_cycle_inline.b.Right.left -> record_cycle_inline.a.Left.right -> record_cycle_inline.b.Right' \
    "$tmp/record_cycle_inline.out"
"$compiler" parse "$program_dir/record_layout_modifiers.n00b" \
    >"$tmp/record_layout_modifiers.parse" 2>&1 || {
    cat "$tmp/record_layout_modifiers.parse"
    exit 1
}
test "$(grep -c '<type-layout> rule=' \
    "$tmp/record_layout_modifiers.parse")" -eq 2
test "$(grep -c '<type-align> rule=' \
    "$tmp/record_layout_modifiers.parse")" -eq 2
if "$compiler" lower "$program_dir/record_layout_modifiers.n00b" \
    >"$tmp/record_layout_modifiers.out" 2>&1; then
    echo 'unfinalized record layout modifiers unexpectedly lowered' >&2
    exit 1
fi
grep -q 'N00BB-L0069.*record layout modifiers are not finalized' \
    "$tmp/record_layout_modifiers.out"
if "$compiler" lower "$program_dir/record_layout_invalid_policy.n00b" \
    >"$tmp/record_layout_invalid_policy.out" 2>&1; then
    echo 'unknown record layout policy unexpectedly lowered' >&2
    exit 1
fi
grep -q 'N00BB-L0069.*unknown record layout policy' \
    "$tmp/record_layout_invalid_policy.out"
if "$compiler" lower "$program_dir/record_layout_duplicate.n00b" \
    >"$tmp/record_layout_duplicate.out" 2>&1; then
    echo 'duplicate record layout policy unexpectedly lowered' >&2
    exit 1
fi
grep -q 'N00BB-L0069.*duplicate record layout policy' \
    "$tmp/record_layout_duplicate.out"
"$compiler" lower --dump-layout "$program_dir/c_record_layout.n00b" \
    >"$tmp/c_record_layout.out" 2>&1 || {
    cat "$tmp/c_record_layout.out"
    exit 1
}
grep -Eq 'layout c_record_layout.Header target=(x86_64-linux|arm64-macos|x86_64-windows) size=8 align=4' \
    "$tmp/c_record_layout.out"
grep -q 'member count bit-offset=0 bit-width=32' \
    "$tmp/c_record_layout.out"
grep -q 'member active bit-offset=32 bit-width=8' \
    "$tmp/c_record_layout.out"
N00B_PATH="$program_dir" "$compiler" lower --dump-layout \
    c_record_import >"$tmp/c_record_import.out" 2>&1 || {
    cat "$tmp/c_record_import.out"
    exit 1
}
grep -Eq 'layout c_record_layout.Header target=(x86_64-linux|arm64-macos|x86_64-windows) size=8 align=4' \
    "$tmp/c_record_import.out"
"$compiler" lower --dump-layout "$program_dir/c_record_nested_local.n00b" \
    >"$tmp/c_record_nested_local.out" 2>&1 || {
    cat "$tmp/c_record_nested_local.out"
    exit 1
}
grep -Eq 'layout c_record_nested_local.Outer target=(x86_64-linux|arm64-macos|x86_64-windows) size=12 align=4' \
    "$tmp/c_record_nested_local.out"
grep -q 'member inner bit-offset=32 bit-width=64' \
    "$tmp/c_record_nested_local.out"
N00B_PATH="$program_dir" "$compiler" lower --dump-layout \
    c_record_nested >"$tmp/c_record_nested.out" 2>&1 || {
    cat "$tmp/c_record_nested.out"
    exit 1
}
grep -Eq 'layout c_record_nested.Wrapped target=(x86_64-linux|arm64-macos|x86_64-windows) size=12 align=4' \
    "$tmp/c_record_nested.out"
grep -q 'member header bit-offset=32 bit-width=64' \
    "$tmp/c_record_nested.out"
"$compiler" lower --dump-layout "$program_dir/c_record_alias.n00b" \
    >"$tmp/c_record_alias.out" 2>&1 || {
    cat "$tmp/c_record_alias.out"
    exit 1
}
grep -Eq 'layout c_record_alias.Distinct target=(x86_64-linux|arm64-macos|x86_64-windows) size=8 align=4' \
    "$tmp/c_record_alias.out"
grep -q 'member count bit-offset=0 bit-width=32' \
    "$tmp/c_record_alias.out"
grep -q 'member active bit-offset=32 bit-width=8' \
    "$tmp/c_record_alias.out"
grep -Eq 'layout c_record_alias.Outer target=(x86_64-linux|arm64-macos|x86_64-windows) size=16 align=4' \
    "$tmp/c_record_alias.out"
grep -q 'member synonym bit-offset=64 bit-width=64' \
    "$tmp/c_record_alias.out"
N00B_PATH="$program_dir" "$compiler" lower --dump-layout \
    c_record_alias_import >"$tmp/c_record_alias_import.out" 2>&1 || {
    cat "$tmp/c_record_alias_import.out"
    exit 1
}
grep -Eq 'layout c_record_alias_import.ImportedOuter target=(x86_64-linux|arm64-macos|x86_64-windows) size=8 align=4' \
    "$tmp/c_record_alias_import.out"
if "$compiler" lower "$program_dir/c_record_default_child.n00b" \
    >"$tmp/c_record_default_child.out" 2>&1; then
    echo 'default-layout record used as C field unexpectedly lowered' >&2
    exit 1
fi
grep -q "field 'inner' has no C-compatible finalized ABI representation" \
    "$tmp/c_record_default_child.out"
if "$compiler" lower "$program_dir/c_record_invalid_field.n00b" \
    >"$tmp/c_record_invalid_field.out" 2>&1; then
    echo 'non-C-compatible record field unexpectedly lowered' >&2
    exit 1
fi
grep -q "field 'value' has no C-compatible finalized ABI representation" \
    "$tmp/c_record_invalid_field.out"
"$compiler" lower --dump-layout "$program_dir/fixed_value_layouts.n00b" \
    >"$tmp/fixed_value_layouts.out" 2>"$tmp/fixed_value_layouts.err" || {
    cat "$tmp/fixed_value_layouts.err"
    exit 1
}
for fixed_type in string 'array[int]' 'view[int]' any; do
    test "$(grep -Fc "  layout $fixed_type {" "$tmp/fixed_value_layouts.out")" -eq 2
done
test "$(grep -Fc '  layout Allocator {' "$tmp/fixed_value_layouts.out")" -eq 2
if "$compiler" lower "$program_dir/flagset_no_primitive.n00b" \
    >"$tmp/flagset_no_primitive.out" 2>&1; then
    echo 'flagset without a primitive flag unexpectedly lowered' >&2
    exit 1
fi
grep -q 'N00BB-T0019.*flagset requires at least one primitive flag' \
    "$tmp/flagset_no_primitive.out"
for invalid_width in sized_integer_zero_width sized_unsigned_too_wide \
    sized_bits_too_wide; do
    if "$compiler" lower "$program_dir/$invalid_width.n00b" \
        >"$tmp/$invalid_width.out" 2>&1; then
        echo "$invalid_width unexpectedly lowered" >&2
        exit 1
    fi
    grep -q 'N00BB-T0028' "$tmp/$invalid_width.out" || {
        cat "$tmp/$invalid_width.out"
        exit 1
    }
done

if "$compiler" parse "$program_dir/spaced_integer_type.n00b" \
    >"$tmp/spaced_integer_type.out" 2>&1; then
    cat "$tmp/spaced_integer_type.out"
    echo 'spaced sized-integer prefix unexpectedly parsed' >&2
    exit 1
fi

"$compiler" preprocess --dump-symbols "$program_dir/implicit_binding.n00b" \
    >"$tmp/implicit_binding.out"
grep -q 'implicit_value: variable, mutable, type=' \
    "$tmp/implicit_binding.out"
grep -q 'implicit_value: variable, mutable, type=int, type-source=inferred, .*uses=0, declaration=none, definitions=\[1:1\], use-sites=\[\]' \
    "$tmp/implicit_binding.out" || {
    cat "$tmp/implicit_binding.out"
    echo 'inferred binding declaration/definition/use sites are not represented on its symbol' >&2
    exit 1
}

"$compiler" preprocess --dump-symbols "$program_dir/reference_alias.n00b" \
    >"$tmp/reference_alias.out"
grep -q 'target: variable, mutable, .*aliased-by=3' \
    "$tmp/reference_alias.out"
grep -q 'target: variable, mutable, .*declaration=1:5, definitions=\[1:5\]' \
    "$tmp/reference_alias.out" || {
    cat "$tmp/reference_alias.out"
    echo 'declared variable is missing its declaration or initializer definition site' >&2
    exit 1
}
grep -q 'borrowed: variable, mutable, .*aliases=1, aliased-by=0' \
    "$tmp/reference_alias.out"
grep -q 'borrowed_copy: variable, mutable, .*aliases=1, aliased-by=0, .*alias-analysis=incomplete' \
    "$tmp/reference_alias.out"
grep -q 'short_lived: variable, mutable, .*escaping-aliased-by=1' \
    "$tmp/reference_alias.out"
grep -q 'local_target: variable, mutable, .*may-escape' \
    "$tmp/reference_alias.out"
grep -q 'call_target: variable, mutable, .*storage-duration=automatic, .*alias-analysis=complete' \
    "$tmp/reference_alias.out"
grep -q 'borrowed_copy_call: variable, mutable, .*alias-analysis=complete' \
    "$tmp/reference_alias.out"
grep -q '^    copy_target: variable, mutable, .*storage-duration=automatic' \
    "$tmp/reference_alias.out" || {
    cat "$tmp/reference_alias.out"
    echo 'reference passed through a nonescaping local copy was not kept automatic' >&2
    exit 1
}
if grep -q '^    copy_target: variable, mutable, .*may-escape' \
    "$tmp/reference_alias.out"; then
    cat "$tmp/reference_alias.out"
    echo 'reference passed through a nonescaping local copy was marked escaping' >&2
    exit 1
fi
grep -q '^    returned_copy_target: variable, mutable, .*storage-duration=dynamic, .*may-escape' \
    "$tmp/reference_alias.out"
grep -q '^    nested_copy_target: variable, mutable, .*storage-duration=dynamic, .*may-escape' \
    "$tmp/reference_alias.out"
grep -q '^    recursive_copy_target: variable, mutable, .*storage-duration=dynamic, .*may-escape' \
    "$tmp/reference_alias.out"
grep -q '^    returned_container_target: variable, mutable, .*storage-duration=dynamic, .*may-escape' \
    "$tmp/reference_alias.out"
grep -q '^    container_result: variable, mutable, .*aliases=[1-9]' \
    "$tmp/reference_alias.out"
grep -q '^    variable_container_result: variable, mutable, .*aliases=[1-9]' \
    "$tmp/reference_alias.out" || {
    cat "$tmp/reference_alias.out"
    echo 'aggregate aliases were lost when returned through a local variable' >&2
    exit 1
}
grep -q '^    nested_call_result: variable, mutable, .*aliases=[1-9]' \
    "$tmp/reference_alias.out" || {
    cat "$tmp/reference_alias.out"
    echo 'nested call result inside an aggregate lost its returned global alias' >&2
    exit 1
}
grep -q '^    nested_copy_result: variable, mutable, .*aliases=[1-9]' \
    "$tmp/reference_alias.out" || {
    cat "$tmp/reference_alias.out"
    echo 'nested aggregate call alias was lost through local aggregate copies' >&2
    exit 1
}
grep -q '^    generic_trait_target: variable, mutable, .*storage-duration=dynamic, .*may-escape' \
    "$tmp/reference_alias.out" || {
    cat "$tmp/reference_alias.out"
    echo 'unresolved generic trait dispatch did not conservatively escape its reference argument' >&2
    exit 1
}
grep -q '^    generic_trait_result: variable, mutable, type=Ref\[int\], .*alias-analysis=incomplete' \
    "$tmp/reference_alias.out" || {
    cat "$tmp/reference_alias.out"
    echo 'generic trait result did not retain its concrete reference type' >&2
    exit 1
}
grep -q '^    unrelated_nested_target: variable, mutable, .*storage-duration=automatic' \
    "$tmp/reference_alias.out" || {
    cat "$tmp/reference_alias.out"
    echo 'unrelated argument to a nested aggregate call was not kept automatic' >&2
    exit 1
}
if grep -q '^    unrelated_nested_target: variable, mutable, .*may-escape' \
    "$tmp/reference_alias.out"; then
    cat "$tmp/reference_alias.out"
    echo 'nested aggregate analysis incorrectly treated a call argument as returned' >&2
    exit 1
fi
grep -q '^    unrelated_nested_copy_target: variable, mutable, .*storage-duration=automatic' \
    "$tmp/reference_alias.out" || {
    cat "$tmp/reference_alias.out"
    echo 'unrelated argument remained escaping through aggregate copies' >&2
    exit 1
}
if grep -q '^    unrelated_nested_copy_target: variable, mutable, .*may-escape' \
    "$tmp/reference_alias.out"; then
    cat "$tmp/reference_alias.out"
    echo 'unrelated argument to a copied nested aggregate was marked escaping' >&2
    exit 1
fi
grep -q '^    parameter_container_target: variable, mutable, .*storage-duration=dynamic, .*may-escape' \
    "$tmp/reference_alias.out" || {
    cat "$tmp/reference_alias.out"
    echo 'aggregate return did not substitute the caller reference argument' >&2
    exit 1
}
grep -q '^    parameter_container_result: variable, mutable, .*aliases=[1-9]' \
    "$tmp/reference_alias.out"
grep -q '^    trait_target: variable, mutable, .*storage-duration=automatic' \
    "$tmp/reference_alias.out" || {
    cat "$tmp/reference_alias.out"
    echo 'resolved trait implementation summary did not preserve a nonescaping argument' >&2
    exit 1
}
if grep -q '^    trait_target: variable, mutable, .*may-escape' \
    "$tmp/reference_alias.out"; then
    cat "$tmp/reference_alias.out"
    echo 'resolved trait implementation marked an unused argument as escaping' >&2
    exit 1
fi
grep -q '^    trait_receiver_result: variable, mutable, .*aliases=[1-9]' \
    "$tmp/reference_alias.out" || {
    cat "$tmp/reference_alias.out"
    echo 'trait result did not retain the implicit reference receiver alias' >&2
    exit 1
}
grep -q '^    direct_identity_result: variable, mutable, .*aliases=[1-9]' \
    "$tmp/reference_alias.out"
grep -q '^    trait_receiver_target: variable, mutable, .*storage-duration=dynamic, .*may-escape' \
    "$tmp/reference_alias.out"
grep -q '^    trait_argument_result: variable, mutable, .*aliases=[1-9]' \
    "$tmp/reference_alias.out" || {
    cat "$tmp/reference_alias.out"
    echo 'trait return alias did not map its explicit argument after the receiver' >&2
    exit 1
}
grep -q '^    trait_argument_target: variable, mutable, .*storage-duration=dynamic, .*may-escape' \
    "$tmp/reference_alias.out"
grep -q '^    trait_aggregate_result: variable, mutable, .*aliases=[1-9]' \
    "$tmp/reference_alias.out" || {
    cat "$tmp/reference_alias.out"
    echo 'trait aggregate return did not preserve the explicit argument alias' >&2
    exit 1
}
grep -q '^    trait_aggregate_target: variable, mutable, .*storage-duration=dynamic, .*may-escape' \
    "$tmp/reference_alias.out"
grep -q '^    direct_return_result: variable, mutable, .*aliases=[1-9]' \
    "$tmp/reference_alias.out" || {
    cat "$tmp/reference_alias.out"
    echo 'direct returned-parameter alias was not attached to the call result' >&2
    exit 1
}
grep -q '^    direct_return_target: variable, mutable, .*storage-duration=dynamic, .*may-escape' \
    "$tmp/reference_alias.out"
grep -q 'return_target: variable, mutable, .*escaping-aliased-by=1' \
    "$tmp/reference_alias.out"
grep -q 'return_target: variable, mutable, .*storage-duration=dynamic' \
    "$tmp/reference_alias.out"
grep -q 'container_target: variable, mutable, .*escaping-aliased-by=0' \
    "$tmp/reference_alias.out"
grep -q 'container_target: variable, mutable, .*storage-duration=dynamic' \
    "$tmp/reference_alias.out"
grep -q 'escaping_target: variable, mutable, .*escaping-aliased-by=[1-9]' \
    "$tmp/reference_alias.out"
grep -q 'escaping_target: variable, mutable, .*storage-duration=dynamic' \
    "$tmp/reference_alias.out"
grep -q 'isolated: variable, mutable, .*alias-analysis=complete' \
    "$tmp/reference_alias.out"

"$compiler" preprocess --dump-link-unit \
    "$program_dir/reference_alias.n00b" \
    >"$tmp/reference_alias_effects.out" \
    2>"$tmp/reference_alias_effects.err" || {
    cat "$tmp/reference_alias_effects.err"
    cat "$tmp/reference_alias_effects.out"
    exit 1
}
grep -q 'function-global-unknown\[unavailable\] reference_alias:1:keep_reference -> <unresolved>' \
    "$tmp/reference_alias_effects.out"
grep -q 'function-global-unknown\[unavailable\] reference_alias:1:passes_stored_reference -> <unresolved>' \
    "$tmp/reference_alias_effects.out"

"$compiler" preprocess --dump-link-unit \
    "$program_dir/parameter_alias.n00b" \
    >"$tmp/parameter_alias.out" 2>"$tmp/parameter_alias.err" || {
    cat "$tmp/parameter_alias.err"
    cat "$tmp/parameter_alias.out"
    exit 1
}
grep -q 'function-global-alias-summary\[resolved\] parameter_alias:1:function_alias -> parameter_alias:0:aliased_global' \
    "$tmp/parameter_alias.out"
grep -q 'function-global-alias-summary\[resolved\] parameter_alias:1:alias_from_call -> parameter_alias:0:aliased_global' \
    "$tmp/parameter_alias.out"
grep -q 'function-global-alias-summary\[resolved\] parameter_alias:1:trait_alias -> parameter_alias:0:aliased_global' \
    "$tmp/parameter_alias.out"
grep -q 'initializer-use\[resolved\] parameter_alias:0:stored_reference -> parameter_alias:0:aliased_global' \
    "$tmp/parameter_alias.out"

"$compiler" preprocess --dump-symbols \
    "$program_dir/parameter_alias.n00b" \
    >"$tmp/parameter_alias_symbols.out" \
    2>"$tmp/parameter_alias_symbols.err" || {
    cat "$tmp/parameter_alias_symbols.err"
    cat "$tmp/parameter_alias_symbols.out"
    exit 1
}
grep -E -q 'call_ref: variable, mutable, type=Ref\[int\], .*aliases=1, aliased-by=0' \
    "$tmp/parameter_alias_symbols.out"
grep -E -q 'produced_ref: variable, mutable, type=Ref\[int\], .*aliases=1, aliased-by=0' \
    "$tmp/parameter_alias_symbols.out"
grep -E -q 'aliased_global: variable, .*escaping-aliased-by=[1-9]' \
    "$tmp/parameter_alias_symbols.out"

N00B_PATH="$program_dir" "$compiler" preprocess --dump-symbols \
    "$program_dir/alias_imported_client.n00b" \
    >"$tmp/alias_imported_client.out" \
    2>"$tmp/alias_imported_client.err" || {
    cat "$tmp/alias_imported_client.err"
    cat "$tmp/alias_imported_client.out"
    exit 1
}
grep -E -q 'imported_ref: variable, mutable, type=Ref\[int\], .*aliases=1, aliased-by=0' \
    "$tmp/alias_imported_client.out"
grep -E -q 'imported_global_ref: variable, mutable, type=Ref\[int\], .*aliases=1, aliased-by=0' \
    "$tmp/alias_imported_client.out"
grep -E -q 'imported_global: variable, .*may-escape' \
    "$tmp/alias_imported_client.out"

N00B_PATH="$program_dir" "$compiler" lower --dump-ir --dump-symbols \
    "$program_dir/return_alias_recursive_client.n00b" \
    >"$tmp/return_alias_recursive.out" \
    2>"$tmp/return_alias_recursive.err" || {
    cat "$tmp/return_alias_recursive.err"
    exit 1
}
grep -q '^    result: variable, mutable, type=Ref\[int\], .*aliases=1' \
    "$tmp/return_alias_recursive.out"

"$compiler" preprocess --dump-symbols \
    "$program_dir/return_alias_deep_chain.n00b" \
    >"$tmp/return_alias_deep_chain.out" \
    2>"$tmp/return_alias_deep_chain.err" || {
    cat "$tmp/return_alias_deep_chain.err"
    exit 1
}
grep -q '^    result: variable, mutable, type=Ref\[int\], .*aliases=1' \
    "$tmp/return_alias_deep_chain.out"
grep -q '^    target: variable, mutable, type=int, .*storage-duration=dynamic' \
    "$tmp/return_alias_deep_chain.out"

grep -Eq '= call @from_a, .* !ir.alias_count\(1\)' \
    "$tmp/return_alias_recursive.out"
grep -Eq '= call @from_b, .* !ir.alias_count\(1\)' \
    "$tmp/return_alias_recursive.out"

N00B_PATH="$program_dir" "$compiler" lower --dump-ir --dump-symbols \
    "$program_dir/return_alias_mismatch_client.n00b" \
    >"$tmp/return_alias_mismatch.out" \
    2>"$tmp/return_alias_mismatch.err" || {
    cat "$tmp/return_alias_mismatch.err"
    exit 1
}
grep -q '^    result: variable, mutable, type=Ref\[int\], .*aliases=0, .*alias-analysis=incomplete' \
    "$tmp/return_alias_mismatch.out"

"$compiler" lower --dump-ir "$program_dir/lower_require_assert.n00b" \
    >"$tmp/lower_require_assert.ir" \
    2>"$tmp/lower_require_assert.err" || {
    cat "$tmp/lower_require_assert.err"
    exit 1
}
grep -q 'branch %x, __n00bb_require_join_' "$tmp/lower_require_assert.ir"
test "$(grep -c '^[[:space:]]*trap ' "$tmp/lower_require_assert.ir")" -eq 3

"$compiler" --release lower --dump-ir \
    "$program_dir/lower_require_assert.n00b" \
    >"$tmp/lower_require_assert_release.ir" \
    2>"$tmp/lower_require_assert_release.err" || {
    cat "$tmp/lower_require_assert_release.err"
    exit 1
}
test "$(grep -c '^[[:space:]]*trap ' "$tmp/lower_require_assert_release.ir")" -eq 1

"$compiler" preprocess --dump-preprocessed "$program_dir/debug_mode.n00b" \
    >"$tmp/debug_mode.out" 2>"$tmp/debug_mode.err" || {
    cat "$tmp/debug_mode.err"
    exit 1
}
grep -q 'debug_branch' "$tmp/debug_mode.out"
if grep -q 'release_branch' "$tmp/debug_mode.out"; then
    echo 'release-only branch survived debug preprocessing' >&2
    exit 1
fi
"$compiler" --release preprocess --dump-preprocessed \
    "$program_dir/debug_mode.n00b" \
    >"$tmp/release_mode.out" 2>"$tmp/release_mode.err" || {
    cat "$tmp/release_mode.err"
    exit 1
}
grep -q 'release_branch' "$tmp/release_mode.out"
if grep -q 'debug_branch' "$tmp/release_mode.out"; then
    echo 'debug-only branch survived release preprocessing' >&2
    exit 1
fi

if "$compiler" lower "$program_dir/lower_require_nonbool.n00b" \
    >"$tmp/lower_require_nonbool.out" \
    2>"$tmp/lower_require_nonbool.err"; then
    echo 'non-boolean require condition was accepted' >&2
    exit 1
fi
grep -q 'assert and require conditions must be bool' \
    "$tmp/lower_require_nonbool.err"

if "$compiler" lower "$program_dir/lower_assert_side_effect.n00b" \
    >"$tmp/lower_assert_side_effect.out" \
    2>"$tmp/lower_assert_side_effect.err"; then
    echo 'assertion with a known global write was accepted' >&2
    exit 1
fi
grep -q 'assert expression has a side effect' \
    "$tmp/lower_assert_side_effect.err"

if "$compiler" lower "$program_dir/lower_assert_modify_effect.n00b" \
    >"$tmp/lower_assert_modify_effect.out" \
    2>"$tmp/lower_assert_modify_effect.err"; then
    echo 'assertion with a modify argument was accepted' >&2
    exit 1
fi
grep -q 'assert expression has a side effect' \
    "$tmp/lower_assert_modify_effect.err"

"$compiler" lower --dump-ir "$program_dir/lower_reference_values.n00b" \
    >"$tmp/lower_reference_values.ir" \
    2>"$tmp/lower_reference_values.err" || {
    cat "$tmp/lower_reference_values.err"
    exit 1
}
grep -q '= alloc ' \
    "$tmp/lower_reference_values.ir"
grep -q '= load %__n00bb_storage_' "$tmp/lower_reference_values.ir"
grep -q 'store %__n00bb_storage_' "$tmp/lower_reference_values.ir"
grep -q 'return %__lowered_' "$tmp/lower_reference_values.ir"
if grep -q '= address.of \$__n00bb_local_' \
    "$tmp/lower_reference_values.ir"; then
    echo 'escaping local reference did not use its selected allocation' >&2
    exit 1
fi
grep -q '= address.of @.*global_target' "$tmp/lower_reference_values.ir"
grep -E -q '= address.of .* !ir.alias0\(' \
    "$tmp/lower_reference_values.ir"
if grep -q 'ir.storage_selected("gc")' "$tmp/lower_reference_values.ir"; then
    echo 'runtime-managed allocation was assigned a frontend-selected gc class' >&2
    exit 1
fi
if grep -q '= address.of %target' "$tmp/lower_reference_values.ir"; then
    echo 'escaping parameter reference bypassed its selected allocation' >&2
    exit 1
fi

"$compiler" lower --dump-ir --dump-link-unit \
    "$program_dir/storage_size.n00b" \
    >"$tmp/storage_size.out" 2>"$tmp/storage_size.err" || {
    cat "$tmp/storage_size.err"
    exit 1
}
target_count=$(grep -Ec '^[[:space:]]+target (x86_64-linux|arm64-macos|x86_64-windows)$' \
    "$tmp/storage_size.out")
if [ "$target_count" -ne 2 ]; then
    echo 'phase IR modules did not retain the native ABI target' >&2
    exit 1
fi
stack_limit=${N00BB_STACK_OBJECT_LIMIT_BYTES:-4096}
grep -q 'n00bb.finalization v1 complete' "$tmp/storage_size.out"
if grep -E -q '^[[:space:]]*(finalize|dealloc) ' \
    "$tmp/storage_size.out"; then
    echo 'default dynamic storage received compiler-scheduled cleanup' >&2
    exit 1
fi
if [ 8 -gt "$stack_limit" ]; then
    grep -q '= alloc int ' "$tmp/storage_size.out"
else
    grep -Eq 'local \$__n00bb_local_0: int .*ir.storage_duration\(2\)' \
        "$tmp/storage_size.out"
fi
if [ 16 -gt "$stack_limit" ]; then
    grep -q 'cleanup-range\[large,phase=runtime,storage=dynamic,release=runtime-managed,.*finalizer=unspecified\]' \
        "$tmp/storage_size.out"
    grep -q '= alloc bitint\[128\]' "$tmp/storage_size.out"
    grep -q 'ir.storage_duration(5)' "$tmp/storage_size.out"
else
    grep -q 'cleanup-range\[large,phase=runtime,storage=automatic,release=none,.*evidence=complete,finalizer=unspecified\]' \
        "$tmp/storage_size.out"
    grep -Eq 'local \$__n00bb_local_0: bitint\[128\].*ir.storage_duration\(2\)' \
        "$tmp/storage_size.out"
fi
if grep -q '= copy \$__n00bb_local_0' "$tmp/lower_reference_values.ir"; then
    echo 'ref lowering unexpectedly read its target value' >&2
    exit 1
fi
if grep -q '= alloca' "$tmp/lower_reference_values.ir"; then
    echo 'ref lowering unexpectedly allocated a fresh target' >&2
    exit 1
fi

"$compiler" preprocess "$program_dir/branches.n00b" >"$tmp/branches.out"
grep -q '"selected"' "$tmp/branches.out"
! grep -q '"removed"' "$tmp/branches.out"
grep -q '"typed_selected"' "$tmp/branches.out"
! grep -q '"typed_removed"' "$tmp/branches.out"

"$compiler" preprocess --dump-symbols "$program_dir/branches.n00b" \
    >"$tmp/branches.symbols"
grep -q 'selected: variable, mutable' "$tmp/branches.symbols"
! grep -q 'removed:' "$tmp/branches.symbols"
grep -q 'enabled: variable, const' "$tmp/branches.symbols"
grep -q 'enabled: variable, const, .*phases=comptime+runtime, mutable-in=comptime, initializer-in=comptime' \
    "$tmp/branches.symbols"
grep -q 'enabled: variable, const, .*storage-duration=module' \
    "$tmp/branches.symbols"
grep -q 'numeric: variable, const, type=' "$tmp/branches.symbols"
grep -q 'worker_state: variable, mutable, .*thread-local, phases=runtime, mutable-in=runtime, initializer-in=comptime' \
    "$tmp/branches.symbols"
grep -q 'worker_state: variable, mutable, .*storage-duration=thread' \
    "$tmp/branches.symbols"
grep -q 'worker_seed: variable, const, .*thread-local, phases=runtime, mutable-in=comptime, initializer-in=comptime' \
    "$tmp/branches.symbols"
"$compiler" lower --dump-symbols \
    "$program_dir/thread_runtime_initializer.n00b" \
    >"$tmp/thread_runtime_initializer.symbols"
grep -q 'worker_state: variable, mutable, .*thread-local, phases=runtime, mutable-in=runtime, initializer-in=comptime+runtime' \
    "$tmp/thread_runtime_initializer.symbols"
"$compiler" lower --dump-ir --dump-link-unit \
    "$program_dir/thread_runtime_initializer.n00b" \
    >"$tmp/thread_runtime_initializer.ir"
grep -q 'global.store @thread_runtime_initializer.worker_state' \
    "$tmp/thread_runtime_initializer.ir"
grep -q 'initializer-phases=\[runtime\]' \
    "$tmp/thread_runtime_initializer.ir"

if "$compiler" lower --dump-ir \
    "$program_dir/thread_runtime_function_uninitialized.n00b" \
    >"$tmp/thread_runtime_function_uninitialized.out" 2>&1; then
    echo 'a callable thread-local read without prior initialization unexpectedly lowered' >&2
    exit 1
fi
grep -q 'N00BB-L0044.*runtime function may read thread-local global' \
    "$tmp/thread_runtime_function_uninitialized.out"

"$compiler" lower --dump-ir \
    "$program_dir/thread_runtime_function_write_before_read.n00b" \
    >"$tmp/thread_runtime_function_write_before_read.ir"
grep -q 'global.store @thread_runtime_function_write_before_read.worker_state' \
    "$tmp/thread_runtime_function_write_before_read.ir"

"$compiler" lower --dump-ir \
    "$program_dir/thread_runtime_function_call_flow.n00b" \
    >"$tmp/thread_runtime_function_call_flow.ir"
grep -q 'call @initialize_worker_state' \
    "$tmp/thread_runtime_function_call_flow.ir"
grep -q 'call @read_worker_state' \
    "$tmp/thread_runtime_function_call_flow.ir"

for thread_function_case in \
    thread_runtime_function_conditional_write \
    thread_runtime_function_read_before_helper_write; do
    if "$compiler" lower --dump-ir \
        "$program_dir/$thread_function_case.n00b" \
        >"$tmp/$thread_function_case.out" 2>&1; then
        echo "$thread_function_case unexpectedly lowered" >&2
        exit 1
    fi
    grep -q 'N00BB-L0044.*runtime function may read thread-local global' \
        "$tmp/$thread_function_case.out"
done

if ! N00B_PATH="$program_dir" "$compiler" lower --dump-ir \
    --dump-control-flow --dump-link-unit \
    "$program_dir/thread_runtime_client.n00b" \
    >"$tmp/thread_runtime_client.out" 2>&1; then
    cat "$tmp/thread_runtime_client.out"
    exit 1
fi
grep -q 'n00bb.runtime-module-initializer-order v1 complete' \
    "$tmp/thread_runtime_client.out"
awk '
    /^module thread_runtime_dep.impl \{/ { current = "implementation" }
    /^module thread_runtime_dep.facade \{/ { current = "facade" }
    /^module thread_runtime_client \{/ { current = "client" }
    current == "implementation" && /__n00bb_runtime_init/ && /initializer_order\(0\)/ { impl_order = 1 }
    current == "facade" && /__n00bb_runtime_init/ && /initializer_order\(1\)/ { facade_order = 1 }
    current == "client" && /__n00bb_runtime_init/ && /initializer_order\(2\)/ { client_order = 1 }
    END { exit !(impl_order && facade_order && client_order) }
' "$tmp/thread_runtime_client.out"
grep -Fq 'runtime-initializer-use[resolved] <initializer>: -> thread_runtime_dep.impl:0:worker_state' \
    "$tmp/thread_runtime_client.out"
grep -q 'global.store @thread_runtime_dep.impl.worker_state' \
    "$tmp/thread_runtime_client.out"
grep -q 'global.store @thread_runtime_client.initial_worker_state' \
    "$tmp/thread_runtime_client.out"
grep -q 'func @read_worker_state_after_initialization' \
    "$tmp/thread_runtime_client.out"

"$compiler" lower --dump-ir \
    "$program_dir/thread_runtime_read_after_write.n00b" \
    >"$tmp/thread_runtime_read_after_write.ir"
awk '
    /func @__n00bb_runtime_init/ { in_init = 1 }
    in_init && /global.store @thread_runtime_read_after_write.worker_state/ { write_line = NR }
    in_init && /global.store @thread_runtime_read_after_write.observed_state/ { read_line = NR }
    in_init && /^  }/ { in_init = 0 }
    END { exit !(write_line && read_line && write_line < read_line) }
' "$tmp/thread_runtime_read_after_write.ir"

"$compiler" lower --dump-ir \
    "$program_dir/thread_runtime_all_paths_write.n00b" \
    >"$tmp/thread_runtime_all_paths_write.ir"
grep -q 'global.store @thread_runtime_all_paths_write.worker_state' \
    "$tmp/thread_runtime_all_paths_write.ir"
grep -q 'global.store @thread_runtime_all_paths_write.observed_state' \
    "$tmp/thread_runtime_all_paths_write.ir"

if "$compiler" lower --dump-ir \
    "$program_dir/thread_runtime_conditional_write.n00b" \
    >"$tmp/thread_runtime_conditional_write.out" 2>&1; then
    echo 'a thread-local read after a conditional-only write unexpectedly lowered' >&2
    exit 1
fi
grep -q 'N00BB-L0044.*thread-local global before a guaranteed runtime initialization' \
    "$tmp/thread_runtime_conditional_write.out"

if "$compiler" lower --dump-ir \
    "$program_dir/thread_runtime_before_definition.n00b" \
    >"$tmp/thread_runtime_before_definition.out" 2>&1; then
    echo 'a thread-local read before its runtime definition unexpectedly lowered' >&2
    exit 1
fi
grep -q 'N00BB-L0044.*thread-local global before a guaranteed runtime initialization' \
    "$tmp/thread_runtime_before_definition.out"

if "$compiler" lower --dump-ir \
    "$program_dir/thread_runtime_read_before_write.n00b" \
    >"$tmp/thread_runtime_read_before_write.out" 2>&1; then
    echo 'a thread-local read before its runtime write unexpectedly lowered' >&2
    exit 1
fi
grep -q 'N00BB-L0044.*thread-local global before a guaranteed runtime initialization' \
    "$tmp/thread_runtime_read_before_write.out"

if "$compiler" lower \
    "$program_dir/thread_const_runtime_write.n00b" \
    >"$tmp/thread_const_runtime_write.out" 2>&1; then
    echo 'a runtime write to a thread const unexpectedly lowered' >&2
    exit 1
fi
grep -q 'cannot assign to this binding during runtime execution' \
    "$tmp/thread_const_runtime_write.out"
grep -q 'compile_local: variable, mutable, type=int, .*phases=comptime, mutable-in=comptime, initializer-in=comptime' \
    "$tmp/branches.symbols"
grep -q 'compile_local: variable, mutable, .*storage-duration=comptime' \
    "$tmp/branches.symbols"
grep -q 'x: parameter, .*phases=comptime+runtime, mutable-in=none, initializer-in=none' \
    "$tmp/branches.symbols"
grep -q 'x: parameter, .*storage-duration=automatic' \
    "$tmp/branches.symbols"
grep -q 'local_value: variable, mutable, .*storage-duration=automatic' \
    "$tmp/branches.symbols"
grep -q 'selected: variable, mutable, .*phases=comptime+runtime, mutable-in=comptime+runtime, initializer-in=comptime+runtime' \
    "$tmp/branches.symbols"
grep -q 'visible: function' "$tmp/branches.symbols"
grep -q 'identity: function, mutable, type=(`a) -> `a fail: nil' \
    "$tmp/branches.symbols"
grep -q 'identity: function, mutable, .*uses=1' "$tmp/branches.symbols"
grep -q 'identity_call: function, mutable, type=() -> int' \
    "$tmp/branches.symbols" || {
    cat "$tmp/branches.symbols"
    exit 1
}
grep -q 'inferred_identity: function, mutable, type=(`a) -> `a fail: nil' \
    "$tmp/branches.symbols"
! grep -Fq 'forall[' "$tmp/branches.symbols"
grep -q 'calls_later: function, mutable, type=() -> int' \
    "$tmp/branches.symbols" || {
    cat "$tmp/branches.symbols"
    exit 1
}
grep -q 'later: function, mutable, .*uses=1' "$tmp/branches.symbols"
grep 'constrained: function' "$tmp/branches.symbols" | grep -q 'Copy'
grep -q 'visibility=public' "$tmp/branches.symbols"
grep -q 'error=string' "$tmp/branches.symbols" || {
    cat "$tmp/branches.symbols"
    exit 1
}
grep -q 'direction=out, pass_by=out' "$tmp/branches.symbols"
grep -q 'direction=modify, pass_by=inout' "$tmp/branches.symbols"
grep -q 'depth=2' "$tmp/branches.symbols"

"$compiler" preprocess --dump-symbols "$program_dir/trait_signatures.n00b" \
    >"$tmp/trait_signatures.out"
grep 'combine: function, mutable, type=(`a, `a) -> `a' \
    "$tmp/trait_signatures.out" | grep -q 'error=string' || {
    cat "$tmp/trait_signatures.out"
    exit 1
}

"$compiler" preprocess "$program_dir/trait_dispatch.n00b" \
    >"$tmp/trait_dispatch.out" 2>&1 || {
    cat "$tmp/trait_dispatch.out"
    exit 1
}

"$compiler" preprocess --dump-symbols --dump-link-unit \
    "$script_dir/programs/generic_trait_dispatch.n00b" \
    >"$tmp/generic_trait_dispatch.out" 2>&1 || {
    cat "$tmp/generic_trait_dispatch.out"
    echo 'constrained generic trait dispatch unexpectedly failed' >&2
    exit 1
}
"$compiler" lower --dump-ir \
    "$script_dir/programs/trait_where_excludes_string.n00b" \
    >"$tmp/trait_where_excludes_string.out" 2>&1 || {
    cat "$tmp/trait_where_excludes_string.out"
    echo 'trait-level exclusion rejected a permitted receiver' >&2
    exit 1
}
grep -q 'extern func @__trait_req_NonString_check.*!= string' \
    "$tmp/trait_where_excludes_string.out"
if "$compiler" lower \
    "$script_dir/programs/trait_where_rejects_string.n00b" \
    >"$tmp/trait_where_rejects_string.out" 2>&1; then
    echo 'trait-level exclusion accepted a string receiver' >&2
    exit 1
fi
grep -q "trait constraint 'trait_where_rejects_string.NonString' is not satisfied for string" \
    "$tmp/trait_where_rejects_string.out"
grep 'generic_render: function' "$tmp/generic_trait_dispatch.out" \
    | grep -q 'Render'
grep -q 'call_generic_render: function, mutable, type=() -> string' \
    "$tmp/generic_trait_dispatch.out"
grep -q 'deferred generic_trait_dispatch -> .*generic trait dispatch awaits specialization' \
    "$tmp/generic_trait_dispatch.out"
grep -q 'trait-dispatch\[resolved\] generic_trait_dispatch:1:generic_render -> generic_trait_dispatch:1:render' \
    "$tmp/generic_trait_dispatch.out"
"$compiler" lower --dump-ir --dump-link-unit \
    "$script_dir/programs/generic_trait_dispatch_lower.n00b" \
    >"$tmp/generic_trait_dispatch.ir" 2>&1 || {
    cat "$tmp/generic_trait_dispatch.ir"
    echo 'constrained generic trait dispatch did not lower to its method requirement' >&2
    exit 1
}
grep -q 'call @render' "$tmp/generic_trait_dispatch.ir"
if grep -q 'trait.method\|type.test' "$tmp/generic_trait_dispatch.ir"; then
    echo 'single-target closed trait dispatch was not lowered to a direct call' >&2
    exit 1
fi
grep -q 'func @generic_render(%item: `a where' \
    "$tmp/generic_trait_dispatch.ir"
grep -q 'trait-dispatch\[resolved\].*:render' \
    "$tmp/generic_trait_dispatch.ir"
"$compiler" lower --dump-ir \
    "$script_dir/programs/generic_trait_default_call.n00b" \
    >"$tmp/generic_trait_default_call.ir" 2>&1 || {
    cat "$tmp/generic_trait_default_call.ir"
    echo 'generic trait call with a defaulted parameter did not lower' >&2
    exit 1
}
grep -q 'call @add_amount, .*missing' \
    "$tmp/generic_trait_default_call.ir"
grep -q '__trait_req_Add_Amount_add_amount.*!ir.optional_parameter(true)' \
    "$tmp/generic_trait_default_call.ir"
"$compiler" library --dump-ir --dump-link-unit \
    "$script_dir/programs/trait_dispatch_single.n00b" \
    >"$tmp/linkable_trait_dispatch.ir" 2>&1 || :
grep -q 'trait.method .*@__trait_req_Render_render' \
    "$tmp/linkable_trait_dispatch.ir"
grep -q 'trait-dispatch\[resolved\].*-> trait_dispatch_single:1:Render.render' \
    "$tmp/linkable_trait_dispatch.ir"
grep -q 'function-global-unknown\[unavailable\].*use_render' \
    "$tmp/linkable_trait_dispatch.ir"
if grep -q 'call @render,' "$tmp/linkable_trait_dispatch.ir" \
    || grep -q 'trait-dispatch\[resolved\].*-> trait_dispatch_single:1:render at' \
        "$tmp/linkable_trait_dispatch.ir" \
    || grep -q '^error\[' "$tmp/linkable_trait_dispatch.ir"; then
    cat "$tmp/linkable_trait_dispatch.ir"
    echo 'linkable trait call lowered to a fixed implementation' >&2
    exit 1
fi
"$compiler" library --dump-ir \
    "$script_dir/programs/index_load_dispatch.n00b" \
    >"$tmp/linkable_index_read.ir" 2>&1 || :
grep -q 'trait.method .*@__trait_req_Index_Read___read' \
    "$tmp/linkable_index_read.ir"
"$compiler" library --dump-ir \
    "$script_dir/programs/array_index_store_lowering.n00b" \
    >"$tmp/linkable_index_write.ir" 2>&1 || :
grep -q 'trait.method .*@__trait_req_Index_Write___write' \
    "$tmp/linkable_index_write.ir"
if grep -q 'call @__write,' "$tmp/linkable_index_write.ir" \
    || grep -q '^error\[' "$tmp/linkable_index_write.ir"; then
    cat "$tmp/linkable_index_write.ir"
    echo 'linkable indexed write lowered to a fixed implementation' >&2
    exit 1
fi
"$compiler" library --dump-ir \
    "$script_dir/programs/preprocessor/equality_trait.n00b" \
    >"$tmp/linkable_equality.ir" 2>&1 || :
grep -q 'trait.method .*@__trait_req_Equal___equal' \
    "$tmp/linkable_equality.ir"
grep -q 'trait.method .*@__trait_req_Comparable___cmp' \
    "$tmp/linkable_equality.ir"
if grep -q 'call @__equal,' "$tmp/linkable_equality.ir" \
    || grep -q 'call @__cmp,' "$tmp/linkable_equality.ir" \
    || grep -q '^error\[' "$tmp/linkable_equality.ir"; then
    cat "$tmp/linkable_equality.ir"
    echo 'linkable comparison lowered to a fixed implementation' >&2
    exit 1
fi
"$compiler" lower --dump-ir \
    "$script_dir/programs/operator_add_result.n00b" \
    >"$tmp/operator_add_result.ir" 2>&1 || {
    cat "$tmp/operator_add_result.ir"
    echo 'operator method result type did not lower' >&2
    exit 1
}
grep -q ': string = call @__add, %left, %right' \
    "$tmp/operator_add_result.ir"
"$compiler" library --dump-ir \
    "$script_dir/programs/operator_add_result.n00b" \
    >"$tmp/linkable_operator_add.ir" 2>&1 || :
grep -q 'trait.method .*@__trait_req_Add___add' \
    "$tmp/linkable_operator_add.ir"
if grep -q 'call @__add,' "$tmp/linkable_operator_add.ir" \
    || grep -q '^error\[' "$tmp/linkable_operator_add.ir"; then
    cat "$tmp/linkable_operator_add.ir"
    echo 'linkable arithmetic operator lowered to a fixed implementation' >&2
    exit 1
fi
"$compiler" lower --dump-ir \
    "$script_dir/programs/operator_add_failure.n00b" \
    >"$tmp/operator_add_failure.ir" 2>&1 || {
    cat "$tmp/operator_add_failure.ir"
    echo 'failing operator method did not lower' >&2
    exit 1
}
grep -q 'variant.payload .*"error"' "$tmp/operator_add_failure.ir"
grep -q 'fail %' "$tmp/operator_add_failure.ir"
if "$compiler" lower --dump-ir \
    "$script_dir/programs/operator_add_wrong_argument.n00b" \
    >"$tmp/operator_add_wrong_argument.ir" 2>&1; then
    echo 'operator method accepted an incompatible right argument' >&2
    exit 1
fi
grep -q 'error\[N00BB-T0003\].*__add' \
    "$tmp/operator_add_wrong_argument.ir"
"$compiler" lower --dump-ir \
    "$script_dir/programs/operator_generic_builtin.n00b" \
    >"$tmp/operator_generic_builtin.ir" 2>&1 || {
    cat "$tmp/operator_generic_builtin.ir"
    echo 'generic builtin operator requirements did not lower' >&2
    exit 1
}
for method in Add___add Negate___negate Not___not Comparable___cmp; do
    grep -q "trait.method .*@__trait_req_$method" \
        "$tmp/operator_generic_builtin.ir"
done
grep -Fq 'call_signature((`b, `c) -> `a fail: `d where' \
    "$tmp/operator_generic_builtin.ir"
grep -Fq 'call_signature((`b) -> `a fail: `c where' \
    "$tmp/operator_generic_builtin.ir"
"$compiler" lower --dump-ir \
    "$script_dir/programs/operator_primitive_comparison_traits.n00b" \
    >"$tmp/operator_primitive_comparison_traits.ir" 2>&1 || {
    cat "$tmp/operator_primitive_comparison_traits.ir"
    echo 'primitive comparison traits did not satisfy generic calls' >&2
    exit 1
}
for type in int bool string f64 'bitint[7]' 'bits:127'; do
    grep -Fq "call_signature(($type, $type) -> bool fail: nil)" \
        "$tmp/operator_primitive_comparison_traits.ir"
done
grep -q 'trait.method "Comparable", "__cmp"' \
    "$tmp/operator_primitive_comparison_traits.ir"
grep -q 'trait.method "Equal", "__equal"' \
    "$tmp/operator_primitive_comparison_traits.ir"
if "$compiler" lower \
    "$script_dir/programs/operator_primitive_bool_order_rejected.n00b" \
    >"$tmp/operator_primitive_bool_order_rejected.out" 2>&1; then
    echo 'bool unexpectedly satisfied Comparable' >&2
    exit 1
fi
grep -q 'constraint implements: interface not satisfied' \
    "$tmp/operator_primitive_bool_order_rejected.out"
"$compiler" lower --dump-ir \
    "$script_dir/programs/operator_boolean_short_circuit.n00b" \
    >"$tmp/operator_boolean_short_circuit.ir" 2>&1 || {
    cat "$tmp/operator_boolean_short_circuit.ir"
    echo 'short-circuit boolean operators did not lower' >&2
    exit 1
}
grep -q 'branch %left, __n00bb_if_then_1, __n00bb_if_join_0' \
    "$tmp/operator_boolean_short_circuit.ir"
grep -q 'branch %left, __n00bb_if_join_0, __n00bb_if_then_1' \
    "$tmp/operator_boolean_short_circuit.ir"
if "$compiler" lower \
    "$script_dir/programs/operator_boolean_wrong_argument.n00b" \
    >"$tmp/operator_boolean_wrong_argument.out" 2>&1; then
    echo 'logical operator accepted an integer operand' >&2
    exit 1
fi
grep -q 'logical operands must be bool' \
    "$tmp/operator_boolean_wrong_argument.out"
"$compiler" lower --dump-ir \
    "$script_dir/programs/operator_nominal_cast.n00b" \
    >"$tmp/operator_nominal_cast.ir" 2>&1 || {
    cat "$tmp/operator_nominal_cast.ir"
    echo 'nominal conversions and any boxing did not lower' >&2
    exit 1
}
grep -q ': operator_nominal_cast.Amount = nominal.cast ' \
    "$tmp/operator_nominal_cast.ir"
grep -q ': int = nominal.cast ' "$tmp/operator_nominal_cast.ir"
grep -q ': any = box .*ir.alias0_escapes(true)' \
    "$tmp/operator_nominal_cast.ir"
if "$compiler" lower \
    "$script_dir/programs/operator_nominal_cast_unrelated.n00b" \
    >"$tmp/operator_nominal_cast_unrelated.out" 2>&1; then
    echo 'unrelated nominal types converted without a language rule' >&2
    exit 1
fi
grep -q 'this cast has no defined conversion' \
    "$tmp/operator_nominal_cast_unrelated.out"
N00B_PATH="$interface_programs" "$compiler" library \
    --emit-interface "$tmp/operator_nominal.n00bi" \
    operator_nominal_archive_dep.facade \
    >"$tmp/operator_nominal_archive.out" 2>&1 || {
    cat "$tmp/operator_nominal_archive.out"
    exit 1
}
"$compiler" lower --dump-ir --link-interface "$tmp/operator_nominal.n00bi" \
    "$script_dir/programs/interfaces/operator_nominal_archive_user.n00b" \
    >"$tmp/operator_nominal_archive_user.ir" 2>&1 || {
    cat "$tmp/operator_nominal_archive_user.ir"
    exit 1
}
grep -q ': int = nominal.cast ' "$tmp/operator_nominal_archive_user.ir"
"$compiler" lower --dump-ir --dump-symbols \
    "$script_dir/programs/operator_enum_signed.n00b" \
    >"$tmp/operator_enum_signed.ir" 2>&1 || {
    cat "$tmp/operator_enum_signed.ir"
    echo 'signed enum constants did not lower' >&2
    exit 1
}
grep -q 'enum.const "negative", -7' "$tmp/operator_enum_signed.ir"
grep -q 'enum.const "following", -6' "$tmp/operator_enum_signed.ir"
grep -q 'enum.const "zero", 0' "$tmp/operator_enum_signed.ir"
grep -q 'enum.const "positive", 7' "$tmp/operator_enum_signed.ir"
grep -q 'enum.const "across_zero", 0' "$tmp/operator_enum_signed.ir"
grep -q 'enum.const "after_zero", 1' "$tmp/operator_enum_signed.ir"
grep -q 'Signed: type.*enum-value-range=i:4' "$tmp/operator_enum_signed.ir"
"$compiler" lower --dump-symbols \
    "$script_dir/programs/operator_enum_ranges.n00b" \
    >"$tmp/operator_enum_ranges.out" 2>&1 || {
    cat "$tmp/operator_enum_ranges.out"
    exit 1
}
grep -q 'Zero: type.*enum-value-range=zero-only' "$tmp/operator_enum_ranges.out"
grep -q 'Unsigned: type.*enum-value-range=u:8' "$tmp/operator_enum_ranges.out"
grep -q 'Signed: type.*enum-value-range=i:5' "$tmp/operator_enum_ranges.out"
grep -q 'Wider: type.*enum-value-range=u:12' "$tmp/operator_enum_ranges.out"
grep -q 'Wide: type.*enum-value-range=u:128' "$tmp/operator_enum_ranges.out"
if "$compiler" lower \
    "$script_dir/programs/operator_enum_noninteger_rejected.n00b" \
    >"$tmp/operator_enum_noninteger_rejected.out" 2>&1; then
    echo 'enum accepted a noninteger item value' >&2
    exit 1
fi
grep -q 'enum item value must be an integer literal' \
    "$tmp/operator_enum_noninteger_rejected.out"
if "$compiler" parse \
    "$script_dir/programs/enum_colon_rejected.n00b" \
    >"$tmp/enum_colon_rejected.out" 2>&1; then
    echo 'enum accepted a colon initializer' >&2
    exit 1
fi
grep -q 'N00BB-P0001.*syntax error' "$tmp/enum_colon_rejected.out"
N00B_PATH="$interface_programs" "$compiler" library \
    --emit-interface "$tmp/operator_enum_signed.n00bi" \
    operator_enum_signed_dep.facade \
    >"$tmp/operator_enum_signed_archive.out" 2>&1 || {
    cat "$tmp/operator_enum_signed_archive.out"
    exit 1
}
"$compiler" lower --dump-ir --dump-symbols \
    --link-interface "$tmp/operator_enum_signed.n00bi" \
    "$script_dir/programs/interfaces/operator_enum_signed_user.n00b" \
    >"$tmp/operator_enum_signed_user.ir" 2>&1 || {
    cat "$tmp/operator_enum_signed_user.ir"
    exit 1
}
grep -q 'enum.const "negative", -7' "$tmp/operator_enum_signed_user.ir"
grep -q 'enum.const "following", -6' "$tmp/operator_enum_signed_user.ir"
grep -q 'Signed: type.*enum-value-range=i:4' "$tmp/operator_enum_signed_user.ir"
"$compiler" lower --dump-ir \
    "$script_dir/programs/operator_byte_cast.n00b" \
    >"$tmp/operator_byte_cast.ir" 2>&1 || {
    cat "$tmp/operator_byte_cast.ir"
    echo 'explicit byte-to-integer casts did not lower' >&2
    exit 1
}
grep -q ': int = cast trap %value' "$tmp/operator_byte_cast.ir"
grep -q ': u8 = cast %value' "$tmp/operator_byte_cast.ir"
grep -q ': i8 = cast trap %value' "$tmp/operator_byte_cast.ir"
grep -q ': ubitint\[3\] = cast trap %value' "$tmp/operator_byte_cast.ir"
if "$compiler" lower \
    "$script_dir/programs/operator_byte_arithmetic_rejected.n00b" \
    >"$tmp/operator_byte_arithmetic_rejected.out" 2>&1; then
    echo 'byte arithmetic accepted without an explicit integer cast' >&2
    exit 1
fi
if "$compiler" lower \
    "$script_dir/programs/operator_integer_to_byte_rejected.n00b" \
    >"$tmp/operator_integer_to_byte_rejected.out" 2>&1; then
    echo 'integer converted to byte without a defined conversion' >&2
    exit 1
fi
grep -q 'this cast has no defined conversion' \
    "$tmp/operator_integer_to_byte_rejected.out"
"$compiler" lower --dump-ir \
    "$script_dir/programs/operator_numeric_identity.n00b" \
    >"$tmp/operator_numeric_identity.ir" 2>&1 || {
    cat "$tmp/operator_numeric_identity.ir"
    echo 'primitive numeric identity did not lower' >&2
    exit 1
}
grep -q ': int = call @inferred, %value' "$tmp/operator_numeric_identity.ir"
grep -q ': f64 = call @inferred, %value' "$tmp/operator_numeric_identity.ir"
grep -q ': ubitint\[37\] = call @inferred, %value' \
    "$tmp/operator_numeric_identity.ir"
for kind in byte string inferred; do
    if "$compiler" lower \
        "$script_dir/programs/operator_numeric_identity_${kind}_rejected.n00b" \
        >"$tmp/operator_numeric_identity_${kind}_rejected.out" 2>&1; then
        echo "numeric identity accepted a nonnumeric ${kind} operand" >&2
        exit 1
    fi
    grep -q 'error\[N00BB-T000[12]\]' \
        "$tmp/operator_numeric_identity_${kind}_rejected.out"
done
N00B_PATH="$interface_programs" "$compiler" library \
    --emit-interface "$tmp/operator_numeric_identity.n00bi" \
    operator_numeric_identity_dep.facade \
    >"$tmp/operator_numeric_identity_archive.out" 2>&1 || {
    cat "$tmp/operator_numeric_identity_archive.out"
    exit 1
}
"$compiler" lower --dump-ir \
    --link-interface "$tmp/operator_numeric_identity.n00bi" \
    "$script_dir/programs/interfaces/operator_numeric_identity_user.n00b" \
    >"$tmp/operator_numeric_identity_user.ir" 2>&1 || {
    cat "$tmp/operator_numeric_identity_user.ir"
    exit 1
}
grep -q 'ir.call_signature((ubitint\[37\]) -> ubitint\[37\]' \
    "$tmp/operator_numeric_identity_user.ir"
if "$compiler" lower --link-interface "$tmp/operator_numeric_identity.n00bi" \
    "$script_dir/programs/interfaces/operator_numeric_identity_rejected.n00b" \
    >"$tmp/operator_numeric_identity_archive_rejected.out" 2>&1; then
    echo 'frozen numeric identity contract accepted a string' >&2
    exit 1
fi
grep -q 'error\[N00BB-T000[12]\]' \
    "$tmp/operator_numeric_identity_archive_rejected.out"
for location in evaluation global; do
    "$compiler" lower --dump-ir \
        "$script_dir/programs/operator_compound_${location}_order.n00b" \
        >"$tmp/operator_compound_${location}_order.ir" 2>&1 || {
        cat "$tmp/operator_compound_${location}_order.ir"
        exit 1
    }
    awk '
        / = copy [$@]/ {
            name = $1
            sub(/:$/, "", name)
            captured[name] = NR
        }
        / = call @change/ { call_line = NR }
        / = add trap / {
            for (i = 1; i <= NF; i++) {
                if ($i != "trap") continue
                operand = $(i + 1)
                sub(/,$/, "", operand)
                snapshot_line = captured[operand]
                break
            }
        }
        END { exit !(snapshot_line && call_line && snapshot_line < call_line) }
    ' "$tmp/operator_compound_${location}_order.ir" || {
        cat "$tmp/operator_compound_${location}_order.ir"
        echo 'compound assignment read its target after the RHS call' >&2
        exit 1
    }
done
"$compiler" lower --dump-ir \
    "$script_dir/programs/operator_compound_traits.n00b" \
    >"$tmp/operator_compound_traits.ir" 2>&1 || {
    cat "$tmp/operator_compound_traits.ir"
    exit 1
}
grep -q '= call @__add,' "$tmp/operator_compound_traits.ir"
grep -q '= call @__string_add,' "$tmp/operator_compound_traits.ir"
grep -q ': ubitint\[37\] = shl trap .*%count' "$tmp/operator_compound_traits.ir"
grep -q ': ubitint\[37\] = shr trap .*%count' "$tmp/operator_compound_traits.ir"
"$compiler" library --dump-ir \
    "$script_dir/programs/operator_compound_traits.n00b" \
    >"$tmp/operator_compound_traits_linkable.ir" 2>&1 || :
test "$(grep -c 'trait.method "Add", "__add".*@__trait_req_Add___add' \
    "$tmp/operator_compound_traits_linkable.ir")" -eq 2
if grep -q '^error\[' "$tmp/operator_compound_traits_linkable.ir"; then
    cat "$tmp/operator_compound_traits_linkable.ir"
    exit 1
fi
for kind in byte result; do
    if "$compiler" lower \
        "$script_dir/programs/operator_compound_${kind}_rejected.n00b" \
        >"$tmp/operator_compound_${kind}_rejected.out" 2>&1; then
        echo "compound operator accepted an invalid ${kind} contract" >&2
        exit 1
    fi
    grep -q 'trait constraint.*__add.*not satisfied' \
        "$tmp/operator_compound_${kind}_rejected.out"
done
"$compiler" lower --dump-ir \
    "$script_dir/programs/operator_compound_generic.n00b" \
    >"$tmp/operator_compound_generic.ir" 2>&1 || {
    cat "$tmp/operator_compound_generic.ir"
    exit 1
}
grep -Fq 'call_signature((int, int) -> int fail: nil)' \
    "$tmp/operator_compound_generic.ir"
grep -Fq 'call_signature((string, string) -> string fail: nil)' \
    "$tmp/operator_compound_generic.ir"
"$compiler" lower --dump-ir \
    "$script_dir/programs/operator_compound_generic_failure.n00b" \
    >"$tmp/operator_compound_generic_failure.ir" 2>&1 || {
    cat "$tmp/operator_compound_generic_failure.ir"
    exit 1
}
grep -Fq 'call_signature((operator_compound_generic_failure.Box, int) -> operator_compound_generic_failure.Box fail: int)' \
    "$tmp/operator_compound_generic_failure.ir"
grep -q 'trait.method "Add", "__add"' \
    "$tmp/operator_compound_generic_failure.ir"
grep -q 'variant.payload .*"error"' \
    "$tmp/operator_compound_generic_failure.ir"
grep -q 'store .*__n00bb_outcome_' \
    "$tmp/operator_compound_generic_failure.ir"
if "$compiler" lower \
    "$script_dir/programs/operator_compound_generic_unwrap_rejected.n00b" \
    >"$tmp/operator_compound_generic_unwrap_rejected.out" 2>&1; then
    echo 'postfix ! accepted an infallible generic specialization' >&2
    exit 1
fi
grep -q 'postfix ! requires a failable expression; this call is infallible' \
    "$tmp/operator_compound_generic_unwrap_rejected.out"
test "$(grep -c '^error\[' \
    "$tmp/operator_compound_generic_unwrap_rejected.out")" -eq 1
"$compiler" lower --dump-ir \
    "$script_dir/programs/operator_unary_traits.n00b" \
    >"$tmp/operator_unary_traits.ir" 2>&1 || {
    cat "$tmp/operator_unary_traits.ir"
    echo 'unary trait methods did not lower' >&2
    exit 1
}
grep -q ': string = call @__negate, %item' \
    "$tmp/operator_unary_traits.ir"
grep -q ': bool = call @__not, %item' \
    "$tmp/operator_unary_traits.ir"
"$compiler" library --dump-ir \
    "$script_dir/programs/operator_unary_traits.n00b" \
    >"$tmp/linkable_operator_unary_traits.ir" 2>&1 || :
grep -q 'trait.method "Negate", "__negate".*@__trait_req_Negate___negate' \
    "$tmp/linkable_operator_unary_traits.ir"
grep -q 'trait.method "Not", "__not".*@__trait_req_Not___not' \
    "$tmp/linkable_operator_unary_traits.ir"
if grep -q 'call @__negate,' "$tmp/linkable_operator_unary_traits.ir" \
    || grep -q 'call @__not,' "$tmp/linkable_operator_unary_traits.ir" \
    || grep -q '^error\[' "$tmp/linkable_operator_unary_traits.ir"; then
    cat "$tmp/linkable_operator_unary_traits.ir"
    echo 'linkable unary operator lowered to a fixed implementation' >&2
    exit 1
fi
"$compiler" lower --dump-ir \
    "$script_dir/programs/operator_failure_coverage.n00b" \
    >"$tmp/operator_failure_coverage.ir" 2>&1 || {
    cat "$tmp/operator_failure_coverage.ir"
    echo 'operator failure coverage did not lower' >&2
    exit 1
}
for method in subtract multiply divide remainder bit_and bit_or bit_xor \
        shift_left shift_right negate not cmp write; do
    grep -q "= call @__$method," "$tmp/operator_failure_coverage.ir"
done
grep -q 'variant.payload .*"error"' "$tmp/operator_failure_coverage.ir"
grep -q 'fail %__n00bb_' "$tmp/operator_failure_coverage.ir"
"$compiler" preprocess --dump-link-unit \
    "$script_dir/programs/operator_failure_coverage.n00b" \
    >"$tmp/operator_failure_coverage.effects" 2>&1 || {
    cat "$tmp/operator_failure_coverage.effects"
    echo 'operator call effects were not analyzed' >&2
    exit 1
}
grep -q 'function-global-write-summary\[resolved\] operator_failure_coverage:1:subtract -> operator_failure_coverage:0:operator_state' \
    "$tmp/operator_failure_coverage.effects"
"$compiler" library --dump-ir \
    "$script_dir/programs/operator_failure_coverage.n00b" \
    >"$tmp/operator_failure_coverage_linkable.ir" 2>&1 || :
for dispatch in \
        'Subtract __subtract' \
        'Multiply __multiply' \
        'Divide __divide' \
        'Remainder __remainder' \
        'Bit_And __bit_and' \
        'Bit_Or __bit_or' \
        'Bit_Xor __bit_xor' \
        'Shift_Left __shift_left' \
        'Shift_Right __shift_right' \
        'Negate __negate' \
        'Not __not' \
        'Comparable __cmp' \
        'operator_failure_coverage.Index_Write __write'; do
    trait=${dispatch% *}
    method=${dispatch#* }
    grep -q "trait.method \"$trait\", \"$method\"" \
        "$tmp/operator_failure_coverage_linkable.ir"
done
if grep -Eq '= call @__(subtract|multiply|divide|remainder|bit_and|bit_or|bit_xor|shift_left|shift_right|negate|not|cmp|write),' \
        "$tmp/operator_failure_coverage_linkable.ir" \
    || grep -q '^error\[' "$tmp/operator_failure_coverage_linkable.ir"; then
    cat "$tmp/operator_failure_coverage_linkable.ir"
    echo 'linkable operator failure path lowered to a fixed implementation' >&2
    exit 1
fi
"$compiler" lower --dump-ir --dump-link-unit \
    "$script_dir/programs/trait_dispatch_multiple.n00b" \
    >"$tmp/trait_dispatch_multiple.ir" 2>&1 || {
    cat "$tmp/trait_dispatch_multiple.ir"
    exit 1
}
test "$(grep -c 'type.test' "$tmp/trait_dispatch_multiple.ir")" -eq 2
test "$(grep -c 'type.narrow' "$tmp/trait_dispatch_multiple.ir")" -eq 2
test "$(grep -c 'trait-dispatch\[resolved\]' \
    "$tmp/trait_dispatch_multiple.ir")" -eq 2
if grep -q 'trait.method' "$tmp/trait_dispatch_multiple.ir"; then
    cat "$tmp/trait_dispatch_multiple.ir"
    echo 'closed trait-valued call retained dynamic dispatch' >&2
    exit 1
fi
"$compiler" library --dump-ir \
    "$script_dir/programs/trait_dispatch_multiple.n00b" \
    >"$tmp/trait_dispatch_multiple_linkable.ir" 2>&1 || :
grep -q 'trait.method "trait_dispatch_multiple.Render", "render"' \
    "$tmp/trait_dispatch_multiple_linkable.ir"
if grep -q 'type.test' "$tmp/trait_dispatch_multiple_linkable.ir" \
    || grep -q '^fatal\[' "$tmp/trait_dispatch_multiple_linkable.ir"; then
    cat "$tmp/trait_dispatch_multiple_linkable.ir"
    echo 'linkable trait-valued call was prematurely specialized' >&2
    exit 1
fi
"$compiler" lower --dump-ir \
    "$script_dir/programs/operator_string_add.n00b" \
    >"$tmp/operator_string_add.ir" 2>&1 || {
    cat "$tmp/operator_string_add.ir"
    exit 1
}
grep -q 'extern func @__string_add' "$tmp/operator_string_add.ir"
grep -q ': string = call @__string_add,' "$tmp/operator_string_add.ir"
"$compiler" library --dump-ir \
    "$script_dir/programs/operator_string_add.n00b" \
    >"$tmp/linkable_operator_string_add.ir" 2>&1 || :
grep -q 'trait.method "Add", "__add".*@__trait_req_Add___add' \
    "$tmp/linkable_operator_string_add.ir"
if grep -q 'call @__string_add,' \
    "$tmp/linkable_operator_string_add.ir"; then
    cat "$tmp/linkable_operator_string_add.ir"
    exit 1
fi
"$compiler" lower --dump-ir --dump-link-unit \
    "$script_dir/programs/generic_trait_correlated_result.n00b" \
    >"$tmp/generic_trait_correlated_result.ir" 2>&1 || {
    cat "$tmp/generic_trait_correlated_result.ir"
    echo 'generic trait call with a receiver-correlated result did not lower' >&2
    exit 1
}
grep -q 'func @generic_echo(%item: `a' \
    "$tmp/generic_trait_correlated_result.ir"
grep -q ': int = call @echo,' \
    "$tmp/generic_trait_correlated_result.ir"
grep -Fq 'call_signature((int) -> int fail: nil)' \
    "$tmp/generic_trait_correlated_result.ir"
if grep -q 'trait.method\|type.test' \
        "$tmp/generic_trait_correlated_result.ir"; then
    echo 'single-target correlated trait result retained dynamic dispatch' >&2
    exit 1
fi
"$compiler" lower --dump-ir \
    "$script_dir/programs/generic_trait_result_propagation.n00b" \
    >"$tmp/generic_trait_result_propagation.ir" 2>&1 || {
    cat "$tmp/generic_trait_result_propagation.ir"
    echo 'resolved trait result did not propagate into its generic call instance' >&2
    exit 1
}
grep -q 'func @generic_render(%item: `a where' \
    "$tmp/generic_trait_result_propagation.ir"
grep -q ': int = call @generic_render' \
    "$tmp/generic_trait_result_propagation.ir"
grep -Fq 'call_signature((int) -> int fail: nil)' \
    "$tmp/generic_trait_result_propagation.ir"
"$compiler" lower --dump-ir \
    "$script_dir/programs/generic_trait_reference_result.n00b" \
    >"$tmp/generic_trait_reference_result.ir" 2>&1 || {
    cat "$tmp/generic_trait_reference_result.ir"
    echo 'generic trait reference result did not lower' >&2
    exit 1
}
grep -Fq 'call_signature((int, ref[int]) -> ref[int] fail: nil)' \
    "$tmp/generic_trait_reference_result.ir"
grep -Fq '!ir.alias_analysis_complete(true)' \
    "$tmp/generic_trait_reference_result.ir"
if "$compiler" lower --dump-ir \
    "$program_dir/generic_trait_resolved_argument_mismatch.n00b" \
    >"$tmp/generic_trait_resolved_argument_mismatch.out" 2>&1; then
    echo 'generic trait specialization accepted an incompatible method argument' >&2
    exit 1
fi
grep -q 'N00BB-T0003.*generic call argument 2 has type' \
    "$tmp/generic_trait_resolved_argument_mismatch.out"
"$compiler" lower --dump-ir --dump-link-unit \
    "$script_dir/programs/generic_trait_failure_channel.n00b" \
    >"$tmp/generic_trait_failure_channel.ir" 2>&1 || {
    cat "$tmp/generic_trait_failure_channel.ir"
    echo 'generic trait failure-channel call did not lower' >&2
    exit 1
}
grep -q 'func @generic_read_failure(%item: `a' \
    "$tmp/generic_trait_failure_channel.ir"
grep -Fq '@__trait_req_ReadFailure_read_failure' \
    "$tmp/generic_trait_failure_channel.ir"
grep -Fq 'call_signature((int) -> int fail: string)' \
    "$tmp/generic_trait_failure_channel.ir"
grep -q 'variant.payload .*"error"' \
    "$tmp/generic_trait_failure_channel.ir"
grep -q 'fail %__n00bb_' "$tmp/generic_trait_failure_channel.ir"
if grep -q 'trait.method\|type.test' \
        "$tmp/generic_trait_failure_channel.ir"; then
    echo 'single-target trait failure channel retained dynamic dispatch' >&2
    exit 1
fi
N00B_PATH="$script_dir/programs/generic_trait_specialization" \
    "$compiler" lower --dump-ir --dump-link-unit \
    "$script_dir/programs/generic_trait_specialization/root.n00b" \
    >"$tmp/generic_trait_specialization.out" \
    2>"$tmp/generic_trait_specialization.err" || {
    cat "$tmp/generic_trait_specialization.out"
    cat "$tmp/generic_trait_specialization.err" >&2
    echo 'concrete generic trait specializations did not resolve' >&2
    exit 1
}
grep -q 'trait-specialization\[resolved\] root:1:generic_render -> generic_trait_specialization.int_impl:1:render.*receiver=int' \
    "$tmp/generic_trait_specialization.out"
grep -q 'trait-specialization\[resolved\] root:1:generic_render -> generic_trait_specialization.label_impl:1:render.*receiver=generic_trait_specialization.label_impl.Label' \
    "$tmp/generic_trait_specialization.out"
grep -q 'func @generic_render(%item: `a where' \
    "$tmp/generic_trait_specialization.out"
test "$(grep -c 'type.test' "$tmp/generic_trait_specialization.out")" -eq 4
test "$(grep -c 'type.narrow' "$tmp/generic_trait_specialization.out")" -eq 4
test "$(grep -c '= call @render' \
    "$tmp/generic_trait_specialization.out")" -eq 2
test "$(grep -c '= call @measure' \
    "$tmp/generic_trait_specialization.out")" -eq 2
if grep -q 'trait.method' "$tmp/generic_trait_specialization.out"; then
    echo 'closed cross-module trait specialization retained dynamic dispatch' >&2
    exit 1
fi
generic_specialization_count=$(grep -c \
    'trait-specialization\[resolved\] root:1:generic_render' \
    "$tmp/generic_trait_specialization.out")
if [ "$generic_specialization_count" -ne 4 ]; then
    grep 'trait-specialization.*root:1:generic_render' \
        "$tmp/generic_trait_specialization.out" >&2
    echo "expected 4 generic trait specializations, got $generic_specialization_count" >&2
    exit 1
fi
N00B_PATH="$script_dir/programs/generic_trait_specialization" \
    "$compiler" preprocess --dump-symbols \
    "$script_dir/programs/generic_trait_specialization/root.n00b" \
    >"$tmp/generic_trait_specialization.symbols" 2>&1
grep -q 'measured: variable, mutable, type=int' \
    "$tmp/generic_trait_specialization.symbols"
if N00B_PATH="$script_dir/programs/generic_trait_specialization" "$compiler" preprocess --dump-link-unit "$script_dir/programs/generic_trait_specialization/missing.n00b" >"$tmp/generic_trait_specialization_missing.out" 2>&1; then
    cat "$tmp/generic_trait_specialization_missing.out"
    echo 'generic trait call without a concrete implementation unexpectedly passed' >&2
    exit 1
fi
grep -q 'N00BB-T0003.*trait constraint.*is not satisfied for bool' "$tmp/generic_trait_specialization_missing.out"
if N00B_PATH="$script_dir/programs/generic_trait_specialization" "$compiler" preprocess --dump-link-unit "$script_dir/programs/generic_trait_specialization/ambiguous.n00b" >"$tmp/generic_trait_specialization_ambiguous.out" 2>&1; then
    cat "$tmp/generic_trait_specialization_ambiguous.out"
    echo 'ambiguous generic trait specialization unexpectedly passed' >&2
    exit 1
fi
grep -q 'N00BB-T0003.*trait constraint.*method dispatch is ambiguous' "$tmp/generic_trait_specialization_ambiguous.out"

N00B_PATH="$interface_programs" "$compiler" library \
    --emit-interface "$tmp/generic_trait_archive.n00bi" \
    generic_trait_archive_dep.facade \
    >"$tmp/generic_trait_archive_library.out" \
    2>"$tmp/generic_trait_archive_library.err" || {
    cat "$tmp/generic_trait_archive_library.err"
    exit 1
}
"$compiler" lower --link-interface "$tmp/generic_trait_archive.n00bi" \
    --dump-ir --dump-link-unit \
    "$script_dir/programs/interfaces/generic_trait_archive_user.n00b" \
    >"$tmp/generic_trait_archive_user.out" \
    2>"$tmp/generic_trait_archive_user.err" || {
    cat "$tmp/generic_trait_archive_user.err"
    exit 1
}
grep -q 'extern func @archived_render' "$tmp/generic_trait_archive_user.out"
grep -q 'call @archived_render' "$tmp/generic_trait_archive_user.out"
grep -q 'call_signature((generic_trait_archive_user.ArchiveWidget) -> int fail: nil)' \
    "$tmp/generic_trait_archive_user.out"
grep -q 'extern func @archived_render.*ArchiveRender.*callable_contract((`a) -> int fail: nil where' \
    "$tmp/generic_trait_archive_user.out"
! grep -Fq 'forall[' "$tmp/generic_trait_archive_user.out"
if "$compiler" lower --link-interface "$tmp/generic_trait_archive.n00bi" \
    "$script_dir/programs/interfaces/generic_trait_archive_missing_impl.n00b" \
    >"$tmp/generic_trait_archive_missing_impl.out" 2>&1; then
    echo 'archived generic trait call without an implementation unexpectedly passed' >&2
    exit 1
fi
grep -q 'constraint implements: interface not satisfied' \
    "$tmp/generic_trait_archive_missing_impl.out"

N00B_PATH="$interface_programs" "$compiler" library \
    --emit-interface "$tmp/trait_archive_render.n00bi" \
    trait_archive_render_dep.facade \
    >"$tmp/trait_archive_render_library.out" \
    2>"$tmp/trait_archive_render_library.err" || {
    cat "$tmp/trait_archive_render_library.err"
    exit 1
}
N00B_PATH="$interface_programs" "$compiler" library \
    --emit-interface "$tmp/trait_archive_measure.n00bi" \
    trait_archive_measure_dep.facade \
    >"$tmp/trait_archive_measure_library.out" \
    2>"$tmp/trait_archive_measure_library.err" || {
    cat "$tmp/trait_archive_measure_library.err"
    exit 1
}
N00B_PATH="$script_dir/programs/trait_archive_impls" \
    "$compiler" lower --link-interface "$tmp/trait_archive_render.n00bi" \
    --link-interface "$tmp/trait_archive_measure.n00bi" \
    --dump-ir --dump-link-unit \
    "$script_dir/programs/interfaces/trait_archive_multi_user.n00b" \
    >"$tmp/trait_archive_multi_user.out" \
    2>"$tmp/trait_archive_multi_user.err" || {
    cat "$tmp/trait_archive_multi_user.err"
    cat "$tmp/trait_archive_multi_user.out"
    exit 1
}
grep -q 'extern func @archived_render.*ArchiveRender' \
    "$tmp/trait_archive_multi_user.out"
grep -q 'extern func @archived_measure.*ArchiveMeasure.*fail: string' \
    "$tmp/trait_archive_multi_user.out"
grep -q 'extern func @archived_render.*trait_archive_render_dep.impl.ArchiveRender.*callable_contract((`a) -> int fail: nil where' \
    "$tmp/trait_archive_multi_user.out"
grep -q 'extern func @archived_measure.*trait_archive_measure_dep.impl.ArchiveMeasure.*callable_contract((`a) -> int fail: string where' \
    "$tmp/trait_archive_multi_user.out"
! grep -Fq 'forall[' "$tmp/trait_archive_multi_user.out"
test "$(grep -c 'call @archived_render' "$tmp/trait_archive_multi_user.out")" -eq 2
test "$(grep -c 'call @archived_measure' "$tmp/trait_archive_multi_user.out")" -eq 2
grep -q 'trait-dispatch\[resolved\] trait_archive_multi_user:1:dispatch_archive_render -> trait_archive_multi_user:1:render' \
    "$tmp/trait_archive_multi_user.out"
grep -q 'trait-dispatch\[resolved\] trait_archive_multi_user:1:dispatch_archive_measure -> trait_archive_multi_user:1:measure' \
    "$tmp/trait_archive_multi_user.out"
if N00B_PATH="$script_dir/programs/trait_archive_impls" \
    "$compiler" lower --link-interface "$tmp/trait_archive_render.n00bi" \
    --link-interface "$tmp/trait_archive_measure.n00bi" \
    "$script_dir/programs/interfaces/trait_archive_multi_missing.n00b" \
    >"$tmp/trait_archive_multi_missing.out" 2>&1; then
    echo 'generic calls from two archives accepted a type missing both traits' >&2
    exit 1
fi
grep -q 'constraint implements: interface not satisfied' \
    "$tmp/trait_archive_multi_missing.out"

N00B_PATH="$script_dir/programs" "$compiler" lower --dump-ir --dump-link-unit \
    "$script_dir/programs/cycle_trait_root.n00b" \
    >"$tmp/cycle_trait.out" 2>&1 || {
    cat "$tmp/cycle_trait.out"
    echo 'trait dispatch across a cyclic module closure unexpectedly failed' >&2
    exit 1
}
grep -q 'trait-dispatch\[resolved\] cycle_trait.b:1:dispatch -> cycle_trait.a:1:echo' \
    "$tmp/cycle_trait.out"

if "$compiler" preprocess "$program_dir/trait_dispatch_missing.n00b" \
    >"$tmp/trait_dispatch_missing.out" 2>&1; then
    cat "$tmp/trait_dispatch_missing.out"
    echo 'missing trait candidate unexpectedly passed' >&2
    exit 1
fi
grep -q 'N00BB-T0003' "$tmp/trait_dispatch_missing.out"

if "$compiler" preprocess "$program_dir/trait_dispatch_wrong_failure.n00b" \
    >"$tmp/trait_dispatch_wrong_failure.out" 2>&1; then
    cat "$tmp/trait_dispatch_wrong_failure.out"
    echo 'trait method with a mismatched failure type unexpectedly passed' >&2
    exit 1
fi
grep -q 'N00BB-T0003' "$tmp/trait_dispatch_wrong_failure.out"

if "$compiler" preprocess "$program_dir/trait_dispatch_bad_argument.n00b" \
    >"$tmp/trait_dispatch_bad_argument.out" 2>&1; then
    cat "$tmp/trait_dispatch_bad_argument.out"
    echo 'trait method with a mismatched argument unexpectedly passed' >&2
    exit 1
fi
grep -q 'N00BB-T0003' "$tmp/trait_dispatch_bad_argument.out"

"$compiler" preprocess --dump-link-unit \
    "$program_dir/trait_dispatch_valid_argument.n00b" \
    >"$tmp/trait_dispatch_valid_argument.out" 2>&1 || {
    cat "$tmp/trait_dispatch_valid_argument.out"
    exit 1
}
grep -q 'trait-dispatch\[resolved\] .*valid_argument -> .*combine' \
    "$tmp/trait_dispatch_valid_argument.out"

if "$compiler" preprocess "$program_dir/direct_call_is_not_trait_dispatch.n00b" \
    >"$tmp/direct_call_is_not_trait_dispatch.out" 2>&1; then
    cat "$tmp/direct_call_is_not_trait_dispatch.out"
    echo 'ordinary function call unexpectedly used trait dispatch' >&2
    exit 1
fi
grep -q 'N00BB-T0002' "$tmp/direct_call_is_not_trait_dispatch.out"
! grep -q 'N00BB-T0003' "$tmp/direct_call_is_not_trait_dispatch.out"

"$compiler" preprocess "$program_dir/trait_dispatch_receiver_offset.n00b" \
    >"$tmp/trait_dispatch_receiver_offset.out" 2>&1 || {
    cat "$tmp/trait_dispatch_receiver_offset.out"
    exit 1
}

N00B_PATH="$program_dir" "$compiler" preprocess "$program_dir/render_consumer.n00b" \
    >"$tmp/render_graph.out" 2>&1 || {
    cat "$tmp/render_graph.out"
    exit 1
}

N00B_PATH="$program_dir" "$compiler" preprocess --dump-symbols \
    "$program_dir/generic_render_consumer.n00b" \
    >"$tmp/generic_render_consumer.out" 2>&1 || {
    cat "$tmp/generic_render_consumer.out"
    exit 1
}
grep 'generic_render: function' "$tmp/generic_render_consumer.out" \
    | grep -q 'render_contract.contract.Render' || {
    cat "$tmp/generic_render_consumer.out"
    echo 'imported trait constraint did not resolve to its defining module' >&2
    exit 1
}
grep -q 'call_generic_render: function, mutable, type=() -> string' \
    "$tmp/generic_render_consumer.out"

N00B_PATH="$program_dir" "$compiler" lower --dump-ir --dump-link-unit \
    "$program_dir/link_render_consumer.n00b" \
    >"$tmp/link_render_consumer.out" 2>&1 || {
    cat "$tmp/link_render_consumer.out"
    exit 1
}
grep -q 'call @render, %item' "$tmp/link_render_consumer.out"
grep -q 'trait-dispatch\[resolved\] link_render_consumer:1:use_plugin -> link_render_contract.plugin:1:render' \
    "$tmp/link_render_consumer.out"
grep -q 'link_render_contract.contract:2:Render kind=' \
    "$tmp/link_render_consumer.out"

N00B_PATH="$program_dir" "$compiler" lower --dump-ir --dump-link-unit \
    "$program_dir/link_global_user.n00b" >"$tmp/link_global_user.out" \
    2>"$tmp/link_global_user.err" || {
    cat "$tmp/link_global_user.out"
    cat "$tmp/link_global_user.err"
    exit 1
}
grep -q 'copy @link_global_dep.impl.remote_global' "$tmp/link_global_user.out"
grep -q 'resolved link_global_user -> link_global_dep.impl:0:remote_global' \
    "$tmp/link_global_user.out" || {
    cat "$tmp/link_global_user.out"
    exit 1
}
grep -q 'binding n00bb.ir.global' "$tmp/link_global_user.out"

N00B_PATH="$program_dir" "$compiler" lower --dump-ir --dump-link-unit \
    "$program_dir/link_global_inferred_user.n00b" \
    >"$tmp/link_global_inferred_user.out" 2>&1 || {
    cat "$tmp/link_global_inferred_user.out"
    exit 1
}
grep -q 'copy @link_global_inferred_dep.impl.remote_global' \
    "$tmp/link_global_inferred_user.out"
grep -q 'resolved link_global_inferred_user -> link_global_inferred_dep.impl:0:remote_global' \
    "$tmp/link_global_inferred_user.out"
grep -q 'binding n00bb.ir.global' "$tmp/link_global_inferred_user.out"
grep -q 'initializer-use\[resolved\] link_global_inferred_dep.impl:0:remote_global -> link_global_dep.impl:0:remote_global' \
    "$tmp/link_global_inferred_user.out"

N00B_PATH="$interface_programs" "$compiler" library \
    --emit-interface "$tmp/global_archive.n00bi" global_archive_dep.facade \
    >"$tmp/global_archive_library.out" 2>"$tmp/global_archive_library.err" || {
    cat "$tmp/global_archive_library.err"
    exit 1
}

N00B_PATH="$interface_programs" "$compiler" library \
    --emit-interface "$tmp/array_archive.n00bi" array_archive_dep.facade \
    >"$tmp/array_archive_library.out" \
    2>"$tmp/array_archive_library.err" || {
    cat "$tmp/array_archive_library.err"
    exit 1
}

N00B_PATH="$interface_programs" "$compiler" library \
    --emit-interface "$tmp/array_shape_archive.n00bi" \
    array_shape_archive_dep.facade \
    >"$tmp/array_shape_archive_library.out" \
    2>"$tmp/array_shape_archive_library.err" || {
    cat "$tmp/array_shape_archive_library.err"
    exit 1
}
strings "$tmp/array_shape_archive.n00bi" | grep -q 'n00b.array-shape'
strings "$tmp/array_shape_archive.n00bi" | grep -q 'n00b.comptime-ir'
strings "$tmp/array_shape_archive.n00bi" | grep -q \
    'n00b.function-array-shapes'
strings "$tmp/array_shape_archive.n00bi" | grep -q \
    'array_shape_archive_dep.renamed_echo'
strings "$tmp/array_shape_archive.n00bi" | grep -q \
    'n00b.member-array-shape.cells'
strings "$tmp/array_shape_archive.n00bi" | grep -Fq \
    'module array_shape_archive_dep.impl$comptime {'
! strings "$tmp/array_shape_archive.n00bi" | grep -Fq \
    'module array_shape_archive_dep {'
"$compiler" lower --link-interface "$tmp/array_shape_archive.n00bi" \
    --dump-symbols --dump-link-unit \
    "$interface_programs/array_shape_archive_user.n00b" \
    >"$tmp/array_shape_archive_user.out" \
    2>"$tmp/array_shape_archive_user.err" || {
    cat "$tmp/array_shape_archive_user.err"
    cat "$tmp/array_shape_archive_user.out"
    exit 1
}
grep -q 'array_shape_archive_dep.impl:0:grid .*array-shape\[rank=1,flat-prefix=1,dim0=comptime:producer=__n00bb_array_extent_' \
    "$tmp/array_shape_archive_user.out"
grep -q 'sequence.shape.check.*dim0=comptime:producer=__n00bb_array_extent_' \
    "$tmp/array_shape_archive_user.out"
grep -q 'renamed_echo: function, mutable, type=(Array\[int\]) -> Array\[int\] fail: nil, visibility=public.*phases=comptime+runtime' \
    "$tmp/array_shape_archive_user.out"

N00B_PATH="$interface_programs" "$compiler" library \
    --emit-interface "$tmp/forward_alias_archive.n00bi" \
    forward_alias_archive_dep.facade \
    >"$tmp/forward_alias_archive_library.out" \
    2>"$tmp/forward_alias_archive_library.err" || {
    cat "$tmp/forward_alias_archive_library.err"
    exit 1
}
"$compiler" lower --link-interface "$tmp/forward_alias_archive.n00bi" \
    --dump-symbols "$interface_programs/forward_alias_archive_user.n00b" \
    >"$tmp/forward_alias_archive_user.out" \
    2>"$tmp/forward_alias_archive_user.err" || {
    cat "$tmp/forward_alias_archive_user.err"
    cat "$tmp/forward_alias_archive_user.out"
    exit 1
}
grep -q 'Forward: type, .*type=int' "$tmp/forward_alias_archive_user.out"
grep -q 'Concrete: type, .*type=int' "$tmp/forward_alias_archive_user.out"
grep -q 'call @forward_alias' \
    "$tmp/forward_alias_archive_user.out"

N00B_PATH="$interface_programs" "$compiler" library \
    --emit-interface "$tmp/array_alias_archive.n00bi" \
    array_alias_archive_dep.facade \
    >"$tmp/array_alias_archive_library.out" \
    2>"$tmp/array_alias_archive_library.err" || {
    cat "$tmp/array_alias_archive_library.err"
    exit 1
}
strings "$tmp/array_alias_archive.n00bi" | grep -q 'n00b.array-shape'
strings "$tmp/array_alias_archive.n00bi" | grep -q 'n00b.function-array-shapes'
"$compiler" preprocess --dump-symbols \
    "$interface_programs/array_alias_archive_dep/impl.n00b" \
    >"$tmp/array_alias_archive_source.out" 2>&1 || {
    cat "$tmp/array_alias_archive_source.out"
    exit 1
}
grep -q 'FixedRow: type, .*type=Array\[int\], array-shape=rank:1, dim0=comptime' \
    "$tmp/array_alias_archive_source.out"
grep -q 'input: parameter, .*type=Array\[int\], array-shape=rank:1, dim0=comptime' \
    "$tmp/array_alias_archive_source.out"
"$compiler" lower --link-interface "$tmp/array_alias_archive.n00bi" \
    --dump-link-unit --dump-symbols \
    "$interface_programs/array_alias_archive_user.n00b" \
    >"$tmp/array_alias_archive_user.out" \
    2>"$tmp/array_alias_archive_user.err" || {
    cat "$tmp/array_alias_archive_user.err"
    cat "$tmp/array_alias_archive_user.out"
    exit 1
}
grep -q 'FixedRow: type, .*type=Array\[int\], array-shape=rank:1, dim0=comptime(producer=__n00bb_array_extent_' \
    "$tmp/array_alias_archive_user.out"
grep -q 'array_alias_archive_dep.impl:2:FixedRow kind=2 binding n00bb.semantic.type binding ir.array-shape' \
    "$tmp/array_alias_archive_user.out"
grep -q 'sequence.shape.check.*dim0=comptime:producer=__n00bb_array_extent_' \
    "$tmp/array_alias_archive_user.out"

if "$compiler" lower --dump-symbols \
    "$program_dir/cyclic_type_alias.n00b" \
    >"$tmp/cyclic_type_alias.out" 2>&1; then
    echo 'cyclic transparent type aliases unexpectedly lowered' >&2
    exit 1
fi
grep -q 'error\[N00BB-T0012\].*cyclic type alias definition' \
    "$tmp/cyclic_type_alias.out"
[ "$(grep -c 'error\[N00BB-T0012\]' "$tmp/cyclic_type_alias.out")" -eq 1 ]
[ "$(grep -c 'fatal\[N00BB-L0006\]' "$tmp/cyclic_type_alias.out")" -eq 1 ]

if N00B_PATH="$program_dir" "$compiler" lower \
    cyclic_alias_import.facade \
    >"$tmp/cyclic_alias_import.out" 2>&1; then
    echo 'cyclic imported transparent aliases unexpectedly lowered' >&2
    exit 1
fi
grep -q 'error\[N00BB-T0012\].*cyclic type alias definition' \
    "$tmp/cyclic_alias_import.out"

N00B_PATH="$interface_programs" "$compiler" library --emit-interface \
    "$tmp/array_shape_chain_dep.n00bi" \
    array_shape_chain_dep.facade \
    >"$tmp/array_shape_chain_dep.out" \
    2>"$tmp/array_shape_chain_dep.err" || {
    cat "$tmp/array_shape_chain_dep.err"
    exit 1
}
N00B_PATH="$interface_programs" "$compiler" library \
    --link-interface "$tmp/array_shape_chain_dep.n00bi" \
    --emit-interface "$tmp/array_shape_chain_middle.n00bi" \
    --dump-link-unit \
    array_shape_chain_middle.facade \
    >"$tmp/array_shape_chain_middle.out" \
    2>"$tmp/array_shape_chain_middle.err" || {
    cat "$tmp/array_shape_chain_middle.err"
    exit 1
}
strings "$tmp/array_shape_chain_middle.n00bi" | \
    grep -q 'array_shape_chain_dep.extent'
strings "$tmp/array_shape_chain_middle.n00bi" | \
    grep -q 'n00b.comptime-producer.__n00bb_array_extent_'
strings "$tmp/array_shape_chain_middle.n00bi" | \
    grep -q 'n00b.comptime-dependencies.__n00bb_array_extent_'
grep -q 'call\[resolved\] array_shape_chain_middle.impl:.* -> array_shape_chain_dep.impl:.*:extent' \
    "$tmp/array_shape_chain_middle.out"
array_shape_dependency_offset=$(grep -abo 'N0CD' \
    "$tmp/array_shape_chain_middle.n00bi" | head -1 | cut -d: -f1)
if [ -z "$array_shape_dependency_offset" ]; then
    echo 'could not locate archived comptime dependency closure' >&2
    exit 1
fi
cp "$tmp/array_shape_chain_middle.n00bi" \
    "$tmp/array_shape_chain_missing_dependency.n00bi"
printf 'X' | dd \
    of="$tmp/array_shape_chain_missing_dependency.n00bi" bs=1 \
    seek="$((array_shape_dependency_offset + 20))" conv=notrunc \
    2>/dev/null
if "$compiler" lower \
    --link-interface "$tmp/array_shape_chain_missing_dependency.n00bi" \
    "$interface_programs/array_shape_chain_user.n00b" \
    >"$tmp/array_shape_chain_missing_dependency.out" \
    2>"$tmp/array_shape_chain_missing_dependency.err"; then
    echo 'archive with a missing comptime dependency unexpectedly loaded' >&2
    exit 1
fi
grep -q 'fatal\[N00BB-L0046\]' \
    "$tmp/array_shape_chain_missing_dependency.err"
grep -q 'array_shape_chain_middle.values' \
    "$tmp/array_shape_chain_missing_dependency.err"
"$compiler" lower \
    --link-interface "$tmp/array_shape_chain_middle.n00bi" \
    --dump-symbols --dump-link-unit \
    "$interface_programs/array_shape_chain_user.n00b" \
    >"$tmp/array_shape_chain_user.out" \
    2>"$tmp/array_shape_chain_user.err" || {
    cat "$tmp/array_shape_chain_user.err"
    cat "$tmp/array_shape_chain_user.out"
    exit 1
}
grep -q 'values: variable, .*array-shape=rank:1, dim0=comptime' \
    "$tmp/array_shape_chain_user.out"
grep -q 'resolved array_shape_chain_user -> array_shape_chain_middle.impl:1:echo' \
    "$tmp/array_shape_chain_user.out"

array_shape_record_index=0
array_shape_function_metadata_offset=
array_shape_owner_errors="$tmp/array_shape_owner_errors.err"
: >"$array_shape_owner_errors"
for array_shape_metadata_offset in $(grep -abo 'N0AS' \
        "$tmp/array_shape_archive.n00bi" | cut -d: -f1); do
    array_shape_record_index=$((array_shape_record_index + 1))
    array_shape_label="array_shape_owner_$array_shape_record_index"
    array_shape_corrupted="$tmp/$array_shape_label.n00bi"
    cp "$tmp/array_shape_archive.n00bi" "$array_shape_corrupted"
    printf 'X' | dd of="$array_shape_corrupted" bs=1 \
        seek="$array_shape_metadata_offset" conv=notrunc 2>/dev/null
    if "$compiler" lower --link-interface "$array_shape_corrupted" \
        "$interface_programs/array_shape_archive_user.n00b" \
        >"$tmp/$array_shape_label.out" 2>"$tmp/$array_shape_label.err"; then
        continue
    fi
    grep -q 'fatal\[N00BB-L0046\]' "$tmp/$array_shape_label.err"
    grep -Eq 'array_shape_archive_dep\.(grid|echo|renamed_echo)' \
        "$tmp/$array_shape_label.err"
    cat "$tmp/$array_shape_label.err" >>"$array_shape_owner_errors"
    if [ -z "$array_shape_function_metadata_offset" ] && \
        grep -q 'interface function has invalid array-shape metadata' \
            "$tmp/$array_shape_label.err"; then
        array_shape_function_metadata_offset=$array_shape_metadata_offset
    fi
done
grep -q 'array_shape_archive_dep.grid' "$array_shape_owner_errors"
grep -q 'array_shape_archive_dep.echo' "$array_shape_owner_errors"
grep -q 'array_shape_archive_dep.renamed_echo' "$array_shape_owner_errors"

array_alias_owner_errors="$tmp/array_alias_owner_errors.err"
: >"$array_alias_owner_errors"
array_alias_record_index=0
array_alias_export_record_count=0
for array_alias_metadata_offset in $(grep -abo 'N0AS' \
        "$tmp/array_alias_archive.n00bi" | cut -d: -f1); do
    array_alias_record_index=$((array_alias_record_index + 1))
    array_alias_label="array_alias_owner_$array_alias_record_index"
    array_alias_corrupted="$tmp/$array_alias_label.n00bi"
    cp "$tmp/array_alias_archive.n00bi" "$array_alias_corrupted"
    printf 'X' | dd of="$array_alias_corrupted" bs=1 \
        seek="$array_alias_metadata_offset" conv=notrunc 2>/dev/null
    if "$compiler" lower --link-interface "$array_alias_corrupted" \
        "$interface_programs/array_alias_archive_user.n00b" \
        >"$tmp/$array_alias_label.out" 2>"$tmp/$array_alias_label.err"; then
        continue
    fi
    grep -q 'fatal\[N00BB-L0046\]' "$tmp/$array_alias_label.err"
    grep -Eq 'array_alias_archive_dep\.(FixedRowBase|FixedRow|echo_row)' \
        "$tmp/$array_alias_label.err"
    cat "$tmp/$array_alias_label.err" >>"$array_alias_owner_errors"
    array_alias_export_record_count=$((array_alias_export_record_count + 1))
done
[ "$array_alias_export_record_count" -eq 4 ]
grep -q 'array_alias_archive_dep.FixedRow' "$array_alias_owner_errors"
grep -q 'array_alias_archive_dep.FixedRowBase' "$array_alias_owner_errors"
grep -q 'array_alias_archive_dep.echo_row' "$array_alias_owner_errors"

array_shape_metadata_offset=$array_shape_function_metadata_offset
shape_owner_pattern='array_shape_archive_dep\.(grid|GridHolder\.cells|echo|renamed_echo)'
reject_shape_record "$tmp/array_shape_archive.n00bi" \
    array_shape_bad_rank "$array_shape_metadata_offset" 4 \
    '\002\000\000\000' \
    "$interface_programs/array_shape_archive_user.n00b" \
    "$shape_owner_pattern"
reject_shape_record "$tmp/array_shape_archive.n00bi" \
    array_shape_bad_flat_prefix "$array_shape_metadata_offset" 8 \
    '\000\000\000\000' \
    "$interface_programs/array_shape_archive_user.n00b" \
    "$shape_owner_pattern"
reject_shape_record "$tmp/array_shape_archive.n00bi" \
    array_shape_invalid_extent_kind "$array_shape_metadata_offset" 16 \
    '\004\000\000\000' \
    "$interface_programs/array_shape_archive_user.n00b" \
    "$shape_owner_pattern"
reject_shape_record "$tmp/array_shape_archive.n00bi" \
    array_shape_overflowed_name_length "$array_shape_metadata_offset" 28 \
    '\377\377\377\377' \
    "$interface_programs/array_shape_archive_user.n00b" \
    "$shape_owner_pattern"

cp "$tmp/array_shape_archive.n00bi" \
    "$tmp/array_shape_archive_bad_shape.n00bi"
array_shape_metadata_offset=$array_shape_function_metadata_offset
if [ -z "$array_shape_metadata_offset" ]; then
    echo 'could not locate array-shape metadata in test archive' >&2
    exit 1
fi
printf 'X' | dd of="$tmp/array_shape_archive_bad_shape.n00bi" bs=1 \
    seek="$array_shape_metadata_offset" conv=notrunc 2>/dev/null
if "$compiler" lower \
    --link-interface "$tmp/array_shape_archive_bad_shape.n00bi" \
    "$interface_programs/array_shape_archive_user.n00b" \
    >"$tmp/array_shape_archive_bad_shape.out" \
    2>"$tmp/array_shape_archive_bad_shape.err"; then
    echo 'malformed array-shape interface metadata unexpectedly loaded' >&2
    exit 1
fi
grep -q 'fatal\[N00BB-L0046\].*invalid array-shape metadata' \
    "$tmp/array_shape_archive_bad_shape.err"
grep -Eq 'invalid array-shape metadata: .*(grid|GridHolder\.cells|echo|renamed_echo)' \
    "$tmp/array_shape_archive_bad_shape.err"

cp "$tmp/array_shape_archive.n00bi" \
    "$tmp/array_shape_archive_missing_producer.n00bi"
array_shape_selected_producer_offset=
for array_shape_producer_offset in $(grep -abo '__n00bb_array_extent_' \
        "$tmp/array_shape_archive.n00bi" | cut -d: -f1); do
    cp "$tmp/array_shape_archive.n00bi" \
        "$tmp/array_shape_archive_missing_producer.n00bi"
    printf 'X' | dd of="$tmp/array_shape_archive_missing_producer.n00bi" \
        bs=1 seek="$array_shape_producer_offset" conv=notrunc 2>/dev/null
    if "$compiler" lower \
        --link-interface "$tmp/array_shape_archive_missing_producer.n00bi" \
        "$interface_programs/array_shape_archive_user.n00b" \
        >"$tmp/array_shape_archive_missing_producer.out" \
        2>"$tmp/array_shape_archive_missing_producer.err"; then
        continue
    fi
    array_shape_selected_producer_offset=$array_shape_producer_offset
    break
done
if [ -z "$array_shape_selected_producer_offset" ]; then
    echo 'could not locate array-shape producer identifier in test archive' >&2
    exit 1
fi
grep -Eq 'fatal\[N00BB-L0046\].*(invalid or missing n00b semantics|invalid array-shape metadata)' \
    "$tmp/array_shape_archive_missing_producer.err"
grep -Eq "$shape_owner_pattern" \
    "$tmp/array_shape_archive_missing_producer.err"

cp "$tmp/array_shape_archive.n00bi" \
    "$tmp/array_shape_archive_contradictory_extent.n00bi"
array_shape_metadata_offset=$array_shape_function_metadata_offset
if [ -z "$array_shape_metadata_offset" ]; then
    echo 'could not locate array-shape metadata in test archive' >&2
    exit 1
fi
array_shape_extent_value_offset=$((array_shape_metadata_offset + 20))
printf '\001' | dd \
    of="$tmp/array_shape_archive_contradictory_extent.n00bi" bs=1 \
    seek="$array_shape_extent_value_offset" conv=notrunc 2>/dev/null
if "$compiler" lower \
    --link-interface "$tmp/array_shape_archive_contradictory_extent.n00bi" \
    "$interface_programs/array_shape_archive_user.n00b" \
    >"$tmp/array_shape_archive_contradictory_extent.out" \
    2>"$tmp/array_shape_archive_contradictory_extent.err"; then
    echo 'contradictory comptime extent metadata unexpectedly loaded' >&2
    exit 1
fi
grep -q 'fatal\[N00BB-L0046\].*invalid array-shape metadata' \
    "$tmp/array_shape_archive_contradictory_extent.err"
grep -Eq 'invalid array-shape metadata: .*(grid|GridHolder\.cells|echo|renamed_echo)' \
    "$tmp/array_shape_archive_contradictory_extent.err"

N00B_PATH="$interface_programs" "$compiler" library \
    --emit-interface "$tmp/static_sequence_archive.n00bi" \
    static_sequence_archive_dep.facade \
    >"$tmp/static_sequence_archive_library.out" \
    2>"$tmp/static_sequence_archive_library.err" || {
    cat "$tmp/static_sequence_archive_library.err"
    exit 1
}
static_rank_two_shape_offset=
for static_shape_offset in $(grep -abo 'N0AS' \
        "$tmp/static_sequence_archive.n00bi" | cut -d: -f1); do
    static_shape_rank=$(od -An -tu4 -j "$((static_shape_offset + 4))" \
        -N4 "$tmp/static_sequence_archive.n00bi" | tr -d ' ')
    if [ "$static_shape_rank" -ne 2 ]; then
        continue
    fi
    cp "$tmp/static_sequence_archive.n00bi" \
        "$tmp/static_shape_zero_extent.n00bi"
    printf '\001\000\000\000' | dd \
        of="$tmp/static_shape_zero_extent.n00bi" bs=1 \
        seek="$((static_shape_offset + 8))" conv=notrunc 2>/dev/null
    printf '\001\000\000\000' | dd \
        of="$tmp/static_shape_zero_extent.n00bi" bs=1 \
        seek="$((static_shape_offset + 16))" conv=notrunc 2>/dev/null
    if "$compiler" lower \
        --link-interface "$tmp/static_shape_zero_extent.n00bi" \
        "$interface_programs/static_sequence_archive_user.n00b" \
        >"$tmp/static_shape_zero_extent.out" \
        2>"$tmp/static_shape_zero_extent.err"; then
        continue
    fi
    static_rank_two_shape_offset=$static_shape_offset
    break
done
if [ -z "$static_rank_two_shape_offset" ]; then
    echo 'could not locate rank-two array-shape metadata in test archive' >&2
    exit 1
fi
grep -q 'fatal\[N00BB-L0046\]' "$tmp/static_shape_zero_extent.err"
grep -q 'static_sequence_archive_dep.nested' \
    "$tmp/static_shape_zero_extent.err"
cp "$tmp/static_sequence_archive.n00bi" \
    "$tmp/static_shape_dimension_order.n00bi"
printf '\001\000\000\000\003\000\000\000' | dd \
    of="$tmp/static_shape_dimension_order.n00bi" bs=1 \
    seek="$((static_rank_two_shape_offset + 32))" conv=notrunc 2>/dev/null
if "$compiler" lower \
    --link-interface "$tmp/static_shape_dimension_order.n00bi" \
    "$interface_programs/static_sequence_archive_user.n00b" \
    >"$tmp/static_shape_dimension_order.out" \
    2>"$tmp/static_shape_dimension_order.err"; then
    echo 'out-of-order array-shape dimensions unexpectedly loaded' >&2
    exit 1
fi
grep -q 'fatal\[N00BB-L0046\]' \
    "$tmp/static_shape_dimension_order.err"
grep -q 'static_sequence_archive_dep.nested' \
    "$tmp/static_shape_dimension_order.err"
"$compiler" preprocess --link-interface "$tmp/static_sequence_archive.n00bi" \
    --dump-link-unit "$interface_programs/static_sequence_archive_user.n00b" \
    >"$tmp/static_sequence_archive_user.out" \
    2>"$tmp/static_sequence_archive_user.err" || {
    cat "$tmp/static_sequence_archive_user.err"
    cat "$tmp/static_sequence_archive_user.out"
    exit 1
}
grep -q 'static_sequence_archive_dep.impl:0:values .*static-value=sequence:\[00000000000000000000000000000001,00000000000000000000000000000002\]' \
    "$tmp/static_sequence_archive_user.out"
grep -q 'static_sequence_archive_dep.impl:0:nested .*static-value=sequence:\[\[00000000000000000000000000000001,00000000000000000000000000000002\],\[00000000000000000000000000000003,00000000000000000000000000000004\]\]' \
    "$tmp/static_sequence_archive_user.out"
grep -q 'static_sequence_archive_dep.impl:0:list_values .*static-value=sequence:\[00000000000000000000000000000005,00000000000000000000000000000006\]' \
    "$tmp/static_sequence_archive_user.out"

N00B_PATH="$interface_programs" "$compiler" library \
    --emit-interface "$tmp/computed_const_archive.n00bi" \
    computed_const_archive_dep.facade \
    >"$tmp/computed_const_archive_library.out" \
    2>"$tmp/computed_const_archive_library.err" || {
    cat "$tmp/computed_const_archive_library.err"
    exit 1
}
"$compiler" lower --link-interface "$tmp/computed_const_archive.n00bi" \
    --dump-ir --dump-link-unit \
    "$interface_programs/computed_const_archive_user.n00b" \
    >"$tmp/computed_const_archive_user.out" \
    2>"$tmp/computed_const_archive_user.err" || {
    cat "$tmp/computed_const_archive_user.err"
    cat "$tmp/computed_const_archive_user.out"
    exit 1
}
grep -q 'extern const global @computed_const_archive_dep.values: array\[int\]' \
    "$tmp/computed_const_archive_user.out"
grep -q 'computed_const_archive_dep.impl:0:values kind=.*facts\[dimensions=1,' \
    "$tmp/computed_const_archive_user.out"
! grep -q 'computed_const_archive_dep.impl:0:values .*static-value=' \
    "$tmp/computed_const_archive_user.out"

N00B_PATH="$interface_programs" "$compiler" library \
    --emit-interface "$tmp/static_aggregate_archive.n00bi" \
    static_aggregate_archive_dep.facade \
    >"$tmp/static_aggregate_archive_library.out" \
    2>"$tmp/static_aggregate_archive_library.err" || {
    cat "$tmp/static_aggregate_archive_library.err"
    exit 1
}
"$compiler" preprocess \
    --link-interface "$tmp/static_aggregate_archive.n00bi" \
    --dump-link-unit \
    "$interface_programs/static_aggregate_archive_user.n00b" \
    >"$tmp/static_aggregate_archive_user.out" \
    2>"$tmp/static_aggregate_archive_user.err" || {
    cat "$tmp/static_aggregate_archive_user.err"
    cat "$tmp/static_aggregate_archive_user.out"
    exit 1
}
grep -q 'static_aggregate_archive_dep.impl:0:point .*static-value=record:\[00000000000000000000000000000007,5 bytes\]' \
    "$tmp/static_aggregate_archive_user.out"
grep -q 'static_aggregate_archive_dep.impl:0:anonymous .*static-value=record:\[1 bytes,00000000000000000000000000000008\]' \
    "$tmp/static_aggregate_archive_user.out"
grep -q 'static_aggregate_archive_dep.impl:0:pair .*static-value=tuple:\[00000000000000000000000000000009,5 bytes\]' \
    "$tmp/static_aggregate_archive_user.out"
grep -q 'static_aggregate_archive_dep.impl:0:lookup .*static-value=dictionary:\[3 bytes,00000000000000000000000000000001,3 bytes,00000000000000000000000000000002\]' \
    "$tmp/static_aggregate_archive_user.out"
grep -q 'static_aggregate_archive_dep.impl:0:flags .*static-value=set:\[00000000000000000000000000000001,00000000000000000000000000000002\]' \
    "$tmp/static_aggregate_archive_user.out"
grep -q 'static_aggregate_archive_dep.impl:0:empty_lookup .*static-value=dictionary:\[\]' \
    "$tmp/static_aggregate_archive_user.out"
grep -q 'static_aggregate_archive_dep.impl:0:empty_flags .*static-value=set:\[\]' \
    "$tmp/static_aggregate_archive_user.out"
grep -q 'static_aggregate_archive_dep.impl:0:point .*site-provenance\[library\]=<interface:static_aggregate_archive_dep>' \
    "$tmp/static_aggregate_archive_user.out"

"$compiler" preprocess --dump-link-unit \
    "$interface_programs/static_aggregate_archive_dep/impl.n00b" \
    >"$tmp/static_aggregate_archive_source.out" \
    2>"$tmp/static_aggregate_archive_source.err" || {
    cat "$tmp/static_aggregate_archive_source.err"
    cat "$tmp/static_aggregate_archive_source.out"
    exit 1
}
grep -q 'impl:0:point .*site-provenance\[source\]=.*static_aggregate_archive_dep/impl.n00b' \
    "$tmp/static_aggregate_archive_source.out"

"$compiler" lower --jobs 1 --dump-ir \
    "$interface_programs/static_aggregate_archive_dep/impl.n00b" \
    >"$tmp/static_aggregate_archive_lower.ir" \
    2>"$tmp/static_aggregate_archive_lower.err" || {
    cat "$tmp/static_aggregate_archive_lower.err"
    cat "$tmp/static_aggregate_archive_lower.ir"
    exit 1
}
grep -Fq 'module impl$comptime {' \
    "$tmp/static_aggregate_archive_lower.ir"
grep -Fq 'module impl {' \
    "$tmp/static_aggregate_archive_lower.ir"
grep -Fq 'empty_flags: set[int] = set{}' \
    "$tmp/static_aggregate_archive_lower.ir"
grep -Fq 'empty_lookup: dict[string, int] = dict{}' \
    "$tmp/static_aggregate_archive_lower.ir"
test "$(grep -Fc 'point: impl.StaticPoint = { x = 7, label = "point" }' \
    "$tmp/static_aggregate_archive_lower.ir")" -eq 2

static_selected_global_offset=
for static_global_offset in $(grep -abo 'n00b.global' \
        "$tmp/static_aggregate_archive.n00bi" | cut -d: -f1); do
    cp "$tmp/static_aggregate_archive.n00bi" \
        "$tmp/static_aggregate_archive_bad_value.n00bi"
    static_value_offset=$((static_global_offset + 35))
    printf '\377' | dd \
        of="$tmp/static_aggregate_archive_bad_value.n00bi" \
        bs=1 seek="$static_value_offset" conv=notrunc 2>/dev/null
    if "$compiler" preprocess \
        --link-interface "$tmp/static_aggregate_archive_bad_value.n00bi" \
        --dump-link-unit \
        "$interface_programs/static_aggregate_archive_user.n00b" \
        >"$tmp/static_aggregate_archive_bad_value.out" \
        2>"$tmp/static_aggregate_archive_bad_value.err"; then
        continue
    fi
    static_selected_global_offset=$static_global_offset
    break
done
if [ -z "$static_selected_global_offset" ]; then
    echo 'could not locate recursive global metadata in test archive' >&2
    exit 1
fi
grep -q 'fatal\[N00BB-L0046\].*interface value has invalid or missing n00b semantics' \
    "$tmp/static_aggregate_archive_bad_value.err"
! grep -q '^link-unit:' "$tmp/static_aggregate_archive_bad_value.out"
! grep -q '^module static_aggregate_archive_user' \
    "$tmp/static_aggregate_archive_bad_value.out"

N00B_PATH="$interface_programs" "$compiler" library \
    --emit-interface "$tmp/static_mismatch_archive.n00bi" \
    static_mismatch_archive_dep.facade \
    >"$tmp/static_mismatch_archive_library.out" \
    2>"$tmp/static_mismatch_archive_library.err" || {
    cat "$tmp/static_mismatch_archive_library.err"
    exit 1
}
static_mismatch_selected_offset=
for static_mismatch_global_offset in $(grep -abo 'n00b.global' \
        "$tmp/static_mismatch_archive.n00bi" | cut -d: -f1); do
    cp "$tmp/static_mismatch_archive.n00bi" \
        "$tmp/static_mismatch_archive_bad_type.n00bi"
    printf '\000\000' | dd \
        of="$tmp/static_mismatch_archive_bad_type.n00bi" \
        bs=1 seek="$((static_mismatch_global_offset + 34))" \
        conv=notrunc 2>/dev/null
    if "$compiler" preprocess \
        --link-interface "$tmp/static_mismatch_archive_bad_type.n00bi" \
        --dump-link-unit --dump-ir \
        "$interface_programs/static_mismatch_archive_user.n00b" \
        >"$tmp/static_mismatch_archive_bad_type.out" \
        2>"$tmp/static_mismatch_archive_bad_type.err"; then
        continue
    fi
    static_mismatch_selected_offset=$static_mismatch_global_offset
    break
done
if [ -z "$static_mismatch_selected_offset" ]; then
    echo 'could not locate typed static global metadata in test archive' >&2
    exit 1
fi
grep -q 'fatal\[N00BB-L0046\].*interface value has invalid or missing n00b semantics' \
    "$tmp/static_mismatch_archive_bad_type.err"
! grep -q '^link-unit:' "$tmp/static_mismatch_archive_bad_type.out"
! grep -q '^module static_mismatch_archive_user' \
    "$tmp/static_mismatch_archive_bad_type.out"

archive_size=$(wc -c < "$tmp/static_aggregate_archive.n00bi")
truncated_archive_size=$((archive_size - 1))
dd if="$tmp/static_aggregate_archive.n00bi" \
    of="$tmp/static_aggregate_archive_truncated.n00bi" \
    bs=1 count="$truncated_archive_size" 2>/dev/null
if "$compiler" preprocess \
    --link-interface "$tmp/static_aggregate_archive_truncated.n00bi" \
    "$interface_programs/static_aggregate_archive_user.n00b" \
    >"$tmp/static_aggregate_archive_truncated.out" \
    2>"$tmp/static_aggregate_archive_truncated.err"; then
    echo 'truncated semantic interface unexpectedly loaded' >&2
    exit 1
fi
grep -q 'error\[N00BB-B0004\].*malformed object archive' \
    "$tmp/static_aggregate_archive_truncated.err"

"$compiler" lower --link-interface "$tmp/array_archive.n00bi" \
    --dump-link-unit "$interface_programs/array_archive_user.n00b" \
    >"$tmp/array_archive_user.out" \
    2>"$tmp/array_archive_user.err" || {
    cat "$tmp/array_archive_user.err"
    cat "$tmp/array_archive_user.out"
    exit 1
}
grep -q 'n00bb.array-shapes v1 complete' \
    "$tmp/array_archive_user.out"
grep -q 'array_archive_dep.impl:0:grid .*facts\[dimensions=2' \
    "$tmp/array_archive_user.out"

"$compiler" preprocess --dump-link-unit \
    "$program_dir/incomplete_storage_lifetime.n00b" \
    >"$tmp/incomplete_storage_lifetime.out"
grep -q 'n00bb.storage-lifetimes v1 unavailable' \
    "$tmp/incomplete_storage_lifetime.out"

"$compiler" lower --link-interface "$tmp/global_archive.n00bi" \
    --dump-ir --dump-link-unit \
    "$interface_programs/global_archive_user.n00b" \
    >"$tmp/global_archive_user.out" 2>"$tmp/global_archive_user.err" || {
    cat "$tmp/global_archive_user.err"
    exit 1
}
grep -q 'n00bb.storage-lifetimes v1 complete' \
    "$tmp/global_archive_user.out"
grep -q 'global_archive_dep.remote_global' "$tmp/global_archive_user.out"
grep -q '@global_archive_dep.worker_count: int .*n00b.thread_local(true)' \
    "$tmp/global_archive_user.out"
grep -q '@global_archive_dep.worker_ratio: f64 .*n00b.thread_local(true)' \
    "$tmp/global_archive_user.out"
grep -q '@global_archive_dep.worker_label: string .*n00b.thread_local(true)' \
    "$tmp/global_archive_user.out"
grep -q '@global_archive_dep.empty_worker_label: string .*n00b.thread_local(true)' \
    "$tmp/global_archive_user.out"
grep -q 'store' "$tmp/global_archive_user.out"
grep -q 'n00bb.phase-values v1 complete' \
    "$tmp/global_archive_user.out"
grep -q 'global_archive_dep.impl:0:fixed_global .*static-value=int:00000000000000000000000000000029' \
    "$tmp/global_archive_user.out"
grep -q 'global_archive_dep.impl:0:negative_global .*static-value=int:ffffffffffffffffffffffffffffff85' \
    "$tmp/global_archive_user.out"
grep -q 'global_archive_dep.impl:0:enabled .*static-value=bool:true' \
    "$tmp/global_archive_user.out"
grep -q 'global_archive_dep.impl:0:ratio .*static-value=float:0x1.4p+0' \
    "$tmp/global_archive_user.out"
grep -q 'global_archive_dep.impl:0:label .*static-value=string:13 bytes' \
    "$tmp/global_archive_user.out"
grep -q 'global_archive_dep.impl:0:empty_label .*static-value=string:0 bytes' \
    "$tmp/global_archive_user.out"
grep -q 'global_archive_dep.impl:0:worker_count .*storage=thread, .*static-value=int:00000000000000000000000000000000' \
    "$tmp/global_archive_user.out"
grep -q 'global_archive_dep.impl:0:worker_ratio .*storage=thread, .*static-value=float:-0x1.4p+0' \
    "$tmp/global_archive_user.out"
grep -q 'global_archive_dep.impl:0:worker_label .*storage=thread, .*static-value=string:11 bytes' \
    "$tmp/global_archive_user.out"
grep -q 'global_archive_dep.impl:0:empty_worker_label .*storage=thread, .*static-value=string:0 bytes' \
    "$tmp/global_archive_user.out"

"$compiler" preprocess --dump-symbols \
    "$interface_programs/array_phase_archive_dep/impl.n00b" \
    >"$tmp/array_phase_archive_source.out" 2>&1 || {
    cat "$tmp/array_phase_archive_source.out"
    exit 1
}
grep -q 'fixed_values: variable, const, .*array-shape=rank:1, dim0=unresolved' \
    "$tmp/array_phase_archive_source.out"
grep -q 'worker_values: variable, mutable, .*array-shape=rank:1, dim0=unresolved, visibility=public, thread-local, phases=runtime' \
    "$tmp/array_phase_archive_source.out"
N00B_PATH="$interface_programs" "$compiler" library \
    --emit-interface "$tmp/array_phase_archive.n00bi" \
    array_phase_archive_dep.facade \
    >"$tmp/array_phase_archive_library.out" \
    2>"$tmp/array_phase_archive_library.err" || {
    cat "$tmp/array_phase_archive_library.err"
    exit 1
}
strings "$tmp/array_phase_archive.n00bi" | grep -q 'array_phase_archive_dep.fixed_values'
strings "$tmp/array_phase_archive.n00bi" | grep -q 'array_phase_archive_dep.worker_values'
strings "$tmp/array_phase_archive.n00bi" | grep -q 'n00b.array-shape'
strings "$tmp/array_phase_archive.n00bi" | grep -q 'N0AS'
"$compiler" lower --link-interface "$tmp/array_phase_archive.n00bi" \
    --dump-symbols --dump-ir \
    "$interface_programs/array_phase_archive_user.n00b" \
    >"$tmp/array_phase_archive_user.out" \
    2>"$tmp/array_phase_archive_user.err" || {
    cat "$tmp/array_phase_archive_user.err"
    cat "$tmp/array_phase_archive_user.out"
    exit 1
}
grep -q 'extern const global @array_phase_archive_dep.fixed_values: array\[int\]' \
    "$tmp/array_phase_archive_user.out"
grep -q 'extern global @array_phase_archive_dep.worker_values: array\[int\].*n00b.thread_local(true)' \
    "$tmp/array_phase_archive_user.out"
grep -q 'func @read_label' "$tmp/global_archive_user.out"
grep -q 'func @read_worker_count' "$tmp/global_archive_user.out"
grep -q 'func @read_worker_ratio' "$tmp/global_archive_user.out"
grep -q 'func @read_worker_label' "$tmp/global_archive_user.out"
grep -q 'func @read_empty_worker_label' "$tmp/global_archive_user.out"

global_lifetime_selected_offset=
for global_lifetime_metadata_offset in $(grep -abo 'n00b.global' \
        "$tmp/global_archive.n00bi" | cut -d: -f1); do
    cp "$tmp/global_archive.n00bi" \
        "$tmp/global_archive_missing_lifetime.n00bi"
    printf '\000' | dd \
        of="$tmp/global_archive_missing_lifetime.n00bi" \
        bs=1 seek="$((global_lifetime_metadata_offset + 27))" \
        conv=notrunc 2>/dev/null
    if ! "$compiler" lower \
        --link-interface "$tmp/global_archive_missing_lifetime.n00bi" \
        --dump-link-unit "$interface_programs/global_archive_user.n00b" \
        >"$tmp/global_archive_missing_lifetime.out" \
        2>"$tmp/global_archive_missing_lifetime.err"; then
        continue
    fi
    if grep -q 'n00bb.storage-lifetimes v1 unavailable' \
            "$tmp/global_archive_missing_lifetime.out"; then
        global_lifetime_selected_offset=$global_lifetime_metadata_offset
        break
    fi
done
if [ -z "$global_lifetime_selected_offset" ]; then
    echo 'could not locate imported global lifetime metadata in test archive' >&2
    exit 1
fi
grep -q 'n00bb.storage-lifetimes v1 unavailable' \
    "$tmp/global_archive_missing_lifetime.out"

if "$compiler" lower --link-interface "$tmp/global_archive.n00bi" \
    "$interface_programs/global_archive_const_user.n00b" \
    >"$tmp/global_archive_const_user.out" 2>&1; then
    echo 'assignment to an archived const global unexpectedly lowered' >&2
    exit 1
fi
grep -q 'cannot assign to this binding' "$tmp/global_archive_const_user.out"

N00B_PATH="$interface_programs" "$compiler" library \
    --emit-interface "$tmp/context_archive.n00bi" context_archive_dep.facade \
    >"$tmp/context_archive_library.out" \
    2>"$tmp/context_archive_library.err" || {
    cat "$tmp/context_archive_library.err"
    exit 1
}
"$compiler" lower --link-interface "$tmp/context_archive.n00bi" \
    --dump-symbols --dump-ir --dump-link-unit \
    "$interface_programs/context_archive_user.n00b" \
    >"$tmp/context_archive_user.out" 2>"$tmp/context_archive_user.err" || {
    cat "$tmp/context_archive_user.err"
    exit 1
}
grep -q 'call @__n00bb_context_RenamedArchiveContext_enter' \
    "$tmp/context_archive_user.out"
grep -q 'call @__n00bb_context_RenamedArchiveContext_exit' \
    "$tmp/context_archive_user.out"
grep -q 'call @__n00bb_context_RenamedArchiveContext_ensure' \
    "$tmp/context_archive_user.out"
grep -q 'call @__n00bb_context_RenamedArchiveContext_error' \
    "$tmp/context_archive_user.out"
grep -q 'call @__n00bb_context_RenamedArchiveContext_default_1, %id' \
    "$tmp/context_archive_user.out"
test "$(grep -c 'call @__n00bb_context_RenamedArchiveContext_default_1' \
    "$tmp/context_archive_user.out")" -eq 1
grep -q 'extern func @__n00bb_context_RenamedArchiveContext_default_1(%arg0: int) -> int fail: int' \
    "$tmp/context_archive_user.out"
grep -q 'RenamedArchiveContext: variable, mutable, type=(int, int) -> int fail: int, visibility=public, phases=comptime+runtime' \
    "$tmp/context_archive_user.out"
grep -q '__n00bb_context_RenamedArchiveContext_exit: function, mutable, type=(int, int, int) -> nil fail: int' \
    "$tmp/context_archive_user.out"
grep -q '__n00bb_context_RenamedArchiveContext_ensure: function, mutable, type=(int, int, int) -> nil fail: nil' \
    "$tmp/context_archive_user.out"

N00B_PATH="$interface_programs" "$compiler" library \
    --emit-interface "$tmp/context_effect_archive.n00bi" \
    context_effect_dep.facade \
    >"$tmp/context_effect_archive_library.out" \
    2>"$tmp/context_effect_archive_library.err" || {
    cat "$tmp/context_effect_archive_library.err"
    exit 1
}
"$compiler" lower --link-interface "$tmp/context_effect_archive.n00bi" \
    --dump-link-unit "$interface_programs/context_effect_user.n00b" \
    >"$tmp/context_effect_archive_user.out" \
    2>"$tmp/context_effect_archive_user.err" || {
    cat "$tmp/context_effect_archive_user.err"
    exit 1
}
grep -q 'function-global-alias-summary\[resolved\] context_effect_dep.impl:0:UpdatingContext -> context_effect_dep.impl:0:shared_value' \
    "$tmp/context_effect_archive_user.out"
grep -q 'function-global-write-summary\[resolved\] context_effect_dep.impl:0:UpdatingContext -> context_effect_dep.impl:0:shared_value' \
    "$tmp/context_effect_archive_user.out"
grep -q 'function-global-write-summary\[resolved\] context_effect_user:1:use_archived_context_effect -> context_effect_dep.impl:0:shared_value' \
    "$tmp/context_effect_archive_user.out"
grep -q 'function-global-write-summary\[resolved\] context_effect_dep.impl:0:FailingUpdatingContext -> context_effect_dep.impl:0:shared_value' \
    "$tmp/context_effect_archive_user.out"
grep -q 'function-global-write-summary\[resolved\] context_effect_user:1:use_archived_nested_context_effect -> context_effect_dep.impl:0:shared_value' \
    "$tmp/context_effect_archive_user.out"
grep -q 'function-global-write-summary\[resolved\] context_effect_user:1:use_archived_failing_context_effect -> context_effect_dep.impl:0:shared_value' \
    "$tmp/context_effect_archive_user.out"
if grep -q 'function-global-unknown\[unavailable\].*UpdatingContext' \
    "$tmp/context_effect_archive_user.out"; then
    echo 'archived context effects were replaced by an unknown summary' >&2
    exit 1
fi

N00B_PATH="$interface_programs" "$compiler" library \
    --emit-interface "$tmp/function_archive.n00bi" function_archive_dep.facade \
    >"$tmp/function_archive_library.out" \
    2>"$tmp/function_archive_library.err" || {
    cat "$tmp/function_archive_library.err"
    exit 1
}
strings "$tmp/function_archive.n00bi" | grep -q \
    'function_archive_dep.renamed_add'
strings "$tmp/function_archive.n00bi" | grep -q \
    'function_archive_dep.renamed_update'
strings "$tmp/function_archive.n00bi" | grep -q \
    'function_archive_dep.renamed_keep'
cp "$tmp/function_archive.n00bi" \
    "$tmp/function_archive_before_wrapper.n00bi"
N00B_PATH="$interface_programs" "$compiler" library \
    --emit-interface "$tmp/function_archive_wrapper.n00bi" \
    --link-interface "$tmp/function_archive.n00bi" \
    function_archive_wrapper.facade \
    >"$tmp/function_archive_wrapper_library.out" \
    2>"$tmp/function_archive_wrapper_library.err" || {
    cat "$tmp/function_archive_wrapper_library.err"
    cat "$tmp/function_archive_wrapper_library.out"
    exit 1
}
cmp -s "$tmp/function_archive_before_wrapper.n00bi" \
    "$tmp/function_archive.n00bi" || {
    echo 'composing archived lifetime effects mutated the frozen input interface' >&2
    exit 1
}
"$compiler" lower --link-interface "$tmp/function_archive_wrapper.n00bi" \
    --dump-symbols --dump-link-unit \
    "$interface_programs/function_archive_wrapper_user.n00b" \
    >"$tmp/function_archive_wrapper_user.out" \
    2>"$tmp/function_archive_wrapper_user.err" || {
    cat "$tmp/function_archive_wrapper_user.err"
    cat "$tmp/function_archive_wrapper_user.out"
    exit 1
}
grep -q 'target: variable, mutable, type=int, .*storage-duration=automatic' \
    "$tmp/function_archive_wrapper_user.out"
grep -q 'return_target: variable, mutable, type=int, .*storage-duration=dynamic, .*may-escape' \
    "$tmp/function_archive_wrapper_user.out"
grep -q 'global_ref: variable, mutable, .*aliases=1' \
    "$tmp/function_archive_wrapper_user.out"

cp "$tmp/function_archive.n00bi" \
    "$tmp/function_archive_bad_lifetime.n00bi"
keep_owner_offset=$(grep -abo 'function_archive_dep.archived_keep' \
    "$tmp/function_archive_bad_lifetime.n00bi" \
    | tail -1 | cut -d: -f1)
keep_summary_offset=$(grep -abo 'N0FL' \
    "$tmp/function_archive_bad_lifetime.n00bi" \
    | cut -d: -f1 \
    | awk -v owner="$keep_owner_offset" '$1 > owner { print; exit }')
if [ -z "$keep_owner_offset" ] || [ -z "$keep_summary_offset" ]; then
    echo 'could not locate archived function lifetime summary in test archive' >&2
    exit 1
fi
printf '\377' | dd \
    of="$tmp/function_archive_bad_lifetime.n00bi" \
    bs=1 seek="$((keep_summary_offset + 9))" conv=notrunc 2>/dev/null
"$compiler" preprocess \
    --link-interface "$tmp/function_archive_bad_lifetime.n00bi" \
    --dump-symbols "$interface_programs/function_archive_reference_escape.n00b" \
    >"$tmp/function_archive_bad_lifetime.out" \
    2>"$tmp/function_archive_bad_lifetime.err" || {
    cat "$tmp/function_archive_bad_lifetime.err"
    cat "$tmp/function_archive_bad_lifetime.out"
    exit 1
}
grep -q 'target: variable, mutable, type=int, .*storage-duration=dynamic, .*may-escape' \
    "$tmp/function_archive_bad_lifetime.out"

cp "$tmp/function_archive.n00bi" \
    "$tmp/function_archive_unknown_lifetime.n00bi"
printf '\000' | dd \
    of="$tmp/function_archive_unknown_lifetime.n00bi" \
    bs=1 seek="$((keep_summary_offset + 9))" conv=notrunc 2>/dev/null
"$compiler" preprocess \
    --link-interface "$tmp/function_archive_unknown_lifetime.n00bi" \
    --dump-symbols "$interface_programs/function_archive_reference_escape.n00b" \
    >"$tmp/function_archive_unknown_lifetime.out" \
    2>"$tmp/function_archive_unknown_lifetime.err" || {
    cat "$tmp/function_archive_unknown_lifetime.err"
    cat "$tmp/function_archive_unknown_lifetime.out"
    exit 1
}
grep -q 'target: variable, mutable, type=int, .*storage-duration=dynamic, .*may-escape' \
    "$tmp/function_archive_unknown_lifetime.out"

"$compiler" preprocess --link-interface "$tmp/function_archive.n00bi" \
    --dump-link-unit \
    "$interface_programs/function_archive_initializer_unknown.n00b" \
    >"$tmp/function_archive_initializer_unknown.link" \
    2>"$tmp/function_archive_initializer_unknown.link.err" || {
    cat "$tmp/function_archive_initializer_unknown.link.err"
    cat "$tmp/function_archive_initializer_unknown.link"
    exit 1
}
grep -q 'n00bb.semantic-ready v1 complete' \
    "$tmp/function_archive_initializer_unknown.link"
grep -q 'n00bb.initializer-dependencies v1 unavailable' \
    "$tmp/function_archive_initializer_unknown.link"
if "$compiler" lower --link-interface "$tmp/function_archive.n00bi" \
    --dump-ir --dump-link-unit \
    "$interface_programs/function_archive_initializer_unknown.n00b" \
    >"$tmp/function_archive_initializer_unknown.out" \
    2>"$tmp/function_archive_initializer_unknown.err"; then
    echo 'an initializer with unknown archived global effects unexpectedly lowered' >&2
    exit 1
fi
grep -q 'required link-unit analysis contribution is not complete' \
    "$tmp/function_archive_initializer_unknown.err" || {
    cat "$tmp/function_archive_initializer_unknown.out"
    cat "$tmp/function_archive_initializer_unknown.err"
    exit 1
}
"$compiler" lower --link-interface "$tmp/function_archive.n00bi" \
    --dump-symbols --dump-ir --dump-link-unit \
    "$interface_programs/function_archive_user.n00b" \
    >"$tmp/function_archive_user.out" \
    2>"$tmp/function_archive_user.err" || {
    cat "$tmp/function_archive_user.err"
    exit 1
}
grep -q 'call @archived_add' "$tmp/function_archive_user.out"
grep -Eq 'call @archived_add, %[^,]+, missing' \
    "$tmp/function_archive_user.out"
if grep -q '__n00bb_default_present' "$tmp/function_archive_user.out"; then
    echo 'an archived default argument retained a presence/value wrapper' >&2
    exit 1
fi
grep -q 'call @archived_update' "$tmp/function_archive_user.out"
grep -q 'call @archived_variadic' "$tmp/function_archive_user.out"
grep -q 'renamed_add: function, mutable, type=(int, int) -> int fail: nil, visibility=public.*phases=comptime+runtime' \
    "$tmp/function_archive_user.out"
grep -q 'renamed_update: function, mutable, type=(out int, inout int) -> nil fail: nil' \
    "$tmp/function_archive_user.out"
grep -q 'renamed_variadic: function, mutable, type=(int, \*int) -> int fail: nil' \
    "$tmp/function_archive_user.out"
grep -q 'extern func @archived_add(%arg0: int, %arg1: int !ir.optional_parameter(true)) -> int fail: nil' \
    "$tmp/function_archive_user.out"
grep -q 'function-global-unknown\[unavailable\] function_archive_dep.impl:1:archived_add -> <unresolved>' \
    "$tmp/function_archive_user.out"
grep -q 'function-global-unknown\[unavailable\] function_archive_user:1:forward_archived_default -> <unresolved>' \
    "$tmp/function_archive_user.out"
if grep -q 'initializer-use\[unavailable\] function_archive_user:0:archive_initialized -> <unresolved>' \
    "$tmp/function_archive_user.out"; then
    echo 'a literal initializer acquired an unrelated unavailable effect' >&2
    exit 1
fi
"$compiler" preprocess --link-interface "$tmp/function_archive.n00bi" \
    --dump-symbols --dump-link-unit \
    "$interface_programs/function_archive_reference_escape.n00b" \
    >"$tmp/function_archive_symbols.out" \
    2>"$tmp/function_archive_symbols.err" || {
    cat "$tmp/function_archive_symbols.err"
    cat "$tmp/function_archive_symbols.out"
    exit 1
}
grep -q 'target: variable, mutable, type=int, .*storage-duration=automatic' \
    "$tmp/function_archive_symbols.out"
grep -q 'cycle_target: variable, mutable, type=int, .*storage-duration=automatic' \
    "$tmp/function_archive_symbols.out"
grep -q 'return_target: variable, mutable, type=int, .*storage-duration=dynamic, .*may-escape' \
    "$tmp/function_archive_symbols.out"
grep -q 'imported_global_ref: variable, mutable, type=Ref\[int\], .*aliases=1' \
    "$tmp/function_archive_symbols.out"
grep -q 'function-global-unknown\[unavailable\] function_archive_dep.impl:1:archived_keep -> <unresolved>' \
    "$tmp/function_archive_symbols.out"
"$compiler" lower --link-interface "$tmp/function_archive.n00bi" \
    --dump-ir --dump-link-unit \
    "$interface_programs/function_archive_reference_escape.n00b" \
    >"$tmp/function_archive_reference_escape.out" \
    2>"$tmp/function_archive_reference_escape.err" || {
    cat "$tmp/function_archive_reference_escape.err"
    cat "$tmp/function_archive_reference_escape.out"
    exit 1
}
grep -q 'call @archived_keep, .*!ir.call_signature((ref\[int\]) -> nil fail: nil)' \
    "$tmp/function_archive_reference_escape.out"
if grep -q 'ir.storage_class("gc") .*ir.lifetime_escapes(true)' \
    "$tmp/function_archive_reference_escape.out"; then
    echo 'runtime-managed allocation was assigned a frontend-selected gc class' >&2
    exit 1
fi

N00B_PATH="$program_dir" "$compiler" preprocess --dump-symbols \
    "$program_dir/link_global_inferred_dep/impl.n00b" \
    >"$tmp/link_global_inferred_dep.symbols" 2>&1
grep -q 'remote_global: variable, mutable, type=int, type-source=inferred, visibility=public, .*storage-duration=module, .*declaration=none, definitions=\[3:8\]' \
    "$tmp/link_global_inferred_dep.symbols" || {
    cat "$tmp/link_global_inferred_dep.symbols"
    echo 'assignment-inferred module global has incorrect ownership or storage duration' >&2
    exit 1
}
grep -q 'remote_global: variable, mutable, type=int, .*uses=1, declaration=1:12, definitions=\[1:12\], use-sites=\[3:24\]' \
    "$tmp/link_global_inferred_dep.symbols" || {
    cat "$tmp/link_global_inferred_dep.symbols"
    echo 'resolved cross-TU use was not recorded on the defining symbol' >&2
    exit 1
}

N00B_PATH="$program_dir" "$compiler" preprocess \
    --dump-symbols --dump-link-unit \
    "$program_dir/qualified_render_consumer.n00b" \
    >"$tmp/qualified_render_consumer.out" 2>&1 || {
    cat "$tmp/qualified_render_consumer.out"
    exit 1
}
grep -q 'resolved qualified_render_consumer -> render_contract.contract:2:Render at 3:51' \
    "$tmp/qualified_render_consumer.out"
grep -q 'Render: type, .*uses=1, declaration=1:1, definitions=\[\], use-sites=\[3:51\]' \
    "$tmp/qualified_render_consumer.out"

if N00B_PATH="$program_dir" "$compiler" preprocess \
    "$program_dir/ambiguous_render_consumer.n00b" \
    >"$tmp/ambiguous_render_consumer.out" 2>&1; then
    cat "$tmp/ambiguous_render_consumer.out"
    echo 'ambiguous imported trait short name unexpectedly resolved' >&2
    exit 1
fi
grep -q 'constraint implements: interface not satisfied' \
    "$tmp/ambiguous_render_consumer.out" || {
    cat "$tmp/ambiguous_render_consumer.out"
    echo 'ambiguous imported trait did not produce a trait diagnostic' >&2
    exit 1
}

if "$compiler" preprocess "$program_dir/signature_mismatch.n00b" \
    >"$tmp/signature_mismatch.out" 2>&1; then
    cat "$tmp/signature_mismatch.out"
    echo 'mismatched function signature unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'N00BB-T0001' "$tmp/signature_mismatch.out"
grep -q 'signature_mismatch.n00b:1:1' "$tmp/signature_mismatch.out"

if "$compiler" preprocess "$program_dir/call_argument_mismatch.n00b" \
    >"$tmp/call_argument_mismatch.out" 2>&1; then
    cat "$tmp/call_argument_mismatch.out"
    echo 'mismatched call argument unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'N00BB-T0002' "$tmp/call_argument_mismatch.out" || {
    cat "$tmp/call_argument_mismatch.out"
    exit 1
}
grep -q 'call_argument_mismatch.n00b:2:' \
    "$tmp/call_argument_mismatch.out"
grep -q 'call_argument_mismatch.n00b:3:' \
    "$tmp/call_argument_mismatch.out"

"$compiler" preprocess --dump-symbols "$program_dir/recursive_calls.n00b" \
    >"$tmp/recursive_calls.out"
grep -q 'first: function, mutable, type=() -> int' \
    "$tmp/recursive_calls.out"
grep -q 'second: function, mutable, type=() -> int' \
    "$tmp/recursive_calls.out"
enabled_line=$(grep -n 'enabled: variable' "$tmp/branches.symbols" | cut -d: -f1)
numeric_line=$(grep -n 'numeric: variable' "$tmp/branches.symbols" | cut -d: -f1)
[ "$enabled_line" -lt "$numeric_line" ]

if "$compiler" preprocess "$program_dir/non_bool_condition.n00b" \
    >"$tmp/non_bool_condition.out" 2>&1; then
    echo 'non-bool when condition unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'when condition must have static type bool' \
    "$tmp/non_bool_condition.out"

if "$compiler" preprocess "$program_dir/skipped_call.n00b" \
    >"$tmp/skipped_call.out" 2>&1; then
    echo 'short-circuited non-builtin call unexpectedly passed validation' >&2
    exit 1
fi
grep -q 'only processor-time builtins may be called' \
    "$tmp/skipped_call.out"

if "$compiler" preprocess "$program_dir/skipped_type_error.n00b" \
    >"$tmp/skipped_type_error.out" 2>&1; then
    echo 'short-circuited non-bool operand unexpectedly passed type checking' >&2
    exit 1
fi
grep -q 'operator or operand type is not supported by the preprocessor' \
    "$tmp/skipped_type_error.out"

if "$compiler" preprocess "$program_dir/skipped_ineligible_const.n00b" \
    >"$tmp/skipped_ineligible_const.out" 2>&1; then
    echo 'short-circuited ineligible const unexpectedly passed checking' >&2
    exit 1
fi
grep -q 'ineligible const variable' "$tmp/skipped_ineligible_const.out"

"$compiler" preprocess "$program_dir/false_branch.n00b" >"$tmp/false_branch.out"
grep -q '"selected"' "$tmp/false_branch.out"
! grep -q '"removed"' "$tmp/false_branch.out"

"$compiler" preprocess "$program_dir/else_when.n00b" >"$tmp/else_when.out"
grep -q '"middle_selected"' "$tmp/else_when.out"
! grep -q '"first_removed"' "$tmp/else_when.out"
! grep -q '"final_removed"' "$tmp/else_when.out"

"$compiler" preprocess "$program_dir/else_only.n00b" >"$tmp/else_only.out"
grep -q '"final_selected"' "$tmp/else_only.out"
! grep -q '"first_removed"' "$tmp/else_only.out"
! grep -q '"middle_removed"' "$tmp/else_only.out"

if "$compiler" preprocess "$program_dir/annotated_condition.n00b" \
    >"$tmp/annotated_condition.out" 2>&1; then
    echo 'annotated literal unexpectedly became an eligible condition' >&2
    exit 1
fi
grep -q "expression form .* is not allowed" \
    "$tmp/annotated_condition.out"

if "$compiler" preprocess "$program_dir/modifier_condition.n00b" \
    >"$tmp/modifier_condition.out" 2>&1; then
    echo 'literal-modifier builtin unexpectedly became a function call' >&2
    exit 1
fi
grep -q 'literal modifiers are not allowed' "$tmp/modifier_condition.out"

N00BB_TEST_ENV=present "$compiler" preprocess \
    -D FEATURE=true "$program_dir/builtin_calls.n00b" >"$tmp/builtin_calls.out"
grep -q '"selected"' "$tmp/builtin_calls.out"
grep -q '"present"' "$tmp/builtin_calls.out"

"$compiler" preprocess "$program_dir/i128_min.n00b" >"$tmp/i128_min.out"
grep -q '"selected"' "$tmp/i128_min.out"
! grep -q '"removed"' "$tmp/i128_min.out"

if "$compiler" preprocess "$program_dir/i128_remainder_overflow.n00b" \
    >"$tmp/i128_remainder_overflow.out" 2>&1; then
    echo 'i128 remainder overflow unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'checked i128 arithmetic' "$tmp/i128_remainder_overflow.out"

"$compiler" preprocess "$program_dir/ineligible_consts.n00b" \
    >"$tmp/ineligible_consts.out"
grep -q '"selected"' "$tmp/ineligible_consts.out"

if "$compiler" preprocess "$program_dir/ineligible_const_use.n00b" \
    >"$tmp/ineligible_const_use.out" 2>&1; then
    echo 'ineligible character const unexpectedly became available' >&2
    exit 1
fi
grep -q 'ineligible const variable' "$tmp/ineligible_const_use.out"

"$compiler" preprocess "$program_dir/const_local.n00b" \
    >"$tmp/const_local.out"
grep -q '"selected"' "$tmp/const_local.out"

if "$compiler" preprocess "$program_dir/const_scope.n00b" \
    >"$tmp/const_scope.out" 2>&1; then
    echo 'block-local const unexpectedly escaped into a sibling scope' >&2
    exit 1
fi
grep -q 'ineligible const variable' "$tmp/const_scope.out"
! grep -q '"leaked"' "$tmp/const_scope.out"

"$compiler" preprocess -D FEATURE=true -D COUNT=42 \
    "$program_dir/external.n00b" >"$tmp/external.out"
grep -q '"selected"' "$tmp/external.out"
grep -q '"42"' "$tmp/external.out"
! grep -q '"removed"' "$tmp/external.out"

"$compiler" preprocess -D 'TEXT="hello"' -D SCALE=1.5 \
    "$program_dir/external_other_types.n00b" >"$tmp/external_other_types.out"
grep -q '"text_value"' "$tmp/external_other_types.out"
grep -q '"hello"' "$tmp/external_other_types.out"
grep -q '"1.5"' "$tmp/external_other_types.out"
! grep -q '"missing_definition"' "$tmp/external_other_types.out"

"$compiler" preprocess "$program_dir/short_circuit.n00b" \
    >"$tmp/short_circuit.out"
grep -q '"short_circuit_selected"' "$tmp/short_circuit.out"
grep -q '"skipped_arithmetic_selected"' "$tmp/short_circuit.out"
! grep -q 'no externally supplied value' "$tmp/short_circuit.out"

if "$compiler" preprocess "$program_dir/skipped_diagnostic.n00b" \
    >"$tmp/skipped_diagnostic.out" 2>&1; then
    echo 'void diagnostic call unexpectedly passed static type checking' >&2
    exit 1
fi
! grep -q 'must not run' "$tmp/skipped_diagnostic.out"

N00BB_TEST_ENV=present "$compiler" preprocess "$program_dir/environment.n00b" \
    >"$tmp/environment.out"
grep -q '"environment_selected"' "$tmp/environment.out"

env -u N00BB_UNSET_TEST "$compiler" preprocess \
    "$program_dir/undefined_environment.n00b" >"$tmp/undefined_environment.out"
grep -q '"undefined_environment_selected"' \
    "$tmp/undefined_environment.out"

"$compiler" preprocess -D LEVEL=1 -D LEVEL=2 \
    "$program_dir/repeated_definitions.n00b" >"$tmp/repeated_definitions.out"
grep -q '"last_definition_wins"' "$tmp/repeated_definitions.out"

if "$compiler" preprocess \
    -D TOO_BIG=170141183460469231731687303715884105728 \
    "$program_dir/out_of_range_definition.n00b" \
    >"$tmp/out_of_range_definition.out" 2>&1; then
    echo 'out-of-range external integer unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'external integer value is outside the preprocessor i128 range' \
    "$tmp/out_of_range_definition.out"

if "$compiler" preprocess -D TOO_WIDE_FLOAT=1e999 \
    "$program_dir/out_of_range_float_definition.n00b" \
    >"$tmp/out_of_range_float_definition.out" 2>&1; then
    echo 'out-of-range external float unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'external float value is outside the preprocessor f64 range' \
    "$tmp/out_of_range_float_definition.out"

"$compiler" preprocess "$program_dir/strings.n00b" >"$tmp/strings.out"
grep -q '"selected"' "$tmp/strings.out"
! grep -q '"removed"' "$tmp/strings.out"
grep -q '"unicode_selected"' "$tmp/strings.out"
! grep -q '"unicode_removed"' "$tmp/strings.out"

"$compiler" preprocess "$program_dir/float_ops.n00b" >"$tmp/float_ops.out"
grep -q '"float_selected"' "$tmp/float_ops.out"
! grep -q '"float_removed"' "$tmp/float_ops.out"
grep -q '"string_not_equal"' "$tmp/float_ops.out"

"$compiler" lower --dump-ir --dump-link-unit \
    "$program_dir/float_alias.n00b" >"$tmp/float_alias.out" 2>&1 || {
    cat "$tmp/float_alias.out"
    exit 1
}
grep -q 'static-value=float:0x1.4p+0' "$tmp/float_alias.out"

"$compiler" preprocess "$program_dir/integer_bit_ops.n00b" \
    >"$tmp/integer_bit_ops.out"
grep -q '"integer_ops_selected"' "$tmp/integer_bit_ops.out"

if "$compiler" preprocess "$program_dir/unsupported_operation.n00b" \
    >"$tmp/unsupported_operation.out" 2>&1; then
    echo 'unsupported string ordering unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'operator or operand type is not supported by the preprocessor' \
    "$tmp/unsupported_operation.out"

if "$compiler" preprocess "$program_dir/diagnostic_condition.n00b" \
    >"$tmp/diagnostic_condition.out" 2>&1; then
    echo 'diagnostic builtin unexpectedly supplied a when condition' >&2
    exit 1
fi
grep -q 'not a value' "$tmp/diagnostic_condition.out"
grep -q 'when condition must have static type bool' \
    "$tmp/diagnostic_condition.out"

"$compiler" preprocess "$program_dir/target_builtins.n00b" \
    >"$tmp/target_builtins.out"
grep -q '"selected"' "$tmp/target_builtins.out"
! grep -q '"unsupported"' "$tmp/target_builtins.out"

"$compiler" preprocess "$program_dir/implementation_builtins.n00b" \
    >"$tmp/implementation_builtins.out"
grep -q '"implementation_selected"' "$tmp/implementation_builtins.out"

if "$compiler" preprocess "$program_dir/missing.n00b" >"$tmp/missing.out" 2>&1; then
    echo 'missing preprocessor value unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'no externally supplied value' "$tmp/missing.out"

"$compiler" preprocess "$program_dir/warning.n00b" >"$tmp/warning.out" 2>&1
grep -q 'warning\[N00BB-PPW0001\]: line 1, column 1: heads up' \
    "$tmp/warning.out"

"$compiler" preprocess "$program_dir/nested_warnings.n00b" \
    >"$tmp/nested_warnings.out" 2>&1
grep -q 'line 1, column 1: first' "$tmp/nested_warnings.out"
grep -q 'line 1, column 27: second' "$tmp/nested_warnings.out"
[ "$(grep -c 'N00BB-PPW0001' "$tmp/nested_warnings.out")" -eq 2 ]

if "$compiler" preprocess "$program_dir/error.n00b" >"$tmp/error.out" 2>&1; then
    echo 'compile_error unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'fatal\[N00BB-PP0002\]: line 1, column 1: stop here' \
    "$tmp/error.out"

if "$compiler" lower --dump-ir --dump-link-unit \
    "$program_dir/error.n00b" \
    >"$tmp/error_lower.out" 2>"$tmp/error_lower.err"; then
    echo 'root lowering continued after a fatal preprocessor diagnostic' >&2
    exit 1
fi
grep -q 'fatal\[N00BB-PP0002\]: line 1, column 1: stop here' \
    "$tmp/error_lower.err"
test ! -s "$tmp/error_lower.out"

if "$compiler" preprocess "$program_dir/error_condition.n00b" \
    >"$tmp/error_condition.out" 2>&1; then
    echo 'compile_error in a when condition unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'fatal\[N00BB-PP0002\].*stop condition' \
    "$tmp/error_condition.out"
! grep -q 'when condition must have static type bool' \
    "$tmp/error_condition.out"
[ "$(grep -c 'N00BB-PP0002' "$tmp/error_condition.out")" -eq 1 ]

if N00B_PATH="$program_dir" "$compiler" lower --dump-ir \
    --dump-link-unit "$program_dir/fatal_import_root.n00b" \
    >"$tmp/fatal_import.out" 2>&1; then
    echo 'root lowering continued after a fatal imported-module diagnostic' >&2
    exit 1
fi
grep -q 'fatal\[N00BB-PP0002\].*fatal dependency' \
    "$tmp/fatal_import.out"
! grep -q '^link-unit:' "$tmp/fatal_import.out"
! grep -q '^module fatal_import_root' "$tmp/fatal_import.out"

if N00B_PATH="$script_dir/programs/type_visibility" \
    "$compiler" lower --dump-ir --dump-link-unit \
    "$program_dir/private_field_consumer.n00b" \
    >"$tmp/semantic_failure.out" 2>&1; then
    echo 'root lowering continued after an imported LinkUnit semantic error' >&2
    exit 1
fi
grep -q 'N00BB-L0021.*record field is not visible' \
    "$tmp/semantic_failure.out"
! grep -q '^link-unit:' "$tmp/semantic_failure.out"
! grep -q '^module private_field_consumer' "$tmp/semantic_failure.out"

N00B_PATH="$program_dir/imports" "$compiler" preprocess importer \
    >"$tmp/import.out"
grep -q '"imported_selected"' "$tmp/import.out"
grep -q '"imported_short_name"' "$tmp/import.out"
! grep -q '"imported_removed"' "$tmp/import.out"
! grep -q '"imported_short_name_removed"' "$tmp/import.out"

N00B_PATH="$program_dir/imports" "$compiler" preprocess --dump-symbols \
    --dump-link-unit selected_when_import \
    >"$tmp/selected_when_import.out" 2>&1 || {
    cat "$tmp/selected_when_import.out"
    exit 1
}
grep -q '^  imports: selected_when_dep$' "$tmp/selected_when_import.out"
grep -q 'resolved selected_when_import -> selected_when_dep:0:enabled' \
    "$tmp/selected_when_import.out"
! grep -q 'missing_unselected_import' "$tmp/selected_when_import.out"
! grep -q 'missing_else_import' "$tmp/selected_when_import.out"

N00B_PATH="$program_dir/imports" "$compiler" preprocess --dump-symbols \
    platform_selected_import \
    >"$tmp/platform_selected_import.out" 2>&1 || {
    cat "$tmp/platform_selected_import.out"
    exit 1
}
test "$(grep -Ec '^symbols platform_(macos|linux):$' \
    "$tmp/platform_selected_import.out")" -eq 1
case "$(uname -s)" in
    Darwin) selected_platform=platform_macos ;;
    Linux) selected_platform=platform_linux ;;
    *) selected_platform= ;;
esac
test -n "$selected_platform"
grep -q "^symbols $selected_platform:$" "$tmp/platform_selected_import.out"
! grep -q 'missing_unsupported_platform' "$tmp/platform_selected_import.out"

for invalid_import in import_in_if import_in_nested_if; do
    if N00B_PATH="$program_dir/imports" "$compiler" preprocess \
        "$invalid_import" \
        >"$tmp/$invalid_import.out" 2>&1; then
        echo "an import nested in ordinary control flow unexpectedly passed" >&2
        exit 1
    fi
    grep -q 'imports are only allowed at module level or directly in a selected when branch' \
        "$tmp/$invalid_import.out"
done

if N00B_PATH="$program_dir/imports" "$compiler" preprocess \
    unsupported_locator \
    >"$tmp/unsupported_import_locator.out" 2>&1; then
    echo 'an unsupported import locator unexpectedly passed' >&2
    exit 1
fi
grep -q "unsupported source locator scheme 'https'.*line 1, column 1" \
    "$tmp/unsupported_import_locator.out"

if "$compiler" locate-tu 'https://example.com/root.n00b' \
    >"$tmp/unsupported_root_locator.out" 2>&1; then
    echo 'an unsupported root locator unexpectedly passed' >&2
    exit 1
fi
grep -q "cannot read 'https://example.com/root.n00b'" \
    "$tmp/unsupported_root_locator.out"
grep -q 'unsupported source locator scheme: https' \
    "$tmp/unsupported_root_locator.out"

N00B_PATH="$program_dir/imports" "$compiler" preprocess --dump-symbols \
    --dump-link-unit \
    importer >"$tmp/imported_signatures.out"
grep -q 'imported_identity: function, mutable, type=(`a) -> `a fail: nil' \
    "$tmp/imported_signatures.out"
! grep -Fq 'forall[' "$tmp/imported_signatures.out"
grep -q 'dep:0:enabled .*static-value=bool:true' \
    "$tmp/imported_signatures.out"

if N00B_PATH="$program_dir/imports" "$compiler" preprocess --dump-symbols \
    --jobs 4 imported_generic_use \
    >"$tmp/imported_generic_use.out" 2>&1; then
    grep -q 'use_imported_generics: function, mutable, type=() -> nil' \
        "$tmp/imported_generic_use.out"
    grep -q 'first: variable, mutable, type=int' \
        "$tmp/imported_generic_use.out"
    grep -q 'second: variable, mutable, type=string' \
        "$tmp/imported_generic_use.out"
    grep -q 'imported_identity: function, mutable, type=(`a) -> `a fail: nil' \
        "$tmp/imported_generic_use.out"
    ! grep -Fq 'forall[' "$tmp/imported_generic_use.out"
else
    cat "$tmp/imported_generic_use.out"
    echo 'independent imported generic uses unexpectedly failed' >&2
    exit 1
fi

"$compiler" lower --dump-symbols \
    "$program_dir/generic_signature_multiple_instantiations.n00b" \
    >"$tmp/generic_signature_multiple_instantiations.out" 2>&1 || {
    cat "$tmp/generic_signature_multiple_instantiations.out"
    echo 'one explicit generic signature did not support independent call instantiations' >&2
    exit 1
}
grep -q 'first: variable, mutable, type=int' \
    "$tmp/generic_signature_multiple_instantiations.out"
grep -q 'second: variable, mutable, type=string' \
    "$tmp/generic_signature_multiple_instantiations.out"

if N00B_PATH="$program_dir" "$compiler" preprocess --dump-symbols \
    recursive_a --jobs 4 >"$tmp/recursive_modules.out" 2>&1; then
    grep -q 'from_a: function, mutable, type=() -> string' \
        "$tmp/recursive_modules.out"
    grep -q 'from_b: function, mutable, type=() -> string' \
        "$tmp/recursive_modules.out"
else
    cat "$tmp/recursive_modules.out"
    echo 'cross-module recursive inference unexpectedly failed' >&2
    exit 1
fi
if N00B_PATH="$program_dir" "$compiler" library --dump-ir \
    "$program_dir/lower_recursive_a.n00b" "$program_dir/lower_recursive_b.n00b" \
    >"$tmp/recursive_modules.ir" 2>"$tmp/recursive_modules.err"; then
    :
else
    grep -q "'library' processing is not implemented yet" \
        "$tmp/recursive_modules.err" || {
        cat "$tmp/recursive_modules.err"
        echo 'cross-module recursive lowering unexpectedly failed' >&2
        exit 1
    }
fi
grep -q '= call @from_b' "$tmp/recursive_modules.ir"
grep -q '= call @from_a' "$tmp/recursive_modules.ir"

if N00B_PATH="$program_dir" "$compiler" preprocess \
    recursive_bad_a >"$tmp/recursive_bad_modules.out" 2>&1; then
    cat "$tmp/recursive_bad_modules.out"
    echo 'incompatible cross-module recursive return unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'N00BB-T0002' "$tmp/recursive_bad_modules.out"

N00B_PATH="$program_dir/imports" "$compiler" preprocess \
    import_after_use >"$tmp/import_after_use.out"
grep -q '"import_still_precedes_module"' "$tmp/import_after_use.out"

N00B_PATH="$program_dir/imports" "$compiler" preprocess shadow_import \
    >"$tmp/shadow_import.out"
grep -q '"local_shadow_won"' "$tmp/shadow_import.out"
! grep -q '"shadow_removed"' "$tmp/shadow_import.out"

if N00B_PATH="$program_dir/imports" "$compiler" preprocess \
    --jobs 4 ambiguous_import \
    >"$tmp/ambiguous_import.out" 2>&1; then
    echo 'ambiguous imported const unexpectedly resolved by short name' >&2
    exit 1
fi
grep -q 'imported const name is ambiguous' "$tmp/ambiguous_import.out"

N00B_PATH="$program_dir/imports" "$compiler" preprocess \
    --jobs 4 qualified_import >"$tmp/qualified_import.out"
grep -q '"qualified_import"' "$tmp/qualified_import.out"

N00B_PATH="$program_dir" "$compiler" preprocess cycle_a \
    --dump-symbols --jobs 4 >"$tmp/cycle.out" 2>&1
grep -q '"cycle_selected"' "$tmp/cycle.out"
! grep -q '"cycle_removed"' "$tmp/cycle.out"
grep -q 'N00BB-PPW0002' "$tmp/cycle.out"
grep -q 'symbols cycle_a:' "$tmp/cycle.out"
grep -q 'symbols cycle_b:' "$tmp/cycle.out"
grep -q 'imports: cycle_b' "$tmp/cycle.out"
grep -q 'imports: cycle_a' "$tmp/cycle.out"

N00B_PATH="$program_dir" "$compiler" preprocess cycle_x \
    --dump-symbols --jobs 4 >"$tmp/indirect_cycle.out" 2>&1
grep -q '"indirect_cycle_selected"' "$tmp/indirect_cycle.out"
! grep -q '"indirect_cycle_removed"' "$tmp/indirect_cycle.out"
grep -q 'N00BB-PPW0002' "$tmp/indirect_cycle.out"
grep -q 'imports: cycle_y' "$tmp/indirect_cycle.out"
grep -q 'imports: cycle_z' "$tmp/indirect_cycle.out"
grep -q 'imports: cycle_x' "$tmp/indirect_cycle.out"

N00B_PATH="$program_dir" "$compiler" preprocess cycle_c \
    >"$tmp/cycle_short_circuit.out" 2>&1
grep -q '"short_circuit_selected"' "$tmp/cycle_short_circuit.out"
! grep -q 'N00BB-PPW0002' "$tmp/cycle_short_circuit.out"

"$compiler" parse "$program_dir/trait_receiver_modes.n00b" \
    >"$tmp/trait_receiver_modes.out"
grep -q '<trait-obj-tref>' "$tmp/trait_receiver_modes.out"
grep -q '"modify"' "$tmp/trait_receiver_modes.out"
grep -q '"?"' "$tmp/trait_receiver_modes.out"

"$compiler" parse "$program_dir/for_in_expression.n00b" \
    >"$tmp/for_in_expression.out" 2>&1 || {
    cat "$tmp/for_in_expression.out"
    exit 1
}
grep -q '<for-stmt>' "$tmp/for_in_expression.out" || {
    cat "$tmp/for_in_expression.out"
    echo 'for-in statement did not parse' >&2
    exit 1
}

"$compiler" preprocess --dump-symbols \
    "$script_dir/programs/for_in_trait_inference.n00b" \
    >"$tmp/for_in_trait_inference.out" \
    2>"$tmp/for_in_trait_inference.err" || {
    cat "$tmp/for_in_trait_inference.out"
    cat "$tmp/for_in_trait_inference.err"
    exit 1
}
grep -q 'item: variable, immutable, type=int' \
    "$tmp/for_in_trait_inference.out" || {
    cat "$tmp/for_in_trait_inference.out"
    echo 'for-in item type was not inferred from Iterator.next' >&2
    exit 1
}

"$compiler" lower --dump-ir \
    "$program_dir/for_binding_forms.n00b" \
    >"$tmp/for_binding_forms.ir" \
    2>"$tmp/for_binding_forms.err" || {
    cat "$tmp/for_binding_forms.err"
    exit 1
}
grep -q 'extract.index .* 0' "$tmp/for_binding_forms.ir"
grep -q 'extract.index .* 1' "$tmp/for_binding_forms.ir"
grep -q 'extract .* "number"' "$tmp/for_binding_forms.ir"
grep -q 'extract .* "label"' "$tmp/for_binding_forms.ir"
test "$(grep -c 'N00BB-W0005' "$tmp/for_binding_forms.err")" -eq 1
"$compiler" library --dump-ir "$program_dir/for_binding_forms.n00b" \
    >"$tmp/linkable_for_binding_forms.ir" 2>&1 || :
grep -q 'trait.method "Iterable", "iter".*@__trait_req_Iterable_iter' \
    "$tmp/linkable_for_binding_forms.ir"
grep -q 'trait.method "Iterator", "next".*@__trait_req_Iterator_next' \
    "$tmp/linkable_for_binding_forms.ir"
grep -q 'trait.method "Iterator", "cleanup".*@__trait_req_Iterator_cleanup' \
    "$tmp/linkable_for_binding_forms.ir"
grep -q 'call.output .* 0' "$tmp/linkable_for_binding_forms.ir"
if grep -q '^error\[' "$tmp/linkable_for_binding_forms.ir"; then
    cat "$tmp/linkable_for_binding_forms.ir"
    echo 'linkable iteration produced an IR diagnostic' >&2
    exit 1
fi

if "$compiler" preprocess "$program_dir/for_binding_type_mismatch.n00b" \
    >"$tmp/for_binding_type_mismatch.out" 2>&1; then
    echo 'typed loop binding unexpectedly accepted the wrong item type' >&2
    exit 1
fi
grep -q 'loop bindings do not match' \
    "$tmp/for_binding_type_mismatch.out"

"$compiler" lower --dump-ir \
    "$program_dir/for_cleanup_edges.n00b" \
    >"$tmp/for_cleanup_edges.ir" \
    2>"$tmp/for_cleanup_edges.err" || {
    cat "$tmp/for_cleanup_edges.err"
    exit 1
}
count_cleanup_calls() {
    awk -v function_name="$1" '
        $0 ~ "^  func @" function_name "\\(" { active = 1; next }
        active && /^  func @/ { exit }
        active && /call @cleanup/ { count++ }
        END { print count + 0 }
    ' "$tmp/for_cleanup_edges.ir"
}
test "$(count_cleanup_calls break_loop)" -eq 2
test "$(count_cleanup_calls continue_loop)" -eq 1
test "$(count_cleanup_calls return_loop)" -eq 2
test "$(count_cleanup_calls body_failure)" -eq 2
test "$(count_cleanup_calls next_failure)" -eq 2
test "$(count_cleanup_calls iter_failure)" -eq 1
test "$(count_cleanup_calls nested_break)" -eq 4
test "$(count_cleanup_calls nested_continue)" -eq 3
grep -q 'call.output .* 0' "$tmp/for_cleanup_edges.ir"
grep -q '= consume .*__n00bb_iteration_cursor_' \
    "$tmp/for_cleanup_edges.ir"
grep -q '!ir.call_signature((consume string) -> nil fail: nil)' \
    "$tmp/for_cleanup_edges.ir"
assert_call_order() {
    awk -v function_name="$1" -v first="$2" -v second="$3" '
        $0 ~ "^  func @" function_name "\\(" { active = 1; next }
        active && /^  func @/ { exit }
        active && !first_line && $0 ~ first { first_line = NR }
        active && !second_line && $0 ~ second { second_line = NR }
        END { exit !(first_line && second_line && first_line < second_line) }
    ' "$tmp/for_cleanup_edges.ir"
}
assert_call_order context_inside_loop \
    '__n00bb_context_LoopRecorder_exit' 'call @cleanup'
assert_call_order loop_inside_context \
    'call @cleanup' '__n00bb_context_LoopRecorder_exit'
awk '
    /^  func @context_inside_loop_failure\(/ { function_active = 1; next }
    function_active && /^  func @/ { exit 1 }
    function_active && /^    block __n00bb_unwrap_error_/ {
        error_block = 1
        next
    }
    error_block && /^    block / { exit !(ensure_line && cleanup_line) }
    error_block && !ensure_line && /__n00bb_context_LoopRecorder_ensure/ {
        ensure_line = NR
    }
    error_block && !cleanup_line && /call @cleanup/ {
        cleanup_line = NR
        exit !(ensure_line < cleanup_line)
    }
    END { if (!cleanup_line) exit 1 }
' "$tmp/for_cleanup_edges.ir"

"$compiler" lower --dump-ir \
    "$program_dir/addressable_arguments.n00b" \
    >"$tmp/addressable_arguments.ir" 2>&1 || {
    cat "$tmp/addressable_arguments.ir"
    exit 1
}
grep -q 'func @use_reference(%value: ref\[int\]' \
    "$tmp/addressable_arguments.ir"
grep -q 'ref\[int\] = field .* "value"' \
    "$tmp/addressable_arguments.ir"
grep -q 'ref\[int\] = index ' "$tmp/addressable_arguments.ir"
test "$(grep -c 'call @__write' "$tmp/addressable_arguments.ir")" -eq 2
test "$(grep -c 'call @__read' "$tmp/addressable_arguments.ir")" -eq 1
test "$(grep -c 'call.output' "$tmp/addressable_arguments.ir")" -eq 4

if "$compiler" lower \
    "$program_dir/addressable_argument_temporary.n00b" \
    >"$tmp/addressable_argument_temporary.out" 2>&1; then
    echo 'a temporary unexpectedly satisfied an out parameter' >&2
    exit 1
fi
grep -q 'out and modify arguments must be writable storage locations' \
    "$tmp/addressable_argument_temporary.out"

if "$compiler" lower \
    "$program_dir/addressable_argument_immutable.n00b" \
    >"$tmp/addressable_argument_immutable.out" 2>&1; then
    echo 'immutable storage unexpectedly satisfied an out parameter' >&2
    exit 1
fi
grep -q 'out and modify arguments must be writable storage locations' \
    "$tmp/addressable_argument_immutable.out"

"$compiler" lower --dump-symbols --dump-ir \
    "$program_dir/modify_variadic.n00b" \
    >"$tmp/modify_variadic.out" 2>&1 || {
    cat "$tmp/modify_variadic.out"
    exit 1
}
grep -q 'items: parameter, mutable, type=Array\[Ref\[int\]\].*variadic' \
    "$tmp/modify_variadic.out"
grep -q 'func @touch(\*%items: ref\[int\])' \
    "$tmp/modify_variadic.out"
grep -q 'array\[ref\[int\]\] = rest.collect %items' \
    "$tmp/modify_variadic.out"
grep -q 'call @touch, %__n00bb_local_address_.*%__n00bb_field_address_' \
    "$tmp/modify_variadic.out"
grep -q 'call @touch !' "$tmp/modify_variadic.out"

if "$compiler" lower \
    "$program_dir/modify_variadic_temporary.n00b" \
    >"$tmp/modify_variadic_temporary.out" 2>&1; then
    echo 'a temporary unexpectedly satisfied a modify variadic parameter' >&2
    exit 1
fi
grep -q 'modify variadic arguments must be writable storage locations' \
    "$tmp/modify_variadic_temporary.out"

"$compiler" lower --dump-ir \
    "$program_dir/constructor_addressable_argument.n00b" \
    >"$tmp/constructor_addressable_argument.ir" 2>&1 || {
    cat "$tmp/constructor_addressable_argument.ir"
    exit 1
}
grep -q 'call @__init, fresh .*call_signature((inout .*Built, inout int)' \
    "$tmp/constructor_addressable_argument.ir"
grep -q 'call.output .* 1' "$tmp/constructor_addressable_argument.ir"
grep -q 'store %__n00bb_field_address_.*%__n00bb_copyout_' \
    "$tmp/constructor_addressable_argument.ir"

"$compiler" lower --dump-ir \
    "$program_dir/callback_reference.n00b" \
    >"$tmp/callback_reference.ir" 2>&1 || {
    cat "$tmp/callback_reference.ir"
    exit 1
}
grep -q 'local .*: ref\[(int) -> int fail: nil\]' \
    "$tmp/callback_reference.ir"
grep -q 'copy @increment' "$tmp/callback_reference.ir"
test "$(grep -c 'call %.*%value.*call_signature((int) -> int fail: nil)' \
    "$tmp/callback_reference.ir")" -eq 2

"$compiler" lower --dump-ir \
    "$program_dir/init_existing.n00b" \
    >"$tmp/init_existing.ir" 2>&1 || {
    cat "$tmp/init_existing.ir"
    exit 1
}
grep -q 'call @__init, %__n00bb_.*, %value.*call_signature((inout .*Initialized, int)' \
    "$tmp/init_existing.ir"
grep -q 'call.output .* 0' "$tmp/init_existing.ir"
grep -q 'store %__n00bb_storage_.*%__n00bb_copyout_' \
    "$tmp/init_existing.ir"
if grep 'call @__init' "$tmp/init_existing.ir" | grep -q 'fresh'; then
    cat "$tmp/init_existing.ir"
    echo 'init(existing, ...) unexpectedly used fresh constructor storage' >&2
    exit 1
fi

"$compiler" lower --dump-ir \
    "$program_dir/comptime_else.n00b" \
    >"$tmp/comptime_else.ir" 2>&1 || {
    cat "$tmp/comptime_else.ir"
    exit 1
}
awk '
    /static func @__n00bb_comptime_init/ { phase = "comptime"; next }
    /static func @__n00bb_runtime_init/ { phase = "runtime"; next }
    phase == "comptime" && /call @__n00bb_comptime_compile_branch/ {
        compile_call = 1
    }
    phase == "comptime" && /call @__n00bb_comptime_runtime_branch/ {
        exit 1
    }
    phase == "runtime" && /call @runtime_branch/ { runtime_call = 1 }
    phase == "runtime" && /call @compile_branch/ { exit 1 }
    END { exit !(compile_call && runtime_call) }
' "$tmp/comptime_else.ir"

"$compiler" lower --dump-ir --dump-control-flow --dump-link-unit \
    "$program_dir/top_level_phase_order.n00b" \
    >"$tmp/top_level_phase_order.ir" 2>&1 || {
    cat "$tmp/top_level_phase_order.ir"
    exit 1
}
awk '
    /static func @__n00bb_comptime_init/ { phase = "comptime"; next }
    /static func @__n00bb_runtime_init/ { phase = "runtime"; next }
    phase == "comptime" && /call @__n00bb_comptime_first/ { compile_first = NR }
    phase == "comptime" && /call @__n00bb_comptime_compile_only/ { compile_middle = NR }
    phase == "comptime" && /call @__n00bb_comptime_last/ { compile_last = NR }
    phase == "runtime" && /call @first/ { runtime_first = NR }
    phase == "runtime" && /call @runtime_only/ { runtime_middle = NR }
    phase == "runtime" && /call @last/ { runtime_last = NR }
    END {
        ordered_compile = compile_first < compile_middle && compile_middle < compile_last
        ordered_runtime = runtime_first < runtime_middle && runtime_middle < runtime_last
        exit !(ordered_compile && ordered_runtime)
    }
' "$tmp/top_level_phase_order.ir"
for reachable in \
    '__n00bb_comptime_init' \
    '__n00bb_comptime_first' \
    '__n00bb_comptime_compile_only' \
    '__n00bb_comptime_last'; do
    grep -q "phase-reachable comptime @top_level_phase_order\\\$comptime.$reachable" \
        "$tmp/top_level_phase_order.ir"
done
for reachable in '__n00bb_runtime_init' first runtime_only last; do
    grep -q "phase-reachable runtime @top_level_phase_order.$reachable" \
        "$tmp/top_level_phase_order.ir"
done
! grep -q 'phase-reachable comptime .*runtime_only' \
    "$tmp/top_level_phase_order.ir"
! grep -q 'phase-reachable runtime .*compile_only' \
    "$tmp/top_level_phase_order.ir"
grep -q 'edge requires #1 -> #[0-9][0-9]*' \
    "$tmp/top_level_phase_order.ir"

"$compiler" parse "$script_dir/programs/comment_regions.n00b" \
    >"$tmp/comment_regions.out" 2>"$tmp/comment_regions.err" || {
    cat "$tmp/comment_regions.err"
    cat "$tmp/comment_regions.out"
    exit 1
}
if "$compiler" parse "$script_dir/programs/comment_region_unmatched.n00b" \
    >"$tmp/comment_region_unmatched.out" \
    2>"$tmp/comment_region_unmatched.err"; then
    echo 'unmatched comment-region close unexpectedly parsed' >&2
    exit 1
fi
grep -q 'unmatched comment-region close' \
    "$tmp/comment_region_unmatched.err"
if "$compiler" parse "$script_dir/programs/comment_region_not_standalone.n00b" \
    >"$tmp/comment_region_not_standalone.out" \
    2>"$tmp/comment_region_not_standalone.err"; then
    echo 'non-standalone comment-region anchor unexpectedly parsed' >&2
    exit 1
fi
grep -q 'region anchor must be the only non-whitespace text on its line' \
    "$tmp/comment_region_not_standalone.err"

"$compiler" lower --dump-ir "$script_dir/programs/flagset_operators.n00b" \
    >"$tmp/flagset_operators.ir" 2>"$tmp/flagset_operators.err" || {
    cat "$tmp/flagset_operators.err"
    exit 1
}
for operation in union difference intersection xor complement is_empty; do
    grep -q "= bitset.$operation" "$tmp/flagset_operators.ir"
done
grep -q 'bitset.const "nil", 0' "$tmp/flagset_operators.ir"
grep -q 'N00BB-FLAGSET-OR' "$tmp/flagset_operators.err"

"$compiler" lower --dump-ir "$script_dir/programs/flagset_fields.n00b" \
    >"$tmp/flagset_fields.ir" 2>"$tmp/flagset_fields.err" || {
    cat "$tmp/flagset_fields.err"
    exit 1
}
grep -q 'bitset.contains' "$tmp/flagset_fields.ir"
grep -q 'bitset.set' "$tmp/flagset_fields.ir"
grep -q 'bitset.value trap' "$tmp/flagset_fields.ir"
grep -q 'bitset.from_value trap' "$tmp/flagset_fields.ir"
grep -q 'N00BB-FLAGSET-COMPOSITE-WRITE' "$tmp/flagset_fields.err"

for invalid in flagset_duplicate_mask flagset_forward_reference \
    flagset_too_many; do
    if "$compiler" lower "$script_dir/programs/$invalid.n00b" \
        >"$tmp/$invalid.out" 2>"$tmp/$invalid.err"; then
        echo "$invalid unexpectedly compiled" >&2
        exit 1
    fi
    grep -q 'invalid flagset declaration' "$tmp/$invalid.err"
done

N00B_PATH="$interface_programs" "$compiler" library \
    --emit-interface "$tmp/flagset_archive.n00bi" flagset_archive_dep.facade \
    >"$tmp/flagset_archive.out" 2>"$tmp/flagset_archive.err" || {
    cat "$tmp/flagset_archive.err"
    exit 1
}
"$compiler" lower --link-interface "$tmp/flagset_archive.n00bi" --dump-ir \
    "$interface_programs/flagset_archive_user.n00b" \
    >"$tmp/flagset_archive_user.ir" \
    2>"$tmp/flagset_archive_user.err" || {
    cat "$tmp/flagset_archive_user.err"
    exit 1
}
grep -q 'bitset.const "read_write", 3' "$tmp/flagset_archive_user.ir"
grep -q 'bitset.contains' "$tmp/flagset_archive_user.ir"

package_programs="$script_dir/programs/packages"
N00B_PATH="$package_programs" "$compiler" lower --dump-ir \
    "$package_programs/nested_selected_consumer.n00b" \
    >"$tmp/package_nested_selected.out" \
    2>"$tmp/package_nested_selected.err" || {
    cat "$tmp/package_nested_selected.err"
    exit 1
}
grep -q 'const global @epsilon.impl.answer: int = 23' \
    "$tmp/package_nested_selected.out"

N00B_PATH="$package_programs" "$compiler" lower --dump-ir \
    --dump-control-flow "$package_programs/subpackage_consumer.n00b" \
    >"$tmp/package_subpackages.out" \
    2>"$tmp/package_subpackages.err" || {
    cat "$tmp/package_subpackages.err"
    exit 1
}
for value in root tool deep; do
    grep -q "copy @zeta.impl.$value" \
        "$tmp/package_subpackages.out"
done
grep -q 'copy @zeta.facade.marker' "$tmp/package_subpackages.out"
test "$(grep -c 'function-entry phase="comptime" function=zeta.facade\$comptime.__n00bb_comptime_init' \
    "$tmp/package_subpackages.out")" -eq 1
test "$(grep -c 'function-entry phase="runtime" function=zeta.facade.__n00bb_runtime_init' \
    "$tmp/package_subpackages.out")" -eq 1

N00B_PATH="$package_programs" "$compiler" library \
    --emit-interface "$tmp/package_interfaces.n00bi" \
    delta.facade beta.facade gamma.facade zeta.facade \
    >"$tmp/package_interfaces.out" \
    2>"$tmp/package_interfaces.err" || {
    cat "$tmp/package_interfaces.err"
    exit 1
}
for exported in delta.selected beta.read gamma.renamed \
    zeta.value zeta.tools.value zeta.tools.deep.value; do
    strings "$tmp/package_interfaces.n00bi" | grep -q "$exported"
done
! strings "$tmp/package_interfaces.n00bi" | grep -q 'delta.rejected'

"$compiler" lower --link-interface "$tmp/package_interfaces.n00bi" \
    --dump-ir --dump-link-unit \
    "$package_programs/selected_export_consumer.n00b" \
    >"$tmp/package_selected_archive.out" \
    2>"$tmp/package_selected_archive.err" || {
    cat "$tmp/package_selected_archive.err"
    exit 1
}
grep -q 'copy @delta.selected' "$tmp/package_selected_archive.out"
grep -q 'resolved selected_export_consumer -> delta.impl:0:selected' \
    "$tmp/package_selected_archive.out"
! grep -q 'delta.impl.rejected' "$tmp/package_selected_archive.out"

"$compiler" lower --link-interface "$tmp/package_interfaces.n00bi" \
    --dump-ir --dump-link-unit \
    "$package_programs/module_export_consumer.n00b" \
    >"$tmp/package_module_archive.out" \
    2>"$tmp/package_module_archive.err" || {
    cat "$tmp/package_module_archive.err"
    exit 1
}
grep -q 'call @read' "$tmp/package_module_archive.out"
grep -q 'resolved module_export_consumer -> beta.impl:1:read' \
    "$tmp/package_module_archive.out"

"$compiler" lower --link-interface "$tmp/package_interfaces.n00bi" \
    --dump-ir --dump-link-unit "$package_programs/rename_consumer.n00b" \
    >"$tmp/package_rename_archive.out" \
    2>"$tmp/package_rename_archive.err" || {
    cat "$tmp/package_rename_archive.err"
    exit 1
}
grep -q 'call @renamed' "$tmp/package_rename_archive.out"
grep -q 'resolved rename_consumer -> gamma.impl:1:original' \
    "$tmp/package_rename_archive.out"

"$compiler" lower --link-interface "$tmp/package_interfaces.n00bi" \
    --dump-ir --dump-control-flow --dump-link-unit \
    "$package_programs/subpackage_consumer.n00b" \
    >"$tmp/package_subpackages_archive.out" \
    2>"$tmp/package_subpackages_archive.err" || {
    cat "$tmp/package_subpackages_archive.err"
    exit 1
}
grep -q 'copy @zeta.value' "$tmp/package_subpackages_archive.out"
grep -q 'copy @zeta.tools.value' "$tmp/package_subpackages_archive.out"
grep -q 'copy @zeta.tools.deep.value' \
    "$tmp/package_subpackages_archive.out"
grep -q 'copy @zeta.marker' "$tmp/package_subpackages_archive.out"
for value in root tool deep; do
    grep -q "resolved subpackage_consumer -> zeta.impl:0:$value" \
        "$tmp/package_subpackages_archive.out"
done

package_negative="$script_dir/programs/package_negative"
for case_spec in \
    'invalid_body.facade:N00BB-M0010' \
    'invalid_block.facade:N00BB-M0010' \
    'export_outside:N00BB-M0003' \
    'module_alias.facade:N00BB-M0007' \
    'duplicate_destination.facade:N00BB-M0006' \
    'wrong_owner.facade:N00BB-M0004' \
    'interface_collision.facade:N00BB-M0005' \
    'private_export.facade:N00BB-M0008' \
    'subpackage_collision.facade:N00BB-M0006' \
    'duplicate_declaration.facade:N00BB-M0001'; do
    case_module=${case_spec%%:*}
    diagnostic=${case_spec##*:}
    case_name=$(printf '%s' "$case_module" | tr '.' '_')
    if N00B_PATH="$package_negative" "$compiler" preprocess \
        "$case_module" >"$tmp/$case_name.out" \
        2>"$tmp/$case_name.err"; then
        echo "$case_module unexpectedly passed package validation" >&2
        exit 1
    fi
    grep -q "$diagnostic" "$tmp/$case_name.err"
done

callback_programs="$script_dir/programs"
"$compiler" lower --dump-ir "$callback_programs/callback_contracts.n00b" \
    >"$tmp/callback_contracts.out" 2>"$tmp/callback_contracts.err" || {
    cat "$tmp/callback_contracts.err"
    exit 1
}
grep -q 'callback.create @setup' "$tmp/callback_contracts.out"
grep -q 'callback.create %' "$tmp/callback_contracts.out"
grep -q 'callback.invoke' "$tmp/callback_contracts.out"
grep -q 'retain.ref' "$tmp/callback_contracts.out"

"$compiler" lower --dump-ir \
    "$callback_programs/callback_setup_defaults.n00b" \
    >"$tmp/callback_setup_defaults.out" \
    2>"$tmp/callback_setup_defaults.err" || {
    cat "$tmp/callback_setup_defaults.err"
    exit 1
}
grep -q 'callback.create @setup' "$tmp/callback_setup_defaults.out"
grep -q 'priority = ' "$tmp/callback_setup_defaults.out"

"$compiler" lower --dump-ir \
    "$callback_programs/callback_generic_construction.n00b" \
    >"$tmp/callback_generic.out" 2>"$tmp/callback_generic.err" || {
    cat "$tmp/callback_generic.err"
    exit 1
}
grep -q 'callback.create @setup' "$tmp/callback_generic.out"
grep -q 'ir.call_signature((string) -> bool fail: string)' \
    "$tmp/callback_generic.out"

N00B_PATH="$interface_programs" "$compiler" lower --dump-ir \
    "$interface_programs/callback_imported_setup_user.n00b" \
    >"$tmp/callback_imported.out" 2>"$tmp/callback_imported.err" || {
    cat "$tmp/callback_imported.err"
    exit 1
}
grep -q 'callback.create @setup' "$tmp/callback_imported.out"

N00B_PATH="$interface_programs" "$compiler" library \
    --emit-interface "$tmp/callback_archive.n00bi" \
    callback_archive_dep.facade \
    >"$tmp/callback_archive_library.out" \
    2>"$tmp/callback_archive_library.err" || {
    cat "$tmp/callback_archive_library.err"
    exit 1
}
"$compiler" lower --link-interface "$tmp/callback_archive.n00bi" \
    --dump-ir --dump-symbols \
    "$interface_programs/callback_archive_user.n00b" \
    >"$tmp/callback_archive_user.out" \
    2>"$tmp/callback_archive_user.err" || {
    cat "$tmp/callback_archive_user.err"
    exit 1
}
test "$(grep -c 'callback.create @setup' \
    "$tmp/callback_archive_user.out")" -eq 2
grep -q 'callback.create @setup.*priority = ' \
    "$tmp/callback_archive_user.out"
grep -q 'callback_archive_dep.impl.Archived_Callback = callback.create' \
    "$tmp/callback_archive_user.out"
grep -q 'callback_archive_dep.impl.Archived_Event = callback.create' \
    "$tmp/callback_archive_user.out"
grep -q 'callback_archive_dep.impl.Archived_Transform = callback.create' \
    "$tmp/callback_archive_user.out"
grep -q 'callback.invoke' "$tmp/callback_archive_user.out"
grep -q 'variant.tag ' "$tmp/callback_archive_user.out"
grep -q 'callback.create @setup.*!ir.alias_count(1)' \
    "$tmp/callback_archive_user.out"

for case_spec in \
    'callback_archive_plain_mismatch:N00BB-T0024' \
    'callback_archive_uninitialized:N00BB-T0027'; do
    case_name=${case_spec%%:*}
    diagnostic=${case_spec##*:}
    if "$compiler" lower --link-interface "$tmp/callback_archive.n00bi" \
        "$interface_programs/$case_name.n00b" \
        >"$tmp/$case_name.out" 2>"$tmp/$case_name.err"; then
        echo "$case_name unexpectedly passed archived callback validation" >&2
        exit 1
    fi
    grep -q "$diagnostic" "$tmp/$case_name.err"
done

for case_spec in \
    'callback_missing_role:N00BB-T0023' \
    'callback_wrong_state_mode:N00BB-T0023' \
    'callback_state_mismatch:N00BB-T0023' \
    'callback_signature_mismatch:N00BB-T0023' \
    'callback_missing_setup:N00BB-T0025' \
    'callback_private_setup:N00BB-T0025' \
    'callback_incompatible_setup:N00BB-T0025' \
    'callback_immutable_state:N00BB-T0026' \
    'callback_uninitialized_state:N00BB-T0027' \
    'callback_plain_function_mismatch:N00BB-T0002'; do
    case_name=${case_spec%%:*}
    diagnostic=${case_spec##*:}
    if "$compiler" lower "$callback_programs/$case_name.n00b" \
        >"$tmp/$case_name.out" 2>"$tmp/$case_name.err"; then
        echo "$case_name unexpectedly passed callback validation" >&2
        exit 1
    fi
    grep -q "$diagnostic" "$tmp/$case_name.err"
done

if N00B_PATH="$interface_programs" "$compiler" lower \
    "$interface_programs/callback_ambiguous_setup_user.n00b" \
    >"$tmp/callback_ambiguous.out" 2>"$tmp/callback_ambiguous.err"; then
    echo 'ambiguous callback setup unexpectedly resolved' >&2
    exit 1
fi
grep -q 'callback setup is ambiguous' "$tmp/callback_ambiguous.err"

"$compiler" lower --dump-symbols --dump-ir \
    "$callback_programs/callback_state_lifetime.n00b" \
    >"$tmp/callback_state_lifetime.out" \
    2>"$tmp/callback_state_lifetime.err" || {
    cat "$tmp/callback_state_lifetime.err"
    exit 1
}
test "$(grep -c 'callback.create @setup' \
    "$tmp/callback_state_lifetime.out")" -eq 1
grep -q 'callback.invoke' "$tmp/callback_state_lifetime.out"
grep -q 'variant.tag ' "$tmp/callback_state_lifetime.out"
grep -q 'callback.create .* !ir.alias_count(1)' \
    "$tmp/callback_state_lifetime.out"
grep -q '^    original: variable, mutable, .*aliases=[1-9]' \
    "$tmp/callback_state_lifetime.out"
grep -q '^    copied: variable, mutable, .*aliases=[1-9]' \
    "$tmp/callback_state_lifetime.out"
grep -q '^    state: variable, mutable, .*storage-duration=dynamic, .*may-escape' \
    "$tmp/callback_state_lifetime.out"
