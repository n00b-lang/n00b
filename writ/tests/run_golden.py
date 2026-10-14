import os
import subprocess
import sys

expected_path = sys.argv[1]
separator = sys.argv.index("--")
command = sys.argv[separator + 1 :]
command[0] = os.path.abspath(command[0])
expect_failure = "--fails" in sys.argv[2:separator]
stdout_only = "--stdout" in sys.argv[2:separator]

source_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
result = subprocess.run(command, cwd=source_root, capture_output=True, text=True)
actual = result.stdout if stdout_only else result.stdout + result.stderr

if expect_failure != (result.returncode != 0):
    sys.exit(f"exit status {result.returncode} was not expected\n{actual}")

if os.environ.get("WRIT_UPDATE_GOLDEN"):
    with open(expected_path, "w", encoding="utf-8") as out:
        out.write(actual)
    sys.exit(0)

with open(expected_path, encoding="utf-8") as expected_file:
    expected = expected_file.read()

if actual != expected:
    import difflib

    diff = difflib.unified_diff(expected.splitlines(), actual.splitlines(), "expected", "actual", lineterm="")
    sys.exit("\n".join(diff))
