#include "ir_internal.h"
#include "ir/tree_pass.h"
#include "ir/typelang.h"
#include "ir/text.h"
#include "ir/diag.h"

#include "lib/buffer.h"
#include "typehash.h"

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
span_from(th_span_t s)
{
    if (!s.start_line && !s.end_line) {
        return (ir_span_t){.implied = true};
    }

    return (ir_span_t){
        .start_line = s.start_line,
        .start_col  = s.start_col,
        .end_line   = s.end_line,
        .end_col    = s.end_col,
    };
}

static void
describe(ncc_buffer_t *sb, ir_module_t *m, const th_error_t *e)
{
    ncc_buffer_printf(sb, "%s", e->message ? e->message
                                           : th_err_kind_name(e->kind));

    if (e->constraint) {
        ncc_buffer_printf(sb, " (%s)", e->constraint);
    }

    if (e->expected) {
        const char *want = m ? ir_type_text(m, e->expected) : nullptr;

        ncc_buffer_printf(sb, ": expected %s", want ? want : "?");
    }

    if (e->got) {
        const char *got = m ? ir_type_text(m, e->got) : nullptr;

        ncc_buffer_printf(sb, e->expected ? ", got %s" : ": got %s",
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

        ir_diag_t *dg = ir_diag_begin(d, code_for(e.kind), span_from(e.span),
                                      th_err_kind_name(e.kind));

        if (!dg) {
            continue;
        }

        ncc_buffer_t *sb = ncc_buffer_empty();

        describe(sb, m, &e);

        char *msg = ncc_buffer_take(sb);

        ir_diag_message(dg, d, msg);
        ncc_free(msg);

        if (e.has_related) {
            ir_diag_secondary(d, dg, span_from(e.related), "related");
        }

        made++;
    }

    th_errors_clear(tc);

    return made;
}

static bool
import_types(ir_module_t *m, ir_diags_t *d, void *state)
{
    (void)state;

    if (!m) {
        return false;
    }

    ir_import_type_errors(m, d);

    return true;
}

const ir_pass_t ir_pass_type_diags = {
    .name        = "annot.type-diags",
    .requires    = IR_PROP_TREE | IR_PROP_SCOPED | IR_PROP_SYMBOLS,
    .establishes = 0,
    .run         = import_types,
};
