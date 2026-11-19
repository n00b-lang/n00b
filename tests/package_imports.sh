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
# The last shadowed_case run must also say how the module was reached.
expect_note() {
    grep -Fxq "  note: $2" "$tmp/shadow.log" || {
        echo "$1: no note '$2'" >&2
        cat "$tmp/shadow.log" >&2
        exit 1
    }
}
# A top-level module of the current directory is in the default package,
# which holds no system package, so one named like a library package is no
# clash: the program builds against the library, and warning N00B-M0026,
# once, at the user's side, names both files.
passed_over_case() {
    label=$1 program=$2 site=$3 name=$4 library=$5 other=$6 modes=${7:-on}
    for cache in $modes; do
        if [ "$cache" = on ]; then
            set -- env -u N00B_NO_PACKAGE_CACHE
        else
            set -- env N00B_NO_PACKAGE_CACHE=1
        fi
        (cd "$shadow/project" && "$@" N00B_PATH="$shadow/path" \
            "$compiler" check "$program") >"$tmp/shadow.log" 2>&1 \
            && grep -Eq "^${site}warning N00B-M0026: \`$name\` loads the standard library's module, not the top-level module of the same name in the current directory\$" \
                "$tmp/shadow.log" \
            && grep -Fxq "  note: the standard library's \`$name\` is \`$stdlib_lib/$library\`; the other file is \`$shadow/$other\`" \
                "$tmp/shadow.log" \
            && [ "$(grep -c 'N00B-M0026' "$tmp/shadow.log")" -eq 1 ] \
            && ! grep -q 'N00B-M0025' "$tmp/shadow.log" || {
            echo "$label (cache $cache): not built against the library with one warning" >&2
            cat "$tmp/shadow.log" >&2
            exit 1
        }
    done
}
module='func one() -> int { return 1 }\n'
printf "$module" >"$shadow/project/unicode.n"
printf "import unicode\nrequire unicode.general_category('A') == unicode.Gc.lu\n" \
    >"$shadow/project/uses_library_unicode.n"
passed_over_case 'default-package unicode.n' uses_library_unicode.n \
    'uses_library_unicode\.n:1:8: ' unicode unicode/__init.n \
    project/unicode.n 'on off'
expect_note 'default-package unicode.n' '`unicode` is imported here directly'
rm "$shadow/project/unicode.n"
# `builtin` reaches unicode as well, but the error goes to the user's own
# import of it, whichever gets there first.
printf "$module" >"$shadow/path/unicode.n"
shadowed_case 'file on N00B_PATH' uses_unicode.n \
    'uses_unicode\.n:1:8: ' unicode unicode/__init.n path/unicode.n
expect_note 'file on N00B_PATH' '`unicode` is imported here directly'
rm "$shadow/path/unicode.n"
mkdir "$shadow/project/sys"
printf "$module" >"$shadow/project/sys/env.n"
shadowed_case 'nested file' uses_env.n 'uses_env\.n:1:8: ' sys.env \
    sys/env/__init.n project/sys/env.n
expect_note 'nested file' '`sys.env` is imported here directly'
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
    'uses_unicode\.n:1:8: ' unicode unicode/__init.n path/mine/__init.n \
    'on off'
expect_note 'manifest elsewhere' '`unicode` is imported here directly'
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
# With several imports of it, the first module by path gets the error.
printf 'import b_langtools_user\nimport langtools\n' \
    >"$shadow/project/z_langtools_root.n"
printf 'require 1 == 1\nimport langtools\n' \
    >"$shadow/project/b_langtools_user.n"
shadowed_case 'several importers' z_langtools_root.n \
    'b_langtools_user\.n:2:8: ' langtools.internal.diag \
    langtools/internal/diag.n project/langtools/internal/diag.n
rm -r "$shadow/project/langtools"

# Library modules reached with no import of them in the program are checked
# too: the implicit `builtin` manifest, a module that manifest imports, and
# `sys.gc`, found only as the parent of the package imported.
mkdir -p "$shadow/project/builtin"
printf "$module" >"$shadow/project/builtin/__init.n"
shadowed_case 'implicit builtin manifest' uses_helper.n 'uses_helper\.n: ' \
    builtin.__init builtin/__init.n project/builtin/__init.n
