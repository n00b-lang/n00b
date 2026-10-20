import os
import re
import shutil
import subprocess
import sys

writ, source_root, resharp_root, work, compiler, variant = sys.argv[1:7]
feature = "experimental_capture_groups"
features = ["--feature", feature] if variant == "features" else []
cargo_features = ["--features", feature] if variant == "features" else []
contract = os.path.join(source_root, "contracts", "resharp.writ")
crate = os.path.join(work, "crate")
generated = os.path.join(work, "c")
target = os.path.join(os.path.dirname(work), "resharp-target")
environment = dict(os.environ, CARGO_TARGET_DIR=target)
sanitize = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
strict = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=0"]


def run(command, **options):
    result = subprocess.run(command, capture_output=True, text=True, env=environment, **options)
    if result.returncode:
        sys.exit(f"{' '.join(command)} failed\n{result.stdout}{result.stderr}")
    return result.stdout + result.stderr


shutil.rmtree(work, ignore_errors=True)
run([writ, *features, "gen", "rust", contract, "--crate-path", os.path.join(resharp_root, "resharp-engine"), "-o", crate])
shutil.copy(os.path.join(resharp_root, "Cargo.lock"), crate)
os.makedirs(os.path.join(crate, "tests"))
shutil.copy(os.path.join(source_root, "tests", "resharp", "differential.rs"), os.path.join(crate, "tests"))
manifest = ["--manifest-path", os.path.join(crate, "Cargo.toml")]
if variant == "miri":
    if subprocess.run(["cargo", "+nightly", "miri", "--version"], capture_output=True).returncode:
        print("cargo miri is not installed")
        sys.exit(77)
    environment["MIRI_SYSROOT"] = os.path.join(os.path.dirname(work), "resharp-miri-sysroot")
    run(["cargo", "+nightly", "miri", "setup"])
    print(run(["cargo", "+nightly", "miri", "test", "-q", *manifest, "--test", "differential"]), end="")
    sys.exit(0)
run(["cargo", "test", "-q", *manifest, *cargo_features])
run(["cargo", "build", "-q", *manifest, *cargo_features])
noted = run(["cargo", "rustc", "-q", *manifest, *cargo_features, "--lib", "--crate-type", "staticlib", "--",
             "--print", "native-static-libs"])
native = re.search(r"native-static-libs: (.*)", noted).group(1).split()
library = os.path.join(target, "debug", "libresharp_writ")
shared = library + (".dylib" if sys.platform == "darwin" else ".so")

verified = run([writ, *features, "verify", contract, "--binary", shared])
promised = set(re.findall(r"^ok (\S+)$", verified, re.M))
exported = run(["nm", "-g", "--defined-only", shared])
present = {name.lstrip("_") for name in re.findall(r" T (_?writ_resharp_\w+)$", exported, re.M)}
if promised != present:
    sys.exit(f"symbols differ from the derived contract\nonly in the library: {sorted(present - promised)}\n"
             f"only in the contract: {sorted(promised - present)}")

run([writ, *features, "gen", "c", contract, "-o", generated, "--conformance"])
conformance = os.path.join(work, "conformance")
run([compiler, *strict, *sanitize, "-I", generated] + [os.path.join(generated, name) for name in
    ("resharp_writ.c", "resharp_writ_mock.c", "resharp_writ_conformance.c")] + ["-o", conformance])
print(run([conformance]), end="")

roundtrip = os.path.join(work, "roundtrip")
defines = ["-DRESHARP_CAPTURE_GROUPS"] if variant == "features" else []
run([compiler, *strict, *sanitize, *defines, "-I", generated, os.path.join(source_root, "tests", "resharp", "roundtrip.c"),
     os.path.join(generated, "resharp_writ.c"), library + ".a", *native, "-o", roundtrip])
print(run([roundtrip]), end="")
print(f"transport ok: {len(present)} symbols")
