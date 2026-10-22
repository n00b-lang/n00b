#!/bin/sh
set -eu

compiler=$1
program_dir=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cd "$program_dir"
if ! "$compiler" build --output "$tmp/package" \
    package_boundary_runtime.n00b >"$tmp/build.log" 2>&1; then
    cat "$tmp/build.log" >&2
    exit 1
fi
"$tmp/package"

if ! "$compiler" build --output "$tmp/aligned" \
    aligned_view_consumer.n00b >"$tmp/aligned.log" 2>&1; then
    cat "$tmp/aligned.log" >&2
    exit 1
fi
"$tmp/aligned"

if ! "$compiler" build --output "$tmp/sys-parent" \
    sys_parent_consumer.n00b >"$tmp/sys-parent.log" 2>&1; then
    cat "$tmp/sys-parent.log" >&2
    exit 1
fi
"$tmp/sys-parent"

if ! "$compiler" build --output "$tmp/sys-child" \
    sys_child_consumer.n00b >"$tmp/sys-child.log" 2>&1; then
    cat "$tmp/sys-child.log" >&2
    exit 1
fi
"$tmp/sys-child"

if ! "$compiler" build --output "$tmp/overload-export" \
    overload_export_consumer.n00b >"$tmp/overload-export.log" 2>&1; then
    cat "$tmp/overload-export.log" >&2
    exit 1
fi
"$tmp/overload-export"

if "$compiler" lower aligned.self_import >"$tmp/self-import.log" 2>&1; then
    echo 'a package unexpectedly imported its own external namespace' >&2
    exit 1
fi
grep -Fq 'a package cannot import its own external namespace' \
    "$tmp/self-import.log" || {
    cat "$tmp/self-import.log" >&2
    exit 1
}

if "$compiler" lower bundle.self_import >"$tmp/renamed-self.log" 2>&1; then
    echo 'a package unexpectedly imported a differently named external view' >&2
    exit 1
fi
grep -Fq 'a package cannot import its own external namespace' \
    "$tmp/renamed-self.log" || {
    cat "$tmp/renamed-self.log" >&2
    exit 1
}

if "$compiler" lower aligned.self_reference \
    >"$tmp/self-reference.log" 2>&1; then
    echo 'a package unexpectedly referenced its external namespace' >&2
    exit 1
fi
grep -Fq 'a package cannot reference its own external namespace' \
    "$tmp/self-reference.log" || {
    cat "$tmp/self-reference.log" >&2
    exit 1
}

if "$compiler" build --output "$tmp/inactive" \
    inactive_export_consumer.n00b >"$tmp/inactive.log" 2>&1; then
    echo 'export from an inactive when branch unexpectedly compiled' >&2
    exit 1
fi
grep -Fq "has no visible member named 'rejected'" \
    "$tmp/inactive.log" || {
    cat "$tmp/inactive.log" >&2
    exit 1
}

if "$compiler" build --output "$tmp/internal" \
    internal_module_consumer.n00b >"$tmp/rejected.log" 2>&1; then
    echo 'internal package module unexpectedly compiled' >&2
    exit 1
fi
grep -Fq 'a library source module cannot be imported from another source package' \
    "$tmp/rejected.log" || {
    cat "$tmp/rejected.log" >&2
    exit 1
}

if "$compiler" lower builtin_private_consumer.n00b \
    >"$tmp/builtin-private.log" 2>&1; then
    echo 'private builtin source module unexpectedly compiled' >&2
    exit 1
fi
grep -Fq 'a library source module cannot be imported from another source package' \
    "$tmp/builtin-private.log" || {
    cat "$tmp/builtin-private.log" >&2
    exit 1
}

if "$compiler" build --output "$tmp/bare" \
    missing_manifest_consumer.n00b >"$tmp/bare.log" 2>&1; then
    echo 'module without a package manifest unexpectedly compiled' >&2
    exit 1
fi
grep -Fq 'a library source module cannot be imported from another source package' \
    "$tmp/bare.log" || {
    cat "$tmp/bare.log" >&2
    exit 1
}

if "$compiler" build --output "$tmp/unexported" \
    unexported_package_member.n00b >"$tmp/unexported.log" 2>&1; then
    echo 'unexported package member unexpectedly compiled' >&2
    exit 1
fi
grep -Fq "has no visible member named 'rejected'" \
    "$tmp/unexported.log" || {
    cat "$tmp/unexported.log" >&2
    exit 1
}

if "$compiler" build --output "$tmp/foreign" \
    foreign_export_consumer.n00b >"$tmp/foreign.log" 2>&1; then
    echo 'foreign package export unexpectedly compiled' >&2
    exit 1
fi
grep -Fq 'a manifest can export only symbols from its own source package' \
    "$tmp/foreign.log" || {
    cat "$tmp/foreign.log" >&2
    exit 1
}
