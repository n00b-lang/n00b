#!/bin/sh
set -u
# The checks below match diagnostic codes, whatever the caller's environment.
export N00B_DIAGNOSTIC_CODES=1

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

setting
expect_match "lto default" '^lto = false +# default, follows build.release$'
printf '[build]\nrelease = true\n' >"$project"
setting
expect_match "lto with release" '^lto = true +# default, follows build.release$'
printf '[build]\nrelease = true\nlto = false\n' >"$project"
setting
expect_match "lto set in project" "^lto = false +# $native_project:3\$"

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

# A dev build keeps its caches under its own build identity, so a rebuilt
# compiler never reads another build's entries: cleaning removes only this
# build's directory, leaving another build's and the shared `dev` one.
caches="$tmp/caches"
mkdir -p "$caches/dev" "$caches/dev-0000000000000000"
isolated N00B_CACHE="$(native "$caches")" "$compiler" cache clean \
    >"$tmp/out" 2>&1
expect_match "cache key" \
    "^Nothing to clean: .*caches[/\\\\]dev-[0-9a-f]{16} does not exist\\.$"
grep -q 'dev-0000000000000000 does not' "$tmp/out" \
    && fail "cache key: the build used another build's cache directory"
[ -d "$caches/dev" ] && [ -d "$caches/dev-0000000000000000" ] \
    || fail "cache key: cleaning removed another build's cache"

# paths.path layers add up instead of replacing one another: flag entries
# first, then N00B_PATH, the project file and the user file. Relative entries
# in a file resolve against that file's directory.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) sep=';' ;;
    *) sep=':' ;;
esac
printf '[paths]\npath = ["user-lib"]\n' >"$user"
printf '[paths]\npath = ["project-lib"]\n' >"$project"
isolated N00B_PATH="env-a${sep}env-b" "$compiler" config --path flag-a \
    --path flag-b >"$tmp/out" 2>&1
expect_line "path layers accumulate" \
    "path = [\"flag-a\", \"flag-b\", \"env-a\", \"env-b\", \"$native_tmp/project/project-lib\", \"$native_tmp/home/.config/n00b/user-lib\"] # flag --path, environment N00B_PATH, $native_project:2, $native_user:2"
isolated N00B_PATH="env-a" "$compiler" config >"$tmp/out" 2>&1
expect_line "environment keeps file paths" \
    "path = [\"env-a\", \"$native_tmp/project/project-lib\", \"$native_tmp/home/.config/n00b/user-lib\"] # environment N00B_PATH, $native_project:2, $native_user:2"
printf '[paths]\npath = []\n' >"$project"
setting
expect_line "an empty project list keeps the user paths" \
    "path = [\"$native_tmp/home/.config/n00b/user-lib\"] # $native_user:2"

# paths.contract_dirs accumulates the same way. A missing directory is
# reported once, from the highest-precedence layer naming it, and each
# report names its own layer.
printf '[paths]\ncontract_dirs = ["user-contracts"]\n' >"$user"
printf '[paths]\ncontract_dirs = ["shared", "shared"]\n' >"$project"
n00b config --contract-dir flag-contracts \
    --contract-dir "$tmp/project/shared" >"$tmp/out" 2>&1
expect_line "contract dirs accumulate" \
    "contract_dirs = [\"flag-contracts\", \"$native_tmp/project/shared\", \"$native_tmp/project/shared\", \"$native_tmp/project/shared\", \"$native_tmp/home/.config/n00b/user-contracts\"] # flag --contract-dir, $native_project:2, $native_user:2"
printf 'fn main() {\n}\n' >"$tmp/project/src/main.n"
# The two checks of main.n below would compile the standard library into
# the isolated home's empty cache. Where the cache is isn't what they test,
# so they use the caller's cache when it has one.
set --
[ -n "${N00B_CACHE:-}" ] && set -- N00B_CACHE="$N00B_CACHE"
isolated "$@" "$compiler" check --contract-dir flag-contracts \
    --contract-dir "$tmp/project/shared" main.n >"$tmp/out" 2>&1
expect_line "contract dir from a flag" \
    "warning N00B-C0034: \`--contract-dir flag-contracts\` does not exist, so it is skipped"
expect_line "contract dir named by a flag and the project file" \
    "warning N00B-C0034: \`--contract-dir $native_tmp/project/shared\` does not exist, so it is skipped"
expect_line "contract dir from the user file" \
    "warning N00B-C0034: contract directory \`$native_tmp/home/.config/n00b/user-contracts\` from \`paths.contract_dirs\` ($native_user:2) does not exist, so it is skipped"
[ "$(grep -c 'N00B-C0034' "$tmp/out")" -eq 3 ] \
    || fail "contract dirs: a directory named twice was reported twice"

# A missing paths.path entry named by several layers is warned about once,
# naming the highest-precedence layer.
printf '[paths]\npath = ["%s"]\n' "$tmp/project/src/missing-lib" >"$project"
# (The path settings are resolved, and their warnings given, before any
# command reads its input, so `parse` reports exactly what `check` does
# here, without loading the library.)
isolated N00B_PATH="missing-lib${sep}missing-lib" "$compiler" parse \
    --path missing-lib main.n >"$tmp/out" 2>&1
expect_line "missing path entry" \
    "warning N00B-C0033: \`--path\` entry \`missing-lib\` does not exist, so it is skipped"
[ "$(grep -c 'N00B-C0033' "$tmp/out")" -eq 1 ] \
    || fail "paths.path: a missing entry named twice was reported twice"

[ "$failed" -eq 0 ] && echo "config layering: all checks passed"
[ "$failed" -eq 0 ]
