#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"
#include "ir/grammar.h"
#include "ir/layout.h"
#include "ir/pass.h"
#include "ir/text.h"
#include "ir/typelang.h"

#include "parse/pwz.h"
#include "parse/tree_util.h"
#include "parse/tree_visit.h"
#include "internal/parse/grammar_internal.h"
#include "scanner/token_stream.h"

typedef enum : uint8_t {
    IB_NAME,
    IB_REF,
    IB_NUM,
    IB_INIT,
    IB_LAYVAL,
    IB_META,
    IB_DEST,
    IB_OPERAND,
    IB_INSTR,
    IB_BLOCK,
    IB_LOCAL,
    IB_PARAM,
} ib_kind;

typedef enum : uint8_t {
    OP_REF,
    OP_NUM,
    OP_STR,
    OP_NAMED,
    OP_PHI,
    OP_SWITCH,
    OP_DEFAULT,
    OP_TYPE,
    OP_WORD,
} opd_how;

typedef enum : uint8_t {
    MT_LOC,
    MT_ORIGIN,
    MT_ANNOT,
} meta_kind;

typedef struct ib_node ib_node;

struct ib_node {
    ib_kind   kind;
    ir_span_t span;

    const char *text;
    const char *text2;
    th_type    *type;

    ir_lit_t     lit;
    double       fval;
    bool         is_float;
    bool         over;
    bool         neg;
    bool         present;
    uint64_t        uval;
    ir_layout_val_t lay;
    ir_valkind_t    refkind;

    opd_how          how;
    meta_kind        mkind;
    ir_origin_kind_t okind;
    ir_annot_t       annot;
    ir_span_t        loc;
    ib_node         *a;
    ib_node         *b;

    ncc_reduce_list_t *l1;
    ncc_reduce_list_t *l2;
    ncc_reduce_list_t *l3;

    ir_block_t       *made_block;
    ir_instr_t       *made_instr;
    ncc_parse_tree_t *node;
};

typedef struct {
    ir_ctx_t             *ctx;
    ir_module_t          *m;
    ir_diags_t           *d;
    int32_t               file;
    bool                  failed;
    ir_typelang_lower_t  *types;

    const ir_profile_t *const *profiles;
    size_t                     n_profiles;
} B;

static void bfail_as(B *b, ir_diagcode_t code, ir_span_t at, const char *what)
{
    if (b->failed) {
        return;
    }
    b->failed = true;

    if (!b->d) {
        return;
    }

    ir_diag_t *dg = ir_diag_begin(b->d, code, at, what);

    if (dg) {
        ir_diag_message(dg, b->d, what);
    }
}

static void bfail(B *b, ir_span_t at, const char *what)
{
    bfail_as(b, IR_DIAG_PARSE, at, what);
}

static ir_span_t tok_span(B *b, const ncc_token_info_t *t)
{
    if (!t) {
        return IR_SPAN_UNKNOWN;
    }

    uint32_t  w = t->endcol > t->column ? t->endcol - t->column : 1;
    ir_span_t s = ir_span_make(b->file, (uint32_t)t->start_byte,
                               (uint32_t)t->end_byte, t->line, t->column,
                               t->line, t->column + w);

    s.implied = true;

    return s;
}

static ib_node *nd(B *b, ib_kind kind, ir_span_t span)
{
    ib_node *n = ir_alloc(b->ctx, sizeof(ib_node));

    n->kind = kind;
    n->span = span;

    return n;
}

static ncc_token_info_t *edge_tok(ncc_parse_tree_t *n, bool last)
{
    if (!n) {
        return nullptr;
    }

    if (ncc_tree_is_leaf(n)) {
        ncc_token_info_t *t = ncc_parse_node_token(n);

        return t && t->tid == ir_grammar_tid("NEWLINE") ? nullptr : t;
    }

    size_t k = ncc_tree_num_children(n);

    for (size_t i = 0; i < k; i++) {
        ncc_token_info_t *t = edge_tok(ncc_tree_child(n, last ? k - 1 - i : i),
                                       last);

        if (t) {
            return t;
        }
    }

    return nullptr;
}

static ir_span_t cover(ncc_reduce_t *r)
{
    B                *b = r->thunk;
    ncc_token_info_t *a = edge_tok(r->node, false);
    ncc_token_info_t *z = edge_tok(r->node, true);

    if (!a) {
        return IR_SPAN_UNKNOWN;
    }

    return ir_span_cover(tok_span(b, a), tok_span(b, z ? z : a));
}

static void *unwrap(void *v)
{
    ncc_reduce_list_t *l = ncc_reduce_as_list(v);

    while (l && l->n == 1) {
        v = l->v[0];
        l = ncc_reduce_as_list(v);
    }

    return v;
}

static bool nt_named(ncc_grammar_t *g, int64_t id, const char *role)
{
    ncc_nonterm_t *nt = ncc_get_nonterm(g, id);

    return nt && nt->name.data && !strcmp(nt->name.data, role);
}

static bool match_holds(ncc_grammar_t *g, const ncc_match_t *m,
                        const char *role, int depth, int limit);

static bool holds_role(ncc_grammar_t *g, int64_t id, const char *role,
                       int depth, int limit)
{
    ncc_nonterm_t *nt = ncc_get_nonterm(g, id);

    if (!nt || depth > limit) {
        return false;
    }

    for (size_t i = 0; i < ncc_list_len(nt->rule_ids); i++) {
        ncc_parse_rule_t *rule = ncc_get_rule(g, nt->rule_ids.data[i]);

        for (size_t k = 0; rule && k < ncc_list_len(rule->contents); k++) {
            if (match_holds(g, &rule->contents.data[k], role, depth,
                            limit)) {
                return true;
            }
        }
    }

    return false;
}

static bool match_holds(ncc_grammar_t *g, const ncc_match_t *m,
                        const char *role, int depth, int limit)
{
    if (depth > limit) {
        return false;
    }

    if (m->kind == NCC_MATCH_NT) {
        return nt_named(g, m->nt_id, role)
            || holds_role(g, m->nt_id, role, depth + 1, limit);
    }

    if (m->kind == NCC_MATCH_GROUP && m->group) {
        int64_t inner = ((const ncc_rule_group_t *)m->group)->contents_id;

        if (depth + 1 > limit) {
            return false;
        }

        return nt_named(g, inner, role)
            || holds_role(g, inner, role, depth + 1, limit);
    }

    return false;
}

static size_t slot_of(ncc_reduce_t *r, const char *role)
{
    ncc_nt_node_t    *pn   = &ncc_tree_node_value(r->node);
    ncc_parse_rule_t *rule = ncc_get_node_rule(r->grammar, pn);

    for (int limit = 0; limit <= 3; limit++) {
        for (size_t i = 0; rule && i < ncc_list_len(rule->contents); i++) {
            if (match_holds(r->grammar, &rule->contents.data[i], role, 0,
                            limit)) {
                return i;
            }
        }
    }

    return SIZE_MAX;
}

static ib_node *leaf_ib(B *b, ncc_parse_tree_t *c);
static ib_node *kid(ncc_reduce_t *r, size_t i);

static ib_node *at(ncc_reduce_t *r, const char *role)
{
    return kid(r, slot_of(r, role));
}

