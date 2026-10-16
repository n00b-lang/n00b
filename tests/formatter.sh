#!/bin/sh
set -eu

compiler="$1"
program_dir="$(CDPATH= cd -- "$(dirname -- "$0")/programs/formatter" && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

"$compiler" parse "$program_dir/function_layout_newlines.n00b" \
    >"$tmp/layout.out" 2>"$tmp/layout.err"

"$compiler" fmt --output "$tmp/channels.n00b" \
    "$program_dir/fmt_channels.n00b"
cat >"$tmp/channels.expected" <<'EOF'
func short(value: int) -> int fail: string {
    return value
}

func a_very_long_function_name_that_should_wrap(first_parameter_with_a_long_name : int,
                                                second_parameter_with_a_long_name: int)
                                               -> int fail: string {
    return first_parameter_with_a_long_name
}

func elaborate(a_parameter_with_a_very_long_name      : int,
               another_parameter_with_a_very_long_name: int)
              -> Ref[AnExtraordinarilyLongNamedRecord] fail: string {
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

trait LongResult {
    func select(self                             : `T,
                first_parameter_with_a_long_name : int,
                second_parameter_with_a_long_name: int)
               -> Ref[AnExtraordinarilyLongNamedRecord] fail: string
}
EOF
diff -u "$tmp/channels.expected" "$tmp/channels.n00b"
"$compiler" fmt --check "$tmp/channels.n00b"

"$compiler" fmt --output "$tmp/comments.n00b" \
    "$program_dir/fmt_comments.n00b"
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
diff -u "$tmp/comments.expected" "$tmp/comments.n00b"
"$compiler" fmt --check "$tmp/comments.n00b"

"$compiler" fmt --output "$tmp/branches.n00b" \
    "$program_dir/fmt_branches.n00b"
diff -u "$program_dir/fmt_branches.expected" "$tmp/branches.n00b"
"$compiler" fmt --check "$tmp/branches.n00b"

cat >"$tmp/unformatted.n00b" <<'EOF'
func example(value:int)->int { return value }
EOF
if "$compiler" fmt --check "$tmp/unformatted.n00b" \
    >"$tmp/check.out" 2>"$tmp/check.err"; then
    echo 'fmt --check accepted unformatted source' >&2
    exit 1
fi
"$compiler" fmt "$tmp/unformatted.n00b"
"$compiler" fmt --check "$tmp/unformatted.n00b"

"$compiler" preprocess "$program_dir/fmt_normalize.n00b" \
    >"$tmp/aliases.out" 2>"$tmp/aliases.err"
"$compiler" fmt --normalize --output "$tmp/normalized.n00b" \
    "$program_dir/fmt_normalize.n00b"
"$compiler" fmt --normalize --check "$tmp/normalized.n00b"
grep -q 'unsigned_input: uint' "$tmp/normalized.n00b"
grep -q 'signed_input: int' "$tmp/normalized.n00b"
grep -q 'let unsigned_value: uint' "$tmp/normalized.n00b"
grep -q 'let signed_value: *int' "$tmp/normalized.n00b"
! grep -Eq '(^|[^[:alnum:]_])(u64|i64)($|[^[:alnum:]_])' \
    "$tmp/normalized.n00b"
