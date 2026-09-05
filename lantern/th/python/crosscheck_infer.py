import os
import subprocess
import sys

from typeobj import Type, unify, BOTTOM
from unify_cases import PAIRS
from crosscheck import spec_types, EXTRA

SCHEMES = [
    "forall[`a] (`a) -> `a",
    "forall[`a, `b] Map[`a, `b]",
    "forall[`a] Vec[`a]",
    "forall[`a] (forall[`b] (`b) -> `b) -> `a",
    "forall[`a, `b] (`a, *`b) -> t(`a, `b)",
    "forall[`a] (x: `a, y: `a)",
    "forall[`a, `b] (Vec[`a] | Set[`b])",
    "forall[`a] array[`a; 4]",
    "forall[`a] ptr[`a]",
    "forall[`a] variant Result[`a, int]",
]

OPEN = [
    "`a",
    "Vec[`a]",
    "Map[`k, `v]",
    "Map[`k, `k]",
    "(`a, `b) -> `a",
    "(`a, *`a) -> int",
    "(x: `a, y: `b)",
    "(Vec[`a] | Set[`b])",
    "array[`a; 9]",
    "t(`a, `a)",
    "int",
    "Map[int, char]",
]


def run(tool, lines):
    r = subprocess.run([tool], input="\n".join(lines) + "\n",
                       capture_output=True, text=True)
    out = r.stdout.splitlines()
    if len(out) != len(lines):
        raise SystemExit("line count mismatch: %d in, %d out\n%s"
                         % (len(lines), len(out), r.stderr[:400]))
    return out


def py_pair(t):
    return (t.hash, t.stream.hex())


def c_pair(line):
    parts = line.split("\t")
    if parts[0] in ("BOTTOM", "ERROR"):
        return parts[0]
    return (int(parts[0]), parts[1])


def check_unify(tool, bad):
    cmds = [":unify %s\t%s" % (a, b) for a, b, _ in PAIRS]
    for (a, b, _), line in zip(PAIRS, run(tool, cmds)):
        got = c_pair(line)
        want = unify(a, b)
        label = "%s ~ %s" % (a, b)
        if want is BOTTOM:
            if got != "BOTTOM":
                print("UNIFY    %-46s C=%s  py=BOTTOM" % (label, got))
                bad.append(label)
            continue
        if got == "BOTTOM":
            print("UNIFY    %-46s C=BOTTOM  py=%s" % (label, want.text))
            bad.append(label)
        elif got != py_pair(want):
            print("UNIFY    %-46s C=%s  py=%s" % (label, got, py_pair(want)))
            bad.append(label)


def check_unary(tool, op, types, pyfn, bad):
    cmds = ["%s %s" % (op, t) for t in types]
    for ty, line in zip(types, run(tool, cmds)):
        got = c_pair(line)
        want = py_pair(pyfn(Type.parse(ty)))
        if got != want:
            print("%-9s%-46s C=%s  py=%s"
                  % (op[1:].upper(), ty, got, want))
            bad.append("%s %s" % (op, ty))


def check_scalar(tool, op, types, pyfn, bad):
    cmds = ["%s %s" % (op, t) for t in types]
    for ty, line in zip(types, run(tool, cmds)):
        want = str(pyfn(Type.parse(ty)))
        if line.strip() != want:
            print("%-9s%-46s C=%s  py=%s" % (op[1:].upper(), ty, line, want))
            bad.append("%s %s" % (op, ty))


def main(tool="./th_tool"):
    bad = []
    types = spec_types() + EXTRA + OPEN + SCHEMES

    check_unify(tool, bad)
    check_unary(tool, ":instantiate", SCHEMES + OPEN,
                lambda t: t.instantiate(), bad)
    check_unary(tool, ":generalize", OPEN + SCHEMES,
                lambda t: t.generalize(), bad)
    check_unary(tool, ":roundtrip", types, lambda t: t, bad)
    check_scalar(tool, ":freevars", types, lambda t: len(t.free), bad)
    check_scalar(tool, ":isscheme", types, lambda t: int(t.is_scheme), bad)
    check_scalar(tool, ":isclosed", types, lambda t: int(t.is_closed), bad)

    checks = (len(PAIRS) + len(SCHEMES) + len(OPEN) * 2 + len(SCHEMES)
              + len(types) * 4)
    print("%d inference checks, %d disagreements" % (checks, len(bad)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:]))
