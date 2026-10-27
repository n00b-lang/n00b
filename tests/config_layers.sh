#!/bin/sh
set -u

compiler=$1
tmp=$(cd "$(mktemp -d)" && pwd -P)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
failed=0

native() {
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*) cygpath -m "$1" ;;
        *) printf '%s\n' "$1" ;;
    esac
}

fail() {
    echo "FAIL  $1" >&2
    [ -f "$tmp/out" ] && sed 's/^/      /' "$tmp/out" >&2
    failed=$((failed + 1))
}

isolated() {
    env -u N00B_CACHE -u N00B_DIAGNOSTICS -u N00B_PATH -u N00B_CLANG \
        -u NO_COLOR -u XDG_CONFIG_HOME HOME="$tmp/home" \
        APPDATA="$(native "$tmp/home/.config")" "$@"
}

n00b() {
    isolated "$compiler" "$@"
}

expect_line() {
    grep -qxF -- "$2" "$tmp/out" || fail "$1: expected the line '$2'"
}

expect_match() {
    grep -qE -- "$2" "$tmp/out" || fail "$1: expected a line matching '$2'"
}

user="$tmp/home/.config/n00b/config.cfg"
project="$tmp/project/n00b.cfg"
native_tmp=$(native "$tmp")
native_user=$(native "$user")
native_project=$(native "$project")
mkdir -p "$tmp/home/.config/n00b" "$tmp/project/src"
cd "$tmp/project/src" || exit 1

setting() {
    n00b config "$@" >"$tmp/out" 2>&1
}

setting
expect_match "default" '^cache = "" +# default, chosen automatically$'
expect_match "default" '^jobs = 4 +# default$'
expect_match "default" '^hints = true +# default$'

printf '[paths]\ncache = "user-cache"\n[build]\njobs = 2\n' >"$user"
setting
expect_match "user file" "^cache = \"$native_tmp/home/.config/n00b/user-cache\" +# $native_user:2\$"
expect_match "user file" "^jobs = 2 +# $native_user:4\$"

printf '[paths]\ncache = "project-cache"\n' >"$project"
setting
expect_match "project file" "^cache = \"$native_tmp/project/project-cache\" +# $native_project:2\$"
expect_match "project file keeps other user settings" "^jobs = 2 +# $native_user:4\$"

isolated N00B_CACHE=env-cache "$compiler" config >"$tmp/out" 2>&1
expect_match "environment" '^cache = "env-cache" +# environment N00B_CACHE$'

isolated N00B_CACHE=env-cache "$compiler" config --cache flag-cache -j 7 \
    --no-hints >"$tmp/out" 2>&1
expect_match "flag" '^cache = "flag-cache" +# flag --cache$'
expect_match "flag" '^jobs = 7 +# flag --jobs$'
expect_match "flag" '^hints = false +# flag --no-hints$'

printf '[build]\njob = 3\n[diagnostcs]\nterse = true\n' >"$project"
n00b --terse-diagnostics config >"$tmp/out" 2>&1
status=$?
[ "$status" -eq 0 ] || fail "unknown keys: warnings must not fail the command"
expect_line "unknown key" \
    "$native_project:2:1: warning N00B-C0102: there is no setting named \`job\` in \`[build]\`"
expect_line "unknown key" "  help: did you mean \`jobs\`?"
expect_line "unknown section" \
    "$native_project:3:2: warning N00B-C0102: there is no settings section named \`diagnostcs\`"

printf '[build]\njobs = "8"\n' >"$project"
printf 'fn main() {\n}\n' >"$tmp/project/src/main.n"
n00b --terse-diagnostics build --output "$tmp/main" main.n >"$tmp/out" 2>&1
status=$?
[ "$status" -eq 1 ] || fail "type error: expected the build to stop with status 1, got $status"
expect_line "type error" \
    "$native_project:2:8: error N00B-C0103: \`build.jobs\` must be a whole number, not a string"
expect_line "type error" "  help: write a number without quotes, as in \`jobs = 4\`"

printf '[build]\njobs = 4\njobs = 5\n' >"$project"
n00b --terse-diagnostics config >"$tmp/out" 2>&1
expect_line "redefinition" \
    "$native_project:3:1: error N00B-C0101: \`jobs\` is already defined"
expect_line "redefinition" "  $native_project:2:1: note: first defined here"

[ "$failed" -eq 0 ] && echo "config layering: all checks passed"
[ "$failed" -eq 0 ]
