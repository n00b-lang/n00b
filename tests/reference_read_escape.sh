#!/bin/sh
set -eu

compiler=$1
source=$2
output=$("$compiler" lower --dump-symbols "$source" 2>&1)

printf '%s\n' "$output" | grep -Eq '^    target: variable, .*may-escape,'
printf '%s\n' "$output" | grep -Eq '^    returned_target: variable, .*may-escape,'
printf '%s\n' "$output" | grep -Eq '^    retained_target: variable, .*may-escape,'
printf '%s\n' "$output" | grep -Eq '^    local_slot: variable, .*may-escape,'
printf '%s\n' "$output" | grep -Eq '^    slot: variable, '
printf '%s\n' "$output" | grep -Eq '^    slot: parameter, '
if printf '%s\n' "$output" | grep -Eq '^    slot: (variable|parameter), .*may-escape,'; then
    echo 'reference slot escaped with the value read through it' >&2
    exit 1
fi
