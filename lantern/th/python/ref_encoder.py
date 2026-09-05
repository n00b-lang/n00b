import unicodedata
import xxhash

assert xxhash.xxh3_64_intdigest(b"") == 0x2D06800538D394C2, "XXH3 not value-stable"

VERSION = 0


def U8(v):
    return bytes([v & 0xFF])


def U32(v):
    return (v & 0xFFFFFFFF).to_bytes(4, "big")


def U64(v):
    return (v & 0xFFFFFFFFFFFFFFFF).to_bytes(8, "big")


def STR(s):
    b = (s or "").encode("utf-8")
    return U64(len(b)) + b


def is_type_name(s):
    return bool(s) and all(part.isidentifier() for part in s.split("."))


def NAME(s):
    if not is_type_name(s):
        raise ValueError("not a valid type name: %r" % s)
    return STR(unicodedata.normalize("NFC", s))


KIND = {"Named": 0, "BitInt": 1, "Indirection": 2, "Sequence": 3,
        "Record": 4, "Function": 5, "Binder": 6, "Variable": 7, "Sum": 8,
        "Var": 9}

UNION, INTERSECTION = 0, 1

IN, OUT, INOUT = 0, 1, 2

NULLABLE, RESEATABLE, INDEXABLE, OWNING, EXCLUSIVE, TEMPORARY = 1, 2, 4, 8, 16, 32

RESERVED_PREFIX = "__ty."

TUPLE = RESERVED_PREFIX + "tuple"
UNTYPED = RESERVED_PREFIX + "untyped"


def fin(stream):
    return xxhash.xxh3_128_intdigest(bytes(stream)) & 0xFFFFFFFFFFFFFFFF


def coerce(v):
    return 1 if v == 0 else v


def anon_name(path, line, col, prefix="_anon_"):
    site = fin(STR(path) + U64(line) + U64(col))
    return "%s%016x" % (prefix, site)


def arm_tag(name):
    return coerce(xxhash.xxh3_64_intdigest(
        unicodedata.normalize("NFC", name).encode("utf-8")))


def encode(node, out=None, stack=None, erase=False):
    if out is None:
        out = bytearray()
    if stack is None:
        stack = []
    if node is None:
        out += U8(0xFF)
        return out
    for i, s in enumerate(stack):
        if s is node:
            out += U8(0xFE) + U64(len(stack) - i)
            return out
    if len(stack) >= 256:
        out += U8(0xFD)
        return out
    stack.append(node)
    body(node, out, stack, erase)
    stack.pop()
    return out


def argument(a, out, stack, erase):
    if a[0] == "type":
        out += U8(0)
        encode(a[1], out, stack, erase)
    elif a[0] == "uint":
        out += U8(1) + U64(a[1])
    elif a[0] == "sint":
        out += U8(2) + U64(a[1])
    elif a[0] == "str":
        out += U8(3) + STR(a[1])
    else:
        raise ValueError(a[0])


def member_bytes(m, stack, erase):
    b = bytearray(NAME(m["name"]))
    encode(m["type"], b, stack, erase)
    return bytes(b)


def kwparam_bytes(p, stack, erase):
    b = bytearray(NAME(p["name"]) + U8(p.get("mode", IN)))
    encode(p["type"], b, stack, erase)
    return bytes(b)


