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

cat >"$tmp/out_view.n" <<'EOF'
func forward(s: string, out t: string) {
    t = s
}
func out_view() -> uint {
    var b: Buffer = buffer_from_string("kept")
    var t: string = ""
    forward(b, t)
    b += "!"
    return byte_len(t)
}
out_view()
EOF
reject out_view 'this call may write the borrowed Buffer, at line 8'

cat >"$tmp/fail_payload.n" <<'EOF'
func failing() -> uint fail: string {
    var b: Buffer = buffer_from_string("kept")
    let view: string = b
    fail view
}
try {
    let n: uint = failing()!
} else e {
}
EOF
reject fail_payload 'the borrowed string is the failure value here'

cat >"$tmp/ref_view.n" <<'EOF'
func ref_view() -> uint {
    var b: Buffer = buffer_from_string("kept")
    var t: string = b
    let r: Ref[string] = ref t
    let u: string = r
    b += "!"
    return byte_len(u)
}
ref_view()
EOF
reject ref_view 'this call may write the borrowed Buffer, at line 6'

cat >"$tmp/state_store.n" <<'EOF'
func state_store() -> uint {
    var b: Buffer = buffer_from_string("kept")
    let view: string = b
    b.state.store[0] = 0x58
    return byte_len(view)
}
state_store()
EOF
reject state_store 'this store may write the borrowed Buffer, at line 4'

cat >"$tmp/escape_site.n" <<'EOF'
var kept: List[string] = new[List[string]]()
func keep(s: string) {
    kept.append(s)
}
func escape_site() -> uint {
    var b: Buffer = buffer_from_string("kept")
    let view: string = b
    let n: uint = byte_len(view)
    keep(view)
    return n
}
escape_site()
EOF
reject escape_site 'may be kept, at line 9'

cat >"$tmp/string_to_buffer.n" <<'EOF'
func wrong() -> Buffer {
    return "text"
}
wrong()
EOF
reject string_to_buffer \
    'a string is immutable and does not convert to Buffer; use buffer_from_string(s)'
