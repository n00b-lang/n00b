import pathlib
import sys

budget_path, root = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
TOLERANCE = 1.2

def count_lines(paths):
    return sum(sum(1 for line in p.read_text(encoding="utf-8").splitlines() if line.strip()) for p in paths)

over = []
for line in budget_path.read_text(encoding="utf-8").splitlines():
    if not line.strip():
        continue
    component, budget, scope = line.split("\t")
    budget = int(budget)
    directory = root / component
    if not directory.exists():
        continue
    files = directory.glob("*.c") if scope == "top" else directory.rglob("*.c")
    used = count_lines(sorted(files))
    print(f"{component:20} {used:6} / {budget:6}")
    if used > budget * TOLERANCE:
        over.append(f"{component} uses {used} lines, more than 20% over its budget of {budget}")

if over:
    sys.exit("\n".join(over))
