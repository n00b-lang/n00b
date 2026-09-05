import re, sys, unicodedata
from ref_encoder import (
    encode, fin, anon_name, arm_tag, is_type_name, named, tuple_of, sum_of, NAME,
    IN, OUT, INOUT, TUPLE, UNION, INTERSECTION, free_var, normalize, typehash,
)

TUPLE_KW = "t"

MODES = {"in": IN, "out": OUT, "inout": INOUT}

RETIRED = {"const", "volatile"}

IND_ATTR = {"nullable": 0x01, "reseatable": 0x02, "indexable": 0x04,
            "owning": 0x08, "exclusive": 0x10, "temporary": 0x20}

IND_SUGAR = {
    "ptr":    IND_ATTR["nullable"] | IND_ATTR["reseatable"] | IND_ATTR["indexable"],
    "ref":    0,
    "mutref": IND_ATTR["exclusive"],
    "own":    IND_ATTR["owning"],
    "gcref":  IND_ATTR["nullable"] | IND_ATTR["reseatable"],
    "tmpref": IND_ATTR["temporary"],
}

RESERVED = (set(MODES) | set(IND_ATTR) | set(IND_SUGAR)
            | {"forall", "bitint", "ubitint", "array", "variant"})

TOK = re.compile(r"""
    \s+
  | (?P<str>"(?:[^"\\]|\\.)*")
  | (?P<int>0[xX][0-9a-fA-F]+|\d+)
  | (?P<id>[^\W\d]\w*)
  | (?P<punct>->|[`\[\](),;:|&@.'*])
""", re.X | re.UNICODE)


class ParseError(Exception):
    pass


def lex(s):
    s = unicodedata.normalize("NFC", s)
    out, i = [], 0
    while i < len(s):
        m = TOK.match(s, i)
        if not m:
            raise ParseError("bad character at %d: %r" % (i, s[i]))
        i = m.end()
        if m.group("str"):
            out.append(("STR", m.group("str")[1:-1]))
        elif m.group("int"):
            out.append(("INT", int(m.group("int"), 0)))
        elif m.group("id"):
            out.append(("ID", m.group("id")))
        elif m.group("punct"):
            out.append(("P", m.group("punct")))
    out.append(("EOF", None))
    return out


