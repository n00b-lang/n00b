import os
import subprocess
import sys
import tempfile

from spec_examples import blocks

writ = os.path.abspath(sys.argv[1])
inputs = sys.argv[2:]


def run(*args):
    return subprocess.run([writ, *args], capture_output=True, text=True)


def sources(scratch):
    for path in inputs:
        if not path.endswith(".md"):
            yield path, path
            continue
        ordinal = 0
        for _, tag, body in blocks(path):
            if tag != "writ":
                continue
            ordinal += 1
            name = os.path.join(scratch, f"{os.path.basename(path)}-{ordinal}.writ")
            with open(name, "w", encoding="utf-8") as out:
                out.write("\n".join(body) + "\n")
            yield name, f"{path} example {ordinal}"


failures = 0
with tempfile.TemporaryDirectory() as scratch:
    for path, label in sources(scratch):
        original = run("parse", path)
        formatted = run("fmt", path)
        once = os.path.join(scratch, "once.writ")
        with open(once, "w", encoding="utf-8") as out:
            out.write(formatted.stdout)
        reparsed = run("parse", once)
        again = run("fmt", once)
        if original.returncode or formatted.returncode:
            print(f"{label}: does not parse")
            failures += 1
        elif reparsed.stdout != original.stdout:
            print(f"{label}: formatting changed the syntax tree")
            failures += 1
        elif again.stdout != formatted.stdout:
            print(f"{label}: formatting is not idempotent")
            failures += 1

print("fmt round-trip ok" if not failures else f"{failures} failures")
sys.exit(1 if failures else 0)
