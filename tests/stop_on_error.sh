#!/bin/sh
# `--stop-on-error`: each file's diagnostics stop after that many errors
# (5 by default), and a note says how many were left out.
set -eu

compiler=$1
src=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

export N00B_DIAGNOSTICS=terse N00B_DIAGNOSTIC_CODES=1
export XDG_CONFIG_HOME="$tmp/config"
# Windows reads the user file from %APPDATA% instead.
case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) APPDATA=$(cygpath -m "$tmp/config"); export APPDATA ;;
esac
cd "$src/tests/programs/stop_on_error"

# Runs `n00b check` with the given arguments; its stderr and exit status go
# to $tmp/out.
check() {
    status=0
    "$compiler" check "$@" >/dev/null 2>"$tmp/out" || status=$?
    echo "exit $status" >>"$tmp/out"
}

expect() {
    if ! diff -u "$tmp/expected" "$tmp/out"; then
        echo "stop_on_error: unexpected output for $1" >&2
        exit 1
    fi
}

seven() {
    i=1
    while [ "$i" -le "$1" ]; do
        echo "seven_errors.n:$i:15: error N00B-T0500: \`\"$i\"\` is \`string\`, which does not convert to \`int\`"
        i=$((i + 1))
    done
}

six() {
    i=1
    while [ "$i" -le "$1" ]; do
        echo "six_errors.n:$i:16: error N00B-T0500: \`$i\` is an integer, which does not convert to \`bool\`"
        i=$((i + 1))
    done
}

omitted() {
    echo "$1: note N00B-C0106: this file reached the error limit of $2; errors not shown: $3 (\`--stop-on-error=all\` shows every one)"
}

# The default limit is 5.
check seven_errors.n
{ seven 5; omitted seven_errors.n 5 2; echo "exit 1"; } >"$tmp/expected"
expect "the default limit"

# An explicit limit, for each of several inputs.
check --stop-on-error=2 seven_errors.n six_errors.n
{ seven 2; omitted seven_errors.n 2 5; six 2; omitted six_errors.n 2 4
  echo "exit 1"; } >"$tmp/expected"
expect "--stop-on-error=2 on two inputs"

# `all` shows every error, with no note.
check --stop-on-error=all seven_errors.n
{ seven 7; echo "exit 1"; } >"$tmp/expected"
expect "--stop-on-error=all"

# A file under the limit is unchanged.
check --stop-on-error=7 seven_errors.n
{ seven 7; echo "exit 1"; } >"$tmp/expected"
expect "--stop-on-error=7"

# The limit is per file: an imported module and its importer each stop.
check --stop-on-error=3 importer.n
{ i=1; while [ "$i" -le 3 ]; do
      echo "helper.n:$i:15: error N00B-T0500: \`\"$i\"\` is \`string\`, which does not convert to \`int\`"
      i=$((i + 1)); done
  i=1; while [ "$i" -le 3 ]; do
      echo "importer.n:$((i + 1)):16: error N00B-T0500: \`$i\` is an integer, which does not convert to \`bool\`"
      i=$((i + 1)); done
  omitted helper.n 3 3; omitted importer.n 3 3; echo "exit 1"; } >"$tmp/expected"
expect "an imported module and its importer"

# The setting, and its layering under the flag.
mkdir -p "$tmp/config/n00b"
printf '[diagnostics]\nstop_on_error = 3\n' >"$tmp/config/n00b/config.cfg"
check seven_errors.n
{ seven 3; omitted seven_errors.n 3 4; echo "exit 1"; } >"$tmp/expected"
expect "diagnostics.stop_on_error = 3"
check --stop-on-error=all seven_errors.n
{ seven 7; echo "exit 1"; } >"$tmp/expected"
expect "the flag over the setting"
printf '[diagnostics]\nstop_on_error = "all"\n' >"$tmp/config/n00b/config.cfg"
check seven_errors.n
{ seven 7; echo "exit 1"; } >"$tmp/expected"
expect "diagnostics.stop_on_error = \"all\""
rm "$tmp/config/n00b/config.cfg"

# Values that are neither a number from 1 nor `all` are refused.
for value in 0 lots; do
    check --stop-on-error="$value" seven_errors.n
    { echo "error N00B-C0011: \`--stop-on-error\` takes a whole number from 1, or \`all\`, not \`$value\`"
      echo "exit 2"; } >"$tmp/expected"
    expect "--stop-on-error=$value"
done
