#!/bin/sh
set -eu

fail() {
    echo "$*" >&2
    exit 1
}

if [ "$#" -ne 2 ]; then
    fail "usage: $0 <build-dir> <out-dir>"
fi

BUILD_DIR=$1
OUT_DIR=$2
BUNDLE_MARKER=".n00b-windows-smoke-bundle"
BUNDLE_MARKER_TEXT="n00b windows smoke bundle v1"
PYTHON=${PYTHON:-python3}
MESON=${MESON:-meson}

case "$OUT_DIR" in
    ""|"/"|".")
        fail "refusing unsafe output directory: $OUT_DIR"
        ;;
    ".."|../*|*/..|*/../*)
        fail "refusing parent-directory output path: $OUT_DIR"
        ;;
esac

[ -d "$BUILD_DIR" ] || fail "missing build directory: $BUILD_DIR"
[ -f "src/text/unicode/tools/gen_tables.py" ] || fail "missing src/text/unicode/tools/gen_tables.py"
[ -f "test/windows_smoke.ps1" ] || fail "missing test/windows_smoke.ps1"
[ -f "test/samples/hello.n" ] || fail "missing test/samples/hello.n"
[ -f "grammars/n00b.bnf" ] || fail "missing grammars/n00b.bnf"
[ -d "test/data/resharp/tests" ] || fail "missing test/data/resharp/tests"

SMOKE_TESTS="
test_array.exe
test_list.exe
test_stack.exe
test_result.exe
test_variant.exe
test_tuple.exe
test_tree.exe
test_buffer.exe
test_dict.exe
test_type_registry.exe
test_unicode_bidi.exe
test_unicode_casemap.exe
test_unicode_collation.exe
test_unicode_emoji.exe
test_unicode_encoding.exe
test_unicode_identifiers.exe
test_unicode_idna.exe
test_unicode_iter.exe
test_unicode_linebreak.exe
test_unicode_normalization.exe
test_unicode_properties.exe
test_unicode_security.exe
test_unicode_segmentation.exe
test_regex_charset.exe
test_regex_parse.exe
test_regex_match.exe
test_regex_api.exe
test_regex_resharp.exe
test_io.exe
test_io_windows.exe
test_signal.exe
test_proc_lifecycle.exe
test_file_change.exe
test_subproc.exe
test_objfile_pe.exe
test_hexdump.exe
test_print.exe
test_fd_managed.exe
test_socket.exe
test_vfs_local.exe
test_vfs_journal.exe
"

UNICODE_TABLES="
gen_age.c
gen_bidi_brackets.c
gen_bidi_class.c
gen_blocks.c
gen_casemap.c
gen_categories.c
gen_collation.c
gen_combining.c
gen_composition.c
gen_confusables.c
gen_decomposition.c
gen_eaw.c
gen_emoji.c
gen_grapheme.c
gen_identifiers.c
gen_idna.c
gen_joining.c
gen_linebreak.c
gen_normprops.c
gen_numeric.c
gen_proplist.c
gen_script_extensions.c
gen_scripts.c
gen_sentbreak.c
gen_wordbreak.c
"

UNICODE_TEST_DATA="
BidiCharacterTest.txt
BidiTest.txt
CollationTest.zip
GraphemeBreakTest.txt
LineBreakTest.txt
NormalizationTest.txt
SentenceBreakTest.txt
WordBreakTest.txt
"

COLLATION_TEST_DATA="
CollationTest_NON_IGNORABLE_SHORT.txt
"

extract_collation_test_data() {
    zip_path=$1
    dest_dir=$2

    "$PYTHON" -c '
import pathlib
import sys
import zipfile

zip_path = pathlib.Path(sys.argv[1])
dest_dir = pathlib.Path(sys.argv[2])

with zipfile.ZipFile(zip_path) as zf:
    for name in sys.argv[3:]:
        member = "CollationTest/" + name
        try:
            data = zf.read(member)
        except KeyError:
            raise SystemExit(f"missing {member} in {zip_path}")
        if not data:
            raise SystemExit(f"empty {member} in {zip_path}")
        dest_dir.mkdir(parents=True, exist_ok=True)
        (dest_dir / name).write_bytes(data)
' "$zip_path" "$dest_dir" $COLLATION_TEST_DATA
}

"$PYTHON" src/text/unicode/tools/gen_tables.py \
    --out-dir "$BUILD_DIR" \
    --cache-dir src/text/unicode/.unicode_cache \
    --test-data-dir test/data

for table in $UNICODE_TABLES; do
    [ -s "$BUILD_DIR/$table" ] || fail "missing generated Unicode table: $BUILD_DIR/$table"
done

"$MESON" compile -C "$BUILD_DIR"

[ -f "$BUILD_DIR/n00b.exe" ] || fail "missing $BUILD_DIR/n00b.exe"
[ -f "$BUILD_DIR/libn00b.a" ] || fail "missing $BUILD_DIR/libn00b.a"

for exe in $SMOKE_TESTS; do
    [ -f "$BUILD_DIR/$exe" ] || fail "missing $BUILD_DIR/$exe"
done

for data in $UNICODE_TEST_DATA; do
    [ -s "test/data/$data" ] || fail "missing Unicode test data: test/data/$data"
done

BUILD_REAL=$(cd "$BUILD_DIR" && pwd -P) || fail "cannot resolve build directory: $BUILD_DIR"
REPO_REAL=$(pwd -P)
OUT_BASE=$(basename "$OUT_DIR")

case "$OUT_BASE" in
    ""|"."|".."|"/")
        fail "refusing unsafe output directory: $OUT_DIR"
        ;;
esac

if [ -L "$OUT_DIR" ]; then
    fail "refusing symlink output directory: $OUT_DIR"
fi

if [ -e "$OUT_DIR" ]; then
    [ -d "$OUT_DIR" ] || fail "output path exists and is not a directory: $OUT_DIR"
    OUT_REAL=$(cd "$OUT_DIR" && pwd -P) || fail "cannot resolve output directory: $OUT_DIR"
else
    OUT_PARENT=$(dirname "$OUT_DIR")
    mkdir -p "$OUT_PARENT"
    OUT_PARENT_REAL=$(cd "$OUT_PARENT" && pwd -P) || fail "cannot resolve output parent: $OUT_PARENT"
    OUT_REAL="${OUT_PARENT_REAL}/${OUT_BASE}"
fi

case "$OUT_REAL" in
    "/"|"$REPO_REAL"|"$BUILD_REAL")
        fail "refusing unsafe output directory: $OUT_DIR"
        ;;
esac

if [ -d "$OUT_DIR" ]; then
    if [ -n "$(find "$OUT_DIR" -mindepth 1 -maxdepth 1 -print -quit)" ]; then
        if [ ! -f "$OUT_DIR/$BUNDLE_MARKER" ] ||
           ! grep -qx "$BUNDLE_MARKER_TEXT" "$OUT_DIR/$BUNDLE_MARKER" ||
           [ ! -f "$OUT_DIR/windows_smoke.ps1" ] ||
           [ ! -d "$OUT_DIR/tests" ] ||
           [ ! -d "$OUT_DIR/grammars" ]; then
            fail "refusing to replace non-smoke-bundle directory: $OUT_DIR"
        fi
        rm -rf -- "$OUT_DIR"
    fi
fi

mkdir -p "$OUT_DIR/tests"
mkdir -p "$OUT_DIR/grammars"
mkdir -p "$OUT_DIR/test/data"
mkdir -p "$OUT_DIR/test-data/resharp"

cp "$BUILD_DIR/n00b.exe" "$OUT_DIR/"
cp "$BUILD_DIR/libn00b.a" "$OUT_DIR/"
cp test/windows_smoke.ps1 "$OUT_DIR/"
cp test/samples/hello.n "$OUT_DIR/"
cp grammars/*.bnf "$OUT_DIR/grammars/"
for data in $UNICODE_TEST_DATA; do
    cp "test/data/$data" "$OUT_DIR/test/data/"
done
extract_collation_test_data "test/data/CollationTest.zip" "$OUT_DIR/test/data"
for data in $COLLATION_TEST_DATA; do
    [ -s "$OUT_DIR/test/data/$data" ] || fail "missing bundled Unicode test data: $data"
done
cp -R test/data/resharp/tests "$OUT_DIR/test-data/resharp/"
cp test/data/resharp/LICENSE "$OUT_DIR/test-data/resharp/"
cp test/data/resharp/README.md "$OUT_DIR/test-data/resharp/"

for exe in $SMOKE_TESTS; do
    cp "$BUILD_DIR/$exe" "$OUT_DIR/tests/"
done

printf '%s\n' "$BUNDLE_MARKER_TEXT" > "$OUT_DIR/$BUNDLE_MARKER"

cat <<EOF
Windows smoke bundle created at: $OUT_DIR

Copy the bundle to Windows. From inside that directory, run:
  pwsh -File .\windows_smoke.ps1 -N00b .\n00b.exe -Transcript .\windows-smoke-transcript.txt

The script writes a transcript to .\windows-smoke-transcript.txt.
Compile mode is expected to report unsupported on Windows.
EOF
