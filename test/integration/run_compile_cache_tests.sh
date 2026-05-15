#!/bin/bash
set -euo pipefail

N00B="${1:-./build_debug/n00b}"

if [ ! -x "$N00B" ]; then
    echo "error: n00b binary not found at $N00B"
    exit 1
fi

TMP_BASE="${TMPDIR:-/tmp}"
RUN_TMP="$(mktemp -d "$TMP_BASE/n00b-compile-cache.XXXXXX")"
trap 'rm -rf "$RUN_TMP"' EXIT

SRC="$RUN_TMP/cache_test.n"
CACHE="$RUN_TMP/cache"

cat >"$SRC" <<'SRC_EOF'
print(42)
0
SRC_EOF

FIRST="$("$N00B" compile --verbose --cache-only --cache-dir "$CACHE" "$SRC")"
SECOND="$("$N00B" compile --verbose --cache-only --cache-dir "$CACHE" "$SRC")"

cat >"$SRC" <<'SRC_EOF'
print(43)
0
SRC_EOF

THIRD="$("$N00B" compile --verbose --cache-only --cache-dir "$CACHE" "$SRC")"

case "$FIRST" in
    *"cache stored "*) ;;
    *)
        echo "expected first compile to store cache"
        echo "$FIRST"
        exit 1
        ;;
esac

case "$SECOND" in
    *"cache restored "*) ;;
    *)
        echo "expected second compile to restore cache"
        echo "$SECOND"
        exit 1
        ;;
esac

case "$THIRD" in
    *"cache stored "*) ;;
    *)
        echo "expected changed source to store a new cache artifact"
        echo "$THIRD"
        exit 1
        ;;
esac

if [ "$(find "$CACHE" -name '*.n00bcache' | wc -l | tr -d '[:space:]')" -lt 2 ]; then
    echo "expected at least two cache artifacts after source change"
    exit 1
fi

TRUNC_SRC="$RUN_TMP/cache_truncated_test.n"
TRUNC_CACHE="$RUN_TMP/cache-truncated"
mkdir -p "$TRUNC_CACHE"

cat >"$TRUNC_SRC" <<'SRC_EOF'
print(44)
0
SRC_EOF

TRUNC_HASH="$(
    python3 - "$TRUNC_SRC" <<'PY'
from pathlib import Path
import sys

data = Path(sys.argv[1]).read_bytes()
h = 1469598103934665603
for byte in data:
    h ^= byte
    h = (h * 1099511628211) & 0xffffffffffffffff
print(f"{h:016x}")
PY
)"
TRUNC_SIZE="$(wc -c <"$TRUNC_SRC" | tr -d '[:space:]')"
TRUNC_ARTIFACT="$TRUNC_CACHE/$TRUNC_HASH.n00bcache"

cat >"$TRUNC_ARTIFACT" <<TRUNC_EOF
N00B_MIR_CACHE_V1
source=$TRUNC_SRC
module=_main
hash=$TRUNC_HASH
size=$TRUNC_SIZE
TRUNC_EOF

TRUNC_OUTPUT=""
TRUNC_EXIT=0
TRUNC_OUTPUT="$("$N00B" compile --verbose --cache-only --cache-dir "$TRUNC_CACHE" "$TRUNC_SRC" 2>&1)" \
    || TRUNC_EXIT=$?

if [ "$TRUNC_EXIT" -ne 0 ]; then
    echo "expected truncated cache metadata to be treated as a cache miss"
    echo "exit: $TRUNC_EXIT"
    echo "$TRUNC_OUTPUT"
    exit 1
fi

case "$TRUNC_OUTPUT" in
    *"cache restored "*)
        echo "truncated cache metadata was restored"
        echo "$TRUNC_OUTPUT"
        exit 1
        ;;
esac

for required_field in public_symbols dependencies ffi_declarations; do
    if ! grep -q "^$required_field=" "$TRUNC_ARTIFACT"; then
        echo "expected rewritten cache metadata to include $required_field"
        echo "$TRUNC_OUTPUT"
        exit 1
    fi
