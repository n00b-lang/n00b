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
    N00B_ANNOT_SCOPE_OPEN,
    N00B_ANNOT_DECLARES,
    N00B_ANNOT_TYPE_DECL,
    N00B_ANNOT_ASSIGNS,
    N00B_ANNOT_BRANCH,
    N00B_ANNOT_LOOP,
    N00B_ANNOT_JUMP,
    N00B_ANNOT_SWITCH,
    N00B_ANNOT_CAPTURE,
    N00B_ANNOT_INDENT,
    N00B_ANNOT_NEWLINE,
    N00B_ANNOT_SPACE,
    N00B_ANNOT_NOSPACE,
    N00B_ANNOT_GROUP,
    N00B_ANNOT_SOFTLINE,
    N00B_ANNOT_HARDLINE,
    N00B_ANNOT_ALIGN,
    N00B_ANNOT_CONCAT,
    N00B_ANNOT_BLANKLINE,
    N00B_ANNOT_TYPE,
    N00B_ANNOT_INFER,
    N00B_ANNOT_TOKENIZER,
    N00B_ANNOT_ADT,
    N00B_ANNOT_FIELD,
    N00B_ANNOT_METHOD,
    N00B_ANNOT_INHERITS,
    N00B_ANNOT_IMPLEMENTS,
    N00B_ANNOT_VISIBILITY,
    N00B_ANNOT_STATIC,
    N00B_ANNOT_ABSTRACT,
    N00B_ANNOT_OPERATOR,
    N00B_ANNOT_LITERAL,
    N00B_ANNOT_CALL,
    N00B_ANNOT_VARREF,
    N00B_ANNOT_PENALTY,
    N00B_ANNOT_NOTRIVIA,
    N00B_ANNOT_NONE,
} n00b_annot_kind_t;

/** @brief How a child is referenced in an annotation. */
typedef enum {
    N00B_ROLE_BY_INDEX,
    N00B_ROLE_BY_NAME,
} n00b_role_kind_t;

/** @brief Reference to a child node in an annotation. */
typedef struct {
    n00b_role_kind_t kind;
    union {
        int32_t       index;
        n00b_string_t name;
    };
} n00b_child_ref_t;

#define N00B_CHILD_IX(i)   ((n00b_child_ref_t){.kind = N00B_ROLE_BY_INDEX, .index = (i)})
#define N00B_CHILD_NT(n)   ((n00b_child_ref_t){.kind = N00B_ROLE_BY_NAME,  .name = (n)})
#define N00B_CHILD_NONE    N00B_CHILD_IX(-1)

// ============================================================================
// Annotation struct
// ============================================================================

/** @brief A single annotation attached to a rule or non-terminal. */
struct n00b_annotation_t {
    n00b_annot_kind_t  kind;
    n00b_child_ref_t   name_ref;
    n00b_child_ref_t   type_ref;
    n00b_child_ref_t   value_ref;
    n00b_string_t      scope_tag;
    bool               capture_by_tag;
    n00b_string_t      type_spec;
    n00b_string_t      infer_expr;
    n00b_string_t      adt_kind;
    n00b_string_t      visibility_spec;
    n00b_string_t      op_kind;
    n00b_child_ref_t   notrivia_ref;   /**< Child that must have no leading trivia. */
};

// ============================================================================
// Annotation API
// ============================================================================

void n00b_nt_annotate(n00b_nonterm_t *nt, n00b_annotation_t annot);
void n00b_rule_annotate(n00b_parse_rule_t *rule, n00b_annotation_t annot);

void n00b_nt_scope_open(n00b_nonterm_t *nt,
                         n00b_string_t scope_tag, n00b_child_ref_t name_ref);
void n00b_nt_declares(n00b_nonterm_t *nt,
                       n00b_child_ref_t name_ref, n00b_child_ref_t type_ref);
void n00b_nt_type_decl(n00b_nonterm_t *nt, n00b_child_ref_t name_ref);
void n00b_nt_type(n00b_nonterm_t *nt,
                   n00b_child_ref_t name_ref, n00b_string_t type_spec);
void n00b_nt_assigns(n00b_nonterm_t *nt,
                      n00b_child_ref_t name_ref, n00b_child_ref_t value_ref);
void n00b_nt_branch(n00b_nonterm_t *nt, n00b_child_ref_t cond_ref,
                     n00b_child_ref_t then_ref, n00b_child_ref_t else_ref);
void n00b_nt_loop(n00b_nonterm_t *nt, n00b_child_ref_t cond_ref,
                   n00b_child_ref_t body_ref);
