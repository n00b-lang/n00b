#!/usr/bin/env python3
"""Attempt to prove collated-010: codegen errors still allow execution."""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path


MARKER = "PROOF_BEFORE_USE"


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def source_text() -> str:
    return f'print("{MARKER}")\nuse sample.module\n0\n'


def run_n00b(n00b: Path) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory(prefix="n00b-collated-010.") as tmp:
        source_path = Path(tmp) / "codegen_error_exec.n"
        source_path.write_text(source_text(), encoding="utf-8")

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

    try:
        result = run_n00b(n00b)
    except subprocess.TimeoutExpired:
        print("NOT PROVED: n00b timed out", file=sys.stderr)
        return 2

    stdout_lines = result.stdout.splitlines()
    stderr = result.stderr.strip()
    proved = (
        result.returncode != 0
        and MARKER in stdout_lines
        and "CG006" in result.stderr
    )

    print("collated-010 proof attempt")
    print(f"n00b: {n00b}")
    print("fixture: print marker before unsupported use statement")
    print(f"exit: {result.returncode}")
    print(f"stdout: {stdout_lines!r}")
    print(f"stderr: {stderr!r}")

    if proved:
        print("PROVED: generated code ran before CG006 forced command failure.")
        return 0

    print("NOT PROVED: expected marker on stdout and CG006 on stderr.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
