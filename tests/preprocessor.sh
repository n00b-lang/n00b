set -eu

compiler="$1"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/parallel_first.n00b" <<'EOF'
func parallel_first() { return 1 }
EOF
cat >"$tmp/parallel_second.n00b" <<'EOF'
func parallel_second() { return 2 }
EOF
"$compiler" parse --jobs 4 "$tmp/parallel_first.n00b" \
    "$tmp/parallel_second.n00b" >"$tmp/parallel_roots.out"
grep -q 'parallel_first' "$tmp/parallel_roots.out"
grep -q 'parallel_second' "$tmp/parallel_roots.out"
if "$compiler" parse --jobs 129 "$tmp/parallel_first.n00b" \
    >"$tmp/parallel_bad_jobs.out" 2>&1; then
    echo 'out-of-range worker count unexpectedly succeeded' >&2
    exit 1
fi
grep -q -- '--jobs must be between 0 and 128' "$tmp/parallel_bad_jobs.out"

cat >"$tmp/lower_arithmetic.n00b" <<'EOF'
func add(left: int, right: int) -> int { return left + right }
EOF
"$compiler" lower "$tmp/lower_arithmetic.n00b" \
    >"$tmp/lower_arithmetic.ir" 2>"$tmp/lower_arithmetic.err" || {
    cat "$tmp/lower_arithmetic.err"
    exit 1
}
grep -q '@add' "$tmp/lower_arithmetic.ir"
grep -q '= add trap' "$tmp/lower_arithmetic.ir"
grep -q 'return %' "$tmp/lower_arithmetic.ir"

cat >"$tmp/spaced_index.n00b" <<'EOF'
func get_item(items) { return items [0] }
EOF
"$compiler" parse "$tmp/spaced_index.n00b" >"$tmp/spaced_index.tree"
grep -q 'postfix-expr' "$tmp/spaced_index.tree"
! grep -q '<tagged-lit>' "$tmp/spaced_index.tree"

cat >"$tmp/adjacent_prefix.n00b" <<'EOF'
const tagged = r"rich text"
EOF
"$compiler" parse "$tmp/adjacent_prefix.n00b" >"$tmp/adjacent_prefix.tree"
grep -q '<tagged-lit>' "$tmp/adjacent_prefix.tree"

cat >"$tmp/literal_shapes.n00b" <<'EOF'
func literal_shapes() {
    unresolved_list = []
    unresolved_braces = {}
    var empty_list: list[int] = []
    var empty_array: array[int] = []
    var empty_set: set[int] = {}
    var empty_dict: dict[string, int] = {}
}
EOF
"$compiler" preprocess --dump-symbols "$tmp/literal_shapes.n00b" \
    >"$tmp/literal_shapes.out"
grep -q 'Bracket_Literal' "$tmp/literal_shapes.out"
grep -q 'Brace_Literal' "$tmp/literal_shapes.out"

cat >"$tmp/wrong_literal_shape.n00b" <<'EOF'
func wrong_literal_shape() {
    var not_a_collection: int = {}
}
EOF
if "$compiler" preprocess "$tmp/wrong_literal_shape.n00b" \
    >"$tmp/wrong_literal_shape.out" 2>&1; then
    echo 'brace literal unexpectedly satisfied an unrelated scalar type' >&2
    exit 1
fi
grep -q 'Brace_Literal' "$tmp/wrong_literal_shape.out"

cat >"$tmp/index_load.n00b" <<'EOF'
trait Index_Load {
    func load(self: `T, index: int) -> string
}
func load(self: array[int], index: int) -> string { return "loaded" }
func get_item(items: array[int]) { return items [0] }
EOF
"$compiler" preprocess --dump-symbols "$tmp/index_load.n00b" \
    >"$tmp/index_load.out" 2>&1 || {
    cat "$tmp/index_load.out"
    exit 1
}
grep 'get_item: function' "$tmp/index_load.out" | grep -q 'type=(array\[int\]) -> string' || {
    cat "$tmp/index_load.out"
    echo 'index load result was not inferred from the Index_Load contract' >&2
    exit 1
}

cat >"$tmp/index_load_bad_result.n00b" <<'EOF'
trait Index_Load {
    func load(self: `T, index: int) -> string
}
func load(self: array[int], index: int) -> string { return "loaded" }
func get_item(items: array[int]) -> bool { return items [0] }
EOF
if "$compiler" preprocess "$tmp/index_load_bad_result.n00b" \
    >"$tmp/index_load_bad_result.out" 2>&1; then
    cat "$tmp/index_load_bad_result.out"
    echo 'index load with a mismatched result type unexpectedly passed' >&2
    exit 1
fi
grep -q 'N00BB-T0001' "$tmp/index_load_bad_result.out"

cat >"$tmp/index_store.n00b" <<'EOF'
trait Index_Store {
    func store(self: `T, index: int, item: string) -> int
}
func store(self: array[int], index: int, item: string) -> int { return 1 }
func set_item(items: array[int]) { items [0] = "stored" }
EOF
"$compiler" preprocess "$tmp/index_store.n00b" \
    >"$tmp/index_store.out" 2>&1 || {
    cat "$tmp/index_store.out"
    exit 1
}

cat >"$tmp/index_store_bad_value.n00b" <<'EOF'
trait Index_Store {
    func store(self: `T, index: int, item: string) -> int
}
func store(self: array[int], index: int, item: string) -> int { return 1 }
func set_item(items: array[int]) { items [0] = true }
EOF
if "$compiler" preprocess "$tmp/index_store_bad_value.n00b" \
    >"$tmp/index_store_bad_value.out" 2>&1; then
    cat "$tmp/index_store_bad_value.out"
    echo 'index store with a mismatched value type unexpectedly passed' >&2
    exit 1
fi
grep -q 'N00BB-T0003' "$tmp/index_store_bad_value.out"

