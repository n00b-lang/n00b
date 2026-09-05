#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"
#include "ir/text.h"

typedef enum : uint8_t {
    T_EOF,
    T_IDENT,
    T_VALUE,
    T_LOCAL,
    T_GLOBAL,
    T_META,
    T_INT,
    T_FLOAT,
    T_STRING,
    T_PUNCT,
} tokkind;

typedef struct {
    tokkind     kind;
    const char *s;
    size_t      n;
    int64_t     i;
    double      f;
    uint32_t    line;
    uint32_t    col;
    uint32_t    byte;
} tok;

typedef struct {
    const char *src;
    size_t      len;
    size_t      pos;
    uint32_t    line;
    uint32_t    col;
    int32_t     file;
    ir_ctx_t   *ctx;
    ir_diags_t *d;
    tok         cur;
    bool        failed;
    ir_sb       comment;
} P;

static void advance(P *p);

static ir_span_t here(P *p)
{
    return ir_span_make(p->file, p->cur.byte, (uint32_t)(p->cur.byte + p->cur.n),
                        p->cur.line, p->cur.col, p->cur.line,
                        (uint32_t)(p->cur.col + p->cur.n));
}

static void fail(P *p, const char *what)
{
    if (p->failed) {
        return;
    }
    p->failed = true;

    if (p->d) {
        ir_diag_t *dg = ir_diag_begin(p->d, IR_DIAG_PARSE, here(p), what);

        if (dg) {
            ir_diag_message(dg, p->d, what);
        }
    }
}

static bool ident_start(unsigned char c)
{
    return c == '_' || c == '.' || (c >= 'a' && c <= 'z')
        || (c >= 'A' && c <= 'Z') || c >= 0x80;
}

static bool ident_cont(unsigned char c)
{
    return ident_start(c) || (c >= '0' && c <= '9') || c == '-';
}

static void bump(P *p, size_t n)
{
    for (size_t i = 0; i < n && p->pos < p->len; i++) {
        if (p->src[p->pos] == '\n') {
            p->line++;
            p->col = 1;
        }
        else if (((unsigned char)p->src[p->pos] & 0xC0) != 0x80) {
            p->col++;
        }
        p->pos++;
    }
}

static void skip_space(P *p)
{
    for (;;) {
        while (p->pos < p->len
               && (p->src[p->pos] == ' ' || p->src[p->pos] == '\t'
                   || p->src[p->pos] == '\r' || p->src[p->pos] == '\n')) {
            bump(p, 1);
        }

        if (p->pos < p->len && p->src[p->pos] == '#') {
            size_t start = p->pos + 1;

            while (start < p->len && p->src[start] == ' ') {
                start++;
            }

            size_t e = start;

            while (e < p->len && p->src[e] != '\n') {
                e++;
            }

            if (p->comment.len) {
                ir_sb_str(&p->comment, "\n");
            }
            ir_sb_put(&p->comment, p->src + start, e - start);
            bump(p, e - p->pos);
            continue;
        }

        return;
    }
}

static void lex(P *p)
{
    skip_space(p);

    p->cur.line = p->line;
    p->cur.col  = p->col;
    p->cur.byte = (uint32_t)p->pos;
    p->cur.s    = p->src + p->pos;
    p->cur.n    = 0;

    if (p->pos >= p->len) {
        p->cur.kind = T_EOF;

        return;
    }

    char c = p->src[p->pos];

    if (c == '%' || c == '$' || c == '@' || c == '!') {
        size_t start = p->pos + 1;
        size_t e     = start;

        while (e < p->len && ident_cont((unsigned char)p->src[e])) {
            e++;
        }

        p->cur.kind = c == '%'   ? T_VALUE
                      : c == '$' ? T_LOCAL
                      : c == '@' ? T_GLOBAL
                                 : T_META;
        p->cur.s    = p->src + start;
        p->cur.n    = e - start;
        bump(p, e - p->pos);

        return;
    }

    if (c == '"') {
        size_t e = p->pos + 1;

        while (e < p->len && p->src[e] != '"') {
            e += p->src[e] == '\\' ? 2 : 1;
        }

        if (e >= p->len) {
            p->cur.kind = T_EOF;
            fail(p, "unterminated string");

            return;
        }
        p->cur.kind = T_STRING;
        p->cur.s    = p->src + p->pos + 1;
        p->cur.n    = e - (p->pos + 1);
        bump(p, e + 1 - p->pos);

        return;
    }

    if ((c >= '0' && c <= '9')
        || (c == '-' && p->pos + 1 < p->len && p->src[p->pos + 1] >= '0'
            && p->src[p->pos + 1] <= '9')) {
        char  *end;
        double dv = strtod(p->src + p->pos, &end);
        size_t n  = (size_t)(end - (p->src + p->pos));
        bool   isf = false;

        for (size_t i = 0; i < n; i++) {
            char ch = p->src[p->pos + i];

            if (ch == '.' || ch == 'e' || ch == 'E') {
                isf = true;
            }
        }

        p->cur.kind = isf ? T_FLOAT : T_INT;
        p->cur.f    = dv;
        p->cur.i    = strtoll(p->src + p->pos, nullptr, 10);
        p->cur.n    = n;
        bump(p, n);

        return;
    }

    if (ident_start((unsigned char)c)) {
        size_t e = p->pos;

        while (e < p->len && ident_cont((unsigned char)p->src[e])) {
            e++;
        }
        p->cur.kind = T_IDENT;
        p->cur.n    = e - p->pos;
        bump(p, e - p->pos);

        return;
    }

    p->cur.kind = T_PUNCT;
    p->cur.n    = 1;
    bump(p, 1);
}