static ib_node *nth_named(ncc_reduce_t *r, const char *role, size_t nth)
{
    size_t seen = 0;

    for (size_t i = 0; i < r->n_kids; i++) {
        if (ncc_tree_is_nt(r->grammar, ncc_tree_child(r->node, i), role)
            && seen++ == nth) {
            return kid(r, i);
        }
    }

    return nullptr;
}

static ib_node *second_name(ncc_reduce_t *r)
{
    return nth_named(r, "name", 1);
}

static ncc_reduce_list_t *tail_group(ncc_reduce_t *r)
{
    for (size_t i = r->n_kids; i-- > 0;) {
        ncc_reduce_list_t *l = ncc_reduce_as_list(r->kids[i]);

        if (l) {
            return l;
        }
    }

    return nullptr;
}

static bool has_minus(ncc_reduce_t *r)
{
    for (size_t i = 0; i < r->n_kids; i++) {
        ncc_reduce_list_t *l = ncc_reduce_as_list(r->kids[i]);
        ib_node           *n = l && l->n == 1 ? unwrap(l->v[0]) : nullptr;

        if (n && n->text && !strcmp(n->text, "-")) {
            return true;
        }
    }

    return false;
}

static bool subtree_has_minus(ncc_parse_tree_t *n, int depth)
{
    if (!n || depth > 4) {
        return false;
    }

    if (ncc_tree_is_leaf(n)) {
        return ncc_tree_leaf_tid(n) == '-';
    }

    for (size_t i = 0; i < ncc_tree_num_children(n); i++) {
        if (subtree_has_minus(ncc_tree_child(n, i), depth + 1)) {
            return true;
        }
    }

    return false;
}

static bool has_minus_tok(ncc_reduce_t *r)
{
    for (size_t i = 0; i < r->n_kids; i++) {
        ncc_parse_tree_t *c = ncc_tree_child(r->node, i);

        if (c && !ncc_tree_is_nt(r->grammar, c, "number")
            && !ncc_tree_is_nt(r->grammar, c, "int")
            && subtree_has_minus(c, 0)) {
            return true;
        }
    }

    return false;
}

static ib_node *str_kid(ncc_reduce_t *r)
{
    for (size_t i = 0; i < r->n_kids; i++) {
        ncc_parse_tree_t *c = ncc_tree_child(r->node, i);

        if (ncc_tree_is_leaf(c)
            && ncc_tree_leaf_tid(c) == ir_grammar_tid("STRING_LIT")) {
            return leaf_ib(r->thunk, c);
        }
    }

    return nullptr;
}

static void *raw_at(ncc_reduce_t *r, const char *role)
{
    size_t i = slot_of(r, role);

    return i < r->n_kids ? unwrap(r->kids[i]) : nullptr;
}

static ncc_reduce_list_t *rep(ncc_reduce_t *r, const char *role)
{
    size_t i = slot_of(r, role);

    return i < r->n_kids ? ncc_reduce_as_list(r->kids[i]) : nullptr;
}

static ib_node *leaf_ib(B *b, ncc_parse_tree_t *c)
{
    ncc_token_info_t *t = ncc_parse_node_token(c);
    ncc_string_t      s = ncc_tree_leaf_text(c);
    ib_node          *n = nd(b, IB_NAME,
                             t && t->tid == ir_grammar_tid("NEWLINE")
                               ? IR_SPAN_UNKNOWN
                               : tok_span(b, t));

    if (s.data) {
        n->text = ir_strdup(b->ctx, s.data);
    }
    else if (t && t->tid > 0 && t->tid < 128) {
        char one[2] = {(char)t->tid, 0};

        n->text = ir_strdup(b->ctx, one);
    }

    return n;
}

static ib_node *kid(ncc_reduce_t *r, size_t i)
{
    if (i >= r->n_kids) {
        return nullptr;
    }

    ncc_parse_tree_t *c = ncc_tree_child(r->node, i);

    if (c && ncc_tree_is_leaf(c)) {
        return leaf_ib(r->thunk, c);
    }

    return (ib_node *)unwrap(r->kids[i]);
}

static size_t sep_count(ncc_reduce_list_t *l)
{
    return l ? l->n / 2 : 0;
}

static ib_node *sep_at(ncc_reduce_list_t *l, size_t k)
{
    return l && 2 * k + 1 < l->n ? (ib_node *)unwrap(l->v[2 * k + 1])
                                 : nullptr;
}

static th_type *type_of(B *b, void *v, ir_span_t where)
{
    th_type *t = ir_typelang_type_of(unwrap(v));

    if (!t) {
        bfail(b, where, "expected a type");

        return nullptr;
    }

    return ir_type_intern(b->m, t);
}

static void attach_comment(B *b, ncc_parse_tree_t *node, ir_annots_t *a)
{
    ncc_token_info_t *t = ncc_tree_find_first_terminal(node);

    if (!t || !t->leading_trivia) {
        return;
    }

    ir_sb sb = {0};

    for (ncc_trivia_t *tv = t->leading_trivia; tv; tv = tv->next) {
        const char *s = tv->text.data;

        if (!s || *s != '#') {
            continue;
        }
        s++;

        while (*s == ' ') {
            s++;
        }

        if (sb.len) {
            ir_sb_str(&sb, "\n");
        }
        ir_sb_str(&sb, s);
    }

    char *msg = ir_sb_take(&sb);

    if (msg && *msg) {
        ir_annot_set(b->ctx, a,
                     (ir_annot_t){.key  = "ir.comment",
                                  .kind = IR_ANNOT_STR,
                                  .s    = ir_strdup(b->ctx, msg)});
    }
    free(msg);
}

static bool decode_int(const char *s, ir_lit_t *out, bool *over)
{
    ir_uint_t acc = 0;
    bool      hex = s[0] == '0' && (s[1] == 'x' || s[1] == 'X');

    *over = false;

    if (hex) {
        for (const char *p = s + 2; *p; p++) {
            unsigned d = *p >= '0' && *p <= '9'   ? (unsigned)(*p - '0')
                       : *p >= 'a' && *p <= 'f'   ? (unsigned)(*p - 'a' + 10)
                       : *p >= 'A' && *p <= 'F'   ? (unsigned)(*p - 'A' + 10)
                                                  : 16u;

            if (d == 16u) {
                return false;
            }

            if (acc >> 124) {
                *over = true;
            }
            acc = acc * 16 + d;
        }
        *out = ir_lit_hex(acc);

        return true;
    }

    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }

        unsigned d = (unsigned)(*p - '0');

        if (acc > (~(ir_uint_t)0 - d) / 10) {
            *over = true;
            break;
        }
        acc = acc * 10 + d;
    }
    *out = (ir_lit_t){.mag = acc};

    return true;
}

static void *r_name(ncc_reduce_t *r)
{
    return kid(r, 0);
}

static void *r_signed_int(ncc_reduce_t *r)
{
    B       *b = r->thunk;
    ib_node *k = at(r, "int");
    ib_node *n = nd(b, IB_NUM, cover(r));

    if (k) {
        n->lit     = k->lit;
        n->lit.neg = has_minus_tok(r);
        n->over    = k->over;
    }

    return n;
}

