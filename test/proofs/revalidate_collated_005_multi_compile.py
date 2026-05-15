#!/usr/bin/env python3
"""Revalidate collated-005: multi-file compile emits duplicate _main symbols."""

from __future__ import annotations

import argparse
import tempfile
from pathlib import Path

from inspector_revalidation_common import (
    base_result,
    exit_code_for_verdict,
    repo_root,
    resolve_repo_path,
    run_command,
    write_result,
)


COLLATED_ID = "collated-005"
TITLE = "Multi-file compile emits duplicate _main symbols"


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

    with tempfile.TemporaryDirectory(prefix="n00b-revalidate-005.") as tmp:
        tmp_path = Path(tmp)
        first = tmp_path / "a.n"
        second = tmp_path / "b.n"
        first.write_text("0\n", encoding="utf-8")
        second.write_text("0\n", encoding="utf-8")
        command = run_command(
            [
                n00b,
                "compile",
                "--output",
                tmp_path / "out",
                "--lib-dir",
                repo_root() / "build_debug",
                first,
                second,
            ],
            timeout=args.timeout,
        )

    stderr = command["stderr"]
    duplicate_main = "multiple definition of `_main'" in stderr
    intentional_guard = "exactly one input file" in stderr

    if command["timed_out"]:
        verdict = "inconclusive"
        confidence = "low"
        summary = "The two-file compile probe timed out."
    elif duplicate_main:
        verdict = "holds"
        confidence = "high"
        summary = "Two input files reached the linker and failed with duplicate _main definitions."
    elif intentional_guard:
        verdict = "refuted"
        confidence = "high"
        summary = "Two input files were rejected before linking by an intentional compile-mode guard."
    else:
        verdict = "inconclusive"
        confidence = "low"
        summary = "The two-file compile probe failed for an unexpected reason."

    observations = [
        f"command exit={command['exit_code']}",
        f"duplicate_main={duplicate_main} intentional_guard={intentional_guard}",
        f"stderr_prefix={stderr[:240]!r}",
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
                "description": "Two minimal source files compiled in one n00b compile invocation.",
            }
        ],
    )
    write_result(args.output_dir, result)
    return exit_code_for_verdict(verdict)


if __name__ == "__main__":
    raise SystemExit(main())
