#!/bin/sh
set -eu
# The checks below match diagnostic codes, whatever the caller's environment.
export N00B_DIAGNOSTIC_CODES=1

compiler=$1
root=$2
tmp=$(cd "$(mktemp -d)" && pwd -P)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

mkdir "$tmp/elsewhere"
cd "$tmp/elsewhere"
if ! N00B_PATH="$root" "$compiler" build --output "$tmp/main" main \
    >"$tmp/build.log" 2>&1; then
    cat "$tmp/build.log" >&2
    exit 1
fi
"$tmp/main"

mkdir -p "$tmp/project/a/b" "$tmp/project/a/gen/x" "$tmp/project/build/x"
cd "$tmp/project"
printf 'require 1 == 1\n' >a/b/mod.n
printf 'require 1 == 1\n' >a/gen/x/hidden.n
printf 'require 1 == 1\n' >build/x/hidden.n
echo generated >a/gen/notes.txt
echo generated >build/notes.txt
for cache in on off; do
    if [ "$cache" = on ]; then
        run() { env -u N00B_NO_PACKAGE_CACHE "$compiler" "$@"; }
    else
        run() { N00B_NO_PACKAGE_CACHE=1 "$compiler" "$@"; }
    fi
    if ! run check a.b.mod >"$tmp/deep.log" 2>&1; then
        cat "$tmp/deep.log" >&2
        exit 1
    fi
    for operand in build.x.hidden a.gen.x.hidden; do
        if run check "$operand" >"$tmp/hidden.log" 2>&1; then
            echo "cache $cache: $operand found under a non-source directory" >&2
            exit 1
        fi
        grep -Fq "cannot find \`$operand\`" "$tmp/hidden.log" || {
            cat "$tmp/hidden.log" >&2
            exit 1
        }
    done
done

mkdir "$tmp/outside"
if N00B_PATH=../outside "$compiler" check a.b.mod \
    >"$tmp/outside.log" 2>&1; then
    echo 'relative N00B_PATH entry outside the current directory accepted' >&2
    exit 1
fi
grep -Fq 'N00B-B0024' "$tmp/outside.log" || {
    cat "$tmp/outside.log" >&2
    exit 1
}

printf 'import sys.env\nrequire sys.env.args().len() > 0\n' >"$tmp/args.n"
for place in "$root/../../../lib/sys" "$tmp"; do
    if ! (cd "$place" && "$compiler" check "$tmp/args.n") \
            >"$tmp/nested.log" 2>&1; then
        echo "checking from $place failed" >&2
        cat "$tmp/nested.log" >&2
        exit 1
    fi
done

# Every paths.path layer contributes roots, highest precedence first: a module
# only on N00B_PATH is still found when --path is given, and the project and
# user files add their entries too. When two layers hold the same module, the
# --path copy wins.
layers="$tmp/layers"
mkdir -p "$layers/flag" "$layers/env" "$layers/home/.config/n00b" \
    "$layers/user" "$layers/project/plib"
printf 'func pick() -> int { return 1 }\n' >"$layers/flag/both.n"
printf 'func pick() -> int { return 2 }\n' >"$layers/env/both.n"
printf 'func e() -> int { return 3 }\n' >"$layers/env/envonly.n"
printf 'func p() -> int { return 4 }\n' >"$layers/project/plib/projonly.n"
printf 'func u() -> int { return 5 }\n' >"$layers/user/useronly.n"
printf '[paths]\npath = ["plib"]\n' >"$layers/project/n00b.cfg"
printf '[paths]\npath = ["%s"]\n' "$layers/user" \
    >"$layers/home/.config/n00b/config.cfg"
