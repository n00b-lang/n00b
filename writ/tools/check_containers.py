import re
import sys

from source_text import code_without_literals, sources

STRUCT = re.compile(r"struct\s*\w*\s*\{([^{}]*)\}\s*(\w*)", re.S)
POINTER = re.compile(r"\*\s*\w+\s*;")
LENGTH = re.compile(r"\b\w*(len|length|cap|capacity|count|size)\s*;")
root = sys.argv[1] if len(sys.argv) > 1 else "src"

violations = []
for path in sources(root):
    code = code_without_literals(path.read_text(encoding="utf-8"))
    for match in STRUCT.finditer(code):
        body, name = match.group(1), match.group(2)
        if name.endswith("_view_t"):
            continue
        if POINTER.search(body) and LENGTH.search(body):
            line = code.count("\n", 0, match.start()) + 1
            violations.append(f"{path}:{line}: struct pairs a pointer with a length; use a lantern container")

print("\n".join(violations) or "container check ok")
sys.exit(1 if violations else 0)
