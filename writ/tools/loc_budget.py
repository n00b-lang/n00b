import pathlib
import re
import sys

plan_path, root = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
ROW = re.compile(r"^\|\s*`([^`]+)`([^|]*)\|\s*([\d,]+)\s*\|")
TOLERANCE = 1.2

def count_lines(paths):
    return sum(sum(1 for line in p.read_text(encoding="utf-8").splitlines() if line.strip()) for p in paths)

text = plan_path.read_text(encoding="utf-8")
section = text[text.index("### 4.13") : text.index("\n## 5.")]
over = []
for line in section.splitlines():
    match = ROW.match(line)
    if not match:
        continue
    component, note, budget = match.group(1).rstrip("/"), match.group(2), int(match.group(3).replace(",", ""))
    directory = root / component
    if not directory.exists():
        continue
    files = directory.glob("*.c") if "excluding" in note else directory.rglob("*.c")
    used = count_lines(sorted(files))
    print(f"{component:20} {used:6} / {budget:6}")
    if used > budget * TOLERANCE:
        over.append(f"{component} uses {used} lines, more than 20% over its budget of {budget}")

if over:
    sys.exit("\n".join(over))
