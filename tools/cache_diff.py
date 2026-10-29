import argparse
import concurrent.futures
import difflib
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

SOURCE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROGRAMS = os.path.join(SOURCE, "tests", "programs")
DIAGNOSTICS = os.path.join(SOURCE, "tests", "diagnostics")

RUN_CASES = [
    ("features/fast_suite.n", ""),
    ("bitcode/bitcode_lang_suite.n", ""),
    ("bitcode/bitcode_gc_units.n", "bitcode"),
    ("text/text_suite.n", ""),
    ("merged/arrays.n", ""),
    ("merged/records.n", ""),
    ("merged/calls.n", ""),
    ("merged/generics.n", ""),
    ("merged/generics_text.n", ""),
    ("merged/lto.n", ""),
    ("consts_literals/consts_literals.n", "consts_literals"),
    ("static_encoding/static_encoding.n", "static_encoding"),
    ("import_cycles/cycles/cycles.n", "import_cycles/cycles"),
    ("imports_bring/main.n", "imports_bring"),
    ("incremental/main.n", "incremental"),
    ("incremental/generic_values.n", "incremental"),
    ("incremental/generic_fields.n", "incremental"),
    ("link_records/record_root.n", "link_records"),
]


def fail(message):
    sys.exit(f"cache-diff: {message}")


def normalize(text, scratch):
    return text.replace(scratch, "<scratch>")


def environment(cache, module_path):
    env = dict(os.environ)
    env["N00B_CACHE"] = cache
    for key in ("N00BB_NO_OBJECT_CACHE", "N00BB_NO_NATIVE_CACHE",
                "N00BB_NO_PACKAGE_CACHE", "N00BB_CACHE_LOG"):
        env.pop(key, None)
    if module_path:
        env["N00B_PATH"] = module_path
    else:
        env.pop("N00B_PATH", None)
    return env


def invoke(command, env, cwd, scratch):
    result = subprocess.run(command, env=env, cwd=cwd, capture_output=True,
                            timeout=900)
    output = result.stdout.decode(errors="replace") + \
        result.stderr.decode(errors="replace")
    return result.returncode, normalize(output, scratch)


class Case:
    def __init__(self, kind, program, module_path):
        self.kind = kind
        self.program = program
        self.module_path = module_path

    @property
    def name(self):
        return f"{self.kind}:{os.path.relpath(self.program, SOURCE)}"

    def outcome(self, compiler, cache, scratch):
        env = environment(cache, self.module_path)
        cwd = os.path.dirname(self.program)
        if self.kind == "check":
            return {"check": invoke([compiler, "check", self.program], env,
                                    cwd, scratch)}
        binary = os.path.join(scratch, "program")
        if os.path.exists(binary):
            os.remove(binary)
        build = invoke([compiler, "build", "--output", binary, self.program],
                       env, cwd, scratch)
        result = {"build": build}
        if build[0] == 0:
            result["run"] = invoke([binary], env, cwd, scratch)
        return result


def deep_cases():
    cases = []
    for program in sorted(glob.glob(os.path.join(PROGRAMS, "**", "*.n"),
                                    recursive=True)):
        cases.append(Case("check", program, os.path.dirname(program)))
    return cases


def run_cases(compiler, root):
    cases = []
    for program, path in RUN_CASES:
        module_path = os.path.join(PROGRAMS, path) if path else ""
        cases.append(Case("run", os.path.join(PROGRAMS, program),
                          module_path))
    for program in sorted(glob.glob(os.path.join(DIAGNOSTICS, "*", "*.n"))):
        if os.sep + "build" + os.sep in program:
            continue
        name = os.path.basename(program)[:-2]
        modules = os.path.join(DIAGNOSTICS, "modules", name)
        cases.append(Case("check", program,
                          modules if os.path.isdir(modules) else ""))
    return cases


def compare(case, scenario, reference, actual):
    problems = []
    for step in sorted(set(reference) | set(actual)):
        if reference.get(step) != actual.get(step):
            expected = reference.get(step) or (None, "")
            got = actual.get(step) or (None, "")
            diff = "".join(difflib.unified_diff(
                expected[1].splitlines(keepends=True),
                got[1].splitlines(keepends=True),
                f"cold (exit {expected[0]})", f"{scenario} (exit {got[0]})"))
            problems.append(f"{case.name}: {scenario} {step} differs\n{diff}")
    return problems


EXTRA_FUNCTION = "\nfunc cache_diff_extra() -> int {\n    return 7\n}\n"
GLOBAL_INTEGER = re.compile(
    r"^((?:var|let|const) +\w+(?: *: *\w+)? *= *)(\d+)[ \t]*$", re.M)
RECORD_OPEN = re.compile(r"^(?:record|object) +\w+[^\n{]*\{[ \t]*\n", re.M)


def mutations(text):
    yield "touch", text + "\n"
    yield "add-function", text + EXTRA_FUNCTION
    match = GLOBAL_INTEGER.search(text)
    if match:
        bumped = str(int(match.group(2)) + 1)
        yield "global-value", text[:match.start(2)] + bumped + \
            text[match.end(2):]
    match = RECORD_OPEN.search(text)
    if match:
        yield "record-field", text[:match.end()] + \
            "    cache_diff_pad: int\n" + text[match.end():]


