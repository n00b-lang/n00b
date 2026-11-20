import collections
import pathlib
import re
import sys

import source_tree

ROOTS = ("compiler/", "runtime/", "abi/", "lantern/src/", "lantern/include/")
LITERAL = re.compile(r'"((?:\\.|[^"\\\n])*)"')
NUMERIC = r"%[-+0#]*\d*(?:hh|h|ll|l|z|j|t)?[diuxX]|%[-+0#]*\d*$"
DISPLAY_FILE = re.compile(r"(dump|print|cli|debug)[^/]*\.[ch]$")
SIGIL_NUMBER = re.compile(r"[@#$](?:%s)|(?:%s)[@#$]" % (NUMERIC, NUMERIC))
DISPLAY_TEXT = re.compile(r"\s|\\n|\\t|[()<>\[\]]")


def encodings(root):
    found = collections.Counter()
    where = collections.defaultdict(list)
    for path in source_tree.files(root):
        if not path.startswith(ROOTS) or not path.endswith((".c", ".h")) or "/tests/" in path \
                or DISPLAY_FILE.search(path):
            continue
        code = source_tree.code(root, path, "c")
        if code is None:
            continue
        for m in LITERAL.finditer(code):
            body = m.group(1)
            if not DISPLAY_TEXT.search(body) and SIGIL_NUMBER.search(body):
                found[(path, body)] += 1
                where[(path, body)].append(code.count("\n", 0, m.start()) + 1)
    return found, where


def main(argv):
    args = [arg for arg in argv if not arg.startswith("--")]
    root = pathlib.Path(args[0] if args else ".")
    baseline_path = pathlib.Path(args[1]) if len(args) > 1 else root / "tests/name-encoding-baseline"
    found, where = encodings(root)
    if "--update" in argv:
        baseline_path.write_text("".join(f"{path}\t{body}\t{count}\n"
                                         for (path, body), count in sorted(found.items())))
        return 0
    baseline = {}
    for line in baseline_path.read_text().splitlines():
        path, body, count = line.split("\t")
        baseline[(path, body)] = int(count)
    new = [key for key, count in sorted(found.items()) if count > baseline.get(key, 0)]
    for path, body in new:
        lines = ",".join(map(str, where[(path, body)]))
        print(f'{path}:{lines}: "{body}" builds a name from a sigil and a number')
    if new:
        print("Identities belong in typed fields, not in strings. Instead of formatting a counter or hash "
              "into a name with @, # or $ and parsing or matching it later, keep the number in a struct "
              "field, an annotation of the right kind, or a table indexed by it. If the string is an "
              "external symbol name that must be unique, add its path<TAB>literal<TAB>count line to "
              "tests/name-encoding-baseline with the reviewer's agreement.")
        return 1
    print(f"no new sigil-number names ({sum(found.values())} known)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