class Parser:
    def __init__(self, text):
        self.t = lex(text)
        self.i = 0
        self.binders = []
        self.free = {}

    def peek(self, k=0):
        return self.t[min(self.i + k, len(self.t) - 1)]

    def at(self, kind, val=None):
        k, v = self.peek()
        return k == kind and (val is None or v == val)

    def take(self, kind=None, val=None):
        k, v = self.peek()
        if kind and (k != kind or (val is not None and v != val)):
            raise ParseError("expected %s %r, got %s %r" % (kind, val, k, v))
        self.i += 1
        return v

    def eat(self, val):
        if self.at("P", val):
            self.i += 1
            return True
        return False

    def parse(self):
        t = self.union_type()
        if not self.at("EOF"):
            raise ParseError("trailing input at token %d: %r" % (self.i, self.peek()))
        return t

    def union_type(self):
        first = self.isect_type()
        if not self.at("P", "|"):
            return first
        arms = [first]
        while self.eat("|"):
            arms.append(self.isect_type())
        return sum_of(arms, UNION)

    def isect_type(self):
        first = self.one_type()
        if not self.at("P", "&"):
            return first
        arms = [first]
        while self.eat("&"):
            arms.append(self.one_type())
        return sum_of(arms, INTERSECTION)

    def one_type(self):
        if self.at("ID") and self.peek()[1] in RETIRED:
            raise ParseError(
                "%r is not part of a type (spec.md 14): immutability is a "
                "parameter mode or a property of storage, and atomic is a "
                "constructor, atomic[t]" % self.peek()[1])
        return self.base_type()

    def base_type(self):
        k, v = self.peek()
        if k == "P" and v == "`":
            return self.tvar()
        if k == "P" and v == "(":
            return self.paren_form()
        if k == "P" and v == "@":
            return self.anon_site()
        if k == "ID":
            if v == "forall":
                return self.binder()
            if v in ("bitint", "ubitint"):
                return self.bitint()
            if v in IND_SUGAR and self.peek(1) == ("P", "["):
                return self.indirection()
            if v == "array" and self.peek(1) == ("P", "["):
                return self.sequence()
            if v == TUPLE_KW and self.peek(1) == ("P", "("):
                return self.tuple_form()
            if v == "variant":
                self.take("ID")
                return self.named_type(is_variant=True)
            if v in RESERVED:
                raise ParseError("%r is reserved and is not a type name" % v)
            return self.named_type()
        raise ParseError("cannot start a type: %s %r" % (k, v))

    def tvar(self):
        self.take("P", "`")
        name = self.take("ID")
        for dist, params in enumerate(reversed(self.binders), start=1):
            if name in params:
                return {"kind": "Variable",
                        "distance": dist, "position": params.index(name)}
        if name not in self.free:
            self.free[name] = free_var(name)
        return self.free[name]

    def binder(self):
        self.take("ID", "forall")
        self.take("P", "[")
        params = []
        while True:
            self.take("P", "`")
            params.append(self.take("ID"))
            if not self.eat(","):
                break
        self.take("P", "]")
        self.binders.append(params)
        body = self.one_type()
        self.binders.pop()
        return {"kind": "Binder", "arity": len(params), "body": body}

    def bitint(self):
        kw = self.take("ID")
        self.take("P", "[")
        w = self.take("INT")
        self.take("P", "]")
        return {"kind": "BitInt", "width": w, "signed": kw == "bitint"}

    def indirection(self):
        name = self.take("ID")
        self.take("P", "[")
        ref = self.union_type()
        attrs = IND_SUGAR[name]
        if self.eat(";"):
            attrs = 0
            while self.at("ID"):
                a = self.take("ID")
                if a not in IND_ATTR:
                    raise ParseError("unknown indirection attribute %r" % a)
                attrs |= IND_ATTR[a]
                if not self.eat(","):
                    break
        self.take("P", "]")
        return {"kind": "Indirection", "attrs": attrs, "referent": ref}

    def sequence(self):
        self.take("ID", "array")
        self.take("P", "[")
        el = self.union_type()
        node = {"kind": "Sequence", "element": el}
        if self.eat(";"):
            node["extent"] = self.take("INT")
        self.take("P", "]")
        return node

    def qualified_name(self):
        parts = [self.take("ID")]
        while self.at("P", ".") and self.peek(1)[0] == "ID":
            self.take("P", ".")
            parts.append(self.take("ID"))
        name = ".".join(parts)
        if not is_type_name(name):
            raise ParseError("not a valid type name: %r" % name)
        return name

    def named_type(self, is_variant=False):
        name = self.qualified_name()
        args = []
        if self.eat("["):
            while True:
                if self.at("INT"):
                    args.append(("uint", self.take("INT")))
                elif self.at("STR"):
                    args.append(("str", self.take("STR")))
                else:
                    args.append(("type", self.union_type()))
                if not self.eat(","):
                    break
            self.take("P", "]")
        return named(name, args, is_variant=is_variant)

    def anon_site(self):
        self.take("P", "@")
        path = self.take("STR")
        self.take("P", ":")
        line = self.take("INT")
        self.take("P", ":")
        col = self.take("INT")
        return named(anon_name(path, line, col))

    def field_list(self):
        fields = []
        while True:
            name = self.take("ID")
            self.take("P", ":")
            fields.append({"name": name, "type": self.union_type()})
            if not self.eat(","):
                break
        return fields

    def field_body(self):
        self.take("P", "(")
        fields = [] if self.at("P", ")") else self.field_list()
        self.take("P", ")")
        return fields

    def looks_like_fields(self):
        return self.peek(1)[0] == "ID" and self.peek(2) == ("P", ":")

    def matching_paren(self):
        depth, j = 0, self.i
        while j < len(self.t):
            if self.t[j] == ("P", "("):
                depth += 1
            elif self.t[j] == ("P", ")"):
                depth -= 1
                if depth == 0:
                    return j
            j += 1
        raise ParseError("unbalanced parentheses")

    def paren_form(self):
        close = self.matching_paren()
        if self.t[close + 1] == ("P", "->"):
            return self.function()
        if self.looks_like_fields():
            return {"kind": "Record", "members": self.field_body()}
        return self.group()

    def tuple_form(self):
        self.take("ID", TUPLE_KW)
        self.take("P", "(")
        elems = []
        if not self.at("P", ")"):
            while True:
                elems.append(self.union_type())
                if not self.eat(","):
                    break
        self.take("P", ")")
        return tuple_of(elems)

    def group(self):
        self.take("P", "(")
        inner = self.union_type()
        if self.at("P", ","):
            raise ParseError(
                "parentheses group a single type; a tuple is t(a, b)")
        self.take("P", ")")
        return inner

    def function(self):
        self.take("P", "(")
        params, rest, kw = [], None, []
        if not self.at("P", ")"):
            while True:
                mode = IN
                if self.at("ID") and self.peek()[1] in MODES:
                    mode = MODES[self.take("ID")]
                if self.at("P", "*"):
                    self.take("P", "*")
                    if rest is not None:
                        raise ParseError("more than one rest parameter")
                    rest = {"mode": mode, "type": self.union_type()}
                elif self.at("ID") and self.peek(1) == ("P", ":"):
                    name = self.take("ID")
                    self.take("P", ":")
                    if self.at("ID") and self.peek()[1] in MODES:
                        if mode != IN:
                            raise ParseError("two modes on %r" % name)
                        mode = MODES[self.take("ID")]
                    if any(f["name"] == name for f in kw):
                        raise ParseError("duplicate keyword parameter %r" % name)
                    kw.append({"name": name, "mode": mode,
                               "type": self.union_type()})
                else:
                    params.append({"mode": mode, "type": self.union_type()})
                if not self.eat(","):
                    break
        self.take("P", ")")
        self.take("P", "->")
        node = {"kind": "Function", "ret": self.union_type(), "params": params}
        if rest is not None:
            node["rest"] = rest
        if kw:
            node["kwargs"] = kw
        return node


