#!/bin/sh
set -eu
# The checks below match diagnostic codes, whatever the caller's environment.
export N00B_DIAGNOSTIC_CODES=1

compiler=$1
program_dir=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cd "$program_dir"
"$compiler" build --output "$tmp/consumers" package_consumers.n \
    >"$tmp/consumers.log" 2>&1 || {
    cat "$tmp/consumers.log" >&2
    exit 1
}
grep -Fq "N00B-M0010" "$tmp/consumers.log" || {
    cat "$tmp/consumers.log" >&2
    exit 1
}
"$tmp/consumers"

if "$compiler" build --output "$tmp/inactive" inactive_export_consumer.n \
    >"$tmp/inactive.log" 2>&1; then
    echo 'the inactive export consumer built' >&2
    exit 1
fi
grep -Fq 'N00B-N0004: `rejected` is not exported' "$tmp/inactive.log" \
    && grep -Fq '`rejected` is declared here' "$tmp/inactive.log" || {
    cat "$tmp/inactive.log" >&2
    exit 1
}

if "$compiler" check sys_parent_consumer.n aligned.self_import \
    bundle.self_import aligned.self_reference inactive_export_consumer.n \
    internal_module_consumer.n renamed_manifest_consumer.n \
    orphan_manifest_consumer.n stranded_consumer.n \
    duplicate_export_consumer.n builtin_private_consumer.n \
    missing_manifest_variable_consumer.n foreign_export_consumer.n \
    >"$tmp/rejected.log" 2>&1; then
    echo 'the rejected package consumers checked' >&2
    exit 1
fi

reported() {
    grep -Fq -- "$1" "$tmp/rejected.log" || {
        echo "expected '$1'" >&2
        cat "$tmp/rejected.log" >&2
        exit 1
    }
}

reported 'N00B-N0003: module `sys` has no function `memory`'
reported "N00B-M0004: \`aligned.child\` is this package's own external name"
test "$(grep -c 'N00B-M0004' "$tmp/rejected.log")" = 2
reported 'external namespace, which code inside the package cannot use'
reported 'N00B-N0004: `rejected` is not exported to this package by module `delta`'
reported 'N00B-M0008: `beta.impl` is a source module of package `beta`'
reported 'N00B-M0008: `bundle.impl` is a source module of package `bundle`'
reported 'N00B-M0007'
reported 'N00B-M0009'
reported 'N00B-M0019'
reported 'N00B-M0008: `builtin.containers.list` is a source module of package `builtin`'
reported 'N00B-N0004: `counter` is not exported'
reported 'N00B-M0020: `beta.read` belongs to package `beta`'

# The modules of `cyclic` import each other; explaining a hidden or missing
# member must not loop through that cycle.
if "$compiler" check cyclic_hidden_consumer.n >"$tmp/cyclic.log" 2>&1; then
    echo 'the cyclic package consumer checked' >&2
    exit 1
fi
grep -Fq 'N00B-N0004: `hidden` is not exported to this package by module `cyclic`' \
    "$tmp/cyclic.log" \
    && grep -Fq '`cyclic` has no member `nowhere`' "$tmp/cyclic.log" \
    && ! grep -Fq 'internal compiler error' "$tmp/cyclic.log" || {
    cat "$tmp/cyclic.log" >&2
    exit 1
}

# looped_consumer: `looped` exports a whole module that is in an import
# cycle with the manifest; its constants are exported all the same.
for consumer in nested_loop_consumer nested_round_consumer looped_consumer; do
    "$compiler" build --output "$tmp/$consumer" "$consumer.n" \
        >"$tmp/$consumer.log" 2>&1 && "$tmp/$consumer" || {
        cat "$tmp/$consumer.log" >&2
        exit 1
    }
done