cat >"$tmp/index_slice.n00b" <<'EOF'
trait Index_Slice {
    func slice(self: `Container, start_ix: int = 0, end_ix: int = -1) -> view[`Element]
}
func slice(self: array[int], start_ix: int = 0, end_ix: int = -1) -> view[int] {
    return self[:]
}
func full(items: array[int]) { return items[:] }
func bounded(items: array[int]) { return items[2:4] }
func no_start(items: array[int]) { return items[:4] }
func no_end(items: array[int]) { return items[2:] }
EOF
"$compiler" preprocess --dump-symbols "$tmp/index_slice.n00b" \
    >"$tmp/index_slice.out" 2>&1 || {
    cat "$tmp/index_slice.out"
    exit 1
}
for function in full bounded no_start no_end; do
    grep "$function: function" "$tmp/index_slice.out" | \
        grep -q -- '-> view\[int\]' || {
        cat "$tmp/index_slice.out"
        echo "$function did not infer the Index_Slice view result" >&2
        exit 1
    }
done

cat >"$tmp/index_slice_bad_bound.n00b" <<'EOF'
trait Index_Slice {
    func slice(self: `Container, start_ix: int = 0, end_ix: int = -1) -> view[`Element]
}
func slice(self: array[int], start_ix: int = 0, end_ix: int = -1) -> view[int] {
    return self[:]
}
func invalid(items: array[int]) { return items["wrong":] }
EOF
if "$compiler" preprocess "$tmp/index_slice_bad_bound.n00b" \
    >"$tmp/index_slice_bad_bound.out" 2>&1; then
    cat "$tmp/index_slice_bad_bound.out"
    echo 'slice indexing unexpectedly accepted a non-integer bound' >&2
    exit 1
fi
grep -q 'LANTERN-T0001' "$tmp/index_slice_bad_bound.out"

cat >"$tmp/ordering_trait.n00b" <<'EOF'
type Meter {
    amount: int
}
trait Comparable {
    func cmp(left: `T, right: `T) -> i:2
}
func cmp(left: Meter, right: Meter) -> i:2 { return 0 : i:2 }
func less(left: Meter, right: Meter) { return left < right }
func less_equal(left: Meter, right: Meter) { return left <= right }
func greater(left: Meter, right: Meter) { return left > right }
func greater_equal(left: Meter, right: Meter) { return left >= right }
EOF
"$compiler" preprocess --dump-symbols "$tmp/ordering_trait.n00b" \
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
        echo "$operation did not infer bool through Comparable.cmp" >&2
        exit 1
    }
done

cat >"$tmp/sized_integer_types.n00b" <<'EOF'
func widths(signed: i:8, unsigned: u:16, flags: bits:4) {}
func soft_keywords_remain_names(i: int, u: int, bits: int) { return i + u + bits }
EOF
"$compiler" preprocess --dump-symbols "$tmp/sized_integer_types.n00b" \
    >"$tmp/sized_integer_types.out" 2>&1 || {
    cat "$tmp/sized_integer_types.out"
    exit 1
}
grep -q 'widths: function' "$tmp/sized_integer_types.out"

cat >"$tmp/spaced_integer_type.n00b" <<'EOF'
func invalid(width: i : 8) {}
EOF
if "$compiler" parse "$tmp/spaced_integer_type.n00b" \
    >"$tmp/spaced_integer_type.out" 2>&1; then
    cat "$tmp/spaced_integer_type.out"
    echo 'spaced sized-integer prefix unexpectedly parsed' >&2
    exit 1
fi

cat >"$tmp/implicit_binding.n00b" <<'EOF'
implicit_value = 42
EOF
"$compiler" preprocess --dump-symbols "$tmp/implicit_binding.n00b" \
    >"$tmp/implicit_binding.out"
grep -q 'implicit_value: variable, mutable, type=' \
    "$tmp/implicit_binding.out"

