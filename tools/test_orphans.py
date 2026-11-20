#!/usr/bin/env python3
"""Reports every file under tests/programs, and every script in tests/, that
no test reads. tests/gates.sh runs it from the source root.

A script is read when the manifest, a build file, a tool or another script
names it. A program is read when a test that is read names it:

- by its path or its file name;
- by a name a script builds from a prefix and a word of its own
  (`fmt_$name` with `name` among `branches ...`);
- by its name or module name in a script that builds paths from names
  (`"$name.n"`) or names modules on a command line, and that is about its
  directory (the script, or the manifest line running it, names
  `programs/DIR`);
- by a test that copies or loops over its whole directory (`"$dir"/*.n`,
  with that directory on the manifest line);
- by an import from a program that is read.

Any other file (a golden, tokens, an embedded `.bin`, a C source) is read
when it sits beside a read program of the same name, or when a test or a
read file names it. The diagnostics corpus has gates of its own.
tests/perf is the performance suite, not a test. tests/orphans-held lists
files kept, unread, until the change named beside each one lands.
"""

import os
import re
import sys

NOT_TESTS = ("tests/perf/",)
HELD = "tests/orphans-held"


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def code(text):
    return "\n".join(line.split("#", 1)[0] for line in text.splitlines())


TOKEN = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.-]*")
_tokens = {}


def word(text, name):
    # Whether `name` is a whole word of `text` (dots and dashes join words:
    # `enum` is not a word of `import-enum`).
    found = _tokens.get(id(text))
    if found is None:
        found = {t.rstrip(".-") for t in TOKEN.findall(text)}
        _tokens[id(text)] = found
    return name in found


def files_under(root):
    for base, _, names in os.walk(root):
        for name in names:
            yield os.path.join(base, name).replace(os.sep, "/")


def module_names(path):
    parts = path[len("tests/programs/"):-len(".n")].split("/")
    names = set()
    for start in range(len(parts)):
        tail = parts[start:]
        if tail[-1] == "__init":
            tail = tail[:-1]
        if tail:
            names.add(".".join(tail))
    return names


def main():
    scripts = sorted("tests/" + n for n in os.listdir("tests")
                     if n.endswith(".sh"))
    manifest = read("tests/n00b.tests")
    others = [p for p in ("tests/tests.mk", "tests/test-programs.mk",
                          "build.sh", "Makefile") if os.path.exists(p)]
    others += ["tools/" + n for n in sorted(os.listdir("tools"))
               if os.path.isfile("tools/" + n)
               and n != os.path.basename(__file__)]
    texts = {p: read(p) for p in scripts + others}
    texts["tests/n00b.tests"] = manifest
    held = {}
    if os.path.exists(HELD):
        for line in read(HELD).splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                path, _, why = line.partition(" ")
                held[path] = why.strip()
    missing = []

    used_scripts = set()
    for script in scripts:
        name = os.path.basename(script)
        if any(name in text for path, text in texts.items()
               if path != script):
            used_scripts.add(script)
        elif script not in held:
            missing.append(script)
    refs = {p: code(t) if p.endswith(".sh") else t for p, t in texts.items()
            if p not in scripts or p in used_scripts}
    all_refs = "\n".join(refs.values())
    lines = manifest.splitlines()
    prefixes = [(m.group(1), text) for text in refs.values()
                for m in re.finditer(r"([A-Za-z0-9_.-]+)\$", text)]

    def about(path, script, text):
        top = "programs/" + path[len("tests/programs/"):].split("/")[0]
        if top in text:
            return True
        name = os.path.basename(script)
        return any(name in line and top in line for line in lines)

    def named(path):
        rel = path[len("tests/"):]
        base = os.path.basename(path)
        if rel in all_refs or word(all_refs, base):
            return True
        for prefix, text in prefixes:
            rest = base.split(".")[0][len(prefix):] \
                if base.startswith(prefix) else ""
            if rest and (word(text, rest) or word(manifest, rest)):
                return True
        return False

    def passed_by_name(path):
        names = module_names(path)
        for script, text in refs.items():
            if any(word(text, n) for n in names) and about(path, script, text):
                return True
        return False

    programs = sorted(p for p in files_under("tests/programs")
                      if not p.startswith(NOT_TESTS))
    sources = [p for p in programs if p.endswith(".n")]
    read_set = {p for p in sources if named(p) or passed_by_name(p)}

    for line in lines:
        dirs = re.findall(r"\$SRC/(tests/programs/[A-Za-z0-9_/]+)", line)
        globbing = any("/*.n" in refs.get(s, "") for s in used_scripts
                       if os.path.basename(s) in line)
        for d in dirs if globbing else ():
            read_set.update(p for p in sources
                            if os.path.dirname(p) == d.rstrip("/"))

    imported = {}
    for path in sources:
        text = read(path)
        names = set(re.findall(r"^\s*import\s+([A-Za-z0-9_.]+)", text, re.M))
        names |= set(re.findall(r"\bfrom\s+([A-Za-z0-9_.]+)", text))
        imported[path] = names
    changed = True
    while changed:
        changed = False
        wanted = set()
        for path in read_set:
            for name in imported.get(path, ()):
                parts = name.split(".")
                wanted.update(".".join(parts[:i])
                              for i in range(1, len(parts) + 1))
        for path in sources:
            if path not in read_set and module_names(path) & wanted:
                read_set.add(path)
                changed = True

    # Other files: beside a read program, or named by a test or a read file.
    rest = [p for p in programs if not p.endswith(".n")]
    changed = True
    while changed:
        changed = False
        named_in = "\n".join(read(p) for p in read_set)
        for path in rest:
            if path in read_set:
                continue
            base = os.path.basename(path)
            sibling = os.path.join(os.path.dirname(path),
                                   base.split(".")[0] + ".n")
            # NAME.PLATFORM.expected stands in for a read NAME.expected.
            parts = base.split(".")
            general = os.path.join(os.path.dirname(path),
                                   ".".join(parts[:-2] + parts[-1:]))
            if (sibling in read_set or general in read_set
                    or named(path) or base in named_in):
                read_set.add(path)
                changed = True

    for path in programs:
        if path not in read_set and path not in held:
            missing.append(path)
    for path in held:
        if not os.path.exists(path):
            missing.append(f"{HELD}: {path} no longer exists")
        elif path in read_set:
            missing.append(f"{HELD}: a test reads {path} now; drop it")

    for path in missing:
        print(f"{path}: no test reads this file", file=sys.stderr)
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
