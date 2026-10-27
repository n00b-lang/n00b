#!/bin/sh
set -u

compiler=$1
src=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cd "$src" || exit 1
native_root=$(pwd -W 2>/dev/null || pwd)
export N00B_DIAGNOSTICS=terse
mkdir -p "$tmp/raw" "$tmp/actual"

key() {
    printf '%s' "$1" | tr / _
}

modules() {
    name=${1##*/}
    printf '%s' "tests/diagnostics/modules/${name%.n}"
}

run_cases() {
    program=$1
    binary="$tmp/$(key "$program").bin"
    "$compiler" build --output "$binary" "$program" >/dev/null \
        2>"$tmp/raw/$(key "${program%.n}")"
    for expected in "${program%.n}".*.expected; do
        [ -f "$expected" ] || continue
        stem=${expected%.expected}
        raw="$tmp/raw/$(key "$stem")"
        "$binary" "${stem##*.}" >/dev/null 2>"$raw"
        echo $? >"$raw.status"
    done
}

checked=
for program in tests/diagnostics/*/*.n; do
    raw="$tmp/raw/$(key "${program%.n}")"
    case "$program" in
    tests/diagnostics/runtime/*)
        run_cases "$program" & ;;
    tests/diagnostics/build/*)
        "$compiler" build --output "$tmp/$(key "$program").bin" "$program" \
            >/dev/null 2>"$raw" & ;;
    *)
        if [ -d "$(modules "$program")" ]; then
            N00B_PATH=$(modules "$program") "$compiler" check \
                --no-comptime "$program" >/dev/null 2>"$raw" &
        else
            checked="$checked $program"
        fi ;;
    esac
done
for layout in tests/diagnostics/*/*.layout.expected; do
    [ -f "$layout" ] || continue
    stem=${layout%.expected}
    N00B_DIAGNOSTICS= NO_COLOR=1 "$compiler" check --no-comptime \
        "${stem%.layout}.n" >/dev/null 2>"$tmp/raw/$(key "$stem")" &
done
for package in tests/diagnostics/packages/*/; do
    package=${package%/}
    N00B_PATH=tests/diagnostics/packages "$compiler" check --no-comptime \
        "${package##*/}.facade" >/dev/null 2>"$tmp/raw/$(key "$package")" &
done
"$compiler" check --no-comptime $checked >"$tmp/stdout" 2>"$tmp/check"
checked_status=$?
awk -v dir="$tmp/raw" '
    /^[^ ]/ {
        split($0, part, ":")
        name = part[1]
        sub(/\.n$/, "", name)
        gsub("/", "_", name)
    }
    name != "" { print > (dir "/" name) }
' "$tmp/check"
wait

failed=0
count=0
if [ "$checked_status" != 1 ]; then
    echo "n00b check exited $checked_status over the corpus, expected 1" >&2
    failed=$((failed + 1))
fi
for program in tests/diagnostics/*/*.n; do
    [ -f "${program%.n}.expected" ] && continue
    echo "missing ${program%.n}.expected" >&2
    failed=$((failed + 1))
done
for expected in tests/diagnostics/*/*.expected; do
    stem=${expected%.expected}
    raw="$tmp/raw/$(key "$stem")"
    actual="$tmp/actual/$(key "$stem")"
    [ -f "$raw" ] || : >"$raw"
    awk -v root="$PWD/" -v native="$native_root/" '{
        while ((at = index($0, root)) > 0)
            $0 = substr($0, 1, at - 1) substr($0, at + length(root))
        while ((at = index($0, native)) > 0)
            $0 = substr($0, 1, at - 1) substr($0, at + length(native))
        print
    }' "$raw" >"$actual"
    count=$((count + 1))
    if [ "${N00B_UPDATE_GOLDENS:-0}" = 1 ]; then
        cp "$actual" "$expected"
        continue
    fi
    if ! diff -u "$expected" "$actual" >"$tmp/diff"; then
        echo "mismatch: $stem" >&2
        cat "$tmp/diff" >&2
        failed=$((failed + 1))
        continue
    fi
    if [ -f "$raw.status" ] && [ "$(cat "$raw.status")" != 101 ]; then
        echo "exit status $(cat "$raw.status"), expected 101: $stem" >&2
        failed=$((failed + 1))
    fi
done
echo "$count diagnostic goldens checked, $failed failed"
[ "$failed" -eq 0 ]
