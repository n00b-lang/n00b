#include "gen.h"

static const char runtime[] =
    "use std::cell::RefCell;\n"
    "use std::ffi::c_int;\n"
    "use std::panic::{catch_unwind, AssertUnwindSafe};\n"
    "\n"
    "pub const STATUS_OK: c_int = 0;\n"
    "pub const STATUS_NONE: c_int = 1;\n"
    "pub const STATUS_ERROR: c_int = 2;\n"
    "pub const STATUS_PANIC: c_int = 3;\n"
    "\n"
    "thread_local! {\n"
    "    static LAST_ERROR: RefCell<Vec<u8>> = const { RefCell::new(Vec::new()) };\n"
    "}\n"
    "\n"
    "fn fail(text: String) -> c_int {\n"
    "    LAST_ERROR.with(|cell| *cell.borrow_mut() = text.into_bytes());\n"
    "    STATUS_ERROR\n"
    "}\n"
    "\n"
    "fn guard(body: impl FnOnce() -> c_int) -> c_int {\n"
    "    match catch_unwind(AssertUnwindSafe(body)) {\n"
    "        Ok(status) => status,\n"
    "        Err(payload) => {\n"
    "            let text = payload\n"
    "                .downcast_ref::<&str>()\n"
    "                .map(|text| text.to_string())\n"
    "                .or_else(|| payload.downcast_ref::<String>().cloned())\n"
    "                .unwrap_or_else(|| \"panic\".to_string());\n"
    "            fail(text);\n"
    "            STATUS_PANIC\n"
    "        }\n"
    "    }\n"
    "}\n"
    "\n"
    "unsafe fn slice<'a, T>(data: *const T, len: usize) -> &'a [T] {\n"
    "    if len == 0 {\n"
    "        &[]\n"
    "    } else {\n"
    "        std::slice::from_raw_parts(data, len)\n"
    "    }\n"
    "}\n"
    "\n"
    "unsafe fn text<'a>(data: *const u8, len: usize) -> Result<&'a str, c_int> {\n"
    "    std::str::from_utf8(slice(data, len)).map_err(|error| fail(error.to_string()))\n"
    "}\n"
    "\n"
    "unsafe fn borrow<'a, T>(handle: *const T) -> Result<&'a T, c_int> {\n"
    "    handle.as_ref().ok_or_else(|| fail(\"null handle\".to_string()))\n"
    "}\n"
    "\n"
    "unsafe fn take<T>(handle: *mut T) -> Result<T, c_int> {\n"
    "    if handle.is_null() {\n"
    "        Err(fail(\"null handle\".to_string()))\n"
    "    } else {\n"
    "        Ok(*Box::from_raw(handle))\n"
    "    }\n"
    "}\n"
    "\n"
    "unsafe fn write_all<T, U>(values: &[T], out: *mut U, cap: usize, total: *mut usize, convert: impl Fn(&T) -> U) {\n"
    "    for (index, value) in values.iter().take(cap).enumerate() {\n"
    "        out.add(index).write(convert(value));\n"
    "    }\n"
    "    *total = values.len();\n"
    "}\n";

typedef struct {
    const writ_rust_t *rust;
    writ_emit_t        emit;
    writ_str_t         crate;
} printer_t;

static writ_str_t
sym(const printer_t *p, writ_sym_t symbol)
{
    return writ_sym_str(p->rust->session, symbol);
}

static writ_str_t
handle_type(const printer_t *p, writ_sym_t name)
{
    writ_str_t snake = writ_name_case(p->rust->session, sym(p, name), WRIT_CASE_SNAKE);
    return writ_str_format(p->rust->session, "%.*s_%.*s", S(p->rust->prefix), S(snake));
}

static writ_str_t
binding(writ_str_t name)
{
    return writ_str_eq(name, writ_cstr("self")) ? writ_cstr("this") : name;
}

static const writ_rust_member_t *
members(const printer_t *p, writ_sym_t type, uint32_t *count)
{
    const writ_rust_type_t *declared = writ_rust_type(p->rust, type);
    *count = declared->count;
    return &p->rust->members.data[declared->first];
}