def body(n, out, stack, erase=False):
    k = n["kind"]
    out += U8(KIND[k] + 1)
    if k == "Named":
        out += U8(1 if n.get("is_variant") else 0)
        out += NAME(n["name"])
        out += U64(len(n.get("args", [])))
        for a in n.get("args", []):
            argument(a, out, stack, erase)
    elif k == "BitInt":
        out += U64(n["width"]) + U8(1 if n["signed"] else 0)
    elif k == "Indirection":
        out += U8(n.get("attrs", 0))
        encode(n["referent"], out, stack, erase)
    elif k == "Sequence":
        if "extent" in n:
            out += U8(1) + U64(n["extent"])
        else:
            out += U8(0)
        encode(n["element"], out, stack, erase)
    elif k == "Record":
        recs = sorted(member_bytes(m, stack, erase) for m in n["members"])
        out += U64(len(recs))
        for r in recs:
            out += r
    elif k == "Function":
        rest, kw = n.get("rest"), n.get("kwargs")
        out += U8((1 if rest else 0) | (2 if kw else 0))
        encode(n["ret"], out, stack, erase)
        out += U64(len(n["params"]))
        for p in n["params"]:
            out += U8(p.get("mode", IN))
            encode(p["type"], out, stack, erase)
        if rest:
            out += U8(rest.get("mode", IN))
            encode(rest["type"], out, stack, erase)
        if kw:
            recs = sorted(kwparam_bytes(p, stack, erase) for p in kw)
            out += U64(len(recs))
            for r in recs:
                out += r
    elif k == "Binder":
        out += U64(n["arity"])
        encode(n["body"], out, stack, erase)
    elif k == "Variable":
        out += U64(n["distance"]) + U64(n["position"])
    elif k == "Var":
        if erase:
            out += U64(0)
        elif n.get("index") is None:
            raise ValueError("free variable has no index; normalize() first")
        else:
            out += U64(n["index"])
    elif k == "Sum":
        arms = sorted(bytes(encode(a, None, list(stack), erase))
                      for a in n["arms"])
        out += U8(n.get("comb", UNION)) + U64(len(arms))
        for a in arms:
            out += a
    else:
        raise ValueError(k)


def normalize(node):
    seen = {}

    def walk(n, stack):
        if n is None or any(s is n for s in stack):
            return
        stack.append(n)
        k = n["kind"]
        if k == "Var":
            if id(n) not in seen:
                n["index"] = len(seen)
                seen[id(n)] = n
        elif k == "Named":
            for a in n.get("args", []):
                if a[0] == "type":
                    walk(a[1], stack)
        elif k == "Indirection":
            walk(n["referent"], stack)
        elif k == "Sequence":
            walk(n["element"], stack)
        elif k == "Record":
            for m in sorted(n["members"], key=lambda m: NAME(m["name"])):
                walk(m["type"], stack)
        elif k == "Function":
            walk(n["ret"], stack)
            for p in n["params"]:
                walk(p["type"], stack)
            if n.get("rest"):
                walk(n["rest"]["type"], stack)
            for p in sorted(n.get("kwargs", []), key=lambda p: NAME(p["name"])):
                walk(p["type"], stack)
        elif k == "Binder":
            walk(n["body"], stack)
        elif k == "Sum":
            keyed = sorted(n["arms"],
                           key=lambda a: bytes(encode(a, None, None, True)))
            for a in keyed:
                walk(a, stack)
        stack.pop()

    walk(node, [])
    return node


def typehash(node):
    normalize(node)
    return fin(U32(VERSION) + bytes(encode(node)))


def named(name, args=(), is_variant=False):
    return {"kind": "Named", "name": name, "args": list(args),
            "is_variant": is_variant}


def tuple_of(elems):
    return named(TUPLE, [("type", e) for e in elems])


def free_var(name="a"):
    return {"kind": "Var", "name": name, "index": None, "ref": None}


def sum_of(arms, comb=UNION):
    flat = []

    def collect(n):
        if n.get("kind") == "Sum" and n.get("comb", UNION) == comb:
            for a in n["arms"]:
                collect(a)
        else:
            flat.append(n)

    for a in arms:
        collect(a)
    seen, uniq = set(), []
    for a in flat:
        s = bytes(encode(a, None, None, True))
        if s not in seen:
            seen.add(s)
            uniq.append(a)
    if len(uniq) == 1:
        return uniq[0]
    return {"kind": "Sum", "comb": comb, "arms": uniq}
