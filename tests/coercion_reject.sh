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

cat >"$tmp/private_state.n" <<'EOF'
func private_state(b: Buffer) -> uint {
    let view: string = b
    __atomic_store(ref b.state.lock, 0)
    return byte_len(view)
}
private_state(buffer_from_string("kept"))
EOF
reject private_state 'record field is not visible from this module'

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

cat >"$tmp/fresh_global.n" <<'EOF'
var g: Buffer = empty_buffer()
func make() -> Buffer {
    let x: Buffer = buffer_from_string("kept")
    g = x
    return x
}
func grow() {
    g += "!"
}
func fresh_global() -> uint {
    let b: Buffer = make()
    let view: string = b
    grow()
    return byte_len(view)
}
fresh_global()
EOF
reject fresh_global 'this call may write the borrowed Buffer, at line 13'

cat >"$tmp/fresh_out.n" <<'EOF'
func make(out c: Buffer) -> Buffer {
    let x: Buffer = buffer_from_string("kept")
    c = x
    return x
}
func fresh_out() -> uint {
    var c: Buffer = empty_buffer()
    let b: Buffer = make(c)
    let view: string = b
    c += "!"
    return byte_len(view)
}
fresh_out()
EOF
reject fresh_out 'this call may write the borrowed Buffer, at line 10'

cat >"$tmp/ref_local.n" <<'EOF'
func hold(s: string) -> Ref[string] {
    var h: string = s
    return ref h
}
func ref_local() -> uint {
    var b: Buffer = buffer_from_string("kept")
    let r: Ref[string] = hold(b)
    b += "!"
    let t: string = r
    return byte_len(t)
}
ref_local()
EOF
reject ref_local 'stored into a field, element or container here, at line 7'

cat >"$tmp/ref_params.n" <<'EOF'
record Holder { b: Buffer }
func fill(first: Ref[Holder], second: Ref[Holder]) {
    let x: Buffer = buffer_from_string("kept")
    first.b = x
    second.b = x
}
func ref_params() -> uint {
    var p: Holder = (b = empty_buffer())
    var q: Holder = (b = empty_buffer())
    fill(ref p, ref q)
    let view: string = p.b
    q.b += "!"
    return byte_len(view)
}
ref_params()
EOF
reject ref_params 'this call may write the borrowed Buffer, at line 12'

cat >"$tmp/finalizer_run.n" <<'EOF'
import sys.gc.finalize
import sys.gc.alloc
var g: Buffer = buffer_from_string("kept")
func drain() -> uint {
    let m: Ref[sys.gc.core.Mutator]? = sys.gc.alloc.mutator_for_native()
    if m == nil {
        return 0
    }
    return sys.gc.finalize.run(m.get().heap, 0)
}
func finalizer_run() -> uint {
    let view: string = g
    let n: uint = drain()
    return byte_len(view) + n
}
finalizer_run()
EOF
reject finalizer_run 'this call may write the borrowed Buffer, at line 13'

cat >"$tmp/memcpy_record.n" <<'EOF'
record Pair {
    b: Buffer
    n: int
}
func memcpy_record() -> uint {
    var first: Pair = (b = buffer_from_string("kept"), n = 1)
    var second: Pair = (b = buffer_from_string("other"), n = 2)
    let view: string = first.b
    __gc_memcpy(unsafe_cast[Ref[byte]] ref second,
                unsafe_cast[Ref[byte]] ref first, 16)
    second.b += "!"
    return byte_len(view)
}
memcpy_record()
EOF
reject memcpy_record 'this call may write the borrowed Buffer, at line 11'

cat >"$tmp/string_to_buffer.n" <<'EOF'
func wrong() -> Buffer {
    return "text"
}
wrong()
EOF
reject string_to_buffer \
    'a string is immutable and does not convert to Buffer; use buffer_from_string(s)'

cat >"$tmp/record_field_literal.n" <<'EOF2'
record Holder {
    b: Buffer?
    n: int
}
var h: Holder = (b = "x", n = 1)
EOF2
reject record_field_literal \
    'a string is immutable and does not convert to Buffer; use buffer_from_string(s)'
grep -Fq 'record_field_literal.n:5:22' "$tmp/record_field_literal.out"

cat >"$tmp/index_store.n" <<'EOF2'
var l: List[Buffer] = new[List[Buffer]]()
l.append(buffer_from_string("a"))
l[0] = "x"
EOF2
reject index_store \
    'a string is immutable and does not convert to Buffer; use buffer_from_string(s)'
grep -Fq 'index_store.n:3:8' "$tmp/index_store.out"

cat >"$tmp/list_literal.n" <<'EOF2'
let l: List[Buffer] = ["x"]
EOF2
reject list_literal \
    'a string is immutable and does not convert to Buffer; use buffer_from_string(s)'
grep -Fq 'list_literal.n:1:24' "$tmp/list_literal.out"

cat >"$tmp/array_literal.n" <<'EOF2'
let a: Array[Buffer] = [buffer_from_string("a"), "x"]
EOF2
reject array_literal \
    'N00BB-T0036'
grep -Fq 'array_literal.n:1:50' "$tmp/array_literal.out"

cat >"$tmp/array_index_store.n" <<'EOF2'
var a: Array[Buffer] = [buffer_from_string("a")]
a[0] = "x"
EOF2
reject array_index_store \
    'N00BB-T0036'
grep -Fq 'array_index_store.n:2:8' "$tmp/array_index_store.out"

cat >"$tmp/constructor.n" <<'EOF2'
record Holder {
    n: int
}
func __init(modify self: Holder, b: Buffer) {
    self.n = 1
}
let h: Holder = new[Holder]("x")
EOF2
reject constructor \
    'N00BB-T0036'
grep -Fq 'constructor.n:7:29' "$tmp/constructor.out"

cat >"$tmp/method_argument.n" <<'EOF2'
var l: List[Buffer] = new[List[Buffer]]()
l.append("x")
EOF2
reject method_argument \
    'N00BB-T0036'
grep -Fq 'method_argument.n:2:10' "$tmp/method_argument.out"

cat >"$tmp/context_argument.n" <<'EOF2'
context Label(s: string) {
    enter() {
    }
    ensure() {
    }
}
var b: Buffer = buffer_from_string("abc")
with Label(b) {
    b += "x"
}
EOF2
reject context_argument \
    'N00BB-T0002'
grep -Fq 'context_argument.n:8:12' "$tmp/context_argument.out"
