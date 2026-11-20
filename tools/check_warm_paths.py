import collections
import pathlib
import re
import sys

import source_tree

ROOTS = ("compiler/", "runtime/", "abi/", "lantern/src/", "lantern/include/", "writ/src/")
LITERAL = re.compile(r'"(?:\\.|[^"\\\n])*"')


def main(argv):
    root = pathlib.Path(argv[0] if argv else ".")
    baseline_path = pathlib.Path(argv[1]) if len(argv) > 1 else root / "tests/warm-baseline"
    baseline = {}
    for line in baseline_path.read_text().splitlines():
        if line.strip():
            name, count = line.split()
            baseline[name] = int(count)
    pattern = re.compile(r"\b(%s)\b" % "|".join(map(re.escape, baseline)))
    counts = collections.Counter()
    sites = collections.defaultdict(list)
    for path in source_tree.files(root):
        if not path.startswith(ROOTS) or not path.endswith((".c", ".h")) or "/tests/" in path:
            continue
        code = source_tree.code(root, path, "c")
        if code is None:
            continue
        code = LITERAL.sub('""', code)
        for m in pattern.finditer(code):
            counts[m.group(1)] += 1
            sites[m.group(1)].append(f"{path}:{code.count(chr(10), 0, m.start()) + 1}")
    grown = [name for name in baseline if counts[name] > baseline[name]]
    for name in grown:
        print(f"{name}: {counts[name]} uses, baseline {baseline[name]}")
        print("  " + "\n  ".join(sites[name]))
    if grown:
        print("Warm-only paths grew. A cold build must produce the same result as a warm one through "
              "the same code: put the decision in the function that already makes it (the cache loader "
              "or the deciding pass), not in a new branch on cache state at a caller. If a new use is "
              "unavoidable, remove another so the count stays, or raise the number in "
              "tests/warm-baseline with the reviewer's agreement.")
        return 1
    shrunk = [f"{name} {counts[name]}" for name in baseline if counts[name] < baseline[name]]
    tighten = f"; lower tests/warm-baseline to: {', '.join(shrunk)}" if shrunk else ""
    print(f"warm-only uses within baseline ({sum(counts.values())} total{tighten})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
