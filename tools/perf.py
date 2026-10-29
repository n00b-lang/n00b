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


def compile_benchmark(compiler, program, output, cache, env, flags=()):
    command = [compiler, "build", *flags, "--output", output, os.path.basename(program)]
    shutil.rmtree(cache, ignore_errors=True)
    environment = dict(env, N00B_CACHE=cache)
    directory = os.path.dirname(program)
    cold = measure(command, environment, directory)
    warm = measure(command, environment, directory)
    return {"cold_compile_s": cold["wall_s"], "cold_compile_cpu_s": cold["cpu_s"],
            "cold_compile_rss_mb": cold["rss_mb"], "warm_compile_s": warm["wall_s"]}


def benchmarks(directory, manifest, names):
    manifest = os.path.join(directory, manifest)
    found = []
    with open(manifest) as rows:
        for line in rows:
            if line.startswith("#") or not line.strip():
                continue
            name, program = line.rstrip("\n").split("\t")[:2]
            found.append((name, os.path.join(directory, program)))
    known = {name for name, _ in found}
    unknown = [name for name in names if name not in known]
    if unknown:
        fail(f"no benchmark named {', '.join(unknown)}; names are the first column of {manifest}")
    return [entry for entry in found if not names or entry[0] in names]


def n00b_builder(compiler, size_tool, flags=(), shared_cache=None):
    def build(name, program, directory, env):
        binary = os.path.join(directory, name)
        if shared_cache:
            command = [compiler, "build", *flags, "--output", binary, os.path.basename(program)]
            measure(command, dict(env, N00B_CACHE=shared_cache), os.path.dirname(program))
            entry = {}
        else:
            entry = compile_benchmark(compiler, program, binary,
                                      os.path.join(directory, "cache-" + name), env, flags)
        entry.update(section_sizes(binary, size_tool))
        return entry, [binary]
    return build


def native_builder(compile_command):
    def build(name, program, directory, env):
        binary = os.path.join(directory, name)
        built = subprocess.run(compile_command(os.path.splitext(program)[0], binary), env=env,
                               capture_output=True, text=True)
        if built.returncode:
            fail(f"cannot build {name}\n{built.stdout[-4000:]}{built.stderr[-4000:]}")
        return {}, [binary]
    return build


def interpreted_builder(interpreter, extension):
    def build(name, program, directory, env):
        return {}, [interpreter, os.path.splitext(program)[0] + extension]
    return build


def language_builders(source, build, skip):
    include = os.path.join(source, "lantern", "include")
    library = os.path.join(build, "lantern", "libir.a")
    cc = config_value(build, "CC") or shutil.which("cc")
    go = shutil.which("go")
    python = shutil.which("python3")
    def c_builder(*optimize):
        return cc and native_builder(lambda stem, binary: [cc, "-std=c2x", *optimize, "-I",
                                                           include, stem + ".c", library, "-o",
                                                           binary])
    available = {
        "c-Ofast": c_builder("-O3", "-ffast-math"),
        "c-O0": c_builder("-O0"),
        "go": go and native_builder(lambda stem, binary: [go, "build", "-o", binary, stem + ".go"]),
        "python": python and interpreted_builder(python, ".py"),
    }
    builders = {}
    for language, builder in available.items():
        if language.split("-")[0] in skip:
            continue
        if not builder:
            print(f"perf: skipping {language}, no toolchain found", flush=True)
            continue
        builders[language] = builder
    return builders


def collect(builders, selected, results, runs):
    environment = {key: value for key, value in os.environ.items() if key != "N00B_CACHE"}
    measured = {label: {} for label in builders}
    for name, program in selected:
        print(f"perf: {name}", flush=True)
        commands = {}
        for label, builder in builders.items():
            directory = os.path.join(results, label)
            os.makedirs(directory, exist_ok=True)
            entry, commands[label] = builder(name, program, directory, environment)
            entry["runs"] = []
            measured[label][name] = entry
        for _ in range(runs):
            for label, command in commands.items():
                measured[label][name]["runs"].append(measure(command, environment))
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


