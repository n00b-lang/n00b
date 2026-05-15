#!/usr/bin/env python3
"""Revalidate collated-013: compile cache writes follow symlinks."""

from __future__ import annotations

import argparse
import os
import tempfile
from pathlib import Path

from inspector_revalidation_common import (
    base_result,
    exit_code_for_verdict,
    fnv1a64,
    resolve_repo_path,
    run_command,
    write_result,
)


COLLATED_ID = "collated-013"
TITLE = "Compile cache writes follow symlinks"
SOURCE = b"print(42)\n0\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--n00b", type=Path, default=Path("build_debug/n00b"))
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--timeout", type=int, default=30)
    args = parser.parse_args()

    n00b = resolve_repo_path(args.n00b)
    if not n00b.is_file():
        result = base_result(
            COLLATED_ID,
            TITLE,
            "inconclusive",
            "low",
            f"n00b executable not found: {n00b}",
            limitations=[f"Missing executable: {n00b}"],
        )
        write_result(args.output_dir, result)
        return exit_code_for_verdict(result["verdict"])

    source_hash = fnv1a64(SOURCE)
    with tempfile.TemporaryDirectory(prefix="n00b-revalidate-013.") as tmp:
        tmp_path = Path(tmp)
        cache_dir = tmp_path / "cache"
        cache_dir.mkdir()
        source_path = tmp_path / "simple.n"
        source_path.write_bytes(SOURCE)
        cache_path = cache_dir / f"{source_hash:016x}.n00bcache"
        sentinel = tmp_path / "sentinel.txt"
        sentinel.write_text("SENTINEL_BEFORE\n", encoding="utf-8")
        os.symlink(sentinel, cache_path)
        before = sentinel.read_text(encoding="utf-8")
        command = run_command(
            [n00b, "compile", "--verbose", "--cache-only", "--cache-dir", cache_dir, source_path],
            timeout=args.timeout,
        )
        after = sentinel.read_text(encoding="utf-8") if sentinel.exists() else ""
        symlink_still_exists = cache_path.is_symlink()

    overwritten = before != after and after.startswith("N00B_MIR_CACHE_V1")

    if command["timed_out"]:
        verdict = "inconclusive"
        confidence = "low"
        summary = "The symlink cache-write probe timed out."
    elif overwritten:
        verdict = "holds"
        confidence = "high"
        summary = "Cache-only mode followed a preexisting cache-path symlink and overwrote the sentinel target."
    elif before == after and command["exit_code"] not in (None, 0):
        verdict = "refuted"
        confidence = "high"
        summary = "Cache-only mode rejected or failed the symlink write and left the sentinel target unchanged."
    elif before == after:
        verdict = "refuted"
        confidence = "medium"
        summary = "The cache symlink target was left unchanged."
    else:
        verdict = "inconclusive"
        confidence = "low"
        summary = "The sentinel changed, but not into recognizable cache metadata."

    observations = [
        f"cache path name={cache_path.name}",
        f"command exit={command['exit_code']} symlink_still_exists={symlink_still_exists} overwritten={overwritten}",
        f"sentinel before={before.strip()!r} after_prefix={after[:32]!r}",
    ]
    result = base_result(
        COLLATED_ID,
        TITLE,
        verdict,
        confidence,
        summary,
        commands=[command],
        observations=observations,
        artifacts=[
            {
                "kind": "reproduction",
                "description": "Temporary deterministic cache path replaced by a symlink to a sentinel file.",
            }
        ],
    )
    write_result(args.output_dir, result)
    return exit_code_for_verdict(verdict)


if __name__ == "__main__":
    raise SystemExit(main())
