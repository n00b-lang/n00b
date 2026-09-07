#include "scanner/lex.h"
#include "scanner/tokenizer.h"
#include "parse/annotation.h"
#include "private/parse/grammar.h"
#include "lib/alloc.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define CAP_COMMENTS 8
#define CAP_STRINGS  16
#define CAP_SIGILS   8
#define CAP_REGEXES  4
#define CAP_HEREDOCS 4
#define CAP_EMBEDS     4
#define CAP_DIRECTIVES 4
#define CAP_RADIXES  8
#define CAP_SUFFIXES 12

typedef struct {
    scan_lex_spec_t    spec;
    scan_lex_comment_t comments[CAP_COMMENTS + 1];
    scan_lex_string_t  strings[CAP_STRINGS + 1];
    scan_lex_sigil_t   sigils[CAP_SIGILS + 1];
    scan_lex_regex_t   regexes[CAP_REGEXES + 1];
    scan_lex_heredoc_t heredocs[CAP_HEREDOCS + 1];
    scan_lex_embed_t     embeds[CAP_EMBEDS + 1];
    scan_lex_directive_t directives[CAP_DIRECTIVES + 1];
    scan_lex_radix_t   radixes[CAP_RADIXES + 1];
    scan_lex_suffix_t  suffixes[CAP_SUFFIXES + 1];

    size_t n_comments;
    size_t n_strings;
    size_t n_sigils;
    size_t n_regexes;
    size_t n_heredocs;
    size_t n_embeds;
    size_t n_directives;
    size_t n_radixes;
    size_t n_suffixes;

    char err[192];
} lex_build_t;

static_assert(offsetof(lex_build_t, spec) == 0,
              "scan_lex_spec_free frees the build through its spec");

static char lex_build_err[192];

const char *
scan_lex_build_error(void)
{
    return lex_build_err[0] ? lex_build_err : nullptr;
}

static bool
named(const parse_lex_opt_t *o, const char *what)
{
    return o->name.data && o->name.u8_bytes == strlen(what)
        && !memcmp(o->name.data, what, o->name.u8_bytes);
}

static core_string_t
arg(const parse_lex_opt_t *o, size_t i)
{
    return i < core_list_len(o->args) ? o->args.data[i]
                                     : (core_string_t){0};
}

static core_string_t
positional(const core_list_t(parse_lex_opt_t) *opts, size_t which)
{
    size_t seen = 0;

    for (size_t i = 0; i < core_list_len(*opts); i++) {
        const parse_lex_opt_t *o = &opts->data[i];

        if (o->name.data) {
            continue;
        }

        if (seen++ == which) {
            return arg(o, 0);
        }
    }

    return (core_string_t){0};
}

static void
oops(lex_build_t *b, const char *kind, core_string_t name)
{
    if (b->err[0]) {
        return;
    }

    snprintf(b->err,
             sizeof(b->err),
             "@lex-%s: unknown option '%.*s'",
             kind,
             name.data ? (int)name.u8_bytes : 1,
             name.data ? name.data : "?");
}

static void
build_comment(lex_build_t *b, const parse_annotation_t *a)
{
    if (b->n_comments >= CAP_COMMENTS) {
        return;
    }

    scan_lex_comment_t *c = &b->comments[b->n_comments];

    c->opener = positional(&a->lex_opts, 0);
    c->closer = positional(&a->lex_opts, 1);
    c->kind   = c->closer.data ? SCAN_LEX_BLOCK : SCAN_LEX_LINE;

    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const parse_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            continue;
        }

        if (named(o, "nested")) {
            c->nested = true;
        }
        else if (named(o, "as")) {
            c->tok = arg(o, 0);
        }
        else {
            oops(b, "comment", o->name);
        }
    }

    if (!c->opener.data) {
        return;
    }

    b->n_comments++;
}

static void
build_string(lex_build_t *b, const parse_annotation_t *a)
{
    if (b->n_strings >= CAP_STRINGS) {
        return;
    }

    scan_lex_string_t *s = &b->strings[b->n_strings];

    s->opener = positional(&a->lex_opts, 0);
    s->closer = positional(&a->lex_opts, 1);

    if (!s->closer.data) {
        s->closer = s->opener;
    }

    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const parse_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            continue;
        }

        if (named(o, "escape")) {
            s->escape = arg(o, 0);
        }
        else if (named(o, "raw")) {
            s->flags |= SCAN_LEX_STR_RAW;
        }
        else if (named(o, "multiline")) {
            s->flags |= SCAN_LEX_STR_MULTILINE;
        }
        else if (named(o, "interp")) {
            s->interp_open  = arg(o, 0);
            s->interp_close = arg(o, 1);
        }
        else if (named(o, "hole_tokens")) {
            s->flags |= SCAN_LEX_STR_HOLE_TOKENS;
        }
        else if (named(o, "as")) {
            s->tok     = arg(o, 0);
            s->tok_mid = arg(o, 1);
            s->tok_end = arg(o, 2);
        }
        else {
            oops(b, "string", o->name);
        }
    }

    if (!s->opener.data) {
        return;
    }

    b->n_strings++;
}

