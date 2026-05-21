#!/usr/bin/env bash
# Download Sysinternals PendMoves.zip, SHA-256 verify, extract
# pendmoves.exe, second SHA-256 verify. Emits the extracted binary
# at $1 (the meson-declared output).
#
# Pins:
#   zip SHA-256:           527143fc701ff297e57419d5200df82edb95b1dd564adaa50d40c5c93f6b36f6
#   pendmoves.exe SHA-256: ed7738b38228a7bccbaad3b78e8830f8c57c7a7a9c81f285eff79210d00bd9e8
#
# Source URL: https://download.sysinternals.com/files/PendMoves.zip
#
# NOT committed to repo per Sysinternals EULA + user direction.
#
# Usage: fetch_pendmoves.sh <output pendmoves.exe>

set -euo pipefail

out="${1:?missing out arg}"
url="https://download.sysinternals.com/files/PendMoves.zip"
zip_sha="527143fc701ff297e57419d5200df82edb95b1dd564adaa50d40c5c93f6b36f6"
exe_sha="ed7738b38228a7bccbaad3b78e8830f8c57c7a7a9c81f285eff79210d00bd9e8"

emit_empty() {
    : > "$out"
    echo "[fixture pe] pendmoves fetch failed ($1); emitting empty placeholder" >&2
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

tmpdir=$(mktemp -d -t n00b-pendmoves-XXXXXX)
trap 'rm -rf "$tmpdir"' EXIT

zip_path="$tmpdir/PendMoves.zip"
if ! curl -sSfL --connect-timeout 10 --max-time 60 -o "$zip_path" "$url" 2>/dev/null; then
    emit_empty "curl download failed (likely offline)"
fi

if ! sha256_check "$zip_path" "$zip_sha"; then
    emit_empty "zip SHA-256 mismatch (upstream changed?)"
fi

if ! command -v unzip >/dev/null 2>&1; then
    emit_empty "unzip missing"
fi

if ! unzip -p "$zip_path" pendmoves.exe > "$out" 2>/dev/null; then
    # Try uppercase
    if ! unzip -p "$zip_path" Pendmoves.exe > "$out" 2>/dev/null; then
        emit_empty "pendmoves.exe not found in zip"
    fi
fi

if ! sha256_check "$out" "$exe_sha"; then
    emit_empty "extracted exe SHA-256 mismatch"
fi

echo "[fixture pe] cached pendmoves.exe ($(wc -c <"$out") bytes; SHA-256 verified)" >&2
