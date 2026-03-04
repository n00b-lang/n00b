#pragma once

/**
 * @file annotation.h
 * @brief Grammar annotations for the slay parser system.
 *
 * Annotations attach semantic information to non-terminals: scope
 * management, symbol declarations, pretty-printing hints, and more.
 */

#include "slay/types.h"

// ============================================================================
// Annotation enums
// ============================================================================

/** @brief Kind of annotation attached to a non-terminal. */
typedef enum {
    NCC_ANNOT_SCOPE_OPEN,
    NCC_ANNOT_DECLARES,
    NCC_ANNOT_TYPE_DECL,
    NCC_ANNOT_ASSIGNS,
    NCC_ANNOT_BRANCH,
    NCC_ANNOT_LOOP,
    NCC_ANNOT_JUMP,
    NCC_ANNOT_SWITCH,
    NCC_ANNOT_CAPTURE,
    NCC_ANNOT_INDENT,
    NCC_ANNOT_NEWLINE,
    NCC_ANNOT_SPACE,
    NCC_ANNOT_NOSPACE,
    NCC_ANNOT_GROUP,
    NCC_ANNOT_SOFTLINE,
    NCC_ANNOT_HARDLINE,
    NCC_ANNOT_ALIGN,
    NCC_ANNOT_CONCAT,
    NCC_ANNOT_BLANKLINE,
    NCC_ANNOT_TYPE,
    NCC_ANNOT_INFER,
    NCC_ANNOT_TOKENIZER,
    NCC_ANNOT_ADT,
    NCC_ANNOT_FIELD,
    NCC_ANNOT_METHOD,
    NCC_ANNOT_INHERITS,
    NCC_ANNOT_IMPLEMENTS,
    NCC_ANNOT_VISIBILITY,
    NCC_ANNOT_STATIC,
    NCC_ANNOT_ABSTRACT,
    NCC_ANNOT_OPERATOR,
    NCC_ANNOT_LITERAL,
    NCC_ANNOT_CALL,
    NCC_ANNOT_VARREF,
    NCC_ANNOT_PENALTY,
    NCC_ANNOT_RECLASSIFY,
    NCC_ANNOT_RECLASSIFY_LIST,
    NCC_ANNOT_NONE,
} ncc_annot_kind_t;

/** @brief How a child is referenced in an annotation. */
typedef enum {
    NCC_ROLE_BY_INDEX,
    NCC_ROLE_BY_NAME,
} ncc_role_kind_t;

/** @brief Reference to a child node in an annotation. */
typedef struct {
    ncc_role_kind_t kind;
    union {
        int32_t       index;
        ncc_string_t name;
    };
} ncc_child_ref_t;

#define NCC_CHILD_IX(i)   ((ncc_child_ref_t){.kind = NCC_ROLE_BY_INDEX, .index = (i)})
#define NCC_CHILD_NT(n)   ((ncc_child_ref_t){.kind = NCC_ROLE_BY_NAME,  .name = (n)})
#define NCC_CHILD_NONE    NCC_CHILD_IX(-1)

// ============================================================================
// Annotation struct
// ============================================================================

/** @brief A single annotation attached to a rule or non-terminal. */
struct ncc_annotation_t {
    ncc_annot_kind_t  kind;
    ncc_child_ref_t   name_ref;
    ncc_child_ref_t   type_ref;
    ncc_child_ref_t   value_ref;
    ncc_string_t      scope_tag;
    bool               capture_by_tag;
    ncc_string_t      type_spec;
    ncc_string_t      infer_expr;
    ncc_string_t      adt_kind;
    ncc_string_t      visibility_spec;
    ncc_string_t      op_kind;
    int64_t            reclassify_tid;
};

// ============================================================================
// Annotation API
// ============================================================================

void ncc_nt_annotate(ncc_nonterm_t *nt, ncc_annotation_t annot);
void ncc_rule_annotate(ncc_parse_rule_t *rule, ncc_annotation_t annot);

void ncc_nt_scope_open(ncc_nonterm_t *nt,
                         ncc_string_t scope_tag, ncc_child_ref_t name_ref);
void ncc_nt_declares(ncc_nonterm_t *nt,
                       ncc_child_ref_t name_ref, ncc_child_ref_t type_ref);
void ncc_nt_type_decl(ncc_nonterm_t *nt, ncc_child_ref_t name_ref);
void ncc_nt_type(ncc_nonterm_t *nt,
                   ncc_child_ref_t name_ref, ncc_string_t type_spec);
void ncc_nt_assigns(ncc_nonterm_t *nt,
                      ncc_child_ref_t name_ref, ncc_child_ref_t value_ref);
void ncc_nt_branch(ncc_nonterm_t *nt, ncc_child_ref_t cond_ref,
                     ncc_child_ref_t then_ref, ncc_child_ref_t else_ref);
