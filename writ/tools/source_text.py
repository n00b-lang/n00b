import pathlib
import re

STRIP = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/', re.S)


def code_without_literals(text):
    return STRIP.sub(lambda m: '""' if m.group(0).startswith('"') else " ", text)


def sources(root):
    return sorted(p for p in pathlib.Path(root).rglob("*") if p.suffix in (".c", ".h"))