MODE_NAME = {IN: "", OUT: "out ", INOUT: "inout "}

SUGAR_BY_ATTRS = {v: k for k, v in
                  sorted(IND_SUGAR.items(), key=lambda kv: kv[0])}

ATTR_NAME = sorted(IND_ATTR.items(), key=lambda kv: kv[1])

VAR_ALPHABET = "abcdefghijklmnopqrstuvwxyz"


def _varname(depth, pos):
    n = depth * 8 + pos
    return VAR_ALPHABET[n % 26] + ("" if n < 26 else str(n // 26))


def unparse(node, binders=None):
    if binders is None:
        normalize(node)
    binders = binders or []
    k = node["kind"]

    if k == "Named":
        args = node.get("args", [])
        if (node["name"] == TUPLE and not node.get("is_variant")
                and all(a[0] == "type" for a in args)):
            return "%s(%s)" % (TUPLE_KW,
                               ", ".join(unparse(a[1], binders) for a in args))
        s = ("variant " if node.get("is_variant") else "") + node["name"]
        if args:
            s += "[" + ", ".join(_unparse_arg(a, binders) for a in args) + "]"
    elif k == "BitInt":
        s = "%s[%d]" % ("bitint" if node["signed"] else "ubitint", node["width"])
    elif k == "Indirection":
        inner = unparse(node["referent"], binders)
        a = node.get("attrs", 0)
        s = ("%s[%s]" % (SUGAR_BY_ATTRS[a], inner) if a in SUGAR_BY_ATTRS
             else "ref[%s; %s]" % (inner, ", ".join(n for n, v in ATTR_NAME if a & v)))
    elif k == "Sequence":
        s = "array[" + unparse(node["element"], binders)
        s += ("; %d]" % node["extent"]) if "extent" in node else "]"
    elif k == "Record":
        s = "(" + ", ".join(_fields(node, binders)) + ")"
    elif k == "Function":
        parts = [_param(p, binders) for p in node["params"]]
        if node.get("rest"):
            parts.append(_param(node["rest"], binders, star=True))
        for p in sorted(node.get("kwargs", []),
                        key=lambda p: bytes(NAME(p["name"]))):
            parts.append("%s: %s%s" % (p["name"], MODE_NAME[p.get("mode", IN)],
                                       unparse(p["type"], binders)))
        s = "(%s) -> %s" % (", ".join(parts), unparse(node["ret"], binders))
    elif k == "Binder":
        depth = len(binders)
        names = [_varname(depth, i) for i in range(node["arity"])]
        s = "forall[%s] %s" % (", ".join("`" + n for n in names),
                               unparse(node["body"], binders + [names]))
    elif k == "Variable":
        s = "`" + binders[len(binders) - node["distance"]][node["position"]]
    elif k == "Var":
        i = node.get("index")
        s = "`" + (node.get("name", "?") if i is None else _varname(0, i))
    elif k == "Sum":
        op = " | " if node.get("comb", 0) == 0 else " & "
        order = sorted(node["arms"], key=lambda a: bytes(encode(a)))
        s = "(" + op.join(unparse(a, binders) for a in order) + ")"
    else:
        raise ValueError(k)
    return s


def _param(p, binders, star=False):
    return "%s%s%s" % (MODE_NAME[p.get("mode", IN)],
                       "*" if star else "",
                       unparse(p["type"], binders))


def _fields(rec, binders):
    order = sorted(rec["members"],
                   key=lambda m: bytes(NAME(m["name"])) + bytes(encode(m["type"])))
    return ["%s: %s" % (m["name"], unparse(m["type"], binders)) for m in order]


def _unparse_arg(a, binders):
    if a[0] == "type":
        return unparse(a[1], binders)
    if a[0] == "str":
        return '"%s"' % a[1]
    return str(a[1])


def parse(text):
    return Parser(text).parse()


def canonical(text):
    return unparse(parse(text))


def hash_of(text):
    node = normalize(parse(text))
    return bytes(encode(node)).hex(), typehash(node)


if __name__ == "__main__":
    src = sys.argv[1:] or [l.strip() for l in sys.stdin if l.strip()]
    for expr in src:
        try:
            node = parse(expr)
            st = bytes(encode(node))
            print("%-38s %-20d %s" % (unparse(node), fin(st), st.hex()))
        except (ParseError, ValueError) as e:
            print("%-38s ERROR: %s" % (expr, e))
