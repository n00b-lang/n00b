#!/usr/bin/env python3
"""Shared helpers for Inspector Gadget manual revalidation scripts."""

from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path
from typing import Any


FNV_OFFSET_64 = 1469598103934665603
FNV_PRIME_64 = 1099511628211
MASK_64 = (1 << 64) - 1


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def resolve_repo_path(path: Path) -> Path:
    if path.is_absolute():
        return path
    return repo_root() / path


def ensure_grammar_link(cwd: Path) -> None:
    """Make the repo grammar visible to n00b when a proof changes CWD."""
    target = repo_root() / "grammars"
    link = cwd / "grammars"
    if link.exists() or link.is_symlink():
        return
    os.symlink(target, link, target_is_directory=True)


def run_command(
    argv: list[str | Path],
    cwd: Path | None = None,
    timeout: int = 30,
) -> dict[str, Any]:
    str_argv = [str(arg) for arg in argv]
    command_cwd = cwd if cwd is not None else repo_root()

    try:
        completed = subprocess.run(
            str_argv,
            cwd=command_cwd,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
        )
        return {
            "argv": str_argv,
            "cwd": str(command_cwd),
            "exit_code": completed.returncode,
            "stdout": completed.stdout,
            "stderr": completed.stderr,
            "timed_out": False,
        }
    except subprocess.TimeoutExpired as exc:
        return {
            "argv": str_argv,
            "cwd": str(command_cwd),
            "exit_code": None,
            "stdout": exc.stdout or "",
            "stderr": exc.stderr or "",
            "timed_out": True,
        }


def fnv1a64(data: bytes) -> int:
    value = FNV_OFFSET_64
    for byte in data:
        value ^= byte
        value = (value * FNV_PRIME_64) & MASK_64
    return value


def result_filename(collated_id: str) -> str:
    return f"revalidate-{collated_id}.json"


def write_result(output_dir: Path | None, result: dict[str, Any]) -> None:
    if output_dir is not None:
        output_dir.mkdir(parents=True, exist_ok=True)
        path = output_dir / result_filename(result["collated_id"])
        path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    brief = {
        "collated_id": result["collated_id"],
        "verdict": result["verdict"],
        "confidence": result["confidence"],
        "summary": result["summary"],
    }
    print(json.dumps(brief, indent=2, sort_keys=True))


def exit_code_for_verdict(verdict: str) -> int:
    return 2 if verdict == "inconclusive" else 0


def base_result(
    collated_id: str,
    title: str,
    verdict: str,
    confidence: str,
    summary: str,
    commands: list[dict[str, Any]] | None = None,
    observations: list[str] | None = None,
    artifacts: list[dict[str, Any]] | None = None,
    limitations: list[str] | None = None,
) -> dict[str, Any]:
    return {
        "schema_version": 1,
        "collated_id": collated_id,
        "title": title,
        "verdict": verdict,
        "confidence": confidence,
        "summary": summary,
        "commands": commands or [],
        "observations": observations or [],
        "artifacts": artifacts or [],
        "limitations": limitations or [],
    }
