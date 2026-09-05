fe_pwz_test = executable(
    'fe_pwz_test',
    'test_pwz.c',
    dependencies: fe_dep,
)

test('fe-pwz', fe_pwz_test)

fe_bnf_test = executable(
    'fe_bnf_test',
    'test_bnf.c',
    dependencies: fe_dep,
)

test(
    'fe-bnf',
    fe_bnf_test,
    workdir: meson.current_source_dir(),
)

fe_annot_test = executable(
    'fe_annot_test',
    'test_annot.c',
    dependencies: fe_dep,
)

test(
    'fe-annot',
    fe_annot_test,
    workdir: meson.current_source_dir(),
)

fe_bnf_slay_test = executable(
    'fe_bnf_slay_test',
    'test_bnf_slay.c',
    dependencies: fe_dep,
)

test(
    'fe-bnf-slay',
    fe_bnf_slay_test,
    workdir: meson.current_source_dir(),
)
