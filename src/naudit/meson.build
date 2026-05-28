# Sources for the naudit module: rule-loader, engine, output
# formatters, and the CLI core. The `naudit` binary lives at
# `src/tools/naudit.c` and links against libn00b (which absorbs
# these TUs via the aggregated `n00b_dll_src` list in the root
# `meson.build`).
#
# The audit-rule-file metagrammar is embedded at build time via
# `embed_metagrammar.py` (a Python codegen script that emits
# `audit_rule_file_grammar.h` with a C-string literal carrying
# the BNF bytes). The custom_target is declared in the root
# `meson.build` and the generated header is listed alongside the
# `.c` sources below so meson generates it before any naudit TU
# compiles and surfaces its containing directory on the implicit
# include path.

n00b_naudit_src = files(
    'blame.c',
    'cli.c',
    'engine.c',
    'exemption.c',
    'filter.c',
    'guidance.c',
    'languages.c',
    'module.c',
    'output.c',
    'output_json.c',
    'rule_file_tokenizer.c',
    'tokenizer_registry.c',
)
