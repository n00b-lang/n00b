import re
import sys

def_path, output_path = sys.argv[1], sys.argv[2]
mode = sys.argv[3] if len(sys.argv) > 3 else ""
extra_path = sys.argv[4] if len(sys.argv) > 4 else None

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

enum_names = {name for name, _ in enums}


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


def header_text():
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
    ]
    for kind in ("const", "record", "handle", "enum", "channel"):
        lines += [
            f"size_t writ_model_{kind}_count(const writ_model_t *model);",
            f"const writ_{kind}_view_t *writ_model_{kind}_at(const writ_model_t *model, size_t index);",
            f"const writ_{kind}_view_t *writ_model_{kind}_named(const writ_model_t *model, writ_str_t name);",
        ]
    lines += [
        "const writ_plan_view_t *writ_model_fn_plan(const writ_model_t *model, const writ_fn_view_t *fn);",
        "writ_status_t writ_model_plan_render(const writ_model_t *model, const writ_plan_view_t *plan, writ_str_t *rendered);",
        "writ_status_t writ_lower_json(const writ_model_t *model, const writ_output_t *output);",
        "",
        "#ifdef __cplusplus",
        "}",
        "#endif",
        "",
        "#endif",
        "",
    ]
    return "\n".join(lines)


def common_prefix(values):
    prefix = values[0]
    for value in values[1:]:
        while not value.startswith(prefix):
            prefix = prefix[:-1]
    return prefix[: prefix.rfind("_") + 1]


def names_text():
    out = ["#pragma once"]
    for name, values in enums:
        prefix = common_prefix(values) if len(values) > 1 else values[0][: values[0].rfind("_") + 1]
        table = name[: -len("_t")] + "_names"
        spelled = ", ".join(f'"{v[len(prefix):].lower()}"' for v in values)
        out.append(f"static const char *const {table}[] = {{{spelled}}};")
    return "\n".join(out) + "\n"


def json_field(kind, kind_arg, field):
    if kind == "WRIT_REF":
        if kind_arg == "type":
            return f"json_type(json, model, view->{field});"
        return f"if (view->{field}) {{ json_view_{kind_arg}(json, model, view->{field}); }} else {{ json_null(json); }}"
    if kind == "WRIT_ARRAY":
        return (f"json_open(json, '['); for (size_t i = 0; i < view->{field}_count; i++) {{ "
                f"json_view_{kind_arg}(json, model, &view->{field}[i]); }} json_close(json, ']');")
    if kind == "WRIT_LIST":
        item = "json_str(json, view->{f}[i])" if kind_arg == "writ_str_t" else "json_uint(json, view->{f}[i])"
        return (f"json_open(json, '['); for (size_t i = 0; i < view->{field}_count; i++) {{ "
                f"{item.format(f=field)}; }} json_close(json, ']');")
    if kind_arg in enum_names:
        table = kind_arg[: -len("_t")] + "_names"
        return f"json_enum(json, {table}, sizeof({table}) / sizeof({table}[0]), view->{field});"
    if kind_arg == "writ_str_t":
        return f"json_str(json, view->{field});"
    if kind_arg == "bool":
        return f"json_bool(json, view->{field});"
    if kind_arg == "writ_span_t":
        return f"json_span(json, view->{field});"
    if field.endswith("_expr") or field.endswith("_node"):
        return f"json_expr(json, model, view->{field});"
    if kind_arg == "int64_t":
        return f"json_int(json, view->{field});"
    return f"json_uint(json, view->{field});"


def json_text():
    out = ["#pragma once", ""]
    lowered = [name for name in order if not name.startswith("plan")]
    for name in lowered:
        out.append(f"static void json_view_{name}(json_t *json, const writ_model_t *model, const {view_type(name)} *view);")
    out.append("")
    for name in lowered:
        out.append(f"static void")
        out.append(f"json_view_{name}(json_t *json, const writ_model_t *model, const {view_type(name)} *view)")
        out.append("{")
        out.append("    (void)model;")
        out.append("    json_open(json, '{');")
        for kind, kind_arg, field in sorted(views[name], key=lambda entry: entry[2]):
            if kind_arg.startswith("plan") or kind_arg == "fn":
                continue
            out.append(f'    json_key(json, "{field}");')
            out.append(f"    {json_field(kind, kind_arg, field)}")
        out.append("    json_close(json, '}');")
        out.append("}")
        out.append("")
    return "\n".join(out)


header = header_text()
if mode == "--check":
    with open(output_path, encoding="utf-8") as existing:
        if existing.read() != header:
            sys.exit(f"{output_path} is stale; regenerate it from {def_path}")
elif mode == "--names":
    with open(extra_path, "w", encoding="utf-8") as out:
        out.write(names_text())
elif mode == "--json":
    with open(extra_path, "w", encoding="utf-8") as out:
        out.write(json_text())
else:
    with open(output_path, "w", encoding="utf-8") as out:
        out.write(header)
