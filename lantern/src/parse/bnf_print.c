#include "parse/bnf.h"
#include "parse/annotation.h"
#include "private/parse/grammar.h"
#include "lib/alloc.h"
#include "lib/buffer.h"

#include <string.h>

static void
put_str(core_buffer_t *b, core_string_t s)
{
    if (s.data) {
        core_buffer_append(b, s.data, s.u8_bytes);
    }
}

static core_string_t
class_name(parse_char_class_t cc)
{
    switch (cc) {
    case PARSE_CC_ASCII_DIGIT:      return CORE_STRING_STATIC("__DIGIT");
    case PARSE_CC_ASCII_ALPHA:      return CORE_STRING_STATIC("__ALPHA");
    case PARSE_CC_ASCII_UPPER:      return CORE_STRING_STATIC("__UPPER");
    case PARSE_CC_ASCII_LOWER:      return CORE_STRING_STATIC("__LOWER");
    case PARSE_CC_HEX_DIGIT:        return CORE_STRING_STATIC("__HEX");
    case PARSE_CC_NONZERO_DIGIT:
        return CORE_STRING_STATIC("__NONZERO_DIGIT");
    case PARSE_CC_WHITESPACE:       return CORE_STRING_STATIC("__WHITESPACE");
    case PARSE_CC_ID_START:         return CORE_STRING_STATIC("__ID_START");
    case PARSE_CC_ID_CONTINUE:      return CORE_STRING_STATIC("__ID_CONTINUE");
    case PARSE_CC_PRINTABLE:        return CORE_STRING_STATIC("__PRINTABLE");
    case PARSE_CC_UNICODE_DIGIT:
        return CORE_STRING_STATIC("__UNICODE_DIGIT");
    case PARSE_CC_JSON_STRING_CHAR: return CORE_STRING_STATIC("__JSON_STR");
    case PARSE_CC_REGEX_BODY_CHAR:  return CORE_STRING_STATIC("__REGEX_STR");
    default:                        return (core_string_t){};
    }
}

static void
put_annot(core_buffer_t *b, const parse_annotation_t *a)
{
    core_buffer_putc(b, '@');
    core_buffer_append_string(b, parse_annot_kind_name(a->kind));

    if (a->args_text.data) {
        core_buffer_putc(b, '(');
        put_str(b, a->args_text);
        core_buffer_putc(b, ')');
    }
}

static void
put_annots(core_buffer_t *b, const core_list_t(parse_annotation_ptr_t) *list)
{
    for (size_t i = 0; i < list->len; i++) {
        core_buffer_putc(b, ' ');
        put_annot(b, list->data[i]);
    }
}

static bool
same_annots(const parse_rule_t *a, const parse_rule_t *b)
{
    if (!a || !b || a->annotations.len != b->annotations.len) {
        return false;
    }

    for (size_t i = 0; i < a->annotations.len; i++) {
        const parse_annotation_t *x = a->annotations.data[i];
        const parse_annotation_t *y = b->annotations.data[i];

        if (x->kind != y->kind
            || !core_string_eq(x->args_text, y->args_text)) {
            return false;
        }
    }

    return true;
}

static void put_items(core_buffer_t *b, parse_grammar_t *g,
                      const core_list_t(parse_match_t) *items);

static bool
is_bnf_anon(const parse_nonterm_t *nt)
{
    static const char prefix[] = "$$bnf_anon_";

    return nt && nt->name.data && nt->name.u8_bytes >= sizeof(prefix) - 1
           && !memcmp(nt->name.data, prefix, sizeof(prefix) - 1);
}

static void
put_bnf_anon(core_buffer_t *b, parse_grammar_t *g, const parse_nonterm_t *nt)
{
    core_buffer_putc(b, '(');

    for (size_t i = 0; i < core_list_len(nt->rule_ids); i++) {
        parse_rule_t *r = parse_get_rule(g, nt->rule_ids.data[i]);

        if (i) {
            core_buffer_puts(b, " | ");
        }

        if (r) {
            put_items(b, g, &r->contents);
        }
    }

    core_buffer_putc(b, ')');
}

static void
put_group(core_buffer_t *b, parse_grammar_t *g, const parse_rule_group_t *grp)
{
    parse_nonterm_t *body = parse_get_nonterm(g, grp->contents_id);
    parse_rule_t    *only = body && core_list_len(body->rule_ids) == 1
                              ? parse_get_rule(g, body->rule_ids.data[0])
                              : nullptr;
    parse_match_t   *item = only && core_list_len(only->contents) == 1
                              ? &only->contents.data[0]
                              : nullptr;
    parse_nonterm_t *anon = item && item->kind == PARSE_MATCH_NT
                              ? parse_get_nonterm(g, item->nt_id)
                              : nullptr;

    if (is_bnf_anon(anon)) {
        put_bnf_anon(b, g, anon);
    }
    else {
        core_buffer_putc(b, '(');

        for (size_t i = 0; body && i < core_list_len(body->rule_ids); i++) {
            parse_rule_t *r = parse_get_rule(g, body->rule_ids.data[i]);

            if (i) {
                core_buffer_puts(b, " | ");
            }

            if (r) {
                put_items(b, g, &r->contents);
            }
        }

        core_buffer_putc(b, ')');
    }

    if (grp->min == 0 && grp->max == 1) {
        core_buffer_putc(b, '?');
    }
    else if (grp->min == 0 && grp->max == 0) {
        core_buffer_putc(b, '*');
    }
    else if (grp->min == 1 && grp->max == 0) {
        core_buffer_putc(b, '+');
    }
}