static void *r_int(ncc_reduce_t *r)
{
    B       *b = r->thunk;
    ib_node *k = kid(r, 0);
    ib_node *n = nd(b, IB_NUM, k ? k->span : IR_SPAN_UNKNOWN);

    if (!k || !k->text || !decode_int(k->text, &n->lit, &n->over)) {
        bfail(b, n->span, "expected an integer literal");
    }

    return n;
}

static void *r_number(ncc_reduce_t *r)
{
    B                *b = r->thunk;
    ib_node          *k = kid(r, 0);
    ncc_parse_tree_t *c = ncc_tree_child(r->node, 0);
    ib_node          *n = nd(b, IB_NUM, k ? k->span : IR_SPAN_UNKNOWN);

    n->text = k ? k->text : nullptr;

    if (ncc_tree_is_leaf(c)
        && ncc_tree_leaf_tid(c) == ir_grammar_tid("FLOAT_LIT")) {
        n->is_float = true;

        return n;
    }

    if (!n->text || !decode_int(n->text, &n->lit, &n->over)) {
        bfail(b, n->span, "expected a number");
    }

    return n;
}

static void *ref_of(ncc_reduce_t *r, ir_valkind_t kind)
{
    B       *b = r->thunk;
    ib_node *k = at(r, "name");
    ib_node *n = nd(b, IB_REF, cover(r));

    n->text    = k ? k->text : nullptr;
    n->refkind = kind;

    return n;
}

static void *r_value(ncc_reduce_t *r)
{
    return ref_of(r, IR_VAL_INSTR);
}

static void *r_local_ref(ncc_reduce_t *r)
{
    return ref_of(r, IR_VAL_LOCAL);
}

static void *r_global(ncc_reduce_t *r)
{
    return ref_of(r, IR_VAL_GLOBAL);
}

static void tokens_into(ir_sb *sb, ncc_parse_tree_t *n, size_t *count)
{
    if (!n) {
        return;
    }

    if (ncc_tree_is_leaf(n)) {
        ncc_token_info_t *t = ncc_parse_node_token(n);
        ncc_string_t      s = ncc_tree_leaf_text(n);

        if (s.data) {
            ir_sb_str(sb, s.data);
        }
        else if (t && t->tid > 0 && t->tid < 128) {
            char one[2] = {(char)t->tid, 0};

            ir_sb_str(sb, one);
        }
        (*count)++;

        return;
    }

    for (size_t i = 0; i < ncc_tree_num_children(n); i++) {
        tokens_into(sb, ncc_tree_child(n, i), count);
    }
}

static void *r_dotted(ncc_reduce_t *r)
{
    B      *b  = r->thunk;
    ir_sb   sb = {0};
    ib_node *n = nd(b, IB_NAME, cover(r));
    size_t  nt = 0;

    tokens_into(&sb, r->node, &nt);

    char *s = ir_sb_take(&sb);

    n->text    = s ? ir_strdup(b->ctx, s) : nullptr;
    n->present = nt > 1;
    free(s);

    return n;
}

static void *r_profile(ncc_reduce_t *r)
{
    (void)r;

    return nullptr;
}

static void *r_target(ncc_reduce_t *r)
{
    B       *b = r->thunk;
    ib_node *t = at(r, "dotted-name");

    if (b->m->target) {
        bfail(b, cover(r), "a module names its target once");
    }
    else if (t && t->text) {
        ir_module_set_target(b->m, t->text);
    }

    return nullptr;
}

static ncc_parse_tree_t *nt_child(ncc_grammar_t *g, ncc_parse_tree_t *n,
                                  const char *name)
{
    return ncc_tree_find_child_by_nt_name(g, n, ncc_string_from_cstr(name));
}

static ncc_parse_tree_t *find_profile(ncc_grammar_t *g, ncc_parse_tree_t *mod)
{
    for (size_t i = 0; i < ncc_tree_num_children(mod); i++) {
        ncc_parse_tree_t *c = ncc_tree_child(mod, i);

        if (ncc_tree_is_leaf(c)) {
            continue;
        }

        for (size_t k = 0; k < ncc_tree_num_children(c); k++) {
            ncc_parse_tree_t *hit = nt_child(g, ncc_tree_child(c, k),
                                             "profile-decl");

            if (hit) {
                return hit;
            }
        }
    }

    return nullptr;
}

static bool open_module(ncc_visit_t *v)
{
    B                *b    = v->thunk;
    ncc_string_t      name = ncc_tree_extract_first_identifier(
        nt_child(v->grammar, v->node, "name"));
    ncc_parse_tree_t *pd   = find_profile(v->grammar, v->node);

    if (!pd) {
        bfail(b, IR_SPAN_UNKNOWN, "a module must name its profile");
        ncc_tree_walk_stop(v->walk);

        return false;
    }

    ncc_string_t want = ncc_tree_extract_first_identifier(
        nt_child(v->grammar, pd, "name"));

    for (size_t i = 0; want.data && i < b->n_profiles; i++) {
        if (!strcmp(b->profiles[i]->name, want.data)) {
            b->m = ir_module_new(b->ctx, name.data ? name.data : "",
                                 b->profiles[i], nullptr);
            break;
        }
    }

    if (!b->m) {
        bfail(b, tok_span(b, ncc_tree_find_first_terminal(
                                 nt_child(v->grammar, pd, "name"))),
              "unknown profile");
        ncc_tree_walk_stop(v->walk);

        return false;
    }

    return true;
}

static void *r_type_decl(ncc_reduce_t *r)
{
    B       *b    = r->thunk;
    ib_node *name = at(r, "name");
    th_type *def  = type_of(b, raw_at(r, "typelang-type"), cover(r));

    if (!name || !name->text || !def) {
        return nullptr;
    }

    th_type *named = ir_type_parse(b->m, name->text);

    if (!named || !ir_type_define(b->m, named, def)) {
        bfail(b, cover(r), "this type is already defined");
    }

    return nullptr;
}

static void *r_module(ncc_reduce_t *r)
{
    B *b = r->thunk;

    if (!b->m) {
        return nullptr;
    }

    ir_module_set_span(b->m, cover(r));
    attach_comment(b, r->node, ir_module_annots(b->m));

    if (!b->m->profile) {
        bfail(b, cover(r), "a module must name its profile");
    }

    return nullptr;
}

static void *r_linkage(ncc_reduce_t *r)
{
    return kid(r, 0);
}

static void *r_mode(ncc_reduce_t *r)
{
    return kid(r, 0);
}

static void *r_modifier(ncc_reduce_t *r)
{
    return kid(r, 0);
}

static void *r_init(ncc_reduce_t *r)
{
    B       *b   = r->thunk;
    ib_node *num = at(r, "number");
    ib_node *n   = nd(b, IB_INIT, cover(r));

    n->present  = true;
    n->neg      = has_minus(r);
    n->is_float = num && num->is_float;
    n->text     = num ? num->text : nullptr;

    if (num && !num->is_float) {
        n->lit     = num->lit;
        n->lit.neg = n->neg;
        n->over    = num->over;
    }

    return n;
}

static void apply_init(B *b, ib_node *init, bool *has, ir_lit_t *lit,
                       double *f, const char **text, bool *is_float)
{
    if (!init || !init->present) {
        return;
    }

    if (init->over) {
        bfail(b, init->span, "this literal does not fit in 128 bits");

        return;
    }
    *has = true;

    if (init->is_float) {
        *is_float = true;
        *text     = init->text;
        *f        = init->text ? strtod(init->text, nullptr) : 0;
        *f        = init->neg ? -*f : *f;

        return;
    }
    *lit = init->lit;
}

