import re
import sys

from source_text import code_without_literals, sources

ALLOCATOR = re.compile(r"\b(malloc|calloc|realloc|free)\s*\(")
root = sys.argv[1] if len(sys.argv) > 1 else "src"
allowed = {"session.c"}

violations = []
for path in sources(root):
    if path.name in allowed:
        continue
    code = code_without_literals(path.read_text(encoding="utf-8"))
    for number, line in enumerate(code.splitlines(), 1):
        if ALLOCATOR.search(line):
            violations.append(f"{path}:{number}: allocator call outside session.c")

print("\n".join(violations) or "allocation check ok")
sys.exit(1 if violations else 0)
