import argparse
import json
import os
import shlex
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROGRAM_SCRIPT = "builtin_os_n00b.sh"


def program_tests(names):
    tests = []
    with open(os.path.join(ROOT, "tests", "n00b.tests")) as listing:
        for line in listing:
            if line.startswith("#") or not line.strip():
                continue
            fields = line.rstrip("\n").split("\t")
            if len(fields) < 4 or (names and fields[0] not in names):
                continue
            words = shlex.split(fields[3].replace("$SRC", ROOT))
            environment = {}
            while words and "=" in words[0] and not words[0].startswith("/"):
                key, _, value = words.pop(0).partition("=")
                environment[key] = value
            if (len(words) == 4 and words[0] == "sh"
                    and os.path.basename(words[1]) == PROGRAM_SCRIPT):
                tests.append({"name": fields[0], "program": words[3],
                              "environment": environment})
    return tests


def build(options):
    os.makedirs(options.out, exist_ok=True)
    built, failed = [], []
    for test in program_tests(options.names):
        output = os.path.join(options.out, test["name"] + ".exe")
        result = subprocess.run(
            [options.compiler, "build", "--target", options.target,
             "--output", output, test["program"]],
            cwd=ROOT, env={**os.environ, **test["environment"]},
            capture_output=True, text=True)
        if result.returncode == 0:
            built.append(test["name"])
            print(f"built   {test['name']}")
        else:
            failed.append(test["name"])
            print(f"FAILED  {test['name']}\n{result.stdout}{result.stderr}")
    with open(os.path.join(options.out, "tests.json"), "w") as manifest:
        json.dump(built, manifest, indent=1)
    print(f"\n{len(built)} built, {len(failed)} failed")
    return 1 if failed else 0


def run(options):
    with open(os.path.join(options.dir, "tests.json")) as manifest:
        names = json.load(manifest)
    failed = []
    for name in names:
        executable = os.path.join(options.dir, name + ".exe")
        try:
            result = subprocess.run([executable], capture_output=True,
                                    timeout=options.timeout)
        except subprocess.TimeoutExpired:
            failed.append(name)
            print(f"FAIL    {name} (timed out after {options.timeout}s)")
            continue
        stderr = result.stderr.decode(errors="replace").rstrip("\r\n")
        if result.returncode == 0 and not result.stdout and stderr == "ok":
            print(f"ok      {name}")
        else:
            failed.append(name)
            print(f"FAIL    {name} (exit {result.returncode})\n"
                  f"{result.stdout.decode(errors='replace')}{stderr}")
    print(f"\n{len(names) - len(failed)} passed, {len(failed)} failed")
    return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser(
        description="cross-build program tests for another target, "
                    "then run them where that target executes")
    commands = parser.add_subparsers(dest="command", required=True)
    build_parser = commands.add_parser("build")
    build_parser.add_argument("--compiler", required=True)
    build_parser.add_argument("--target", required=True)
    build_parser.add_argument("--out", required=True)
    build_parser.add_argument("names", nargs="*")
    run_parser = commands.add_parser("run")
    run_parser.add_argument("--dir", required=True)
    run_parser.add_argument("--timeout", type=int, default=600)
    options = parser.parse_args()
    sys.exit(build(options) if options.command == "build" else run(options))


main()