static void *r_symbolic(ncc_reduce_t *r)
{
    B            *b = r->thunk;
    ib_node      *n = nd(b, IB_LAYVAL, cover(r));
    ib_node      *w = kid(r, 0);
    ib_node      *f = str_kid(r);
    th_type      *t = type_of(b, raw_at(r, "typelang-type"), cover(r));
    ir_symkind_t  k;

    if (!t || !w || !w->text || !ir_symkind_from_name(w->text, &k)) {
        bfail(b, cover(r), "expected a symbolic layout value");

        return n;
    }

    n->lay = (ir_layout_val_t){.state    = IR_LAYOUT_SYMBOLIC,
                               .authored = true,
                               .sym      = k,
                               .of       = t,
                               .member   = f ? f->text : nullptr};

    return n;
}

static void *r_layout_val(ncc_reduce_t *r)
{
    B       *b = r->thunk;
    ib_node *k = kid(r, 0);

    if (k && k->kind == IB_LAYVAL) {
        return k;
    }

    ib_node *n = nd(b, IB_LAYVAL, k ? k->span : IR_SPAN_UNKNOWN);

    if (k && k->kind == IB_NUM && !k->over && !k->lit.neg) {
        n->lay = (ir_layout_val_t){.state    = IR_LAYOUT_PINNED,
                                   .authored = true,
                                   .value    = (uint64_t)k->lit.mag};
    }
    else {
        bfail(b, n->span, "expected a layout value");
    }

    return n;
}

static ir_props_t prop_named(B *b, const char *s, ir_span_t at)
{
    static const ir_props_t ALL[] = {
        IR_PROP_PARSED,     IR_PROP_TYPED,        IR_PROP_SSA,
        IR_PROP_STRUCTURED, IR_PROP_LAID_OUT,     IR_PROP_NO_CRIT_EDGE,
        IR_PROP_VERIFIED,
    };

    for (size_t i = 0; i < sizeof(ALL) / sizeof(ALL[0]); i++) {
        const char *n = ir_prop_name(ALL[i]);

        if (n && !strcmp(n, s)) {
            return ALL[i];
        }
    }
    bfail(b, at, "unknown property in an annotation");

    return 0;
}

static void *r_ns_key(ncc_reduce_t *r)
{
    B       *b  = r->thunk;
    ib_node *ns = kid(r, 0);
    ib_node *k  = second_name(r);
    ib_node *n  = nd(b, IB_NAME, cover(r));
    ir_sb    sb = {0};

    ir_sb_str(&sb, ns && ns->text ? ns->text : "");
    ir_sb_str(&sb, ".");
    ir_sb_str(&sb, k && k->text ? k->text : "");

    char *s = ir_sb_take(&sb);

    n->text = s ? ir_strdup(b->ctx, s) : nullptr;
    free(s);

    return n;
}

static void *r_provenance(ncc_reduce_t *r)
{
    return at(r, "name");
}

static void *r_annot_value(ncc_reduce_t *r)
{
    B       *b = r->thunk;
    ib_node *k = kid(r, 0);
    th_type *t = r->n_kids ? ir_typelang_type_of(unwrap(r->kids[0]))
                           : nullptr;
    ib_node *n = nd(b, IB_META, cover(r));

    n->mkind = MT_ANNOT;

    if (t) {
        n->annot.kind = IR_ANNOT_TYPE;
        n->annot.t    = ir_type_intern(b->m, t);

        return n;
    }

    ib_node *num = at(r, "int");
    ib_node *ref = at(r, "ref");
    ib_node *str = str_kid(r);

    if (num) {
        n->annot.kind  = IR_ANNOT_INT;
        n->annot.i     = num->lit;
        n->annot.i.neg = has_minus_tok(r);

        return n;
    }

    if (ref) {
        n->annot.kind = IR_ANNOT_VALUE;
        n->text       = ref->text;
        n->refkind    = ref->refkind;

        return n;
    }

    if (str) {
        n->annot.kind = IR_ANNOT_STR;
        n->annot.s    = str->text;

        return n;
    }

    if (!k) {
        return n;
    }

    if (k->text && !strcmp(k->text, "true")) {
        n->annot.kind = IR_ANNOT_BOOL;
        n->annot.b    = true;
    }
    else if (k->text && !strcmp(k->text, "false")) {
        n->annot.kind = IR_ANNOT_BOOL;
        n->annot.b    = false;
    }
    else {
        n->annot.kind = IR_ANNOT_STR;
        n->annot.s    = k->text;
    }

    return n;
}

static void *r_annot(ncc_reduce_t *r)
{
    B       *b   = r->thunk;
    ib_node *key = at(r, "ns-key");
    ib_node *val = at(r, "annot-value");
    ib_node *n   = nd(b, IB_META, cover(r));

    n->mkind = MT_ANNOT;

    if (val) {
        n->annot   = val->annot;
        n->text2   = val->text;
        n->refkind = val->refkind;
    }
    n->annot.key = key ? key->text : nullptr;

    ncc_reduce_list_t *prov = rep(r, "provenance");

    if (prov && prov->n) {
        ib_node *p = prov->v[0];

        if (p && p->text) {
            n->annot.derived_from = prop_named(b, p->text, cover(r));
        }
    }

    return n;
}

static void *r_loc(ncc_reduce_t *r)
{
    B       *b = r->thunk;
    ib_node *n = nd(b, IB_META, cover(r));
    int64_t  v[7] = {0};
    size_t   got  = 0;

    n->mkind = MT_LOC;

    ib_node *first = at(r, "signed-int");

    if (first) {
        v[got++] = ir_lit_signed(first->lit);
    }

    ncc_reduce_list_t *rest = tail_group(r);

    for (size_t i = 0; i < sep_count(rest) && got < 7; i++) {
        ib_node *num = sep_at(rest, i);

        if (num) {
            v[got++] = ir_lit_signed(num->lit);
        }
    }

    if (got != 7) {
        bfail(b, cover(r), "a location takes seven numbers");

        return n;
    }
    n->loc = ir_span_make((int32_t)v[0], (uint32_t)v[1], (uint32_t)v[2],
                          (uint32_t)v[3], (uint32_t)v[4], (uint32_t)v[5],
                          (uint32_t)v[6]);

    return n;
}

static void *r_origin(ncc_reduce_t *r)
{
    B       *b    = r->thunk;
    ib_node *n    = nd(b, IB_META, cover(r));
    ib_node *k    = at(r, "origin-kind");
    ib_node *ag   = str_kid(r);
    int64_t  v[7] = {0};
    size_t   got  = 0;

    n->mkind = MT_ORIGIN;

    if (!k || !k->text || !ir_origin_kind_from_name(k->text, &n->okind)) {
        bfail(b, cover(r), "unknown origin kind");

        return n;
    }

    ncc_reduce_list_t *nums = tail_group(r);

    for (size_t i = 0; i < sep_count(nums) && got < 7; i++) {
        ib_node *num = sep_at(nums, i);

        if (num) {
            v[got++] = ir_lit_signed(num->lit);
        }
    }

    if (got != 7) {
        bfail(b, cover(r), "an origin takes seven numbers");

        return n;
    }
    n->loc  = ir_span_make((int32_t)v[0], (uint32_t)v[1], (uint32_t)v[2],
                           (uint32_t)v[3], (uint32_t)v[4], (uint32_t)v[5],
                           (uint32_t)v[6]);
    n->text = ag && ag->text && *ag->text ? ag->text : nullptr;

    return n;
}

