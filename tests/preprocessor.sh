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

cat >"$tmp/typecase_narrowing.n00b" <<'EOF'
func typecase_narrowing(item: (int | string)) -> int {
    typecase(item) {
        case int { return item + 1 }
        case string { return 2 }
    }
}
EOF
"$compiler" preprocess --dump-symbols "$tmp/typecase_narrowing.n00b" \
    >"$tmp/typecase_narrowing.out" 2>"$tmp/typecase_narrowing.err" || {
    cat "$tmp/typecase_narrowing.err"
    exit 1
}
"$compiler" lower --dump-ir "$tmp/typecase_narrowing.n00b" \
    >"$tmp/typecase_narrowing.ir" 2>"$tmp/typecase_narrowing.lower.err" || {
    cat "$tmp/typecase_narrowing.lower.err"
    exit 1
}
grep -q 'type.test %item, int' "$tmp/typecase_narrowing.ir"
grep -q 'type.narrow %item, int' "$tmp/typecase_narrowing.ir"
grep -q 'add trap %__n00bb_narrow_' "$tmp/typecase_narrowing.ir"

cat >"$tmp/typecase_multiple_types.n00b" <<'EOF'
func typecase_multiple_types(item: (int | string | bool)) -> int {
    typecase(item) {
        case int, string { return 1 }
        case bool { return 2 }
    }
}
EOF
"$compiler" lower --dump-ir "$tmp/typecase_multiple_types.n00b" \
    >"$tmp/typecase_multiple_types.ir" 2>"$tmp/typecase_multiple_types.err" || {
    cat "$tmp/typecase_multiple_types.err"
    exit 1
}
grep -q 'type.test %item, (int | string)' \
    "$tmp/typecase_multiple_types.ir"

cat >"$tmp/typecase_else_and_break.n00b" <<'EOF'
func typecase_else(item: any) -> int {
    typecase(item) {
        case int { return item }
        else { return 0 }
    }
}
func typecase_break(item: (int | string)) -> int {
    var result: int = 0
    typecase(item) {
        case int { result = 1; break }
        case string { result = 2 }
    }
    return result
}
EOF
"$compiler" lower --dump-ir --dump-control-flow \
    "$tmp/typecase_else_and_break.n00b" \
    >"$tmp/typecase_else_and_break.out" \
    2>"$tmp/typecase_else_and_break.err" || {
    cat "$tmp/typecase_else_and_break.err"
    exit 1
}
grep -q 'type.test %item, int' "$tmp/typecase_else_and_break.out"
grep -q 'type.narrow %item, int' "$tmp/typecase_else_and_break.out"
grep -q 'typecase_join' "$tmp/typecase_else_and_break.out"

cat >"$tmp/typecase_nonvariable.n00b" <<'EOF'
func typecase_nonvariable() {
    typecase(1) { case int {} }
}
EOF
if "$compiler" preprocess "$tmp/typecase_nonvariable.n00b" \
    >"$tmp/typecase_nonvariable.out" 2>&1; then
    echo 'typecase with a non-variable subject unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'typecase requires a variable subject' \
    "$tmp/typecase_nonvariable.out"

cat >"$tmp/typecase_nonexhaustive.n00b" <<'EOF'
func typecase_nonexhaustive(item: (int | string)) {
    typecase(item) { case int {} }
}
EOF
if "$compiler" lower "$tmp/typecase_nonexhaustive.n00b" \
    >"$tmp/typecase_nonexhaustive.out" 2>&1; then
    echo 'non-exhaustive typecase unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'typecase without else must cover every subject type' \
    "$tmp/typecase_nonexhaustive.out"

cat >"$tmp/context_signature.n00b" <<'EOF'
context TestContext(resource: int) provides: int fail: int {
    enter() { return resource }
    exit(guard) {}
    error(err) {}
    ensure(guard) {}
}
EOF
"$compiler" parse --dump-symbols "$tmp/context_signature.n00b" \
    >"$tmp/context_signature.symbols" 2>"$tmp/context_signature.err" || {
    cat "$tmp/context_signature.err"
    exit 1
}
grep -q 'TestContext: variable, mutable, type=(int) -> int fail: int' \
    "$tmp/context_signature.symbols"
"$compiler" lower "$tmp/context_signature.n00b" \
    >"$tmp/context_signature.ir" 2>"$tmp/context_signature.lower.err" || {
    cat "$tmp/context_signature.lower.err"
    exit 1
}
grep -q '@__n00bb_context_TestContext_enter(%resource: int) -> int fail: int' \
    "$tmp/context_signature.ir"
grep -q '@__n00bb_context_TestContext_exit(%resource: int, %guard: int)' \
    "$tmp/context_signature.ir"
grep -q '@__n00bb_context_TestContext_error(%resource: int, %err: int)' \
    "$tmp/context_signature.ir"
grep -q '@__n00bb_context_TestContext_ensure(%resource: int, %guard: int)' \
    "$tmp/context_signature.ir"

cat >"$tmp/context_variadic.n00b" <<'EOF'
context Batch(*values: int) provides: array[int] {
    enter() { return values }
    exit(result) {}
}
func use_batch() {
    with batch = Batch(1, 2, 3) {}
}
EOF
"$compiler" lower --dump-ir "$tmp/context_variadic.n00b" \
    >"$tmp/context_variadic.ir" 2>"$tmp/context_variadic.err" || {
    cat "$tmp/context_variadic.err"
    exit 1
}
grep -q '@__n00bb_context_Batch_enter(%values: array\[int\]) -> array\[int\]' \
    "$tmp/context_variadic.ir"
grep -q '= aggregate %__lowered_' "$tmp/context_variadic.ir"
grep -q 'call @__n00bb_context_Batch_enter, %__n00bb_context_rest_' \
    "$tmp/context_variadic.ir"

cat >"$tmp/lower_context_with.n00b" <<'EOF'
context Recorder(id: int) provides: int {
    enter() { return id }
    exit(guard) {}
    ensure(guard) {}
}
context QuietRecorder(id: int) provides: int {
    enter() { return id }
    ensure(guard) {}
}
context FailingEnter(id: int) provides: int fail: int {
    enter() { fail id }
    error(problem) {}
    ensure(guard) {}
}
func context_return() -> int {
    with first = Recorder(1), second = Recorder(2) {
        return first + second
    }
}
func context_fallthrough() -> int {
    with Recorder(3) {}
    return 4
}
func context_failure() -> int fail: int {
    with Recorder(4) { fail 8 }
}
func context_failure_without_error() -> int fail: int {
    with QuietRecorder(6) { fail 9 }
}
func context_enter_failure() -> int fail: int {
    with prior = QuietRecorder(10), failed = FailingEnter(11) {
        return prior + failed
    }
}
EOF
"$compiler" lower --dump-ir "$tmp/lower_context_with.n00b" \
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
grep -q 'call @__n00bb_context_QuietRecorder_ensure' \
    "$tmp/lower_context_with.ir"
! grep -q '@__n00bb_context_QuietRecorder_error' \
    "$tmp/lower_context_with.ir"
grep -q '^  static func @context_failure() -> int fail: int' \
    "$tmp/lower_context_with.ir"
