import os
import re
import subprocess
import sys

from typelang import hash_of

SPEC = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir,
                    "spec.md")

ROW = re.compile(r"^\| `(?P<ty>.+?)` \| (?:`(?P<stream>[0-9a-f]+)` \| )?"
                 r"(?P<hash>\d+) \|$")

EXTRA = [
    "Vec[`a]",
    "(`a, `b) -> `a",
    "forall[`a, `b] Map[`a, `b]",
    "(Vec[`a] | Set[`b])",
    "(Set[`b] | Vec[`a])",
    "Map[`k, `k]",
    "t(t(int), t())",
    "(a: int, b: (c: char, d: i64))",
    "array[t(int, char); 9]",
    "((int & char) | (i64 & string))",
    "go.iface[(read: () -> int)]",
    "(inout x: Vec[`a], *`a) -> t(`a, `a)",
    "N[\"a string arg\", 42, int]",
    "forall[`a] (forall[`b] (`b) -> `b) -> `a",
    "Vec[Vec[Vec[Vec[Vec[Vec[int]]]]]]",
    "((((((int))))))",
]


def spec_types(path=SPEC):
    text = open(path).read()
    body = text[text.index("## §13 Conformance vectors"):text.index("## §14 ")]
    section = ""
    out = []
    for line in body.splitlines():
        if line.startswith("### "):
            section = line
        if ("Arm tags" in section or "Cycles" in section
                or not line.startswith("| `")):
            continue
        m = ROW.match(line)
        if m:
            out.append(m.group("ty").replace("\\|", "|"))
    return out


def main(tool="./th_tool"):
    types = spec_types() + EXTRA
    r = subprocess.run([tool], input="\n".join(types) + "\n",
                       capture_output=True, text=True)
    lines = r.stdout.splitlines()
    if len(lines) != len(types):
        print("line count mismatch: %d vs %d" % (len(lines), len(types)))
        print(r.stderr[:400])
        return 1

    bad = 0
    for ty, line in zip(types, lines):
        parts = line.split("\t")
        try:
            pstream, phash = hash_of(ty)
        except Exception as e:
            print("py-error %-40s %s" % (ty, e))
            bad += 1
            continue
        if parts[0] == "ERROR":
            print("C-ERROR  %-40s %s" % (ty, parts[1]))
            bad += 1
            continue
        chash, cstream = int(parts[0]), parts[1]
        if chash != phash:
            print("HASH     %-40s C=%d  py=%d" % (ty, chash, phash))
            bad += 1
        elif cstream != pstream:
            print("STREAM   %-40s" % ty)
            print("           C  %s" % cstream)
            print("           py %s" % pstream)
            bad += 1

    print("%d types, %d disagreements" % (len(types), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:]))
