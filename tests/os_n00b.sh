#!/bin/sh
set -eu

compiler=$1
directory=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

build() {
    if ! (cd "$directory" \
          && "$compiler" build --output "$tmp/$1" "$1.n") \
        >"$tmp/$1.out" 2>"$tmp/$1.err"; then
        cat "$tmp/$1.out" "$tmp/$1.err" >&2
        exit 1
    fi
}

run() {
    if ! (cd "$tmp" && "./$1") >"$tmp/stdout" 2>"$tmp/stderr" \
        || test -s "$tmp/stdout" || test "$(cat "$tmp/stderr")" != "$2"; then
        cat "$tmp/stdout" "$tmp/stderr" >&2
        exit 1
    fi
}

build os_io
run os_io ok

build os_memory
run os_memory ok

build os_threads
run os_threads ok

build os_exit
status=0
(cd "$tmp" && ./os_exit) >"$tmp/stdout" 2>"$tmp/stderr" || status=$?
if test "$status" -ne 3 || test "$(cat "$tmp/stderr")" != exiting; then
    echo "os_exit exited with status $status" >&2
    cat "$tmp/stdout" "$tmp/stderr" >&2
    exit 1
fi

case "$(uname -s)" in
Linux) ;;
*) exit 0 ;;
esac

build syscall
run syscall "$(printf 'wrote\nok')"
if nm "$tmp/syscall" 2>/dev/null | grep -q 'n00b\.syscall'; then
    echo "the program references a syscall extern symbol" >&2
    exit 1
fi

for program in os_io syscall; do
    if readelf -l "$tmp/$program" | grep -q INTERP \
        || readelf -d "$tmp/$program" | grep -q NEEDED \
        || test -n "$(nm -u "$tmp/$program")"; then
        echo "$program is not a static, libc-free program:" >&2
        readelf -d "$tmp/$program" >&2 || true
        nm -u "$tmp/$program" >&2 || true
        exit 1
    fi
done

if (cd "$directory" && N00B_DIAGNOSTICS=terse "$compiler" check bad_errno.n) \
    >"$tmp/bad.out" 2>"$tmp/bad.err"; then
    echo "a closed enum was accepted as the syscall errno type" >&2
    exit 1
fi
if ! grep -q '^bad_errno.n:13:5:.*syscall failures cannot arrive as `Closed_Errno`' \
    "$tmp/bad.err"; then
    cat "$tmp/bad.out" "$tmp/bad.err" >&2
    exit 1
fi