static void advance(P *p)
{
    lex(p);
}

static bool is_punct(P *p, char c)
{
    return p->cur.kind == T_PUNCT && p->cur.n == 1 && p->cur.s[0] == c;
}

static bool is_word(P *p, const char *w)
{
    return p->cur.kind == T_IDENT && p->cur.n == strlen(w)
        && !memcmp(p->cur.s, w, p->cur.n);
}

static bool eat_punct(P *p, char c)
{
    if (!is_punct(p, c)) {
        return false;
    }
    advance(p);

    return true;
}

static bool eat_word(P *p, const char *w)
{
    if (!is_word(p, w)) {
        return false;
    }
    advance(p);

    return true;
}

static bool expect_punct(P *p, char c, const char *what)
{
    if (eat_punct(p, c)) {
        return true;
    }
    fail(p, what);

    return false;
}

static char *text_of(P *p)
{
    char *s = ir_alloc(p->ctx, p->cur.n + 1);

    if (!s) {
        return nullptr;
    }

    size_t o = 0;

    for (size_t i = 0; i < p->cur.n; i++) {
        char c = p->cur.s[i];

        if (c == '\\' && i + 1 < p->cur.n) {
            char e = p->cur.s[++i];

            c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
        }
        s[o++] = c;
    }
    s[o] = 0;

    return s;
}

static char *ident_text(P *p)
{
    char *s = ir_alloc(p->ctx, p->cur.n + 1);

    memcpy(s, p->cur.s, p->cur.n);
    s[p->cur.n] = 0;

    return s;
}

static char *take_comment(P *p)
{
    if (!p->comment.len) {
        return nullptr;
    }

    char *s = ir_alloc(p->ctx, p->comment.len + 1);

    memcpy(s, p->comment.p, p->comment.len + 1);
    p->comment.len = 0;

    if (p->comment.p) {
        p->comment.p[0] = 0;
    }

    return s;
}

static void attach_comment(P *p, ir_annots_t *a, char *c)
{
    if (!c || !a) {
        return;
    }
    ir_annot_set(p->ctx, a,
                 (ir_annot_t){.key = "ir.comment", .kind = IR_ANNOT_STR,
                              .s = c});
}

static th_type *parse_type(P *p, ir_module_t *m)
{
    p->pos  = p->cur.byte;
    p->line = p->cur.line;
    p->col  = p->cur.col;

    th_error err      = {0};
    size_t   consumed = 0;
    th_type *t = th_parse_prefix(ir_ctx_types(p->ctx), p->src + p->pos,
                                 &consumed, &err);

    if (!t || !consumed) {
        fail(p, err.msg ? err.msg : "expected a type");
        advance(p);

        return nullptr;
    }
    bump(p, consumed);
    advance(p);

    return ir_type_intern(m, t);
}

static bool parse_span(P *p, ir_span_t *out)
{
    int64_t v[7];

    if (!expect_punct(p, '(', "expected ( after !loc")) {
        return false;
    }

    for (int i = 0; i < 7; i++) {
        bool neg = eat_punct(p, '-');

        if (p->cur.kind != T_INT) {
            fail(p, "expected a number in !loc");

            return false;
        }
        v[i] = neg ? -p->cur.i : p->cur.i;
        advance(p);

        if (i < 6 && !expect_punct(p, ',', "expected , in !loc")) {
            return false;
        }
    }

    if (!expect_punct(p, ')', "expected ) after !loc")) {
        return false;
    }

    *out = ir_span_make((int32_t)v[0], (uint32_t)v[1], (uint32_t)v[2],
                        (uint32_t)v[3], (uint32_t)v[4], (uint32_t)v[5],
                        (uint32_t)v[6]);

    return true;
}

