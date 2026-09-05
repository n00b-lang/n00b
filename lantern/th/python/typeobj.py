
import ref_encoder as E
import typelang as T


class Bottom:
    __slots__ = ()

    def __bool__(self):
        return False

    def __repr__(self):
        return "BOTTOM"


BOTTOM = Bottom()


def clone(node, memo=None):
    if memo is None:
        memo = {}
    if node is None:
        return None
    if id(node) in memo:
        return memo[id(node)]
    out = {}
    memo[id(node)] = out
    for k, v in node.items():
        if k in ("referent", "element", "ret", "body"):
            out[k] = clone(v, memo)
        elif k == "args":
            out[k] = [(t, clone(x, memo)) if t == "type" else (t, x)
                      for t, x in v]
        elif k in ("members", "kwargs"):
            out[k] = [dict(m, type=clone(m["type"], memo)) for m in v]
        elif k == "params":
            out[k] = [dict(p, type=clone(p["type"], memo)) for p in v]
        elif k == "rest":
            out[k] = dict(v, type=clone(v["type"], memo)) if v else v
        elif k == "arms":
            out[k] = [clone(a, memo) for a in v]
        elif k == "ref":
            out[k] = None
        else:
            out[k] = v
    return out


def resolve(node):
    while node is not None and node.get("kind") == "Var" and node.get("ref"):
        node = node["ref"]
    return node


def free_vars(node, acc=None, stack=None):
    acc = [] if acc is None else acc
    stack = [] if stack is None else stack
    node = resolve(node)
    if node is None or any(s is node for s in stack):
        return acc
    stack.append(node)
    k = node["kind"]
    if k == "Var":
        if not any(v is node for v in acc):
            acc.append(node)
    elif k == "Named":
        for t, v in node.get("args", []):
            if t == "type":
                free_vars(v, acc, stack)
    elif k == "Indirection":
        free_vars(node["referent"], acc, stack)
    elif k == "Sequence":
        free_vars(node["element"], acc, stack)
    elif k == "Record":
        for m in sorted(node["members"], key=lambda m: E.NAME(m["name"])):
            free_vars(m["type"], acc, stack)
    elif k == "Function":
        free_vars(node["ret"], acc, stack)
        for p in node["params"]:
            free_vars(p["type"], acc, stack)
        if node.get("rest"):
            free_vars(node["rest"]["type"], acc, stack)
        for p in sorted(node.get("kwargs", []), key=lambda p: E.NAME(p["name"])):
            free_vars(p["type"], acc, stack)
    elif k == "Binder":
        free_vars(node["body"], acc, stack)
    elif k == "Sum":
        for a in sorted(node["arms"],
                        key=lambda a: bytes(E.encode(a, None, None, True))):
            free_vars(a, acc, stack)
    stack.pop()
    return acc


def apply_subst(node, memo=None):
    if memo is None:
        memo = {}
    node = resolve(node)
    if node is None:
        return None
    if id(node) in memo:
        return memo[id(node)]
    if node["kind"] == "Var":
        return node
    out = {}
    memo[id(node)] = out
    for k, v in node.items():
        if k in ("referent", "element", "ret", "body"):
            out[k] = apply_subst(v, memo)
        elif k == "args":
            out[k] = [(t, apply_subst(x, memo)) if t == "type" else (t, x)
                      for t, x in v]
        elif k in ("members", "kwargs"):
            out[k] = [dict(m, type=apply_subst(m["type"], memo)) for m in v]
        elif k == "params":
            out[k] = [dict(p, type=apply_subst(p["type"], memo)) for p in v]
        elif k == "rest":
            out[k] = dict(v, type=apply_subst(v["type"], memo)) if v else v
        elif k == "arms":
            out[k] = [apply_subst(a, memo) for a in v]
        else:
            out[k] = v
    return out


