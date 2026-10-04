#include <stdlib.h>
#include <string.h>

#include "ir/grammar.h"
#include "ir/layout.h"
#include "ir/pass.h"
#include "ir/text.h"
#include "ir/typelang.h"
#include "private/ir/internal.h"

#include "parse/pwz.h"
#include "parse/tree_util.h"
#include "parse/tree_visit.h"
#include "private/parse/grammar.h"
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
    OP_SPREAD,
    OP_FRESH,
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
typedef ib_node       *ib_node_ptr_t;

core_list_decl(ib_node_ptr_t);
core_list_decl(th_param);
core_list_decl(th_kwparam);

struct ib_node {
    ib_kind   kind;
    ir_span_t span;

    const char *text;
    const char *member;
    th_type    *type;
    ib_node    *second;

    ir_lit_t     lit;
    bool         is_float;
    bool         over;
    bool         neg;
    bool         present;
    bool         is_string;
    size_t       text_length;
    uint64_t     uval;
    ir_valkind_t refkind;

    ir_span_t            blame;
    parse_reduce_list_t *l1;
    parse_reduce_list_t *l2;

    union {
        struct {
            ir_annot_t       annot;
            ir_span_t        at;
            const char      *from;
            meta_kind        mkind;
            ir_origin_kind_t okind;
        };

        struct {
            ir_layout_val_t lay;
            ib_node        *lo;
            ib_node        *hi;
        };

        struct {
            ib_node    *first;
            const char *refname;
            opd_how     how;
        };

        struct {
            ib_node             *dest;
            parse_reduce_list_t *l3;
            parse_tree_t        *node;
            ir_instr_t          *made_instr;
        };

        ir_block_t *made_block;
    };
};

typedef struct {
    ir_ctx_t            *ctx;
    ir_module_t         *m;
    ir_diags_t          *d;
    int32_t              file;
    bool                 failed;
    ir_typelang_lower_t *types;

    const ir_profile_t *const *profiles;
    size_t                     n_profiles;
} B;

static ir_value_t *global_value(B *b, const char *name);

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

static ir_span_t tok_span(B *b, const parse_token_info_t *t)
{
    if (!t) {
        return IR_SPAN_UNKNOWN;
    }

    uint32_t end_line = t->endline ? t->endline : t->line;
    uint32_t end_col  = t->endcol;

    if (!end_col) {
        end_col = t->column + 1;
    }

    ir_span_t s =
        ir_span_make(b->file, (uint32_t)t->start_byte, (uint32_t)t->end_byte,
                     t->line, t->column, end_line, end_col);

    s.implied = true;

    return s;
}

static ib_node *nd(B *b, ib_kind kind, ir_span_t span)
{
    ib_node *n = ir_alloc(b->ctx, sizeof(ib_node));

    memset(n, 0, sizeof(*n));
    n->kind = kind;
    n->span = span;

    return n;
}

static bool is_spanning(parse_tree_t *leaf, void *arg)
{
    (void)arg;

    parse_token_info_t *t = parse_node_token(leaf);

    return t && t->tid != ir_grammar_tid("NEWLINE");
}

static parse_token_info_t *edge_tok(parse_tree_t *n, bool last)
{
    parse_tree_t *hit = parse_tree_scan_leaves(n, last, is_spanning, nullptr);

    return hit ? parse_node_token(hit) : nullptr;
}

static ir_span_t cover(parse_reduce_t *r)
{
    B                  *b = r->thunk;
    parse_token_info_t *a = edge_tok(r->node, false);
    parse_token_info_t *z = edge_tok(r->node, true);

    if (!a) {
        return IR_SPAN_UNKNOWN;
    }

    return ir_span_cover(tok_span(b, a), tok_span(b, z ? z : a));
}

static void *unwrap(void *v)
{
    parse_reduce_list_t *l = parse_reduce_as_list(v);

    while (l && l->n == 1) {
        v = l->v[0];
        l = parse_reduce_as_list(v);
    }

    return v;
}

static bool nt_named(parse_grammar_t *g, int64_t id, core_string_t role)
{
    parse_nonterm_t *nt = parse_get_nonterm(g, id);

    return nt && core_string_eq(nt->name, role);
}

static bool match_holds(parse_grammar_t *g, const parse_match_t *m,
                        core_string_t role, int depth, int limit);

static bool holds_role(parse_grammar_t *g, int64_t id, core_string_t role,
                       int depth, int limit)
{
    parse_nonterm_t *nt = parse_get_nonterm(g, id);

    if (!nt || depth > limit) {
        return false;
    }

    for (size_t i = 0; i < core_list_len(nt->rule_ids); i++) {
        parse_rule_t *rule = parse_get_rule(g, nt->rule_ids.data[i]);

        for (size_t k = 0; rule && k < core_list_len(rule->contents); k++) {
            if (match_holds(g, &rule->contents.data[k], role, depth, limit)) {
                return true;
            }
        }
    }

    return false;
}

static bool match_holds(parse_grammar_t *g, const parse_match_t *m,
                        core_string_t role, int depth, int limit)
{
    if (depth > limit) {
        return false;
    }

    if (m->kind == PARSE_MATCH_NT) {
        return nt_named(g, m->nt_id, role) ||
               holds_role(g, m->nt_id, role, depth + 1, limit);
    }

    if (m->kind == PARSE_MATCH_GROUP && m->group) {
        int64_t inner = ((const parse_rule_group_t *)m->group)->contents_id;

        if (depth + 1 > limit) {
            return false;
        }

        return nt_named(g, inner, role) ||
               holds_role(g, inner, role, depth + 1, limit);
    }

    return false;
}

static size_t slot_of(parse_reduce_t *r, core_string_t role)
{
    parse_nt_node_t *pn   = &core_tree_node_value(r->node);
    parse_rule_t    *rule = parse_get_node_rule(r->grammar, pn);

    for (int limit = 0; limit <= 3; limit++) {
        for (size_t i = 0; rule && i < core_list_len(rule->contents); i++) {
            if (match_holds(r->grammar, &rule->contents.data[i], role, 0,
                            limit)) {
                return i;
            }
        }
    }

    return SIZE_MAX;
}

static ib_node *leaf_ib(B *b, parse_tree_t *c);
static ib_node *kid(parse_reduce_t *r, size_t i);

static ib_node *at(parse_reduce_t *r, core_string_t role)
{
    return kid(r, slot_of(r, role));
}

static ib_node *nth_named(parse_reduce_t *r, core_string_t role, size_t nth)
{
    size_t seen = 0;

    for (size_t i = 0; i < r->n_kids; i++) {
        if (parse_tree_is_nt(r->grammar, core_tree_child(r->node, i), role) &&
            seen++ == nth) {
            return kid(r, i);
        }
    }

    return nullptr;
}

static bool is_string_literal(parse_tree_t *leaf, void *context)
{
    (void)context;
    return parse_tree_leaf_tid(leaf) == ir_grammar_tid("STRING_LIT");
}

static ib_node *second_name(parse_reduce_t *r)
{
    return nth_named(r, CORE_STRING_STATIC("name"), 1);
}

static parse_reduce_list_t *tail_group(parse_reduce_t *r)
{
    for (size_t i = r->n_kids; i-- > 0;) {
        parse_reduce_list_t *l = parse_reduce_as_list(r->kids[i]);

        if (l) {
            return l;
        }
    }

    return nullptr;
}

static bool is_minus(parse_tree_t *leaf, void *arg)
{
    (void)arg;

    return parse_tree_leaf_tid(leaf) == '-';
}

static bool subtree_has_minus(parse_tree_t *n)
{
    return parse_tree_scan_leaves(n, false, is_minus, nullptr) != nullptr;
}

static bool has_minus_tok(parse_reduce_t *r)
{
    for (size_t i = 0; i < r->n_kids; i++) {
        parse_tree_t *c = core_tree_child(r->node, i);

        if (c &&
            !parse_tree_is_nt(r->grammar, c, CORE_STRING_STATIC("number")) &&
            !parse_tree_is_nt(r->grammar, c, CORE_STRING_STATIC("int")) &&
            subtree_has_minus(c)) {
            return true;
        }
    }

    return false;
}

static ib_node *str_kid(parse_reduce_t *r)
{
    for (size_t i = 0; i < r->n_kids; i++) {
        parse_tree_t *c = core_tree_child(r->node, i);

        if (core_tree_is_leaf(c) &&
            parse_tree_leaf_tid(c) == ir_grammar_tid("STRING_LIT")) {
            return leaf_ib(r->thunk, c);
        }
    }

    return nullptr;
}

static void *raw_at(parse_reduce_t *r, core_string_t role)
{
    size_t i = slot_of(r, role);

    return i < r->n_kids ? unwrap(r->kids[i]) : nullptr;
}

static parse_reduce_list_t *rep(parse_reduce_t *r, core_string_t role)
{
    size_t i = slot_of(r, role);

    return i < r->n_kids ? parse_reduce_as_list(r->kids[i]) : nullptr;
}

static ib_node *leaf_ib(B *b, parse_tree_t *c)
{
    parse_token_info_t *t = parse_node_token(c);
    core_string_t       s = parse_tree_leaf_text(c);
    ib_node *n = nd(b, IB_NAME,
                    t && t->tid == ir_grammar_tid("NEWLINE") ? IR_SPAN_UNKNOWN
                                                             : tok_span(b, t));

    if (s.data || (t && t->tid == ir_grammar_tid("STRING_LIT"))) {
        char *copy = ir_alloc(b->ctx, s.u8_bytes + 1);
        if (copy) {
            if (s.u8_bytes) memcpy(copy, s.data, s.u8_bytes);
            copy[s.u8_bytes] = '\0';
            n->text = copy;
        }
        n->text_length = s.u8_bytes;
    } else if (t && t->tid > 0 && t->tid < 128) {
        char one[2] = {(char)t->tid, 0};

        n->text = ir_strdup(b->ctx, one);
    }

    return n;
}

