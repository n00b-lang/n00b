#!/usr/bin/env python3
"""Attempt to prove collated-006: 129-entry literals truncate to 128."""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def literal_source() -> str:
    list_items = ", ".join(str(i) for i in range(129))
    dict_items = ", ".join(f"{i}: {i}" for i in range(129))

    return "\n".join(
        [
            f"var xs = [{list_items}]",
            f"var d = {{{dict_items}}}",
            "",
            "print(len(xs))",
            "print(len(d))",
            "0",
            "",
        ]
    )


def run_n00b(n00b: Path, source: str) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory(prefix="n00b-collated-006.") as tmp:
        source_path = Path(tmp) / "large_literals_129.n"
        source_path.write_text(source, encoding="utf-8")

        return subprocess.run(
            [str(n00b), "run", "--quiet", str(source_path)],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=30,
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--n00b",
        type=Path,
        default=repo_root() / "build_debug" / "n00b",
        help="Path to the n00b executable.",
    )
    args = parser.parse_args()

    n00b = args.n00b
    if not n00b.is_absolute():
        n00b = repo_root() / n00b

    if not n00b.is_file():
        print(f"NOT PROVED: n00b executable not found: {n00b}", file=sys.stderr)
        return 2

    source = literal_source()
    try:
        result = run_n00b(n00b, source)
    except subprocess.TimeoutExpired:
        print("NOT PROVED: n00b timed out", file=sys.stderr)
        return 2

    stdout_lines = result.stdout.splitlines()
    proved = (
        result.returncode == 0
        and stdout_lines == ["128", "128"]
        and result.stderr.strip() == ""
    )

    print("collated-006 proof attempt")
    print(f"n00b: {n00b}")
    print("fixture: 129 list entries and 129 dict entries")
    print(f"exit: {result.returncode}")
    print(f"stdout: {stdout_lines!r}")
    print(f"stderr: {result.stderr.strip()!r}")

    if proved:
        print("PROVED: both literals compiled cleanly but len() reported 128.")
        return 0

    print("NOT PROVED: expected clean execution with stdout ['128', '128'].")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