# The standard library cannot be shadowed. A module file or a package in the
# current directory or on the package path that has the module path of a
# standard library module or package is an error naming both, whichever root
# it is in and whether it is a file or a manifest.
shadow=$(cd "$tmp" && pwd -P)/shadow
stdlib_lib=$(cd "$program_dir/../../../lib" && pwd -P)
mkdir -p "$shadow/project" "$shadow/path"
printf 'import unicode\n' >"$shadow/project/uses_unicode.n"
printf 'import sys.env\n' >"$shadow/project/uses_env.n"
printf 'import helper\nrequire helper.one() == 1\n' \
    >"$shadow/project/uses_helper.n"
printf 'func one() -> int { return 1 }\n' >"$shadow/project/helper.n"
# The clash is reported once, at the user's side: at the import that finds
# it, or, when library modules reach it, at the user's import that started
# the chain. SITE is what precedes `error` on the line
# (the import's position, the module of an implicit import, or nothing for a
# module named on the command line), as an extended regular expression. A
# note names both files.
shadowed_case() {
    label=$1 program=$2 site=$3 name=$4 library=$5 other=$6 modes=${7:-on}
    for cache in $modes; do
        if [ "$cache" = on ]; then
            set -- env -u N00B_NO_PACKAGE_CACHE
        else
            set -- env N00B_NO_PACKAGE_CACHE=1
        fi
        if (cd "$shadow/project" && "$@" N00B_PATH="$shadow/path" \
                "$compiler" check "$program") \
                >"$tmp/shadow.log" 2>&1; then
            echo "$label (cache $cache): shadowing the standard library checked" >&2
            exit 1
        fi
        grep -Eq "^${site}error N00B-M0025: standard library module \`$name\` is shadowed by another file with the same module path\$" \
            "$tmp/shadow.log" \
            && grep -Fxq "  note: the standard library's \`$name\` is \`$stdlib_lib/$library\`; the other file is \`$shadow/$other\`" \
                "$tmp/shadow.log" \
            && [ "$(grep -c 'N00B-M0025' "$tmp/shadow.log")" -eq 1 ] \
            && ! grep -q "^$stdlib_lib" "$tmp/shadow.log" || {
            echo "$label (cache $cache): not reported once, at the user's import" >&2
            cat "$tmp/shadow.log" >&2
            exit 1
        }
    done
}
module='func one() -> int { return 1 }\n'
printf "$module" >"$shadow/project/unicode.n"
# (`builtin` also reaches unicode, so the report may come from the implicit
# import, at the top of the module.)
shadowed_case 'file in the current directory' uses_unicode.n \
    'uses_unicode\.n(:1:8)?: ' unicode unicode/__init.n project/unicode.n \
    'on off'
rm "$shadow/project/unicode.n"
printf "$module" >"$shadow/path/unicode.n"
shadowed_case 'file on N00B_PATH' uses_unicode.n \
    'uses_unicode\.n(:1:8)?: ' unicode unicode/__init.n path/unicode.n
rm "$shadow/path/unicode.n"
mkdir "$shadow/project/sys"
printf "$module" >"$shadow/project/sys/env.n"
shadowed_case 'nested file' uses_env.n 'uses_env\.n(:1:8)?: ' sys.env \
    sys/env/__init.n project/sys/env.n
rm -r "$shadow/project/sys"
# (langtools is not loaded by `builtin`, so the user's import is the only way
# these are reached.)
printf 'import langtools\n' >"$shadow/project/uses_langtools.n"
mkdir "$shadow/project/langtools"
printf 'package langtools {\n}\n' >"$shadow/project/langtools/__init.n"
shadowed_case 'manifest at the library path' uses_langtools.n \
    'uses_langtools\.n:1:8: ' langtools.__init langtools/__init.n \
    project/langtools/__init.n
grep -Fxq '  note: `langtools.__init` is loaded by importing `langtools`' \
    "$tmp/shadow.log" || {
    echo 'manifest at the library path: no note on how it was reached' >&2
    cat "$tmp/shadow.log" >&2
    exit 1
}
rm -r "$shadow/project/langtools"
mkdir "$shadow/path/mine"
printf 'package unicode {\n}\n' >"$shadow/path/mine/__init.n"
shadowed_case 'manifest elsewhere' uses_unicode.n \
    'uses_unicode\.n(:1:8)?: ' unicode unicode/__init.n path/mine/__init.n \
    'on off'
