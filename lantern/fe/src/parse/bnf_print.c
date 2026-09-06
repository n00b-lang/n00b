#include "parse/bnf.h"
#include "parse/annotation.h"
#include "internal/parse/grammar_internal.h"
#include "lib/alloc.h"
#include "lib/buffer.h"

#include <string.h>

static void
put_str(ncc_buffer_t *b, ncc_string_t s)
{
    if (s.data) {
        ncc_buffer_append(b, s.data, s.u8_bytes);
    }
}

static const char *
class_name(ncc_char_class_t cc)
{
    switch (cc) {
    case NCC_CC_ASCII_DIGIT:       return "__DIGIT";
    case NCC_CC_ASCII_ALPHA:       return "__ALPHA";
    case NCC_CC_ASCII_UPPER:       return "__UPPER";
    case NCC_CC_ASCII_LOWER:       return "__LOWER";
    case NCC_CC_HEX_DIGIT:         return "__HEX";
    case NCC_CC_NONZERO_DIGIT:     return "__NONZERO_DIGIT";
    case NCC_CC_WHITESPACE:        return "__WHITESPACE";
    case NCC_CC_ID_START:          return "__ID_START";
    case NCC_CC_ID_CONTINUE:       return "__ID_CONTINUE";
    case NCC_CC_PRINTABLE:         return "__PRINTABLE";
    case NCC_CC_UNICODE_DIGIT:     return "__UNICODE_DIGIT";
    case NCC_CC_JSON_STRING_CHAR:  return "__JSON_STR";
    case NCC_CC_REGEX_BODY_CHAR:   return "__REGEX_STR";
    default:                       return nullptr;
    }
}

static void
put_annot(ncc_buffer_t *b, const ncc_annotation_t *a)
{
    ncc_buffer_printf(b, "@%s", ncc_annot_kind_name(a->kind));

    if (a->args_text.data) {
        ncc_buffer_putc(b, '(');
        put_str(b, a->args_text);
        ncc_buffer_putc(b, ')');
    }
}

static void
put_annots(ncc_buffer_t *b, const ncc_list_t(ncc_annotation_ptr_t) *list)
{
    for (size_t i = 0; i < list->len; i++) {
        ncc_buffer_putc(b, ' ');
        put_annot(b, list->data[i]);
    }
}

static bool
same_annots(const ncc_parse_rule_t *a, const ncc_parse_rule_t *b)
{
    if (!a || !b || a->annotations.len != b->annotations.len) {
        return false;
    }

    for (size_t i = 0; i < a->annotations.len; i++) {
        const ncc_annotation_t *x = a->annotations.data[i];
        const ncc_annotation_t *y = b->annotations.data[i];

        if (x->kind != y->kind
            || !ncc_string_eq(x->args_text, y->args_text)) {
            return false;
        }
    }

    return true;
}

static void put_items(ncc_buffer_t *b, ncc_grammar_t *g,
                      const ncc_list_t(ncc_match_t) *items);

static void
put_group(ncc_buffer_t *b, ncc_grammar_t *g, const ncc_rule_group_t *grp)
{
    ncc_nonterm_t *body = ncc_get_nonterm(g, grp->contents_id);

    ncc_buffer_putc(b, '(');

    for (size_t i = 0; body && i < ncc_list_len(body->rule_ids); i++) {
        ncc_parse_rule_t *r = ncc_get_rule(g, body->rule_ids.data[i]);

        if (i) {
            ncc_buffer_puts(b, " | ");
        }

        if (r) {
            put_items(b, g, &r->contents);
        }
    }

    ncc_buffer_putc(b, ')');

    if (grp->min == 0 && grp->max == 1) {
        ncc_buffer_putc(b, '?');
    }
    else if (grp->min == 0 && grp->max == 0) {
        ncc_buffer_putc(b, '*');
    }
    else if (grp->min == 1 && grp->max == 0) {
        ncc_buffer_putc(b, '+');
    }
}