cat >"$tmp/branches.n00b" <<'EOF'
const enabled = true
const numeric: uint = 1
public func visible(x: int, out y: bool, modify z: string) -> bool fail: string { return true }
public func identity(item: `a) -> `a { return item }
public func identity_call() { return identity(42) }
public func constrained(item: `a) -> `a where `a: Copy { return item }
public func inferred_identity(item) { return item }
public func calls_later() { return later() }
public func later() { return 42 }
when enabled and (2 * 3 == 6) {
    var selected = 1
    when numeric == 1 {
        var typed_selected = true
    } else {
        var typed_removed = true
    }
} else {
    var removed = 2
}
EOF
"$compiler" preprocess "$tmp/branches.n00b" >"$tmp/branches.out"
grep -q '"selected"' "$tmp/branches.out"
! grep -q '"removed"' "$tmp/branches.out"
grep -q '"typed_selected"' "$tmp/branches.out"
! grep -q '"typed_removed"' "$tmp/branches.out"

"$compiler" preprocess --dump-symbols "$tmp/branches.n00b" \
    >"$tmp/branches.symbols"
grep -q 'selected: variable, mutable' "$tmp/branches.symbols"
! grep -q 'removed:' "$tmp/branches.symbols"
grep -q 'enabled: variable, const' "$tmp/branches.symbols"
grep -q 'numeric: variable, const, type=' "$tmp/branches.symbols"
grep -q 'visible: function' "$tmp/branches.symbols"
grep -q 'identity: function, mutable, type=forall\[`a\] (`a) -> `a' \
    "$tmp/branches.symbols"
grep -q 'identity: function, mutable, .*uses=1' "$tmp/branches.symbols"
grep -q 'identity_call: function, mutable, type=() -> int' \
    "$tmp/branches.symbols" || {
    cat "$tmp/branches.symbols"
    exit 1
}
grep -q 'inferred_identity: function, mutable, type=forall\[`a\] (`a) -> `a' \
    "$tmp/branches.symbols"
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

cat >"$tmp/trait_signatures.n00b" <<'EOF'
public trait Combine {
    func combine(self: `T, other: `T) -> `T fail: string
}
EOF
"$compiler" preprocess --dump-symbols "$tmp/trait_signatures.n00b" \
    >"$tmp/trait_signatures.out"
grep 'combine: function, mutable, type=(`a, `a) -> `a' \
    "$tmp/trait_signatures.out" | grep -q 'error=string' || {
    cat "$tmp/trait_signatures.out"
    exit 1
}

cat >"$tmp/trait_dispatch.n00b" <<'EOF'
trait Render {
    func render(self: `T) -> string
}
func render(self: int) -> string { return "ok" }
func use_it(item: int) -> string { return item.render() }
EOF
"$compiler" preprocess "$tmp/trait_dispatch.n00b" \
    >"$tmp/trait_dispatch.out" 2>&1 || {
    cat "$tmp/trait_dispatch.out"
    exit 1
}

cat >"$tmp/generic_trait_dispatch.n00b" <<'EOF'
trait Render {
    func render(self: `T) -> string
}
func render(self: int) -> string { return "ok" }
func generic_render(item: `T) -> string where `T: Render {
    return item.render()
}
func call_generic_render() -> string { return generic_render(42) }
EOF
"$compiler" preprocess --dump-symbols \
    "$tmp/generic_trait_dispatch.n00b" \
    >"$tmp/generic_trait_dispatch.out" 2>&1 || {
    cat "$tmp/generic_trait_dispatch.out"
    echo 'constrained generic trait dispatch unexpectedly failed' >&2
    exit 1
}
grep 'generic_render: function' "$tmp/generic_trait_dispatch.out" \
    | grep -q 'Render'
grep -q 'call_generic_render: function, mutable, type=() -> string' \
    "$tmp/generic_trait_dispatch.out"

cat >"$tmp/trait_dispatch_missing.n00b" <<'EOF'
trait Render {
    func render(self: `T) -> string
}
func render(self: int) -> string { return "ok" }
func use_it(item: string) -> string { return item.render() }
EOF
if "$compiler" preprocess "$tmp/trait_dispatch_missing.n00b" \
    >"$tmp/trait_dispatch_missing.out" 2>&1; then
    cat "$tmp/trait_dispatch_missing.out"
    echo 'missing trait candidate unexpectedly passed' >&2
    exit 1
fi
grep -q 'N00BB-T0003' "$tmp/trait_dispatch_missing.out"

cat >"$tmp/trait_dispatch_wrong_failure.n00b" <<'EOF'
trait ReadFailure {
    func read_failure(self: `T) -> string fail: string
}
func read_failure(self: int) -> string fail: int { return "value" }
func wrong_failure(item: int) -> string { return item.read_failure() }
EOF
if "$compiler" preprocess "$tmp/trait_dispatch_wrong_failure.n00b" \
    >"$tmp/trait_dispatch_wrong_failure.out" 2>&1; then
    cat "$tmp/trait_dispatch_wrong_failure.out"
    echo 'trait method with a mismatched failure type unexpectedly passed' >&2
    exit 1
fi
grep -q 'N00BB-T0003' "$tmp/trait_dispatch_wrong_failure.out"

cat >"$tmp/trait_dispatch_bad_argument.n00b" <<'EOF'
trait Combine {
    func combine(self: `T, other: `T) -> `T
}
func combine(self: int, other: int) -> int { return other }
func wrong_argument(item: int, other: string) -> int {
    return item.combine(other)
}
EOF
if "$compiler" preprocess "$tmp/trait_dispatch_bad_argument.n00b" \
    >"$tmp/trait_dispatch_bad_argument.out" 2>&1; then
    cat "$tmp/trait_dispatch_bad_argument.out"
    echo 'trait method with a mismatched argument unexpectedly passed' >&2
    exit 1
fi
grep -q 'N00BB-T0003' "$tmp/trait_dispatch_bad_argument.out"

cat >"$tmp/trait_dispatch_valid_argument.n00b" <<'EOF'
trait Combine {
    func combine(self: `T, other: `T) -> `T
}
func combine(self: int, other: int) -> int { return other }
func valid_argument(item: int, other: int) -> int {
    return item.combine(other)
}
EOF
"$compiler" preprocess "$tmp/trait_dispatch_valid_argument.n00b" \
    >"$tmp/trait_dispatch_valid_argument.out" 2>&1 || {
    cat "$tmp/trait_dispatch_valid_argument.out"
    exit 1
}

cat >"$tmp/direct_call_is_not_trait_dispatch.n00b" <<'EOF'
trait DirectCall {
    func direct_call(self: `T) -> string
}
func direct_call(self: int) -> string { return "ok" }
func valid_member(item: int) -> string { return item.direct_call() }
func wrong_direct_argument(item: string) -> string {
    return direct_call(item)
}
EOF
if "$compiler" preprocess "$tmp/direct_call_is_not_trait_dispatch.n00b" \
    >"$tmp/direct_call_is_not_trait_dispatch.out" 2>&1; then
    cat "$tmp/direct_call_is_not_trait_dispatch.out"
    echo 'ordinary function call unexpectedly used trait dispatch' >&2
    exit 1
fi
grep -q 'N00BB-T0002' "$tmp/direct_call_is_not_trait_dispatch.out"
! grep -q 'N00BB-T0003' "$tmp/direct_call_is_not_trait_dispatch.out"

cat >"$tmp/trait_dispatch_receiver_offset.n00b" <<'EOF'
trait Describe {
    func describe(self: `T, label: string) -> `T
}
func describe(self: int, label: string) -> int { return self }
func valid_receiver_and_argument(item: int, label: string) -> int {
    return item.describe(label)
}
EOF
"$compiler" preprocess "$tmp/trait_dispatch_receiver_offset.n00b" \
    >"$tmp/trait_dispatch_receiver_offset.out" 2>&1 || {
    cat "$tmp/trait_dispatch_receiver_offset.out"
    exit 1
}

cat >"$tmp/render_contract.n00b" <<'EOF'
public trait Render {
    func render(self: `T) -> string
}
EOF
cat >"$tmp/render_plugin.n00b" <<'EOF'
import render_contract
public func render(self: int) -> string { return "plugin" }
EOF
cat >"$tmp/other_render_contract.n00b" <<'EOF'
public trait Render {
    func draw(self: `T) -> string
}
EOF
cat >"$tmp/render_consumer.n00b" <<'EOF'
import render_contract
import render_plugin
func use_plugin(item: int) -> string { return item.render() }
EOF
N00B_PATH="$tmp" "$compiler" preprocess "$tmp/render_consumer.n00b" \
    >"$tmp/render_graph.out" 2>&1 || {
    cat "$tmp/render_graph.out"
    exit 1
}

cat >"$tmp/generic_render_consumer.n00b" <<'EOF'
import render_contract
import render_plugin
func generic_render(item: `T) -> string where `T: Render {
    return item.render()
}
func call_generic_render() -> string { return generic_render(1) }
EOF
N00B_PATH="$tmp" "$compiler" preprocess --dump-symbols \
    "$tmp/generic_render_consumer.n00b" \
    >"$tmp/generic_render_consumer.out" 2>&1 || {
    cat "$tmp/generic_render_consumer.out"
    exit 1
}
grep 'generic_render: function' "$tmp/generic_render_consumer.out" \
    | grep -q 'render_contract.Render' || {
    cat "$tmp/generic_render_consumer.out"
    echo 'imported trait constraint did not resolve to its defining module' >&2
    exit 1
}
grep -q 'call_generic_render: function, mutable, type=() -> string' \
    "$tmp/generic_render_consumer.out"

cat >"$tmp/qualified_render_consumer.n00b" <<'EOF'
import render_contract
import render_plugin
import other_render_contract
func generic_render(item: `T) -> string where `T: render_contract.Render {
    return item.render()
}
func call_generic_render() -> string { return generic_render(1) }
EOF
N00B_PATH="$tmp" "$compiler" preprocess \
    "$tmp/qualified_render_consumer.n00b" \
    >"$tmp/qualified_render_consumer.out" 2>&1 || {
    cat "$tmp/qualified_render_consumer.out"
    exit 1
}

cat >"$tmp/ambiguous_render_consumer.n00b" <<'EOF'
import render_contract
import render_plugin
import other_render_contract
func generic_render(item: `T) -> string where `T: Render {
    return item.render()
}
func call_generic_render() -> string { return generic_render(1) }
EOF
if N00B_PATH="$tmp" "$compiler" preprocess \
    "$tmp/ambiguous_render_consumer.n00b" \
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

cat >"$tmp/signature_mismatch.n00b" <<'EOF'
func wrong() -> string { return 1 }
EOF
if "$compiler" preprocess "$tmp/signature_mismatch.n00b" \
    >"$tmp/signature_mismatch.out" 2>&1; then
    cat "$tmp/signature_mismatch.out"
    echo 'mismatched function signature unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'N00BB-T0001' "$tmp/signature_mismatch.out"
grep -q 'signature_mismatch.n00b:1:1' "$tmp/signature_mismatch.out"

cat >"$tmp/call_argument_mismatch.n00b" <<'EOF'
func needs_text(item: string) -> string { return item }
func wrong_argument() { return needs_text(1) }
func wrong_variable_argument(item: int) -> string { return needs_text(item) }
EOF
if "$compiler" preprocess "$tmp/call_argument_mismatch.n00b" \
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

cat >"$tmp/recursive_calls.n00b" <<'EOF'
func first() {
    if true { return second() } else { return 0 }
}
func second() {
    if true { return 1 } else { return first() }
}
EOF
"$compiler" preprocess --dump-symbols "$tmp/recursive_calls.n00b" \
    >"$tmp/recursive_calls.out"
grep -q 'first: function, mutable, type=() -> int' \
    "$tmp/recursive_calls.out"
grep -q 'second: function, mutable, type=() -> int' \
    "$tmp/recursive_calls.out"
enabled_line=$(grep -n 'enabled: variable' "$tmp/branches.symbols" | cut -d: -f1)
numeric_line=$(grep -n 'numeric: variable' "$tmp/branches.symbols" | cut -d: -f1)
[ "$enabled_line" -lt "$numeric_line" ]

cat >"$tmp/non_bool_condition.n00b" <<'EOF'
when 1 {
    var impossible = true
}
EOF
if "$compiler" preprocess "$tmp/non_bool_condition.n00b" \
    >"$tmp/non_bool_condition.out" 2>&1; then
    echo 'non-bool when condition unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'when condition must have static type bool' \
    "$tmp/non_bool_condition.out"

cat >"$tmp/skipped_call.n00b" <<'EOF'
when true or ordinary_function() {
    var selected = true
}
EOF
if "$compiler" preprocess "$tmp/skipped_call.n00b" \
    >"$tmp/skipped_call.out" 2>&1; then
    echo 'short-circuited non-builtin call unexpectedly passed validation' >&2
    exit 1
fi
grep -q 'only processor-time builtins may be called' \
    "$tmp/skipped_call.out"

cat >"$tmp/skipped_type_error.n00b" <<'EOF'
when true or 1 {
    var impossible = true
}
EOF
if "$compiler" preprocess "$tmp/skipped_type_error.n00b" \
    >"$tmp/skipped_type_error.out" 2>&1; then
    echo 'short-circuited non-bool operand unexpectedly passed type checking' >&2
    exit 1
fi
grep -q 'operator or operand type is not supported by the preprocessor' \
    "$tmp/skipped_type_error.out"

cat >"$tmp/skipped_ineligible_const.n00b" <<'EOF'
const character = 'x'
when true or character == "x" {
    var impossible = true
}
EOF
if "$compiler" preprocess "$tmp/skipped_ineligible_const.n00b" \
    >"$tmp/skipped_ineligible_const.out" 2>&1; then
    echo 'short-circuited ineligible const unexpectedly passed checking' >&2
    exit 1
fi
grep -q 'ineligible const variable' "$tmp/skipped_ineligible_const.out"

cat >"$tmp/false_branch.n00b" <<'EOF'
when false {
    var removed = true
} else {
    var selected = true
}
EOF
"$compiler" preprocess "$tmp/false_branch.n00b" >"$tmp/false_branch.out"
grep -q '"selected"' "$tmp/false_branch.out"
! grep -q '"removed"' "$tmp/false_branch.out"

cat >"$tmp/else_when.n00b" <<'EOF'
when false {
    var first_removed = true
} else when 1 == 1 {
    var middle_selected = true
} else {
    var final_removed = true
}
EOF
"$compiler" preprocess "$tmp/else_when.n00b" >"$tmp/else_when.out"
grep -q '"middle_selected"' "$tmp/else_when.out"
! grep -q '"first_removed"' "$tmp/else_when.out"
! grep -q '"final_removed"' "$tmp/else_when.out"

cat >"$tmp/else_only.n00b" <<'EOF'
when false {
    var first_removed = true
} else when false {
    var middle_removed = true
} else {
    var final_selected = true
}
EOF
"$compiler" preprocess "$tmp/else_only.n00b" >"$tmp/else_only.out"
grep -q '"final_selected"' "$tmp/else_only.out"
! grep -q '"first_removed"' "$tmp/else_only.out"
! grep -q '"middle_removed"' "$tmp/else_only.out"

cat >"$tmp/annotated_condition.n00b" <<'EOF'
when true : bool {
    var unexpected = true
}
EOF
if "$compiler" preprocess "$tmp/annotated_condition.n00b" \
    >"$tmp/annotated_condition.out" 2>&1; then
    echo 'annotated literal unexpectedly became an eligible condition' >&2
    exit 1
fi
grep -q "expression form .* is not allowed" \
    "$tmp/annotated_condition.out"

cat >"$tmp/modifier_condition.n00b" <<'EOF'
when preproc_env"N00BB_TEST_ENV" == "present" {
    var unexpected = true
}
EOF
if "$compiler" preprocess "$tmp/modifier_condition.n00b" \
    >"$tmp/modifier_condition.out" 2>&1; then
    echo 'literal-modifier builtin unexpectedly became a function call' >&2
    exit 1
fi
grep -q 'literal modifiers are not allowed' "$tmp/modifier_condition.out"

cat >"$tmp/builtin_calls.n00b" <<'EOF'
when preproc_value("FEATURE") == true {
    var selected = preproc_env("N00BB_TEST_ENV")
}
EOF
N00BB_TEST_ENV=present "$compiler" preprocess \
    -D FEATURE=true "$tmp/builtin_calls.n00b" >"$tmp/builtin_calls.out"
grep -q '"selected"' "$tmp/builtin_calls.out"
grep -q '"present"' "$tmp/builtin_calls.out"

cat >"$tmp/i128_min.n00b" <<'EOF'
const lowest = -170141183460469231731687303715884105728
when lowest < 0 {
    var selected = true
} else {
    var removed = false
}
EOF
"$compiler" preprocess "$tmp/i128_min.n00b" >"$tmp/i128_min.out"
grep -q '"selected"' "$tmp/i128_min.out"
! grep -q '"removed"' "$tmp/i128_min.out"

cat >"$tmp/i128_remainder_overflow.n00b" <<'EOF'
const lowest = -170141183460469231731687303715884105728
when lowest % -1 == 0 {
    var unreachable = true
}
EOF
if "$compiler" preprocess "$tmp/i128_remainder_overflow.n00b" \
    >"$tmp/i128_remainder_overflow.out" 2>&1; then
    echo 'i128 remainder overflow unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'checked i128 arithmetic' "$tmp/i128_remainder_overflow.out"

cat >"$tmp/ineligible_consts.n00b" <<'EOF'
const character = 'x'
const mismatched: bool = 1
const too_wide = 170141183460469231731687303715884105728
when true {
    var selected = true
}
EOF
"$compiler" preprocess "$tmp/ineligible_consts.n00b" \
    >"$tmp/ineligible_consts.out"
grep -q '"selected"' "$tmp/ineligible_consts.out"

cat >"$tmp/ineligible_const_use.n00b" <<'EOF'
const character = 'x'
when character == "x" {
    var selected = true
}
EOF
if "$compiler" preprocess "$tmp/ineligible_const_use.n00b" \
    >"$tmp/ineligible_const_use.out" 2>&1; then
    echo 'ineligible character const unexpectedly became available' >&2
    exit 1
fi
grep -q 'ineligible const variable' "$tmp/ineligible_const_use.out"

cat >"$tmp/const_local.n00b" <<'EOF'
when true {
    const visible = true
    when visible {
        var selected = true
    }
}
EOF
"$compiler" preprocess "$tmp/const_local.n00b" \
    >"$tmp/const_local.out"
grep -q '"selected"' "$tmp/const_local.out"

cat >"$tmp/const_scope.n00b" <<'EOF'
when true {
    when true {
        const hidden = true
    }
    when hidden {
        var leaked = true
    }
}
EOF
if "$compiler" preprocess "$tmp/const_scope.n00b" \
    >"$tmp/const_scope.out" 2>&1; then
    echo 'block-local const unexpectedly escaped into a sibling scope' >&2
    exit 1
fi
grep -q 'ineligible const variable' "$tmp/const_scope.out"
! grep -q '"leaked"' "$tmp/const_scope.out"

cat >"$tmp/external.n00b" <<'EOF'
when preproc_value("FEATURE") == true {
    var selected = preproc_value("COUNT")
} else {
    var removed = 2
}
EOF
"$compiler" preprocess -D FEATURE=true -D COUNT=42 \
    "$tmp/external.n00b" >"$tmp/external.out"
grep -q '"selected"' "$tmp/external.out"
grep -q '"42"' "$tmp/external.out"
! grep -q '"removed"' "$tmp/external.out"

cat >"$tmp/external_other_types.n00b" <<'EOF'
when preproc_defines("TEXT") and preproc_value("TEXT") == "hello" and preproc_value("SCALE") == 1.5 {
    var text_value = preproc_value("TEXT")
    var float_value = preproc_value("SCALE")
} else {
    var missing_definition = true
}
EOF
"$compiler" preprocess -D 'TEXT="hello"' -D SCALE=1.5 \
    "$tmp/external_other_types.n00b" >"$tmp/external_other_types.out"
grep -q '"text_value"' "$tmp/external_other_types.out"
grep -q '"hello"' "$tmp/external_other_types.out"
grep -q '"1.5"' "$tmp/external_other_types.out"
! grep -q '"missing_definition"' "$tmp/external_other_types.out"

cat >"$tmp/short_circuit.n00b" <<'EOF'
when true or preproc_value("MISSING") == 1 {
    var short_circuit_selected = true
}
when false and preproc_value("ALSO_MISSING") == 1 {
    var second_short_circuit = true
}
when true or (1 / 0 == 0) {
    var skipped_arithmetic_selected = true
}
EOF
"$compiler" preprocess "$tmp/short_circuit.n00b" \
    >"$tmp/short_circuit.out"
grep -q '"short_circuit_selected"' "$tmp/short_circuit.out"
grep -q '"skipped_arithmetic_selected"' "$tmp/short_circuit.out"
! grep -q 'no externally supplied value' "$tmp/short_circuit.out"

cat >"$tmp/skipped_diagnostic.n00b" <<'EOF'
when true or compile_warning("must not run") {
    var impossible = true
}
EOF
if "$compiler" preprocess "$tmp/skipped_diagnostic.n00b" \
    >"$tmp/skipped_diagnostic.out" 2>&1; then
    echo 'void diagnostic call unexpectedly passed static type checking' >&2
    exit 1
fi
! grep -q 'must not run' "$tmp/skipped_diagnostic.out"

cat >"$tmp/environment.n00b" <<'EOF'
when preproc_env_defines("N00BB_TEST_ENV") and preproc_env("N00BB_TEST_ENV") == "present" {
    var environment_selected = true
}
EOF
N00BB_TEST_ENV=present "$compiler" preprocess "$tmp/environment.n00b" \
    >"$tmp/environment.out"
grep -q '"environment_selected"' "$tmp/environment.out"

cat >"$tmp/undefined_environment.n00b" <<'EOF'
when !preproc_env_defines("N00BB_UNSET_TEST") and preproc_env("N00BB_UNSET_TEST") == "" {
    var undefined_environment_selected = true
}
EOF
env -u N00BB_UNSET_TEST "$compiler" preprocess \
    "$tmp/undefined_environment.n00b" >"$tmp/undefined_environment.out"
grep -q '"undefined_environment_selected"' \
    "$tmp/undefined_environment.out"

cat >"$tmp/repeated_definitions.n00b" <<'EOF'
when preproc_value("LEVEL") == 2 {
    var last_definition_wins = true
}
EOF
"$compiler" preprocess -D LEVEL=1 -D LEVEL=2 \
    "$tmp/repeated_definitions.n00b" >"$tmp/repeated_definitions.out"
grep -q '"last_definition_wins"' "$tmp/repeated_definitions.out"

cat >"$tmp/out_of_range_definition.n00b" <<'EOF'
when preproc_value("TOO_BIG") > 0 {
    var impossible = true
}
EOF
if "$compiler" preprocess \
    -D TOO_BIG=170141183460469231731687303715884105728 \
    "$tmp/out_of_range_definition.n00b" \
    >"$tmp/out_of_range_definition.out" 2>&1; then
    echo 'out-of-range external integer unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'external integer value is outside the preprocessor i128 range' \
    "$tmp/out_of_range_definition.out"

cat >"$tmp/out_of_range_float_definition.n00b" <<'EOF'
when preproc_value("TOO_WIDE_FLOAT") > 1.0 {
    var impossible = true
}
EOF
if "$compiler" preprocess -D TOO_WIDE_FLOAT=1e999 \
    "$tmp/out_of_range_float_definition.n00b" \
    >"$tmp/out_of_range_float_definition.out" 2>&1; then
    echo 'out-of-range external float unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'external float value is outside the preprocessor f64 range' \
    "$tmp/out_of_range_float_definition.out"

cat >"$tmp/strings.n00b" <<'EOF'
when "a\n" == "a\x0a" {
    var selected = 1
    when "\xff" == "ÿ" {
        var unicode_selected = true
    } else {
        var unicode_removed = true
    }
} else {
    var removed = 2
}
EOF
"$compiler" preprocess "$tmp/strings.n00b" >"$tmp/strings.out"
grep -q '"selected"' "$tmp/strings.out"
! grep -q '"removed"' "$tmp/strings.out"
grep -q '"unicode_selected"' "$tmp/strings.out"
! grep -q '"unicode_removed"' "$tmp/strings.out"

cat >"$tmp/float_ops.n00b" <<'EOF'
when !false and (-1.5 < +0.0) and (1.5 + 2.5 == 4.0) and (2.0 != 3.0) {
    var float_selected = true
} else {
    var float_removed = true
}
when "alpha" != "beta" {
    var string_not_equal = true
}
EOF
"$compiler" preprocess "$tmp/float_ops.n00b" >"$tmp/float_ops.out"
grep -q '"float_selected"' "$tmp/float_ops.out"
! grep -q '"float_removed"' "$tmp/float_ops.out"
grep -q '"string_not_equal"' "$tmp/float_ops.out"

cat >"$tmp/integer_bit_ops.n00b" <<'EOF'
when (0xff & 0xf0) == 0xf0 and (0xf0 | 0x0f) == 0xff and (3 ^ 1) == 2 and (1 << 3) == 8 and (8 >> 2) == 2 and (5 % 2) == 1 {
    var integer_ops_selected = true
}
EOF
"$compiler" preprocess "$tmp/integer_bit_ops.n00b" \
    >"$tmp/integer_bit_ops.out"
grep -q '"integer_ops_selected"' "$tmp/integer_bit_ops.out"

cat >"$tmp/unsupported_operation.n00b" <<'EOF'
when "alpha" < "beta" {
    var impossible = true
}
EOF
if "$compiler" preprocess "$tmp/unsupported_operation.n00b" \
    >"$tmp/unsupported_operation.out" 2>&1; then
    echo 'unsupported string ordering unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'operator or operand type is not supported by the preprocessor' \
    "$tmp/unsupported_operation.out"

cat >"$tmp/diagnostic_condition.n00b" <<'EOF'
when compile_warning("not a value") {
    var impossible = true
}
EOF
if "$compiler" preprocess "$tmp/diagnostic_condition.n00b" \
    >"$tmp/diagnostic_condition.out" 2>&1; then
    echo 'diagnostic builtin unexpectedly supplied a when condition' >&2
    exit 1
fi
grep -q 'not a value' "$tmp/diagnostic_condition.out"
grep -q 'when condition must have static type bool' \
    "$tmp/diagnostic_condition.out"

cat >"$tmp/target_builtins.n00b" <<'EOF'
when (target_os() != "macos" or target_os_major() > 0) and target_alu() == 64 and target_arch() != "" {
    var selected = target_os()
} else {
    var unsupported = true
}
EOF
"$compiler" preprocess "$tmp/target_builtins.n00b" \
    >"$tmp/target_builtins.out"
grep -q '"selected"' "$tmp/target_builtins.out"
! grep -q '"unsupported"' "$tmp/target_builtins.out"

cat >"$tmp/implementation_builtins.n00b" <<'EOF'
when n00b_major() == 0 and n00b_minor() == 1 and n00b_patch() == 0 and n00b_impl() == "n00bb" and target_is_little_endian() {
    var implementation_selected = true
}
EOF
"$compiler" preprocess "$tmp/implementation_builtins.n00b" \
    >"$tmp/implementation_builtins.out"
grep -q '"implementation_selected"' "$tmp/implementation_builtins.out"

cat >"$tmp/missing.n00b" <<'EOF'
when preproc_value("MISSING") == true {
    var selected = 1
}
EOF
if "$compiler" preprocess "$tmp/missing.n00b" >"$tmp/missing.out" 2>&1; then
    echo 'missing preprocessor value unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'no externally supplied value' "$tmp/missing.out"

cat >"$tmp/warning.n00b" <<'EOF'
compile_warning("heads up")
EOF
"$compiler" preprocess "$tmp/warning.n00b" >"$tmp/warning.out" 2>&1
grep -q 'warning\[N00BB-PPW0001\]: line 1, column 1: heads up' \
    "$tmp/warning.out"

cat >"$tmp/nested_warnings.n00b" <<'EOF'
compile_warning("first") + compile_warning("second")
EOF
"$compiler" preprocess "$tmp/nested_warnings.n00b" \
    >"$tmp/nested_warnings.out" 2>&1
grep -q 'line 1, column 1: first' "$tmp/nested_warnings.out"
grep -q 'line 1, column 28: second' "$tmp/nested_warnings.out"
[ "$(grep -c 'N00BB-PPW0001' "$tmp/nested_warnings.out")" -eq 2 ]

cat >"$tmp/error.n00b" <<'EOF'
compile_error("stop here")
EOF
if "$compiler" preprocess "$tmp/error.n00b" >"$tmp/error.out" 2>&1; then
    echo 'compile_error unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'fatal\[N00BB-PP0002\]: line 1, column 1: stop here' \
    "$tmp/error.out"

cat >"$tmp/error_condition.n00b" <<'EOF'
when compile_error("stop condition") {
    var impossible = true
}
EOF
if "$compiler" preprocess "$tmp/error_condition.n00b" \
    >"$tmp/error_condition.out" 2>&1; then
    echo 'compile_error in a when condition unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'fatal\[N00BB-PP0002\].*stop condition' \
    "$tmp/error_condition.out"
! grep -q 'when condition must have static type bool' \
    "$tmp/error_condition.out"
[ "$(grep -c 'N00BB-PP0002' "$tmp/error_condition.out")" -eq 1 ]

cat >"$tmp/dep.n00b" <<'EOF'
const enabled = true
public func imported_identity(item: `a) -> `a { return item }
EOF
cat >"$tmp/importer.n00b" <<'EOF'
import dep
when dep.enabled {
    var imported_selected = 1
    when enabled {
        var imported_short_name = true
    } else {
        var imported_short_name_removed = true
    }
} else {
    var imported_removed = 2
}
EOF
N00B_PATH="$tmp" "$compiler" preprocess "$tmp/importer.n00b" \
    >"$tmp/import.out"
grep -q '"imported_selected"' "$tmp/import.out"
grep -q '"imported_short_name"' "$tmp/import.out"
! grep -q '"imported_removed"' "$tmp/import.out"
! grep -q '"imported_short_name_removed"' "$tmp/import.out"

N00B_PATH="$tmp" "$compiler" preprocess --dump-symbols \
    "$tmp/importer.n00b" >"$tmp/imported_signatures.out"
grep -q 'imported_identity: function, mutable, type=forall\[`a\] (`a) -> `a' \
    "$tmp/imported_signatures.out"

cat >"$tmp/imported_generic_use.n00b" <<'EOF'
import dep
func use_imported_generics() {
    first = dep.imported_identity(1)
    second = dep.imported_identity("text")
}
EOF
if N00B_PATH="$tmp" "$compiler" preprocess --dump-symbols \
    --jobs 4 "$tmp/imported_generic_use.n00b" \
    >"$tmp/imported_generic_use.out" 2>&1; then
    grep -q 'use_imported_generics: function, mutable, type=() -> nil' \
        "$tmp/imported_generic_use.out"
    grep -q 'first: variable, mutable, type=int' \
        "$tmp/imported_generic_use.out"
    grep -q 'second: variable, mutable, type=string' \
        "$tmp/imported_generic_use.out"
    grep -q 'imported_identity: function, mutable, type=forall\[`a\] (`a) -> `a' \
        "$tmp/imported_generic_use.out"
else
    cat "$tmp/imported_generic_use.out"
    echo 'independent imported generic uses unexpectedly failed' >&2
    exit 1
fi

cat >"$tmp/recursive_a.n00b" <<'EOF'
import recursive_b
func needs_text(item: string) -> string { return item }
public func from_a() {
    if true { return recursive_b.from_b() } else { return needs_text("text") }
}
func checks_group_type() { return needs_text(from_a()) }
EOF
cat >"$tmp/recursive_b.n00b" <<'EOF'
import recursive_a
public func from_b() {
    if true { return recursive_a.from_a() } else { return "text" }
}
EOF
if N00B_PATH="$tmp" "$compiler" preprocess --dump-symbols \
    "$tmp/recursive_a.n00b" --jobs 4 >"$tmp/recursive_modules.out" 2>&1; then
    grep -q 'from_a: function, mutable, type=() -> string' \
        "$tmp/recursive_modules.out"
    grep -q 'from_b: function, mutable, type=() -> string' \
        "$tmp/recursive_modules.out"
else
    cat "$tmp/recursive_modules.out"
    echo 'cross-module recursive inference unexpectedly failed' >&2
    exit 1
fi

cat >"$tmp/recursive_bad_a.n00b" <<'EOF'
import recursive_bad_b
func needs_int(item: int) -> int { return item }
func from_a() { return recursive_bad_b.from_b() }
func rejects_bad_group_type() { return needs_int(from_a()) }
EOF
cat >"$tmp/recursive_bad_b.n00b" <<'EOF'
import recursive_bad_a
func from_b() {
    if true { return recursive_bad_a.from_a() } else { return "text" }
}
EOF
if N00B_PATH="$tmp" "$compiler" preprocess \
    "$tmp/recursive_bad_a.n00b" >"$tmp/recursive_bad_modules.out" 2>&1; then
    cat "$tmp/recursive_bad_modules.out"
    echo 'incompatible cross-module recursive return unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'N00BB-T0002' "$tmp/recursive_bad_modules.out"

cat >"$tmp/import_after_use.n00b" <<'EOF'
when dep.enabled {
    var import_still_precedes_module = true
}
import dep
EOF
N00B_PATH="$tmp" "$compiler" preprocess \
    "$tmp/import_after_use.n00b" >"$tmp/import_after_use.out"
grep -q '"import_still_precedes_module"' "$tmp/import_after_use.out"

cat >"$tmp/shadow_import.n00b" <<'EOF'
import dep
const enabled = false
when enabled {
    var shadow_removed = true
} else {
    var local_shadow_won = true
}
EOF
N00B_PATH="$tmp" "$compiler" preprocess "$tmp/shadow_import.n00b" \
    >"$tmp/shadow_import.out"
grep -q '"local_shadow_won"' "$tmp/shadow_import.out"
! grep -q '"shadow_removed"' "$tmp/shadow_import.out"

cat >"$tmp/dep2.n00b" <<'EOF'
const enabled = false
EOF
cat >"$tmp/ambiguous_import.n00b" <<'EOF'
import dep
import dep2
when enabled {
    var ambiguous = true
}
EOF
if N00B_PATH="$tmp" "$compiler" preprocess \
    --jobs 4 "$tmp/ambiguous_import.n00b" \
    >"$tmp/ambiguous_import.out" 2>&1; then
    echo 'ambiguous imported const unexpectedly resolved by short name' >&2
    exit 1
fi
grep -q 'imported const name is ambiguous' "$tmp/ambiguous_import.out"

cat >"$tmp/qualified_import.n00b" <<'EOF'
import dep
import dep2
when dep.enabled {
    var qualified_import = true
}
EOF
N00B_PATH="$tmp" "$compiler" preprocess \
    --jobs 4 "$tmp/qualified_import.n00b" >"$tmp/qualified_import.out"
grep -q '"qualified_import"' "$tmp/qualified_import.out"

cat >"$tmp/cycle_a.n00b" <<'EOF'
import cycle_b
when enabled {
    var cycle_selected = 1
} else {
    var cycle_removed = 2
}
EOF
cat >"$tmp/cycle_b.n00b" <<'EOF'
import cycle_a
const enabled = true
EOF
N00B_PATH="$tmp" "$compiler" preprocess "$tmp/cycle_a.n00b" \
    --dump-symbols --jobs 4 >"$tmp/cycle.out" 2>&1
grep -q '"cycle_selected"' "$tmp/cycle.out"
! grep -q '"cycle_removed"' "$tmp/cycle.out"
grep -q 'N00BB-PPW0002' "$tmp/cycle.out"
grep -q 'symbols cycle_a:' "$tmp/cycle.out"
grep -q 'symbols cycle_b:' "$tmp/cycle.out"
grep -q 'imports: cycle_b' "$tmp/cycle.out"
grep -q 'imports: cycle_a' "$tmp/cycle.out"

cat >"$tmp/cycle_x.n00b" <<'EOF'
import cycle_y
when cycle_y.enabled {
    var indirect_cycle_selected = true
} else {
    var indirect_cycle_removed = true
}
EOF
cat >"$tmp/cycle_y.n00b" <<'EOF'
import cycle_z
const enabled = true
EOF
cat >"$tmp/cycle_z.n00b" <<'EOF'
import cycle_x
const leaf = false
EOF
N00B_PATH="$tmp" "$compiler" preprocess "$tmp/cycle_x.n00b" \
    --dump-symbols --jobs 4 >"$tmp/indirect_cycle.out" 2>&1
grep -q '"indirect_cycle_selected"' "$tmp/indirect_cycle.out"
! grep -q '"indirect_cycle_removed"' "$tmp/indirect_cycle.out"
grep -q 'N00BB-PPW0002' "$tmp/indirect_cycle.out"
grep -q 'imports: cycle_y' "$tmp/indirect_cycle.out"
grep -q 'imports: cycle_z' "$tmp/indirect_cycle.out"
grep -q 'imports: cycle_x' "$tmp/indirect_cycle.out"

cat >"$tmp/cycle_c.n00b" <<'EOF'
import cycle_d
when true or cycle_d.enabled {
    var short_circuit_selected = true
}
EOF
cat >"$tmp/cycle_d.n00b" <<'EOF'
import cycle_c
const enabled = false
EOF
N00B_PATH="$tmp" "$compiler" preprocess "$tmp/cycle_c.n00b" \
    >"$tmp/cycle_short_circuit.out" 2>&1
grep -q '"short_circuit_selected"' "$tmp/cycle_short_circuit.out"
! grep -q 'N00BB-PPW0002' "$tmp/cycle_short_circuit.out"
