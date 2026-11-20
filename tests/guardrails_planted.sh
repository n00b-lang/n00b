#!/bin/sh
set -u

src=$1
tools="$src/tools"
work=$(mktemp -d "${TMPDIR:-/tmp}/guardrails.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT
status=0

fixture() {
    rm -rf "${work:?}/$1"
    mkdir -p "$work/$1/tests" "$work/$1/compiler" "$work/$1/abi" "$work/$1/lib"
}

expect() {
    want=$1
    label=$2
    shift 2
    out=$("$@" 2>&1)
    got=$?
    if printf '%s\n' "$out" | grep -q '^Traceback'; then
        echo "$label: the check crashed"
        echo "$out"
        status=1
    elif [ "$want" = fail ] && [ "$got" -eq 0 ]; then
        echo "$label: the check passed a planted violation"
        echo "$out"
        status=1
    elif [ "$want" = pass ] && [ "$got" -ne 0 ]; then
        echo "$label: the check failed a clean tree"
        echo "$out"
        status=1
    else
        echo "ok: $label"
    fi
}

check() {
    python3 "$tools/check_$1.py" "$work/$2"
}

for part in "$src"/tests/guardrails/*.sh; do
    . "$part"
done
exit "$status"