static ir_origin_t *parse_origin(P *p, ir_origin_t *parent)
{
    if (!expect_punct(p, '(', "expected ( after !origin")) {
        return nullptr;
    }

    if (p->cur.kind != T_IDENT) {
        fail(p, "expected an origin kind");

        return nullptr;
    }

    char            *kindname = ident_text(p);
    ir_origin_kind_t k;

    if (!ir_origin_kind_from_name(kindname, &k)) {
        fail(p, "unknown origin kind");

        return nullptr;
    }
    advance(p);

    if (!expect_punct(p, ',', "expected , in !origin")) {
        return nullptr;
    }

    if (p->cur.kind != T_STRING) {
        fail(p, "expected an agent string");

        return nullptr;
    }

    char *agent = text_of(p);

    advance(p);

    int64_t v[7];

    for (int i = 0; i < 7; i++) {
        if (!expect_punct(p, ',', "expected , in !origin")) {
            return nullptr;
        }

        bool neg = eat_punct(p, '-');

        if (p->cur.kind != T_INT) {
            fail(p, "expected a number in !origin");

            return nullptr;
        }
        v[i] = neg ? -p->cur.i : p->cur.i;
        advance(p);
    }

    if (!expect_punct(p, ')', "expected ) after !origin")) {
        return nullptr;
    }

    ir_span_t s = ir_span_make((int32_t)v[0], (uint32_t)v[1], (uint32_t)v[2],
                               (uint32_t)v[3], (uint32_t)v[4], (uint32_t)v[5],
                               (uint32_t)v[6]);

    return ir_origin_new(p->ctx, k, s, *agent ? agent : nullptr, parent);
}

static bool prop_from_name(const char *s, ir_props_t *out)
{
    static const ir_props_t all[] = {
        IR_PROP_PARSED,   IR_PROP_TYPED,        IR_PROP_SSA,
        IR_PROP_STRUCTURED, IR_PROP_LAID_OUT,   IR_PROP_NO_CRIT_EDGE,
        IR_PROP_VERIFIED,
    };

    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        const char *n = ir_prop_name(all[i]);

        if (n && !strcmp(n, s)) {
            *out = all[i];

            return true;
        }
    }

    return false;
}

static bool parse_annot(P *p, ir_module_t *m, ir_annots_t *a, const char *key)
{
    if (!expect_punct(p, '(', "expected ( after an annotation key")) {
        return false;
    }

    ir_annot_t an = {.key = key};

    if (p->cur.kind == T_STRING) {
        an.kind = IR_ANNOT_STR;
        an.s    = text_of(p);
        advance(p);
    }
    else if (is_punct(p, '{')) {
        an.kind = IR_ANNOT_TYPE;
        an.t    = parse_type(p, m);
    }
    else if (p->cur.kind == T_VALUE) {
        an.kind = IR_ANNOT_VALUE;
        an.v    = nullptr;
        advance(p);
    }
    else if (is_word(p, "true") || is_word(p, "false")) {
        an.kind = IR_ANNOT_BOOL;
        an.b    = is_word(p, "true");
        advance(p);
    }
    else if (p->cur.kind == T_INT || is_punct(p, '-')) {
        bool neg = eat_punct(p, '-');

        an.kind = IR_ANNOT_INT;
        an.i    = neg ? -p->cur.i : p->cur.i;
        advance(p);
    }
    else {
        fail(p, "unrecognised annotation value");

        return false;
    }

    if (eat_word(p, "from")) {
        if (p->cur.kind != T_IDENT) {
            fail(p, "expected a property name after from");

            return false;
        }

        if (!prop_from_name(ident_text(p), &an.derived_from)) {
            fail(p, "unknown property name");

            return false;
        }
        advance(p);
    }

    if (!expect_punct(p, ')', "expected ) after an annotation")) {
        return false;
    }
    ir_annot_set(p->ctx, a, an);

    return true;
}

static void parse_meta(P *p, ir_module_t *m, ir_span_t *span,
                       ir_origin_t **origin, ir_annots_t *a)
{
    ir_origin_t **chain   = nullptr;
    size_t        n_chain = 0;
    size_t        cap     = 0;

    while (!p->failed && p->cur.kind == T_META) {
        char *key = ident_text(p);

        advance(p);

        if (!strcmp(key, "loc")) {
            ir_span_t s;

            if (parse_span(p, &s) && span) {
                *span = s;
            }
        }
        else if (!strcmp(key, "origin")) {
            ir_origin_t *o = parse_origin(p, nullptr);

            if (!o) {
                return;
            }
            chain = ir_grow(p->ctx, chain, &cap, n_chain + 1,
                            sizeof(ir_origin_t *));

            if (!chain) {
                fail(p, "out of memory");

                return;
            }
            chain[n_chain++] = o;
        }
        else {
            parse_annot(p, m, a, key);
        }
    }

    for (size_t i = n_chain; i-- > 1;) {
        chain[i - 1]->parent = chain[i];
    }

    if (n_chain && origin) {
        *origin = chain[0];
    }
}

