import os
import subprocess
import sys
import tempfile

from spec_examples import EMBEDDED, examples

writ = os.path.abspath(sys.argv[1])
inputs = sys.argv[2:]


def run(flags, *args):
    return subprocess.run([writ, *flags, *args], capture_output=True, text=True)


def sources(scratch):
    for path in inputs:
        if not os.path.isdir(path):
            embedded = os.sep + "embedded" + os.sep in path
            yield path, path, EMBEDDED if embedded else []
            continue
        for ordinal, flags, source in examples(path):
            yield source, f"{path} example {ordinal}", flags


failures = 0
with tempfile.TemporaryDirectory() as scratch:
    for path, label, flags in sources(scratch):
        original = run(flags, "parse", path)
        formatted = run(flags, "fmt", path)
        once = os.path.join(scratch, "once.writ")
        with open(once, "w", encoding="utf-8") as out:
            out.write(formatted.stdout)
        reparsed = run(flags, "parse", once)
        again = run(flags, "fmt", once)
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
