#!/bin/sh
set -eu
# The checks below match diagnostic codes, whatever the caller's environment.
export N00B_DIAGNOSTIC_CODES=1

compiler=$1
program=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

"$compiler" lower "$program" >/dev/null 2>"$tmp/output"
if ! grep -Fq "global \`shared\` is written by thread entry \`worker\`" "$tmp/output" \
    || ! grep -Fq "bitcode_thread_races.n:8:5" "$tmp/output" \
    || [ "$(grep -c "N00B-S0060" "$tmp/output")" != 1 ]; then
    cat "$tmp/output" >&2
    exit 1
fi

# A race depends on the whole program's threads, not just the module it is
# in: a module reused from the cache gets the race the same program would
# report compiled from scratch, whether its cached copy came from a program
# with the thread or without it.
mkdir "$tmp/race"
cat >"$tmp/race/helper.n" <<'EOF'
import sys.threads

var shared: int = 0

func worker() {
    shared += 1
}
func bump() {
    shared += 1
}
EOF
printf 'import helper\nhelper.bump()\nhelper.worker()\n' >"$tmp/race/plain.n"
printf '%s\n' 'import helper' 'import sys.threads' \
    'require sys.threads.thread_start(callback[sys.threads.Thread_Entry](ref helper.worker))' \
    'helper.bump()' >"$tmp/race/threaded.n"
race_check() {
    cp "$tmp/race/$1.n" "$tmp/race/main.n"
    (cd "$tmp/race" && N00B_DIAGNOSTICS=terse "$compiler" check main.n) \
        >"$tmp/race/$2" 2>&1 || true
}
race_check threaded threaded.cold
race_check plain plain.warm
race_check threaded threaded.warm
rm -f "$tmp/race/main.n"
# helper.n is cached from the threaded program; a plain program must not
# get its race, and a fresh copy of the plain program agrees.
cmp -s "$tmp/race/threaded.cold" "$tmp/race/threaded.warm" || {
    echo "the race in a cached module differs from the cold check" >&2
    diff -u "$tmp/race/threaded.cold" "$tmp/race/threaded.warm" >&2
    exit 1
}
grep -q 'N00B-S0060' "$tmp/race/threaded.warm" || {
    cat "$tmp/race/threaded.warm" >&2
    echo "no race reported in the cached module" >&2
    exit 1
}
if grep -q 'N00B-S0060' "$tmp/race/plain.warm"; then
    cat "$tmp/race/plain.warm" >&2
    echo "a race kept from another program was reported" >&2
    exit 1
fi
