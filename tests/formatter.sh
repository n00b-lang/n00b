#!/bin/sh
set -eu

compiler="$1"
program_dir="$(CDPATH= cd -- "$(dirname -- "$0")/programs/formatter" && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

"$compiler" parse "$program_dir/function_layout_newlines.n" \
    >"$tmp/layout.out" 2>"$tmp/layout.err"

"$compiler" fmt --output "$tmp/channels.n" \
    "$program_dir/fmt_channels.n"
cat >"$tmp/channels.expected" <<'EOF'
func short(value: int) -> int fail: string {
    return value
}

func a_very_long_function_name_that_should_wrap(first_parameter_with_a_long_name : int,
                                                second_parameter_with_a_long_name: int) ->
                                                int fail: string {
    return first_parameter_with_a_long_name
}

func elaborate(a_parameter_with_a_very_long_name      : int,
               another_parameter_with_a_very_long_name: int) ->
               Ref[AnExtraordinarilyLongNamedRecord] fail: string {
    return nil
}

func fail_only(a_parameter_with_a_very_long_name      : int,
               another_parameter_with_a_very_long_name: int) fail: string {
    fail "bad"
}

func wide_single_parameter(
    value_with_an_extremely_long_name: Ref[AnExtraordinarilyLongNamedRecord]) -> int {
    return 0
}

func initialize_array_backing(payload      : Ref[byte],
                              payload_bytes: uint,
                              type_index   : u32,
                              shape        : Ref[sys.abi.Array_Descriptor_Header]
                             ) -> bool {
    return true
}

func default_right_paren(first_parameter_with_a_long_name : int,
                         second_parameter_with_a_long_name: char = ')') ->
                         Ref[AnExtraordinarilyLongNamedRecord] {
    return nil
}

trait LongResult {
    func select(self                             : 'T,
                first_parameter_with_a_long_name : int,
                second_parameter_with_a_long_name: int) ->
                Ref[AnExtraordinarilyLongNamedRecord] fail: string
}
EOF
diff -u "$tmp/channels.expected" "$tmp/channels.n"
"$compiler" fmt --check "$tmp/channels.n"

"$compiler" fmt --output "$tmp/comments.n" \
    "$program_dir/fmt_comments.n"
cat >"$tmp/comments.expected" <<'EOF'
// top-level comment
record Commented {
    // field comment
    value: int
    /* before record closer */
}

func comment_layout(flag: bool) -> int {
    // first statement
    let value: int = 1
    if flag {
        // nested line
        /* nested block
         * second line */
        return value
        // before closer
    }
    return 0
}

when comptime() {
    /* empty body */
} else {
    require comment_layout(true) == 1
}
EOF
diff -u "$tmp/comments.expected" "$tmp/comments.n"
"$compiler" fmt --check "$tmp/comments.n"

"$compiler" fmt --output "$tmp/branches.n" \
    "$program_dir/fmt_branches.n"
diff -u "$program_dir/fmt_branches.expected" "$tmp/branches.n"
"$compiler" fmt --check "$tmp/branches.n"

cat >"$tmp/unformatted.n" <<'EOF'
func example(value:int)->int { return value }
EOF
if "$compiler" fmt --check "$tmp/unformatted.n" \
    >"$tmp/check.out" 2>"$tmp/check.err"; then
    echo 'fmt --check accepted unformatted source' >&2
    exit 1
fi
"$compiler" fmt "$tmp/unformatted.n"
"$compiler" fmt --check "$tmp/unformatted.n"

"$compiler" preprocess "$program_dir/fmt_normalize.n" \
    >"$tmp/aliases.out" 2>"$tmp/aliases.err"
"$compiler" fmt --normalize --output "$tmp/normalized.n" \
    "$program_dir/fmt_normalize.n"
"$compiler" fmt --normalize --check "$tmp/normalized.n"
grep -q 'unsigned_input: uint' "$tmp/normalized.n"
grep -q 'signed_input: int' "$tmp/normalized.n"
grep -q 'let unsigned_value: uint' "$tmp/normalized.n"
grep -q 'let signed_value: *int' "$tmp/normalized.n"
! grep -Eq '(^|[^[:alnum:]_])(u64|i64)($|[^[:alnum:]_])' \
    "$tmp/normalized.n"

cat >"$tmp/rich.n" <<'EOF2'
let a = "[:b:]bold[:/b:] «i»it«/i» \[:x:] [:odd»:]"'r
let b = "[:plain:]"
EOF2
"$compiler" fmt --output "$tmp/rich_guillemets.n" "$tmp/rich.n"
cat >"$tmp/rich_guillemets.expected" <<'EOF2'
let a = "«b»bold«/b» «i»it«/i» \[:x:] [:odd»:]"'r
let b = "[:plain:]"
EOF2
diff -u "$tmp/rich_guillemets.expected" "$tmp/rich_guillemets.n"
"$compiler" fmt --ascii-tags --output "$tmp/rich_ascii.n" "$tmp/rich.n"
cat >"$tmp/rich_ascii.expected" <<'EOF2'
let a = "[:b:]bold[:/b:] [:i:]it[:/i:] \[:x:] [:odd»:]"'r
let b = "[:plain:]"
EOF2
diff -u "$tmp/rich_ascii.expected" "$tmp/rich_ascii.n"