cat >"$layers/project/main.n" <<'PROGRAM'
import both
import envonly
import projonly
import useronly
require both.pick() == 1
require envonly.e() + projonly.p() + useronly.u() == 12
PROGRAM
cd "$layers/project"
if ! env -u XDG_CONFIG_HOME HOME="$layers/home" N00B_PATH="$layers/env" \
        "$compiler" build --path "$layers/flag" --output "$tmp/layers.bin" \
        main.n >"$tmp/layers.log" 2>&1; then
    echo 'paths.path layers did not all contribute roots' >&2
    cat "$tmp/layers.log" >&2
    exit 1
fi
"$tmp/layers.bin"

# Each root is labelled by the layer it came from, in resolver notes and in
# `n00b env`, and a directory named twice is searched and listed once.
printf 'import nowhere\n' >"$layers/project/missing.n"
labels() {
    env -u XDG_CONFIG_HOME HOME="$layers/home" \
        N00B_PATH="$layers/env:$layers/env:$layers/flag:." "$compiler" \
        "$@" --path "$layers/flag" --path "$layers/flag"
}
if labels check missing.n >"$tmp/labels.log" 2>&1; then
    echo 'a missing module was found' >&2
    exit 1
fi
expect_once() {
    count=$(grep -cxF -- "$2" "$1" || true)
    [ "$count" -eq 1 ] || {
        echo "expected the line '$2' once, saw it $count times" >&2
        cat "$1" >&2
        exit 1
    }
}
expect_once "$tmp/labels.log" '  note: searched the toolchain library'
expect_once "$tmp/labels.log" '  note: searched the current directory'
expect_once "$tmp/labels.log" \
    "  note: searched \`--path\` entry \`$layers/flag\`"
expect_once "$tmp/labels.log" \
    "  note: searched \`N00B_PATH\` entry \`$layers/env\`"
expect_once "$tmp/labels.log" \
    "  note: searched \`paths.path\` entry \`plib\` from $layers/project/n00b.cfg:2"
expect_once "$tmp/labels.log" \
    "  note: searched \`paths.path\` entry \`$layers/user\` from $layers/home/.config/n00b/config.cfg:2"
[ "$(grep -c 'note: searched' "$tmp/labels.log")" -eq 6 ] || {
    echo 'duplicate roots were searched more than once' >&2
    cat "$tmp/labels.log" >&2
    exit 1
}
labels env >"$tmp/env.log" 2>&1 || true
sed -n '/^package roots$/,/^$/p' "$tmp/env.log" >"$tmp/roots.log"
expect_once "$tmp/roots.log" "  --path     $layers/flag"
expect_once "$tmp/roots.log" "  N00B_PATH  $layers/env"
expect_once "$tmp/roots.log" \
    "  project    $layers/project/plib  (from $layers/project/n00b.cfg:2)"
expect_once "$tmp/roots.log" \
    "  user       $layers/user  (from $layers/home/.config/n00b/config.cfg:2)"
[ "$(grep -c '^  ' "$tmp/roots.log")" -eq 6 ] || {
    echo 'n00b env listed a root more than once' >&2
    cat "$tmp/roots.log" >&2
    exit 1
}

# Relative entries resolve against the canonical current directory, the same
# one the resolver uses, even when it is reached through a symbolic link.
mkdir -p "$layers/project/sub"
ln -s "$layers/project" "$tmp/linked"
(cd "$tmp/linked" && env -u XDG_CONFIG_HOME HOME="$layers/home" \
    "$compiler" env --path sub --path "$layers/project/sub") \
    >"$tmp/linked.log" 2>&1 || true
sed -n '/^package roots$/,/^$/p' "$tmp/linked.log" >"$tmp/roots.log"
expect_once "$tmp/roots.log" "  default    $layers/project"
expect_once "$tmp/roots.log" "  --path     sub"
if grep -q 'problem' "$tmp/roots.log" \
        || [ "$(grep -c '^  --path' "$tmp/roots.log")" -ne 1 ]; then
    echo 'a relative entry from a linked directory was mishandled' >&2
    cat "$tmp/roots.log" >&2
    exit 1
fi