static ir_value_t *find_value(ir_func_t *f, const char *name)
{
    for (uint32_t i = 0; i < f->n_params; i++) {
        if (!strcmp(f->params[i]->name, name)) {
            return f->params[i];
        }
    }

    for (uint32_t b = 0; b < f->n_blocks; b++) {
        for (uint32_t k = 0; k < f->blocks[b]->n_instrs; k++) {
            ir_instr_t *in = f->blocks[b]->instrs[k];

            if (in->dest_kind == IR_DEST_VALUE && in->dest
                && !strcmp(in->dest->name, name)) {
                return in->dest;
            }
        }
    }
    return nullptr;
}

typedef struct {
    ir_instr_t *in;
    uint32_t    slot;
    char       *name;
    bool        is_phi;
} fixup;

typedef struct {
    fixup  *v;
    size_t  n;
    size_t  cap;
} fixups;

static void need_value(P *p, fixups *fx, ir_instr_t *in, uint32_t slot,
                       char *name, bool is_phi)
{
    fx->v = ir_grow(p->ctx, fx->v, &fx->cap, fx->n + 1, sizeof(fixup));
    fx->v[fx->n++] = (fixup){.in = in, .slot = slot, .name = name,
                             .is_phi = is_phi};
}

static bool on_line(P *p, uint32_t line)
{
    return p->cur.kind != T_EOF && p->cur.line == line;
}

static bool pushed(P *p, bool ok, const char *what)
{
    if (!ok) {
        fail(p, what);
    }

    return ok;
}

static bool next_is_punct(P *p, char c)
{
    size_t at = p->pos;

    while (at < p->len && (p->src[at] == ' ' || p->src[at] == '\t')) {
        at++;
    }

    return at < p->len && p->src[at] == c;
}

static bool starts_operand(P *p, ir_opdkind_t kind)
{
    switch (kind) {
    case IR_OPD_ARG:
        return p->cur.kind == T_VALUE || p->cur.kind == T_LOCAL
            || p->cur.kind == T_GLOBAL || p->cur.kind == T_IDENT;

    case IR_OPD_VALUE:
        return p->cur.kind == T_VALUE || p->cur.kind == T_LOCAL
            || p->cur.kind == T_GLOBAL;
    case IR_OPD_NAME:
        return p->cur.kind == T_STRING;
    case IR_OPD_BLOCK:
    case IR_OPD_DEFAULT_BLOCK:
        return p->cur.kind == T_IDENT;
    case IR_OPD_CONST:
        return p->cur.kind == T_INT || p->cur.kind == T_FLOAT
            || is_punct(p, '-');
    case IR_OPD_PHI_ARM:
    case IR_OPD_SWITCH_ARM:
        return is_punct(p, '[');
    case IR_OPD_TYPE:
        return p->cur.kind == T_IDENT || is_punct(p, '(')
            || is_punct(p, '`');
    default:
        return false;
    }
}

static bool parse_block_ref(P *p, ir_func_t *f, ir_instr_t *in)
{
    if (p->cur.kind != T_IDENT) {
        fail(p, "expected a block name");

        return false;
    }

    char       *name = ident_text(p);
    ir_block_t *b    = ir_block_find(f, name);

    if (!b) {
        b = ir_block_add(f, name);
    }
    advance(p);

    return pushed(p, ir_instr_push_block(in, b), "block operand rejected");
}

