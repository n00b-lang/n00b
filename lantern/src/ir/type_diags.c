#include "private/ir/internal.h"
#include "ir/typelang.h"
#include "ir/text.h"
#include "ir/diag.h"

#include "lib/buffer.h"
#include "th/typehash.h"

#include <string.h>

static ir_diagcode_t
code_for(th_err_kind kind)
{
    switch (kind) {
    case TH_ERR_UNIFY:
    case TH_ERR_OCCURS:
        return IR_DIAG_TYPE_MISMATCH;

    case TH_ERR_ARITY_MISMATCH:
    case TH_ERR_PARAM_MISMATCH:
    case TH_ERR_MISSING_KEYWORD:
    case TH_ERR_UNKNOWN_KEYWORD:
        return IR_DIAG_TYPE_ARGS;

    case TH_ERR_NO_SUCH_FIELD:
        return IR_DIAG_TYPE_MEMBER;

    case TH_ERR_DUPLICATE_VARIANT:
    case TH_ERR_NON_EXHAUSTIVE:
    case TH_ERR_UNREACHABLE_PATTERN:
        return IR_DIAG_TYPE_ARM;

    default:
        return IR_DIAG_TYPE_CLASS;
    }
}

static ir_span_t
span_from(ir_module_t *m, th_span_t s)
{
    if (!s.start_line && !s.end_line) {
        return (ir_span_t){.file = -1, .implied = true};
    }

    int32_t file = -1;

    if (m && s.file) {
        for (size_t i = 0; i < ir_file_count(m); i++) {
            const char *path = ir_file_path(m, (int32_t)i);

            if (path && !strcmp(path, s.file)) {
                file = (int32_t)i;

                break;
            }
        }
    }

    return (ir_span_t){
        .file       = file,
        .start_line = s.start_line,
        .start_col  = s.start_col,
        .end_line   = s.end_line,
        .end_col    = s.end_col,
    };
}

static void
describe(core_buffer_t *sb, ir_module_t *m, const th_error_t *e)
{
    core_buffer_printf(sb, "%s", e->message ? e->message
                                           : th_err_kind_name(e->kind));

    if (e->constraint) {
        core_buffer_printf(sb, " (%s)", e->constraint);
    }

    if (e->expected) {
        const char *want = m ? ir_type_text(m, e->expected) : nullptr;

        core_buffer_printf(sb, ": expected %s", want ? want : "?");
    }

    if (e->got) {
        const char *got = m ? ir_type_text(m, e->got) : nullptr;

        core_buffer_printf(sb, e->expected ? ", got %s" : ": got %s",
                          got ? got : "?");
    }
}

uint32_t ir_import_type_errors(ir_module_t *m, ir_diags_t *d)
{
    th_ctx *tc = m ? ir_module_types(m) : nullptr;

    if (!tc || !d) {
        return 0;
    }

    size_t   n    = th_error_count(tc);
    uint32_t made = 0;

    for (size_t i = 0; i < n; i++) {
        th_error_t e = {0};

        if (!th_error_at(tc, i, &e)) {
            continue;
        }

        ir_diag_t *dg = ir_diag_begin(d, code_for(e.kind), span_from(m, e.span),
                                      th_err_kind_name(e.kind));

        if (!dg) {
            continue;
        }

        core_buffer_t *sb = core_buffer_empty();

        describe(sb, m, &e);

        char *msg = core_buffer_take(sb);

        ir_diag_message(dg, d, msg);
        core_free(msg);

        if (e.has_related) {
            ir_diag_secondary(d, dg, span_from(m, e.related), "related");
        }

        made++;
    }

    th_errors_clear(tc);

    return made;
}

