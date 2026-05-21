#!/usr/bin/env bash
# Download Sysinternals Sigcheck.zip, SHA-256 verify, extract
# sigcheck64.exe (PE32+ / x86-64), second SHA-256 verify. Emits the
# extracted binary at $1 (the meson-declared output).
#
# Why sigcheck64.exe? `n00b_pe_parse` only accepts PE32+ (64-bit)
# binaries. The previous Sysinternals fixture (pendmoves.exe) shipped
# only as PE32 (32-bit Intel 80386) and was rejected with
# N00B_ERR_NOT_SUPPORTED. sigcheck64.exe is a small (~515 KB),
# Authenticode-signed, PE32+ x86-64 binary from the same CDN.
#
# Pins:
#   zip SHA-256:             e28a0ee282023abefdaa422b9529bc771c2e16d96360d109807ed4c048ddf1c1
#   sigcheck64.exe SHA-256:  a2efff8d5bce9db4b899d38afaa706bdfd822711f929616a51b0dbc9f76c6281
#
# Source URL: https://download.sysinternals.com/files/Sigcheck.zip
#
# NOT committed to repo per Sysinternals EULA + user direction.
#
# Usage: fetch_sigcheck.sh <output sigcheck64.exe>

set -euo pipefail

out="${1:?missing out arg}"
url="https://download.sysinternals.com/files/Sigcheck.zip"
zip_sha="e28a0ee282023abefdaa422b9529bc771c2e16d96360d109807ed4c048ddf1c1"
exe_sha="a2efff8d5bce9db4b899d38afaa706bdfd822711f929616a51b0dbc9f76c6281"

emit_empty() {
    : > "$out"
    echo "[fixture pe] sigcheck fetch failed ($1); emitting empty placeholder" >&2
    exit 0
}

if ! command -v curl >/dev/null 2>&1; then
    emit_empty "curl missing"
fi

if ! command -v shasum >/dev/null 2>&1 && ! command -v sha256sum >/dev/null 2>&1; then
    emit_empty "sha256 hasher missing"
fi

sha256_check() {
    local file="$1"
    local expected="$2"
    local actual
    if command -v shasum >/dev/null 2>&1; then
        actual=$(shasum -a 256 "$file" | awk '{print $1}')
    else
        actual=$(sha256sum "$file" | awk '{print $1}')
    fi
    if [ "$actual" != "$expected" ]; then
        echo "[fixture pe] SHA-256 mismatch on $file" >&2
        echo "  expected: $expected" >&2
        echo "  actual:   $actual" >&2
        return 1
    fi
    return 0
}

tmpdir=$(mktemp -d -t n00b-sigcheck-XXXXXX)
trap 'rm -rf "$tmpdir"' EXIT

zip_path="$tmpdir/Sigcheck.zip"
if ! curl -sSfL --connect-timeout 10 --max-time 60 -o "$zip_path" "$url" 2>/dev/null; then
    emit_empty "curl download failed (likely offline)"
fi

if ! sha256_check "$zip_path" "$zip_sha"; then
    emit_empty "zip SHA-256 mismatch (upstream changed?)"
fi

if ! command -v unzip >/dev/null 2>&1; then
    emit_empty "unzip missing"
fi

if ! unzip -p "$zip_path" sigcheck64.exe > "$out" 2>/dev/null; then
    # Try uppercase
    if ! unzip -p "$zip_path" Sigcheck64.exe > "$out" 2>/dev/null; then
        emit_empty "sigcheck64.exe not found in zip"
    fi
fi

if ! sha256_check "$out" "$exe_sha"; then
    emit_empty "extracted exe SHA-256 mismatch"
fi

echo "[fixture pe] cached sigcheck64.exe ($(wc -c <"$out") bytes; SHA-256 verified)" >&2