static bool parse_operand(P *p, ir_module_t *m, ir_func_t *f, ir_instr_t *in,
                          fixups *fx)
{
    ir_opdkind_t want = ir_opinfo_operand_kind(
        ir_module_op_info(m, in->op), in->n_operands);

    if (want == IR_OPD_ARG && p->cur.kind == T_IDENT
        && next_is_punct(p, '=')) {
        char *name = ident_text(p);

        advance(p);

        if (!expect_punct(p, '=', "expected = after an argument name")) {
            return false;
        }

        if (p->cur.kind != T_VALUE && p->cur.kind != T_LOCAL
            && p->cur.kind != T_GLOBAL) {
            fail(p, "expected a value after an argument name");

            return false;
        }

        ir_value_t *v = ir_alloc(p->ctx, sizeof(ir_value_t));

        if (!v) {
            fail(p, "out of memory");

            return false;
        }
        v->kind = IR_VAL_INSTR;
        v->name = ident_text(p);

        uint32_t slot = in->n_operands;

        advance(p);

        if (!pushed(p, ir_instr_push_named(in, name, v),
                    "named argument rejected")) {
            return false;
        }
        need_value(p, fx, in, slot, (char *)v->name, false);

        return true;
    }

    switch (want) {
    case IR_OPD_BLOCK:
        return parse_block_ref(p, f, in);

    case IR_OPD_DEFAULT_BLOCK: {
        if (!eat_word(p, "default")
            || !expect_punct(p, ':', "expected : after default")) {
            fail(p, "expected default: before a block name");

            return false;
        }

        if (p->cur.kind != T_IDENT) {
            fail(p, "expected a block name after default:");

            return false;
        }

        char       *name = ident_text(p);
        ir_block_t *b    = ir_block_find(f, name);

        if (!b) {
            b = ir_block_add(f, name);
        }
        advance(p);

        return pushed(p, ir_instr_push_default(in, b),
                      "default block rejected");
    }

    case IR_OPD_TYPE: {
        th_type *t = parse_type(p, m);

        return t && pushed(p, ir_instr_push_type(in, t),
                           "type operand rejected");
    }

    case IR_OPD_NAME: {
        if (p->cur.kind != T_STRING) {
            fail(p, "expected a quoted name");

            return false;
        }

        char *s = text_of(p);

        advance(p);

        return pushed(p, ir_instr_push_name(in, s), "name operand rejected");
    }

    case IR_OPD_CONST: {
        bool neg = eat_punct(p, '-');

        if (p->cur.kind == T_FLOAT) {
            double v = neg ? -p->cur.f : p->cur.f;

            advance(p);

            return pushed(p, ir_instr_push_float(in, v),
                          "literal operand rejected");
        }

        if (p->cur.kind != T_INT) {
            fail(p, "expected a literal");

            return false;
        }

        int64_t v = neg ? -p->cur.i : p->cur.i;

        advance(p);

        return pushed(p, ir_instr_push_int(in, v),
                      "literal operand rejected");
    }

    case IR_OPD_PHI_ARM: {
        if (!expect_punct(p, '[', "expected [ before a phi arm")) {
            return false;
        }

        if (p->cur.kind != T_IDENT) {
            fail(p, "expected a block name in a phi arm");

            return false;
        }

        char       *bn = ident_text(p);
        ir_block_t *bb = ir_block_find(f, bn);

        if (!bb) {
            bb = ir_block_add(f, bn);
        }
        advance(p);

        if (!expect_punct(p, ':', "expected : in a phi arm")) {
            return false;
        }

        if (p->cur.kind != T_VALUE) {
            fail(p, "expected a value in a phi arm");

            return false;
        }

        char *vn = ident_text(p);

        advance(p);

        if (!expect_punct(p, ']', "expected ] after a phi arm")) {
            return false;
        }

        uint32_t    slot        = in->n_operands;
        ir_value_t *placeholder = ir_alloc(p->ctx, sizeof(ir_value_t));

        if (!placeholder) {
            fail(p, "out of memory");

            return false;
        }
        placeholder->kind = IR_VAL_INSTR;
        placeholder->name = vn;

        if (!pushed(p, ir_instr_push_phi_arm(in, bb, placeholder),
                    "phi arm rejected")) {
            return false;
        }
        need_value(p, fx, in, slot, vn, true);

        return true;
    }

    case IR_OPD_SWITCH_ARM: {
        if (!expect_punct(p, '[', "expected [ before a switch arm")) {
            return false;
        }

        bool neg = eat_punct(p, '-');

        if (p->cur.kind != T_INT) {
            fail(p, "expected a case value");

            return false;
        }

        int64_t v = neg ? -p->cur.i : p->cur.i;

        advance(p);

        if (!expect_punct(p, ':', "expected : in a switch arm")) {
            return false;
        }

        if (p->cur.kind != T_IDENT) {
            fail(p, "expected a block name in a switch arm");

            return false;
        }

        char       *bn = ident_text(p);
        ir_block_t *bb = ir_block_find(f, bn);

        if (!bb) {
            bb = ir_block_add(f, bn);
        }
        advance(p);

        if (!expect_punct(p, ']', "expected ] after a switch arm")) {
            return false;
        }

        return pushed(p, ir_instr_push_switch_arm(in, v, bb),
                      "switch arm rejected");
    }

    default: {
        if (p->cur.kind == T_GLOBAL) {
            char        *n = ident_text(p);
            ir_global_t *g = ir_global_find(m, n);
            ir_func_t   *fn = ir_func_find(m, n);

            advance(p);

            ir_value_t *v = ir_alloc(p->ctx, sizeof(ir_value_t));

            v->kind = g ? IR_VAL_GLOBAL : IR_VAL_FUNC;
            v->name = n;
            v->type = g ? g->type : fn ? fn->type : nullptr;

            return pushed(p, ir_instr_push_value(in, v),
                          "global operand rejected");
        }

        if (p->cur.kind == T_LOCAL) {
            char       *n = ident_text(p);
            ir_local_t *l = ir_local_find(f, n);

            advance(p);

            if (!l) {
                fail(p, "unknown local");

                return false;
            }

            return pushed(p, ir_instr_push_local(in, l),
                          "local operand rejected");
        }

        if (p->cur.kind != T_VALUE) {
            fail(p, "expected a value");

            return false;
        }

        char       *n           = ident_text(p);
        uint32_t    slot        = in->n_operands;
        ir_value_t *placeholder = ir_alloc(p->ctx, sizeof(ir_value_t));

        if (!placeholder) {
            fail(p, "out of memory");

            return false;
        }
        placeholder->kind = IR_VAL_INSTR;
        placeholder->name = n;
        advance(p);

        if (!pushed(p, ir_instr_push_value(in, placeholder),
                    "value operand rejected")) {
            return false;
        }
        need_value(p, fx, in, slot, n, false);

        return true;
    }
    }
}

