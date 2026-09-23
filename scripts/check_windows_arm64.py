#!/usr/bin/env python3
"""Catch Windows code that is gated on the OS but not on the architecture.

n00b#444 was the archetype: src/debug/platform/windows.c was an x86
debug-register backend behind a bare `#if defined(_WIN32)`.  Windows-on-ARM64
defines _WIN32 too, so it compiled and failed on 47 references to CONTEXT
members that do not exist in the ARM64 CONTEXT.  It was the only thing in
libn00b blocking a native windows-arm64 build (crashappsec/wax#905).

NO CI LANE COMPILES THAT CODE.  windows-component.yml targets
x86_64-pc-windows-msvc; build-and-test is macOS + Ubuntu.  Every existing lane
takes the x86 branch, so a green run says nothing about arm64.  This script is
what closes that, and it deliberately needs no Windows runner: clang
cross-compiles to Windows targets from any host, and mingw-w64 supplies
arch-correct headers (its CONTEXT really does carry Dr0-Dr7 on x86 and Bvr/Wvr
banks on arm64 -- verified, see the self-test below).

TWO CHECKS, because the bug family has two failure modes:

  syntax   Compile each file for BOTH aarch64- and x86_64-pc-windows-gnu and
           compare the in-file error counts.  It is the DIFFERENCE that is the
           signal, not the absolute count: most files cannot be syntax-checked
           standalone (n00b's generated headers and codegen macros are not
           present), and that noise is identical on both targets, so it
           cancels.  A file that errors MORE on aarch64 than on x86_64 has an
           arch-specific defect.  This is self-calibrating -- there is no
           allowlist to fall out of date, and adding a Windows file to the
           tree opts it in automatically.

  gates    Scan for arch switches whose non-x86 branch silently degrades
           rather than failing.  A compiler cannot catch this one: n00b#440
           (crash_capture.c) COMPILED on arm64 and fell through to
           `(void)uctx;`, leaving crash reports with no registers and `valid`
           never set -- discovered in the field, not at build time.  That is
           the worse failure, and the reason this script does not stop at the
           syntax check.

Exit status is nonzero if either check finds something.  --self-test verifies
the checks can actually fail, because a guard check that cannot fail is worse
than none (it reports success forever).
"""

import argparse
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Where mingw-w64 puts its headers.  Ubuntu's mingw-w64 package uses the first;
# Homebrew's uses the second (handy for reproducing a CI failure locally).
HEADER_CANDIDATES = [
    "/usr/share/mingw-w64/include",
    "/opt/homebrew/opt/mingw-w64/toolchain-x86_64/x86_64-w64-mingw32/include",
    "/usr/local/opt/mingw-w64/toolchain-x86_64/x86_64-w64-mingw32/include",
]

# -std=c2x is REQUIRED, not cosmetic.  n00b is C23 and uses `nullptr`; without
# it clang reports every `nullptr` as an undeclared identifier in BOTH arms.
# Those cancel in the differential, but they inflate the raw counts and have
# already confused one manual reproduction (47/0 becomes 54/7).  Pin it so the
# numbers in the issue, the PR and this script all agree.
COMMON_FLAGS = [
    "-fsyntax-only",
    "-std=c2x",
    "-ferror-limit=0",  # count them all; the default limit truncates at 19
    "-Wno-pragma-pack",  # mingw's own headers trip this; not our code
    "-D_WINDOWS",  # what the real Windows build defines (see platform.h:44)
]

TARGETS = {"aarch64": "aarch64-pc-windows-gnu", "x86_64": "x86_64-pc-windows-gnu"}

WINDOWS_GATE = re.compile(r"defined\(_WIN32\)|defined\(_WINDOWS\)|#ifdef\s+_WIN32|#ifdef\s+_WINDOWS")

# An arch switch that mentions x86 explicitly.  These are the ones that can
# strand arm64 in a fallthrough.
X86_GATE = re.compile(r"#\s*(?:if|elif)\s+.*(?:_M_X64|__x86_64__)")
ARM_MENTION = re.compile(r"_M_ARM64|__aarch64__")

