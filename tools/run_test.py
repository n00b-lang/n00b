#!/usr/bin/env python3
import os
import signal
import subprocess
import sys
import time

WINDOWS = sys.platform in ("win32", "cygwin", "msys")


def write(path, text, mode="w"):
    try:
        with open(path, mode) as out:
            out.write(text)
    except OSError:
        pass


def main():
    manifest, suite, src, build, name = sys.argv[1:6]
    results = os.path.join(os.environ["TEST_RESULTS"], suite)
    os.makedirs(results, exist_ok=True)
    log = os.path.join(results, name + ".log")
    status_file = os.path.join(results, name + ".status")

    line = None
    with open(manifest) as entries:
        for candidate in entries:
            fields = candidate.rstrip("\n").split("\t")
            if fields[0] == name:
                line = fields
                break
    if line is None:
        print(f"FAIL    {suite}/{name} (not in {manifest})")
        write(status_file, "FAIL\n")
        return
    tags = set(line[1].split(","))
    limit = float(line[2])
    command = "\t".join(line[3:])

    def skip(reason):
        write(log, reason + "\n")
        write(status_file, "SKIP\n")
        print(f"skip    {suite}/{name} ({reason})")
        sys.exit(0)

    if "rust" in tags and os.environ.get("N00B_RUST", "0") != "1":
        skip("Rust is off; build with N00B_RUST=1")
    if "darwin" in tags and sys.platform != "darwin":
        skip("macOS only")
    if "asan" in tags and os.environ.get("N00B_EMULATED", "0") == "1":
        skip("AddressSanitizer cannot run under CPU emulation")
    if "asan" in tags and WINDOWS:
        skip("AddressSanitizer is not available for the Windows toolchain")
    if "unix" in tags and WINDOWS:
        skip("needs a Unix-only toolchain feature (relocatable links for symbol isolation)")

    os.makedirs(build, exist_ok=True)
    budget = limit * float(os.environ.get("N00B_TEST_BUDGET_SCALE", "1"))
    environment = dict(os.environ, SRC=src, BUILD=build)
    start = time.monotonic()
    timed_out = False
    with open(log, "w") as output:
        child = subprocess.Popen(["bash", "-c", command], cwd=build,
                                 env=environment, stdout=output,
                                 stderr=subprocess.STDOUT,
                                 start_new_session=True)
        try:
            child.wait(timeout=int(budget * 8 + 10))
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(child.pid, signal.SIGTERM)
            child.wait()
    usage = os.times()
    cpu = usage.children_user + usage.children_system
    elapsed = time.monotonic() - start
    code = child.returncode
    if code < 0:
        code = 128 - code
    write(os.path.join(results, name + ".cpu"), f"{cpu:.2f}\n")
    write(os.path.join(results, name + ".time"), f"{elapsed:.2f}\n")
    over = round(cpu, 2) > round(budget, 2)

    if "xfail" in tags and not timed_out:
        code = 1 if code == 0 else 0

    if timed_out:
        write(status_file, "TIMEOUT\n")
        print(f"TIMEOUT {suite}/{name} ({elapsed:.2f}s)")
    elif code == 0 and over:
        with open(log, "rb") as text:
            output = text.read()
        separator = "" if not output or output.endswith(b"\n") else "\n"
        write(log, f"{separator}used {cpu:.2f} CPU seconds, over its "
                   f"{budget:.2f}s budget in {manifest}\n", "a")
        write(status_file, "OVER\n")
        print(f"OVER    {suite}/{name} ({cpu:.2f} CPU s, budget {budget:.2f}; "
              f"{elapsed:.2f}s wall)")
    elif code == 0:
        write(status_file, "OK\n")
        print(f"ok      {suite}/{name} ({cpu:.2f} CPU s, {elapsed:.2f}s wall)")
    elif code == 77:
        with open(log, errors="replace") as text:
            lines = text.read().splitlines()
        write(status_file, "SKIP\n")
        print(f"skip    {suite}/{name} ({lines[-1] if lines else ''})")
    else:
        write(status_file, "FAIL\n")
        print(f"FAIL    {suite}/{name} (exit {code}, {elapsed:.2f}s)")


main()
