
from typeobj import Type, unify, BOTTOM
from unify_cases import PAIRS

OK = []
FAIL = []


def check(name, cond):
    (OK if cond else FAIL).append(name)


def u(a, b, expect):
    got = unify(a, b)
    if expect is None:
        check("%s  ~  %s  =  BOTTOM" % (a, b), got is BOTTOM)
        return
    want = Type.parse(expect)
    label = "%s  ~  %s  =  %s" % (a, b, expect)
    if got is BOTTOM:
        check(label + "   [got BOTTOM]", False)
    else:
        check(label + ("" if got.hash == want.hash else "   [got %s]" % got.text),
              got.hash == want.hash)


for _a, _b, _expect in PAIRS:
    u(_a, _b, _expect)

sch = Type.parse("forall[`a] (`a) -> `a")
opened = sch.instantiate()
check("instantiate opens a scheme", not opened.is_scheme)
check("instantiate yields an open type", len(opened.free) == 1)
check("instantiate == the written open type",
      opened.hash == Type.parse("(`a) -> `a").hash)
check("instantiated scheme unifies with a ground type",
      unify(opened, Type.parse("(int) -> int")).hash
      == Type.parse("(int) -> int").hash)
check("generalize round-trips", opened.generalize().hash == sch.hash)
check("generalize of a closed type is identity",
      Type.parse("int").generalize().hash == Type.parse("int").hash)
check("scheme and open type differ",
      Type.parse("forall[`a] Vec[`a]").hash != Type.parse("Vec[`a]").hash)
check("instantiate is fresh each time",
      sch.instantiate().free[0] is not sch.instantiate().free[0])

inferred = Type.parse("(`a) -> `a")
scheme = inferred.generalize()
check("generalization promotes unbound parameters", scheme.is_scheme)

call_int = unify(scheme.instantiate(), Type.parse("(int) -> `r"))
call_str = unify(scheme.instantiate(), Type.parse("(string) -> `r"))
check("caller 1 binds at int", call_int.hash == Type.parse("(int) -> int").hash)
check("caller 2 binds at string",
      call_str.hash == Type.parse("(string) -> string").hash)
check("callers do not interfere", scheme.hash == inferred.generalize().hash)
check("the scheme survives its call sites",
      scheme.hash == Type.parse("forall[`a] (`a) -> `a").hash)

two_arg = Type.parse("(`a, `b) -> t(`a, `b)").generalize()
c1 = unify(two_arg.instantiate(), Type.parse("(int, char) -> `r"))
c2 = unify(two_arg.instantiate(), Type.parse("(char, int) -> `r"))
check("independent instantiations, distinct results",
      c1.hash == Type.parse("(int, char) -> t(int, char)").hash
      and c2.hash == Type.parse("(char, int) -> t(char, int)").hash
      and c1.hash != c2.hash)

t = Type.parse("Map[int, char]")
check("Type.text is canonical", t.text == "Map[int, char]")
check("Type.hash is stable", t.hash == Type.parse("Map[int, char]").hash)
check("Type.stream is bytes", isinstance(t.stream, bytes) and len(t.stream) > 0)
check("Type equality by hash", t == Type.parse("Map[int, char]"))
check("Type is dict-usable", len({t, Type.parse("Map[int, char]")}) == 1)
check("Vec[`t] is a base type with a hash",
      Type.parse("Vec[`t]").hash == Type.parse("Vec[`u]").hash)
check("unify does not mutate its inputs",
      (lambda a: (unify(a, Type.parse("Vec[int]")),
                  a.hash == Type.parse("Vec[`a]").hash)[1])(
          Type.parse("Vec[`a]")))

if __name__ == "__main__":
    for n in FAIL:
        print("FAIL  " + n)
    print("\n%d passed, %d failed" % (len(OK), len(FAIL)))
    raise SystemExit(1 if FAIL else 0)