rm -r "$shadow/path/mine"
mkdir -p "$shadow/project/langtools/internal"
printf "$module" >"$shadow/project/langtools/internal/diag.n"
shadowed_case 'library source module' uses_langtools.n \
    'uses_langtools\.n:1:8: ' langtools.internal.diag \
    langtools/internal/diag.n project/langtools/internal/diag.n 'on off'
grep -Fxq '  note: `langtools.internal.diag` is loaded by importing `langtools`' \
    "$tmp/shadow.log" || {
    echo 'library source module: no note on how it was reached' >&2
    cat "$tmp/shadow.log" >&2
    exit 1
}
rm -r "$shadow/project/langtools"

# Library modules reached with no import of them in the program are checked
# too: the implicit `builtin` manifest, a module that manifest imports, and
# `sys.gc`, found only as the parent of the package imported.
mkdir -p "$shadow/project/builtin"
printf "$module" >"$shadow/project/builtin/__init.n"
shadowed_case 'implicit builtin manifest' uses_helper.n '(uses_)?helper\.n: ' \
    builtin.__init builtin/__init.n project/builtin/__init.n
grep -Fxq '  note: `builtin.__init` is loaded implicitly, through `builtin`' \
    "$tmp/shadow.log" || {
    echo 'implicit builtin manifest: no note on how it was reached' >&2
    cat "$tmp/shadow.log" >&2
    exit 1
}
rm "$shadow/project/builtin/__init.n"
printf "$module" >"$shadow/project/builtin/sort.n"
shadowed_case 'module of the builtin package' uses_helper.n \
    '(uses_)?helper\.n: ' builtin.sort builtin/sort.n project/builtin/sort.n
rm -r "$shadow/project/builtin"
mkdir "$shadow/project/sys"
printf "$module" >"$shadow/project/sys/gc.n"
printf 'import sys.gc.alloc\n' >"$shadow/project/uses_gc.n"
# Whichever reaches `sys.gc` first, the implicit `builtin` or the import,
# reports it in uses_gc.n.
shadowed_case 'parent of an imported package' uses_gc.n \
    'uses_gc\.n(:1:8)?: ' sys.gc sys/gc/__init.n project/sys/gc.n
rm -r "$shadow/project/sys"

# A namesake nobody imports is no error, and other modules still resolve.
printf "$module" >"$shadow/project/text.n"
(cd "$shadow/project" && N00B_PATH="$shadow/path" "$compiler" check \
    uses_helper.n) >"$tmp/shadow.log" 2>&1 || {
    echo 'an unimported standard library namesake failed the check' >&2
    cat "$tmp/shadow.log" >&2
    exit 1
}

# A module named on the command line follows the same rule; a file path
# names exactly that file and is left alone.
rm "$shadow/project/text.n"
printf "$module" >"$shadow/project/unicode.n"
shadowed_case 'module operand' unicode '' unicode unicode/__init.n \
    project/unicode.n 'on off'
mkdir -p "$shadow/path/sys"
printf "$module" >"$shadow/path/sys/env.n"
shadowed_case 'module operand on N00B_PATH' sys.env '' sys.env \
    sys/env/__init.n path/sys/env.n
rm -r "$shadow/path/sys"
mkdir -p "$shadow/project/unicode/internal"
printf "$module" >"$shadow/project/unicode/internal/types.n"
shadowed_case 'library module operand' unicode.internal.types '' \
    unicode.internal.types unicode/internal/types.n \
    project/unicode/internal/types.n
grep -Fxq '  note: `unicode.internal.types` is named on the command line' \
    "$tmp/shadow.log" || {
    echo 'library module operand: no note on how it was reached' >&2
    cat "$tmp/shadow.log" >&2
    exit 1
}
rm -r "$shadow/project/unicode"
(cd "$shadow/project" && "$compiler" check uses_helper unicode.n) \
    >"$tmp/shadow.log" 2>&1 || {
    echo 'an ordinary module operand or a file operand failed' >&2
    cat "$tmp/shadow.log" >&2
    exit 1
}
