#!/bin/sh
set -eu

compiler="$1"
library="$2"
headers="$3"
work="$4"
shift 4

leaked="$(nm -g "$library" | awk '$2 != "U" && NF == 3 && $3 !~ /^_?writ_/ && $3 !~ /^_*(asan|ubsan|sanitizer)_/')"
if [ -n "$leaked" ]; then
    echo "non-writ symbols are global in $library:" >&2
    echo "$leaked" >&2
    exit 1
fi

mkdir -p "$work"
cat > "$work/host.c" <<'EOF'
#include <stdio.h>
#include "writ/writ.h"
int parse_pwz_parse(void *parser, void *stream) { (void)parser; (void)stream; return 42; }
int main(void) {
    writ_session_t *session = writ_session_new(NULL);
    int ok = session != NULL && parse_pwz_parse(NULL, NULL) == 42;
    writ_session_free(session);
    printf(ok ? "isolation ok\n" : "isolation broken\n");
    return !ok;
}
EOF
"$compiler" -std=c11 -I"$headers" "$work/host.c" "$library" "$@" -o "$work/host"
"$work/host"
