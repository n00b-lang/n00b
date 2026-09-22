#!/usr/bin/env python3
"""Static guard/arity check for src/conduit/local_windows.c.

This file is compiled ONLY when target_os == 'windows' (src/conduit/meson.build),
so no macOS or Linux build ever sees it and `ninja` reports "no work to do" after
editing it. The only real signal is n00b's Windows CI lane, which costs ~20
minutes per attempt. Two defects reached that lane during n00b#411:

  1. our own typedefs placed inside the `#ifndef _WINDOWS` block, which declares
     the Win32 API surface for the NON-Windows build only -- invisible in the
     build that uses them;
  2. a helper whose call site gained a parameter its definition never did.

Extracting a function body into a standalone harness does NOT catch either: the
harness has no `#ifndef _WINDOWS` block and no other call sites. This script
does, by reading the real file.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src/conduit/local_windows.c"
HDR = ROOT / "src/conduit/local_windows_native.h"
CALLER = ROOT / "src/conduit/local.c"

def arity(params: str) -> int:
    p = params.strip()
    return 0 if p in ("", "void") else p.count(",") + 1

def call_arity(text: str, start: int) -> int:
    i, depth, args, body = start, 1, 0, ""
    while i < len(text) and depth > 0:
        ch = text[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                break
        elif ch == "," and depth == 1:
            args += 1
        body += ch
        i += 1
    return 0 if body.strip() == "" else args + 1

def main() -> int:
    src = SRC.read_text()
    lines = src.split("\n")
    problems = []

    # --- 1. nothing of OURS inside the #ifndef _WINDOWS fallback block --------
    start = end = None
    for i, l in enumerate(lines, start=1):
        if l.startswith("#ifndef _WINDOWS") and start is None:
            start = i
        elif start and end is None and l.startswith("#endif"):
            end = i
    if start and end:
        for i in range(start, end):
            l = lines[i - 1]
            # Our own symbols are local_windows_*; Win32 decls are not.
            if re.match(r"\s*typedef\b.*local_windows_\w+", l) or \
               re.match(r"\s+local_windows_\w+_fn\)", l):
                problems.append(
                    f"{SRC.name}:{i}: our own type declared inside the "
                    f"`#ifndef _WINDOWS` block ({start}-{end}); a real Windows "
                    f"build skips that block and will not see it")

    # --- 2. static helper definition arity vs every call site ----------------
    defs = {}
    for m in re.finditer(r"\nstatic\s+[\w \*]+\n(\w+)\(([^)]*)\)\s*\{", src):
        defs[m.group(1)] = (arity(m.group(2)), src[:m.start()].count("\n") + 2)
    for name, (want, defline) in defs.items():
        for m in re.finditer(r"(?<![\w>.])" + re.escape(name) + r"\s*\(", src):
            ln = src[:m.start()].count("\n") + 1
            if ln == defline:
                continue
            got = call_arity(src, m.end())
            if got != want:
                problems.append(
                    f"{SRC.name}:{ln}: {name} called with {got} args, "
                    f"defined at :{defline} with {want}")

    # --- 3. native ABI: header vs impl vs caller -----------------------------
    hdr, caller = HDR.read_text(), CALLER.read_text()
    decls = {m.group(1): arity(m.group(2)) for m in re.finditer(
        r"extern\s+[\w \*]+?\b(_n00b_conduit_local_windows_native_\w+)\(([^;]*?)\);",
        hdr, re.S)}
    impls = {m.group(1): arity(m.group(2)) for m in re.finditer(
        r"\n(_n00b_conduit_local_windows_native_\w+)\(([^)]*)\)\s*\{", src, re.S)}
    for name, want in sorted(decls.items()):
        if name in impls and impls[name] != want:
            problems.append(
                f"{HDR.name}: {name} declared with {want} params, "
                f"implemented with {impls[name]}")
        for m in re.finditer(re.escape(name) + r"\s*\(", caller):
            got = call_arity(caller, m.end())
            if got != want:
                ln = caller[:m.start()].count("\n") + 1
                problems.append(
                    f"{CALLER.name}:{ln}: {name} called with {got} args, "
                    f"declared with {want}")

    if problems:
        print("local_windows.c guard/arity check FAILED:\n", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return 1
    print("local_windows.c guard/arity check passed")
    return 0

if __name__ == "__main__":
    sys.exit(main())