awk '
    /^  static func @context_enter_failure/ { in_enter_failure = 1 }
    /^  static func @__n00bb_context_/ { in_enter_failure = 0 }
    in_enter_failure && /^    block __n00bb_enter_error_/ { in_enter_error = 1 }
    in_enter_failure && /^    block / && !/__n00bb_enter_error_/ { in_enter_error = 0 }
    in_enter_error && /call @__n00bb_context_QuietRecorder_ensure/ { prior_ensure = 1 }
    in_enter_error && /call @__n00bb_context_FailingEnter_(error|ensure)/ { failed_cleanup = 1 }
    in_enter_error && /fail %/ { propagated = 1 }
    END { exit !(prior_ensure && !failed_cleanup && propagated) }
' "$tmp/lower_context_with.ir"
awk '
    /^  static func @context_return/ { in_return = 1 }
    /^  static func @context_fallthrough/ { in_return = 0 }
    in_return && /call @__n00bb_context_Recorder_exit/ { exits++ }
    in_return && /return %/ { returned = 1 }
    END { exit !(exits == 2 && returned) }
' "$tmp/lower_context_with.ir"

cat >"$tmp/lower_context_hook_failures.n00b" <<'EOF'
context RecoverExit() fail: int {
    enter() {}
    exit() { fail 13 }
    error(problem) {}
    ensure() {}
}
context PropagatingExit() fail: int {
    enter() {}
    exit() { fail 17 }
    error(problem) { fail problem }
    ensure() {}
}
context UnhandledExit() fail: int {
    enter() {}
    exit() { fail 19 }
    ensure() {}
}
func recover_exit_error() -> nil fail: int {
    with RecoverExit() {}
}
func propagate_error_hook_failure() -> nil fail: int {
    with PropagatingExit() {}
}
func propagate_unhandled_exit_failure() -> nil fail: int {
    with UnhandledExit() {}
}
EOF
"$compiler" lower --dump-ir "$tmp/lower_context_hook_failures.n00b" \
    >"$tmp/lower_context_hook_failures.ir" \
    2>"$tmp/lower_context_hook_failures.err" || {
    cat "$tmp/lower_context_hook_failures.err"
    exit 1
}
awk '
    /^  static func @recover_exit_error/ { in_recover = 1 }
    /^  static func @__n00bb_context_/ { in_recover = 0 }
    in_recover && /call @__n00bb_context_RecoverExit_error/ { handled++ }
    in_recover && /call @__n00bb_context_RecoverExit_ensure/ { ensured++ }
    in_recover && /fail %/ { propagated++ }
    END { exit !(handled == 1 && ensured == 3 && propagated == 2) }
' "$tmp/lower_context_hook_failures.ir" || {
    cat "$tmp/lower_context_hook_failures.ir"
    exit 1
}
awk '
    /^  static func @propagate_error_hook_failure/ { in_handler = 1 }
    /^  static func @recover_exit_error/ { in_handler = 0 }
    in_handler && /call @__n00bb_context_PropagatingExit_error/ { handled++ }
    in_handler && /call @__n00bb_context_PropagatingExit_ensure/ { ensured++ }
    in_handler && /fail %/ { propagated++ }
    END { exit !(handled == 1 && ensured == 3 && propagated == 2) }
' "$tmp/lower_context_hook_failures.ir"
awk '
    /^  static func @propagate_unhandled_exit_failure/ { in_unhandled = 1 }
    /^  static func @propagate_error_hook_failure/ { in_unhandled = 0 }
    in_unhandled && /call @__n00bb_context_UnhandledExit_ensure/ { ensured++ }
    in_unhandled && /fail %/ { propagated++ }
    END { exit !(ensured == 2 && propagated == 2) }
' "$tmp/lower_context_hook_failures.ir"

cat >"$tmp/lower_context_body_failure.n00b" <<'EOF'
context BodyFailure() fail: int {
    enter() {}
    exit() {}
    error(problem) {}
    ensure() {}
}
func propagate_body_failure() -> nil fail: int {
    with BodyFailure() { fail 23 }
}
EOF
"$compiler" lower --dump-ir "$tmp/lower_context_body_failure.n00b" \
    >"$tmp/lower_context_body_failure.ir" \
    2>"$tmp/lower_context_body_failure.err" || {
    cat "$tmp/lower_context_body_failure.err"
    exit 1
}
awk '
    /^  static func @propagate_body_failure/ { in_body = 1 }
    /^  static func @__n00bb_context_/ { in_body = 0 }
    in_body && /call @__n00bb_context_BodyFailure_error/ { error_calls++ }
    in_body && /call @__n00bb_context_BodyFailure_ensure/ { ensure_calls++ }
    in_body && /const 23/ { original_failure = 1 }
    END { exit !(error_calls == 0 && ensure_calls == 1 && original_failure) }
' "$tmp/lower_context_body_failure.ir"

cat >"$tmp/lower_context_failing_ensure.n00b" <<'EOF'
context FailingEnsure() {
    enter() {}
    ensure() { fail 31 }
}
func use_failing_ensure() {
    with FailingEnsure() {}
}
EOF
if "$compiler" lower "$tmp/lower_context_failing_ensure.n00b" \
    >"$tmp/lower_context_failing_ensure.out" 2>&1; then
    echo 'a fallible ensure hook unexpectedly lowered' >&2
    exit 1
fi
grep -q 'fail statement requires a non-nil function failure type' \
    "$tmp/lower_context_failing_ensure.out"

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
"$compiler" lower --dump-control-flow --dump-pdg \
    "$tmp/lower_arithmetic.n00b" >"$tmp/lower_arithmetic.analysis"
grep -q '^control-flow @add$' "$tmp/lower_arithmetic.analysis"
grep -q '^pdg @add ' "$tmp/lower_arithmetic.analysis"
grep -q 'data .* =>' "$tmp/lower_arithmetic.analysis"
! grep -q '^module lower_arithmetic' "$tmp/lower_arithmetic.analysis"
"$compiler" lower --dump-ir --dump-pdg \
    "$tmp/lower_arithmetic.n00b" >"$tmp/lower_arithmetic.ir_and_pdg"
grep -q '^module lower_arithmetic' "$tmp/lower_arithmetic.ir_and_pdg"
grep -q '^pdg @add ' "$tmp/lower_arithmetic.ir_and_pdg"
for command in build library compile; do
    if "$compiler" "$command" --dump-ir --dump-control-flow --dump-pdg \
        "$tmp/lower_arithmetic.n00b" >"$tmp/$command.dumps" 2>&1; then
        echo "$command unexpectedly completed its unimplemented backend stage" >&2
        exit 1
    fi
    grep -q '^module lower_arithmetic' "$tmp/$command.dumps"
    grep -q '^control-flow @add$' "$tmp/$command.dumps"
    grep -q '^pdg @add ' "$tmp/$command.dumps"
    grep -q "'$command' processing is not implemented yet" \
        "$tmp/$command.dumps"
done

cat >"$tmp/lower_value_return.n00b" <<'EOF'
func value_return() -> int { value 42 }
EOF
"$compiler" lower "$tmp/lower_value_return.n00b" \
    >"$tmp/lower_value_return.ir" 2>"$tmp/lower_value_return.err" || {
    cat "$tmp/lower_value_return.err"
    exit 1
}
grep -q 'return %' "$tmp/lower_value_return.ir"

