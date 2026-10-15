#include <stdlib.h>

#include "../gen/gen.h"
#include "toolchain.h"

writ_command_t
writ_compile_command(const writ_model_t *model, writ_str_t sysroot, uint32_t extra)
{
    writ_session_t *session = model->session;
    const writ_library_view_t *library = writ_model_library(model);
    uint32_t capacity = 12 + extra + (uint32_t)(library->defines_count + library->include_paths_count);
    writ_command_t command = {writ_alloc(session, sizeof(char *) * capacity), 0};
    writ_command_add(&command, writ_tool_path(session, "clang").data);
    writ_command_add(&command, "-target");
    writ_command_add(&command, writ_str_copy(session, library->platform.data, library->platform.length).data);
    writ_str_t root = writ_sysroot(session, sysroot);
    if (root.length) {
        writ_command_add(&command, "-isysroot");
        writ_command_add(&command, writ_str_copy(session, root.data, root.length).data);
    }
    for (size_t i = 0; i < library->include_paths_count; i++) {
        writ_command_add(&command, writ_str_format(session, "-I%.*s", S(library->include_paths[i])).data);
    }
    for (size_t i = 0; i < library->defines_count; i++) {
        writ_command_add(&command, writ_str_format(session, "-D%.*s", S(library->defines[i])).data);
    }
    writ_command_add(&command, "-x");
    writ_command_add(&command, "c");
    writ_command_add(&command, "-std=c11");
    return command;
}

void
writ_command_add(writ_command_t *command, const char *arg)
{
    command->argv[command->used++] = arg;
    command->argv[command->used] = nullptr;
}

writ_str_t
writ_probe_includes(const writ_model_t *model)
{
    const writ_library_view_t *library = writ_model_library(model);
    writ_emit_t emit;
    writ_emit_init(&emit);
    writ_emit_line(&emit, "#include <stdarg.h>");
    writ_emit_line(&emit, "#include <stdbool.h>");
    writ_emit_line(&emit, "#include <stddef.h>");
    writ_emit_line(&emit, "#include <stdint.h>");
    for (size_t i = 0; i < library->headers_count; i++) {
        writ_emit_line(&emit, "#include <%.*s>", S(library->headers[i]));
    }
    return writ_emit_finish(&emit, model->session);
}

static writ_str_t
layout_probe(const writ_model_t *model)
{
    writ_session_t *session = model->session;
    writ_emit_t emit;
    writ_emit_init(&emit);
    writ_emit_text(&emit, "%.*s", S(writ_probe_includes(model)));
    for (uint32_t r = 0; r < model->records.len; r++) {
        const writ_record_rec_t *record = &model->records.data[r];
        const writ_type_rec_t *type = writ_type(model, record->type);
        writ_str_t spelled = writ_sym_str(session, type->c_spelling);
        writ_emit_line(&emit, "_Static_assert(sizeof(%.*s) == %u, \"writ:record:%u\");", S(spelled), type->size, r);
        writ_emit_line(&emit, "_Static_assert(_Alignof(%.*s) == %u, \"writ:record:%u\");", S(spelled), type->align, r);
        for (uint32_t f = 0; f < record->field_count; f++) {
            const writ_slot_rec_t *field = &model->slots.data[record->first_field + f];
            writ_str_t name = writ_sym_str(session, field->name);
            writ_emit_line(&emit, "_Static_assert(offsetof(%.*s, %.*s) == %u, \"writ:slot:%u\");", S(spelled), S(name),
                           field->offset, record->first_field + f);
        }
    }
    for (uint32_t c = 0; c < model->consts.len; c++) {
        const writ_const_rec_t *constant = &model->consts.data[c];
        if (constant->text) {
            continue;
        }
        writ_str_t name = writ_sym_str(session, constant->name);
        writ_emit_line(&emit, "_Static_assert((long long)(%.*s) == %lldLL, \"writ:const:%u\");", S(name),
                       (long long)constant->value, c);
    }
    return writ_emit_finish(&emit, session);
}

static void
report_failures(const writ_model_t *model, writ_str_t output)
{
    writ_session_t *session = model->session;
    static const char *const kinds[] = {"writ:record:", "writ:slot:", "writ:const:"};
    for (size_t at = 0; at < output.length; at++) {
        for (size_t k = 0; k < 3; k++) {
            size_t length = strlen(kinds[k]);
            if (at + length > output.length || memcmp(output.data + at, kinds[k], length)) {
                continue;
            }
            uint32_t index = (uint32_t)strtoul(output.data + at + length, nullptr, 10);
            writ_loc_t loc = k == 0 ? model->records.data[index].loc
                           : k == 1 ? model->slots.data[index].loc
                                    : model->consts.data[index].loc;
            writ_report(session, WRIT_D_LAYOUT_TOOLCHAIN, loc, "the compiler's probe disagrees");
        }
    }
}

static bool
prepare_probe(const writ_model_t *model, writ_str_t sysroot, writ_str_t *report, const char *const *flags,
              writ_command_t *command, writ_status_t *status)
{
    *status = !model || !report ? WRIT_INVALID_ARGUMENT : WRIT_OK;
    bool headers = *status == WRIT_OK && writ_model_library(model)->headers_count;
    if (*status == WRIT_OK && !headers) {
        *report = writ_cstr("skipped: the contract names no headers\n");
    }
    uint32_t count = 0;
    while (headers && flags[count]) {
        count++;
    }
    if (headers) {
        *command = writ_compile_command(model, sysroot, count);
    }
    for (uint32_t i = 0; i < count; i++) {
        writ_command_add(command, flags[i]);
    }
    return headers;
}

