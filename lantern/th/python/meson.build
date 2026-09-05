python = find_program('python3', required: false)

if not python.found()
    subdir_done()
endif

here = meson.current_source_dir()

test(
    'spec-vectors',
    python,
    args: [files('test_spec.py')],
    workdir: here,
)

test(
    'unification',
    python,
    args: [files('test_unify.py')],
    workdir: here,
)

test(
    'c-vs-python',
    python,
    args: [files('crosscheck.py'), th_tool],
    workdir: here,
)

test(
    'c-vs-python-inference',
    python,
    args: [files('crosscheck_infer.py'), th_tool],
    workdir: here,
)

test(
    'cycles',
    python,
    args: [files('check_cycles.py'), th_cycle_test],
    workdir: here,
)