LANGUAGE_ROWS = (
    ("current", "n00b -Ofast", "c-Ofast"),
    ("n00b-O0", "n00b -O0", "c-O0"),
    ("c-Ofast", "c -Ofast", "c-Ofast"),
    ("c-O0", "c -O0", "c-O0"),
    ("go", "go", "c-Ofast"),
    ("python", "python", "c-Ofast"),
)


def report_languages(summaries):
    print(f"\n{'benchmark':14} {'build':12} {'wall_s':>9} {'cpu_s':>9} {'rss_mb':>9} "
          f"{'vs c':>8} {'vs go':>8}")
    for name in summaries["current"]:
        for label, title, c_label in LANGUAGE_ROWS:
            if label not in summaries:
                continue
            entry = summaries[label][name]
            ratios = [f"{entry['wall_s'] / summaries[other][name]['wall_s']:7.2f}x"
                      if other in summaries and summaries[other][name]["wall_s"] else f"{'-':>8}"
                      for other in (c_label, "go")]
            print(f"{name:14} {title:12} {entry['wall_s']:9.3f} {entry['cpu_s']:9.3f} "
                  f"{entry['rss_mb']:9.1f} {ratios[0]} {ratios[1]}")
        print()


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
    parser.add_argument("--runs", type=int)
    parser.add_argument("--threshold", type=float)
    parser.add_argument("--languages", action="store_true",
                        help="run the cross-language suite in tests/perf/xlang against C, Go and Python")
    parser.add_argument("--skip", action="append", default=[], choices=("c", "go", "python"),
                        help="leave a language out of --languages")
    parser.add_argument("names", nargs="*")
    options = parser.parse_args()
    source, build = os.path.abspath(options.source), os.path.abspath(options.build)
    if options.languages:
        selected = benchmarks(os.path.join(source, "tests", "perf", "xlang"), "xlang.tests", options.names)
        results = os.path.join(build, "perf-xlang-results")
        flags, runs = ("--release",), options.runs or 3
        threshold = options.threshold or 5.0
    else:
        selected = benchmarks(os.path.join(source, "tests", "perf"), "perf.tests", options.names)
        results = os.path.join(build, "perf-results")
        flags, runs = (), options.runs or 7
        threshold = options.threshold or 3.0
    saved = None
    if options.baseline and options.baseline.endswith(".json"):
        with open(options.baseline) as stored:
            saved = json.load(stored)
    shutil.rmtree(results, ignore_errors=True)
    os.makedirs(results)
    bindir = config_value(build, "LLVM_BINDIR")
    size_tool = shutil.which("llvm-size", path=bindir) if bindir else shutil.which("llvm-size")
    def cache(label):
        return os.path.join(build, "perf-xlang-cache", label) if options.languages else None
    builders = {"current": n00b_builder(os.path.join(build, "n00b"), size_tool, flags,
                                        cache("current"))}
    if options.baseline and saved is None:
        builders = {"baseline": n00b_builder(baseline_compiler(source, build, options.baseline),
                                             size_tool, flags, cache("baseline")), **builders}
    if options.languages:
        builders["n00b-O0"] = n00b_builder(os.path.join(build, "n00b"), size_tool,
                                           (*flags, "--opt-level", "0"), cache("n00b-O0"))
        builders.update(language_builders(source, build, options.skip))
    measured = collect(builders, selected, results, runs)
    summaries = {label: {name: summarize(entry) for name, entry in entries.items()}
                 for label, entries in measured.items()}
    for label, summary in summaries.items():
        with open(os.path.join(results, label + ".json"), "w") as out:
            json.dump(summary, out, indent=2)
    if options.languages:
        report_languages(summaries)
    baseline = saved if saved is not None else summaries.get("baseline")
    if baseline is None:
        if not options.languages:
            report_single(summaries["current"])
        print(f"\nperf: results in {results}")
        return
    rows, regressions = compare(baseline, summaries["current"], threshold)
    report(rows)
    print(f"\nperf: results in {results}")
    if regressions:
        print(f"perf: {len(regressions)} regression(s) over {threshold}%")
        sys.exit(1)


main()