cat >"$tmp/lower_failure_exit.n00b" <<'EOF'
func fail_with_int() -> int fail: int { fail 7 }
EOF
"$compiler" lower "$tmp/lower_failure_exit.n00b" \
    >"$tmp/lower_failure_exit.ir" 2>"$tmp/lower_failure_exit.err" || {
    cat "$tmp/lower_failure_exit.err"
    exit 1
}
grep -q 'fail: int' "$tmp/lower_failure_exit.ir"
grep -q 'fail %' "$tmp/lower_failure_exit.ir"

cat >"$tmp/lower_error_propagation.n00b" <<'EOF'
func fail_with_int() -> int fail: int { fail 7 }
func propagate_error() -> int fail: int {
    return fail_with_int()!
}
EOF
"$compiler" lower "$tmp/lower_error_propagation.n00b" \
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

cat >"$tmp/lower_unhandled_error.n00b" <<'EOF'
func fail_with_int() -> int fail: int { fail 7 }
func unhandled_error() -> int { return fail_with_int() }
EOF
if "$compiler" lower --dump-ir --dump-control-flow --dump-pdg \
    "$tmp/lower_unhandled_error.n00b" \
    >"$tmp/lower_unhandled_error.out" \
    2>"$tmp/lower_unhandled_error.err"; then
    echo 'a fallible call without postfix ! unexpectedly lowered' >&2
    exit 1
fi
grep -q 'must be used with postfix !' "$tmp/lower_unhandled_error.err"
! grep -Eq '^(module |control-flow |pdg )' \
    "$tmp/lower_unhandled_error.out"

cat >"$tmp/lower_caught_error.n00b" <<'EOF'
func fail_with_int() -> int fail: int { fail 7 }
func catch_error() -> int {
    try { return fail_with_int()! } else err { return err }
}
EOF
"$compiler" lower "$tmp/lower_caught_error.n00b" \
    >"$tmp/lower_caught_error.ir" 2>"$tmp/lower_caught_error.err" || {
    cat "$tmp/lower_caught_error.err"
    exit 1
}
grep -q 'variant.tag' "$tmp/lower_caught_error.ir"
grep -q 'try_handler' "$tmp/lower_caught_error.ir"
! grep -q 'fail %' "$tmp/lower_caught_error.ir"

cat >"$tmp/lower_caught_fail.n00b" <<'EOF'
func catch_direct_failure() -> int fail: int {
    try { fail 7 } else err { return 42 }
}
EOF
"$compiler" lower "$tmp/lower_caught_fail.n00b" \
    >"$tmp/lower_caught_fail.ir" 2>"$tmp/lower_caught_fail.err" || {
    cat "$tmp/lower_caught_fail.err"
    exit 1
}
grep -q 'try_handler' "$tmp/lower_caught_fail.ir"
! grep -q 'fail %' "$tmp/lower_caught_fail.ir"

cat >"$tmp/lower_try_value.n00b" <<'EOF'
func fail_with_int() -> int fail: int { fail 7 }
func catch_error_value() -> int {
    return try { value fail_with_int()! } else err { value 42 }
}
EOF
"$compiler" lower "$tmp/lower_try_value.n00b" \
    >"$tmp/lower_try_value.ir" 2>"$tmp/lower_try_value.err" || {
    cat "$tmp/lower_try_value.err"
    exit 1
}
grep -q 'try_handler' "$tmp/lower_try_value.ir"
grep -q 'try_value_' "$tmp/lower_try_value.ir"
grep -q 'fail %' "$tmp/lower_try_value.ir"

cat >"$tmp/lower_nested_try.n00b" <<'EOF'
func fail_with_int() -> int fail: int { fail 7 }
func nested_try() -> int {
    try {
        try { return fail_with_int()! } else inner { return inner }
    } else outer { return 99 }
}
EOF
"$compiler" lower "$tmp/lower_nested_try.n00b" \
    >"$tmp/lower_nested_try.ir" 2>"$tmp/lower_nested_try.err" || {
    cat "$tmp/lower_nested_try.err"
    exit 1
}
[ "$(grep -c 'block __n00bb_try_handler' \
    "$tmp/lower_nested_try.ir")" -eq 2 ]

cat >"$tmp/lower_mismatched_error.n00b" <<'EOF'
func fail_with_int() -> int fail: int { fail 7 }
func mismatched_error() -> int fail: bool {
    return fail_with_int()!
}
EOF
if "$compiler" lower "$tmp/lower_mismatched_error.n00b" \
    >"$tmp/lower_mismatched_error.out" \
    2>"$tmp/lower_mismatched_error.err"; then
    echo 'error propagation across mismatched channels unexpectedly passed' >&2
    exit 1
fi
grep -q "must match the enclosing function's failure channel" \
    "$tmp/lower_mismatched_error.err"

cat >"$tmp/lower_value_block.n00b" <<'EOF'
func value_block() -> int {
    let result = { value 42 }
    return result
}
EOF
"$compiler" lower "$tmp/lower_value_block.n00b" \
    >"$tmp/lower_value_block.ir" 2>"$tmp/lower_value_block.err" || {
    cat "$tmp/lower_value_block.err"
    exit 1
}
grep -q '__n00bb_block_value_' "$tmp/lower_value_block.ir"
grep -q 'block_value_cont' "$tmp/lower_value_block.ir"

cat >"$tmp/lower_local_assignment.n00b" <<'EOF'
func local_assignment() -> int {
    var total: int = 1
    total = total + 2
    return total
}
EOF
"$compiler" lower "$tmp/lower_local_assignment.n00b" \
    >"$tmp/lower_local_assignment.ir" \
    2>"$tmp/lower_local_assignment.err" || {
    cat "$tmp/lower_local_assignment.err"
    exit 1
}
grep -q '__n00bb_local_' "$tmp/lower_local_assignment.ir"
grep -q '\$__n00bb_local_.* = copy' "$tmp/lower_local_assignment.ir"

cat >"$tmp/lower_compound_assignment.n00b" <<'EOF'
func compound_assignment() -> int {
    var total = 1
    total += 2
    total -= 1
    total *= 2
    total /= 2
    total %= 7
    total <<= 1
    total >>= 1
    total &= 7
    total |= 4
    total ^= 1
    return total
}
EOF
"$compiler" lower "$tmp/lower_compound_assignment.n00b" \
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
grep -q 'shr' "$tmp/lower_compound_assignment.ir"
grep -q 'and' "$tmp/lower_compound_assignment.ir"
grep -q 'or' "$tmp/lower_compound_assignment.ir"
grep -q 'xor' "$tmp/lower_compound_assignment.ir"

cat >"$tmp/lower_immutable_assignment.n00b" <<'EOF'
func immutable_assignment() {
    let total = 1
    total = 2
}
EOF
if "$compiler" lower "$tmp/lower_immutable_assignment.n00b" \
    >"$tmp/lower_immutable_assignment.ir" \
    2>"$tmp/lower_immutable_assignment.err"; then
    echo 'assignment to an immutable local unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'cannot assign to an immutable local' \
    "$tmp/lower_immutable_assignment.err"

