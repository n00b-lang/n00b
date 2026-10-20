import os
import subprocess
import sys
import tempfile

CONTRACTS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "contracts")
TAGS = {
    "writ": [],
    "writ-embedded": ["--embedded", "n00b", "--contract-dir", CONTRACTS],
}


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


def main():
    writ = os.path.abspath(sys.argv[1])
    failed = False
    with tempfile.TemporaryDirectory() as scratch:
        for document in sys.argv[2:]:
            name = os.path.basename(document)
            ordinal = 0
            for _, tag, body in blocks(document):
                if tag not in TAGS:
                    continue
                ordinal += 1
                path = os.path.join(scratch, f"{name}-{ordinal}.writ")
                with open(path, "w", encoding="utf-8") as out:
                    out.write("\n".join(body) + "\n")
                flags = TAGS[tag]
                parsed = subprocess.run([writ, *flags, "parse", path], capture_output=True, text=True)
                checked = subprocess.run([writ, *flags, "--platform", "aarch64-apple-darwin", "check", path],
                                         capture_output=True, text=True)
                print(f"== {name} example {ordinal}")
                if parsed.returncode != 0:
                    failed = True
                    print("parse failed")
                print((checked.stdout + checked.stderr).replace(scratch + os.sep, ""), end="")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
