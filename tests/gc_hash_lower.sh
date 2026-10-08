#!/bin/sh
set -eu

compiler=$1
program=$2
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT HUP INT TERM

"$compiler" lower "$program" >"$tmp" 2>&1
