#!/usr/bin/env bash
# Sign hello.exe via osslsigncode if available and the input is a
# real PE (non-empty placeholder).
#
# Usage: sign_hello_exe.sh <hello.exe input>
#                         <pkcs7_fixture_cert.pem>
#                         <pkcs7_fixture_key.pem>
#                         <hello.exe.signed output>

set -euo pipefail

input="${1:?missing input arg}"
cert="${2:?missing cert arg}"
key="${3:?missing key arg}"
out="${4:?missing out arg}"

if [ ! -s "$input" ]; then
    # Upstream placeholder (mingw absent). Pass the placeholder
    # through so the test SKIP path triggers.
    : > "$out"
    echo "[fixture pe] input hello.exe is empty (mingw absent); SKIP signing" >&2
    exit 0
fi

if ! command -v osslsigncode >/dev/null 2>&1; then
    : > "$out"
    echo "[fixture pe] osslsigncode not found; emitting empty placeholder" >&2
    exit 0
fi

osslsigncode sign \
    -certs "$cert" \
    -key "$key" \
    -h sha256 \
    -n "n00b-attest test fixture" \
    -in "$input" \
    -out "$out"
echo "[fixture pe] signed hello.exe.signed ($(wc -c <"$out") bytes)" >&2
