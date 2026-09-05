
import os
import re
import sys

from ref_encoder import arm_tag
from typelang import hash_of, canonical

SPEC = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir,
                    "spec.md")

ROW = re.compile(r"^\| `(?P<ty>.+?)` \| (?:`(?P<stream>[0-9a-f]+)` \| )?"
                 r"(?P<hash>\d+) \|$")

SKIP_PREFIXES = ("| Type ", "|---", "| Identity ", "| Arm ", "| Written ")


def main(path=SPEC):
    text = open(path).read()
    start = text.index("## §13 Conformance vectors")
    end = text.index("## §14 ")
    body = text[start:end]

    checked = failed = 0
    section = ""
    for line in body.splitlines():
        if line.startswith("### "):
            section = line
        if line.startswith(SKIP_PREFIXES) or not line.startswith("| `"):
            continue
        m = ROW.match(line)
        if not m:
            continue
        ty = m.group("ty").replace("\\|", "|")
        if "Cycles" in section:
            continue
        if "Arm tags" in section:
            checked += 1
            if arm_tag(ty) != int(m.group("hash")):
                print("FAIL  arm %-36s %d != %s" % (ty, arm_tag(ty),
                                                    m.group("hash")))
                failed += 1
            continue
        want_hash = int(m.group("hash"))
        want_stream = m.group("stream")
        try:
            got_stream, got_hash = hash_of(ty)
        except Exception as e:
            print("FAIL  %-40s parse error: %s" % (ty, e))
            failed += 1
            continue
        checked += 1
        if got_hash != want_hash:
            print("FAIL  %-40s hash %d != %d" % (ty, got_hash, want_hash))
            failed += 1
        elif want_stream and got_stream != want_stream:
            print("FAIL  %-40s stream mismatch" % ty)
            failed += 1

    rt = 0
    for line in body.splitlines():
        m = ROW.match(line) if line.startswith("| `") else None
        if not m:
            continue
        ty = m.group("ty").replace("\\|", "|")
        if ty in ("ok", "err") or ty.startswith(("A = ", "B = ")):
            continue
        try:
            c = canonical(ty)
            assert canonical(c) == c, "not idempotent: %s -> %s" % (ty, c)
            assert hash_of(c)[1] == hash_of(ty)[1], "hash moved: %s" % ty
            rt += 1
        except AssertionError as e:
            print("FAIL  canonical: %s" % e)
            failed += 1

    print("\n%d vectors checked, %d round-tripped, %d failed"
          % (checked, rt, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:]))
