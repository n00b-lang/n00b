import subprocess
import sys

from ref_encoder import encode, typehash


def rec(members):
    return {"kind": "Record", "members": members}


def ptr(t):
    return {"kind": "Indirection", "attrs": 7, "referent": t}


def main(tool="./th_cycle_test"):
    a = rec([])
    a["members"].append({"name": "next", "type": ptr(a)})

    b = rec([])
    c = rec([{"name": "next", "type": ptr(b)}])
    b["members"].append({"name": "next", "type": ptr(c)})

    want = [(typehash(a), bytes(encode(a)).hex()),
            (typehash(b), bytes(encode(b)).hex())]

    out = subprocess.run([tool], capture_output=True, text=True).stdout
    got = []
    for line in out.splitlines():
        h, s = line.split("\t")
        got.append((int(h.split()[-1]), s.strip()))

    if len(got) != len(want):
        print("cycles: expected %d lines, got %d" % (len(want), len(got)))
        return 1

    bad = 0
    for i, (w, g) in enumerate(zip(want, got)):
        if w != g:
            bad += 1
            print("cycle %d disagrees:\n  C  %s\n  py %s" % (i, g, w))

    print("cycles: %d compared, %d disagreements" % (len(want), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:]))
