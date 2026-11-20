import fnmatch
import pathlib
import subprocess
import sys

import comments
import source_tree

STRIP_CHECK = ["--strip-comments", "--check"]
BATCH = 200


def n00b_strips(n00b):
    try:
        usage = subprocess.run([n00b, "fmt", "--help"], capture_output=True, text=True).stdout
    except OSError:
        return False
    return "--strip-comments" in usage


def batches(paths):
    pending = list(paths)
    while pending:
        names = set()
        batch = []
        rest = []
        for path in pending:
            name = path.rsplit("/", 1)[-1]
            if name in names or len(batch) == BATCH:
                rest.append(path)
            else:
                names.add(name)
                batch.append(path)
        yield batch
        pending = rest


def n00b_commented(n00b, root, paths):
    root = pathlib.Path(root).resolve()

    def changed(flags, batch):
        out = subprocess.run([n00b, "fmt", *flags, *batch], cwd=root, capture_output=True, text=True).stdout
        return {pathlib.Path(root, line.strip()).resolve() for line in out.splitlines()
                if line.strip().endswith(".n")}

    found = []
    for batch in batches(paths):
        stripped = changed(STRIP_CHECK, batch) - changed(["--check"], batch)
        found += [path for path in batch if (root / path).resolve() in stripped]
    return found


def main(argv):
    args = [arg for arg in argv if not arg.startswith("--")]
    root = pathlib.Path(args[0] if args else ".")
    n00b = next((arg.split("=", 1)[1] for arg in argv if arg.startswith("--n00b=")), None)
    allowlist = root / "tests/comment-allowlist"
    lines = [line.strip() for line in allowlist.read_text().splitlines() if line.strip()] \
        if allowlist.exists() else ["enforce on"]
    enforced = "--enforce" in argv or lines[:1] == ["enforce on"]
    allowed = [line for line in lines if not line.startswith("enforce ")]
    found = []
    n00b_files = []
    for path in source_tree.files(root):
        lang = comments.language(path)
        if not lang or any(fnmatch.fnmatch(path, pattern) for pattern in allowed):
            continue
        if lang == "n00b":
            n00b_files.append(path)
            continue
        text = source_tree.read(root, path)
        if text is not None:
            found += [f"{path}:{line}: {body.splitlines()[0][:72]}"
                      for line, body in comments.comments(text, lang)]
    note = ""
    if n00b and n00b_strips(n00b):
        found += [f"{path}: n00b fmt --strip-comments changes more than n00b fmt does"
                  for path in n00b_commented(n00b, root, n00b_files)]
    elif n00b_files:
        note = (f"; {len(n00b_files)} .n files not checked: pass --n00b=PATH to a compiler "
                "whose fmt has --strip-comments")
    if not found:
        print(f"no comments{note}")
        return 0
    if not enforced:
        print(f"{len(found)} comments; not enforced until line 1 of tests/comment-allowlist "
              f"reads 'enforce on'{note}")
        return 0
    print("\n".join(found))
    print(f"{len(found)} comments{note}. Delete them; this repository keeps no comments in source. "
          "Run n00b fmt --strip-comments on .n files and python3 tools/strip_comments.py on the "
          "rest, or edit by hand. If a file is a test input whose comments are what it tests, add "
          "its path or a glob to tests/comment-allowlist.")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