static bool parse_instr(P *p, ir_module_t *m, ir_func_t *f, ir_block_t *b,
                        fixups *fx)
{
    char *comment = take_comment(p);
    char *dest_v  = nullptr;
    char *dest_l  = nullptr;
    th_type *dt   = nullptr;

    if (p->cur.kind == T_VALUE) {
        dest_v = ident_text(p);
        advance(p);

        if (!expect_punct(p, ':', "expected : after a destination")) {
            return false;
        }
        dt = parse_type(p, m);

        if (!dt || !expect_punct(p, '=', "expected = after a destination")) {
            return false;
        }
    }
    else if (p->cur.kind == T_LOCAL) {
        dest_l = ident_text(p);
        advance(p);

        if (!expect_punct(p, '=', "expected = after a local destination")) {
            return false;
        }
    }

    if (p->cur.kind != T_IDENT) {
        fail(p, "expected an opcode");

        return false;
    }

    uint32_t line   = p->cur.line;
    char    *opname = ident_text(p);
    ir_op_t  op;

    if (!ir_module_op_lookup(m, opname, &op)) {
        if (strchr(opname, '.')) {
            fail(p, "this opcode needs a profile that is not loaded");
        }
        else {
            fail(p, "unknown opcode");
        }

        return false;
    }
    advance(p);

    ir_instr_t *in = ir_instr_add(b, op);

    if (!in) {
        fail(p, "instruction could not be created");

        return false;
    }
    attach_comment(p, &in->annots, comment);

    while (on_line(p, line) && p->cur.kind == T_IDENT) {
        char *mod = ident_text(p);

        if (!ir_opinfo_accepts_mod(ir_module_op_info(m, op), mod)) {
            if (ir_mod_exists(mod)) {
                fail(p, "this opcode does not take that modifier");

                return false;
            }
            break;
        }

        if (!ir_instr_push_mod(in, mod)) {
            fail(p, "modifier could not be recorded");

            return false;
        }
        advance(p);
    }

    const ir_opinfo_t *info = ir_module_op_info(m, op);

    for (uint32_t i = 0; i < info->min_operands; i++) {
        if (i && !expect_punct(p, ',', "expected , between operands")) {
            return false;
        }

        if (!parse_operand(p, m, f, in, fx)) {
            return false;
        }
    }

    if (info->max_operands > info->min_operands) {
        if (!info->min_operands && on_line(p, line)
            && starts_operand(p, ir_opinfo_operand_kind(info, 0))
            && !parse_operand(p, m, f, in, fx)) {
            return false;
        }

        while (on_line(p, line) && eat_punct(p, ',')) {
            if (!parse_operand(p, m, f, in, fx)) {
                return false;
            }
        }
    }

    if (dest_v && !ir_instr_set_dest(in, dest_v, dt)) {
        fail(p, "this opcode has no result");

        return false;
    }

    if (dest_l) {
        ir_local_t *l = ir_local_find(f, dest_l);

        if (!l) {
            fail(p, "unknown local as destination");

            return false;
        }

        if (!ir_instr_set_dest_local(in, l)) {
            fail(p, "this opcode has no result");

            return false;
        }
    }

    if (on_line(p, line)) {
        parse_meta(p, m, &in->span, &in->origin, &in->annots);
    }

    return !p->failed;
}

