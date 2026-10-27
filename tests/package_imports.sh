#!/bin/sh
set -eu

compiler=$1
program_dir=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cd "$program_dir"
"$compiler" build --output "$tmp/consumers" package_consumers.n00b \
    >"$tmp/consumers.log" 2>&1 || {
    cat "$tmp/consumers.log" >&2
    exit 1
}
grep -Fq "N00B-M0010" "$tmp/consumers.log" || {
    cat "$tmp/consumers.log" >&2
    exit 1
}
"$tmp/consumers"

if "$compiler" build --output "$tmp/inactive" inactive_export_consumer.n00b \
    >"$tmp/inactive.log" 2>&1; then
    echo 'the inactive export consumer built' >&2
    exit 1
fi
grep -Fq 'N00B-N0004: `rejected` is not exported' "$tmp/inactive.log" \
    && grep -Fq '`rejected` is declared here' "$tmp/inactive.log" || {
    cat "$tmp/inactive.log" >&2
    exit 1
}

if "$compiler" check sys_parent_consumer.n00b aligned.self_import \
    bundle.self_import aligned.self_reference inactive_export_consumer.n00b \
    internal_module_consumer.n00b renamed_manifest_consumer.n00b \
    orphan_manifest_consumer.n00b stranded_consumer.n00b \
    duplicate_export_consumer.n00b builtin_private_consumer.n00b \
    missing_manifest_variable_consumer.n00b foreign_export_consumer.n00b \
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