cat >"$tmp/lower_global.n00b" <<'EOF'
var global_value: int = 1
func update_global(delta: int) -> int {
    global_value = global_value + delta
    return global_value
}
EOF
if ! "$compiler" lower --dump-ir "$tmp/lower_global.n00b" \
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

cat >"$tmp/lower_record_type.n00b" <<'EOF'
type LoweredPoint {
    x: int
    y: float
}
func identity_point(pt: LoweredPoint) -> LoweredPoint { return pt }
EOF
"$compiler" lower --dump-ir "$tmp/lower_record_type.n00b" \
    >"$tmp/lower_record_type.ir" 2>"$tmp/lower_record_type.err" || {
    cat "$tmp/lower_record_type.err"
    exit 1
}
grep -q 'type lower_record_type.LoweredPoint = (x: int, y: float)' \
    "$tmp/lower_record_type.ir"
grep -q 'func @identity_point(%pt: lower_record_type.LoweredPoint) -> lower_record_type.LoweredPoint' \
    "$tmp/lower_record_type.ir"

cat >"$tmp/record_dep.n00b" <<'EOF'
public type SharedRecord { remote: int }
EOF
cat >"$tmp/record_root.n00b" <<'EOF'
import record_dep
type SharedRecord { local: float }
func use_remote(item: record_dep.SharedRecord) -> record_dep.SharedRecord {
    return item
}
func use_local(item: SharedRecord) -> SharedRecord { return item }
EOF
N00B_PATH="$tmp" "$compiler" lower --dump-ir "$tmp/record_root.n00b" \
    >"$tmp/record_root.ir" 2>"$tmp/record_root.err" || {
    cat "$tmp/record_root.err"
    exit 1
}
grep -q 'type record_root.SharedRecord = (local: float)' \
    "$tmp/record_root.ir"
grep -q 'type record_dep.SharedRecord = (remote: int)' \
    "$tmp/record_root.ir"
grep -q 'func @use_remote(%item: record_dep.SharedRecord) -> record_dep.SharedRecord' \
    "$tmp/record_root.ir"
grep -q 'func @use_local(%item: record_root.SharedRecord) -> record_root.SharedRecord' \
    "$tmp/record_root.ir"

cat >"$tmp/variant_dep.n00b" <<'EOF'
public variant RemoteValue {
    number: int
    text: string
}
EOF
cat >"$tmp/variant_root.n00b" <<'EOF'
import variant_dep
func inspect_remote(item: variant_dep.RemoteValue) -> int {
    switch item {
        case number { return item.number }
        case text { return 0 }
    }
}
EOF
N00B_PATH="$tmp" "$compiler" lower --dump-ir "$tmp/variant_root.n00b" \
    >"$tmp/variant_root.ir" 2>"$tmp/variant_root.err" || {
    cat "$tmp/variant_root.err"
    exit 1
}
grep -q 'variant_dep.RemoteValue' "$tmp/variant_root.ir"
grep -q 'variant.tag %item' "$tmp/variant_root.ir"
grep -q 'variant.payload' "$tmp/variant_root.ir"

cat >"$tmp/lower_out_parameter.n00b" <<'EOF'
func writes_output(out result: int) { result = 1 }
EOF
"$compiler" lower "$tmp/lower_out_parameter.n00b" \
    >"$tmp/lower_out_parameter.ir" \
    2>"$tmp/lower_out_parameter.err" || {
    cat "$tmp/lower_out_parameter.err"
    cat "$tmp/lower_out_parameter.ir"
    exit 1
}
grep -q '%result: ptr\[int\]' "$tmp/lower_out_parameter.ir" || {
    cat "$tmp/lower_out_parameter.ir"
    exit 1
}
grep -q 'store' "$tmp/lower_out_parameter.ir" || {
    cat "$tmp/lower_out_parameter.ir"
    exit 1
}

cat >"$tmp/lower_if.n00b" <<'EOF'
func choose(flag: bool) -> int {
    if flag { return 1 }
    return 2
}
EOF
"$compiler" lower "$tmp/lower_if.n00b" \
    >"$tmp/lower_if.ir" 2>"$tmp/lower_if.err" || {
    cat "$tmp/lower_if.err"
    exit 1
}
grep -Eq 'branch %flag, __n00bb_if_then_[0-9]+, __n00bb_if_join_[0-9]+' \
    "$tmp/lower_if.ir"
grep -q 'return %' "$tmp/lower_if.ir"

cat >"$tmp/lower_if_else.n00b" <<'EOF'
func choose_both(flag: bool) -> int { if flag { return 1 } else { return 2 } }
EOF
"$compiler" lower "$tmp/lower_if_else.n00b" \
    >"$tmp/lower_if_else.ir" 2>"$tmp/lower_if_else.err" || {
    cat "$tmp/lower_if_else.err"
    exit 1
}
grep -Eq 'branch %flag, __n00bb_if_then_[0-9]+, __n00bb_if_else_[0-9]+' \
    "$tmp/lower_if_else.ir"
grep -q 'unreachable' "$tmp/lower_if_else.ir"

cat >"$tmp/lower_if_elif.n00b" <<'EOF'
func choose(flag_a: bool, flag_b: bool) -> int {
    if flag_a { return 1 }
    elif flag_b { return 2 }
    else { return 3 }
}
EOF
"$compiler" lower "$tmp/lower_if_elif.n00b" \
    >"$tmp/lower_if_elif.ir" 2>"$tmp/lower_if_elif.err" || {
    cat "$tmp/lower_if_elif.err"
    exit 1
}
grep -q '__n00bb_if_test_' "$tmp/lower_if_elif.ir"
grep -q '__n00bb_if_else_' "$tmp/lower_if_elif.ir"
grep -q 'unreachable' "$tmp/lower_if_elif.ir"

cat >"$tmp/lower_if_elif_no_else.n00b" <<'EOF'
func choose_default(flag_a: bool, flag_b: bool) -> int {
    if flag_a { return 1 }
    elif flag_b { return 2 }
    return 3
}
EOF
"$compiler" lower "$tmp/lower_if_elif_no_else.n00b" \
    >"$tmp/lower_if_elif_no_else.ir" 2>"$tmp/lower_if_elif_no_else.err" || {
    cat "$tmp/lower_if_elif_no_else.err"
    exit 1
}
grep -q '__n00bb_if_test_' "$tmp/lower_if_elif_no_else.ir"
grep -q '__n00bb_if_join_' "$tmp/lower_if_elif_no_else.ir"

cat >"$tmp/lower_if_value.n00b" <<'EOF'
func choose_value(flag_a: bool, flag_b: bool) -> int {
    return if flag_a { value 1 }
    elif flag_b { value 2 }
    else { value 3 }
}
EOF
"$compiler" lower "$tmp/lower_if_value.n00b" \
    >"$tmp/lower_if_value.ir" 2>"$tmp/lower_if_value.err" || {
    cat "$tmp/lower_if_value.err"
    exit 1
}
grep -q '__n00bb_if_test_' "$tmp/lower_if_value.ir"
grep -q '__n00bb_if_value_' "$tmp/lower_if_value.ir"