static void
put_terminal(core_buffer_t *b, parse_grammar_t *g, int64_t tid)
{
    parse_terminal_t *t = tid > PARSE_TOK_START_ID ? parse_get_terminal(g, tid)
                                               : nullptr;

    if (t && t->value.data) {
        core_buffer_puts(b, "%\"");
        put_str(b, t->value);
        core_buffer_putc(b, '"');

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

        core_buffer_putc(b, '"');
        core_buffer_append(b, utf8, (size_t)n);
        core_buffer_putc(b, '"');

        return;
    }

    core_buffer_puts(b, "%UNKNOWN");
}

static void
put_items(core_buffer_t *b, parse_grammar_t *g,
          const core_list_t(parse_match_t) *items)
{
    for (size_t i = 0; i < items->len; i++) {
        const parse_match_t *m = &items->data[i];

        if (i) {
            core_buffer_putc(b, ' ');
        }

        switch (m->kind) {
        case PARSE_MATCH_EMPTY:
            core_buffer_puts(b, "\"\"");
            break;

        case PARSE_MATCH_NT: {
            parse_nonterm_t *nt = parse_get_nonterm(g, m->nt_id);

            if (is_bnf_anon(nt)) {
                put_bnf_anon(b, g, nt);
            }
            else {
                core_buffer_putc(b, '<');
                put_str(b, nt ? nt->name : core_string_empty());
                core_buffer_putc(b, '>');
            }
            break;
        }

        case PARSE_MATCH_TERMINAL:
            put_terminal(b, g, m->terminal_id);
            break;

        case PARSE_MATCH_ANY:
            core_buffer_puts(b, "__PRINTABLE");
            break;

        case PARSE_MATCH_CLASS: {
            core_string_t nm = class_name(m->char_class);

            if (nm.data) {
                core_buffer_append_string(b, nm);
            }
            else {
                core_buffer_puts(b, "__PRINTABLE");
            }
            break;
        }

        case PARSE_MATCH_SET:
            core_buffer_puts(b, "%SET");
            break;

        case PARSE_MATCH_GROUP:
            put_group(b, g, (const parse_rule_group_t *)m->group);
            break;
        }
    }
}

core_string_t
parse_bnf_print(parse_grammar_t *g, int32_t unit)
{
    if (!g) {
        return (core_string_t){};
    }

    core_buffer_t *b = core_buffer_empty();

    for (size_t i = 0; i < parse_grammar_annot_count(g); i++) {
        put_annot(b, parse_grammar_annot_at(g, i));
        core_buffer_putc(b, '\n');
    }

    if (parse_grammar_annot_count(g)) {
        core_buffer_putc(b, '\n');
    }

    for (size_t i = 0; i < core_list_len(g->nt_list); i++) {
        parse_nonterm_t *nt = g->nt_list.data[i];

        if (!nt->defined_here || nt->group_nt
            || (unit && nt->unit != unit)) {
            continue;
        }

        if (nt->name.data && nt->name.u8_bytes > 1
            && nt->name.data[0] == '$' && nt->name.data[1] == '$') {
            continue;
        }

        size_t nrules = core_list_len(nt->rule_ids);
        size_t j      = 0;

        while (j < nrules) {
            parse_rule_t *first = parse_get_rule(g, nt->rule_ids.data[j]);
            size_t            k     = j + 1;

            while (k < nrules
                   && same_annots(first,
                                  parse_get_rule(g, nt->rule_ids.data[k]))) {
                k++;
            }

            core_buffer_putc(b, '<');
            put_str(b, nt->name);
            core_buffer_putc(b, '>');
            put_annots(b, &nt->annotations);

            if (first) {
                put_annots(b, &first->annotations);
            }

            core_buffer_puts(b, " ::= ");

            for (size_t r = j; r < k; r++) {
                parse_rule_t *rp = parse_get_rule(g, nt->rule_ids.data[r]);

                if (r > j) {
                    core_buffer_puts(b, "\n    | ");
                }

                if (rp) {
                    put_items(b, g, &rp->contents);
                }
            }

            core_buffer_putc(b, '\n');

            j = k;
        }

        if (!nrules) {
            core_buffer_putc(b, '<');
            put_str(b, nt->name);
            core_buffer_putc(b, '>');
            put_annots(b, &nt->annotations);
            core_buffer_puts(b, " ::= \"\"\n");
        }
    }

    return core_buffer_take_string(b);
}
