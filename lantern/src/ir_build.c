#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"
#include "ir/build.h"
#include "ir/grammar.h"
#include "ir/layout.h"
#include "ir/pass.h"
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
    IB_STR,
    IB_TYPE,
} ib_kind;

typedef struct {
    ib_kind   kind;
    ir_span_t span;

    const char *text;
    th_type    *type;

    ir_lit_t     lit;
    bool         is_float;
    bool         over;
    ir_valkind_t refkind;
} ib_node;

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

static void bfail(B *b, ir_span_t at, const char *what)
{
    if (b->failed) {
        return;
    }
    b->failed = true;

    if (!b->d) {
        return;
    }

    ir_diag_t *dg = ir_diag_begin(b->d, IR_DIAG_PARSE, at, what);

    if (dg) {
        ir_diag_message(dg, b->d, what);
    }
}

static ir_span_t tok_span(B *b, const ncc_token_info_t *t)
{
    if (!t) {
        return IR_SPAN_UNKNOWN;
    }

    uint32_t w = t->endcol > t->column ? t->endcol - t->column : 1;

    return ir_span_make(b->file, (uint32_t)t->start_byte,
                        (uint32_t)t->end_byte, t->line, t->column, t->line,
                        t->column + w);
}

static ib_node *nd(B *b, ib_kind kind, ir_span_t span)
{
    ib_node *n = ir_alloc(b->ctx, sizeof(ib_node));

    n->kind = kind;
    n->span = span;

    return n;
}

static ir_span_t span_of(void *v)
{
    ncc_reduce_list_t *l = ncc_reduce_as_list(v);

    if (l) {
        ir_span_t s = IR_SPAN_UNKNOWN;

        for (size_t i = 0; i < l->n; i++) {
            s = ir_span_cover(s, span_of(l->v[i]));
        }

        return s;
    }

    if (!v) {
        return IR_SPAN_UNKNOWN;
    }

    th_type *t = ir_typelang_type_of(v);

    return t ? IR_SPAN_UNKNOWN : ((ib_node *)v)->span;
}

static ir_span_t cover(ncc_reduce_t *r)
{
    ir_span_t s = IR_SPAN_UNKNOWN;

    for (size_t i = 0; i < r->n_kids; i++) {
        s = ir_span_cover(s, span_of(r->kids[i]));
    }

    return s;
}

static ib_node *kid(ncc_reduce_t *r, size_t i)
{
    return i < r->n_kids ? (ib_node *)r->kids[i] : nullptr;
}

static ncc_reduce_list_t *group(ncc_reduce_t *r, size_t i)
{
    return i < r->n_kids ? ncc_reduce_as_list(r->kids[i]) : nullptr;
}

static void *seq_at(void *elem, size_t i)
{
    ncc_reduce_list_t *l = ncc_reduce_as_list(elem);

    return l && i < l->n ? l->v[i] : nullptr;
}

static th_type *type_of(B *b, void *v, ir_span_t where)
{
    th_type *t = ir_typelang_type_of(v);

    if (!t) {
        bfail(b, where, "expected a type");

        return nullptr;
    }

    return ir_type_intern(b->m, t);
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

static void *r_leaf(ncc_reduce_t *r)
{
    B                *b = r->thunk;
    ncc_token_info_t *t = ncc_parse_node_token(r->node);
    ncc_string_t      s = ncc_tree_leaf_text(r->node);
    ib_node          *n = nd(b, IB_NAME, tok_span(b, t));

    n->text = s.data ? ir_strdup(b->ctx, s.data) : nullptr;

    return n;
}

static void *r_name(ncc_reduce_t *r)
{
    return kid(r, 0);
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

    if (ncc_tree_leaf_tid(ncc_tree_child(c, 0)) == ir_grammar_tid("FLOAT_LIT")
        || ncc_tree_leaf_tid(c) == ir_grammar_tid("FLOAT_LIT")) {
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
    ib_node *k = kid(r, 1);
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

static void *r_dotted(ncc_reduce_t *r)
{
    B      *b  = r->thunk;
    ir_sb   sb = {0};
    ib_node *n = nd(b, IB_NAME, cover(r));

    ir_sb_str(&sb, kid(r, 0) && kid(r, 0)->text ? kid(r, 0)->text : "");

    ncc_reduce_list_t *rest = group(r, 1);

    for (size_t i = 0; rest && i < rest->n; i++) {
        void *e = rest->v[i];

        for (size_t k = 0;; k++) {
            void *part = seq_at(e, k);

            if (!part) {
                if (!k) {
                    ib_node *one = e;

                    ir_sb_str(&sb, one && one->text ? one->text : "");
                }
                break;
            }

            ib_node *p = part;

            ir_sb_str(&sb, p->text ? p->text : "");
        }
    }

    char *s = ir_sb_take(&sb);

    n->text = s ? ir_strdup(b->ctx, s) : nullptr;
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
    ib_node *t = kid(r, 1);

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
        bfail(b, IR_SPAN_UNKNOWN, "unknown profile");

        return false;
    }

    return true;
}

static void *r_type_decl(ncc_reduce_t *r)
{
    B       *b    = r->thunk;
    ib_node *name = kid(r, 1);
    th_type *def  = type_of(b, r->kids[3], cover(r));

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

    ir_module_set_span(b->m, cover(r));

    if (!b->m->profile) {
        bfail(b, cover(r), "a module must name its profile");
    }

    return nullptr;
}

static void reg(B *b, ncc_tree_walk_t *w, const char *nt, ncc_reduce_fn fn)
{
    if (!ncc_walk_reduce_nt(w, ncc_string_from_cstr(nt), fn, b)) {
        bfail(b, IR_SPAN_UNKNOWN, "the IR grammar is missing a production");
    }
}

ir_module_t *ir_module_build(ir_ctx_t *ctx, const char *text, int32_t file,
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
    ncc_walk_reduce_leaf(w, r_leaf, &b);
    ncc_walk_on_nt(w, ncc_string_from_cstr("module"), open_module, nullptr,
                   &b);

    reg(&b, w, "name", r_name);
    reg(&b, w, "int", r_int);
    reg(&b, w, "number", r_number);
    reg(&b, w, "value", r_value);
    reg(&b, w, "local-ref", r_local_ref);
    reg(&b, w, "global", r_global);
    reg(&b, w, "dotted-name", r_dotted);
    reg(&b, w, "profile-decl", r_profile);
    reg(&b, w, "target-decl", r_target);
    reg(&b, w, "type-decl", r_type_decl);
    reg(&b, w, "module", r_module);

    ncc_tree_walk_reduce(w, ncc_pwz_get_tree(p));
    ncc_tree_walk_free(w);
    ir_typelang_lower_free(b.types);
    ncc_pwz_free(p);
    ncc_token_stream_free(ts);

    return b.failed ? nullptr : b.m;
}