static void
build_number(lex_build_t *b, const parse_annotation_t *a)
{
    scan_lex_number_t *n = &b->spec.number;

    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const parse_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            continue;
        }

        if (named(o, "radix")) {
            if (b->n_radixes < CAP_RADIXES) {
                b->radixes[b->n_radixes].prefix = arg(o, 0);
                b->radixes[b->n_radixes].digits = arg(o, 1);
                b->radixes[b->n_radixes].tok    = arg(o, 2);
                b->n_radixes++;
            }
        }
        else if (named(o, "suffix")) {
            if (b->n_suffixes < CAP_SUFFIXES) {
                b->suffixes[b->n_suffixes].suffix = arg(o, 0);
                b->suffixes[b->n_suffixes].tok    = arg(o, 1);
                b->n_suffixes++;
            }
        }
        else if (named(o, "sep")) {
            n->sep = arg(o, 0);
        }
        else if (named(o, "sign")) {
            n->sign = arg(o, 0);
        }
        else if (named(o, "exp")) {
            n->exponent = arg(o, 0);
        }
        else if (named(o, "int")) {
            n->tok_int = arg(o, 0);
        }
        else if (named(o, "float")) {
            n->tok_float = arg(o, 0);
        }
        else {
            oops(b, "number", o->name);
        }
    }
}

static void
build_layout(lex_build_t *b, const parse_annotation_t *a)
{
    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const parse_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            continue;
        }

        if (named(o, "indent")) {
            b->spec.layout |= SCAN_LEX_LAYOUT_INDENT;

            if (core_list_len(o->args)) {
                b->spec.tok_indent = arg(o, 0);
                b->spec.tok_dedent = arg(o, 1);
            }
        }
        else if (named(o, "newline")) {
            b->spec.layout |= SCAN_LEX_LAYOUT_NEWLINE;

            if (core_list_len(o->args)) {
                b->spec.tok_newline = arg(o, 0);
            }
        }
        else if (named(o, "eof")) {
            b->spec.tok_eof = arg(o, 0);
        }
        else if (named(o, "terminator")) {
            b->spec.tok_terminator   = arg(o, 0);
            b->spec.terminator_after = arg(o, 1);
        }
        else if (named(o, "continue")) {
            b->spec.line_continue = arg(o, 0);
        }
        else if (named(o, "brackets")) {
            b->spec.brackets = arg(o, 0);
        }
        else {
            oops(b, "layout", o->name);
        }
    }
}

static void
build_sigil(lex_build_t *b, const parse_annotation_t *a)
{
    if (b->n_sigils >= CAP_SIGILS) {
        return;
    }

    scan_lex_sigil_t *g = &b->sigils[b->n_sigils];

    g->prefix = positional(&a->lex_opts, 0);

    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const parse_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            continue;
        }

        if (named(o, "as")) {
            g->tok = arg(o, 0);
        }
        else if (named(o, "digits")) {
            g->digits_only = true;
        }
        else {
            oops(b, "sigil", o->name);
        }
    }

    if (!g->prefix.data) {
        return;
    }

    b->n_sigils++;
}

static void
build_regex(lex_build_t *b, const parse_annotation_t *a)
{
    if (b->n_regexes >= CAP_REGEXES) {
        return;
    }

    scan_lex_regex_t *r = &b->regexes[b->n_regexes];

    r->opener = positional(&a->lex_opts, 0);
    r->closer = positional(&a->lex_opts, 1);

    if (!r->closer.data) {
        r->closer = r->opener;
    }

    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const parse_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            continue;
        }

        if (named(o, "as")) {
            r->tok = arg(o, 0);
        }
        else if (named(o, "flags")) {
            r->suffix_chars = arg(o, 0);
        }
        else {
            oops(b, "regex", o->name);
        }
    }

    if (!r->opener.data) {
        return;
    }

    b->n_regexes++;
}

static void
build_heredoc(lex_build_t *b, const parse_annotation_t *a)
{
    if (b->n_heredocs >= CAP_HEREDOCS) {
        return;
    }

    scan_lex_heredoc_t *h = &b->heredocs[b->n_heredocs];

    h->opener = positional(&a->lex_opts, 0);

    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const parse_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            continue;
        }

        if (named(o, "as")) {
            h->tok = arg(o, 0);
        }
        else if (named(o, "strip")) {
            h->indent_strip = true;
        }
        else {
            oops(b, "heredoc", o->name);
        }
    }

    if (!h->opener.data) {
        return;
    }

    b->n_heredocs++;
}