grep -Fxq '  note: `builtin.__init` is loaded implicitly, through `builtin`' \
    "$tmp/shadow.log" || {
    echo 'implicit builtin manifest: no note on how it was reached' >&2
    cat "$tmp/shadow.log" >&2
    exit 1
}
rm -r "$shadow/project/builtin"
# A file on the package path taking the name of the package loaded
# implicitly is a clash; the same file in the current directory is a
# default-package module, passed over with a warning, and the implicit
# import still loads the library's `builtin`.
printf "$module" >"$shadow/path/builtin.n"
shadowed_case 'file named like the implicit package' uses_helper.n \
    'uses_helper\.n: ' builtin builtin/__init.n path/builtin.n
expect_note 'file named like the implicit package' \
    '`builtin` is loaded implicitly, for every program'
mv "$shadow/path/builtin.n" "$shadow/project/builtin.n"
# (Both modules load `builtin` implicitly; the first by path, helper.n,
# gets the warning.)
passed_over_case 'default-package builtin.n' uses_helper.n \
    'helper\.n: ' builtin builtin/__init.n project/builtin.n
expect_note 'default-package builtin.n' \
    '`builtin` is loaded implicitly, for every program'
rm "$shadow/project/builtin.n"
mkdir "$shadow/project/builtin"
printf "$module" >"$shadow/project/builtin/sort.n"
shadowed_case 'module of the builtin package' uses_helper.n \
    'uses_helper\.n: ' builtin.sort builtin/sort.n project/builtin/sort.n
expect_note 'module of the builtin package' \
    '`builtin.sort` is loaded implicitly, through `builtin`'
rm -r "$shadow/project/builtin"
mkdir "$shadow/project/sys"
printf "$module" >"$shadow/project/sys/gc.n"
printf 'import sys.gc.alloc\n' >"$shadow/project/uses_gc.n"
# The implicit `builtin` reaches `sys.gc` too, but the user's import of a
# package inside it is where the error goes.
shadowed_case 'parent of an imported package' uses_gc.n \
    'uses_gc\.n:1:8: ' sys.gc sys/gc/__init.n project/sys/gc.n 'on off'
expect_note 'parent of an imported package' \
    '`sys.gc` is loaded by importing `sys.gc.alloc`'
rm -r "$shadow/project/sys"

# A namesake nobody imports is no error, and other modules still resolve.
printf "$module" >"$shadow/project/text.n"
(cd "$shadow/project" && N00B_PATH="$shadow/path" "$compiler" check \
    uses_helper.n) >"$tmp/shadow.log" 2>&1 || {
    echo 'an unimported standard library namesake failed the check' >&2
    cat "$tmp/shadow.log" >&2
    exit 1
}

# A module or file named on the command line follows the same rule.
rm "$shadow/project/text.n"
printf "$module" >"$shadow/path/unicode.n"
shadowed_case 'module operand' unicode '' unicode unicode/__init.n \
    path/unicode.n 'on off'
rm "$shadow/path/unicode.n"
# A file operand, like a top-level module of the current directory, is in
# the default package, so `unicode.n` is no clash: it builds, with no
# warning or error. A default-package module has its own identity, so the
# library package of its name is still the library's from inside it:
# unicode.n importing `unicode` gets the library's, and builtin.n gets the
# implicit `builtin`.
# (Each in a directory of its own: beside the other, either file would be a
# passed-over namesake, with its warning.)
mkdir -p "$shadow/operand_unicode" "$shadow/operand_builtin"
printf "import unicode\nrequire unicode.general_category('A') == unicode.Gc.lu\n" \
    >"$shadow/operand_unicode/unicode.n"
printf 'var items: List[int] = [1, 2]\nrequire items.len() == 2\n' \
    >"$shadow/operand_builtin/builtin.n"