static void
emit_struct_copy(printer_t *p, writ_sym_t type, const char *path, const char *from)
{
    uint32_t count = 0;
    const writ_rust_member_t *fields = members(p, type, &count);
    writ_str_t name = sym(p, type);
    writ_emit_text(&p->emit, "%s%.*s { ", path, S(name));
    for (uint32_t i = 0; i < count; i++) {
        writ_str_t field = sym(p, fields[i].name);
        writ_emit_text(&p->emit, "%s%.*s: %s.%.*s", i ? ", " : "", S(field), from, S(field));
    }
    writ_emit_text(&p->emit, " }");
}

static void
emit_c_params(printer_t *p, writ_str_t name, const writ_rust_value_t *value)
{
    writ_emit_t *e = &p->emit;
    writ_str_t local = binding(name);
    writ_str_t type = sym(p, value->name);
    switch (value->kind) {
    case WRIT_RUST_STR:
        writ_emit_line(e, "%.*s: *const u8,", S(local));
        writ_emit_line(e, "%.*s_len: usize,", S(name));
        return;
    case WRIT_RUST_SLICE:
        writ_emit_line(e, "%.*s: *const %.*s,", S(local), S(type));
        writ_emit_line(e, "%.*s_len: usize,", S(name));
        return;
    case WRIT_RUST_ENUM:
        writ_emit_line(e, "%.*s: c_int,", S(local));
        return;
    case WRIT_RUST_OPAQUE: {
        writ_str_t handle = handle_type(p, value->name);
        writ_emit_line(e, "%.*s: *%s %.*s,", S(local), value->borrowed ? "const" : "mut", S(handle));
        return;
    }
    default:
        writ_emit_line(e, "%.*s: %.*s,", S(local), S(type));
        return;
    }
}

static void
emit_result_params(printer_t *p, const writ_rust_value_t *value)
{
    writ_emit_t *e = &p->emit;
    writ_str_t type = sym(p, value->name);
    switch (value->kind) {
    case WRIT_RUST_UNIT:
        return;
    case WRIT_RUST_STRING:
    case WRIT_RUST_VEC:
        writ_emit_line(e, "out: *mut %.*s,", S(value->kind == WRIT_RUST_STRING ? writ_cstr("u8") : type));
        writ_emit_line(e, "out_cap: usize,");
        writ_emit_line(e, "out_total: *mut usize,");
        return;
    case WRIT_RUST_ENUM:
        writ_emit_line(e, "out: *mut c_int,");
        return;
    case WRIT_RUST_OPAQUE: {
        writ_str_t handle = handle_type(p, value->name);
        writ_emit_line(e, "out: *mut *mut %.*s,", S(handle));
        return;
    }
    default:
        writ_emit_line(e, "out: *mut %.*s,", S(type));
        return;
    }
}

