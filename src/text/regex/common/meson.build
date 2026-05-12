# Regex-internal common port — Phase 3.
#
# Per the Phase 2 audit (~/dd/n00b_regex_port_plan.md § 7.2), the resharp-c
# `src/common/` subtree is fully obsoleted: every file is REPLACE (translated
# at downstream call sites in Phases 5–8) or DROP. The lone PORT row is
# `ids.h` — header-only, no .c file lands here.
#
# This file exists to satisfy the subdir() call in the parent meson.build.

n00b_text_regex_common_src = files()