cat >"$tmp/lower_while_break.n00b" <<'EOF'
func loop_break(flag: bool) -> int {
    while flag { break }
    return 1
}
EOF
"$compiler" lower "$tmp/lower_while_break.n00b" \
    >"$tmp/lower_while_break.ir" 2>"$tmp/lower_while_break.err" || {
    cat "$tmp/lower_while_break.err"
    exit 1
}
grep -q '__n00bb_while_header_' "$tmp/lower_while_break.ir"
grep -q '__n00bb_while_exit_' "$tmp/lower_while_break.ir"

cat >"$tmp/lower_while_continue.n00b" <<'EOF'
func loop_continue(flag: bool) -> int {
    while flag { continue }
    return 1
}
EOF
"$compiler" lower "$tmp/lower_while_continue.n00b" \
    >"$tmp/lower_while_continue.ir" 2>"$tmp/lower_while_continue.err" || {
    cat "$tmp/lower_while_continue.err"
    exit 1
}
grep -q '__n00bb_while_header_' "$tmp/lower_while_continue.ir"
grep -q '__n00bb_while_exit_' "$tmp/lower_while_continue.ir"

cat >"$tmp/lower_labeled_break.n00b" <<'EOF'
func labeled_break(flag: bool) {
    outer: while flag {
        while flag { break outer }
    }
}
EOF
"$compiler" lower "$tmp/lower_labeled_break.n00b" \
    >"$tmp/lower_labeled_break.ir" 2>"$tmp/lower_labeled_break.err" || {
    cat "$tmp/lower_labeled_break.err"
    exit 1
}
grep -q 'jump __n00bb_while_exit_' "$tmp/lower_labeled_break.ir"

cat >"$tmp/lower_labeled_continue.n00b" <<'EOF'
func labeled_continue(flag: bool) {
    outer: while flag {
        while flag { continue outer }
    }
}
EOF
"$compiler" lower "$tmp/lower_labeled_continue.n00b" \
    >"$tmp/lower_labeled_continue.ir" \
    2>"$tmp/lower_labeled_continue.err" || {
    cat "$tmp/lower_labeled_continue.err"
    exit 1
}
grep -q 'jump __n00bb_while_header_0' "$tmp/lower_labeled_continue.ir"

cat >"$tmp/lower_range_for.n00b" <<'EOF'
func range_for() -> int {
    for index in 1 to 3 { return index }
    for other from 1 until 3 { continue }
    return 0
}
EOF
"$compiler" lower "$tmp/lower_range_for.n00b" \
    >"$tmp/lower_range_for.ir" 2>"$tmp/lower_range_for.err" || {
    cat "$tmp/lower_range_for.err"
    exit 1
}
grep -q 'le %' "$tmp/lower_range_for.ir"
grep -q 'lt %' "$tmp/lower_range_for.ir"
grep -q '__n00bb_for_latch_' "$tmp/lower_range_for.ir"

cat >"$tmp/lower_switch.n00b" <<'EOF'
func choose(code: int) -> int {
    switch code {
    case 1 { return 10 }
    case 2, 3 { return 20 }
    case -1 { break }
    else { return 0 }
    }
    return 30
}
EOF
"$compiler" lower "$tmp/lower_switch.n00b" \
    >"$tmp/lower_switch.ir" 2>"$tmp/lower_switch.err" || {
    cat "$tmp/lower_switch.err"
    exit 1
}
grep -q 'switch %code, default: __n00bb_switch_else_' "$tmp/lower_switch.ir"
grep -q '\[1: __n00bb_switch_case_' "$tmp/lower_switch.ir"
grep -q '\[2: __n00bb_switch_case_' "$tmp/lower_switch.ir"
grep -q '\[-1: __n00bb_switch_case_' "$tmp/lower_switch.ir"
grep -q '__n00bb_switch_else_' "$tmp/lower_switch.ir"

cat >"$tmp/lower_labeled_switch_break.n00b" <<'EOF'
func labeled_switch(flag: bool, code: int) {
    outer: while flag {
        choice: switch code {
        case 1 { break choice }
        }
        break outer
    }
}
EOF
"$compiler" lower "$tmp/lower_labeled_switch_break.n00b" \
    >"$tmp/lower_labeled_switch_break.ir" \
    2>"$tmp/lower_labeled_switch_break.err" || {
    cat "$tmp/lower_labeled_switch_break.err"
    exit 1
}
grep -q 'jump __n00bb_switch_join_' "$tmp/lower_labeled_switch_break.ir"

cat >"$tmp/lower_switch_no_else.n00b" <<'EOF'
func choose_optional(flag: bool) -> int {
    switch flag {
    case true { return 1 }
    }
    return 0
}
EOF
"$compiler" lower "$tmp/lower_switch_no_else.n00b" \
    >"$tmp/lower_switch_no_else.ir" 2>"$tmp/lower_switch_no_else.err" || {
    cat "$tmp/lower_switch_no_else.err"
    exit 1
}
grep -q 'switch %flag, default: __n00bb_switch_join_' \
    "$tmp/lower_switch_no_else.ir"

cat >"$tmp/lower_switch_value.n00b" <<'EOF'
func choose_value(code: int) -> int {
    return switch code {
        case 1 { value 10 }
        else { value 20 }
    }
}
EOF
"$compiler" lower "$tmp/lower_switch_value.n00b" \
    >"$tmp/lower_switch_value.ir" \
    2>"$tmp/lower_switch_value.err" || {
    cat "$tmp/lower_switch_value.err"
    exit 1
}
grep -q 'switch %code' "$tmp/lower_switch_value.ir"
grep -q '__n00bb_switch_result_' "$tmp/lower_switch_value.ir"

cat >"$tmp/lower_switch_ranges.n00b" <<'EOF'
func choose_range(code: int) -> int {
    switch code {
    case 1 to 3, 8 { return 1 }
    case 4 until 7 { return 2 }
    else { return 0 }
    }
}
EOF
"$compiler" lower "$tmp/lower_switch_ranges.n00b" \
    >"$tmp/lower_switch_ranges.ir" \
    2>"$tmp/lower_switch_ranges.err" || {
    cat "$tmp/lower_switch_ranges.err"
    exit 1
}
grep -q 'ge %' "$tmp/lower_switch_ranges.ir"
grep -q 'le %' "$tmp/lower_switch_ranges.ir"
grep -q 'lt %' "$tmp/lower_switch_ranges.ir"
grep -q '__n00bb_switch_test_' "$tmp/lower_switch_ranges.ir"

cat >"$tmp/lower_switch_expression_pattern.n00b" <<'EOF'
func lower_switch_expression_pattern(code: int, wanted: int) -> int {
    switch code {
        case wanted { return 1 }
        case 0 { return 2 }
        else { return 3 }
    }
}
EOF
"$compiler" lower --dump-ir "$tmp/lower_switch_expression_pattern.n00b" \
    >"$tmp/lower_switch_expression_pattern.ir" \
    2>"$tmp/lower_switch_expression_pattern.err" || {
    cat "$tmp/lower_switch_expression_pattern.err"
    exit 1
}
grep -q 'eq %code, %wanted' "$tmp/lower_switch_expression_pattern.ir"
grep -q '__n00bb_switch_test_' "$tmp/lower_switch_expression_pattern.ir"