static void
emit_conversion(printer_t *p, writ_str_t name, const writ_rust_value_t *value)
{
    writ_emit_t *e = &p->emit;
    writ_str_t local = binding(name);
    switch (value->kind) {
    case WRIT_RUST_STR:
        writ_emit_line(e, "let %.*s = match text(%.*s, %.*s_len) {", S(local), S(local), S(name));
        break;
    case WRIT_RUST_SLICE:
        if (value->element == WRIT_RUST_STRUCT) {
            writ_emit_text(e, "let %.*s: Vec<%.*s::%.*s> = slice(%.*s, %.*s_len).iter().map(|value| ", S(local),
                           S(p->crate), S(sym(p, value->name)), S(local), S(name));
            emit_struct_copy(p, value->name, writ_str_format(p->rust->session, "%.*s::", S(p->crate)).data, "value");
            writ_emit_line(e, ").collect();");
        } else {
            writ_emit_line(e, "let %.*s = slice(%.*s, %.*s_len);", S(local), S(local), S(name));
        }
        return;
    case WRIT_RUST_ENUM: {
        uint32_t count = 0;
        const writ_rust_member_t *variants = members(p, value->name, &count);
        writ_emit_open(e, "let %.*s = match %.*s", S(local), S(local));
        for (uint32_t i = 0; i < count; i++) {
            writ_emit_line(e, "%u => %.*s::%.*s::%.*s,", i, S(p->crate), S(sym(p, value->name)),
                           S(sym(p, variants[i].name)));
        }
        writ_emit_line(e, "_ => return fail(\"invalid %.*s\".to_string()),", S(sym(p, value->name)));
        writ_emit_close(e, ";");
        return;
    }
    case WRIT_RUST_STRUCT:
        writ_emit_text(e, "let %.*s = ", S(local));
        emit_struct_copy(p, value->name, writ_str_format(p->rust->session, "%.*s::", S(p->crate)).data,
                         writ_str_copy(p->rust->session, local.data, local.length).data);
        writ_emit_line(e, ";");
        return;
    case WRIT_RUST_OPAQUE:
        writ_emit_line(e, "let %.*s = match %s(%.*s) {", S(local), value->borrowed ? "borrow" : "take", S(local));
        writ_emit_indent(e, 1);
        writ_emit_line(e, value->borrowed ? "Ok(value) => &value.0," : "Ok(value) => value.0,");
        writ_emit_line(e, "Err(status) => return status,");
        writ_emit_close(e, ";");
        return;
    default:
        return;
    }
    writ_emit_indent(e, 1);
    writ_emit_line(e, "Ok(value) => value,");
    writ_emit_line(e, "Err(status) => return status,");
    writ_emit_close(e, ";");
}

static void
emit_write(printer_t *p, const writ_rust_value_t *value)
{
    writ_emit_t *e = &p->emit;
    switch (value->kind) {
    case WRIT_RUST_UNIT:
        return;
    case WRIT_RUST_PRIM:
        writ_emit_line(e, "*out = result;");
        return;
    case WRIT_RUST_ENUM: {
        uint32_t count = 0;
        const writ_rust_member_t *variants = members(p, value->name, &count);
        writ_emit_line(e, "#[allow(unreachable_patterns)]");
        writ_emit_open(e, "let code = match result");
        for (uint32_t i = 0; i < count; i++) {
            writ_emit_line(e, "%.*s::%.*s::%.*s => %u,", S(p->crate), S(sym(p, value->name)),
                           S(sym(p, variants[i].name)), i);
        }
        writ_emit_line(e, "_ => return fail(\"unknown %.*s\".to_string()),", S(sym(p, value->name)));
        writ_emit_close(e, ";");
        writ_emit_line(e, "*out = code;");
        return;
    }
    case WRIT_RUST_STRUCT:
        writ_emit_text(e, "*out = ");
        emit_struct_copy(p, value->name, "", "result");
        writ_emit_line(e, ";");
        return;
    case WRIT_RUST_OPAQUE:
        writ_emit_line(e, "*out = Box::into_raw(Box::new(%.*s(result)));", S(handle_type(p, value->name)));
        return;
    case WRIT_RUST_STRING:
        writ_emit_line(e, "write_all(result.as_bytes(), out, out_cap, out_total, |byte| *byte);");
        return;
    case WRIT_RUST_VEC:
        if (value->element == WRIT_RUST_STRUCT) {
            writ_emit_text(e, "write_all(&result, out, out_cap, out_total, |value| ");
            emit_struct_copy(p, value->name, "", "value");
            writ_emit_line(e, ");");
        } else {
            writ_emit_line(e, "write_all(&result, out, out_cap, out_total, |value| *value);");
        }
        return;
    default:
        return;
    }
}

static writ_str_t
feature_gate(const printer_t *p, writ_sym_t feature)
{
    return feature ? writ_str_format(p->rust->session, "#[cfg(feature = \"%.*s\")]\n", S(sym(p, feature)))
                   : (writ_str_t){"", 0};
}

