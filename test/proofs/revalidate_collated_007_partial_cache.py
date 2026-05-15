#!/usr/bin/env python3
"""Revalidate collated-007: partial cache artifacts can be reported as valid."""

from __future__ import annotations

import argparse
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


COLLATED_ID = "collated-007"
TITLE = "Partial cache artifacts can be reported as valid"
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
    with tempfile.TemporaryDirectory(prefix="n00b-revalidate-007.") as tmp:
        tmp_path = Path(tmp)
        cache_dir = tmp_path / "cache"
        cache_dir.mkdir()
        source_path = tmp_path / "simple.n"
        source_path.write_bytes(SOURCE)
        cache_path = cache_dir / f"{source_hash:016x}.n00bcache"
        cache_path.write_text(
            "N00B_MIR_CACHE_V1\n"
            f"source={source_path}\n"
            "module=_main\n"
            f"hash={source_hash:016x}\n"
            f"size={len(SOURCE)}\n",
            encoding="utf-8",
        )
        truncated_content = cache_path.read_text(encoding="utf-8")
        command = run_command(
            [n00b, "compile", "--verbose", "--cache-only", "--cache-dir", cache_dir, source_path],
            timeout=args.timeout,
        )
        after_content = cache_path.read_text(encoding="utf-8") if cache_path.exists() else ""

    restored = command["exit_code"] == 0 and "cache restored" in command["stdout"]
    still_truncated = after_content == truncated_content

    if command["timed_out"]:
        verdict = "inconclusive"
        confidence = "low"
        summary = "The truncated-cache probe timed out."
    elif restored and still_truncated:
        verdict = "holds"
        confidence = "high"
        summary = "A cache artifact truncated after the size line was accepted as cache restored."
    elif not restored:
        verdict = "refuted"
        confidence = "high"
        summary = "The cache reader did not report the truncated artifact as restored."
    else:
        verdict = "inconclusive"
        confidence = "low"
        summary = "The command restored the cache but also changed the artifact, so the result needs inspection."

    observations = [
        f"precreated cache path={cache_path.name}",
        f"command exit={command['exit_code']} restored={restored} still_truncated={still_truncated}",
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
                "description": "Temporary cache metadata file containing magic, source, module, hash, and size, with later fields omitted.",
            }
        ],
    )
    write_result(args.output_dir, result)
    return exit_code_for_verdict(verdict)


if __name__ == "__main__":
    raise SystemExit(main())
