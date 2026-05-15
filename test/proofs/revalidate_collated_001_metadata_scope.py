#!/usr/bin/env python3
"""Revalidate collated-001: module metadata is keyed by bare names."""

from __future__ import annotations

import argparse
import sys
import tempfile
from pathlib import Path

from inspector_revalidation_common import (
    base_result,
    ensure_grammar_link,
    exit_code_for_verdict,
    resolve_repo_path,
    run_command,
    write_result,
)


COLLATED_ID = "collated-001"
TITLE = "Module metadata is keyed by bare names"


def write_fixture(root: Path) -> tuple[Path, Path]:
    modules = root / "modules"
    modules.mkdir()
    (modules / "default_dep.n").write_text(
        "func shared(x: int = 101) -> int {\n"
        "    return x\n"
        "}\n",
        encoding="utf-8",
    )
    (modules / "strict_dep.n").write_text(
        "func shared(x: int) -> int {\n"
        "    return x\n"
        "}\n",
        encoding="utf-8",
    )

    default_then_strict = root / "main_default_then_strict.n"
    default_then_strict.write_text(
        'use default_dep from "modules"\n'
        'use strict_dep from "modules"\n'
        "\n"
        "print(shared())\n"
        "0\n",
        encoding="utf-8",
    )

    strict_then_default = root / "main_strict_then_default.n"
    strict_then_default.write_text(
        'use strict_dep from "modules"\n'
        'use default_dep from "modules"\n'
        "\n"
        "print(shared())\n"
        "0\n",
        encoding="utf-8",
    )
    return default_then_strict, strict_then_default


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

    with tempfile.TemporaryDirectory(prefix="n00b-revalidate-001.") as tmp:
        root = Path(tmp)
        ensure_grammar_link(root)
        first, second = write_fixture(root)
        first_cmd = run_command([n00b, "run", "--quiet", first], cwd=root, timeout=args.timeout)
        second_cmd = run_command([n00b, "run", "--quiet", second], cwd=root, timeout=args.timeout)

    first_out = first_cmd["stdout"].splitlines()
    second_out = second_cmd["stdout"].splitlines()
    first_text = first_cmd["stdout"] + first_cmd["stderr"]
    second_text = second_cmd["stdout"] + second_cmd["stderr"]
    both_ambiguous = (
        "ambiguous function name imported from multiple modules" in first_text
        and "ambiguous function name imported from multiple modules" in second_text
    )
    order_sensitive = (
        first_cmd["exit_code"] != second_cmd["exit_code"]
        or first_cmd["stdout"] != second_cmd["stdout"]
        or first_cmd["stderr"] != second_cmd["stderr"]
    )

    if "cannot find grammars/n00b.bnf" in first_text or "cannot find grammars/n00b.bnf" in second_text:
        verdict = "inconclusive"
        confidence = "low"
        summary = "The duplicate-name import probe failed before parsing because the grammar file was not visible."
    elif first_cmd["timed_out"] or second_cmd["timed_out"]:
        verdict = "inconclusive"
        confidence = "low"
        summary = "At least one duplicate-name import probe timed out before a metadata decision could be made."
    elif both_ambiguous:
        verdict = "refuted"
        confidence = "high"
        summary = (
            "Both duplicate-name import orders rejected the unqualified shared() call as ambiguous, "
            "so neither order borrowed the other module's bare-name metadata."
        )
    elif order_sensitive and (
        "CG020" in first_text
        or "CG020" in second_text
        or first_out == ["101"]
        or second_out == ["101"]
    ):
        verdict = "holds"
        confidence = "high"
        summary = (
            "Reversing only duplicate module import order changed the same shared() call behavior, "
            "which supports bare-name/session-scoped metadata lookup."
        )
    elif not order_sensitive:
        verdict = "refuted"
        confidence = "medium"
        summary = (
            "Both duplicate-name import orders produced the same behavior, so this build did not "
            "reproduce order-dependent bare-name metadata lookup."
        )
    else:
        verdict = "inconclusive"
        confidence = "low"
        summary = "The duplicate-name import orders differed, but not in a way tied to call metadata."

    observations = [
        f"default_dep then strict_dep: exit={first_cmd['exit_code']} stdout={first_out!r}",
        f"strict_dep then default_dep: exit={second_cmd['exit_code']} stdout={second_out!r}",
        f"both_ambiguous={both_ambiguous}",
    ]
    result = base_result(
        COLLATED_ID,
        TITLE,
        verdict,
        confidence,
        summary,
        commands=[first_cmd, second_cmd],
        observations=observations,
        artifacts=[
            {
                "kind": "reproduction",
                "description": "Temporary modules define duplicate public shared() functions with different default-argument metadata.",
            }
        ],
    )
    write_result(args.output_dir, result)
    return exit_code_for_verdict(verdict)


if __name__ == "__main__":
    raise SystemExit(main())
