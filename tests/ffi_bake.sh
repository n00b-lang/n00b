#!/bin/sh
set -eu

compiler=$1
bake_dir=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cd "$tmp"
"${CC:-cc}" -c "$bake_dir/bake.c" -o bake.o
ar rcs libbake.a bake.o

builds() {
    name=$1
    if ! "$compiler" build --contract-dir "$bake_dir" --link-object libbake.a \
        --output "$name" "$bake_dir/$name.n00b" >"$name.log" 2>&1; then
        cat "$name.log" >&2
        exit 1
    fi
}

printf 'Zbake spec' >bake_spec.txt
builds bake_runtime
./bake_runtime

builds bake_baked
if nm bake_baked | grep -q '\.bake_build$'; then
    echo "bake_baked ships the build-time bake_build call" >&2
    exit 1
fi
test "$(./bake_baked 2>&1)" = "baked ok"
rm bake_spec.txt
test "$(./bake_baked 2>&1)" = "recreated baked ok"

if "$compiler" build --output undescribed "$bake_dir/undescribed.n00b" \
    >undescribed.log 2>&1; then
    echo "undescribed pointer was baked" >&2
    exit 1
fi
grep -q "const 'held' cannot be baked: field 'held.inner.blob' points to memory that has no description" undescribed.log
