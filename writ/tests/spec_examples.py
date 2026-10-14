import os
import subprocess
import sys
import tempfile

writ = os.path.abspath(sys.argv[1])
documents = sys.argv[2:]


def blocks(path):
    with open(path, encoding="utf-8") as source:
        lines = source.read().split("\n")
    current = None
    for number, line in enumerate(lines, 1):
        if line.startswith("```"):
            if current is None:
                current = (number, line[3:].strip(), [])
            else:
                yield current
                current = None
        elif current is not None:
            current[2].append(line)


failed = False
with tempfile.TemporaryDirectory() as scratch:
    for document in documents:
        name = os.path.basename(document)
        ordinal = 0
        for _, tag, body in blocks(document):
            if tag != "writ":
                continue
            ordinal += 1
            number = ordinal
            path = os.path.join(scratch, f"{name}-{number}.writ")
            with open(path, "w", encoding="utf-8") as out:
                out.write("\n".join(body) + "\n")
            parsed = subprocess.run([writ, "parse", path], capture_output=True, text=True)
            checked = subprocess.run([writ, "check", path], capture_output=True, text=True)
            print(f"== {name} example {number}")
            if parsed.returncode != 0:
                failed = True
                print("parse failed")
            print((checked.stdout + checked.stderr).replace(scratch + os.sep, ""), end="")

sys.exit(1 if failed else 0)
