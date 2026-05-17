# Engine core port — Phase 7. Mirrors ~/resharp-c/src/engine/ (excluding
# stream.c, which lives in ../stream/ per ~/dd/n00b_regex_port_plan.md § 6).
#
# NOTE: engine/regex.c is the resharp internal `Regex` struct impl — distinct
# from the public-facing src/text/regex/public.c (Phase 9). Keep both names
# as-is to keep upstream-proxy diffs mechanical.

n00b_text_regex_engine_src = files(
    'accel.c',
    'bdfa.c',
    'engine.c',
    'fas.c',
    'prefix.c',
    'regex.c',
)