static void apply_metas(B *b, ncc_reduce_list_t *metas, ir_func_t *f,
                        ir_span_t *span, ir_origin_t **origin,
                        ir_annots_t *annots)
{
    for (size_t i = 0; metas && i < metas->n; i++) {
        ib_node *mt = metas->v[i];

        if (!mt || mt->kind != IB_META) {
            continue;
        }

        switch (mt->mkind) {
        case MT_LOC:
            if (span) {
                *span = mt->loc;
            }
            break;

        case MT_ORIGIN:
            break;

        case MT_ANNOT: {
            ir_annot_t a = mt->annot;

            if (a.kind == IR_ANNOT_VALUE) {
                a.v = f ? ir_value_find(f, mt->text2) : nullptr;

                if (!a.v) {
                    bfail(b, mt->span,
                          "an annotation may only name a value already "
                          "defined in the enclosing function");

                    return;
                }
            }

            if (!ir_annot_key_ok(a.key,
                                 b->m->profile ? b->m->profile->name : "")) {
                bfail(b, mt->span, "this annotation key is not namespaced");

                return;
            }
            ir_annot_set(b->ctx, annots, a);
            break;
        }
        }
    }

    if (!origin) {
        return;
    }

    for (size_t i = metas ? metas->n : 0; i-- > 0;) {
        ib_node *mt = unwrap(metas->v[i]);

        if (mt && mt->kind == IB_META && mt->mkind == MT_ORIGIN) {
            *origin = ir_origin_new(b->ctx, mt->okind, mt->loc, mt->text,
                                    *origin);
        }
    }
}

static void *r_meta_decl(ncc_reduce_t *r)
{
    B       *b  = r->thunk;
    ib_node *an = at(r, "annot");

    if (an && an->kind == IB_META && an->mkind == MT_ANNOT) {
        apply_metas(b, nullptr, nullptr, nullptr, nullptr, nullptr);

        if (!ir_annot_key_ok(an->annot.key,
                             b->m->profile ? b->m->profile->name : "")) {
            bfail(b, cover(r), "this annotation key is not namespaced");

            return nullptr;
        }
        ir_annot_set(b->ctx, ir_module_annots(b->m), an->annot);
    }

    return nullptr;
}

static bool is_word(ib_node *n, const char *w)
{
    return n && n->text && !strcmp(n->text, w);
}

static void *r_global_decl(ncc_reduce_t *r)
{
    B                 *b    = r->thunk;
    ncc_reduce_list_t *link = rep(r, "linkage");
    ib_node           *name = at(r, "global");
    th_type           *t    = type_of(b, raw_at(r, "typelang-type"), cover(r));

    if (!name || !name->text || !t) {
        return nullptr;
    }

    ir_global_t *g = ir_global_add(b->m, name->text, t);

    if (!g) {
        bfail(b, cover(r), "duplicate global");

        return nullptr;
    }

    if (link && link->n) {
        g->is_static = is_word(link->v[0], "static");
        g->is_extern = is_word(link->v[0], "extern");
    }

    ncc_reduce_list_t *init = rep(r, "init");

    if (init && init->n) {
        apply_init(b, init->v[0], &g->has_init, &g->init, &g->init_f,
                   &g->init_text, &g->init_is_float);
    }
    g->span = cover(r);
    apply_metas(b, rep(r, "meta"), nullptr, &g->span, &g->origin, &g->annots);

    return nullptr;
}

static void *r_layout_item(ncc_reduce_t *r)
{
    B       *b    = r->thunk;
    ib_node *head = kid(r, 0);
    ib_node *n    = nd(b, IB_LAYVAL, cover(r));
    size_t   nv   = 0;

    n->text = head ? head->text : nullptr;
    n->span = head ? head->span : n->span;

    for (size_t i = 0; i < r->n_kids && nv < 2; i++) {
        ib_node *e = kid(r, i);

        if (e && e->kind == IB_LAYVAL && e != head) {
            if (nv++) {
                n->b = e;
            }
            else {
                n->a = e;
            }
        }
    }
    n->present = nv == 2;

    return n;
}

static void *r_layout_decl(ncc_reduce_t *r)
{
    B       *b = r->thunk;
    th_type *t = type_of(b, raw_at(r, "typelang-type"), cover(r));

    if (!t) {
        return nullptr;
    }

    ncc_reduce_list_t *items = rep(r, "layout-item");
    ir_descriptor_t   *d     = ir_layout_ensure(b->m, t, nullptr);

    if (!d) {
        bfail(b, cover(r), "this type has no descriptor");

        return nullptr;
    }

    for (size_t i = 0; items && i < items->n; i++) {
        ib_node *it = unwrap(items->v[i]);

        if (!it || it->kind != IB_LAYVAL || !it->a) {
            continue;
        }

        ib_node *head = it;

        if (is_word(head, "size")) {
            d->size = it->a->lay;
        }
        else if (is_word(head, "align")) {
            d->align = it->a->lay;
        }
        else if (is_word(head, "stride")) {
            d->stride = it->a->lay;
        }
        else if (it->present) {
            ir_layout_val_t lo = it->a->lay;
            ir_layout_val_t hi = it->b->lay;

            if (lo.state != IR_LAYOUT_PINNED || hi.state != IR_LAYOUT_PINNED
                || hi.value < lo.value) {
                bfail(b, head->span,
                      "a member's bit range must be two numbers, low then "
                      "high");
            }
            else if (!ir_layout_pin_member(b->m, t, head->text, lo.value,
                                           hi.value - lo.value)) {
                bfail(b, head->span,
                      "the member's bit range contradicts one already "
                      "pinned");
            }
        }
    }

    return nullptr;
}

static void *r_param(ncc_reduce_t *r)
{
    B       *b = r->thunk;
    ib_node *n = nd(b, IB_PARAM, cover(r));

    if (r->n_kids == 2) {
        n->neg  = true;
        n->type = type_of(b, raw_at(r, "typelang-type"), n->span);

        return n;
    }

    ncc_reduce_list_t *mode = rep(r, "param-mode");
    ib_node           *who  = at(r, "value") ? at(r, "value") : at(r, "name");

    n->text    = who ? who->text : nullptr;
    n->refkind = who && who->kind == IB_REF ? IR_VAL_INSTR : IR_VAL_GLOBAL;
    n->type    = type_of(b, raw_at(r, "typelang-type"), n->span);
    n->uval    = TH_IN;

    if (mode && mode->n) {
        ib_node *md = mode->v[0];

        n->uval = is_word(md, "out")     ? TH_OUT
                : is_word(md, "inout")   ? TH_INOUT
                                         : TH_IN;
    }

    return n;
}

