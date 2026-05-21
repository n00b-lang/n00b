#!/usr/bin/env bash
# Build hello.exe via mingw-w64 if available; output to $1.
#
# Used as a meson custom_target command. Returns exit-status-0 with
# a placeholder file (empty) when x86_64-w64-mingw32-gcc is absent
# so the rule still produces its declared output (meson requires
# the output to exist) — the runtime test treats an empty output
# as "fixture unavailable, SKIP".
#
# Usage: build_hello_exe.sh <hello.c source> <output hello.exe>

set -euo pipefail

src="${1:?missing src arg}"
out="${2:?missing out arg}"

if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    # Toolchain absent — emit empty placeholder so meson is happy.
    : > "$out"
    echo "[fixture pe] x86_64-w64-mingw32-gcc not found; emitting empty placeholder" >&2
    exit 0
fi

x86_64-w64-mingw32-gcc -O2 -s -o "$out" "$src"
echo "[fixture pe] built hello.exe ($(wc -c <"$out") bytes)" >&2
