#!/bin/sh
set -eu

compiler=$1
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

reject() {
    name=$1
    expected=$2
    if "$compiler" build --output "$tmp/$name" "$tmp/$name.n" \
            >"$tmp/$name.out" 2>&1; then
        echo "$name: compiled but must be rejected" >&2
        exit 1
    fi
    if ! grep -Fq "$expected" "$tmp/$name.out"; then
        echo "$name: missing '$expected'" >&2
        cat "$tmp/$name.out" >&2
        exit 1
    fi
}

cat >"$tmp/returned.n" <<'EOF'
func leak() -> string {
    var b: Buffer = buffer_from_string("kept")
    return b
}
leak()
EOF
reject returned 'the borrowed string is returned here'

cat >"$tmp/field_store.n" <<'EOF'
record Holder { text: string }
func keep() -> uint {
    var b: Buffer = buffer_from_string("kept")
    var holder: Holder = (text = "")
    holder.text = b
    return byte_len(holder.text)
}
keep()
EOF
reject field_store 'this use of a Buffer as a string outlives a safe borrow'

cat >"$tmp/source_write.n" <<'EOF'
func write_inside() -> uint {
    var b: Buffer = buffer_from_string("kept")
    let view: string = b
    b += "!"
    return byte_len(view)
}
write_inside()
EOF
reject source_write 'this call may write the borrowed Buffer'

cat >"$tmp/indirect_call.n" <<'EOF'
callback Step {
    call(value: int) -> int
    handler(value: int) -> int
}
func bump(value: int) -> int {
    return value + 1
}
func make() -> Step {
    return callback[Step](ref bump)
}
func call_inside() -> uint {
    var b: Buffer = buffer_from_string("kept")
    let step: Step = make()
    let view: string = b
    let n: int = step.call(1)
    return byte_len(view) + cast[uint] n
}
call_inside()
EOF
reject indirect_call 'this indirect or unknown call may write the borrowed Buffer'

cat >"$tmp/loop.n" <<'EOF'
func loop_inside() -> uint {
    var b: Buffer = buffer_from_string("kept")
    let view: string = b
    var total: uint = 0
    var i: int = 0
    while i < 3 {
        total = total + byte_len(view)
        i = i + 1
    }
    return total
}
loop_inside()
EOF
reject loop 'the borrow is still live around this loop'

cat >"$tmp/self_concat.n" <<'EOF'
func self_concat() {
    var b: Buffer = buffer_from_string("kept")
    concat(b, b)
}
self_concat()
EOF
reject self_concat 'this call may write the borrowed Buffer'

cat >"$tmp/string_to_buffer.n" <<'EOF'
func wrong() -> Buffer {
    return "text"
}
wrong()
EOF
reject string_to_buffer \
    'a string is immutable and does not convert to Buffer; use buffer_from_string(s)'
