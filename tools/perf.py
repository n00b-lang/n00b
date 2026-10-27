import argparse
import json
import os
import shutil
import statistics
import subprocess
import sys
import time

FRAME_SECTIONS = ("__n00b_frames", ".data.rel.ro.n00b_frames")
FRAME_SYMBOL = "__n00bb_gc_native_frames"
STACKMAP_SECTIONS = ("__llvm_stackmaps", ".llvm_stackmaps")
TEXT_SECTIONS = ("__text", ".text")
RUN_METRICS = ("wall_s", "cpu_s", "rss_mb")
COMPILE_METRICS = ("cold_compile_s", "cold_compile_cpu_s", "cold_compile_rss_mb", "warm_compile_s")
SIZE_METRICS = ("binary_kb", "text_kb", "frames_kb", "stackmaps_kb")


def fail(message):
    sys.exit(f"perf: {message}")


def config_value(build, key):
    try:
        with open(os.path.join(build, "config.mk")) as config:
            for line in config:
                name, _, value = line.partition(":=")
                if name.strip() == key:
                    return value.strip()
    except OSError:
        pass
    return ""


def measure(command, env, cwd=None):
    started = time.perf_counter()
    process = subprocess.Popen(command, env=env, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    output = process.stdout.read()
    _, status, usage = os.wait4(process.pid, 0)
    process.stdout.close()
    wall = time.perf_counter() - started
    code = os.waitstatus_to_exitcode(status)
    if code:
        fail(f"{' '.join(command)} exited {code}\n{output.decode(errors='replace')}")
    rss = usage.ru_maxrss / (1 << 20) if sys.platform == "darwin" else usage.ru_maxrss / 1024
    return {"wall_s": wall, "cpu_s": usage.ru_utime + usage.ru_stime, "rss_mb": rss}


def section_sizes(path, size_tool):
    sizes = {"binary_kb": os.path.getsize(path) / 1024}
    if not size_tool:
        return sizes
    listing = subprocess.run([size_tool, "-A", path], capture_output=True, text=True)
    sections = {}
    for line in listing.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[1].isdigit():
            sections[fields[0]] = sections.get(fields[0], 0) + int(fields[1])
    for metric, names in (("text_kb", TEXT_SECTIONS), ("frames_kb", FRAME_SECTIONS),
                          ("stackmaps_kb", STACKMAP_SECTIONS)):
        sizes[metric] = sum(sections.get(name, 0) for name in names) / 1024
    if not sizes["frames_kb"]:
        sizes["frames_kb"] = symbol_size(path, size_tool, FRAME_SYMBOL) / 1024
    return sizes


def symbol_size(path, size_tool, symbol):
    nm = os.path.join(os.path.dirname(size_tool), "llvm-nm")
    if not os.access(nm, os.X_OK):
        return 0
    listing = subprocess.run([nm, "-S", "--defined-only", path], capture_output=True, text=True)
    for line in listing.stdout.splitlines():
        fields = line.split()
        if len(fields) == 4 and fields[3] == symbol:
            return int(fields[1], 16)
    return 0


def baseline_compiler(source, build, baseline):
    if os.path.isfile(baseline) and os.access(baseline, os.X_OK):
        return os.path.abspath(baseline)
    root = os.path.join(build, "perf-baseline")
    workspace = os.path.join(root, "src")
    if not shutil.which("jj"):
        fail("a revision baseline needs jj; pass a compiler path or a results file instead")
    resolved = subprocess.run(["jj", "-R", source, "log", "--no-graph", "-r", baseline, "-T", "commit_id"],
                              capture_output=True, text=True)
    if resolved.returncode or not resolved.stdout.strip():
        fail(f"cannot resolve baseline revision '{baseline}'\n{resolved.stderr}")
    commit = resolved.stdout.strip()
    if not os.path.isdir(workspace):
        os.makedirs(root, exist_ok=True)
        subprocess.run(["jj", "-R", source, "workspace", "add", "--name", "perf-baseline", "-r", commit,
                        workspace], check=True, capture_output=True)
    else:
        subprocess.run(["jj", "-R", workspace, "new", commit], check=True, capture_output=True)
    environment = dict(os.environ, BUILD_DIR=os.path.join(root, "build"))
    for key in ("BUILD_TYPE", "LLVM_CONFIG", "N00B_RUST"):
        value = config_value(build, key)
        if value:
            environment[key] = value
    print(f"perf: building baseline {commit[:12]}", flush=True)
    built = subprocess.run(["sh", os.path.join(workspace, "build.sh"), "compile"], env=environment,
                           capture_output=True, text=True)
    if built.returncode:
        fail(f"baseline build failed\n{built.stdout[-4000:]}{built.stderr[-4000:]}")
    return os.path.join(root, "build", "n00b")


def compile_benchmark(compiler, program, output, cache, env):
    command = [compiler, "build", "--output", output, os.path.basename(program)]
    shutil.rmtree(cache, ignore_errors=True)
    environment = dict(env, N00B_CACHE=cache)
    directory = os.path.dirname(program)
    cold = measure(command, environment, directory)
    warm = measure(command, environment, directory)
    return {"cold_compile_s": cold["wall_s"], "cold_compile_cpu_s": cold["cpu_s"],
            "cold_compile_rss_mb": cold["rss_mb"], "warm_compile_s": warm["wall_s"]}


def benchmarks(source, names):
    manifest = os.path.join(source, "tests", "perf", "perf.tests")
    found = []
    with open(manifest) as rows:
        for line in rows:
            if line.startswith("#") or not line.strip():
                continue
            name, program = line.rstrip("\n").split("\t")[:2]
            found.append((name, os.path.join(source, "tests", "perf", program)))
    known = {name for name, _ in found}
    unknown = [name for name in names if name not in known]
    if unknown:
        fail(f"no benchmark named {', '.join(unknown)}; names are the first column of {manifest}")
    return [entry for entry in found if not names or entry[0] in names]


def collect(compilers, selected, results, runs, size_tool):
    environment = {key: value for key, value in os.environ.items() if key != "N00B_CACHE"}
    measured = {label: {} for label in compilers}
    for name, program in selected:
        print(f"perf: {name}", flush=True)
        binaries = {}
        for label, compiler in compilers.items():
            binary = os.path.join(results, label, name)
            os.makedirs(os.path.dirname(binary), exist_ok=True)
            entry = compile_benchmark(compiler, program, binary,
                                      os.path.join(results, label, "cache-" + name), environment)
            entry.update(section_sizes(binary, size_tool))
            entry["runs"] = []
            measured[label][name] = entry
            binaries[label] = binary
        for _ in range(runs):
            for label, binary in binaries.items():
                measured[label][name]["runs"].append(measure([binary], environment))
    return measured


def summarize(entry):
    summary = {key: value for key, value in entry.items() if key != "runs"}
    for metric in RUN_METRICS:
        values = [run[metric] for run in entry["runs"]]
        summary[metric] = statistics.median(values)
        summary[metric + "_min"] = min(values)
        summary[metric + "_max"] = max(values)
    return summary


def compare(baseline, current, threshold):
    regressions = []
    rows = []
    for name in current:
        if name not in baseline:
            continue
        before, after = baseline[name], current[name]
        for metric in RUN_METRICS + COMPILE_METRICS + SIZE_METRICS:
            if metric not in before or metric not in after:
                continue
            old, new = before[metric], after[metric]
            change = (new - old) / old * 100 if old else 0.0
            noisy = metric in RUN_METRICS and after[metric + "_min"] <= before[metric + "_max"]
            exact = metric in SIZE_METRICS
            flagged = change > threshold and (exact or not noisy) and metric not in COMPILE_METRICS
            rows.append((name, metric, old, new, change, flagged))
            if flagged:
                regressions.append((name, metric, change))
    return rows, regressions


def report(rows):
    print(f"\n{'benchmark':16} {'metric':20} {'baseline':>12} {'current':>12} {'change':>9}")
    for name, metric, old, new, change, flagged in rows:
        mark = "  REGRESSION" if flagged else ""
        print(f"{name:16} {metric:20} {old:12.3f} {new:12.3f} {change:+8.1f}%{mark}")


def report_single(current):
    print(f"\n{'benchmark':16} {'metric':20} {'value':>12}")
    for name, entry in current.items():
        for metric in RUN_METRICS + COMPILE_METRICS + SIZE_METRICS:
            if metric in entry:
                print(f"{name:16} {metric:20} {entry[metric]:12.3f}")


def main():
    parser = argparse.ArgumentParser(prog="build.sh perf")
    parser.add_argument("--source", required=True)
    parser.add_argument("--build", required=True)
    parser.add_argument("--baseline", help="jj revision, compiler path, or results JSON from an earlier run")
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--threshold", type=float, default=3.0)
    parser.add_argument("names", nargs="*")
    options = parser.parse_args()
    source, build = os.path.abspath(options.source), os.path.abspath(options.build)
    selected = benchmarks(source, options.names)
    results = os.path.join(build, "perf-results")
    shutil.rmtree(results, ignore_errors=True)
    os.makedirs(results)
    bindir = config_value(build, "LLVM_BINDIR")
    size_tool = shutil.which("llvm-size", path=bindir) if bindir else shutil.which("llvm-size")
    compilers = {"current": os.path.join(build, "n00b")}
    saved = None
    if options.baseline and options.baseline.endswith(".json"):
        with open(options.baseline) as stored:
            saved = json.load(stored)
    elif options.baseline:
        compilers = {"baseline": baseline_compiler(source, build, options.baseline), **compilers}
    measured = collect(compilers, selected, results, options.runs, size_tool)
    summaries = {label: {name: summarize(entry) for name, entry in entries.items()}
                 for label, entries in measured.items()}
    for label, summary in summaries.items():
        with open(os.path.join(results, label + ".json"), "w") as out:
            json.dump(summary, out, indent=2)
    baseline = saved if saved is not None else summaries.get("baseline")
    if baseline is None:
        report_single(summaries["current"])
        print(f"\nperf: results in {results}")
        return
    rows, regressions = compare(baseline, summaries["current"], options.threshold)
    report(rows)
    print(f"\nperf: results in {results}")
    if regressions:
        print(f"perf: {len(regressions)} regression(s) over {options.threshold}%")
        sys.exit(1)


main()
