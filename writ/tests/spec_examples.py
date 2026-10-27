import concurrent.futures
import os
import re
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
        examples = []
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
                examples.append((name, ordinal, TAGS[tag], path))
        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 1) as pool:
            results = list(pool.map(lambda example: run_example(writ, example[2], example[3]), examples))
        for (name, ordinal, _, _), (parsed, output) in zip(examples, results):
            print(f"== {name} example {ordinal}")
            if parsed != 0:
                failed = True
                print("parse failed")
            print(prefix.sub("", output), end="")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
