import pathlib
import re
import sys

import source_tree

CODE_ROOTS = ("compiler/", "runtime/", "abi/", "lantern/src/", "lantern/include/")
KEY_LITERAL = re.compile(r'"((?:n00bb|ir)\.[A-Za-z0-9_.-]+)"')
KEY_MACRO = re.compile(r"^[ \t]*#[ \t]*define[ \t]+([A-Z0-9_]+_KEY)\b", re.M)
ABI_ROW = re.compile(r"typedef struct\s*\{([^{}]*)\}\s*(n00b_abi_\w+_v1)\s*;")
AXIOM_ENTRY = re.compile(r"\b(?:[A-Z_]*NATIVE|FUNCTION|BINDING)\s*\(|\{\s*\.key\s*=")
AXIOM_FILE = "compiler/compiler_axioms.c"
ABI_FILE = "abi/n00b-abi-v1.h"
HOW = {
    "key": "an IR annotation or parse-symbol binding key",
    "macro": "a *_KEY macro",
    "abi": "a runtime ABI row field",
    "axiom": "an entry in the axiom/NATIVE table",
}


def code(root, path):
    return source_tree.code(root, path, "c") or ""


def abi_fields(text):
    for body, row in ABI_ROW.findall(text):
        for declaration in body.split(";"):
            for declarator in declaration.split(","):
                names = re.findall(r"[A-Za-z_]\w*", declarator.split("[")[0].split(":")[0])
                if names:
                    yield f"{row}.{names[-1]}"


def axioms(text):
    start = text.find("axioms[] = {")
    if start < 0:
        return
    table = text[start:text.find("\n};", start)]
    marks = [m.start() for m in AXIOM_ENTRY.finditer(table)] + [len(table)]
    for a, b in zip(marks, marks[1:]):
        strings = re.findall(r'"([^"]*)"', table[a:b])
        if strings:
            yield ":".join(strings[:2]) if table[a:b].lstrip().startswith("FUNCTION") else strings[0]


def found(root):
    items = set()
    for path in source_tree.files(root):
        if not path.startswith(CODE_ROOTS) or not path.endswith((".c", ".h", ".def")) or "/tests/" in path:
            continue
        text = code(root, path)
        items |= {("key", key) for key in KEY_LITERAL.findall(text)}
        items |= {("macro", name) for name in KEY_MACRO.findall(text)}
    items |= {("abi", field) for field in abi_fields(code(root, ABI_FILE))}
    items |= {("axiom", name) for name in axioms(code(root, AXIOM_FILE))}
    return items


def main(argv):
    args = [arg for arg in argv if not arg.startswith("--")]
    root = pathlib.Path(args[0] if args else ".")
    allowlist_path = pathlib.Path(args[1]) if len(args) > 1 else root / "tests/sideband-allowlist"
    items = found(root)
    if "--update" in argv:
        allowlist_path.write_text("".join(f"{kind}\t{name}\texisting\n" for kind, name in sorted(items)))
        return 0
    listed = {}
    problems = []
    for number, line in enumerate(allowlist_path.read_text().splitlines(), 1):
        fields = line.split("\t")
        if len(fields) != 3 or fields[0] not in HOW or not fields[2].strip():
            problems.append(f"{allowlist_path.name}:{number}: write kind<TAB>name<TAB>reason, "
                            f"kind one of {', '.join(HOW)}")
            continue
        listed[(fields[0], fields[1])] = fields[2]
    new = sorted(items - listed.keys())
    for kind, name in new:
        problems.append(f"new {HOW[kind]}: {name}")
    for kind, name in sorted(listed.keys() - items):
        problems.append(f"{kind} {name} is in tests/sideband-allowlist but no longer exists; delete its line")
    if problems:
        print("\n".join(problems))
        if new:
            print("Side-band channels grew. Before adding a key, field or native, carry the fact in the "
                  "structure that already owns it (a typed field, a function result, the existing table "
                  "row). If the new channel is the right design, add a line kind<TAB>name<TAB>reason to "
                  "tests/sideband-allowlist with a one-phrase reason.")
        return 1
    print(f"side-band channels match tests/sideband-allowlist ({len(items)})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