cat >"$tmp/lower_variant_switch.n00b" <<'EOF'
variant SwitchValue {
    number: int
    empty: nil
}
func lower_variant_typed_arg(item: SwitchValue) -> int {
    switch item {
        case number { return item.number }
        else { return 2 }
    }
}
EOF
"$compiler" lower --dump-ir "$tmp/lower_variant_switch.n00b" \
    >"$tmp/lower_variant_switch.ir" \
    2>"$tmp/lower_variant_switch.err" || {
    cat "$tmp/lower_variant_switch.err"
    exit 1
}
grep -q 'variant.tag %item' "$tmp/lower_variant_switch.ir"
grep -q 'switch %__n00bb_variant_tag_' "$tmp/lower_variant_switch.ir"
grep -q 'variant.payload' "$tmp/lower_variant_switch.ir"
grep -q 'switch_else' "$tmp/lower_variant_switch.ir"

cat >"$tmp/lower_variant_exhaustive.n00b" <<'EOF'
variant SwitchValue {
    number: int
    empty: nil
}
func lower_variant_exhaustive(item: SwitchValue) -> int {
    switch item {
        case number { return item.number }
        case empty { return 0 }
    }
}
EOF
"$compiler" lower --dump-ir "$tmp/lower_variant_exhaustive.n00b" \
    >"$tmp/lower_variant_exhaustive.ir" \
    2>"$tmp/lower_variant_exhaustive.err" || {
    cat "$tmp/lower_variant_exhaustive.err"
    exit 1
}
grep -q 'switch_unreachable' "$tmp/lower_variant_exhaustive.ir"

cat >"$tmp/lower_variant_nonexhaustive.n00b" <<'EOF'
variant SwitchValue {
    number: int
    empty: nil
}
func lower_variant_nonexhaustive(item: SwitchValue) {
    switch item {
        case number {}
    }
}
EOF
if "$compiler" lower "$tmp/lower_variant_nonexhaustive.n00b" \
    >"$tmp/lower_variant_nonexhaustive.out" 2>&1; then
    echo 'non-exhaustive variant switch unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'variant switch without else must cover every arm exactly once' \
    "$tmp/lower_variant_nonexhaustive.out"

cat >"$tmp/lower_variant_nonvariable_subject.n00b" <<'EOF'
variant SwitchValue {
    number: int
    empty: nil
}
func identity_variant(item: SwitchValue) -> SwitchValue { return item }
func lower_variant_nonvariable_subject(item: SwitchValue) {
    switch identity_variant(item) {
        case number {}
        else {}
    }
}
EOF
if "$compiler" lower "$tmp/lower_variant_nonvariable_subject.n00b" \
    >"$tmp/lower_variant_nonvariable_subject.out" 2>&1; then
    echo 'non-variable variant switch subject unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'variant switch subject must be a variable' \
    "$tmp/lower_variant_nonvariable_subject.out"

cat >"$tmp/lower_variant_wrong_arm_payload.n00b" <<'EOF'
variant SwitchValue {
    number: int
    empty: nil
}
func lower_variant_wrong_arm_payload(item: SwitchValue) -> int {
    switch item {
        case empty { return item.number }
        else { return 0 }
    }
}
EOF
if "$compiler" lower "$tmp/lower_variant_wrong_arm_payload.n00b" \
    >"$tmp/lower_variant_wrong_arm_payload.out" 2>&1; then
    echo 'payload access for an unmatched variant arm unexpectedly succeeded' >&2
    exit 1
fi

cat >"$tmp/lower_variant_nil_payload.n00b" <<'EOF'
variant SwitchValue {
    number: int
    empty: nil
}
func lower_variant_nil_payload(item: SwitchValue) {
    switch item {
        case empty { return item.empty }
        else { return }
    }
}
EOF
if "$compiler" lower "$tmp/lower_variant_nil_payload.n00b" \
    >"$tmp/lower_variant_nil_payload.out" 2>&1; then
    echo 'reading a nil variant arm unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'nil variant arm has no readable payload' \
    "$tmp/lower_variant_nil_payload.out" || {
    cat "$tmp/lower_variant_nil_payload.out"
    exit 1
}

cat >"$tmp/lower_variant_duplicate_arm.n00b" <<'EOF'
variant SwitchValue {
    number: int
    empty: nil
}
func lower_variant_duplicate_arm(item: SwitchValue) {
    switch item {
        case number {}
        case number {}
        else {}
    }
}
EOF
if "$compiler" lower "$tmp/lower_variant_duplicate_arm.n00b" \
    >"$tmp/lower_variant_duplicate_arm.out" 2>&1; then
    echo 'duplicate variant switch arm unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'variant switch arm appears more than once' \
    "$tmp/lower_variant_duplicate_arm.out"

cat >"$tmp/lower_variant_else_payload.n00b" <<'EOF'
variant SwitchValue {
    number: int
    empty: nil
}
func lower_variant_else_payload(item: SwitchValue) -> int {
    switch item {
        case empty { return 0 }
        else { return item.number }
    }
}
EOF
if "$compiler" lower "$tmp/lower_variant_else_payload.n00b" \
    >"$tmp/lower_variant_else_payload.out" 2>&1; then
    echo 'variant payload access in else unexpectedly succeeded' >&2
    exit 1
fi

cat >"$tmp/lower_variant_unknown_arm.n00b" <<'EOF'
variant SwitchValue {
    number: int
    empty: nil
}
func lower_variant_unknown_arm(item: SwitchValue) {
    switch item {
        case missing {}
    }
}
EOF
if "$compiler" lower "$tmp/lower_variant_unknown_arm.n00b" \
    >"$tmp/lower_variant_unknown_arm.out" 2>&1; then
    echo 'unknown variant switch arm unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'variant switch cases must name variant arms' \
    "$tmp/lower_variant_unknown_arm.out"

cat >"$tmp/lower_variant_compound_arm.n00b" <<'EOF'
variant SwitchValue {
    number: int
    empty: nil
}
func lower_variant_compound_arm(item: SwitchValue) {
    switch item {
        case number + 1 {}
        else {}
    }
}
EOF
if "$compiler" lower "$tmp/lower_variant_compound_arm.n00b" \
    >"$tmp/lower_variant_compound_arm.out" 2>&1; then
    echo 'compound variant switch label unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'variant switch cases must name variant arms' \
    "$tmp/lower_variant_compound_arm.out"

cat >"$tmp/lower_variant_grouped_arms.n00b" <<'EOF'
variant SwitchValue {
    number: int
    empty: nil
}
func lower_variant_grouped_arms(item: SwitchValue) {
    switch item {
        case number, empty {}
    }
}
EOF
if "$compiler" lower "$tmp/lower_variant_grouped_arms.n00b" \
    >"$tmp/lower_variant_grouped_arms.out" 2>&1; then
    echo 'grouped variant switch arms unexpectedly succeeded' >&2
    exit 1
fi
grep -q 'variant switch cases must name exactly one arm' \
    "$tmp/lower_variant_grouped_arms.out"

