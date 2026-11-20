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

# Each run of the compiler costs more than formatting a file, so the cases
# are formatted together, in place on copies, in one run (`--output` takes
# one input; channels and comments above use it), each then diffed against
# its .expected. fmt_cases.n holds every case that changes something but
# two: a package body (one package to a file) and the return skin, which is
# normalized below too. One `fmt --check` run at the end checks every file
# that must already be formatted; it fails if any is not.
cases="cases package_body return_skin"
mkdir "$tmp/cases"
for name in $cases; do
    cp "$program_dir/fmt_$name.n" "$tmp/cases/$name.n"
done
# Region anchors are trivia, not comments, and once were dropped.
cp "$program_dir/../langtools/samples/sample.n" "$tmp/cases/sample.n"
"$compiler" fmt "$tmp/cases/"*.n
for name in $cases; do
    diff -u "$program_dir/fmt_$name.expected" "$tmp/cases/$name.n"
done
for anchor in '#[setup#' '#]setup#'; do
    grep -qxF "$anchor" "$tmp/cases/sample.n"
done

# Already formatted: these results (formatting them again leaves them as
# they are), and fmt_stable.n, the cases formatting must leave alone.
set -- "$tmp/channels.n" "$tmp/comments.n" "$tmp/cases/"*.n \
    "$program_dir/fmt_stable.n"

cat >"$tmp/unformatted.n" <<'EOF'
func example(value:int)->int { return value }
EOF
if "$compiler" fmt --check "$tmp/unformatted.n" \
    >"$tmp/check.out" 2>"$tmp/check.err"; then
    echo 'fmt --check accepted unformatted source' >&2
    exit 1
fi
"$compiler" fmt "$tmp/unformatted.n"
set -- "$@" "$tmp/unformatted.n"

# Rich-string markup is plain `[...]` text to the formatter: it passes
# through byte for byte, escapes included (so --check accepts it as it is).
cat >"$tmp/rich.n" <<'EOF2'
let a = "[b]bold[/b] [i]it[/] \[x] [#0:a\]b] [1, 2]"'r
let b = "[#name:>4] \[\] [bold"'rich
let c = "[plain] \[ \]"
EOF2
set -- "$@" "$tmp/rich.n"

"$compiler" fmt --check "$@"

# --normalize types the program, which costs a full check, so the two
# normalized cases share one run, in place on copies.
mkdir "$tmp/normalize"
cp "$program_dir/fmt_return_skin.n" "$tmp/normalize/return_skin.n"
cp "$program_dir/fmt_normalize.n" "$tmp/normalized.n"
"$compiler" preprocess "$program_dir/fmt_normalize.n" \
    >"$tmp/aliases.out" 2>"$tmp/aliases.err"
"$compiler" fmt --normalize "$tmp/normalize/return_skin.n" "$tmp/normalized.n"
diff -u "$program_dir/fmt_return_skin.expected" \
    "$tmp/normalize/return_skin.n"
"$compiler" fmt --normalize --check "$tmp/normalized.n"
grep -q 'unsigned_input: uint' "$tmp/normalized.n"
grep -q 'signed_input: int' "$tmp/normalized.n"
grep -q 'let unsigned_value: uint' "$tmp/normalized.n"
grep -q 'let signed_value: *int' "$tmp/normalized.n"
if grep -Eq '(^|[^[:alnum:]_])(u64|i64)($|[^[:alnum:]_])' \
    "$tmp/normalized.n"; then
    echo 'normalized source still names u64 or i64' >&2
    exit 1
fi

# Normalizing writes only types the source can say: the result is a
# program that checks, and normalizes to itself.
"$compiler" fmt --normalize --output "$tmp/unspellable.n" \
    "$program_dir/fmt_normalize_unspellable.n"
"$compiler" fmt --normalize --check "$tmp/unspellable.n"
"$compiler" check "$tmp/unspellable.n" >/dev/null 2>&1
grep -q 'let s: Box = cast\[Scales\]' "$tmp/unspellable.n"
grep -q 'let first = o.opened()' "$tmp/unspellable.n"
grep -q 'let x = either(flag)' "$tmp/unspellable.n"