static void *r_dest(ncc_reduce_t *r)
{
    B       *b   = r->thunk;
    ib_node *who = kid(r, 0);
    ib_node *n   = nd(b, IB_DEST, who ? who->span : cover(r));

    n->text    = who ? who->text : nullptr;
    n->refkind = who ? who->refkind : IR_VAL_INSTR;

    if (n->refkind == IR_VAL_INSTR) {
        n->type = type_of(b, raw_at(r, "typelang-type"), n->span);
    }

    return n;
}

static void *r_operand(ncc_reduce_t *r)
{
    B       *b = r->thunk;
    ib_node *k = kid(r, 0);
    th_type *t = r->n_kids ? ir_typelang_type_of(unwrap(r->kids[0]))
                           : nullptr;
    ib_node *n = nd(b, IB_OPERAND, cover(r));

    if (t) {
        ncc_string_t word = ncc_tree_extract_first_identifier(r->node);

        n->how  = OP_TYPE;
        n->type = ir_type_intern(b->m, t);
        n->text = word.data ? ir_strdup(b->ctx, word.data) : nullptr;

        return n;
    }

    if (r->n_kids == 2) {
        ib_node *num = at(r, "number");

        n->how      = OP_NUM;
        n->neg      = true;
        n->is_float = num && num->is_float;
        n->text     = num ? num->text : nullptr;
        n->over     = num && num->over;

        if (num && !num->is_float) {
            n->lit     = num->lit;
            n->lit.neg = true;
        }

        return n;
    }

    ib_node *str = str_kid(r);

    if (str) {
        n->how  = OP_STR;
        n->text = str->text;

        return n;
    }

    if (!k) {
        return n;
    }

    switch (k->kind) {
    case IB_REF:
        n->how     = OP_REF;
        n->text    = k->text;
        n->refkind = k->refkind;
        break;

    case IB_NUM:
        n->how      = OP_NUM;
        n->lit      = k->lit;
        n->is_float = k->is_float;
        n->text     = k->text;
        n->over     = k->over;
        break;

    case IB_OPERAND:
        return k;

    default:
        n->how  = k->text && k->kind == IB_NAME ? OP_WORD : OP_STR;
        n->text = k->text;
        break;
    }

    return n;
}

static void *r_named_arg(ncc_reduce_t *r)
{
    B       *b   = r->thunk;
    ib_node *nm  = kid(r, 0);
    ib_node *ref = at(r, "ref");
    ib_node *n   = nd(b, IB_OPERAND, cover(r));

    n->how     = OP_NAMED;
    n->text    = nm ? nm->text : nullptr;
    n->text2   = ref ? ref->text : nullptr;
    n->refkind = ref ? ref->refkind : IR_VAL_INSTR;

    return n;
}

static void *r_phi_arm(ncc_reduce_t *r)
{
    B       *b   = r->thunk;
    ib_node *blk = at(r, "name");
    ib_node *ref = at(r, "ref");
    ib_node *n   = nd(b, IB_OPERAND, cover(r));

    n->how   = OP_PHI;
    n->text  = blk ? blk->text : nullptr;
    n->text2 = ref ? ref->text : nullptr;

    return n;
}

static void *r_switch_arm(ncc_reduce_t *r)
{
    B       *b   = r->thunk;
    ib_node *num = at(r, "number");
    ib_node *blk = at(r, "name");
    ib_node *n   = nd(b, IB_OPERAND, cover(r));

    n->how  = OP_SWITCH;
    n->text = blk ? blk->text : nullptr;

    if (num) {
        n->lit     = num->lit;
        n->lit.neg = has_minus(r);
        n->over    = num->over;
    }

    return n;
}

static void *r_default_block(ncc_reduce_t *r)
{
    B       *b   = r->thunk;
    ib_node *blk = at(r, "name");
    ib_node *n   = nd(b, IB_OPERAND, cover(r));

    n->how  = OP_DEFAULT;
    n->text = blk ? blk->text : nullptr;

    return n;
}

static void *r_instr(ncc_reduce_t *r)
{
    B                 *b    = r->thunk;
    ncc_reduce_list_t *dest = rep(r, "dest");
    ib_node           *op   = at(r, "opcode");
    ib_node           *n    = nd(b, IB_INSTR, cover(r));

    n->a    = dest && dest->n ? unwrap(dest->v[0]) : nullptr;
    n->text    = op ? op->text : nullptr;
    n->present = op && op->present;
    n->loc     = op ? op->span : n->span;
    n->node    = r->node;
    n->l1   = rep(r, "modifier");
    n->l2   = rep(r, "operands");
    n->l3   = rep(r, "meta");

    return n;
}

static void *r_operands(ncc_reduce_t *r)
{
    B       *b = r->thunk;
    ib_node *n = nd(b, IB_OPERAND, cover(r));

    n->how = OP_WORD;
    n->a   = kid(r, 0);
    n->l1  = tail_group(r);

    return n;
}

static void *r_block(ncc_reduce_t *r)
{
    B       *b    = r->thunk;
    ib_node *name = at(r, "name");
    ib_node *n    = nd(b, IB_BLOCK, cover(r));

    n->text = name ? name->text : nullptr;
    n->l1   = rep(r, "meta");
    n->l2   = rep(r, "instr");

    return n;
}

static ib_node *instr_at(ib_node *blk, size_t i)
{
    size_t n = blk->l2 ? blk->l2->n : 0;

    if (i < n) {
        return unwrap(blk->l2->v[i]);
    }

    return blk->l3 && i - n < blk->l3->n ? unwrap(blk->l3->v[i - n])
                                        : nullptr;
}

static size_t instr_count(ib_node *blk)
{
    return (blk->l2 ? blk->l2->n : 0) + (blk->l3 ? blk->l3->n : 0);
}

static void *r_local(ncc_reduce_t *r)
{
    B       *b    = r->thunk;
    ib_node *name = at(r, "local-ref");
    ib_node *n    = nd(b, IB_LOCAL, cover(r));

    n->text = name ? name->text : nullptr;
    n->loc  = name ? name->span : n->span;
    n->type = type_of(b, raw_at(r, "typelang-type"), n->span);
    n->l1   = rep(r, "init");
    n->l2   = rep(r, "meta");

    return n;
}

static ir_block_t *block_named(ir_func_t *f, const char *name)
{
    ir_block_t *bb = ir_block_find(f, name);

    return bb ? bb : ir_block_add(f, name);
}

static ir_value_t *global_value(B *b, const char *name)
{
    ir_global_t *g  = ir_global_find(b->m, name);
    ir_func_t   *fn = ir_func_find(b->m, name);
    ir_value_t  *v  = ir_alloc(b->ctx, sizeof(ir_value_t));

    v->kind = g ? IR_VAL_GLOBAL : IR_VAL_FUNC;
    v->name = name;
    v->type = g ? g->type : fn ? fn->type : nullptr;

    return v;
}

