#pragma once

#include "parse/types.h"
#include "lib/list.h"

#define PARSE_ANNOT_KINDS(X) \
    X(SCOPE_OPEN, "scope") \
    X(DECLARES, "declares") \
    X(ASSIGNS, "assigns") \
    X(BRANCH, "branch") \
    X(LOOP, "loop") \
    X(JUMP, "jump") \
    X(SWITCH, "switch") \
    X(INDENT, "indent") \
    X(TRIVIA_INDENT, "trivia_indent") \
    X(NEWLINE, "newline") \
    X(SPACE, "space") \
    X(NOSPACE, "nospace") \
    X(ADJACENT, "adjacent") \
    X(GROUP, "group") \
    X(HARDLINE, "hardline") \
    X(ALIGN, "align") \
    X(BLANKLINE, "blankline") \
    X(TYPE, "type") \
    X(TOKENIZER, "tokenizer") \
    X(LEXER, "lexer") \
    X(FIELD, "field") \
    X(VISIBILITY, "visibility") \
    X(OPERATOR, "operator") \
    X(CALL, "call") \
    X(VARREF, "varref") \
    X(EXPOSES, "exposes") \
    X(DISAMBIG, "disambig") \
    X(NAMESPACE, "namespace") \
    X(DIRECTION, "direction") \
    X(PASS_BY, "pass_by") \
    X(DECLARATION_POINT, "declaration_point") \
    X(STATEMENT_END, "statement_end") \
    X(CONTINUE_AFTER, "continue_after") \
    X(DISCARDS, "discards")

typedef enum {
#define PARSE_ANNOT_ENUM(sym, spelling) PARSE_ANNOT_##sym,
    PARSE_ANNOT_KINDS(PARSE_ANNOT_ENUM)
#undef PARSE_ANNOT_ENUM
        PARSE_ANNOT_NONE,
    PARSE_ANNOT_KIND_COUNT,
} parse_annot_kind_t;

struct parse_annotation_t {
    parse_annot_kind_t kind;
    core_string_t      source_origin;
    core_string_t      args_text;
    int32_t            name_ref;
    int32_t            type_ref;
    int32_t            value_ref;
    core_string_t      tag;
    int32_t            cost;
    core_list_t(core_string_t) tags;
};

parse_annotation_t *parse_annot_new(parse_annot_kind_t kind);
parse_annotation_t *parse_annot_clone(const parse_annotation_t *a);
void              parse_annot_free(parse_annotation_t *a);
parse_annot_kind_t  parse_annot_kind_from_name(core_string_t name);

void parse_rule_annotate(parse_rule_t *rule, parse_annotation_t *a);
void parse_grammar_annotate(parse_grammar_t *g, parse_annotation_t *a);

size_t            parse_nt_annot_count(const parse_nonterm_t *nt);
parse_annotation_t *parse_nt_annot_at(const parse_nonterm_t *nt, size_t i);
size_t            parse_rule_annot_count(const parse_rule_t *rule);
parse_annotation_t *parse_rule_annot_at(const parse_rule_t *rule, size_t i);
size_t            parse_grammar_annot_count(const parse_grammar_t *g);
parse_annotation_t *parse_grammar_annot_at(const parse_grammar_t *g, size_t i);

parse_annotation_t *parse_rule_find_annot(parse_grammar_t    *g,
                                      const parse_rule_t *rule,
                                      parse_annot_kind_t  kind);
parse_annotation_t *parse_nt_find_direct_annot(
    const parse_nonterm_t *nt, parse_annot_kind_t kind);
parse_annotation_t *parse_grammar_find_annot(parse_grammar_t   *g,
                                         parse_annot_kind_t kind);

