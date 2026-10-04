#include "private/ir/internal.h"
#include "ir/tree_pass.h"
#include "ir/typelang.h"
#include "ir/diag.h"

#include "parse/symtab.h"
#include "parse/tree_util.h"
#include "lib/buffer.h"

static void
say(ir_diags_t *d, ir_diagcode_t code, ir_span_t span, const char *label,
    const char *fmt, core_string_t name)
{
    ir_diag_t *dg = ir_diag_begin(d, code, span, label);

    if (!dg) {
        return;
    }

    core_buffer_t *sb = core_buffer_empty();

    core_buffer_printf(sb, fmt, name.data ? name.data : "?");

    char *msg = core_buffer_take(sb);

    ir_diag_message(dg, d, msg);
    core_free(msg);
}

static ir_span_t
span_of(parse_tree_t *node)
{
    parse_token_info_t *tok = node ? parse_tree_find_first_terminal(node)
                                 : nullptr;

    if (!tok) {
        return (ir_span_t){.implied = true};
    }

    return (ir_span_t){
        .start_byte = (uint32_t)tok->start_byte,
        .end_byte   = (uint32_t)tok->end_byte,
        .start_line = tok->line,
        .start_col  = tok->column,
        .end_line   = tok->endline ? tok->endline : tok->line,
        .end_col    = tok->endcol,
    };
}

static bool
symcheck(ir_module_t *m, ir_diags_t *d, void *state)
{
    parse_annot_result_t *r = ir_module_tree_annots(m);
    parse_symtab_t       *st = ir_module_symtab(m);

    (void)state;

    if (!st) {
        return false;
    }

    for (parse_scope_t *s = st->all_scopes; s; s = s->all_next) {
        for (parse_sym_entry_t *e = s->first_in_scope; e; e = e->next_in_scope) {
            if (e->name.data && e->name.u8_bytes && e->name.data[0] == '$') {
                continue;
            }

            if (!e->uses) {
                say(d,
                    IR_DIAG_UNUSED_SYMBOL,
                    span_of(e->decl_node),
                    "declared here",
                    "%s is declared and never used",
                    e->name);
            }

            if (e->shadowed) {
                say(d,
                    IR_DIAG_SHADOWED_SYMBOL,
                    span_of(e->decl_node),
                    "shadows an outer declaration",
                    "%s shadows a declaration in an enclosing scope",
                    e->name);
            }
        }
    }

    for (size_t i = 0; r && i < core_list_len(r->undefined); i++) {
        parse_tree_t *use = r->undefined.data[i];
        parse_nt_node_t *node = use && !core_tree_is_leaf(use)
            ? &core_tree_node_value(use) : nullptr;
        core_string_t name = node && node->unresolved_name.data
            ? node->unresolved_name
            : node && node->sym ? node->sym->name : (core_string_t){0};

        say(d,
            IR_DIAG_UNDEFINED_SYMBOL,
            span_of(use),
            "used here",
            "%s is used but never declared",
            name.data ? name : CORE_STRING_STATIC("<unknown>"));
    }

    return true;
}

const ir_pass_t ir_pass_symcheck = {
    .name        = "annot.symcheck",
    .requires    = IR_PROP_TREE | IR_PROP_SCOPED | IR_PROP_SYMBOLS
                 | IR_PROP_CFLOW,
    .establishes = 0,
    .run         = symcheck,
};