static void
emit_fn(printer_t *p, const writ_rust_fn_t *fn)
{
    writ_emit_t *e = &p->emit;
    const writ_rust_t *rust = p->rust;
    writ_emit_line(e, "");
    writ_emit_text(e, "%.*s", S(feature_gate(p, fn->feature)));
    writ_emit_line(e, "#[no_mangle]");
    writ_emit_line(e, "pub unsafe extern \"C\" fn %.*s(", S(sym(p, fn->symbol)));
    writ_emit_indent(e, 1);
    writ_rust_value_t self = {.kind = WRIT_RUST_OPAQUE, .name = fn->owner, .borrowed = fn->receiver != WRIT_RUST_BY_VALUE};
    if (fn->receiver != WRIT_RUST_FREE) {
        emit_c_params(p, writ_cstr("self"), &self);
    }
    for (uint32_t i = 0; i < fn->count; i++) {
        const writ_rust_member_t *param = &rust->members.data[fn->first + i];
        emit_c_params(p, sym(p, param->name), &param->value);
    }
    emit_result_params(p, &fn->result);
    writ_emit_indent(e, -1);
    writ_emit_open(e, ") -> c_int");
    writ_emit_open(e, "guard(||");
    if (fn->receiver != WRIT_RUST_FREE) {
        emit_conversion(p, writ_cstr("self"), &self);
    }
    for (uint32_t i = 0; i < fn->count; i++) {
        const writ_rust_member_t *param = &rust->members.data[fn->first + i];
        emit_conversion(p, sym(p, param->name), &param->value);
    }
    if (fn->receiver != WRIT_RUST_FREE) {
        writ_emit_text(e, "let result = this.%.*s(", S(sym(p, fn->name)));
    } else {
        writ_emit_text(e, "let result = %.*s::%.*s::%.*s(", S(p->crate), S(sym(p, fn->owner)), S(sym(p, fn->name)));
    }
    for (uint32_t i = 0; i < fn->count; i++) {
        const writ_rust_member_t *param = &rust->members.data[fn->first + i];
        bool by_ref = param->value.kind == WRIT_RUST_SLICE && param->value.element == WRIT_RUST_STRUCT;
        writ_emit_text(e, "%s%s%.*s", i ? ", " : "", by_ref ? "&" : "", S(binding(sym(p, param->name))));
    }
    writ_emit_line(e, ");");
    if (fn->result.fallible) {
        writ_emit_open(e, "let result = match result");
        writ_emit_line(e, "Ok(value) => value,");
        writ_emit_line(e, "Err(error) => return fail(error.to_string()),");
        writ_emit_close(e, ";");
    }
    if (fn->result.optional) {
        writ_emit_open(e, "let result = match result");
        writ_emit_line(e, "Some(value) => value,");
        writ_emit_line(e, "None => return STATUS_NONE,");
        writ_emit_close(e, ";");
    }
    if (fn->result.kind == WRIT_RUST_UNIT) {
        writ_emit_line(e, "let () = result;");
    }
    emit_write(p, &fn->result);
    writ_emit_line(e, "STATUS_OK");
    writ_emit_close(e, ")");
    writ_emit_close(e, nullptr);
}

static void
emit_type(printer_t *p, const writ_rust_type_t *type)
{
    writ_emit_t *e = &p->emit;
    writ_str_t name = sym(p, type->name);
    if (type->kind == WRIT_RUST_STRUCT) {
        uint32_t count = 0;
        const writ_rust_member_t *fields = members(p, type->name, &count);
        writ_emit_line(e, "");
        writ_emit_line(e, "#[repr(C)]");
        writ_emit_line(e, "#[derive(Clone, Copy)]");
        writ_emit_open(e, "pub struct %.*s", S(name));
        for (uint32_t i = 0; i < count; i++) {
            writ_emit_line(e, "pub %.*s: %.*s,", S(sym(p, fields[i].name)), S(sym(p, fields[i].value.name)));
        }
        writ_emit_close(e, nullptr);
    } else if (type->kind == WRIT_RUST_OPAQUE) {
        writ_str_t handle = handle_type(p, type->name);
        writ_emit_line(e, "");
        writ_emit_line(e, "pub struct %.*s(%.*s::%.*s);", S(handle), S(p->crate), S(name));
        writ_emit_line(e, "");
        writ_emit_line(e, "#[no_mangle]");
        writ_emit_open(e, "pub unsafe extern \"C\" fn %.*s_free(value: *mut %.*s)", S(handle), S(handle));
        writ_emit_open(e, "if !value.is_null()");
        writ_emit_line(e, "let _ = catch_unwind(AssertUnwindSafe(|| drop(Box::from_raw(value))));");
        writ_emit_close(e, nullptr);
        writ_emit_close(e, nullptr);
    }
}

