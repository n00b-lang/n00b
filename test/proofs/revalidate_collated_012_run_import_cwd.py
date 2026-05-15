#!/usr/bin/env python3
"""Revalidate collated-012: run-mode relative imports resolve from CWD."""

from __future__ import annotations

import argparse
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


COLLATED_ID = "collated-012"
TITLE = "Run-mode relative imports resolve from CWD"


def write_tree(root: Path) -> tuple[Path, Path]:
    trusted = root / "trusted"
    attacker = root / "attacker-cwd"
    (trusted / "modules").mkdir(parents=True)
    (attacker / "modules").mkdir(parents=True)
    ensure_grammar_link(trusted)
    ensure_grammar_link(attacker)

    (trusted / "modules" / "dep.n").write_text(
        "func dep_value() -> int {\n"
        "    return 111\n"
        "}\n",
        encoding="utf-8",
    )
    (attacker / "modules" / "dep.n").write_text(
        "func dep_value() -> int {\n"
        "    return 999\n"
        "}\n",
        encoding="utf-8",
    )
    main = trusted / "main.n"
    main.write_text(
        'use dep from "modules"\n'
        "\n"
        "print(dep_value())\n"
        "0\n",
        encoding="utf-8",
    )
    return main, attacker


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

    with tempfile.TemporaryDirectory(prefix="n00b-revalidate-012.") as tmp:
        root = Path(tmp)
        trusted_main, attacker = write_tree(root)
        trusted_cmd = run_command([n00b, "run", "--quiet", trusted_main], cwd=trusted_main.parent, timeout=args.timeout)
        attacker_cmd = run_command([n00b, "run", "--quiet", trusted_main], cwd=attacker, timeout=args.timeout)

    trusted_out = trusted_cmd["stdout"].splitlines()
    attacker_out = attacker_cmd["stdout"].splitlines()
    combined_text = trusted_cmd["stderr"] + attacker_cmd["stderr"]

    if "cannot find grammars/n00b.bnf" in combined_text:
        verdict = "inconclusive"
        confidence = "low"
        summary = "The relative-import probe failed before parsing because the grammar file was not visible."
    elif trusted_cmd["timed_out"] or attacker_cmd["timed_out"]:
        verdict = "inconclusive"
        confidence = "low"
        summary = "At least one relative-import probe timed out."
    elif trusted_out == ["111"] and attacker_out == ["999"]:
        verdict = "holds"
        confidence = "high"
        summary = "The same trusted source imported the trusted module from trusted CWD and the attacker module from attacker CWD."
    elif trusted_out == ["111"] and attacker_out == ["111"]:
        verdict = "refuted"
        confidence = "high"
        summary = "Relative import resolution was anchored to the trusted source directory, not process CWD."
    elif attacker_cmd["exit_code"] not in (None, 0) and not attacker_out:
        verdict = "refuted"
        confidence = "medium"
        summary = "Running from attacker CWD did not import the attacker module and instead rejected or failed the relative import."
    else:
        verdict = "inconclusive"
        confidence = "low"
        summary = "The relative-import probe produced unexpected output."

    observations = [
        f"trusted cwd exit={trusted_cmd['exit_code']} stdout={trusted_out!r}",
        f"attacker cwd exit={attacker_cmd['exit_code']} stdout={attacker_out!r}",
    ]
    result = base_result(
        COLLATED_ID,
        TITLE,
        verdict,
        confidence,
        summary,
        commands=[trusted_cmd, attacker_cmd],
        observations=observations,
        artifacts=[
            {
                "kind": "reproduction",
                "description": "Trusted and attacker directory trees both provide modules/dep.n with different dep_value() results.",
            }
        ],
    )
    write_result(args.output_dir, result)
    return exit_code_for_verdict(verdict)


if __name__ == "__main__":
    raise SystemExit(main())