static void *r_leaf(parse_reduce_t *r) { return leaf_ib(r->thunk, r->node); }

static ib_node *kid(parse_reduce_t *r, size_t i)
{
    if (i >= r->n_kids) {
        return nullptr;
    }

    parse_tree_t *c = core_tree_child(r->node, i);

    if (c && core_tree_is_leaf(c)) {
        return leaf_ib(r->thunk, c);
    }

    return (ib_node *)unwrap(r->kids[i]);
}

static size_t sep_count(parse_reduce_list_t *l) { return l ? l->n / 2 : 0; }

static ib_node *sep_at(parse_reduce_list_t *l, size_t k)
{
    return l && 2 * k + 1 < l->n ? (ib_node *)unwrap(l->v[2 * k + 1]) : nullptr;
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

static void attach_comment(B *b, parse_tree_t *node, ir_annots_t *a)
{
    parse_token_info_t *t = parse_tree_find_first_terminal(node);

    if (!t || !t->leading_trivia) {
        return;
    }

    core_buffer_t *sb = core_buffer_empty();

    for (parse_trivia_t *tv = t->leading_trivia; tv; tv = tv->next) {
        const char *s = tv->text.data;

        if (!s || *s != '#') {
            continue;
        }
        s++;

        while (*s == ' ') {
            s++;
        }

        if (sb->byte_len) {
            core_buffer_puts(sb, "\n");
        }
        core_buffer_puts(sb, s);
    }

    char *msg = core_buffer_take(sb);

    if (msg && *msg) {
        ir_annot_set(b->ctx, a,
                     (ir_annot_t){.key  = "ir.comment",
                                  .kind = IR_ANNOT_STR,
                                  .s    = ir_strdup(b->ctx, msg)});
    }
    core_free(msg);
}

static bool decode_int(const char *s, ir_lit_t *out, bool *over)
{
    ir_uint_t acc = 0;
    bool      hex = s[0] == '0' && (s[1] == 'x' || s[1] == 'X');

    *over = false;

    if (hex) {
        for (const char *p = s + 2; *p; p++) {
            unsigned d = *p >= '0' && *p <= '9'   ? (unsigned)(*p - '0')
                         : *p >= 'a' && *p <= 'f' ? (unsigned)(*p - 'a' + 10)
                         : *p >= 'A' && *p <= 'F' ? (unsigned)(*p - 'A' + 10)
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

static void *r_name(parse_reduce_t *r) { return kid(r, 0); }

static void *r_signed_int(parse_reduce_t *r)
{
    B       *b = r->thunk;
    ib_node *k = at(r, CORE_STRING_STATIC("int"));
    ib_node *n = nd(b, IB_NUM, cover(r));

    if (k) {
        n->lit     = k->lit;
        n->lit.neg = has_minus_tok(r);
        n->over    = k->over;
    }

    return n;
}

static void *r_int(parse_reduce_t *r)
{
    B       *b = r->thunk;
    ib_node *k = kid(r, 0);
    ib_node *n = nd(b, IB_NUM, k ? k->span : IR_SPAN_UNKNOWN);

    if (!k || !k->text || !decode_int(k->text, &n->lit, &n->over)) {
        bfail(b, n->span, "expected an integer literal");
    }

    return n;
}

static void *r_number(parse_reduce_t *r)
{
    B            *b = r->thunk;
    ib_node      *k = kid(r, 0);
    parse_tree_t *c = core_tree_child(r->node, 0);
    ib_node      *n = nd(b, IB_NUM, k ? k->span : IR_SPAN_UNKNOWN);

    n->text = k ? k->text : nullptr;

    if (core_tree_is_leaf(c) &&
        parse_tree_leaf_tid(c) == ir_grammar_tid("FLOAT_LIT")) {
        n->is_float = true;

        return n;
    }

    if (!n->text || !decode_int(n->text, &n->lit, &n->over)) {
        bfail(b, n->span, "expected a number");
    }

    return n;
}

static void *ref_of(parse_reduce_t *r, ir_valkind_t kind)
{
    B       *b = r->thunk;
    ib_node *k = at(r, CORE_STRING_STATIC("name"));
    ib_node *n = nd(b, IB_REF, cover(r));

    n->text    = k ? k->text : nullptr;
    n->refkind = kind;

    return n;
}

static void *r_value(parse_reduce_t *r) { return ref_of(r, IR_VAL_INSTR); }

static void *r_local_ref(parse_reduce_t *r) { return ref_of(r, IR_VAL_LOCAL); }

static void *r_global(parse_reduce_t *r) { return ref_of(r, IR_VAL_GLOBAL); }

typedef struct {
    core_buffer_t *sb;
    size_t         count;
} spell;

static bool spell_leaf(parse_tree_t *leaf, void *arg)
{
    spell              *w = arg;
    parse_token_info_t *t = parse_node_token(leaf);
    core_string_t       s = parse_tree_leaf_text(leaf);

    if (s.data) {
        core_buffer_puts(w->sb, s.data);
    } else if (t && t->tid > 0 && t->tid < 128) {
        char one[2] = {(char)t->tid, 0};

        core_buffer_puts(w->sb, one);
    }
    w->count++;

    return false;
}

static void tokens_into(core_buffer_t *sb, parse_tree_t *n, size_t *count)
{
    spell w = {.sb = sb};

    parse_tree_scan_leaves(n, false, spell_leaf, &w);
    *count += w.count;
}

static void *r_dotted(parse_reduce_t *r)
{
    B             *b  = r->thunk;
    core_buffer_t *sb = core_buffer_empty();
    ib_node       *n  = nd(b, IB_NAME, cover(r));
    size_t         nt = 0;

    tokens_into(sb, r->node, &nt);

    char *s = core_buffer_take(sb);

    n->text    = s ? ir_strdup(b->ctx, s) : nullptr;
    n->present = nt > 1;
    core_free(s);

    return n;
}

static void *r_profile(parse_reduce_t *r)
{
    (void)r;

    return nullptr;
}

static void *r_target(parse_reduce_t *r)
{
    B       *b = r->thunk;
    ib_node *t = at(r, CORE_STRING_STATIC("dotted-name"));

    if (b->m->target) {
        bfail(b, cover(r), "a module names its target once");
    } else if (t && t->text) {
        ir_module_set_target(b->m, t->text);
    }

    return nullptr;
}

static parse_tree_t *nt_child(parse_grammar_t *g, parse_tree_t *n,
                              core_string_t name)
{
    return parse_tree_find_child_by_nt_name(g, n, name);
}

static core_string_t direct_name(parse_tree_t *node)
{
    if (!node || core_tree_is_leaf(node)
        || core_tree_num_children(node) != 1) {
        return (core_string_t){0};
    }

    parse_tree_t *token = core_tree_child(node, 0);

    return core_tree_is_leaf(token) ? parse_tree_leaf_text(token)
                                    : (core_string_t){0};
}

static parse_tree_t *find_profile(parse_grammar_t *g, parse_tree_t *mod)
{
    for (size_t i = 0; i < core_tree_num_children(mod); i++) {
        parse_tree_t *c = core_tree_child(mod, i);

        if (core_tree_is_leaf(c)) {
            continue;
        }

        for (size_t k = 0; k < core_tree_num_children(c); k++) {
            parse_tree_t *hit = nt_child(g, core_tree_child(c, k),
                                         CORE_STRING_STATIC("profile-decl"));

            if (hit) {
                return hit;
            }
        }
    }

    return nullptr;
}

static bool open_module(parse_visit_t *v)
{
    B            *b    = v->thunk;
    core_string_t name = direct_name(
        nt_child(v->grammar, v->node, CORE_STRING_STATIC("name")));
    parse_tree_t *pd = find_profile(v->grammar, v->node);

    if (!pd) {
        bfail(b, IR_SPAN_UNKNOWN, "a module must name its profile");
        parse_tree_walk_stop(v->walk);

        return false;
    }

    core_string_t want = direct_name(
        nt_child(v->grammar, pd, CORE_STRING_STATIC("name")));

    for (size_t i = 0; want.data && i < b->n_profiles; i++) {
        if (!strcmp(b->profiles[i]->name, want.data)) {
            b->m = ir_module_new(b->ctx, name.data ? name.data : "",
                                 b->profiles[i], nullptr);
            break;
        }
    }

    if (!b->m) {
        bfail(b,
              tok_span(b, parse_tree_find_first_terminal(nt_child(
                              v->grammar, pd, CORE_STRING_STATIC("name")))),
              "unknown profile");
        parse_tree_walk_stop(v->walk);

        return false;
    }

    return true;
}

static void *r_type_decl(parse_reduce_t *r)
{
    B       *b    = r->thunk;
    ib_node *name = at(r, CORE_STRING_STATIC("name"));
    th_type *def =
        type_of(b, raw_at(r, CORE_STRING_STATIC("typelang-type")), cover(r));

    if (!name || !name->text || !def) {
        return nullptr;
    }

    parse_tree_t *kind = core_tree_num_children(r->node) > 1
        ? core_tree_child(r->node, 1) : nullptr;
    th_type *named = parse_tree_leaf_is(kind,
                                         CORE_STRING_STATIC("variant"))
        ? th_variant(ir_module_types(b->m), name->text, nullptr, 0)
        : ir_type_parse(b->m, name->text);

    if (!named || !ir_type_define(b->m, named, def)) {
        bfail(b, cover(r), "this type is already defined");
    }

    return nullptr;
}

static void *r_module(parse_reduce_t *r)
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

static int32_t rule_of(parse_reduce_t *r)
{
    return core_tree_node_value(r->node).rule_index;
}

typedef enum {
    LAY_MEMBER = 0,
    LAY_SIZE,
    LAY_ALIGN,
    LAY_STRIDE,
    LAY_INDIRECT,
    LAY_WORDS,
    LAY_VARIANT_TAG,
    LAY_NICHE,
} lay_kind;

static lay_kind lay_kind_of(parse_grammar_t *g, int32_t rule_ix)
{
    static const struct {
        core_string_t word;
        lay_kind      kind;
    } tab[] = {
        {CORE_STRING_STATIC("size"), LAY_SIZE},
        {CORE_STRING_STATIC("align"), LAY_ALIGN},
        {CORE_STRING_STATIC("stride"), LAY_STRIDE},
        {CORE_STRING_STATIC("indirect"), LAY_INDIRECT},
        {CORE_STRING_STATIC("words"), LAY_WORDS},
        {CORE_STRING_STATIC("variant"), LAY_VARIANT_TAG},
        {CORE_STRING_STATIC("niche"), LAY_NICHE},
    };

    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        if (parse_alt_leads_with(g, CORE_STRING_STATIC("layout-item"), rule_ix,
                                 tab[i].word)) {
            return tab[i].kind;
        }
    }

    return LAY_MEMBER;
}

static void *r_linkage(parse_reduce_t *r)
{
    ib_node *n = kid(r, 0);

    if (n) {
        n->uval = rule_of(r) == 0 ? IR_LINK_INTERNAL : IR_LINK_EXTERN;
    }

    return n;
}

static void *r_mutability(parse_reduce_t *r)
{
    ib_node *n = kid(r, 0);

    if (n) {
        n->uval = (uint64_t)rule_of(r);
    }

    return n;
}

static ir_mutability_t mutability_of(parse_reduce_t *r)
{
    parse_reduce_list_t *m = rep(r, CORE_STRING_STATIC("mutability"));

    if (!m || !m->n) {
        return IR_MUT_MUTABLE;
    }

    ib_node *n = unwrap(m->v[0]);

    return n ? (ir_mutability_t)n->uval : IR_MUT_MUTABLE;
}

static void *r_mode(parse_reduce_t *r)
{
    ib_node *n = kid(r, 0);

    if (n) {
        n->uval = rule_of(r) == 1 ? TH_OUT : rule_of(r) == 2 ? TH_INOUT : TH_IN;
    }

    return n;
}

static void *r_modifier(parse_reduce_t *r) { return kid(r, 0); }

static void *r_signed_number(parse_reduce_t *r)
{
    B       *b   = r->thunk;
    ib_node *num = at(r, CORE_STRING_STATIC("number"));
    ib_node *n   = nd(b, IB_NUM, cover(r));

    n->neg      = has_minus_tok(r);
    n->is_float = num && num->is_float;
    n->text     = num ? num->text : nullptr;

    if (num && !num->is_float) {
        n->lit     = num->lit;
        n->lit.neg = n->neg;
        n->over    = num->over;
    }

    return n;
}

static void *r_init(parse_reduce_t *r)
{
    B       *b   = r->thunk;
    ib_node *num = at(r, CORE_STRING_STATIC("signed-number"));
    ib_node *n   = nd(b, IB_INIT, cover(r));

    n->present  = true;
    n->neg      = num && num->neg;
    n->is_float = num && num->is_float;
    n->text     = num ? num->text : nullptr;

    if (num && !num->is_float) {
        n->lit  = num->lit;
        n->over = num->over;
    }

    return n;
}

static void *r_global_init_value(parse_reduce_t *r)
{
    B       *b = r->thunk;
    ib_node *raw = kid(r, 0);
    ib_node *n = nd(b, IB_INIT, cover(r));
    if (!raw) return nullptr;
    if (raw->kind == IB_INIT) return raw;
    n->present = true;
    if (raw->kind == IB_NUM) {
        n->uval = raw->is_float ? IR_GLOBAL_INIT_FLOAT : IR_GLOBAL_INIT_INT;
        n->neg = raw->neg;
        n->is_float = raw->is_float;
        n->text = raw->text;
        n->lit = raw->lit;
        n->over = raw->over;
    } else if (raw->kind == IB_REF && raw->text) {
        n->uval = IR_GLOBAL_INIT_SYMBOL;
        n->present = true;
        n->text = raw->text;
        n->refkind = IR_VAL_GLOBAL;
    } else if (raw->kind == IB_NAME && raw->text) {
        if (!strcmp(raw->text, "true") || !strcmp(raw->text, "false")) {
            n->uval = IR_GLOBAL_INIT_BOOL;
            n->neg = !strcmp(raw->text, "true");
        } else {
            n->uval = IR_GLOBAL_INIT_STRING;
            n->is_string = true;
            n->text = raw->text;
            n->text_length = raw->text_length;
        }
    } else {
        return nullptr;
    }
    return n;
}

static void *r_global_init_values(parse_reduce_t *r)
{
    B       *b = r->thunk;
    ib_node *n = nd(b, IB_OPERAND, cover(r));
    n->first = kid(r, 0);
    n->l1 = tail_group(r);
    return n;
}

static void *r_global_init(parse_reduce_t *r)
{
    return at(r, CORE_STRING_STATIC("global-init-value"));
}

static ib_node *global_init_aggregate(parse_reduce_t *r,
                                      ir_global_init_kind_t kind,
                                      core_string_t values_role)
{
    B       *b = r->thunk;
    ib_node *n = nd(b, IB_INIT, cover(r));
    ib_node *values = at(r, values_role);
    n->uval = kind;
    if (values) {
        parse_reduce_list_t *empty = parse_reduce_as_list(values);
        if (empty) {
            if (empty->n) {
                bfail(b, cover(r), "invalid aggregate initializer reduction");
                return nullptr;
            }
            return n;
        }
        n->first = values->first;
        n->l1 = values->l1;
    }
    return n;
}

static void *r_global_init_sequence(parse_reduce_t *r)
{
    return global_init_aggregate(r, IR_GLOBAL_INIT_SEQUENCE,
        CORE_STRING_STATIC("global-init-values"));
}

static void *r_global_init_tuple(parse_reduce_t *r)
{
    return global_init_aggregate(r, IR_GLOBAL_INIT_TUPLE,
        CORE_STRING_STATIC("global-init-values"));
}

static void *r_global_init_set(parse_reduce_t *r)
{
    return global_init_aggregate(r, IR_GLOBAL_INIT_SET,
        CORE_STRING_STATIC("global-init-values"));
}

static void *r_global_init_field(parse_reduce_t *r)
{
    B       *b = r->thunk;
    ib_node *field = nd(b, IB_INIT, cover(r));
    ib_node *name = at(r, CORE_STRING_STATIC("name"));
    field->member = name ? name->text : nullptr;
    field->first = at(r, CORE_STRING_STATIC("global-init-value"));
    return field;
}

static void *r_global_init_fields(parse_reduce_t *r)
{
    B       *b = r->thunk;
    ib_node *n = nd(b, IB_OPERAND, cover(r));
    n->first = at(r, CORE_STRING_STATIC("global-init-field"));
    n->l1 = tail_group(r);
    return n;
}

static void *r_global_init_record(parse_reduce_t *r)
{
    return global_init_aggregate(r, IR_GLOBAL_INIT_RECORD,
        CORE_STRING_STATIC("global-init-fields"));
}

static void *r_global_init_pair(parse_reduce_t *r)
{
    B       *b = r->thunk;
    ib_node *pair = nd(b, IB_INIT, cover(r));
    pair->first = at(r, CORE_STRING_STATIC("global-init-value"));
    pair->second = nth_named(r, CORE_STRING_STATIC("global-init-value"), 1);
    return pair;
}

static void *r_global_init_pairs(parse_reduce_t *r)
{
    B       *b = r->thunk;
    ib_node *n = nd(b, IB_OPERAND, cover(r));
    n->first = at(r, CORE_STRING_STATIC("global-init-pair"));
    n->l1 = tail_group(r);
    return n;
}

static void *r_global_init_dictionary(parse_reduce_t *r)
{
    return global_init_aggregate(r, IR_GLOBAL_INIT_DICTIONARY,
        CORE_STRING_STATIC("global-init-pairs"));
}

static void *r_global_init_phase_result(parse_reduce_t *r)
{
    ib_node *identifier = str_kid(r);
    if (!identifier || !identifier->text || !identifier->text[0])
        return nullptr;
    ib_node *value = nd(r->thunk, IB_INIT, cover(r));
    value->present = true;
    value->uval = IR_GLOBAL_INIT_PHASE_RESULT;
    value->text = identifier->text;
    return value;
}

static void apply_init(B *b, ib_node *init, bool *has, ir_lit_t *lit, double *f,
                       const char **text, bool *is_float)
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

static size_t global_init_item_count(ib_node *list)
{
    return list && list->first ? 1 + sep_count(list->l1) : 0;
}

static ib_node *global_init_item_at(ib_node *list, size_t index)
{
    return !index ? list ? list->first : nullptr
                  : sep_at(list ? list->l1 : nullptr, index - 1);
}

static bool build_global_init_value(B *b, ib_node *source,
                                    ir_global_init_value_t *value,
                                    size_t depth)
{
    if (!source || !value || depth > 64) return false;
    memset(value, 0, sizeof(*value));
    value->member = source->member;
    value->kind = (ir_global_init_kind_t)source->uval;
    switch (value->kind) {
    case IR_GLOBAL_INIT_BOOL:
        value->boolean = source->neg;
        return true;
    case IR_GLOBAL_INIT_INT:
        if (source->over) return false;
        value->integer = source->lit;
        return true;
    case IR_GLOBAL_INIT_FLOAT:
        value->floating = source->text ? strtod(source->text, nullptr) : 0;
        if (source->neg) value->floating = -value->floating;
        return true;
    case IR_GLOBAL_INIT_STRING:
        value->string.bytes = source->text;
        value->string.length = source->text_length;
        return true;
    case IR_GLOBAL_INIT_PHASE_RESULT:
        if (!source->text || !source->text[0]) return false;
        value->phase_result.identifier = source->text;
        return true;
    case IR_GLOBAL_INIT_SYMBOL: {
        if (!source->text) return false;
        ir_value_t *symbol = ir_alloc(b->ctx, sizeof(*symbol));
        if (!symbol) return false;
        symbol->kind = IR_VAL_GLOBAL;
        symbol->name = source->text;
        value->symbol = symbol;
        return true;
    }
    case IR_GLOBAL_INIT_SEQUENCE:
    case IR_GLOBAL_INIT_TUPLE:
    case IR_GLOBAL_INIT_SET: {
        size_t count = global_init_item_count(source);
        ir_global_init_value_t *items = ir_alloc(b->ctx,
            count ? count * sizeof(*items) : 1);
        if (!items) return false;
        for (size_t i = 0; i < count; i++)
            if (!build_global_init_value(b, global_init_item_at(source, i),
                                         &items[i], depth + 1))
                return false;
        value->aggregate.items = items;
        value->aggregate.count = count;
        return true;
    }
    case IR_GLOBAL_INIT_RECORD: {
        size_t count = global_init_item_count(source);
        ir_global_init_value_t *items = ir_alloc(b->ctx,
            count ? count * sizeof(*items) : 1);
        if (!items) return false;
        for (size_t i = 0; i < count; i++) {
            ib_node *field = global_init_item_at(source, i);
            if (!field || !field->member || !field->first
                || !build_global_init_value(b, field->first, &items[i],
                                            depth + 1))
                return false;
            items[i].member = field->member;
        }
        value->aggregate.items = items;
        value->aggregate.count = count;
        return true;
    }
    case IR_GLOBAL_INIT_DICTIONARY: {
        size_t pairs = global_init_item_count(source);
        if (pairs > SIZE_MAX / 2) return false;
        size_t count = pairs * 2;
        ir_global_init_value_t *items = ir_alloc(b->ctx,
            count ? count * sizeof(*items) : 1);
        if (!items) return false;
        for (size_t i = 0; i < pairs; i++) {
            ib_node *pair = global_init_item_at(source, i);
            if (!pair || !pair->first || !pair->second
                || !build_global_init_value(b, pair->first, &items[i * 2],
                                            depth + 1)
                || !build_global_init_value(b, pair->second,
                                            &items[i * 2 + 1], depth + 1))
                return false;
        }
        value->aggregate.items = items;
        value->aggregate.count = count;
        return true;
    }
    }
    return false;
}

static void *r_symbolic(parse_reduce_t *r)
{
    B       *b = r->thunk;
    ib_node *n = nd(b, IB_LAYVAL, cover(r));
    ib_node *w = kid(r, 0);
    ib_node *f = str_kid(r);
    th_type *t =
        type_of(b, raw_at(r, CORE_STRING_STATIC("typelang-type")), cover(r));
    ir_symkind_t k;

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

static void *r_layout_val(parse_reduce_t *r)
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
    } else {
        bfail(b, n->span, "expected a layout value");
    }

    return n;
}

static ir_props_t prop_named(B *b, const char *s, ir_span_t at)
{
    static const ir_props_t ALL[] = {
        IR_PROP_PARSED,     IR_PROP_TYPED,    IR_PROP_SSA,
        IR_PROP_STRUCTURED, IR_PROP_LAID_OUT, IR_PROP_NO_CRIT_EDGE,
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

static void *r_ns_key(parse_reduce_t *r)
{
    B             *b  = r->thunk;
    ib_node       *ns = kid(r, 0);
    ib_node       *k  = second_name(r);
    ib_node       *n  = nd(b, IB_NAME, cover(r));
    core_buffer_t *sb = core_buffer_empty();

    core_buffer_puts(sb, ns && ns->text ? ns->text : "");
    core_buffer_puts(sb, ".");
    core_buffer_puts(sb, k && k->text ? k->text : "");

    char *s = core_buffer_take(sb);

    n->text = s ? ir_strdup(b->ctx, s) : nullptr;
    core_free(s);

    return n;
}

static void *r_provenance(parse_reduce_t *r)
{
    return at(r, CORE_STRING_STATIC("name"));
}

static void *r_annot_value(parse_reduce_t *r)
{
    B       *b = r->thunk;
    ib_node *k = kid(r, 0);
    th_type *t = r->n_kids ? ir_typelang_type_of(unwrap(r->kids[0])) : nullptr;
    ib_node *n = nd(b, IB_META, cover(r));

    n->mkind = MT_ANNOT;

    if (t) {
        n->annot.kind = IR_ANNOT_TYPE;
        n->annot.t    = ir_type_intern(b->m, t);

        return n;
    }

    ib_node *num = at(r, CORE_STRING_STATIC("int"));
    ib_node *ref = at(r, CORE_STRING_STATIC("ref"));
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

    int32_t alt = rule_of(r);

    if (parse_alt_leads_with(r->grammar, CORE_STRING_STATIC("annot-value"), alt,
                             CORE_STRING_STATIC("true")) ||
        parse_alt_leads_with(r->grammar, CORE_STRING_STATIC("annot-value"), alt,
                             CORE_STRING_STATIC("false"))) {
        n->annot.kind = IR_ANNOT_BOOL;
        n->annot.b =
            parse_alt_leads_with(r->grammar, CORE_STRING_STATIC("annot-value"),
                                 alt, CORE_STRING_STATIC("true"));
    } else {
        n->annot.kind = IR_ANNOT_STR;
        n->annot.s    = k->text;
    }

    return n;
}

static void *r_annot(parse_reduce_t *r)
{
    B       *b   = r->thunk;
    ib_node *key = at(r, CORE_STRING_STATIC("ns-key"));
    ib_node *val = at(r, CORE_STRING_STATIC("annot-value"));
    ib_node *n   = nd(b, IB_META, cover(r));

    n->mkind = MT_ANNOT;

    if (val) {
        n->annot   = val->annot;
        n->from    = val->text;
        n->refkind = val->refkind;
    }
    n->annot.key = key ? key->text : nullptr;

    parse_reduce_list_t *prov = rep(r, CORE_STRING_STATIC("provenance"));

    if (prov && prov->n) {
        ib_node *p = prov->v[0];

        if (p && p->text) {
            n->annot.derived_from = prop_named(b, p->text, cover(r));
        }
    }

    return n;
}

static void *r_loc(parse_reduce_t *r)
{
    B       *b    = r->thunk;
    ib_node *n    = nd(b, IB_META, cover(r));
    int64_t  v[7] = {0};
    size_t   got  = 0;

    n->mkind = MT_LOC;

    ib_node *first = at(r, CORE_STRING_STATIC("signed-int"));

    if (first) {
        v[got++] = ir_lit_signed(first->lit);
    }

    parse_reduce_list_t *rest = tail_group(r);

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
    n->at = ir_span_make((int32_t)v[0], (uint32_t)v[1], (uint32_t)v[2],
                         (uint32_t)v[3], (uint32_t)v[4], (uint32_t)v[5],
                         (uint32_t)v[6]);

    return n;
}

static void *r_origin(parse_reduce_t *r)
{
    B       *b    = r->thunk;
    ib_node *n    = nd(b, IB_META, cover(r));
    ib_node *k    = at(r, CORE_STRING_STATIC("origin-kind"));
    ib_node *ag   = str_kid(r);
    int64_t  v[7] = {0};
    size_t   got  = 0;

    n->mkind = MT_ORIGIN;

    if (!k || !k->text || !ir_origin_kind_from_name(k->text, &n->okind)) {
        bfail(b, cover(r), "unknown origin kind");

        return n;
    }

    parse_reduce_list_t *nums = tail_group(r);

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
    n->at   = ir_span_make((int32_t)v[0], (uint32_t)v[1], (uint32_t)v[2],
                           (uint32_t)v[3], (uint32_t)v[4], (uint32_t)v[5],
                           (uint32_t)v[6]);
    n->text = ag && ag->text && *ag->text ? ag->text : nullptr;

    return n;
}

static void apply_metas(B *b, parse_reduce_list_t *metas, ir_func_t *f,
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
                *span = mt->at;
            }
            break;

        case MT_ORIGIN:
            break;

        case MT_ANNOT: {
            ir_annot_t a = mt->annot;

            if (a.kind == IR_ANNOT_VALUE) {
                if (mt->refkind == IR_VAL_LOCAL) {
                    ir_local_t *local = f
                        ? ir_local_find(f, mt->from) : nullptr;
                    a.v = local ? ir_local_value(f, local) : nullptr;
                }
                else if (mt->refkind == IR_VAL_GLOBAL) {
                    a.v = global_value(b, mt->from);
                }
                else {
                    a.v = f ? ir_value_find(f, mt->from) : nullptr;
                }

                if (!a.v) {
                    bfail(b, mt->span,
                          "an annotation refers to an unknown IR value");

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
            *origin =
                ir_origin_new(b->ctx, mt->okind, mt->at, mt->text, *origin);
        }
    }
}

static void *r_meta_decl(parse_reduce_t *r)
{
    B       *b  = r->thunk;
    ib_node *an = at(r, CORE_STRING_STATIC("annot"));

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

static void *r_global_decl(parse_reduce_t *r)
{
    B                   *b    = r->thunk;
    parse_reduce_list_t *link = rep(r, CORE_STRING_STATIC("linkage"));
    ib_node             *name = at(r, CORE_STRING_STATIC("global"));
    th_type             *t =
        type_of(b, raw_at(r, CORE_STRING_STATIC("typelang-type")), cover(r));

    if (!name || !name->text || !t) {
        return nullptr;
    }

    ir_global_t *g = ir_global_add(b->m, name->text, t);

    if (!g) {
        bfail(b, cover(r), "duplicate global");

        return nullptr;
    }

    if (link && link->n) {
        ib_node *lk = unwrap(link->v[0]);

        g->is_static = lk && lk->uval == IR_LINK_INTERNAL;
        g->is_extern = lk && lk->uval == IR_LINK_EXTERN;
    }

    g->mutability = mutability_of(r);

    parse_reduce_list_t *global_init =
        rep(r, CORE_STRING_STATIC("global-init"));
    ib_node *initializer = global_init && global_init->n
        ? unwrap(global_init->v[0]) : nullptr;
    if (initializer) {
        ir_global_init_value_t *value = ir_alloc(b->ctx, sizeof(*value));
        if (!build_global_init_value(b, initializer, value, 0)) {
            bfail(b, initializer->span,
                  "invalid or oversized global initializer");
            return nullptr;
        }
        g->static_initializer = value;
    }
    g->span = cover(r);
    apply_metas(b, rep(r, CORE_STRING_STATIC("meta")), nullptr, &g->span,
                &g->origin, &g->annots);

    return nullptr;
}

static void *r_layout_item(parse_reduce_t *r)
{
    B       *b    = r->thunk;
    ib_node *head = kid(r, 0);
    ib_node *n    = nd(b, IB_LAYVAL, cover(r));
    size_t   nv   = 0;
    lay_kind kind = lay_kind_of(r->grammar, rule_of(r));

    n->text = head ? head->text : nullptr;
    n->span = head ? head->span : n->span;
    n->uval = (uint64_t)kind;

    for (size_t i = 0; i < r->n_kids && nv < 2; i++) {
        ib_node *e = kid(r, i);

        if (e && e->kind == IB_LAYVAL && e != head) {
            if (nv++) {
                n->hi = e;
            } else {
                n->lo = e;
            }
        }
    }
    n->present = nv == 2;

    if (kind == LAY_INDIRECT) {
        ib_node *repr = at(r, CORE_STRING_STATIC("indirect-kind"));

        n->present = repr && repr->text && !strcmp(repr->text, "fat");
    } else if (kind == LAY_NICHE) {
        ib_node *value = at(r, CORE_STRING_STATIC("layout-bool"));

        n->present = value && value->text && !strcmp(value->text, "true");
    }

    return n;
}

static void *r_layout_target(parse_reduce_t *r)
{
    B       *b = r->thunk;
    ib_node *n = nd(b, IB_NAME, cover(r));
    ib_node *s = str_kid(r);

    n->text    = s ? s->text : nullptr;
    n->present = s != nullptr;

    return n;
}

static void *r_layout_decl(parse_reduce_t *r)
{
    B       *b = r->thunk;
    th_type *t =
        type_of(b, raw_at(r, CORE_STRING_STATIC("typelang-type")), cover(r));

    if (!t) {
        return nullptr;
    }

    parse_reduce_list_t *items = rep(r, CORE_STRING_STATIC("layout-item"));
    size_t        target_slot = slot_of(r, CORE_STRING_STATIC("layout-target"));
    parse_tree_t *target_node = target_slot < core_tree_num_children(r->node)
                                    ? core_tree_child(r->node, target_slot)
                                    : nullptr;
    parse_tree_t *target_string =
        target_node
            ? parse_tree_scan_leaves(target_node, false, is_string_literal, r)
            : nullptr;
    core_string_t    target_text = target_string
                                       ? parse_tree_leaf_text(target_string)
                                       : (core_string_t){0};
    ir_descriptor_t *d = target_string
                             ? ir_layout_ensure(b->m, t, target_text.data)
                             : ir_layout_ensure_targetless(b->m, t);

    if (!d) {
        bfail(b, cover(r), "this type has no descriptor");

        return nullptr;
    }

    for (size_t i = 0; items && i < items->n; i++) {
        ib_node *it = unwrap(items->v[i]);

        if (!it || it->kind != IB_LAYVAL) {
            continue;
        }

        ib_node *head = it;

        if (it->uval == LAY_INDIRECT) {
            d->indirect.fat      = it->present;
            d->indirect.authored = true;

            continue;
        }
        if (it->uval == LAY_NICHE) {
            d->variant.niche    = it->present;
            d->variant.authored = true;

            continue;
        }
        if (!it->lo) {
            continue;
        }

        if (it->uval == LAY_SIZE) {
            d->size = it->lo->lay;
        } else if (it->uval == LAY_ALIGN) {
            d->align = it->lo->lay;
        } else if (it->uval == LAY_STRIDE) {
            d->stride = it->lo->lay;
        } else if (it->uval == LAY_WORDS) {
            d->indirect.words    = it->lo->lay;
            d->indirect.authored = true;
        } else if (it->uval == LAY_VARIANT_TAG && it->present) {
            ir_layout_val_t lo = it->lo->lay;
            ir_layout_val_t hi = it->hi->lay;

            if (lo.state != IR_LAYOUT_PINNED || hi.state != IR_LAYOUT_PINNED ||
                hi.value < lo.value) {
                bfail(b, head->span,
                      "a variant tag's bit range must be two numbers, low "
                      "then high");
            } else {
                d->variant.tag_bit_offset = lo;
                d->variant.tag_bit_width  = hi;
                d->variant.tag_bit_width.value -= lo.value;
                d->variant.authored = true;
            }
        } else if (it->present) {
            ir_layout_val_t lo = it->lo->lay;
            ir_layout_val_t hi = it->hi->lay;

            if (lo.state != IR_LAYOUT_PINNED || hi.state != IR_LAYOUT_PINNED ||
                hi.value < lo.value) {
                bfail(b, head->span,
                      "a member's bit range must be two numbers, low then "
                      "high");
            } else if (!ir_layout_descriptor_pin_member(b->m, d, head->text,
                                                        lo.value,
                                                        hi.value - lo.value)) {
                bfail(b, head->span,
                      "the member's bit range contradicts one already "
                      "pinned");
            }
        }
    }

    return nullptr;
}

static void *r_param(parse_reduce_t *r)
{
    B       *b = r->thunk;
    ib_node *n = nd(b, IB_PARAM, cover(r));
    n->l1 = rep(r, CORE_STRING_STATIC("meta"));

    if (parse_tree_is_nt(r->grammar, r->node,
                         CORE_STRING_STATIC("func-param-rest"))) {
        n->neg = true;
        ib_node *who = at(r, CORE_STRING_STATIC("value"));
        n->text = who ? who->text : nullptr;
        n->type =
            type_of(b, raw_at(r, CORE_STRING_STATIC("typelang-type")), n->span);

        return n;
    }

    parse_reduce_list_t *mode = rep(r, CORE_STRING_STATIC("param-mode"));
    ib_node             *who  = at(r, CORE_STRING_STATIC("value"))
                                    ? at(r, CORE_STRING_STATIC("value"))
                                    : at(r, CORE_STRING_STATIC("name"));

    n->text    = who ? who->text : nullptr;
    n->refkind = who && who->kind == IB_REF ? IR_VAL_INSTR : IR_VAL_GLOBAL;
    n->type =
        type_of(b, raw_at(r, CORE_STRING_STATIC("typelang-type")), n->span);
    n->uval = TH_IN;

    if (mode && mode->n) {
        ib_node *md = unwrap(mode->v[0]);

        n->uval = md ? md->uval : TH_IN;
    }

    return n;
}

static void *r_dest(parse_reduce_t *r)
{
    B       *b   = r->thunk;
    ib_node *who = kid(r, 0);
    ib_node *n   = nd(b, IB_DEST, who ? who->span : cover(r));

    n->text    = who ? who->text : nullptr;
    n->refkind = who ? who->refkind : IR_VAL_INSTR;

    if (n->refkind == IR_VAL_INSTR) {
        n->type =
            type_of(b, raw_at(r, CORE_STRING_STATIC("typelang-type")), n->span);
    }

    return n;
}

static void *r_operand(parse_reduce_t *r)
{
    B       *b = r->thunk;
    ib_node *k = kid(r, 0);
    th_type *t = r->n_kids ? ir_typelang_type_of(unwrap(r->kids[0])) : nullptr;
    ib_node *n = nd(b, IB_OPERAND, cover(r));

    if (t) {
        parse_token_info_t *first = parse_tree_find_first_terminal(r->node);
        core_string_t word = first && core_option_is_set(first->value)
            ? core_option_get(first->value) : (core_string_t){0};

        n->how  = OP_TYPE;
        n->type = ir_type_intern(b->m, t);
        n->text = word.data ? ir_strdup(b->ctx, word.data) : nullptr;

        return n;
    }

    if (r->n_kids == 2) {
        ib_node *num = at(r, CORE_STRING_STATIC("number"));

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

static void *r_spread_arg(parse_reduce_t *r)
{
    B       *b = r->thunk;
    ib_node *value = at(r, CORE_STRING_STATIC("value"));
    ib_node *n = nd(b, IB_OPERAND, cover(r));
    if (!value || !value->text) return nullptr;
    n->how = OP_SPREAD;
    n->refname = value->text;
    return n;
}

static void *r_fresh_arg(parse_reduce_t *r)
{
    B       *b = r->thunk;
    ib_node *value = at(r, CORE_STRING_STATIC("value"));
    ib_node *n = nd(b, IB_OPERAND, cover(r));
    if (!value || !value->text) return nullptr;
    n->how = OP_FRESH;
    n->refname = value->text;
    return n;
}

static void *r_named_arg(parse_reduce_t *r)
{
    B       *b   = r->thunk;
    ib_node *nm  = kid(r, 0);
    ib_node *ref = at(r, CORE_STRING_STATIC("ref"));
    ib_node *out = kid(r, 2);
    ib_node *n   = nd(b, IB_OPERAND, cover(r));

    n->how     = OP_NAMED;
    n->text    = nm ? nm->text : nullptr;
    n->refname = ref ? ref->text
        : out && out->text && !strcmp(out->text, "out") ? out->text
        : nullptr;
    n->refkind = ref ? ref->refkind : IR_VAL_INSTR;

    return n;
}

static void *r_phi_arm(parse_reduce_t *r)
{
    B       *b   = r->thunk;
    ib_node *blk = at(r, CORE_STRING_STATIC("name"));
    ib_node *ref = at(r, CORE_STRING_STATIC("ref"));
    ib_node *n   = nd(b, IB_OPERAND, cover(r));

    n->how     = OP_PHI;
    n->text    = blk ? blk->text : nullptr;
    n->refname = ref ? ref->text : nullptr;

    return n;
}

static void *r_switch_arm(parse_reduce_t *r)
{
    B       *b   = r->thunk;
    ib_node *num = at(r, CORE_STRING_STATIC("number"));
    ib_node *blk = at(r, CORE_STRING_STATIC("name"));
    ib_node *n   = nd(b, IB_OPERAND, cover(r));

    n->how  = OP_SWITCH;
    n->text = blk ? blk->text : nullptr;

    if (num) {
        n->lit     = num->lit;
        n->lit.neg = has_minus_tok(r);
        n->over    = num->over;
    }

    return n;
}

static void *r_default_block(parse_reduce_t *r)
{
    B       *b   = r->thunk;
    ib_node *blk = at(r, CORE_STRING_STATIC("name"));
    ib_node *n   = nd(b, IB_OPERAND, cover(r));

    n->how  = OP_DEFAULT;
    n->text = blk ? blk->text : nullptr;

    return n;
}

static void *r_instr(parse_reduce_t *r)
{
    B                   *b    = r->thunk;
    parse_reduce_list_t *dest = rep(r, CORE_STRING_STATIC("dest"));
    ib_node             *op   = at(r, CORE_STRING_STATIC("opcode"));
    ib_node             *n    = nd(b, IB_INSTR, cover(r));

    n->dest    = dest && dest->n ? unwrap(dest->v[0]) : nullptr;
    n->text    = op ? op->text : nullptr;
    n->present = op && op->present;
    n->blame   = op ? op->span : n->span;
    n->node    = r->node;
    n->l1      = rep(r, CORE_STRING_STATIC("modifier"));
    n->l2      = rep(r, CORE_STRING_STATIC("operands"));
    n->l3      = rep(r, CORE_STRING_STATIC("meta"));

    return n;
}

static void *r_operands(parse_reduce_t *r)
{
    B       *b = r->thunk;
    ib_node *n = nd(b, IB_OPERAND, cover(r));

    n->how   = OP_WORD;
    n->first = kid(r, 0);
    n->l1    = tail_group(r);

    return n;
}

static void *r_block(parse_reduce_t *r)
{
    B       *b    = r->thunk;
    ib_node *name = at(r, CORE_STRING_STATIC("name"));
    ib_node *n    = nd(b, IB_BLOCK, cover(r));

    n->text = name ? name->text : nullptr;
    n->l1   = rep(r, CORE_STRING_STATIC("meta"));
    n->l2   = rep(r, CORE_STRING_STATIC("instr"));

    return n;
}

static ib_node *instr_at(ib_node *blk, size_t i)
{
    return blk->l2 && i < blk->l2->n ? unwrap(blk->l2->v[i]) : nullptr;
}

static size_t instr_count(ib_node *blk) { return blk->l2 ? blk->l2->n : 0; }

static void *r_local(parse_reduce_t *r)
{
    B       *b    = r->thunk;
    ib_node *name = at(r, CORE_STRING_STATIC("local-ref"));
    ib_node *n    = nd(b, IB_LOCAL, cover(r));

    n->text  = name ? name->text : nullptr;
    n->blame = name ? name->span : n->span;
    n->type =
        type_of(b, raw_at(r, CORE_STRING_STATIC("typelang-type")), n->span);
    n->l1   = rep(r, CORE_STRING_STATIC("init"));
    n->l2   = rep(r, CORE_STRING_STATIC("meta"));
    n->uval = (uint64_t)mutability_of(r);

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
    return g ? ir_global_address(b->m, g)
             : fn ? ir_func_address(b->m, fn) : nullptr;
}

static void push_operand(B *b, ir_func_t *f, ir_instr_t *in, ib_node *o)
{
    if (!o || o->kind != IB_OPERAND) {
        return;
    }

    ir_opdkind_t want =
        ir_opinfo_operand_kind(ir_module_op_info(b->m, in->op), in->n_operands);

    if (o->over) {
        bfail(b, o->span, "this literal does not fit in 128 bits");

        return;
    }

    switch (o->how) {
    case OP_SPREAD: {
        ir_value_t *value = ir_value_find(f, o->refname);
        if (!value || !value->is_rest || !ir_instr_operand_spread(in, value))
            bfail(b, o->span, "spread operand must name a rest parameter");
        return;
    }
    case OP_FRESH: {
        ir_value_t *value = ir_value_find(f, o->refname);
        if (!value || !ir_instr_operand_fresh(in, value))
            bfail(b, o->span,
                  "fresh construction target must name allocated storage");
        return;
    }
    case OP_NAMED: {
        if (o->refname && !strcmp(o->refname, "out")) {
            if (!ir_instr_operand_named_out(in, o->text))
                bfail(b, o->span, "invalid named out argument");
            return;
        }
        ir_value_t *v = o->refkind == IR_VAL_GLOBAL
                            ? global_value(b, o->refname)
                            : ir_value_find(f, o->refname);

        if (!v && o->refkind == IR_VAL_LOCAL) {
            ir_local_t *l = ir_local_find(f, o->refname);

            if (!l) {
                bfail(b, o->span, "unknown local");

                return;
            }
            ir_instr_operand_local(in, l);

            return;
        }

        if (!v) {
            bfail(b, o->span, "unknown value");

            return;
        }
        ir_instr_operand_named(in, o->text, v);

        return;
    }

    case OP_PHI: {
        ir_value_t *v = ir_value_find(f, o->refname);

        if (!v) {
            bfail(b, o->span, "unknown value");

            return;
        }
        ir_instr_operand_phi_arm(in, block_named(f, o->text), v);

        return;
    }

    case OP_SWITCH:
        ir_instr_operand_switch_arm_lit(in, o->lit, block_named(f, o->text));

        return;

    case OP_DEFAULT:
        ir_instr_operand_default(in, block_named(f, o->text));

        return;

    case OP_TYPE:
        if (want == IR_OPD_BLOCK && o->text) {
            ir_instr_operand_block(in, block_named(f, o->text));

            return;
        }
        ir_instr_operand_type(in, o->type);

        return;

    case OP_NUM:
        if (o->is_float) {
            core_buffer_t *sb = core_buffer_empty();

            if (o->neg) {
                core_buffer_puts(sb, "-");
            }
            core_buffer_puts(sb, o->text ? o->text : "0");

            char *s = core_buffer_take(sb);

            ir_instr_operand_float_text(in, ir_strdup(b->ctx, s ? s : "0"));
            core_free(s);

            return;
        }
        ir_instr_operand_int_lit(in, o->lit);

        return;

    case OP_STR:
        ir_instr_operand_name(in, o->text);

        return;

    case OP_WORD:
        if ((want == IR_OPD_BLOCK || want == IR_OPD_DEFAULT_BLOCK)
            && o->text) {
            ir_instr_operand_block(in, block_named(f, o->text));
            return;
        }
        if (want == IR_OPD_ARG && o->text
            && !strcmp(o->text, "missing")) {
            if (!ir_instr_operand_missing(in))
                bfail(b, o->span, "missing is only valid as an optional call argument");
            return;
        }
        if (want == IR_OPD_ARG && o->text && !strcmp(o->text, "out")) {
            if (!ir_instr_operand_out(in))
                bfail(b, o->span, "out is only valid as a call argument");
            return;
        }
        if (want == IR_OPD_TYPE) {
            th_type *t = ir_type_parse(b->m, o->text);

            if (!t) {
                bfail(b, o->span, "expected a type");

                return;
            }
            ir_instr_operand_type(in, t);

            return;
        }

        if (want == IR_OPD_NAME) {
            ir_instr_operand_name(in, o->text);

            return;
        }
        bfail(b, o->span, "expected a value");

        return;

    case OP_REF:
        break;
    }

    if (o->refkind == IR_VAL_GLOBAL) {
        ir_instr_operand_value(in, global_value(b, o->text));

        return;
    }

    if (o->refkind == IR_VAL_LOCAL) {
        ir_local_t *l = ir_local_find(f, o->text);

        if (!l) {
            bfail(b, o->span, "unknown local");

            return;
        }
        ir_instr_operand_local(in, l);

        return;
    }

    ir_value_t *v = ir_value_find(f, o->text);

    if (!v) {
        bfail(b, o->span, "unknown value");

        return;
    }
    ir_instr_operand_value(in, v);
}

static void each_operand(B *b, ir_func_t *f, ir_instr_t *in, ib_node *ops)
{
    if (!ops) {
        return;
    }
    push_operand(b, f, in, ops->first);

    for (size_t i = 0; i < sep_count(ops->l1) && !b->failed; i++) {
        ib_node *o = sep_at(ops->l1, i);

        if (o) {
            push_operand(b, f, in, o);
        }
    }
}

static void *r_func(parse_reduce_t *r)
{
    B                   *b    = r->thunk;
    parse_reduce_list_t *link = rep(r, CORE_STRING_STATIC("linkage"));
    ib_node             *name = at(r, CORE_STRING_STATIC("global"));
    parse_reduce_list_t *ps   = rep(r, CORE_STRING_STATIC("params"));
    th_type             *ret =
        type_of(b, raw_at(r, CORE_STRING_STATIC("typelang-type")), cover(r));
    parse_reduce_list_t *failure_nodes = rep(
        r, CORE_STRING_STATIC("function-failure"));
    void *failure_node = failure_nodes && failure_nodes->n
        ? failure_nodes->v[0] : nullptr;
    th_type *failure = failure_node
        ? type_of(b, failure_node, cover(r)) : nullptr;

    if (!name || !name->text || !ret) {
        return nullptr;
    }

    ib_node *plist = ps && ps->n ? ps->v[0] : nullptr;

    core_list_t(ib_node_ptr_t) pv = core_list_new(ib_node_ptr_t);
    core_list_t(th_param) params  = core_list_new(th_param);
    core_list_t(th_kwparam) kws   = core_list_new(th_kwparam);
    th_param rest                 = {0};
    bool     has_rest             = false;

    if (plist) {
        core_list_push(pv, plist->first);

        for (size_t i = 0; i < sep_count(plist->l1); i++) {
            ib_node *p = sep_at(plist->l1, i);

            if (p) {
                core_list_push(pv, p);
            }
        }
    }

    for (size_t i = 0; i < pv.len; i++) {
        ib_node *p = pv.data[i];

        if (!p || !p->type) {
            continue;
        }

        if (p->neg) {
            rest     = (th_param){.mode = TH_IN, .type = p->type};
            has_rest = true;
        } else if (p->refkind == IR_VAL_GLOBAL) {
            core_list_push(kws, ((th_kwparam){.name = p->text,
                                              .mode = (th_mode)p->uval,
                                              .type = p->type}));
        } else {
            core_list_push(params, ((th_param){.mode = (th_mode)p->uval,
                                               .type = p->type}));
        }
    }

    th_type *ft = th_func_with_error(ir_ctx_types(b->ctx), ret, failure,
        params.data, params.len, has_rest ? &rest : nullptr, kws.data,
        kws.len);

    core_list_free(params);
    core_list_free(kws);
    ir_linkage_t lk = IR_LINK_EXTERNAL;

    if (link && link->n) {
        ib_node *node = unwrap(link->v[0]);

        lk = node ? (ir_linkage_t)node->uval : IR_LINK_EXTERNAL;
    }

    ir_func_t *f = ir_func_add(b->m, name->text, ft, lk);

    if (!f) {
        bfail(b, cover(r), "duplicate function");
        core_list_free(pv);

        return nullptr;
    }

    for (size_t i = 0; i < pv.len; i++) {
        ib_node *p = pv.data[i];

        if (p && p->neg && p->text && p->type) {
            if (!ir_restparam_add(f, p->text, p->type)) {
                bfail(b, p->span, "duplicate or invalid rest parameter");
                return nullptr;
            }
        } else if (p && !p->neg && p->refkind == IR_VAL_INSTR && p->type) {
            ir_param_add(f, p->text, p->type);
        } else if (p && !p->neg && p->refkind == IR_VAL_GLOBAL && p->type) {
            if (!ir_kwparam_add(f, p->text, p->type)) {
                bfail(b, p->span, "duplicate or invalid keyword parameter");
                return nullptr;
            }
        }
    }

    for (size_t i = 0; i < pv.len; i++) {
        ib_node *p = pv.data[i];
        if (!p || !p->text || !p->l1) continue;
        ir_value_t *value = ir_value_find(f, p->text);
        if (!value) {
            bfail(b, p->span, "parameter annotations need a named value");
            return nullptr;
        }
        apply_metas(b, p->l1, f, nullptr, nullptr, &value->annots);
        ir_annot_t *optional = ir_annot_get(&value->annots,
                                             "ir.optional_parameter");
        if (optional && optional->kind == IR_ANNOT_BOOL && optional->b
            && !ir_param_set_optional(value, true)) {
            bfail(b, p->span, "invalid optional parameter metadata");
            return nullptr;
        }
        if (b->failed) return nullptr;
    }

    core_list_free(pv);
    f->span = cover(r);
    apply_metas(b, rep(r, CORE_STRING_STATIC("meta")), f, &f->span, &f->origin,
                &f->annots);

    parse_reduce_list_t *locals = rep(r, CORE_STRING_STATIC("local"));

    for (size_t i = 0; locals && i < locals->n && !b->failed; i++) {
        ib_node *ln = locals->v[i];

        if (ln && ln->kind != IB_LOCAL) {
            continue;
        }

        ir_local_t *l =
            ln && ln->type ? ir_local_add(f, ln->text, ln->type) : nullptr;

        if (!l) {
            bfail(b, ln ? ln->blame : IR_SPAN_UNKNOWN, "duplicate local");

            return nullptr;
        }
        l->span       = ln->span;
        l->mutability = (ir_mutability_t)ln->uval;

        if (ln->l1 && ln->l1->n) {
            apply_init(b, ln->l1->v[0], &l->has_init, &l->init, &l->init_f,
                       &l->init_text, &l->init_is_float);
        }
        apply_metas(b, ln->l2, f, &l->span, &l->origin, &l->annots);
    }

    parse_reduce_list_t *blocks = rep(r, CORE_STRING_STATIC("block"));

    for (size_t i = 0; blocks && i < blocks->n; i++) {
        ib_node *bn = blocks->v[i];

        bn->made_block       = block_named(f, bn->text);
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
                    bfail(b, ins->blame,
                          "this opcode needs a profile that is not loaded");
                } else {
                    bfail_as(b, IR_DIAG_UNKNOWN_OPCODE, ins->blame,
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

            if (ins->dest && ins->dest->refkind == IR_VAL_INSTR) {
                if (!ir_instr_set_dest(ins->made_instr, ins->dest->text,
                                       ins->dest->type)) {
                    bfail(b, ins->dest->span, "this opcode has no result");

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

            const ir_opinfo_t *info  = ir_module_op_info(b->m, in->op);
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

                if (!ir_instr_add_mod(in, md->text)) {
                    bfail(b, md->span,
                          "this opcode does not take that modifier");

                    return nullptr;
                }
            }

            if (ins->dest && ins->dest->refkind == IR_VAL_LOCAL) {
                ir_local_t *l = ir_local_find(f, ins->dest->text);

                if (!l) {
                    bfail(b, ins->dest->span, "unknown local as destination");

                    return nullptr;
                }
                ir_instr_set_dest_local(in, l);
            }

            for (size_t j = ins->l1->n - stray; stray && j < ins->l1->n; j++) {
                ib_node *word = ins->l1->v[j];
                ib_node  as   = {.kind = IB_OPERAND,
                                 .how  = OP_WORD,
                                 .text = word->text,
                                 .span = word->span};

                push_operand(b, f, in, &as);
            }

            each_operand(b, f, in,
                         ins->l2 && ins->l2->n ? ins->l2->v[0] : nullptr);
            apply_metas(b, ins->l3, f, &in->span, &in->origin, &in->annots);
        }
    }

    return nullptr;
}

static void *r_function_failure(parse_reduce_t *r)
{
    return kid(r, 2);
}

static void reg(B *b, parse_tree_walk_t *w, core_string_t nt,
                parse_reduce_fn fn)
{
    if (!parse_walk_reduce_nt(w, nt, fn, b)) {
        bfail(b, IR_SPAN_UNKNOWN, "the IR grammar is missing a production");
    }
}

static bool resolve_static_initializer_symbols(B *b,
                                                ir_global_init_value_t *value,
                                                size_t depth)
{
    if (!value || depth > 64) return false;
    if (value->kind == IR_GLOBAL_INIT_SYMBOL) {
        ir_value_t *symbol = value->symbol;
        if (!symbol || !symbol->name) return false;
        ir_func_t *function = ir_func_find(b->m, symbol->name);
        ir_global_t *target = ir_global_find(b->m, symbol->name);
        if (function) {
            symbol->kind = IR_VAL_FUNC;
            symbol->type = function->type;
            symbol->owner = function;
        } else if (target) {
            symbol->kind = IR_VAL_GLOBAL;
            symbol->type = target->type;
        } else {
            return false;
        }
        return true;
    }
    switch (value->kind) {
    case IR_GLOBAL_INIT_SEQUENCE:
    case IR_GLOBAL_INIT_TUPLE:
    case IR_GLOBAL_INIT_RECORD:
    case IR_GLOBAL_INIT_SET:
    case IR_GLOBAL_INIT_DICTIONARY:
        if (value->aggregate.count && !value->aggregate.items) return false;
        for (size_t i = 0; i < value->aggregate.count; i++)
            if (!resolve_static_initializer_symbols(
                    b, (ir_global_init_value_t *)&value->aggregate.items[i],
                    depth + 1))
                return false;
        break;
    default:
        break;
    }
    return true;
}

static void resolve_global_initializers(B *b)
{
    for (size_t gi = 0; gi < ir_global_count(b->m) && !b->failed; gi++) {
        ir_global_t *global = ir_global_at(b->m, gi);
        if (global->static_initializer && !global->has_static_initializer) {
            ir_global_init_value_t *pending = global->static_initializer;
            if (!resolve_static_initializer_symbols(b, pending, 0)) {
                bfail(b, global->span,
                      "global initializer references an unknown symbol");
            } else if (!ir_global_set_static_initializer(b->m, global,
                                                          pending)) {
                bfail(b, global->span,
                      "global initializer does not match its type");
            }
        }
    }
}

ir_module_t *ir_module_parse(ir_ctx_t *ctx, const char *text, int32_t file,
                             const ir_profile_t *const *profiles,
                             size_t n_profiles, ir_diags_t *d)
{
    if (!ctx || !text) {
        return nullptr;
    }

    parse_grammar_t *g = ir_grammar();

    if (!g) {
        return nullptr;
    }

    parse_token_stream_t *ts = ir_lex(text, strlen(text));
    parse_pwz_parser_t *p = parse_pwz_new_for_start(g, ir_grammar_nt("module"));

    if (!parse_pwz_parse(p, ts)) {
        int32_t             error_pos = parse_pwz_error_pos(p);
        parse_token_info_t *token =
            error_pos >= 0 && error_pos < scan_stream_token_count(ts)
                ? scan_stream_get(ts, error_pos)
                : nullptr;
        ir_span_t span =
            token ? ir_span_make(file, (uint32_t)token->start_byte,
                                 (uint32_t)token->end_byte, token->line,
                                 token->column, token->endline, token->endcol)
                  : ir_span_make(file, 0, 0, 1, 1, 1, 1);
        ir_diag_t *dg = ir_diag_begin(d, IR_DIAG_PARSE, span, "here");

        if (dg) {
            ir_diag_message(dg, d,
                            "this is not a module; parsing stopped here");
        }
        parse_pwz_free(p);
        scan_token_stream_free(ts);

        return nullptr;
    }

    B b = {.ctx        = ctx,
           .d          = d,
           .file       = file,
           .profiles   = profiles,
           .n_profiles = n_profiles,
           .types      = ir_typelang_lower_new(ir_ctx_types(ctx))};

    parse_tree_walk_t *w = parse_tree_walk_new(g, &b);

    if (!ir_typelang_install(w, g, b.types)) {
        ir_diag_t *dg = ir_diag_begin(
            d, IR_DIAG_INTERNAL, ir_span_make(file, 0, 0, 1, 1, 1, 1), "here");
        if (dg) {
            ir_diag_message(dg, d, "could not install typelang reducers");
        }
        parse_tree_walk_free(w);
        ir_typelang_lower_free(b.types);
        parse_pwz_free(p);
        scan_token_stream_free(ts);
        return nullptr;
    }
    parse_walk_reduce_leaf_in(w, ir_grammar_unit(), r_leaf, &b);
    parse_walk_on_nt(w, CORE_STRING_STATIC("module"), open_module, nullptr, &b);

    reg(&b, w, CORE_STRING_STATIC("name"), r_name);
    reg(&b, w, CORE_STRING_STATIC("int"), r_int);
    reg(&b, w, CORE_STRING_STATIC("signed-int"), r_signed_int);
    reg(&b, w, CORE_STRING_STATIC("number"), r_number);
    reg(&b, w, CORE_STRING_STATIC("value"), r_value);
    reg(&b, w, CORE_STRING_STATIC("local-ref"), r_local_ref);
    reg(&b, w, CORE_STRING_STATIC("global"), r_global);
    reg(&b, w, CORE_STRING_STATIC("dotted-name"), r_dotted);
    reg(&b, w, CORE_STRING_STATIC("profile-decl"), r_profile);
    reg(&b, w, CORE_STRING_STATIC("target-decl"), r_target);
    reg(&b, w, CORE_STRING_STATIC("type-decl"), r_type_decl);
    reg(&b, w, CORE_STRING_STATIC("meta-decl"), r_meta_decl);
    reg(&b, w, CORE_STRING_STATIC("linkage"), r_linkage);
    reg(&b, w, CORE_STRING_STATIC("mutability"), r_mutability);
    reg(&b, w, CORE_STRING_STATIC("param-mode"), r_mode);
    reg(&b, w, CORE_STRING_STATIC("origin-kind"), r_name);
    reg(&b, w, CORE_STRING_STATIC("modifier"), r_modifier);
    reg(&b, w, CORE_STRING_STATIC("init"), r_init);
    reg(&b, w, CORE_STRING_STATIC("global-init-value"), r_global_init_value);
    reg(&b, w, CORE_STRING_STATIC("global-init-values"), r_global_init_values);
    reg(&b, w, CORE_STRING_STATIC("global-init"), r_global_init);
    reg(&b, w, CORE_STRING_STATIC("global-init-sequence"),
        r_global_init_sequence);
    reg(&b, w, CORE_STRING_STATIC("global-init-tuple"),
        r_global_init_tuple);
    reg(&b, w, CORE_STRING_STATIC("global-init-set"), r_global_init_set);
    reg(&b, w, CORE_STRING_STATIC("global-init-field"), r_global_init_field);
    reg(&b, w, CORE_STRING_STATIC("global-init-fields"),
        r_global_init_fields);
    reg(&b, w, CORE_STRING_STATIC("global-init-record"),
        r_global_init_record);
    reg(&b, w, CORE_STRING_STATIC("global-init-pair"), r_global_init_pair);
    reg(&b, w, CORE_STRING_STATIC("global-init-pairs"),
        r_global_init_pairs);
    reg(&b, w, CORE_STRING_STATIC("global-init-dictionary"),
        r_global_init_dictionary);
    reg(&b, w, CORE_STRING_STATIC("global-init-phase-result"),
        r_global_init_phase_result);
    reg(&b, w, CORE_STRING_STATIC("signed-number"), r_signed_number);
    reg(&b, w, CORE_STRING_STATIC("symbolic-expr"), r_symbolic);
    reg(&b, w, CORE_STRING_STATIC("layout-val"), r_layout_val);
    reg(&b, w, CORE_STRING_STATIC("layout-target"), r_layout_target);
    reg(&b, w, CORE_STRING_STATIC("indirect-kind"), r_name);
    reg(&b, w, CORE_STRING_STATIC("layout-bool"), r_name);
    reg(&b, w, CORE_STRING_STATIC("layout-item"), r_layout_item);
    reg(&b, w, CORE_STRING_STATIC("layout-decl"), r_layout_decl);
    reg(&b, w, CORE_STRING_STATIC("ns-key"), r_ns_key);
    reg(&b, w, CORE_STRING_STATIC("provenance"), r_provenance);
    reg(&b, w, CORE_STRING_STATIC("annot-value"), r_annot_value);
    reg(&b, w, CORE_STRING_STATIC("annot"), r_annot);
    reg(&b, w, CORE_STRING_STATIC("loc"), r_loc);
    reg(&b, w, CORE_STRING_STATIC("origin"), r_origin);
    reg(&b, w, CORE_STRING_STATIC("global-decl"), r_global_decl);
    reg(&b, w, CORE_STRING_STATIC("func-param-pos"), r_param);
    reg(&b, w, CORE_STRING_STATIC("func-param-kw"), r_param);
    reg(&b, w, CORE_STRING_STATIC("func-param-rest"), r_param);
    reg(&b, w, CORE_STRING_STATIC("function-failure"),
        r_function_failure);
    reg(&b, w, CORE_STRING_STATIC("params"), r_operands);
    reg(&b, w, CORE_STRING_STATIC("dest-value"), r_dest);
    reg(&b, w, CORE_STRING_STATIC("dest-local"), r_dest);
    reg(&b, w, CORE_STRING_STATIC("operand"), r_operand);
    reg(&b, w, CORE_STRING_STATIC("spread-arg"), r_spread_arg);
    reg(&b, w, CORE_STRING_STATIC("fresh-arg"), r_fresh_arg);
    reg(&b, w, CORE_STRING_STATIC("operands"), r_operands);
    reg(&b, w, CORE_STRING_STATIC("named-arg"), r_named_arg);
    reg(&b, w, CORE_STRING_STATIC("phi-arm"), r_phi_arm);
    reg(&b, w, CORE_STRING_STATIC("switch-arm"), r_switch_arm);
    reg(&b, w, CORE_STRING_STATIC("default-block"), r_default_block);
    reg(&b, w, CORE_STRING_STATIC("opcode"), r_dotted);
    reg(&b, w, CORE_STRING_STATIC("instr"), r_instr);
    reg(&b, w, CORE_STRING_STATIC("block"), r_block);
    reg(&b, w, CORE_STRING_STATIC("local"), r_local);
    reg(&b, w, CORE_STRING_STATIC("func"), r_func);
    reg(&b, w, CORE_STRING_STATIC("module"), r_module);

    parse_tree_walk_reduce(w, parse_pwz_get_tree(p));
    resolve_global_initializers(&b);
    parse_tree_walk_free(w);
    ir_typelang_lower_free(b.types);
    parse_pwz_free(p);
    scan_token_stream_free(ts);

    return b.failed ? nullptr : b.m;
}
