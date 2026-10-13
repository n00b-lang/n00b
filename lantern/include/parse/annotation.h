#pragma once

#include "parse/types.h"
#include "lib/list.h"

#define PARSE_ANNOT_KINDS(X) \
    X(SCOPE_OPEN, "scope") \
    X(DECLARES, "declares") \
    X(TYPE_DECL, "type_decl") \
    X(ASSIGNS, "assigns") \
    X(BRANCH, "branch") \
    X(LOOP, "loop") \
    X(JUMP, "jump") \
    X(SWITCH, "switch") \
    X(CAPTURE, "capture") \
    X(INDENT, "indent") \
    X(NEWLINE, "newline") \
    X(SPACE, "space") \
    X(NOSPACE, "nospace") \
    X(ADJACENT, "adjacent") \
    X(GROUP, "group") \
    X(SOFTLINE, "softline") \
    X(HARDLINE, "hardline") \
    X(ALIGN, "align") \
    X(CONCAT, "concat") \
    X(BLANKLINE, "blankline") \
    X(TYPE, "type") \
    X(INFER, "infer") \
    X(TOKENIZER, "tokenizer") \
    X(LEXER, "lexer") \
    X(ADT, "adt") \
    X(FIELD, "field") \
    X(METHOD, "method") \
    X(INHERITS, "inherits") \
    X(IMPLEMENTS, "implements") \
    X(VISIBILITY, "visibility") \
    X(STATIC, "static") \
    X(ABSTRACT, "abstract") \
    X(OPERATOR, "operator") \
    X(LITERAL, "literal") \
    X(CALL, "call") \
    X(VARREF, "varref") \
    X(PENALTY, "penalty") \
    X(NOTRIVIA, "notrivia") \
    X(EXPOSES, "exposes") \
    X(RECORD, "record") \
    X(DISAMBIG, "disambig") \
    X(MUTABILITY, "mutability") \
    X(NAMESPACE, "namespace") \
    X(DIRECTION, "direction") \
    X(PASS_BY, "pass_by") \
    X(CONSTRAIN, "constrain") \
    X(EMIT, "emit") \
    X(SIZE, "size") \
    X(ALIGNMENT, "alignment") \
    X(OFFSET, "offset") \
    X(WIDTH, "width")

typedef enum {
#define PARSE_ANNOT_ENUM(sym, spelling) PARSE_ANNOT_##sym,
    PARSE_ANNOT_KINDS(PARSE_ANNOT_ENUM)
#undef PARSE_ANNOT_ENUM
        PARSE_ANNOT_NONE,
    PARSE_ANNOT_KIND_COUNT,
} parse_annot_kind_t;

typedef enum {
    PARSE_ROLE_BY_INDEX,
    PARSE_ROLE_BY_NAME,
} parse_role_kind_t;

typedef struct {
    th_con_kind              kind;
    core_string_t             name;
    core_list_t(core_string_t) types;
    uint64_t                 index;
} parse_con_spec_t;

core_list_decl(parse_con_spec_t);

typedef struct {
    parse_role_kind_t kind;
    int32_t         index;
    core_string_t    name;
} parse_child_ref_t;

core_list_decl(parse_child_ref_t);

#define PARSE_CHILD_IX(i) \
    ((parse_child_ref_t){.kind = PARSE_ROLE_BY_INDEX, .index = (i)})
#define PARSE_CHILD_NT(n) \
    ((parse_child_ref_t){.kind = PARSE_ROLE_BY_NAME, .index = -1, .name = (n)})
#define PARSE_CHILD_NONE PARSE_CHILD_IX(-1)

static inline bool
parse_child_ref_is_set(parse_child_ref_t r)
{
    return r.kind == PARSE_ROLE_BY_INDEX ? r.index >= 0 : r.name.data != nullptr;
}

typedef struct parse_infer_expr_t parse_infer_expr_t;

struct parse_annotation_t {
    parse_annot_kind_t  kind;
    core_string_t       source_origin;
    parse_infer_expr_t *infer_ast;
    core_string_t      args_text;
    parse_child_ref_t  name_ref;
    parse_child_ref_t  type_ref;
    parse_child_ref_t  value_ref;
    parse_child_ref_t  notrivia_ref;
    parse_child_ref_t  adt_keyword_ref;
    core_string_t     scope_tag;
    core_string_t     type_spec;
    core_string_t     infer_expr;
    core_string_t     adt_kind;
    core_string_t     visibility_spec;
    core_string_t     op_kind;
    core_string_t     sym_kind;
    core_string_t     disambig_predicate;
    int32_t          penalty_cost;
    int32_t          disambig_cost;
    bool             capture_by_tag;

    core_string_t     mutability;
    core_string_t     ns_name;
    core_string_t     direction;
    core_string_t     pass_by;
    core_string_t     emit_action;
    core_list_t(parse_child_ref_t) emit_operands;
    core_list_t(parse_con_spec_t) constraints;

    parse_child_ref_t  val_ref;
    int64_t          val_int;
    core_string_t     val_expr;
    bool             has_val_int;
};

parse_annotation_t *parse_annot_new(parse_annot_kind_t kind);
parse_annotation_t *parse_annot_clone(const parse_annotation_t *a);
void              parse_annot_free(parse_annotation_t *a);
const char       *parse_annot_kind_name(parse_annot_kind_t kind);
parse_annot_kind_t  parse_annot_kind_from_name(core_string_t name);

void parse_nt_annotate(parse_nonterm_t *nt, parse_annotation_t *a);
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
