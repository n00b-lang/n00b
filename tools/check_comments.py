import concurrent.futures
import fnmatch
import os
import pathlib
import re
import subprocess
import sys

import comments
import source_tree

REPORTED = re.compile(r"^(.+?\.n):\d+:\d+:", re.M)


class Unmatched(Exception):
    pass


def allowlist(root):
    path = pathlib.Path(root) / "tests/comment-allowlist"
    return [line.strip() for line in path.read_text().splitlines() if line.strip()] \
        if path.exists() else ["enforce on"]


def targets(root):
    allowed = [line for line in allowlist(root) if not line.startswith("enforce ")]
    for path in source_tree.files(root):
        lang = comments.language(path)
        if lang and not any(fnmatch.fnmatch(path, pattern) for pattern in allowed):
            yield path, lang


def n00b_strips(n00b):
    try:
        usage = subprocess.run([n00b, "fmt", "--help"], capture_output=True, text=True).stdout
    except OSError:
        return False
    return "--strip-comments" in usage


def same_file(reported, path):
    name = reported.strip().replace("\\", "/")
    return name == path or name.endswith("/" + path)


def n00b_fmt(n00b, root, paths, flags):
    n00b = os.path.abspath(n00b) if os.sep in n00b else n00b

    def run(batch):
        done = subprocess.run([n00b, "fmt", *flags, *batch], cwd=root, capture_output=True, text=True)
        reported = REPORTED.findall(done.stderr)
        broken = {path for path in batch if any(same_file(name, path) for name in reported)}
        if broken:
            rest = [path for path in batch if path not in broken]
            return run(rest) if rest else set()
        named = [line.strip() for line in done.stdout.splitlines() if line.strip()]
        if done.returncode != 0 and not named:
            if len(batch) > 1:
                return set().union(*(run([path]) for path in batch))
            raise Unmatched(f"n00b fmt {' '.join(flags)} {batch[0]} failed: {done.stderr.strip()}")
        found = {path for path in batch for name in named if same_file(name, path)}
        if len(found) != len(named) or (named and done.returncode == 0):
            raise Unmatched(f"n00b fmt {' '.join(flags)} reported {named}, which do not match "
                            f"the {len(batch)} files it was given")
        return found

    seen, groups = {}, {}
    for path in paths:
        name = path.rsplit("/", 1)[-1]
        seen[name] = seen.get(name, -1) + 1
        groups.setdefault(seen[name], []).append(path)
    batches = [group[i:i + 64] for group in groups.values() for i in range(0, len(group), 64)]
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        return set().union(set(), *pool.map(run, batches))


def main(argv):
    args = [arg for arg in argv if not arg.startswith("--")]
    root = pathlib.Path(args[0] if args else ".").resolve()
    n00b = next((arg.split("=", 1)[1] for arg in argv if arg.startswith("--n00b=")), None)
    enforced = "--enforce" in argv or allowlist(root)[:1] == ["enforce on"]
    found, n00b_files, note = [], [], ""
    for path, lang in targets(root):
        text = source_tree.read(root, path) if lang != "n00b" else None
        if lang == "n00b":
            n00b_files.append(path)
        elif text is not None:
            found += [f"{path}:{line}: {body.splitlines()[0][:72]}"
                      for line, body in comments.comments(text, lang)]
    if n00b and n00b_strips(n00b):
        try:
            found += [f"{path}: n00b fmt --strip-comments would remove comments"
                      for path in sorted(n00b_fmt(n00b, root, n00b_files, ["--strip-comments", "--check"]))]
        except Unmatched as err:
            print(f"cannot check .n files: {err}")
            return 1
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
          "Run python3 tools/strip_comments.py, or edit by hand. If a file is a test input whose "
          "comments are what it tests, add its path or a glob to tests/comment-allowlist.")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