# An else-branch that refuses loudly rather than degrading quietly. `#error`
# stops the build; an explicit *_UNSUPPORTED return refuses at runtime in a way
# callers already handle. Either is a deliberate answer; `(void)x;` is not.
ACCEPTABLE_ELSE = re.compile(r"#\s*error|UNSUPPORTED|NOT_SUPPORTED")


def find_headers():
    for path in HEADER_CANDIDATES:
        if os.path.isdir(path):
            return path
    sys.exit(
        "error: mingw-w64 headers not found. Install with `apt-get install mingw-w64` "
        "(Ubuntu) or `brew install mingw-w64` (macOS).\nLooked in:\n  "
        + "\n  ".join(HEADER_CANDIDATES)
    )


def count_errors(path, target, headers, source=None):
    """In-file error count for one file on one target.

    Only diagnostics whose location is THIS file are counted.  Errors raised
    inside included headers belong to the include graph, not to the file under
    test, and they are identical on both targets anyway.
    """
    cmd = [
        "clang",
        f"--target={TARGETS[target]}",
        *COMMON_FLAGS,
        f"-I{headers}",
        f"-I{os.path.join(REPO, 'include')}",
        f"-I{os.path.join(REPO, 'include', 'internal')}",
    ]
    # Hand clang the path RELATIVE to REPO and run with cwd=REPO, so its
    # diagnostics come back relative and match `needle` below.  Passing an
    # absolute path makes clang print absolute locations, the needle never
    # matches, and every file silently scores 0 -- i.e. the check passes on a
    # tree that is broken.  That bug was in this script and was caught only
    # because a known-bad file came back clean; --self-test now pins it.
    rel = None if source is not None else os.path.relpath(path, REPO)
    if source is not None:
        cmd += ["-x", "c", "-"]
    else:
        cmd += [rel]
    try:
        proc = subprocess.run(
            cmd,
            input=source,
            capture_output=True,
            text=True,
            timeout=300,
            cwd=REPO,
        )
    except subprocess.TimeoutExpired:
        return None
    needle = "<stdin>:" if source is not None else rel + ":"
    return sum(
        1 for line in proc.stderr.splitlines() if line.startswith(needle) and "error:" in line
    )


def windows_sources():
    out = []
    for root, _dirs, files in os.walk(os.path.join(REPO, "src")):
        for name in sorted(files):
            if not name.endswith((".c", ".h")):
                continue
            path = os.path.join(root, name)
            try:
                with open(path, encoding="utf-8", errors="replace") as handle:
                    if WINDOWS_GATE.search(handle.read()):
                        out.append(path)
            except OSError:
                continue
    return sorted(out)


def check_syntax(paths, headers, jobs):
    """Differential syntax check.  Returns a list of failure dicts."""

    def one(path):
        arm = count_errors(path, "aarch64", headers)
        x86 = count_errors(path, "x86_64", headers)
        if arm is None or x86 is None:
            return {"path": path, "timeout": True}
        return {"path": path, "arm": arm, "x86": x86}

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(one, paths))

    failures = []
    for r in results:
        if r.get("timeout"):
            failures.append(r)
        # Only a file that is CLEAN for x86_64-windows can be judged. If it
        # already errors there it does not syntax-check standalone (n00b's
        # generated headers and codegen macros are absent), and the error
        # recovery from that cascade is not identical across targets -- so a
        # small delta is an artifact, not a defect.
        #
        # Measured: src/core/init.c came back 56 vs 55 on Ubuntu/clang-18 and
        # equal on macOS/clang-21. The single extra diagnostic was
        # `environ = envp;` -> "expression is not assignable", a mingw
        # header-macro difference, with nothing wrong in n00b. Flagging on a
        # raw delta would have made this check fail on one runner and pass on
        # another, which is how a guard gets switched off.
        #
        # This costs nothing against the bug it exists for: windows.c scored
        # x86_64=0, aarch64=47 and is still caught.
        elif r["x86"] == 0 and r["arm"] > 0:
            failures.append(r)
    return results, failures


