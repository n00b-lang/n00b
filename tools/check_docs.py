import fnmatch
import pathlib
import re
import sys

import source_tree

DOC_SUFFIX = re.compile(r"\.(md|markdown|mdx|rst|adoc|asciidoc|org|textile|rtf|docx?|pdf)$", re.I)
DOC_NAME = re.compile(r"^(readme|notes|todo|changelog|changes|design|hacking|authors|history|"
                      r"contributing|plan|summary|report|analysis|findings)([_-][^.]*)?(\.te?xt)?$", re.I)


def doc_like(path):
    name = path.rsplit("/", 1)[-1]
    in_tests = "tests" in path.split("/")[:-1]
    return bool(DOC_SUFFIX.search(name) or DOC_NAME.match(name)
                or (name.lower().endswith(".txt") and not in_tests))


def main(argv):
    root = pathlib.Path(argv[0] if argv else ".")
    allowlist = root / "tests/doc-allowlist"
    allowed = [line.strip() for line in allowlist.read_text().splitlines() if line.strip()] \
        if allowlist.exists() else []
    found = [path for path in source_tree.files(root)
             if not path.startswith("docs/") and doc_like(path)
             and not any(fnmatch.fnmatch(path, pattern) for pattern in allowed)]
    if not found:
        print("no documentation outside docs/")
        return 0
    print("\n".join(found))
    print(f"{len(found)} documentation files outside docs/. Move what a person must read into docs/, "
          "and delete notes, plans and reports written for a change; put those in the change "
          "description. A file that is not documentation but has a doc-like name (a license, a "
          "test input) goes in tests/doc-allowlist.")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