static void
put_terminal(ncc_buffer_t *b, ncc_grammar_t *g, int64_t tid)
{
    ncc_terminal_t *t = tid > NCC_TOK_START_ID ? ncc_get_terminal(g, tid)
                                               : nullptr;

    if (t && t->value.data) {
        ncc_buffer_puts(b, "%\"");
        put_str(b, t->value);
        ncc_buffer_putc(b, '"');

        return;
    }

    if (tid > 0 && tid < 0x110000) {
        char utf8[5] = {0};
        int  n       = 0;

        if (tid < 0x80) {
            utf8[n++] = (char)tid;
        }
        else if (tid < 0x800) {
            utf8[n++] = (char)(0xC0 | (tid >> 6));
            utf8[n++] = (char)(0x80 | (tid & 0x3F));
        }
        else if (tid < 0x10000) {
            utf8[n++] = (char)(0xE0 | (tid >> 12));
            utf8[n++] = (char)(0x80 | ((tid >> 6) & 0x3F));
            utf8[n++] = (char)(0x80 | (tid & 0x3F));
        }
        else {
            utf8[n++] = (char)(0xF0 | (tid >> 18));
            utf8[n++] = (char)(0x80 | ((tid >> 12) & 0x3F));
            utf8[n++] = (char)(0x80 | ((tid >> 6) & 0x3F));
            utf8[n++] = (char)(0x80 | (tid & 0x3F));
        }

        ncc_buffer_putc(b, '"');
        ncc_buffer_append(b, utf8, (size_t)n);
        ncc_buffer_putc(b, '"');

        return;
    }

    ncc_buffer_puts(b, "%UNKNOWN");
}

static void
put_items(ncc_buffer_t *b, ncc_grammar_t *g,
          const ncc_list_t(ncc_match_t) *items)
{
    for (size_t i = 0; i < items->len; i++) {
        const ncc_match_t *m = &items->data[i];

        if (i) {
            ncc_buffer_putc(b, ' ');
        }

        switch (m->kind) {
        case NCC_MATCH_EMPTY:
            ncc_buffer_puts(b, "\"\"");
            break;

        case NCC_MATCH_NT: {
            ncc_nonterm_t *nt = ncc_get_nonterm(g, m->nt_id);

            ncc_buffer_putc(b, '<');
            put_str(b, nt ? nt->name : ncc_string_empty());
            ncc_buffer_putc(b, '>');
            break;
        }

        case NCC_MATCH_TERMINAL:
            put_terminal(b, g, m->terminal_id);
            break;

        case NCC_MATCH_ANY:
            ncc_buffer_puts(b, "__PRINTABLE");
            break;

        case NCC_MATCH_CLASS: {
            const char *nm = class_name(m->char_class);

            ncc_buffer_puts(b, nm ? nm : "__PRINTABLE");
            break;
        }

        case NCC_MATCH_SET:
            ncc_buffer_puts(b, "%SET");
            break;

        case NCC_MATCH_GROUP:
            put_group(b, g, (const ncc_rule_group_t *)m->group);
            break;
        }
    }
}

char *
ncc_bnf_print(ncc_grammar_t *g, int32_t unit)
{
    if (!g) {
        return nullptr;
    }

    ncc_buffer_t *b = ncc_buffer_empty();

    for (size_t i = 0; i < ncc_grammar_annot_count(g); i++) {
        put_annot(b, ncc_grammar_annot_at(g, i));
        ncc_buffer_putc(b, '\n');
    }

    if (ncc_grammar_annot_count(g)) {
        ncc_buffer_putc(b, '\n');
    }

    for (size_t i = 0; i < ncc_list_len(g->nt_list); i++) {
        ncc_nonterm_t *nt = g->nt_list.data[i];

        if (!nt->defined_here || nt->group_nt
            || (unit && nt->unit != unit)) {
            continue;
        }

        if (nt->name.data && nt->name.u8_bytes > 1
            && nt->name.data[0] == '$' && nt->name.data[1] == '$') {
            continue;
        }

        size_t nrules = ncc_list_len(nt->rule_ids);
        size_t j      = 0;

        while (j < nrules) {
            ncc_parse_rule_t *first = ncc_get_rule(g, nt->rule_ids.data[j]);
            size_t            k     = j + 1;

            while (k < nrules
                   && same_annots(first,
                                  ncc_get_rule(g, nt->rule_ids.data[k]))) {
                k++;
            }

            ncc_buffer_putc(b, '<');
            put_str(b, nt->name);
            ncc_buffer_putc(b, '>');
            put_annots(b, &nt->annotations);

            if (first) {
                put_annots(b, &first->annotations);
            }

            ncc_buffer_puts(b, " ::= ");

            for (size_t r = j; r < k; r++) {
                ncc_parse_rule_t *rp = ncc_get_rule(g, nt->rule_ids.data[r]);

                if (r > j) {
                    ncc_buffer_puts(b, "\n    | ");
                }

                if (rp) {
                    put_items(b, g, &rp->contents);
                }
            }

            ncc_buffer_putc(b, '\n');

            j = k;
        }

        if (!nrules) {
            ncc_buffer_putc(b, '<');
            put_str(b, nt->name);
            ncc_buffer_putc(b, '>');
            put_annots(b, &nt->annotations);
            ncc_buffer_puts(b, " ::= \"\"\n");
        }
    }

    return ncc_buffer_take(b);
}