static bool parse_func(P *p, ir_module_t *m, ir_linkage_t link, char *comment)
{
    if (p->cur.kind != T_GLOBAL) {
        fail(p, "expected a function name");

        return false;
    }

    char *name = ident_text(p);

    advance(p);

    if (!expect_punct(p, '(', "expected ( after a function name")) {
        return false;
    }

    char      **pnames = nullptr;
    th_type   **ptypes = nullptr;
    size_t      np     = 0;
    size_t      ncap   = 0;
    size_t      tcap   = 0;
    th_kwparam *kws    = nullptr;
    size_t      nkw    = 0;
    size_t      kcap   = 0;
    th_param    rest   = {0};
    bool        has_rest = false;

    if (!is_punct(p, ')')) {
        do {
            if (eat_punct(p, '*')) {
                th_type *rt = parse_type(p, m);

                if (!rt) {
                    return false;
                }
                rest     = (th_param){.mode = TH_IN, .type = rt};
                has_rest = true;
                continue;
            }

            if (p->cur.kind == T_IDENT) {
                char *kn = ident_text(p);

                advance(p);

                if (!expect_punct(p, ':',
                                  "expected : after a keyword parameter")) {
                    return false;
                }

                th_type *kt = parse_type(p, m);

                if (!kt) {
                    return false;
                }
                kws = ir_grow(p->ctx, kws, &kcap, nkw + 1,
                              sizeof(th_kwparam));

                if (!kws) {
                    fail(p, "out of memory");

                    return false;
                }
                kws[nkw++] = (th_kwparam){.name = kn, .mode = TH_IN,
                                          .type = kt};
                continue;
            }

            if (p->cur.kind != T_VALUE) {
                fail(p, "expected a parameter");

                return false;
            }
            pnames = ir_grow(p->ctx, pnames, &ncap, np + 1, sizeof(char *));
            ptypes = ir_grow(p->ctx, ptypes, &tcap, np + 1, sizeof(th_type *));

            if (!pnames || !ptypes) {
                fail(p, "out of memory");

                return false;
            }
            pnames[np] = ident_text(p);
            advance(p);

            if (!expect_punct(p, ':', "expected : after a parameter name")) {
                return false;
            }
            ptypes[np] = parse_type(p, m);

            if (!ptypes[np]) {
                return false;
            }
            np++;
        } while (eat_punct(p, ','));
    }

    if (!expect_punct(p, ')', "expected ) after parameters")) {
        return false;
    }

    if (!expect_punct(p, '-', "expected -> before a return type")
        || !expect_punct(p, '>', "expected -> before a return type")) {
        return false;
    }

    th_type *ret = parse_type(p, m);

    if (!ret) {
        return false;
    }

    th_param *params = ir_alloc(p->ctx, (np ? np : 1) * sizeof(th_param));

    if (!params) {
        fail(p, "out of memory");

        return false;
    }

    for (size_t i = 0; i < np; i++) {
        params[i] = (th_param){.mode = TH_IN, .type = ptypes[i]};
    }

    th_type *ft = th_func(ir_ctx_types(p->ctx), ret, params, np,
                          has_rest ? &rest : nullptr, kws, nkw);
    ir_func_t *f = ir_func_add(m, name, ft, link);

    if (!f) {
        fail(p, "duplicate function");

        return false;
    }
    attach_comment(p, &f->annots, comment);

    for (size_t i = 0; i < np; i++) {
        ir_param_add(f, pnames[i], ptypes[i]);
    }

    parse_meta(p, m, &f->span, &f->origin, &f->annots);

    if (!expect_punct(p, '{', "expected { to open a function body")) {
        return false;
    }

    fixups fx = {0};

    while (!p->failed && is_word(p, "local")) {
        char *lc = take_comment(p);

        advance(p);

        if (p->cur.kind != T_LOCAL) {
            fail(p, "expected a local name");

            return false;
        }

        char *ln = ident_text(p);

        advance(p);

        if (!expect_punct(p, ':', "expected : after a local name")) {
            return false;
        }

        th_type *lt = parse_type(p, m);

        if (!lt) {
            return false;
        }

        ir_local_t *l = ir_local_add(f, ln, lt);

        if (!l) {
            fail(p, "duplicate local");

            return false;
        }
        attach_comment(p, &l->annots, lc);
        parse_meta(p, m, &l->span, nullptr, &l->annots);
    }

    uint32_t declared = 0;

    while (!p->failed && is_word(p, "block")) {
        char *bc = take_comment(p);

        advance(p);

        if (p->cur.kind != T_IDENT) {
            fail(p, "expected a block name");

            return false;
        }

        char       *bn = ident_text(p);
        ir_block_t *b  = ir_block_find(f, bn);

        if (!b) {
            b = ir_block_add(f, bn);
        }
        advance(p);

        if (b->index != declared) {
            ir_block_t *other = f->blocks[declared];

            f->blocks[declared] = b;
            f->blocks[b->index] = other;
            other->index        = b->index;
            b->index            = declared;
        }
        declared++;

        if (!expect_punct(p, ':', "expected : after a block name")) {
            return false;
        }
        attach_comment(p, &b->annots, bc);
        parse_meta(p, m, &b->span, &b->origin, &b->annots);

        while (!p->failed && !is_word(p, "block") && !is_punct(p, '}')) {
            if (!parse_instr(p, m, f, b, &fx)) {
                return false;
            }
        }
    }

    for (size_t i = 0; i < fx.n; i++) {
        ir_value_t *v = find_value(f, fx.v[i].name);

        if (!v) {
            fail(p, "unknown value");

            return false;
        }

        ir_operand_t *o = &fx.v[i].in->operands[fx.v[i].slot];

        switch (o->kind) {
        case IR_OPD_PHI_ARM:
            o->phi.value = v;
            break;

        case IR_OPD_NAMED_ARG:
            o->named.value = v;
            break;

        default:
            o->value = v;
            break;
        }
    }

    return expect_punct(p, '}', "expected } to close a function body");
}