static void push_operand(B *b, ir_func_t *f, ir_instr_t *in, ib_node *o)
{
    ir_opdkind_t want = ir_opinfo_operand_kind(
        ir_module_op_info(b->m, in->op), in->n_operands);

    if (o->over) {
        bfail(b, o->span, "this literal does not fit in 128 bits");

        return;
    }

    switch (o->how) {
    case OP_NAMED: {
        ir_value_t *v = o->refkind == IR_VAL_GLOBAL
                          ? global_value(b, o->text2)
                          : ir_value_find(f, o->text2);

        if (!v && o->refkind == IR_VAL_LOCAL) {
            ir_local_t *l = ir_local_find(f, o->text2);

            if (!l) {
                bfail(b, o->span, "unknown local");

                return;
            }
            ir_instr_push_local(in, l);

            return;
        }

        if (!v) {
            bfail(b, o->span, "unknown value");

            return;
        }
        ir_instr_push_named(in, o->text, v);

        return;
    }

    case OP_PHI: {
        ir_value_t *v = ir_value_find(f, o->text2);

        if (!v) {
            bfail(b, o->span, "unknown value");

            return;
        }
        ir_instr_push_phi_arm(in, block_named(f, o->text), v);

        return;
    }

    case OP_SWITCH:
        ir_instr_push_switch_case(in, o->lit, block_named(f, o->text));

        return;

    case OP_DEFAULT:
        ir_instr_push_default(in, block_named(f, o->text));

        return;

    case OP_TYPE:
        if (want == IR_OPD_BLOCK && o->text) {
            ir_instr_push_block(in, block_named(f, o->text));

            return;
        }
        ir_instr_push_type(in, o->type);

        return;

    case OP_NUM:
        if (o->is_float) {
            ir_sb sb = {0};

            if (o->neg) {
                ir_sb_str(&sb, "-");
            }
            ir_sb_str(&sb, o->text ? o->text : "0");

            char *s = ir_sb_take(&sb);

            ir_instr_push_float_text(in, ir_strdup(b->ctx, s ? s : "0"));
            free(s);

            return;
        }
        ir_instr_push_lit(in, o->lit);

        return;

    case OP_STR:
        ir_instr_push_name(in, o->text);

        return;

    case OP_WORD:
        if (want == IR_OPD_BLOCK || want == IR_OPD_DEFAULT_BLOCK) {
            ir_instr_push_block(in, block_named(f, o->text));

            return;
        }

        if (want == IR_OPD_TYPE) {
            th_type *t = ir_type_parse(b->m, o->text);

            if (!t) {
                bfail(b, o->span, "expected a type");

                return;
            }
            ir_instr_push_type(in, t);

            return;
        }

        if (want == IR_OPD_NAME) {
            ir_instr_push_name(in, o->text);

            return;
        }
        bfail(b, o->span, "expected a value");

        return;

    case OP_REF:
        break;
    }

    if (o->refkind == IR_VAL_GLOBAL) {
        ir_instr_push_value(in, global_value(b, o->text));

        return;
    }

    if (o->refkind == IR_VAL_LOCAL) {
        ir_local_t *l = ir_local_find(f, o->text);

        if (!l) {
            bfail(b, o->span, "unknown local");

            return;
        }
        ir_instr_push_local(in, l);

        return;
    }

    ir_value_t *v = ir_value_find(f, o->text);

    if (!v) {
        bfail(b, o->span, "unknown value");

        return;
    }
    ir_instr_push_value(in, v);
}

static void each_operand(B *b, ir_func_t *f, ir_instr_t *in, ib_node *ops)
{
    if (!ops) {
        return;
    }
    push_operand(b, f, in, ops->a);

    for (size_t i = 0; i < sep_count(ops->l1) && !b->failed; i++) {
        ib_node *o = sep_at(ops->l1, i);

        if (o) {
            push_operand(b, f, in, o);
        }
    }
}

static void *r_func(ncc_reduce_t *r)
{
    B                 *b    = r->thunk;
    ncc_reduce_list_t *link = rep(r, "linkage");
    ib_node           *name = at(r, "global");
    ncc_reduce_list_t *ps   = rep(r, "params");
    th_type           *ret  = type_of(b, raw_at(r, "typelang-type"), cover(r));

    if (!name || !name->text || !ret) {
        return nullptr;
    }

    ib_node *plist = ps && ps->n ? ps->v[0] : nullptr;
    size_t   np    = 0;
    size_t   nkw   = 0;

    ib_node  *pv[64];
    th_param  params[64];
    th_kwparam kws[64];
    th_param  rest     = {0};
    bool      has_rest = false;
    size_t    npv      = 0;

    if (plist) {
        pv[npv++] = plist->a;

        for (size_t i = 0; i < sep_count(plist->l1) && npv < 64; i++) {
            ib_node *p = sep_at(plist->l1, i);

            if (p) {
                pv[npv++] = p;
            }
        }
    }

    for (size_t i = 0; i < npv; i++) {
        ib_node *p = pv[i];

        if (!p || !p->type) {
            continue;
        }

        if (p->neg) {
            rest     = (th_param){.mode = TH_IN, .type = p->type};
            has_rest = true;
        }
        else if (p->refkind == IR_VAL_GLOBAL) {
            kws[nkw++] = (th_kwparam){.name = p->text,
                                      .mode = (th_mode)p->uval,
                                      .type = p->type};
        }
        else {
            params[np++] = (th_param){.mode = (th_mode)p->uval,
                                      .type = p->type};
        }
    }

    th_type   *ft = th_func(ir_ctx_types(b->ctx), ret, params, np,
                            has_rest ? &rest : nullptr, kws, nkw);
    ir_linkage_t lk = IR_LINK_EXTERNAL;

    if (link && link->n) {
        lk = is_word(link->v[0], "static") ? IR_LINK_INTERNAL
           : is_word(link->v[0], "extern") ? IR_LINK_EXTERN
                                           : IR_LINK_EXTERNAL;
    }

    ir_func_t *f = ir_func_add(b->m, name->text, ft, lk);

    if (!f) {
        bfail(b, cover(r), "duplicate function");

        return nullptr;
    }

    for (size_t i = 0; i < npv; i++) {
        ib_node *p = pv[i];

        if (p && !p->neg && p->refkind == IR_VAL_INSTR && p->type) {
            ir_param_add(f, p->text, p->type);
        }
    }
    f->span = cover(r);
    apply_metas(b, rep(r, "meta"), f, &f->span, &f->origin, &f->annots);

    ncc_reduce_list_t *locals = rep(r, "local");

    for (size_t i = 0; locals && i < locals->n && !b->failed; i++) {
        ib_node    *ln = locals->v[i];
        ir_local_t *l  = ln && ln->type ? ir_local_add(f, ln->text, ln->type)
                                        : nullptr;

        if (!l) {
            bfail(b, ln ? ln->loc : IR_SPAN_UNKNOWN, "duplicate local");

            return nullptr;
        }
        l->span = ln->span;

        if (ln->l1 && ln->l1->n) {
            apply_init(b, ln->l1->v[0], &l->has_init, &l->init, &l->init_f,
                       &l->init_text, &l->init_is_float);
        }
        apply_metas(b, ln->l2, f, &l->span, &l->origin, &l->annots);
    }

    ncc_reduce_list_t *blocks = rep(r, "block");

    for (size_t i = 0; blocks && i < blocks->n; i++) {
        ib_node *bn = blocks->v[i];

        bn->made_block = block_named(f, bn->text);
        bn->made_block->span = bn->span;
    }

    for (size_t i = 0; blocks && i < blocks->n && !b->failed; i++) {
        ib_node *bn = blocks->v[i];

        apply_metas(b, bn->l1, f, &bn->made_block->span,
                    &bn->made_block->origin, &bn->made_block->annots);

        for (size_t k = 0; k < instr_count(bn) && !b->failed; k++) {
            ib_node *ins = instr_at(bn, k);
            ir_op_t  op;

            if (!ir_module_op_lookup(b->m, ins->text, &op)) {
                if (ins->present) {
                    bfail(b, ins->loc,
                          "this opcode needs a profile that is not loaded");
                }
                else {
                    bfail_as(b, IR_DIAG_UNKNOWN_OPCODE, ins->loc,
                             "unknown opcode");
                }

                return nullptr;
            }

            ins->made_instr = ir_instr_add(bn->made_block, op);

            if (!ins->made_instr) {
                bfail(b, ins->span, "this instruction is not accepted here");

                return nullptr;
            }
            ins->made_instr->span = ins->span;
            attach_comment(b, ins->node, &ins->made_instr->annots);

            if (ins->a && ins->a->refkind == IR_VAL_INSTR) {
                if (!ir_instr_set_dest(ins->made_instr, ins->a->text,
                                       ins->a->type)) {
                    bfail(b, ins->a->span, "this opcode has no result");

                    return nullptr;
                }
            }
        }
    }

    for (size_t i = 0; blocks && i < blocks->n && !b->failed; i++) {
        ib_node *bn = blocks->v[i];

        for (size_t k = 0; k < instr_count(bn) && !b->failed; k++) {
            ib_node    *ins = instr_at(bn, k);
            ir_instr_t *in  = ins->made_instr;

            const ir_opinfo_t *info = ir_module_op_info(b->m, in->op);
            size_t             stray = 0;

            for (size_t j = 0; ins->l1 && j < ins->l1->n; j++) {
                ib_node *md = ins->l1->v[j];

                if (!md || !md->text) {
                    continue;
                }

                if (!ir_opinfo_accepts_mod(info, md->text)) {
                    stray = ins->l1->n - j;
                    break;
                }

                if (!ir_instr_push_mod(in, md->text)) {
                    bfail(b, md->span,
                          "this opcode does not take that modifier");

                    return nullptr;
                }
            }

            if (ins->a && ins->a->refkind == IR_VAL_LOCAL) {
                ir_local_t *l = ir_local_find(f, ins->a->text);

                if (!l) {
                    bfail(b, ins->a->span, "unknown local as destination");

                    return nullptr;
                }
                ir_instr_set_dest_local(in, l);
            }

            for (size_t j = ins->l1->n - stray; stray && j < ins->l1->n;
                 j++) {
                ib_node *word = ins->l1->v[j];
                ib_node  as   = {.kind = IB_OPERAND, .how = OP_WORD,
                                 .text = word->text, .span = word->span};

                push_operand(b, f, in, &as);
            }

            each_operand(b, f, in,
                         ins->l2 && ins->l2->n ? ins->l2->v[0] : nullptr);
            apply_metas(b, ins->l3, f, &in->span, &in->origin, &in->annots);
        }
    }

    return nullptr;
}

