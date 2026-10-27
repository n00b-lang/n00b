#!/bin/sh
set -eu

source_root=$1
build_dir=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

"$source_root/tools/install.sh" --build "$build_dir" --prefix "$tmp/first" \
    --no-modify-path >"$tmp/install.log" 2>&1 || {
    cat "$tmp/install.log" >&2
    exit 1
}
"$source_root/tools/install.sh" --build "$build_dir" --prefix "$tmp/first" \
    --link --no-modify-path >"$tmp/link.log" 2>&1 || {
    cat "$tmp/link.log" >&2
    exit 1
}
mv "$tmp/first" "$tmp/moved"
n00b="$tmp/moved/bin/n00b"
PATH="$tmp/moved/bin:/usr/bin:/bin${MINGW_PREFIX:+:$MINGW_PREFIX/bin}"
export PATH

mkdir -p "$tmp/project/sub"
cd "$tmp/project"
printf 'raw_debug("installed and moved\\n")\n' > hello.n
out=$("$n00b" run hello.n 2>&1)
[ "$out" = "installed and moved" ] || {
    echo "unexpected run output: $out" >&2
    exit 1
}

"$n00b" -V | grep -q '\-link (active)' || {
    "$n00b" -V >&2
    exit 1
}
"$n00b" env >"$tmp/env.log" || {
    cat "$tmp/env.log" >&2
    exit 1
}
grep -q 'moved/share/n00b/toolchains/.*/stdlib' "$tmp/env.log" || {
    cat "$tmp/env.log" >&2
    exit 1
}

[ "$("$n00b" toolchain list | wc -l)" -eq 2 ] || {
    "$n00b" toolchain list >&2
    exit 1
}
plain=$("$n00b" toolchain list | grep -v -- '-link' | awk '{print $1}')
"$n00b" toolchain use "$plain" >/dev/null
"$n00b" -V | grep -q "$plain (active)" || {
    "$n00b" -V >&2
    exit 1
}

N00B_TOOLCHAIN=latest-link "$n00b" -V >/dev/null 2>&1 || {
    echo 'an unmatched N00B_TOOLCHAIN blocked --version' >&2
    exit 1
}
N00B_TOOLCHAIN=latest-link "$n00b" build "$tmp/absent.n" >"$tmp/missing.log" 2>&1 && {
    echo 'an unmatched N00B_TOOLCHAIN was accepted' >&2
    exit 1
}
grep -q 'no installed toolchain matches' "$tmp/missing.log" || {
    cat "$tmp/missing.log" >&2
    exit 1
}
link=$("$n00b" toolchain list | grep -- '-link' | awk '{print $1}')
N00B_TOOLCHAIN="$link" "$n00b" -V | grep -q "selected by N00B_TOOLCHAIN" || {
    N00B_TOOLCHAIN="$link" "$n00b" -V >&2
    exit 1
}

"$n00b" toolchain pin "$link" >/dev/null
(cd sub && "$n00b" -V | grep -q "$link (pinned by") || {
    (cd sub && "$n00b" -V) >&2
    exit 1
}
"$n00b" toolchain unpin >/dev/null

"$source_root/tools/install.sh" --uninstall --prefix "$tmp/moved" \
    >"$tmp/uninstall.log" 2>&1 || {
    cat "$tmp/uninstall.log" >&2
    exit 1
}
[ ! -e "$tmp/moved/share/n00b" ] && [ ! -e "$tmp/moved/bin/n00b" ]