for cache in on off; do
    if [ "$cache" = on ]; then
        set -- env -u N00B_NO_PACKAGE_CACHE
    else
        set -- env N00B_NO_PACKAGE_CACHE=1
    fi
    for operand in unicode builtin; do
        (cd "$shadow/operand_$operand" && "$@" N00B_PATH="$shadow/path" \
            "$compiler" build --output "$tmp/${operand}_file" "$operand.n") \
            >"$tmp/shadow.log" 2>&1 \
            && "$tmp/${operand}_file" \
            && ! grep -q 'N00B-M002[56]' "$tmp/shadow.log" || {
            echo "file operand $operand.n (cache $cache): did not build cleanly against the library" >&2
            cat "$tmp/shadow.log" >&2
            exit 1
        }
    done
done
printf "$module" >"$shadow/project/unicode.n"
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

# System-ness is by package origin, not name: a default-package builtin.n
# is the user's, so a trait it declares with a `__` method defines no
# language hook, and a function by that name elsewhere is still rejected,
# while a real hook (`__equal`) is accepted. The library's hooks come from
# cached interfaces as well as from source, so the same holds either way.
hooks="$shadow/hooks"
mkdir -p "$hooks"
printf "trait Fancy {\n    func __fancy(self: 'F) -> int\n}\n" >"$hooks/builtin.n"
cat >"$hooks/uses.n" <<'PROGRAM'
object Point {
    x: int
}

func __fancy(view self: Point) -> int {
    return self.x
}

func __equal(view self: Point, view other: Point) -> bool {
    return self.x == other.x
}
PROGRAM
reserved='is not a language hook, and names starting with `__` are reserved for the language'
for cache in warm warm off; do
    if [ "$cache" = off ]; then
        set -- env N00B_NO_OBJECT_CACHE=1 N00B_NO_PACKAGE_CACHE=1
    else
        set -- env -u N00B_NO_OBJECT_CACHE -u N00B_NO_PACKAGE_CACHE
    fi
    if (cd "$hooks" && "$@" "$compiler" check builtin.n uses.n) \
            >"$tmp/hooks.log" 2>&1; then
        echo "hooks (cache $cache): a user trait made a language hook" >&2
        cat "$tmp/hooks.log" >&2
        exit 1
    fi
    grep -Fxq "builtin.n:2:10: error N00B-N0008: \`__fancy\` $reserved" \
        "$tmp/hooks.log" \
        && grep -Fxq "uses.n:5:6: error N00B-N0008: \`__fancy\` $reserved" \
            "$tmp/hooks.log" \
        && [ "$(grep -c 'N00B-N0008' "$tmp/hooks.log")" -eq 2 ] || {
        echo "hooks (cache $cache): user names treated as system ones" >&2
        cat "$tmp/hooks.log" >&2
        exit 1
    }
done

# The compiler's own declarations (module `builtins`) are told by package,
# not by that name: a default-package builtins.n is the user's, and its
# trait, record and functions are ordinary items. The second build restores
# the module's interface from the cache.
names="$shadow/builtins_module"
mkdir -p "$names"
cat >"$names/builtins.n" <<'PROGRAM'
trait Shape {
    func area(self: 'S) -> int
}

record Square {
    side: int
}

func area(self: Square) -> int {
    return self.side * self.side
}

func double(x: int) -> int {
    return x * 2
}

func total(view s: 'T) -> int where 'T: Shape {
    return s.area()
}
PROGRAM
cat >"$names/main.n" <<'PROGRAM'
import builtins

let s: builtins.Square = (side = 3)
let t: builtins.Square = (side = 3)
require s == t
require builtins.double(builtins.total(s)) == 18
PROGRAM
for build in first cached; do
    (cd "$names" && env -u N00B_NO_OBJECT_CACHE -u N00B_NO_PACKAGE_CACHE \
        "$compiler" build --output "$tmp/builtins_user" main.n) \
        >"$tmp/builtins.log" 2>&1 && "$tmp/builtins_user" || {
        echo "builtins.n ($build build): not treated as a user module" >&2
        cat "$tmp/builtins.log" >&2
        exit 1
    }
done
