#include "ir_internal.h"

ir_span_t ir_span_make(int32_t file, uint32_t sb, uint32_t eb, uint32_t sl,
                       uint32_t sc, uint32_t el, uint32_t ec)
{
    return (ir_span_t){
        .file       = file,
        .start_byte = sb,
        .end_byte   = eb,
        .start_line = sl,
        .start_col  = sc,
        .end_line   = el,
        .end_col    = ec,
    };
}

bool ir_span_known(ir_span_t s)
{
    return s.file >= 0;
}

ir_origin_t *ir_origin_new(ir_ctx_t *ctx, ir_origin_kind_t kind,
                           ir_span_t span, const char *agent,
                           ir_origin_t *parent)
{
    if (!ctx) {
        return nullptr;
    }

    if (kind != IR_ORIGIN_SOURCE && !agent) {
        return nullptr;
    }

    const ir_origin_t *slow = parent;
    const ir_origin_t *fast = parent;

    while (fast && fast->parent) {
        slow = slow->parent;
        fast = fast->parent->parent;

        if (slow == fast) {
            return nullptr;
        }
    }

    ir_origin_t *o = ir_alloc(ctx, sizeof(ir_origin_t));

    if (!o) {
        return nullptr;
    }

    o->kind   = kind;
    o->span   = span;
    o->agent  = agent ? ir_strdup(ctx, agent) : nullptr;
    o->parent = parent;

    return o;
}

uint32_t ir_origin_depth(const ir_origin_t *o)
{
    uint32_t n = 0;

    for (const ir_origin_t *p = o; p; p = p->parent) {
        n++;
    }

    return n;
}
