import re
import sys

def_path, output_path = sys.argv[1], sys.argv[2]
check_only = len(sys.argv) > 3 and sys.argv[3] == "--check"
names_path = sys.argv[3] if len(sys.argv) > 3 and not check_only else None

call = re.compile(r"^(WRIT_[A-Z]+)\((.*)\)\s*$")
enums, views, order = [], {}, []

with open(def_path, encoding="utf-8") as source:
    for line in source:
        match = call.match(line.strip())
        if not match:
            continue
        macro, args = match.group(1), [a.strip() for a in match.group(2).split(",")]
        if macro == "WRIT_ENUM":
            enums.append((args[0], args[1:]))
        elif macro == "WRIT_VIEW":
            views[args[0]] = []
            order.append(args[0])
        elif macro in ("WRIT_FIELD", "WRIT_REF", "WRIT_ARRAY", "WRIT_LIST"):
            views[args[0]].append((macro, args[1], args[2]))

def view_type(name):
    return f"writ_{name}_view_t"

def member(kind, kind_arg, field):
    if kind == "WRIT_FIELD":
        return [f"    {kind_arg} {field};"]
    if kind == "WRIT_REF":
        return [f"    const {view_type(kind_arg)} *{field};"]
    if kind == "WRIT_ARRAY":
        return [f"    size_t {field}_count;", f"    const {view_type(kind_arg)} *{field};"]
    return [f"    size_t {field}_count;", f"    const {kind_arg} *{field};"]

lines = [
    "#ifndef WRIT_MODEL_H",
    "#define WRIT_MODEL_H",
    "",
    '#include "writ/writ.h"',
    "",
    "#ifdef __cplusplus",
    'extern "C" {',
    "#endif",
    "",
]
for name, values in enums:
    lines.append(f"typedef uint32_t {name};")
    lines.append("enum {")
    lines.extend(f"    {value}," for value in values)
    lines.append("};")
    lines.append("")
for name in order:
    lines.append(f"typedef struct writ_{name}_view {view_type(name)};")
lines.append("")
for name in order:
    lines.append(f"struct writ_{name}_view {{")
    lines.append("    size_t struct_size;")
    for kind, kind_arg, field in views[name]:
        lines.extend(member(kind, kind_arg, field))
    lines.append("};")
    lines.append("")
lines += [
    "#define WRIT_PLAN_NONE UINT32_MAX",
    "",
    "const writ_library_view_t *writ_model_library(const writ_model_t *model);",
    "size_t writ_model_fn_count(const writ_model_t *model);",
    "const writ_fn_view_t *writ_model_fn_at(const writ_model_t *model, size_t index);",
    "const writ_fn_view_t *writ_model_fn_named(const writ_model_t *model, writ_str_t name);",
    "size_t writ_model_const_count(const writ_model_t *model);",
    "const writ_const_view_t *writ_model_const_at(const writ_model_t *model, size_t index);",
    "const writ_plan_view_t *writ_model_fn_plan(const writ_model_t *model, const writ_fn_view_t *fn);",
    "writ_status_t writ_model_plan_render(const writ_model_t *model, const writ_plan_view_t *plan, writ_str_t *rendered);",
    "",
    "#ifdef __cplusplus",
    "}",
    "#endif",
    "",
    "#endif",
    "",
]
text = "\n".join(lines)

def common_prefix(values):
    prefix = values[0]
    for value in values[1:]:
        while not value.startswith(prefix):
            prefix = prefix[:-1]
    return prefix[: prefix.rfind("_") + 1]

if names_path:
    with open(names_path, "w", encoding="utf-8") as out:
        out.write("#pragma once\n")
        for name, values in enums:
            prefix = common_prefix(values) if len(values) > 1 else values[0][: values[0].rfind("_") + 1]
            table = name[: -len("_t")] + "_names"
            spelled = ", ".join(f'"{v[len(prefix):].lower()}"' for v in values)
            out.write(f"static const char *const {table}[] = {{{spelled}}};\n")
    sys.exit(0)

if check_only:
    with open(output_path, encoding="utf-8") as existing:
        if existing.read() != text:
            sys.exit(f"{output_path} is stale; regenerate it from {def_path}")
else:
    with open(output_path, "w", encoding="utf-8") as out:
        out.write(text)
