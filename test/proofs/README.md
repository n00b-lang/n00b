# Inspector Proof Artifacts

These scripts exercise findings from the inspector run without registering them
in the normal Meson suite. They are proof-oriented: they exit 0 when the current
suspect behavior is reproduced, and exit nonzero when the finding is not
reproduced.

Run them from the repository root:

```sh
python3 test/proofs/prove_collated_006_large_literals.py --n00b build_debug/n00b
python3 test/proofs/prove_collated_009_kwarg_rescan.py
python3 test/proofs/prove_collated_010_codegen_error_exec.py --n00b build_debug/n00b
```

Or run the wrapper:

```sh
bash test/proofs/run_collated_proofs.sh build_debug/n00b
```

After the underlying defects are fixed, convert the relevant cases into normal
regression tests with fixed expectations.
