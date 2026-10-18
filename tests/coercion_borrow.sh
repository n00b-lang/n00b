#!/bin/sh
set -eu

compiler=$1
program=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

"$compiler" build --output "$tmp/program" "$program" >"$tmp/build" 2>&1 || {
    cat "$tmp/build" >&2
    exit 1
}
"$tmp/program"
"$compiler" lower --dump-ir "$program" >"$tmp/ir" 2>"$tmp/lower" || {
    cat "$tmp/lower" >&2
    exit 1
}

body() {
    awk -v name="$1" '
        $0 ~ "func @" name "\\(" { inside = 1 }
        inside { print }
        inside && /^  }/ { exit }
    ' "$tmp/ir"
}

ordered() {
    function_name=$1
    shift
    body "$function_name" >"$tmp/body"
    line=0
    for pattern do
        found=$(awk -v start="$line" -v pattern="$pattern" '
            NR > start && index($0, pattern) { print NR; exit }
        ' "$tmp/body")
        if [ -z "$found" ]; then
            echo "$function_name: expected '$pattern' after line $line" >&2
            cat "$tmp/body" >&2
            exit 1
        fi
        line=$found
    done
}

ordered call_argument 'buffer.as_string' 'call @measure' 'buffer.borrow_end'
ordered local_then_other_write 'buffer.as_string' 'call @byte_len' \
    'buffer.borrow_end' 'call @__add_assign'
ordered distinct_concat 'buffer.as_string' 'call @concat' 'buffer.borrow_end'
ordered out_view 'buffer.as_string' 'call @forward' 'call.output' \
    'call @byte_len' 'buffer.borrow_end' 'call @__add_assign'
ordered reassign_global 'buffer.as_string %__n00bb_borrow_source' \
    'global.store' 'buffer.borrow_end %__n00bb_borrow_source'

body append_in_place >"$tmp/body"
if [ "$(grep -c 'call @__add_assign' "$tmp/body")" != 1 ] \
        || grep -q 'call @__add,' "$tmp/body"; then
    echo "append_in_place: += must lower to one __add_assign" >&2
    cat "$tmp/body" >&2
    exit 1
fi

for function_name in call_argument local_then_other_write distinct_concat \
        out_view reassign_global; do
    body "$function_name" >"$tmp/body"
    begins=$(grep -c 'buffer.as_string' "$tmp/body" || true)
    ends=$(grep -c 'buffer.borrow_end' "$tmp/body" || true)
    if [ "$begins" != "$ends" ]; then
        echo "$function_name: $begins borrows but $ends borrow ends" >&2
        exit 1
    fi
done