def occurs(var, node, stack=None):
    stack = [] if stack is None else stack
    node = resolve(node)
    if node is None or any(s is node for s in stack):
        return False
    if node is var:
        return True
    stack.append(node)
    try:
        return any(occurs(var, v, stack) for v in free_vars(node))
    finally:
        stack.pop()


def _unify(a, b, seen):
    a, b = resolve(a), resolve(b)
    if a is b:
        return True
    if a is None or b is None:
        return False
    key = (id(a), id(b))
    if key in seen:
        return True
    seen.add(key)

    if a["kind"] == "Var":
        if occurs(a, b):
            return False
        a["ref"] = b
        return True
    if b["kind"] == "Var":
        return _unify(b, a, seen)

    if a["kind"] != b["kind"]:
        return False
    k = a["kind"]

    if k == "Named":
        if (a["name"] != b["name"]
                or bool(a.get("is_variant")) != bool(b.get("is_variant"))
                or len(a.get("args", [])) != len(b.get("args", []))):
            return False
        for (ta, va), (tb, vb) in zip(a.get("args", []), b.get("args", [])):
            if ta != tb:
                return False
            if ta == "type":
                if not _unify(va, vb, seen):
                    return False
            elif va != vb:
                return False
        return True
    if k == "BitInt":
        return a["width"] == b["width"] and a["signed"] == b["signed"]
    if k == "Indirection":
        return (a.get("attrs", 0) == b.get("attrs", 0)
                and _unify(a["referent"], b["referent"], seen))
    if k == "Sequence":
        if ("extent" in a) != ("extent" in b):
            return False
        if "extent" in a and a["extent"] != b["extent"]:
            return False
        return _unify(a["element"], b["element"], seen)
    if k == "Record":
        ma = {m["name"]: m["type"] for m in a["members"]}
        mb = {m["name"]: m["type"] for m in b["members"]}
        if set(ma) != set(mb):
            return False
        return all(_unify(ma[n], mb[n], seen) for n in sorted(ma))
    if k == "Function":
        if bool(a.get("rest")) != bool(b.get("rest")):
            return False
        if len(a["params"]) != len(b["params"]):
            return False
        if not _unify(a["ret"], b["ret"], seen):
            return False
        for pa, pb in zip(a["params"], b["params"]):
            if pa.get("mode", E.IN) != pb.get("mode", E.IN):
                return False
            if not _unify(pa["type"], pb["type"], seen):
                return False
        if a.get("rest"):
            ra, rb = a["rest"], b["rest"]
            if ra.get("mode", E.IN) != rb.get("mode", E.IN):
                return False
            if not _unify(ra["type"], rb["type"], seen):
                return False
        ka = {p["name"]: p for p in a.get("kwargs", [])}
        kb = {p["name"]: p for p in b.get("kwargs", [])}
        if set(ka) != set(kb):
            return False
        for n in sorted(ka):
            if ka[n].get("mode", E.IN) != kb[n].get("mode", E.IN):
                return False
            if not _unify(ka[n]["type"], kb[n]["type"], seen):
                return False
        return True
    if k == "Binder":
        return a["arity"] == b["arity"] and _unify(a["body"], b["body"], seen)
    if k == "Variable":
        return (a["distance"] == b["distance"]
                and a["position"] == b["position"])
    if k == "Sum":
        if a.get("comb", 0) != b.get("comb", 0):
            return False
        if len(a["arms"]) != len(b["arms"]):
            return False
        if not free_vars(a) and not free_vars(b):
            return E.typehash(a) == E.typehash(b)
        key_ = lambda n: bytes(E.encode(n, None, None, True))
        for x, y in zip(sorted(a["arms"], key=key_),
                        sorted(b["arms"], key=key_)):
            if not _unify(x, y, seen):
                return False
        return True
    raise ValueError(k)