def check_gates(paths):
    """Find arch switches whose non-x86 branch silently degrades.

    Reports an x86 arch switch when the construct never mentions arm64 AND its
    else-branch is not an #error.  Such a branch compiles on arm64 and does
    something other than what the x86 branch does -- n00b#440's `(void)uctx;`
    being the case that shipped.
    """
    findings = []
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as handle:
            lines = handle.readlines()
        for i, line in enumerate(lines):
            if not X86_GATE.search(line):
                continue
            # If this is an #elif, the arm64 case is very often an EARLIER
            # branch of the same chain -- crash.c:388 dispatches
            # apple/arm64, apple/x86, linux/arm64, linux/x86 in that order.
            # Judging the chain from the x86 branch forward would flag all
            # four of those as unhandled, so walk back to the opening #if
            # first and consider the whole construct.
            start = i
            if re.match(r"#\s*elif", lines[i].lstrip()):
                depth = 0
                for j in range(i - 1, -1, -1):
                    stripped = lines[j].lstrip()
                    if re.match(r"#\s*endif", stripped):
                        depth += 1
                    elif re.match(r"#\s*if", stripped):
                        if depth == 0:
                            start = j
                            break
                        depth -= 1
            # Walk to the matching #else/#endif at the same nesting depth.
            depth, else_at, end_at = 0, None, None
            for j in range(start + 1, len(lines)):
                stripped = lines[j].lstrip()
                if re.match(r"#\s*if", stripped):
                    depth += 1
                elif re.match(r"#\s*endif", stripped):
                    if depth == 0:
                        end_at = j
                        break
                    depth -= 1
                elif depth == 0 and re.match(r"#\s*(else|elif)", stripped):
                    if else_at is None:
                        else_at = j
            if end_at is None:
                continue
            region = "".join(lines[start : end_at + 1])
            if ARM_MENTION.search(region):
                continue  # arm64 is handled explicitly somewhere in the chain
            if else_at is None:
                continue  # no else: arm64 simply omits the block
            tail = "".join(lines[else_at : end_at + 1])
            if ACCEPTABLE_ELSE.search(tail):
                # Two shapes are correct here, and the difference matters.
                #
                #   #error        -- right when arm64 genuinely cannot work and
                #                    a build is better stopped (stw.c, thread.c:
                #                    the callstack switch has no arm64 fallback).
                #   UNSUPPORTED   -- right when the caller can cope at runtime.
                #                    n00b#448 uses this: hardware watchpoints go
                #                    missing on arm64, but the debug substrate
                #                    already handles a refusal (NO_SLOT when all
                #                    four DRs are busy), so #error would have
                #                    blocked the whole platform over an optional
                #                    feature.
                #
                # Accepting only #error would have flagged #448 -- the correct
                # fix for the very bug this script was written for.
                continue
            findings.append(
                {
                    "path": os.path.relpath(path, REPO),
                    "line": i + 1,
                    "else_line": else_at + 1,
                    "text": lines[i].strip(),
                }
            )
    return findings


