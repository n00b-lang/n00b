import pathlib
import re
import subprocess
import sys

import comments

SLACK = 1.5
UNBUDGETED = 30
GOLDENS = 10
GOLDEN_SUFFIXES = (".expected", ".out", ".fixed", ".plan", ".golden")
SOURCE_SUFFIXES = (".writ", ".rs", ".go", ".js", ".ts", ".rb", ".swift", ".kt", ".java", ".cs", ".php", ".lua")
BUDGET = re.compile(r"^Budget:[ \t]*(-?\d+)[ \t]*$", re.M)
BUDGET_LINE = re.compile(r"^Budget:.*$", re.M)
GOLDENS_TRAILER = re.compile(r"^Goldens:[ \t]*\S", re.M)


def run(root, command):
    return subprocess.run(command, cwd=root, capture_output=True, text=True, check=True).stdout


def is_test(path):
    parts = path.split("/")
    return "tests" in parts[:-1] or "test" in parts[:-1] or parts[-1].startswith("test_")


def is_golden(path):
    return is_test(path) and (path.endswith(GOLDEN_SUFFIXES) or "/golden/" in path)


def is_source(path):
    return not is_test(path) and (comments.language(path) is not None or path.endswith(SOURCE_SUFFIXES))


def diff_stats(diff):
    net = 0
    goldens = set()
    path = None
    in_hunk = False
    for line in diff.splitlines():
        if line.startswith("diff --git "):
            path = line.split(" b/", 1)[-1]
            in_hunk = False
            if is_golden(path):
                goldens.add(path)
        elif line.startswith("@@"):
            in_hunk = True
        elif in_hunk and path and is_source(path) and line[1:].strip():
            if line.startswith("+"):
                net += 1
            elif line.startswith("-"):
                net -= 1
    return net, goldens


def jj_changes(root, revset):
    template = '"\\x1d" ++ change_id.short(8) ++ "\\x1f" ++ description ++ "\\x1e"'
    out = run(root, ["jj", "--no-pager", "--color=never", "log", "--no-graph", "-r", revset, "-T", template,
                     "--git", "--context=0"])
    for record in out.split("\x1d")[1:]:
        header, diff = record.split("\x1e", 1)
        name, description = header.split("\x1f", 1)
        yield name, description, diff


def git_changes(root, revset):
    for commit in run(root, ["git", "rev-list", "--no-merges", "--reverse", revset]).split():
        description = run(root, ["git", "log", "-1", "--format=%B", commit])
        diff = run(root, ["git", "show", "--format=", "--no-color", "--no-ext-diff", "--unified=0", commit])
        yield commit[:12], description, diff


def check(name, description, diff):
    net, goldens = diff_stats(diff)
    m = BUDGET.search(description)
    budget = int(m.group(1)) if m else None
    title = description.strip().splitlines()[0] if description.strip() else "(no description)"
    problems = []
    if budget is None and BUDGET_LINE.search(description):
        problems.append(
            f"has a malformed '{BUDGET_LINE.search(description).group(0)}' trailer; write 'Budget: N' "
            "with N a whole number of net source lines, negative when the change should remove lines.")
    elif budget is None and net > UNBUDGETED:
        problems.append(
            f"adds {net} net source lines with no 'Budget: N' trailer. Before writing a change over "
            f"{UNBUDGETED} lines, decide how many net source lines it should need and put 'Budget: N' "
            "on its own line in the description. If the change grew past what the problem needs, cut it "
            "back: extend the function that makes the decision instead of its callers, and drop "
            "stepping-stone code.")
    elif budget is not None and net > budget + (SLACK - 1) * abs(budget):
        problems.append(
            f"adds {net} net source lines, over its budget of {budget} with {SLACK} x slack. Shrink the change to "
            "the budget: reuse the existing central path rather than a parallel one, fix the root cause "
            "once, and remove scope the task did not ask for. Raise the budget only if the original "
            "estimate was wrong, and say why in the description.")
    if len(goldens) > GOLDENS and not GOLDENS_TRAILER.search(description):
        problems.append(
            f"edits {len(goldens)} golden files with no 'Goldens: <reason>' trailer. If a default "
            "changed, pass the old setting as a flag in the test harness so existing goldens still "
            "hold; if the output really changed everywhere, add a 'Goldens:' line explaining why.")
    status = "FAIL" if problems else "ok"
    shown = budget if budget is not None else "-"
    print(f"{status:4} {name} net={net} budget={shown} goldens={len(goldens)} {title}")
    for problem in problems:
        print(f"     {problem}")
    return not problems


def main(argv):
    args = [arg for arg in argv if not arg.startswith("--")]
    root = pathlib.Path(args[0] if args else ".")
    vcs = "jj" if (root / ".jj").is_dir() else "git"
    for arg in argv:
        if arg.startswith("--vcs="):
            vcs = arg.split("=", 1)[1]
    revset = args[1] if len(args) > 1 else ("main..@" if vcs == "jj" else "origin/main..HEAD")
    changes = jj_changes(root, revset) if vcs == "jj" else git_changes(root, revset)
    results = [check(name, description, diff) for name, description, diff in changes
               if diff.strip() or description.strip()]
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