static void
build_embed(lex_build_t *b, const parse_annotation_t *a)
{
    if (b->n_embeds >= CAP_EMBEDS) {
        return;
    }

    scan_lex_embed_t *e = &b->embeds[b->n_embeds];

    e->open  = positional(&a->lex_opts, 0);
    e->close = positional(&a->lex_opts, 1);

    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const parse_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            continue;
        }

        if (named(o, "as")) {
            e->tok = arg(o, 0);
        }
        else if (named(o, "open_as")) {
            e->open_tok = arg(o, 0);
        }
        else if (named(o, "close_as")) {
            e->close_tok = arg(o, 0);
        }
        else {
            oops(b, "embed", o->name);
        }
    }

    if (!e->open.data) {
        return;
    }

    b->n_embeds++;
}

static void
build_directive(lex_build_t *b, const parse_annotation_t *a)
{
    if (b->n_directives >= CAP_DIRECTIVES) {
        return;
    }

    scan_lex_directive_t *d = &b->directives[b->n_directives];

    d->open = positional(&a->lex_opts, 0);

    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const parse_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            continue;
        }

        if (named(o, "eol")) {
            d->tok_eol = arg(o, 0);
        }
        else if (named(o, "as")) {
            d->tok_other = arg(o, 0);
        }
        else {
            oops(b, "directive", o->name);
        }
    }

    if (!d->open.data) {
        return;
    }

    b->n_directives++;
}

static void
build_ident(lex_build_t *b, const parse_annotation_t *a)
{
    for (size_t i = 0; i < core_list_len(a->lex_opts); i++) {
        const parse_lex_opt_t *o = &a->lex_opts.data[i];

        if (!o->name.data) {
            continue;
        }

        if (named(o, "as")) {
            b->spec.tok_ident = arg(o, 0);
        }
        else if (named(o, "start")) {
            b->spec.ident_start_extra = arg(o, 0);
        }
        else if (named(o, "extra")) {
            b->spec.ident_extra = arg(o, 0);
        }
        else if (named(o, "upper")) {
            b->spec.tok_ident_upper = arg(o, 0);
        }
        else {
            oops(b, "ident", o->name);
        }
    }
}

const scan_lex_spec_t *
scan_lex_spec_from_grammar(parse_grammar_t *g)
{
    size_t n = parse_grammar_annot_count(g);
    bool   any = false;

    for (size_t i = 0; i < n; i++) {
        parse_annotation_t *a = parse_grammar_annot_at(g, i);

        if (a && a->kind >= PARSE_ANNOT_LEX_COMMENT
            && a->kind <= PARSE_ANNOT_LEX_DIRECTIVE) {
            any = true;

            break;
        }
    }

    if (!any) {
        return nullptr;
    }

    lex_build_t *b = core_alloc(lex_build_t);

    b->spec.name      = scan_grammar_tokenizer(g);
    b->spec.tok_ident = CORE_STRING_STATIC("IDENTIFIER");

    for (size_t i = 0; i < n; i++) {
        parse_annotation_t *a = parse_grammar_annot_at(g, i);

        if (!a) {
            continue;
        }

        switch (a->kind) {
        case PARSE_ANNOT_LEX_COMMENT: build_comment(b, a); break;
        case PARSE_ANNOT_LEX_STRING:  build_string(b, a);  break;
        case PARSE_ANNOT_LEX_NUMBER:  build_number(b, a);  break;
        case PARSE_ANNOT_LEX_LAYOUT:  build_layout(b, a);  break;
        case PARSE_ANNOT_LEX_SIGIL:   build_sigil(b, a);   break;
        case PARSE_ANNOT_LEX_REGEX:   build_regex(b, a);   break;
        case PARSE_ANNOT_LEX_HEREDOC: build_heredoc(b, a); break;
        case PARSE_ANNOT_LEX_EMBED:   build_embed(b, a);   break;
        case PARSE_ANNOT_LEX_DIRECTIVE: build_directive(b, a); break;
        case PARSE_ANNOT_LEX_IDENT:   build_ident(b, a);   break;
        default: break;
        }
    }

    if (b->err[0]) {
        snprintf(lex_build_err, sizeof(lex_build_err), "%s", b->err);
        core_free(b);

        return nullptr;
    }

    lex_build_err[0] = '\0';

    b->spec.comments = b->n_comments ? b->comments : nullptr;
    b->spec.strings  = b->n_strings ? b->strings : nullptr;
    b->spec.sigils   = b->n_sigils ? b->sigils : nullptr;
    b->spec.regexes  = b->n_regexes ? b->regexes : nullptr;
    b->spec.heredocs = b->n_heredocs ? b->heredocs : nullptr;
    b->spec.embeds   = b->n_embeds ? b->embeds : nullptr;
    b->spec.directives = b->n_directives ? b->directives : nullptr;

    b->spec.number.radixes  = b->n_radixes ? b->radixes : nullptr;
    b->spec.number.suffixes = b->n_suffixes ? b->suffixes : nullptr;

    return &b->spec;
}

void
scan_lex_spec_free(const scan_lex_spec_t *spec)
{
    if (!spec) {
        return;
    }

    core_free((void *)(uintptr_t)(const void *)spec);
}