writ_status_t
writ_toolchain_verify_probes(const writ_model_t *model, writ_str_t sysroot, writ_str_t *report)
{
    static const char *const flags[] = {"-fsyntax-only", "-", nullptr};
    writ_command_t command;
    writ_status_t ready;
    if (!prepare_probe(model, sysroot, report, flags, &command, &ready)) {
        return ready;
    }
    writ_session_t *session = model->session;
    size_t first_diag = session->diags.len;
    writ_str_t probe = layout_probe(model);
    writ_run_result_t result;
    writ_status_t status = writ_run(session, command.argv, probe, &result);
    if (status != WRIT_OK) {
        writ_report(session, WRIT_D_TOOLCHAIN, WRIT_NO_LOC, "cannot run %s", command.argv[0]);
    } else if (result.exit_code) {
        report_failures(model, result.err);
        if (session->diags.len == first_diag) {
            writ_report(session, WRIT_D_TOOLCHAIN, WRIT_NO_LOC, "%.*s", S(result.err));
        }
    }
    *report = probe;
    writ_diag_flush(session, first_diag);
    return session->diags.len == first_diag ? WRIT_OK : WRIT_HAS_ERRORS;
}

static bool
probeable(const writ_fn_rec_t *fn)
{
    return fn->kind == WRIT_FN_FUNCTION && !fn->expands_node && !fn->no_symbol && fn->varargs == WRIT_VARARGS_NONE;
}

static writ_str_t
abi_probe(const writ_model_t *model)
{
    writ_session_t *session = model->session;
    writ_c_context_t c = {model, writ_cstr("writ"), writ_cstr("WRIT"), false};
    writ_emit_t emit;
    writ_emit_init(&emit);
    writ_emit_text(&emit, "%.*s", S(writ_probe_includes(model)));
    for (uint32_t f = 0; f < model->fns.len; f++) {
        const writ_fn_rec_t *record = &model->fns.data[f];
        if (!probeable(record)) {
            continue;
        }
        const writ_fn_view_t *fn = &model->fn_views[f];
        writ_str_t probe = writ_str_format(session, "writ_probe_%u", f);
        writ_emit_text(&emit, "__attribute__((used)) ");
        writ_c_emit_real_prototype(&c, &emit, fn, probe, false, "");
        writ_emit_open(&emit, "");
        writ_emit_text(&emit, "%s%.*s(", fn->result ? "return " : "", S(fn->link_symbol));
        for (size_t i = 0; i < fn->slots_count; i++) {
            writ_emit_text(&emit, "%s%.*s", i ? ", " : "", S(fn->slots[i].name));
        }
        writ_emit_line(&emit, ");");
        writ_emit_close(&emit, nullptr);
    }
    return writ_emit_finish(&emit, session);
}

static writ_str_t
declaration_of(const writ_model_t *model, writ_str_t ir, writ_str_t symbol)
{
    writ_str_t needle = writ_str_format(model->session, "@%.*s(", S(symbol));
    for (size_t at = 0; at + 8 < ir.length; at++) {
        if ((at && ir.data[at - 1] != '\n') || memcmp(ir.data + at, "declare ", 8)) {
            continue;
        }
        size_t end = at;
        while (end < ir.length && ir.data[end] != '\n') {
            end++;
        }
        writ_str_t line = {ir.data + at, end - at};
        for (size_t i = 0; i + needle.length <= line.length; i++) {
            if (!memcmp(line.data + i, needle.data, needle.length)) {
                return line;
            }
        }
    }
    return writ_cstr("");
}

writ_status_t
writ_toolchain_abi_classes(const writ_model_t *model, writ_str_t sysroot, writ_str_t *report)
{
    static const char *const flags[] = {"-S", "-emit-llvm", "-O0", "-o", "-", "-", nullptr};
    writ_command_t command;
    writ_status_t ready;
    if (!prepare_probe(model, sysroot, report, flags, &command, &ready)) {
        return ready;
    }
    writ_run_result_t result;
    writ_session_t *session = model->session;
    size_t first_diag = session->diags.len;
    writ_emit_t emit;
    writ_emit_init(&emit);
    if (writ_run(session, command.argv, abi_probe(model), &result) != WRIT_OK || result.exit_code) {
        writ_report(session, WRIT_D_TOOLCHAIN, WRIT_NO_LOC, "the ABI probe does not compile: %.*s", S(result.err));
    }
    for (uint32_t f = 0; session->diags.len == first_diag && f < model->fns.len; f++) {
        const writ_fn_rec_t *record = &model->fns.data[f];
        if (!probeable(record)) {
            continue;
        }
        writ_str_t symbol = writ_sym_str(session, record->link_symbol);
        writ_str_t lowered = declaration_of(model, result.out, symbol);
        if (!lowered.length) {
            writ_report(session, WRIT_D_TOOLCHAIN, record->loc, "the ABI probe did not declare `%.*s`", S(symbol));
        }
        writ_emit_line(&emit, "%.*s: %.*s", S(symbol), S(lowered));
    }
    *report = writ_emit_finish(&emit, session);
    writ_diag_flush(session, first_diag);
    return session->diags.len == first_diag ? WRIT_OK : WRIT_HAS_ERRORS;
}
