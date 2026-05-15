#!/usr/bin/env python3
"""Revalidate collated-006: codegen errors are treated as successful emission."""

from __future__ import annotations

import argparse
import tempfile
from pathlib import Path

from inspector_revalidation_common import (
    base_result,
    exit_code_for_verdict,
    resolve_repo_path,
    run_command,
    write_result,
)


COLLATED_ID = "collated-006"
TITLE = "Codegen errors are treated as successful emission"
MARKER = "CG_MARKER_BEFORE_SET"


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

    source = (
        f'print("{MARKER}")\n'
        "var xs = {1, 2}\n"
        "print(xs[0])\n"
        "0\n"
    )
    with tempfile.TemporaryDirectory(prefix="n00b-revalidate-006.") as tmp:
        tmp_path = Path(tmp)
        source_path = tmp_path / "marker_then_set_index_invalid.n"
        cache_dir = tmp_path / "cache"
        source_path.write_text(source, encoding="utf-8")
        run_cmd = run_command([n00b, "run", "--quiet", source_path], timeout=args.timeout)
        cache_cmd = run_command(
            [n00b, "compile", "--verbose", "--cache-only", "--cache-dir", cache_dir, source_path],
            timeout=args.timeout,
        )
        cache_files = sorted(path.name for path in cache_dir.glob("*.n00bcache")) if cache_dir.exists() else []

    run_text = run_cmd["stdout"] + run_cmd["stderr"]
    cache_text = cache_cmd["stdout"] + cache_cmd["stderr"]
    marker_executed = MARKER in run_cmd["stdout"].splitlines()
    run_holds = run_cmd["exit_code"] not in (None, 0) and marker_executed and "CG010" in run_text
    cache_holds = cache_cmd["exit_code"] == 0 and bool(cache_files) and "CG010" in cache_text

    if run_cmd["timed_out"] or cache_cmd["timed_out"]:
        verdict = "inconclusive"
        confidence = "low"
        summary = "The codegen-error execution or cache-only probe timed out."
    elif run_holds or cache_holds:
        verdict = "holds"
        confidence = "high"
        summary = (
            "A set-index codegen error still allowed earlier generated code to execute "
            "or allowed cache-only mode to write cache metadata."
        )
    elif run_cmd["exit_code"] not in (None, 0) and not marker_executed and cache_cmd["exit_code"] != 0 and not cache_files:
        verdict = "refuted"
        confidence = "high"
        summary = "Codegen errors prevented marker execution and prevented cache-only metadata writes."
    else:
        verdict = "inconclusive"
        confidence = "low"
        summary = "The probes did not match either the old false-success behavior or a clean fixed behavior."

    observations = [
        f"run exit={run_cmd['exit_code']} marker_executed={marker_executed}",
        f"cache-only exit={cache_cmd['exit_code']} cache_files={cache_files!r}",
    ]
    result = base_result(
        COLLATED_ID,
        TITLE,
        verdict,
        confidence,
        summary,
        commands=[run_cmd, cache_cmd],
        observations=observations,
        artifacts=[
            {
                "kind": "reproduction",
                "description": "Temporary n00b fixture prints a marker before indexing a set value.",
            }
        ],
    )
    write_result(args.output_dir, result)
    return exit_code_for_verdict(verdict)


if __name__ == "__main__":
    raise SystemExit(main())