done

MULTI_SRC_A="$RUN_TMP/multi_compile_a.n"
MULTI_SRC_B="$RUN_TMP/multi_compile_b.n"

cat >"$MULTI_SRC_A" <<'SRC_EOF'
print(1)
0
SRC_EOF

cat >"$MULTI_SRC_B" <<'SRC_EOF'
print(2)
0
SRC_EOF

MULTI_OUTPUT=""
MULTI_EXIT=0
MULTI_OUTPUT="$("$N00B" compile "$MULTI_SRC_A" "$MULTI_SRC_B" 2>&1)" || MULTI_EXIT=$?

if [ "$MULTI_EXIT" -eq 0 ]; then
    echo "expected compile with two input files to fail"
    echo "$MULTI_OUTPUT"
    exit 1
fi

case "$MULTI_OUTPUT" in
    *"exactly one input file"*) ;;
    *)
        echo "expected two-input compile diagnostic to mention exactly one input file"
        echo "$MULTI_OUTPUT"
        exit 1
        ;;
esac

SYMLINK_SRC="$RUN_TMP/cache_symlink_test.n"
SYMLINK_CACHE="$RUN_TMP/cache-symlink"
mkdir -p "$SYMLINK_CACHE"

cat >"$SYMLINK_SRC" <<'SRC_EOF'
print(42)
0
SRC_EOF

SYMLINK_HASH="$(
    python3 - "$SYMLINK_SRC" <<'PY'
from pathlib import Path
import sys

data = Path(sys.argv[1]).read_bytes()
h = 1469598103934665603
for byte in data:
    h ^= byte
    h = (h * 1099511628211) & 0xffffffffffffffff
print(f"{h:016x}")
PY
)"
SYMLINK_ARTIFACT="$SYMLINK_CACHE/$SYMLINK_HASH.n00bcache"
SENTINEL="$RUN_TMP/cache-symlink-sentinel.txt"
printf 'SENTINEL_BEFORE\n' >"$SENTINEL"

if ln -s "$SENTINEL" "$SYMLINK_ARTIFACT" 2>/dev/null; then
    SYMLINK_OUTPUT=""
    SYMLINK_EXIT=0
    SYMLINK_OUTPUT="$("$N00B" compile --verbose --cache-only --cache-dir "$SYMLINK_CACHE" "$SYMLINK_SRC" 2>&1)" \
        || SYMLINK_EXIT=$?

    if [ "$(cat "$SENTINEL")" != "SENTINEL_BEFORE" ]; then
        echo "cache write followed preexisting artifact symlink"
        echo "exit: $SYMLINK_EXIT"
        echo "$SYMLINK_OUTPUT"
        exit 1
    fi
else
    echo "warning: symlink creation unavailable; skipping cache symlink sentinel check"
fi

IMPORT_SRC="$RUN_TMP/import_compile.n"
cat >"$IMPORT_SRC" <<'SRC_EOF'
use basic_dep from "test/interp/modules"

print(dep_value())
0
SRC_EOF

if "$N00B" compile --cache-only --cache-dir "$CACHE" "$IMPORT_SRC" \
        >"$RUN_TMP/import-cache.out" 2>"$RUN_TMP/import-cache.err"; then
    echo "expected cache-only compile with use import to fail"
    cat "$RUN_TMP/import-cache.out"
    cat "$RUN_TMP/import-cache.err"
    exit 1
fi

if "$N00B" compile --output "$RUN_TMP/import-bin" "$IMPORT_SRC" \
        >"$RUN_TMP/import-compile.out" 2>"$RUN_TMP/import-compile.err"; then
    echo "expected native compile with use import to fail"
    cat "$RUN_TMP/import-compile.out"
    cat "$RUN_TMP/import-compile.err"
    exit 1
fi

echo "OK: compile cache store/restore/invalidate/import rejection"