static void reg(B *b, ncc_tree_walk_t *w, const char *nt, ncc_reduce_fn fn)
{
    if (!ncc_walk_reduce_nt(w, ncc_string_from_cstr(nt), fn, b)) {
        bfail(b, IR_SPAN_UNKNOWN, "the IR grammar is missing a production");
    }
}

ir_module_t *ir_module_parse(ir_ctx_t *ctx, const char *text, int32_t file,
                             const ir_profile_t *const *profiles,
                             size_t n_profiles, ir_diags_t *d)
{
    if (!ctx || !text) {
        return nullptr;
    }

    ncc_grammar_t *g = ir_grammar();

    if (!g) {
        return nullptr;
    }

    ncc_grammar_set_start_id(g, ir_grammar_nt("module"));

    ncc_token_stream_t *ts = ir_lex(text, strlen(text));
    ncc_pwz_parser_t   *p  = ncc_pwz_new(g);

    if (!ncc_pwz_parse(p, ts)) {
        ir_diag_t *dg = ir_diag_begin(d, IR_DIAG_PARSE,
                                      ir_span_make(file, 0, 0, 1, 1, 1, 1),
                                      "here");

        if (dg) {
            ir_diag_message(dg, d, "this is not a module");
        }
        ncc_pwz_free(p);
        ncc_token_stream_free(ts);

        return nullptr;
    }

    B b = {.ctx        = ctx,
           .d          = d,
           .file       = file,
           .profiles   = profiles,
           .n_profiles = n_profiles,
           .types      = ir_typelang_lower_new(ir_ctx_types(ctx))};

    ncc_tree_walk_t *w = ncc_tree_walk_new(g, &b);

    ir_typelang_install(w, b.types);
    ncc_walk_on_nt(w, ncc_string_from_cstr("module"), open_module, nullptr,
                   &b);

    reg(&b, w, "name", r_name);
    reg(&b, w, "int", r_int);
    reg(&b, w, "signed-int", r_signed_int);
    reg(&b, w, "number", r_number);
    reg(&b, w, "value", r_value);
    reg(&b, w, "local-ref", r_local_ref);
    reg(&b, w, "global", r_global);
    reg(&b, w, "dotted-name", r_dotted);
    reg(&b, w, "profile-decl", r_profile);
    reg(&b, w, "target-decl", r_target);
    reg(&b, w, "type-decl", r_type_decl);
    reg(&b, w, "meta-decl", r_meta_decl);
    reg(&b, w, "linkage", r_linkage);
    reg(&b, w, "param-mode", r_mode);
    reg(&b, w, "origin-kind", r_name);
    reg(&b, w, "modifier", r_modifier);
    reg(&b, w, "init", r_init);
    reg(&b, w, "symbolic-expr", r_symbolic);
    reg(&b, w, "layout-val", r_layout_val);
    reg(&b, w, "layout-item", r_layout_item);
    reg(&b, w, "layout-decl", r_layout_decl);
    reg(&b, w, "ns-key", r_ns_key);
    reg(&b, w, "provenance", r_provenance);
    reg(&b, w, "annot-value", r_annot_value);
    reg(&b, w, "annot", r_annot);
    reg(&b, w, "loc", r_loc);
    reg(&b, w, "origin", r_origin);
    reg(&b, w, "global-decl", r_global_decl);
    reg(&b, w, "func-param", r_param);
    reg(&b, w, "params", r_operands);
    reg(&b, w, "dest", r_dest);
    reg(&b, w, "operand", r_operand);
    reg(&b, w, "operands", r_operands);
    reg(&b, w, "named-arg", r_named_arg);
    reg(&b, w, "phi-arm", r_phi_arm);
    reg(&b, w, "switch-arm", r_switch_arm);
    reg(&b, w, "default-block", r_default_block);
    reg(&b, w, "opcode", r_dotted);
    reg(&b, w, "instr", r_instr);
    reg(&b, w, "block", r_block);
    reg(&b, w, "local", r_local);
    reg(&b, w, "func", r_func);
    reg(&b, w, "module", r_module);

    ncc_tree_walk_reduce(w, ncc_pwz_get_tree(p));
    ncc_tree_walk_free(w);
    ir_typelang_lower_free(b.types);
    ncc_pwz_free(p);
    ncc_token_stream_free(ts);

    return b.failed ? nullptr : b.m;
}
