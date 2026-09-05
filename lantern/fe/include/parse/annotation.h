#pragma once

#include "parse/types.h"

#define NCC_ANNOT_KINDS(X) \
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
    X(GROUP, "group") \
    X(SOFTLINE, "softline") \
    X(HARDLINE, "hardline") \
    X(ALIGN, "align") \
    X(CONCAT, "concat") \
    X(BLANKLINE, "blankline") \
    X(TYPE, "type") \
    X(INFER, "infer") \
    X(TOKENIZER, "tokenizer") \
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
    X(DISAMBIG, "disambig")

typedef enum {
#define NCC_ANNOT_ENUM(sym, spelling) NCC_ANNOT_##sym,
    NCC_ANNOT_KINDS(NCC_ANNOT_ENUM)
#undef NCC_ANNOT_ENUM
        NCC_ANNOT_NONE,
    NCC_ANNOT_KIND_COUNT,
} ncc_annot_kind_t;

typedef enum {
    NCC_ROLE_BY_INDEX,
    NCC_ROLE_BY_NAME,
} ncc_role_kind_t;

typedef struct {
    ncc_role_kind_t kind;
    int32_t         index;
    ncc_string_t    name;
} ncc_child_ref_t;

#define NCC_CHILD_IX(i) \
    ((ncc_child_ref_t){.kind = NCC_ROLE_BY_INDEX, .index = (i)})
#define NCC_CHILD_NT(n) \
    ((ncc_child_ref_t){.kind = NCC_ROLE_BY_NAME, .index = -1, .name = (n)})
#define NCC_CHILD_NONE NCC_CHILD_IX(-1)

static inline bool
ncc_child_ref_is_set(ncc_child_ref_t r)
{
    return r.kind == NCC_ROLE_BY_INDEX ? r.index >= 0 : r.name.data != nullptr;
}

struct ncc_annotation_t {
    ncc_annot_kind_t kind;
    ncc_child_ref_t  name_ref;
    ncc_child_ref_t  type_ref;
    ncc_child_ref_t  value_ref;
    ncc_child_ref_t  notrivia_ref;
    ncc_child_ref_t  adt_keyword_ref;
    ncc_string_t     scope_tag;
    ncc_string_t     type_spec;
    ncc_string_t     infer_expr;
    ncc_string_t     adt_kind;
    ncc_string_t     visibility_spec;
    ncc_string_t     op_kind;
    ncc_string_t     sym_kind;
    ncc_string_t     disambig_predicate;
    int32_t          penalty_cost;
    int32_t          disambig_cost;
    bool             capture_by_tag;
};

ncc_annotation_t *ncc_annot_new(ncc_annot_kind_t kind);
// Frees the annotation and the strings it copied out of the grammar text.
void              ncc_annot_free(ncc_annotation_t *a);
const char       *ncc_annot_kind_name(ncc_annot_kind_t kind);
ncc_annot_kind_t  ncc_annot_kind_from_name(ncc_string_t name);

void ncc_nt_annotate(ncc_nonterm_t *nt, ncc_annotation_t *a);
void ncc_rule_annotate(ncc_parse_rule_t *rule, ncc_annotation_t *a);
void ncc_grammar_annotate(ncc_grammar_t *g, ncc_annotation_t *a);

size_t            ncc_nt_annot_count(const ncc_nonterm_t *nt);
ncc_annotation_t *ncc_nt_annot_at(const ncc_nonterm_t *nt, size_t i);
size_t            ncc_rule_annot_count(const ncc_parse_rule_t *rule);
ncc_annotation_t *ncc_rule_annot_at(const ncc_parse_rule_t *rule, size_t i);
size_t            ncc_grammar_annot_count(const ncc_grammar_t *g);
ncc_annotation_t *ncc_grammar_annot_at(const ncc_grammar_t *g, size_t i);

ncc_annotation_t *ncc_nt_find_annot(const ncc_nonterm_t *nt,
                                    ncc_annot_kind_t     kind);
ncc_annotation_t *ncc_grammar_find_annot(const ncc_grammar_t *g,
                                         ncc_annot_kind_t     kind);