static writ_str_t
library_text(printer_t *p)
{
    writ_emit_t *e = &p->emit;
    const writ_rust_t *rust = p->rust;
    writ_emit_init(e);
    writ_emit_line(e, "#![allow(non_camel_case_types, clippy::missing_safety_doc)]");
    writ_emit_line(e, "");
    writ_emit_text(e, "%s", runtime);
    writ_emit_line(e, "");
    writ_emit_line(e, "#[no_mangle]");
    writ_emit_open(e, "pub unsafe extern \"C\" fn %.*s_last_error(buf: *mut u8, cap: usize) -> usize", S(rust->prefix));
    writ_emit_open(e, "LAST_ERROR.with(|cell|");
    writ_emit_line(e, "let text = cell.borrow();");
    writ_emit_line(e, "let count = text.len().min(cap);");
    writ_emit_open(e, "if count > 0");
    writ_emit_line(e, "std::ptr::copy_nonoverlapping(text.as_ptr(), buf, count);");
    writ_emit_close(e, nullptr);
    writ_emit_line(e, "text.len()");
    writ_emit_close(e, ")");
    writ_emit_close(e, nullptr);
    for (size_t i = 0; i < rust->types.len; i++) {
        emit_type(p, &rust->types.data[i]);
    }
    for (size_t i = 0; i < rust->fns.len; i++) {
        emit_fn(p, &rust->fns.data[i]);
    }
    return writ_emit_finish(e, rust->session);
}

static writ_str_t
manifest_text(printer_t *p, const writ_gen_rust_request_t *request)
{
    const writ_rust_t *rust = p->rust;
    writ_emit_t e;
    writ_emit_init(&e);
    writ_str_t package = sym(p, rust->crate);
    writ_emit_line(&e, "[package]");
    writ_emit_line(&e, "name = \"%.*s-writ\"", S(package));
    writ_emit_line(&e, "version = \"%.*s\"", S(sym(p, rust->version)));
    writ_emit_line(&e, "edition = \"2021\"");
    writ_emit_line(&e, "publish = false");
    writ_emit_line(&e, "");
    writ_emit_line(&e, "[lib]");
    writ_emit_line(&e, "name = \"%.*s_writ\"", S(p->crate));
    writ_emit_line(&e, "path = \"src/lib.rs\"");
    writ_emit_line(&e, "crate-type = [\"staticlib\", \"cdylib\", \"rlib\"]");
    writ_emit_line(&e, "");
    writ_emit_line(&e, "[dependencies]");
    writ_emit_line(&e, "%.*s = { path = \"%.*s\" }", S(package), S(request->crate_path));
    writ_emit_line(&e, "");
    writ_emit_line(&e, "[features]");
    for (size_t i = 0; i < rust->features.len; i++) {
        writ_emit_line(&e, "%.*s = [\"%.*s/%.*s\"]", S(sym(p, rust->features.data[i].name)), S(package),
                       S(sym(p, rust->features.data[i].cargo)));
    }
    writ_emit_line(&e, "");
    writ_emit_line(&e, "[workspace]");
    return writ_emit_finish(&e, rust->session);
}

writ_status_t
writ_gen_rust(const writ_model_t *model, const writ_gen_rust_request_t *request, const writ_output_t *output)
{
    if (!model || !request || !output || !model->rust || !request->crate_path.length) {
        return WRIT_INVALID_ARGUMENT;
    }
    printer_t p = {.rust = model->rust,
                   .crate = writ_name_case(model->session, writ_sym_str(model->session, model->rust->crate),
                                           WRIT_CASE_SNAKE)};
    writ_status_t status = output->write_file(output->context, writ_cstr("Cargo.toml"), manifest_text(&p, request));
    return status == WRIT_OK ? output->write_file(output->context, writ_cstr("src/lib.rs"), library_text(&p)) : status;
}
