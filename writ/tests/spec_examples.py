import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import tempfile

CONTRACTS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "contracts")
EMBEDDED = ["--embedded", "n00b", "--contract-dir", CONTRACTS]


def examples(directory):
    for name in sorted(os.listdir(directory)):
        if name.endswith(".writ"):
            embedded = name.endswith(".embedded.writ")
            yield int(name.split(".", 1)[0]), EMBEDDED if embedded else [], os.path.join(directory, name)


def run_example(writ, flags, path):
    parsed = subprocess.run([writ, *flags, "parse", path], capture_output=True, text=True)
    checked = subprocess.run([writ, *flags, "--platform", "aarch64-apple-darwin", "check", path],
                             capture_output=True, text=True)
    return parsed.returncode, checked.stdout + checked.stderr


def main():
    writ = os.path.abspath(sys.argv[1])
    failed = False
    with tempfile.TemporaryDirectory() as scratch:
        prefix = re.compile(r"[^\s`]*" + re.escape(os.path.basename(scratch)) + r"[/\\]")
        found = []
        for directory in sys.argv[2:]:
            label = os.path.basename(os.path.normpath(directory))
            for ordinal, flags, source in examples(directory):
                path = os.path.join(scratch, f"{label}-{ordinal}.writ")
                shutil.copyfile(source, path)
                found.append((label, ordinal, flags, path))
        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 1) as pool:
            results = list(pool.map(lambda example: run_example(writ, example[2], example[3]), found))
        for (label, ordinal, _, _), (parsed, output) in zip(found, results):
            print(f"== {label} example {ordinal}")
            if parsed != 0:
                failed = True
                print("parse failed")
            print(prefix.sub("", output), end="")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