void n00b_nt_jump(n00b_nonterm_t *nt, n00b_string_t jump_kind);
void n00b_nt_capture(n00b_nonterm_t *nt, n00b_string_t tag, bool by_tag);
void n00b_nt_indent(n00b_nonterm_t *nt);
void n00b_nt_group(n00b_nonterm_t *nt);
void n00b_nt_concat(n00b_nonterm_t *nt);
void n00b_nt_blankline(n00b_nonterm_t *nt);
void n00b_nt_softline(n00b_nonterm_t *nt, n00b_child_ref_t child_ref);
void n00b_nt_hardline(n00b_nonterm_t *nt, n00b_child_ref_t child_ref);
void n00b_nt_pp_newline(n00b_nonterm_t *nt, n00b_child_ref_t child_ref);
void n00b_nt_pp_space(n00b_nonterm_t *nt, n00b_child_ref_t child_ref);
void n00b_nt_nospace(n00b_nonterm_t *nt, n00b_child_ref_t child_ref);
void n00b_nt_pp_align(n00b_nonterm_t *nt, n00b_child_ref_t child_ref);
void n00b_nt_infer(n00b_nonterm_t *nt, n00b_string_t constraint_expr);
void n00b_nt_switch(n00b_nonterm_t *nt, n00b_child_ref_t cond_ref,
                     n00b_child_ref_t cases_ref);
void n00b_nt_adt(n00b_nonterm_t *nt, n00b_string_t adt_kind,
                  n00b_child_ref_t name_ref, n00b_string_t scope_tag);
void n00b_nt_field(n00b_nonterm_t *nt,
                    n00b_child_ref_t name_ref, n00b_child_ref_t type_ref);
void n00b_nt_method(n00b_nonterm_t *nt,
                     n00b_child_ref_t name_ref, n00b_child_ref_t type_ref);
void n00b_nt_inherits(n00b_nonterm_t *nt, n00b_child_ref_t name_ref);
void n00b_nt_implements(n00b_nonterm_t *nt, n00b_child_ref_t name_ref);
void n00b_nt_visibility(n00b_nonterm_t *nt, n00b_string_t visibility_spec);
void n00b_nt_static(n00b_nonterm_t *nt);
void n00b_nt_abstract(n00b_nonterm_t *nt);
void n00b_nt_operator(n00b_nonterm_t *nt, n00b_string_t op_str);
void n00b_nt_literal(n00b_nonterm_t *nt, n00b_string_t lit_kind);
void n00b_nt_call(n00b_nonterm_t *nt, n00b_child_ref_t func_ref,
                   n00b_child_ref_t args_ref);
void n00b_nt_varref(n00b_nonterm_t *nt, n00b_child_ref_t name_ref);
void n00b_nt_notrivia(n00b_nonterm_t *nt, n00b_child_ref_t child_ref);

// Rule-level notrivia convenience.
void n00b_rule_notrivia(n00b_parse_rule_t *rule, n00b_child_ref_t child_ref);

// Rule-level annotation convenience functions (parallel to n00b_nt_* variants).
void n00b_rule_scope_open(n00b_parse_rule_t *rule,
                           n00b_string_t scope_tag, n00b_child_ref_t name_ref);
void n00b_rule_declares(n00b_parse_rule_t *rule,
                         n00b_child_ref_t name_ref, n00b_child_ref_t type_ref);
void n00b_rule_type_decl(n00b_parse_rule_t *rule, n00b_child_ref_t name_ref);
void n00b_rule_type(n00b_parse_rule_t *rule,
                     n00b_child_ref_t name_ref, n00b_string_t type_spec);
void n00b_rule_assigns(n00b_parse_rule_t *rule,
                        n00b_child_ref_t name_ref, n00b_child_ref_t value_ref);
void n00b_rule_branch(n00b_parse_rule_t *rule, n00b_child_ref_t cond_ref,
                       n00b_child_ref_t then_ref, n00b_child_ref_t else_ref);
void n00b_rule_loop(n00b_parse_rule_t *rule, n00b_child_ref_t cond_ref,
                     n00b_child_ref_t body_ref);
void n00b_rule_jump(n00b_parse_rule_t *rule, n00b_string_t jump_kind);
void n00b_rule_switch(n00b_parse_rule_t *rule, n00b_child_ref_t cond_ref,
                       n00b_child_ref_t cases_ref);
void n00b_rule_capture(n00b_parse_rule_t *rule, n00b_string_t tag, bool by_tag);
void n00b_rule_concat(n00b_parse_rule_t *rule);