IMPORT_LINE = re.compile(
    r"^\s*(?:import\s+([\w.]+(?:\s*,\s*[\w.]+)*)|bring\b.*\bfrom\s+([\w.]+))",
    re.M)


def imported_files(root, program):
    reached = []
    pending = [program]
    while pending:
        path = pending.pop()
        if path in reached:
            continue
        reached.append(path)
        with open(path) as source:
            text = source.read()
        for match in IMPORT_LINE.finditer(text):
            names = match.group(1) or match.group(2)
            for name in names.split(","):
                name = name.strip().split(" ")[0]
                parts = name.split(".")
                for candidate in (os.path.join(root, *parts) + ".n",
                                  os.path.join(root, *parts, "__init.n"),
                                  os.path.join(root, *parts, "facade.n")):
                    if os.path.isfile(candidate):
                        pending.append(candidate)
                        break
    return [path for path in reached if path != program]


def relocated(case, source_root, copy_root):
    program = os.path.join(copy_root,
                           os.path.relpath(case.program, source_root))
    return Case(case.kind, program, copy_root)


def mutation_problems(compiler, case, keep):
    source_root = case.module_path
    scratch = tempfile.mkdtemp(prefix="cache-diff-edit-")
    problems = []
    edits = 0
    try:
        pristine = os.path.join(scratch, "pristine")
        tree = os.path.join(scratch, "tree")
        primed = os.path.join(scratch, "primed")
        shutil.copytree(source_root, pristine)
        shutil.copytree(pristine, tree)
        copy = relocated(case, source_root, tree)
        copy.outcome(compiler, primed, scratch)
        seed = os.path.join(scratch, "seed")
        seed_program = os.path.join(scratch, "seed_program.n")
        with open(seed_program, "w") as target:
            target.write("var items: List[int] = [1]\nrequire len(items) == 1\n")
        Case("run", seed_program, "").outcome(compiler, seed, scratch)
        dependencies = sorted(imported_files(tree, copy.program))
        for dependency in dependencies:
            with open(dependency) as source:
                original = source.read()
            for kind, edited in mutations(original):
                shutil.rmtree(tree)
                shutil.copytree(pristine, tree)
                warm_cache = os.path.join(scratch, "warm")
                cold_cache = os.path.join(scratch, "cold")
                shutil.rmtree(warm_cache, ignore_errors=True)
                shutil.rmtree(cold_cache, ignore_errors=True)
                shutil.copytree(primed, warm_cache)
                shutil.copytree(seed, cold_cache)
                with open(dependency, "w") as target:
                    target.write(edited)
                warm = copy.outcome(compiler, warm_cache, scratch)
                cold = copy.outcome(compiler, cold_cache, scratch)
                label = f"{kind} {os.path.relpath(dependency, tree)}"
                problems += compare(case, f"incremental after {label}",
                                    cold, warm)
                edits += 1
        return problems, edits
    finally:
        if not keep:
            shutil.rmtree(scratch, ignore_errors=True)


def check_case(compiler, case, primer, keep):
    scratch = tempfile.mkdtemp(prefix="cache-diff-")
    try:
        own = os.path.join(scratch, "own")
        shared = os.path.join(scratch, "shared")
        cold = case.outcome(compiler, own, scratch)
        problems = []
        warm = case.outcome(compiler, own, scratch)
        problems += compare(case, "warm", cold, warm)
        if primer is not None:
            primer.outcome(compiler, shared, scratch)
            crossed = case.outcome(compiler, shared, scratch)
            problems += compare(case, f"after {primer.name}", cold, crossed)
        edits = 0
        if case.kind == "run" and case.module_path:
            edited, edits = mutation_problems(compiler, case, keep)
            problems += edited
        return case, problems, edits
    finally:
        if not keep:
            shutil.rmtree(scratch, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(
        description="Check that cached builds behave exactly like cold builds")
    parser.add_argument("compiler")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--match", default="")
    parser.add_argument("--keep", action="store_true")
    parser.add_argument("--deep", action="store_true",
                        help="also check every program under tests/programs")
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    if not os.access(compiler, os.X_OK):
        fail(f"{compiler} is not executable")
    candidates = run_cases(compiler, SOURCE)
    if args.deep:
        candidates += deep_cases()
    cases = [case for case in candidates if args.match in case.name]
    run_programs = [case for case in cases if case.kind == "run"]
    failures = []
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futures = []
        for index, case in enumerate(cases):
            primer = None
            if case.kind == "run" and len(run_programs) > 1:
                position = run_programs.index(case)
                primer = run_programs[(position + 1) % len(run_programs)]
            futures.append(pool.submit(check_case, compiler, case, primer,
                                       args.keep))
        for future in concurrent.futures.as_completed(futures):
            case, problems, edits = future.result()
            status = "FAIL" if problems else "ok"
            detail = f"  ({edits} dependency edits)" if edits else ""
            print(f"{status:4}  {case.name}{detail}", flush=True)
            failed += 1 if problems else 0
            failures += problems
    for problem in failures:
        print(problem, file=sys.stderr)
    print(f"{len(cases) - failed} of {len(cases)} cases matched")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