cat >"$tmp/lower_call.n00b" <<'EOF'
func identity(item: int) -> int { return item }
func call_identity(item: int) -> int { return identity(item) }
EOF
"$compiler" lower "$tmp/lower_call.n00b" \
    >"$tmp/lower_call.ir" 2>"$tmp/lower_call.err" || {
    cat "$tmp/lower_call.err"
    exit 1
}
grep -q '= call @identity, %item' "$tmp/lower_call.ir"

cat >"$tmp/lower_variadic.n00b" <<'EOF'
func collect(prefix: int, *values: int) -> array[int] {
    return values
}
func collect_many() -> array[int] { return collect(0, 10, 20) }
func collect_none() -> array[int] { return collect(0) }
func collect_inferred(*values) -> array[int] { return values }
func collect_inferred_call() -> array[int] { return collect_inferred(7) }
EOF
"$compiler" lower --dump-ir "$tmp/lower_variadic.n00b" \
    >"$tmp/lower_variadic.ir" 2>"$tmp/lower_variadic.err" || {
    cat "$tmp/lower_variadic.err"
    cat "$tmp/lower_variadic.ir"
    exit 1
}
grep -q 'func @collect(%prefix: int, %values: array\[int\]) -> array\[int\]' \
    "$tmp/lower_variadic.ir"
grep -q '= aggregate' "$tmp/lower_variadic.ir"
test "$(grep -c '= aggregate' "$tmp/lower_variadic.ir")" -eq 3
grep -q 'call @collect' "$tmp/lower_variadic.ir"

cat >"$tmp/lower_array_literals.n00b" <<'EOF'
func lower_array_values() -> array[int] {
    var values: array[int] = [10, 20, 30]
    return values
}
func lower_empty_array() -> array[int] {
    var values: array[int] = []
    return values
}
EOF
"$compiler" lower --dump-ir "$tmp/lower_array_literals.n00b" \
    >"$tmp/lower_array_literals.ir" 2>"$tmp/lower_array_literals.err" || {
    cat "$tmp/lower_array_literals.err"
    cat "$tmp/lower_array_literals.ir"
    exit 1
}
test "$(grep -c '= aggregate' "$tmp/lower_array_literals.ir")" -eq 2
grep -q '= const 10' "$tmp/lower_array_literals.ir"
grep -q '= aggregate %' "$tmp/lower_array_literals.ir"

cat >"$tmp/lower_module_initializers.n00b" <<'EOF'
func initial_value() -> int { return 3 }
var dynamic: int = initial_value()
dynamic = initial_value()
var static_value: int = 0
comptime { static_value = 9; var compile_value: int = 4 }
EOF
"$compiler" lower --dump-ir "$tmp/lower_module_initializers.n00b" \
    >"$tmp/lower_module_initializers.ir" \
    2>"$tmp/lower_module_initializers.err" || {
    cat "$tmp/lower_module_initializers.err"
    exit 1
}
grep -q '@__n00bb_runtime_init() -> nil !n00bb.initializer("runtime")' \
    "$tmp/lower_module_initializers.ir"
grep -q '@__n00bb_comptime_init() -> nil !n00bb.initializer("comptime")' \
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

cat >"$tmp/lower_module_initializer_failure.n00b" <<'EOF'
func may_fail() -> int fail: int { fail 9 }
may_fail()!
EOF
if "$compiler" lower "$tmp/lower_module_initializer_failure.n00b" \
    >"$tmp/lower_module_initializer_failure.out" 2>&1; then
    echo 'a failing top-level initializer unexpectedly lowered' >&2
    exit 1
fi
grep -q 'uncaught call error must match the enclosing function' \
    "$tmp/lower_module_initializer_failure.out" || {
    cat "$tmp/lower_module_initializer_failure.out"
    exit 1
}

cat >"$tmp/init_dependency.n00b" <<'EOF'
func dependency_init() -> int { return 1 }
var dependency_value: int = dependency_init()
EOF
cat >"$tmp/init_root.n00b" <<'EOF'
import init_dependency
func root_init() -> int { return 2 }
var root_value: int = root_init()
EOF
N00B_PATH="$tmp" "$compiler" lower --dump-ir "$tmp/init_root.n00b" \
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

cat >"$tmp/init_cycle_a.n00b" <<'EOF'
import init_cycle_b
func init_cycle_a_value() -> int { return 1 }
var init_cycle_a_runtime: int = init_cycle_a_value()
comptime { var init_cycle_a_compile: int = 1 }
EOF
cat >"$tmp/init_cycle_b.n00b" <<'EOF'
import init_cycle_a
func init_cycle_b_value() -> int { return 2 }
var init_cycle_b_runtime: int = init_cycle_b_value()
comptime { var init_cycle_b_compile: int = 2 }
EOF
N00B_PATH="$tmp" "$compiler" lower --dump-ir "$tmp/init_cycle_a.n00b" \
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
    current == "a" && /__n00bb_comptime_init/ && /initializer_order\([01]\)/ { a_comptime_order = 1 }
    current == "a" && /__n00bb_runtime_init/ && /initializer_order\([01]\)/ { a_runtime_order = 1 }
    current == "b" && /__n00bb_comptime_init/ && /initializer_order\([01]\)/ { b_comptime_order = 1 }
    current == "b" && /__n00bb_runtime_init/ && /initializer_order\([01]\)/ { b_runtime_order = 1 }
    END {
        exit !(a_comptime && a_runtime && b_comptime && b_runtime &&
               a_comptime_order && a_runtime_order &&
               b_comptime_order && b_runtime_order)
    }
' "$tmp/init_cycle_order.ir"

cat >"$tmp/lower_nested_comptime.n00b" <<'EOF'
func nested_compile_time() -> int { return comptime 4 }
EOF
if "$compiler" lower "$tmp/lower_nested_comptime.n00b" \
    >"$tmp/lower_nested_comptime.out" 2>&1; then
    echo 'nested compile-time work was lowered as runtime code' >&2
    exit 1
fi
grep -q 'compile-time work inside a function must be evaluated before runtime lowering' \
    "$tmp/lower_nested_comptime.out" || {
    cat "$tmp/lower_nested_comptime.out"
    exit 1
}

cat >"$tmp/lower_default_arguments.n00b" <<'EOF'
func defaulted(first: int = later + 1, later: int = 4,
               last: int = 8) -> int {
    return first + later + last
}
func named(first: int, second: int = 5) -> int { return first + second }
func first_value() -> int { return 1 }
func second_value() -> int { return 2 }
func use_defaults() -> int { return defaulted(last = 12) }
func use_partial_default() -> int { return defaulted(7, last = 10) }
func use_named_arguments() -> int { return named(second = 9, first = 2) }
func use_named_evaluation_order() -> int {
    return named(second = second_value(), first = first_value())
}
EOF
"$compiler" lower --dump-ir "$tmp/lower_default_arguments.n00b" \
    >"$tmp/lower_default_arguments.ir" \
    2>"$tmp/lower_default_arguments.err" || {
    cat "$tmp/lower_default_arguments.err"
    exit 1
}
grep -q 'call @defaulted, %__n00bb_default_arg_' \
    "$tmp/lower_default_arguments.ir"
grep -Eq 'call @named, %__lowered_[0-9]+, %__n00bb_default_arg_' \
    "$tmp/lower_default_arguments.ir"
