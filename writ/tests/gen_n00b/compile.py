import os
import subprocess
import sys
import tempfile

writ, compiler, triple, root = sys.argv[1:5]
writ = os.path.abspath(writ)
cases = sys.argv[5:]
failed = False
with tempfile.TemporaryDirectory() as scratch:
    for case in cases:
        out = os.path.join(scratch, case)
        os.makedirs(out)
        generated = subprocess.run(
            [writ, "--platform", triple, "--feature", "experimental_capture_groups",
             "--contract-dir", os.path.join(root, "contracts"), "gen", "n00b", "--embedded",
             os.path.join(root, "tests", "gen_n00b", case + ".writ"), "--profile",
             os.path.join(root, "profiles", "n00b.writ"), "-o", out],
            capture_output=True, text=True, cwd=root)
        if generated.returncode:
            print(f"{case}: generation failed\n{generated.stderr}")
            failed = True
            continue
        for name in sorted(os.listdir(out)):
            if not name.endswith(".c"):
                continue
            built = subprocess.run(
                [compiler, "--target=" + triple, "-std=c11", "-Wall", "-Wextra", "-Werror", "-c", "-emit-llvm",
                 "-O2", "-I", out, os.path.join(out, name), "-o", os.path.join(out, name + ".bc")],
                capture_output=True, text=True)
            if built.returncode:
                print(f"{case}/{name}: does not compile\n{built.stderr}")
                failed = True
            else:
                print(f"{case}/{name}: bitcode")
sys.exit(1 if failed else 0)
