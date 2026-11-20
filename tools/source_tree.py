import functools
import os
import pathlib
import subprocess

import comments

SKIP_DIRS = {".git", ".jj", ".claude", "__pycache__", "vendor", "node_modules"}


def _listed(root, command):
    try:
        out = subprocess.run(command, cwd=root, capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    return sorted(line for line in out.splitlines() if line)


@functools.lru_cache(maxsize=None)
def _files(root):
    root = pathlib.Path(root)
    listed = None
    if (root / ".jj").is_dir():
        listed = _listed(root, ["jj", "--no-pager", "file", "list"])
    if listed is None and (root / ".git").exists():
        listed = _listed(root, ["git", "ls-files", "--cached", "--others", "--exclude-standard"])
    if listed is None:
        listed = []
        for directory, names, found in os.walk(root):
            names[:] = sorted(n for n in names if n not in SKIP_DIRS and not n.startswith("build"))
            base = pathlib.Path(directory).relative_to(root)
            listed += [(base / name).as_posix() for name in sorted(found)]
    return tuple(path for path in listed
                 if not SKIP_DIRS.intersection(path.split("/")[:-1]) and (root / path).is_file())


def files(root):
    return _files(str(pathlib.Path(root).resolve()))


@functools.lru_cache(maxsize=None)
def _read(root, path):
    try:
        return (pathlib.Path(root) / path).read_text(encoding="utf-8")
    except (UnicodeDecodeError, OSError):
        return None


def read(root, path):
    return _read(str(pathlib.Path(root).resolve()), path)


@functools.lru_cache(maxsize=None)
def _code(root, path, lang):
    text = _read(root, path)
    return None if text is None else comments.blank(text, lang)


def code(root, path, lang):
    return _code(str(pathlib.Path(root).resolve()), path, lang)
