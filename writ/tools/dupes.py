import re
import sys

from source_text import sources

COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)
TOKEN = re.compile(r'"(?:\\.|[^"\\])*"|[A-Za-z_]\w*|\d\w*|->|<<|>>|[^\s\w]')
WINDOW = 60
root = sys.argv[1] if len(sys.argv) > 1 else "src"

seen = {}
reported = set()
violations = []
for path in sources(root):
    code = COMMENT.sub(" ", path.read_text(encoding="utf-8"))
    tokens = [(m.group(0), code.count("\n", 0, m.start()) + 1) for m in TOKEN.finditer(code)]
    words = [t for t, _ in tokens]
    for i in range(len(words) - WINDOW + 1):
        key = hash(tuple(words[i : i + WINDOW]))
        first = seen.setdefault(key, (str(path), i, tokens[i][1]))
        if first[0] == str(path) and i - first[1] < WINDOW:
            continue
        pair = (first[0], first[2] // 20, str(path), tokens[i][1] // 20)
        if pair not in reported:
            reported.add(pair)
            violations.append(f"{path}:{tokens[i][1]} repeats {first[0]}:{first[2]}")

print("\n".join(violations) or "duplication check ok")
sys.exit(1 if violations else 0)
