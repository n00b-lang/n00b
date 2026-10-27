#!/bin/sh
set -eu

compiler=$1
program=$2
shift 2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

(cd "$(dirname "$program")" \
    && "$compiler" lower --dump-ranges "$(basename "$program")") \
    >"$tmp/output" 2>"$tmp/errors"
failed=0
for module in "$@"; do
    grep "^range $module " "$tmp/output" >"$tmp/pairs" || true
    if ! test -s "$tmp/pairs"; then
        echo "$module: no shared range pairs" >&2
        cat "$tmp/errors" >&2
        failed=1
        continue
    fi
    if awk '
        function compare(a, b,    na, nb, r) {
            na = a ~ /^-/
            nb = b ~ /^-/
            if (na != nb) return na ? -1 : 1
            if (na) { a = substr(a, 2); b = substr(b, 2) }
            r = length(a) < length(b) ? -1 : length(a) > length(b) ? 1 \
                : (a "") < (b "") ? -1 : (a "") > (b "") ? 1 : 0
            return na ? -r : r
        }
        function bounded(lo, hi, tlo, thi) {
            return compare(lo, tlo) > 0 || compare(hi, thi) < 0
        }
        bounded($5, $6, $11, $12) != bounded($8, $9, $11, $12) {
            print "only one analysis bounds: " $0
            bad = 1
        }
        END { exit !bad }' "$tmp/pairs" >&2; then
        failed=1
    fi
done
exit "$failed"