void ncc_nt_loop(ncc_nonterm_t *nt, ncc_child_ref_t cond_ref,
                   ncc_child_ref_t body_ref);
void ncc_nt_jump(ncc_nonterm_t *nt, ncc_string_t jump_kind);
void ncc_nt_capture(ncc_nonterm_t *nt, ncc_string_t tag, bool by_tag);
void ncc_nt_indent(ncc_nonterm_t *nt);
void ncc_nt_group(ncc_nonterm_t *nt);
void ncc_nt_concat(ncc_nonterm_t *nt);
void ncc_nt_blankline(ncc_nonterm_t *nt);
void ncc_nt_softline(ncc_nonterm_t *nt, ncc_child_ref_t child_ref);
void ncc_nt_hardline(ncc_nonterm_t *nt, ncc_child_ref_t child_ref);
void ncc_nt_pp_newline(ncc_nonterm_t *nt, ncc_child_ref_t child_ref);
void ncc_nt_pp_space(ncc_nonterm_t *nt, ncc_child_ref_t child_ref);
void ncc_nt_nospace(ncc_nonterm_t *nt, ncc_child_ref_t child_ref);
void ncc_nt_pp_align(ncc_nonterm_t *nt, ncc_child_ref_t child_ref);
void ncc_nt_infer(ncc_nonterm_t *nt, ncc_string_t constraint_expr);
void ncc_nt_switch(ncc_nonterm_t *nt, ncc_child_ref_t cond_ref,
                     ncc_child_ref_t cases_ref);
void ncc_nt_adt(ncc_nonterm_t *nt, ncc_string_t adt_kind,
                  ncc_child_ref_t name_ref, ncc_string_t scope_tag);
void ncc_nt_field(ncc_nonterm_t *nt,
                    ncc_child_ref_t name_ref, ncc_child_ref_t type_ref);
void ncc_nt_method(ncc_nonterm_t *nt,
                     ncc_child_ref_t name_ref, ncc_child_ref_t type_ref);
void ncc_nt_inherits(ncc_nonterm_t *nt, ncc_child_ref_t name_ref);
void ncc_nt_implements(ncc_nonterm_t *nt, ncc_child_ref_t name_ref);
void ncc_nt_visibility(ncc_nonterm_t *nt, ncc_string_t visibility_spec);
void ncc_nt_static(ncc_nonterm_t *nt);
void ncc_nt_abstract(ncc_nonterm_t *nt);
void ncc_nt_operator(ncc_nonterm_t *nt, ncc_string_t op_str);
void ncc_nt_literal(ncc_nonterm_t *nt, ncc_string_t lit_kind);
void ncc_nt_call(ncc_nonterm_t *nt, ncc_child_ref_t func_ref,
                   ncc_child_ref_t args_ref);
void ncc_nt_varref(ncc_nonterm_t *nt, ncc_child_ref_t name_ref);
void ncc_nt_reclassify(ncc_nonterm_t *nt,
                         ncc_child_ref_t guard_ref,
                         ncc_string_t guard_text,
                         int64_t new_tid);
void ncc_nt_reclassify_list(ncc_nonterm_t *nt,
                              ncc_child_ref_t guard_ref,
                              ncc_string_t guard_text,
                              ncc_child_ref_t list_ref,
                              int64_t new_tid);

// Rule-level annotation convenience functions (parallel to ncc_nt_* variants).
void ncc_rule_scope_open(ncc_parse_rule_t *rule,
                           ncc_string_t scope_tag, ncc_child_ref_t name_ref);
void ncc_rule_declares(ncc_parse_rule_t *rule,
                         ncc_child_ref_t name_ref, ncc_child_ref_t type_ref);
void ncc_rule_type_decl(ncc_parse_rule_t *rule, ncc_child_ref_t name_ref);
void ncc_rule_type(ncc_parse_rule_t *rule,
                     ncc_child_ref_t name_ref, ncc_string_t type_spec);
void ncc_rule_assigns(ncc_parse_rule_t *rule,
                        ncc_child_ref_t name_ref, ncc_child_ref_t value_ref);
void ncc_rule_branch(ncc_parse_rule_t *rule, ncc_child_ref_t cond_ref,
                       ncc_child_ref_t then_ref, ncc_child_ref_t else_ref);
void ncc_rule_loop(ncc_parse_rule_t *rule, ncc_child_ref_t cond_ref,
                     ncc_child_ref_t body_ref);
void ncc_rule_jump(ncc_parse_rule_t *rule, ncc_string_t jump_kind);
void ncc_rule_switch(ncc_parse_rule_t *rule, ncc_child_ref_t cond_ref,
                       ncc_child_ref_t cases_ref);
void ncc_rule_capture(ncc_parse_rule_t *rule, ncc_string_t tag, bool by_tag);
void ncc_rule_concat(ncc_parse_rule_t *rule);