class Type:

    __slots__ = ("term", "_stream", "_hash", "_text")

    def __init__(self, term):
        self.term = E.normalize(term)
        self._stream = self._hash = self._text = None

    @classmethod
    def parse(cls, text):
        return cls(T.parse(text))

    @property
    def stream(self):
        if self._stream is None:
            self._stream = bytes(E.encode(E.normalize(self.term)))
        return self._stream

    @property
    def hash(self):
        if self._hash is None:
            self._hash = E.typehash(self.term)
        return self._hash

    @property
    def text(self):
        if self._text is None:
            self._text = T.unparse(self.term)
        return self._text

    @property
    def free(self):
        return free_vars(self.term)

    @property
    def is_closed(self):
        return not self.free

    @property
    def is_scheme(self):
        return self.term.get("kind") == "Binder"

    def instantiate(self):
        if not self.is_scheme:
            return self
        n = self.term["arity"]
        fresh = [E.free_var("t%d" % i) for i in range(n)]

        def go(node, depth, memo):
            node = resolve(node)
            if node is None:
                return None
            if id(node) in memo:
                return memo[id(node)]
            if node["kind"] == "Variable":
                if node["distance"] == depth + 1:
                    return fresh[node["position"]]
                return node
            if node["kind"] == "Var":
                return node
            out = {}
            memo[id(node)] = out
            d = depth + 1 if node["kind"] == "Binder" else depth
            for k, v in node.items():
                if k in ("referent", "element", "ret"):
                    out[k] = go(v, depth, memo)
                elif k == "body":
                    out[k] = go(v, d, memo)
                elif k == "args":
                    out[k] = [(t, go(x, depth, memo)) if t == "type" else (t, x)
                              for t, x in v]
                elif k in ("members", "kwargs"):
                    out[k] = [dict(m, type=go(m["type"], depth, memo)) for m in v]
                elif k == "params":
                    out[k] = [dict(p, type=go(p["type"], depth, memo)) for p in v]
                elif k == "rest":
                    out[k] = dict(v, type=go(v["type"], depth, memo)) if v else v
                elif k == "arms":
                    out[k] = [go(x, depth, memo) for x in v]
                else:
                    out[k] = v
            return out

        return Type(go(self.term["body"], 0, {}))

    def generalize(self):
        vs = self.free
        if not vs:
            return self
        pos = {id(v): i for i, v in enumerate(vs)}

        def go(node, depth, memo):
            node = resolve(node)
            if node is None:
                return None
            if node["kind"] == "Var" and id(node) in pos:
                return {"kind": "Variable", "distance": depth + 1,
                        "position": pos[id(node)]}
            if id(node) in memo:
                return memo[id(node)]
            out = {}
            memo[id(node)] = out
            d = depth + 1 if node["kind"] == "Binder" else depth
            for k, v in node.items():
                if k in ("referent", "element", "ret"):
                    out[k] = go(v, depth, memo)
                elif k == "body":
                    out[k] = go(v, d, memo)
                elif k == "args":
                    out[k] = [(t, go(x, depth, memo)) if t == "type" else (t, x)
                              for t, x in v]
                elif k in ("members", "kwargs"):
                    out[k] = [dict(m, type=go(m["type"], depth, memo)) for m in v]
                elif k == "params":
                    out[k] = [dict(p, type=go(p["type"], depth, memo)) for p in v]
                elif k == "rest":
                    out[k] = dict(v, type=go(v["type"], depth, memo)) if v else v
                elif k == "arms":
                    out[k] = [go(x, depth, memo) for x in v]
                else:
                    out[k] = v
            return out

        return Type({"kind": "Binder", "arity": len(vs),
                     "body": go(self.term, 0, {})})

    def __eq__(self, other):
        return isinstance(other, Type) and self.hash == other.hash

    def __hash__(self):
        return self.hash

    def __repr__(self):
        return "Type(%s = %d)" % (self.text, self.hash)


def unify(a, b):
    if isinstance(a, str):
        a = Type.parse(a)
    if isinstance(b, str):
        b = Type.parse(b)
    ca, cb = clone(a.term), clone(b.term)
    if not _unify(ca, cb, set()):
        return BOTTOM
    return Type(apply_subst(ca))