def self_test(headers):
    """Prove both checks can fail.

    A check that cannot fail silently reports success forever, which is how
    n00b#444 survived in the first place.
    """
    ok = True

    # (1) The syntax differential must fire on the real pre-#448 defect: an
    # x86-only CONTEXT read behind a bare _WIN32 gate.
    bad = (
        "#include <windows.h>\n"
        "#if defined(_WIN32)\n"
        "int probe(CONTEXT *c) { return (int)c->Dr0 + (int)c->Rax; }\n"
        "#endif\n"
    )
    arm = count_errors(None, "aarch64", headers, source=bad)
    x86 = count_errors(None, "x86_64", headers, source=bad)
    if arm is not None and x86 is not None and arm > x86:
        print(f"  self-test syntax   PASS  (unguarded x86 CONTEXT: aarch64={arm} x86_64={x86})")
    else:
        print(f"  self-test syntax   FAIL  (expected aarch64 > x86_64, got {arm} vs {x86})")
        ok = False

    # The same code, correctly gated, must come back clean -- otherwise the
    # check would flag every compliant file and get switched off.
    good = (
        "#include <windows.h>\n"
        "#if defined(_WIN32)\n"
        "#if defined(_M_X64) || defined(__x86_64__)\n"
        "int probe(CONTEXT *c) { return (int)c->Dr0 + (int)c->Rax; }\n"
        "#else\n"
        "int probe(void *c) { (void)c; return 0; }\n"
        "#endif\n"
        "#endif\n"
    )
    arm = count_errors(None, "aarch64", headers, source=good)
    x86 = count_errors(None, "x86_64", headers, source=good)
    if arm == x86 == 0:
        print("  self-test clean    PASS  (correctly gated file reports 0/0)")
    else:
        print(f"  self-test clean    FAIL  (expected 0/0, got {arm} vs {x86})")
        ok = False

    # (1b) End-to-end on a real file path, not just a stdin buffer.  The
    # stdin probes above passed while the file-path route was returning 0 for
    # everything (clang was handed an absolute path, so its diagnostics never
    # matched the relative needle).  A check that scores a broken tree as
    # clean is the failure mode this whole script exists to prevent, so pin
    # the path plumbing itself: write a file with an ungated x86 CONTEXT read
    # into the repo tree and require that check_syntax flags it.
    import tempfile

    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".c", prefix="arm64_selftest_", dir=os.path.join(REPO, "src"), delete=False
    ) as handle:
        probe_path = handle.name
        handle.write(
            "#include <windows.h>\n"
            "#if defined(_WIN32)\n"
            "int n00b_arm64_selftest(CONTEXT *c) { return (int)c->Dr0 + (int)c->Rax; }\n"
            "#endif\n"
        )
    try:
        _results, failures = check_syntax([probe_path], headers, 2)
        if failures and not failures[0].get("timeout"):
            print(
                "  self-test file-path PASS  (real file flagged: "
                f"aarch64={failures[0]['arm']} x86_64={failures[0]['x86']})"
            )
        else:
            print("  self-test file-path FAIL  (known-bad FILE not flagged -- path plumbing broken)")
            ok = False
    finally:
        os.unlink(probe_path)

    # (2) The gate scan must fire on n00b#440's shape: an x86 switch whose
    # else silently degrades instead of failing.

    with tempfile.TemporaryDirectory() as tmp:
        probe = os.path.join(tmp, "probe.c")
        with open(probe, "w", encoding="utf-8") as handle:
            handle.write(
                "#if defined(_WIN32)\n"
                "#if defined(__x86_64__) || defined(_M_X64)\n"
                "    regs->pc = ctx->Rip;\n"
                "#else\n"
                "    (void)uctx;\n"
                "#endif\n"
                "#endif\n"
            )
        if check_gates([probe]):
            print("  self-test gates    PASS  (silent arm64 fallthrough detected)")
        else:
            print("  self-test gates    FAIL  (silent fallthrough NOT detected)")
            ok = False

        with open(probe, "w", encoding="utf-8") as handle:
            handle.write(
                "#if defined(_WIN32)\n"
                "#if defined(__x86_64__) || defined(_M_X64)\n"
                "    regs->pc = ctx->Rip;\n"
                "#else\n"
                "#error \"unsupported architecture\"\n"
                "#endif\n"
                "#endif\n"
            )
        if not check_gates([probe]):
            print("  self-test gates-ok PASS  (#error branch accepted)")
        else:
            print("  self-test gates-ok FAIL  (#error branch wrongly flagged)")
            ok = False

        # An UNSUPPORTED return is equally correct and must not be flagged.
        # This is n00b#448's shape; an earlier revision of this script accepted
        # only #error and therefore flagged the correct fix for the exact bug
        # it was written to catch.
        with open(probe, "w", encoding="utf-8") as handle:
            handle.write(
                "#if defined(_WIN32)\n"
                "#if defined(_M_X64) || defined(__x86_64__)\n"
                "    return read_dr(ctx);\n"
                "#else\n"
                "    return N00B_DEBUG_ERR_UNSUPPORTED;\n"
                "#endif\n"
                "#endif\n"
            )
        if not check_gates([probe]):
            print("  self-test unsupported PASS  (explicit UNSUPPORTED accepted)")
        else:
            print("  self-test unsupported FAIL  (UNSUPPORTED branch wrongly flagged)")
            ok = False

        # Regression guard for a false positive this script actually had: when
        # the x86 branch is an #elif, the arm64 case is usually an EARLIER
        # branch of the same chain (crash.c:388 dispatches apple/arm64,
        # apple/x86, linux/arm64, linux/x86).  Judging from the x86 branch
        # forward flagged all of those as unhandled -- five false positives
        # against one real finding, which is the ratio that gets a check
        # switched off.
        with open(probe, "w", encoding="utf-8") as handle:
            handle.write(
                "#if defined(_WIN32)\n"
                "#if defined(__APPLE__) && defined(__aarch64__)\n"
                "    a();\n"
                "#elif defined(__linux__) && defined(__aarch64__)\n"
                "    b();\n"
                "#elif defined(__linux__) && defined(__x86_64__)\n"
                "    c();\n"
                "#else\n"
                "    d();\n"
                "#endif\n"
                "#endif\n"
            )
        if not check_gates([probe]):
            print("  self-test elif-chain PASS  (earlier arm64 branch honored)")
        else:
            print("  self-test elif-chain FAIL  (false positive on #elif chain)")
            ok = False

    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true", help="verify the checks can fail, then exit")
    ap.add_argument("--jobs", type=int, default=min(8, (os.cpu_count() or 4)))
    ap.add_argument("--syntax-only", action="store_true", help="skip the arch-gate scan")
    ap.add_argument("--gates-only", action="store_true", help="skip the syntax check (no clang needed)")
    args = ap.parse_args()

    if args.gates_only:
        headers = None
    else:
        headers = find_headers()
        print(f"mingw-w64 headers: {headers}")

    if args.self_test:
        sys.exit(0 if self_test(headers) else 1)

    paths = windows_sources()
    print(f"Windows-gated sources: {len(paths)}")
    failed = False

    if not args.gates_only:
        print("\n== differential syntax check (aarch64 vs x86_64) ==")
        results, failures = check_syntax(paths, headers, args.jobs)
        judged = sum(1 for r in results if not r.get("timeout") and r["x86"] == 0)
        unjudged = sum(1 for r in results if not r.get("timeout") and r["x86"] > 0)
        # Say the coverage limit out loud every run. This check can only judge
        # files that syntax-check standalone; the rest are carried by the
        # arch-gate scan below, which needs no compiler. Printing the split
        # keeps "green" from reading as "all 84 files verified".
        print(f"  judged (x86_64 clean, so a delta is meaningful): {judged}")
        print(f"  NOT judged (do not syntax-check standalone):     {unjudged}"
              f"  <- covered only by the arch-gate scan")
        for f in failures:
            failed = True
            if f.get("timeout"):
                print(f"  TIMEOUT  {os.path.relpath(f['path'], REPO)}")
            else:
                rel = os.path.relpath(f["path"], REPO)
                print(f"  ARM64-ONLY FAILURE  {rel}")
                print(f"      aarch64={f['arm']} errors, x86_64={f['x86']} -- {f['arm'] - f['x86']} arch-specific")
        if not failures:
            print("  no arm64-only compile failures")

    if not args.syntax_only:
        print("\n== arch-gate scan (silent arm64 degradation) ==")
        findings = check_gates(paths)
        for f in findings:
            failed = True
            print(f"  SILENT ARM64 FALLTHROUGH  {f['path']}:{f['line']}")
            print(f"      {f['text']}")
            print(f"      else at line {f['else_line']} is not #error and never mentions arm64")
        if not findings:
            print("  no silent arm64 fallthroughs")

    if failed:
        print(
            "\nFAILED. Gate the x86 body on `#if defined(_M_X64) || defined(__x86_64__)`\n"
            "and give arm64 either a real implementation or a loud failure (#error, or\n"
            "an explicit UNSUPPORTED return). src/core/stw.c:271 is the reference shape."
        )
        return 1
    print("\nOK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
