#!/bin/sh
set -eu

compiler=$1
root=$2
tmp=$(mktemp -d)
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
        run() { env -u N00BB_NO_PACKAGE_CACHE "$compiler" "$@"; }
    else
        run() { N00BB_NO_PACKAGE_CACHE=1 "$compiler" "$@"; }
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
