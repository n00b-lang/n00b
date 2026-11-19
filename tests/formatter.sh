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

"$compiler" fmt --output "$tmp/return_skin.n" \
    "$program_dir/fmt_return_skin.n"
diff -u "$program_dir/fmt_return_skin.expected" "$tmp/return_skin.n"
"$compiler" fmt --check "$tmp/return_skin.n"
"$compiler" fmt --normalize --output "$tmp/return_skin_normalized.n" \
    "$program_dir/fmt_return_skin.n"
diff -u "$program_dir/fmt_return_skin.expected" \
    "$tmp/return_skin_normalized.n"

for name in block_bodies package_body where_clauses comment_blank_lines casts \
        not_operator sized_ints empty_result; do
    "$compiler" fmt --output "$tmp/$name.n" "$program_dir/fmt_$name.n"
    diff -u "$program_dir/fmt_$name.expected" "$tmp/$name.n"
    "$compiler" fmt --check "$program_dir/fmt_$name.expected"
done

"$compiler" fmt --output "$tmp/trait_receivers.n" \
    "$program_dir/fmt_trait_receivers.n"
diff -u "$program_dir/fmt_trait_receivers.expected" "$tmp/trait_receivers.n"
"$compiler" fmt --check "$program_dir/fmt_trait_receivers.expected"

"$compiler" fmt --output "$tmp/line_comments.n" \
    "$program_dir/fmt_line_comments.n"
diff -u "$program_dir/fmt_line_comments.expected" "$tmp/line_comments.n"
"$compiler" fmt --output "$tmp/line_comments_again.n" "$tmp/line_comments.n"
diff -u "$tmp/line_comments.n" "$tmp/line_comments_again.n"

"$compiler" fmt --output "$tmp/variants.n" "$program_dir/fmt_variants.n"
diff -u "$program_dir/fmt_variants.expected" "$tmp/variants.n"
"$compiler" fmt --check "$program_dir/fmt_variants.expected"

"$compiler" fmt --output "$tmp/default_params.n" \
    "$program_dir/fmt_default_params.n"
diff -u "$program_dir/fmt_default_params.expected" "$tmp/default_params.n"
"$compiler" fmt --check "$program_dir/fmt_default_params.expected"

"$compiler" fmt --output "$tmp/value_sigils.n" \
    "$program_dir/fmt_value_sigils.n"
diff -u "$program_dir/fmt_value_sigils.expected" "$tmp/value_sigils.n"

"$compiler" fmt --output "$tmp/try_bodies.n" "$program_dir/fmt_try_bodies.n"
diff -u "$program_dir/fmt_try_bodies.expected" "$tmp/try_bodies.n"
"$compiler" fmt --check "$program_dir/fmt_try_bodies.expected"

"$compiler" fmt --output "$tmp/for_bindings.n" \
    "$program_dir/fmt_for_bindings.n"
diff -u "$program_dir/fmt_for_bindings.expected" "$tmp/for_bindings.n"
"$compiler" fmt --check "$program_dir/fmt_for_bindings.expected"

"$compiler" fmt --output "$tmp/package_exports.n" \
    "$program_dir/fmt_package_exports.n"
diff -u "$program_dir/fmt_package_exports.expected" "$tmp/package_exports.n"
"$compiler" fmt --check "$program_dir/fmt_package_exports.expected"

"$compiler" fmt --output "$tmp/try_handlers.n" \
    "$program_dir/fmt_try_handlers.n"
diff -u "$program_dir/fmt_try_handlers.expected" "$tmp/try_handlers.n"
"$compiler" fmt --check "$program_dir/fmt_try_handlers.expected"

"$compiler" fmt --output "$tmp/function_spellings.n" \
    "$program_dir/fmt_function_spellings.n"
diff -u "$program_dir/fmt_function_spellings.expected" \
    "$tmp/function_spellings.n"
"$compiler" fmt --check "$tmp/function_spellings.n"

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

# Rich-string markup is plain `[...]` text to the formatter: it passes
# through byte for byte, escapes included.
cat >"$tmp/rich.n" <<'EOF2'
let a = "[b]bold[/b] [i]it[/] \[x] [#0:a\]b] [1, 2]"'r
let b = "[#name:>4] \[\] [bold"'rich
let c = "[plain] \[ \]"
EOF2
"$compiler" fmt --output "$tmp/rich_formatted.n" "$tmp/rich.n"
diff -u "$tmp/rich.n" "$tmp/rich_formatted.n"
if "$compiler" fmt --ascii-tags --output "$tmp/rich_ascii.n" "$tmp/rich.n" \
    >/dev/null 2>&1; then
    echo 'fmt unexpectedly accepted the removed --ascii-tags flag' >&2
    exit 1
fi
