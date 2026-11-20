#!/usr/bin/env python3
import pathlib
import sys

import check_comments
import comments
import source_tree


def main(argv):
    args = [arg for arg in argv if not arg.startswith("--")]
    root = pathlib.Path(args[0] if args else pathlib.Path(__file__).parent.parent).resolve()
    n00b = next((arg.split("=", 1)[1] for arg in argv if arg.startswith("--n00b=")),
                str(root / "build" / "n00b"))
    failed, n00b_files = 0, []
    for path, lang in check_comments.targets(root):
        text = source_tree.read(root, path) if lang != "n00b" else None
        if lang == "n00b":
            n00b_files.append(path)
            continue
        try:
            new = comments.strip(text, lang) if text is not None else text
        except (comments.LexError, SyntaxError) as err:
            print(f"{path}: {err}")
            failed = 1
            continue
        if new != text:
            (root / path).write_bytes(new.encode("utf-8"))
            print(f"stripped {path}")
    try:
        check_comments.n00b_fmt(n00b, root, n00b_files, ["--strip-comments"])
    except (check_comments.Unmatched, OSError) as err:
        print(f"cannot strip .n files: {err}")
        failed = 1
    return failed


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