ir_module_t *ir_module_parse(ir_ctx_t *ctx, const char *text, int32_t file,
                             const ir_profile_t *const *profiles,
                             size_t n_profiles, ir_diags_t *d)
{
    if (!ctx || !text) {
        return nullptr;
    }

    P p = {.src = text, .len = strlen(text), .line = 1, .col = 1,
           .file = file, .ctx = ctx, .d = d};

    advance(&p);

    char *modc = take_comment(&p);

    if (!eat_word(&p, "module")) {
        fail(&p, "expected module");

        return nullptr;
    }

    if (p.cur.kind != T_IDENT) {
        fail(&p, "expected a module name");

        return nullptr;
    }

    char *name = ident_text(&p);

    advance(&p);

    if (!expect_punct(&p, '{', "expected { to open a module")) {
        return nullptr;
    }

    if (!eat_word(&p, "profile")) {
        fail(&p, "a module must name its profile");

        return nullptr;
    }

    if (p.cur.kind != T_IDENT) {
        fail(&p, "expected a profile name");

        return nullptr;
    }

    char               *pn  = ident_text(&p);
    const ir_profile_t *pro = nullptr;

    for (size_t i = 0; i < n_profiles; i++) {
        if (!strcmp(profiles[i]->name, pn)) {
            pro = profiles[i];
        }
    }

    if (!pro) {
        fail(&p, "unknown profile");

        return nullptr;
    }
    advance(&p);

    char *target = nullptr;

    if (eat_word(&p, "target")) {
        if (p.cur.kind != T_IDENT) {
            fail(&p, "expected a target name");

            return nullptr;
        }
        target = ident_text(&p);
        advance(&p);
    }

    ir_module_t *m = ir_module_new(ctx, name, pro, target);

    if (!m) {
        fail(&p, "module could not be created");

        return nullptr;
    }
    attach_comment(&p, ir_module_annots(m), modc);

    if (eat_word(&p, "meta")) {
        parse_meta(&p, m, nullptr, nullptr, ir_module_annots(m));
    }

    while (!p.failed && !is_punct(&p, '}') && p.cur.kind != T_EOF) {
        char        *c    = take_comment(&p);
        ir_linkage_t link = IR_LINK_EXTERNAL;
        bool         st   = false;
        bool         ex   = false;

        if (eat_word(&p, "static")) {
            link = IR_LINK_INTERNAL;
            st   = true;
        }
        else if (eat_word(&p, "extern")) {
            link = IR_LINK_EXTERN;
            ex   = true;
        }

        if (eat_word(&p, "func")) {
            if (!parse_func(&p, m, link, c)) {
                return nullptr;
            }
            continue;
        }

        if (eat_word(&p, "global")) {
            if (p.cur.kind != T_GLOBAL) {
                fail(&p, "expected a global name");

                return nullptr;
            }

            char *gn = ident_text(&p);

            advance(&p);

            if (!expect_punct(&p, ':', "expected : after a global name")) {
                return nullptr;
            }

            th_type *gt = parse_type(&p, m);

            if (!gt) {
                return nullptr;
            }

            ir_global_t *g = ir_global_add(m, gn, gt);

            if (!g) {
                fail(&p, "duplicate global");

                return nullptr;
            }
            g->is_static = st;
            g->is_extern = ex;
            attach_comment(&p, &g->annots, c);
            parse_meta(&p, m, &g->span, &g->origin, &g->annots);
            continue;
        }

        fail(&p, "expected func, global, or }");

        return nullptr;
    }

    if (!expect_punct(&p, '}', "expected } to close the module")) {
        return nullptr;
    }

    free(p.comment.p);

    return p.failed ? nullptr : m;
}