grep -q 'extract .*"present"' "$tmp/lower_default_arguments.ir"
grep -q 'add trap' "$tmp/lower_default_arguments.ir"
awk '
    /^  static func @use_named_evaluation_order/ { in_call = 1 }
    /^  static func @/ && !/use_named_evaluation_order/ && in_call { exit }
    in_call && /call @second_value/ { second = NR }
    in_call && /call @first_value/ { first = NR }
    END { exit !(second && first && second < first) }
' "$tmp/lower_default_arguments.ir"

cat >"$tmp/lower_missing_required_argument.n00b" <<'EOF'
func required(input: int) -> int { return input }
func missing_required() -> int { return required() }
EOF
if "$compiler" lower "$tmp/lower_missing_required_argument.n00b" \
    >"$tmp/lower_missing_required_argument.out" 2>&1; then
    echo 'a missing required argument unexpectedly lowered' >&2
    exit 1
fi
grep -q 'call omits a required parameter' \
    "$tmp/lower_missing_required_argument.out"

cat >"$tmp/lower_cyclic_defaults.n00b" <<'EOF'
func cyclic(first: int = second, second: int = first) -> int {
    return first
}
func use_cyclic_defaults() -> int { return cyclic() }
EOF
if "$compiler" lower "$tmp/lower_cyclic_defaults.n00b" \
    >"$tmp/lower_cyclic_defaults.out" 2>&1; then
    echo 'cyclic default dependencies unexpectedly lowered' >&2
    exit 1
fi
grep -q 'default parameter dependencies are cyclic' \
    "$tmp/lower_cyclic_defaults.out"

cat >"$tmp/lower_parameter_modes.n00b" <<'EOF'
func copy_modify(modify target: int, incoming: int) { target = incoming }
func write_out(out target: int, incoming: int) { target = incoming }
func exercise_modes(initial: int) -> int {
    var local: int
    write_out(local, initial)
    copy_modify(local, initial)
    return local
}
EOF
"$compiler" lower "$tmp/lower_parameter_modes.n00b" \
    >"$tmp/lower_parameter_modes.ir" \
    2>"$tmp/lower_parameter_modes.err" || {
    cat "$tmp/lower_parameter_modes.err"
    exit 1
}
grep -q '%target: ptr\[int\]' \
    "$tmp/lower_parameter_modes.ir"
grep -q 'alloca int' "$tmp/lower_parameter_modes.ir"
grep -q 'store %__n00bb_arg_' "$tmp/lower_parameter_modes.ir"
grep -q 'load %__n00bb_arg_' "$tmp/lower_parameter_modes.ir"
grep -q '= call @copy_modify' "$tmp/lower_parameter_modes.ir"
grep -q '= call @write_out' "$tmp/lower_parameter_modes.ir"

cat >"$tmp/lower_trait_dispatch.n00b" <<'EOF'
trait Render {
    func render(self: `T) -> int
}
func render(self: int) -> int { return self }
func use_render(item: int) -> int { return item.render() }
EOF
"$compiler" lower "$tmp/lower_trait_dispatch.n00b" \
    >"$tmp/lower_trait_dispatch.ir" \
    2>"$tmp/lower_trait_dispatch.err" || {
    cat "$tmp/lower_trait_dispatch.err"
    exit 1
}
grep -q '= call @render, %item' "$tmp/lower_trait_dispatch.ir"
grep -q 'return %__n00bb_' "$tmp/lower_trait_dispatch.ir"

cat >"$tmp/dep.n00b" <<'EOF'
public func imported(item: int) -> int { return item }
EOF
cat >"$tmp/lower_import_call.n00b" <<'EOF'
import dep
func call_imported(item: int) -> int { return dep.imported(item) }
EOF
N00B_PATH="$tmp" "$compiler" lower "$tmp/lower_import_call.n00b" \
    >"$tmp/lower_import_call.ir" 2>"$tmp/lower_import_call.err" || {
    cat "$tmp/lower_import_call.err"
    exit 1
}
grep -q '= call @imported, %item' "$tmp/lower_import_call.ir"

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
grep -q 'N00BB-T0001' "$tmp/index_load_bad_result.out" || {
    cat "$tmp/index_load_bad_result.out"
    exit 1
}

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
cat >"$tmp/lower_recursive_a.n00b" <<'EOF'
import lower_recursive_b
public func from_a(flag: bool) -> int {
    if flag { return lower_recursive_b.from_b(false) }
    else { return lower_recursive_b.from_b(true) }
}
EOF
cat >"$tmp/lower_recursive_b.n00b" <<'EOF'
import lower_recursive_a
public func from_b(flag: bool) -> int {
    if flag { return lower_recursive_a.from_a(false) }
    else { return lower_recursive_a.from_a(true) }
}
EOF
if N00B_PATH="$tmp" "$compiler" lower --dump-ir \
    "$tmp/lower_recursive_a.n00b" >"$tmp/recursive_modules.ir" 2>&1; then
    grep -q '= call @from_b' "$tmp/recursive_modules.ir"
    grep -q '= call @from_a' "$tmp/recursive_modules.ir"
else
    cat "$tmp/recursive_modules.ir"
    echo 'cross-module recursive lowering unexpectedly failed' >&2
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

cat >"$tmp/trait_receiver_modes.n00b" <<'EOF'
trait CursorOps {
    func next(modify self: `Cursor) -> `Item?
}
EOF
"$compiler" parse "$tmp/trait_receiver_modes.n00b" \
    >"$tmp/trait_receiver_modes.out"
grep -q '<trait-obj-tref>' "$tmp/trait_receiver_modes.out"
grep -q '"modify"' "$tmp/trait_receiver_modes.out"
grep -q '"?"' "$tmp/trait_receiver_modes.out"

cat >"$tmp/for_in_expression.n00b" <<'EOF'
func consume(items: list[int]) {
    for item in items { }
}
EOF
"$compiler" parse "$tmp/for_in_expression.n00b" \
    >"$tmp/for_in_expression.out" 2>&1 || {
    cat "$tmp/for_in_expression.out"
    exit 1
}
grep -q '<for-stmt>' "$tmp/for_in_expression.out" || {
    cat "$tmp/for_in_expression.out"
    echo 'for-in statement did not parse' >&2
    exit 1
}

cat >"$tmp/for_in_trait_inference.n00b" <<'EOF'
var next_item: int?
public func iter(self: int) -> string { return "cursor" }
public func next(modify self: string) -> int? { return next_item }
func call_iter(items: int) -> string { return items.iter() }
func call_next(modify cursor: string) -> int? { return cursor.next() }
func consume(items: int) {
    for item in items { let checked = item + 1 }
}
EOF
"$compiler" preprocess --dump-symbols \
    "$tmp/for_in_trait_inference.n00b" \
    >"$tmp/for_in_trait_inference.out" \
    2>"$tmp/for_in_trait_inference.err" || {
    cat "$tmp/for_in_trait_inference.out"
    cat "$tmp/for_in_trait_inference.err"
    exit 1
}
grep -q 'item: variable, mutable, type=int' \
    "$tmp/for_in_trait_inference.out" || {
    cat "$tmp/for_in_trait_inference.out"
    echo 'for-in item type was not inferred from Iterator.next' >&2
    exit 1
}
