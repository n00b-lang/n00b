// annotation.c - Production annotations for grammar non-terminals
//
// Provides programmatic annotation helpers that attach semantic
// information (scope, declarations, control flow, ADT, formatting)
// to non-terminals.  Each convenience function creates a stack-local
// n00b_annotation_t, fills the relevant fields, and calls
// n00b_nt_annotate() which heap-allocates a copy and prepends it to
// the non-terminal's annotation linked list.
//
// All string fields are n00b_string_t (value type, GC-managed .data)
// so no strdup/free is needed.

#include "slay/annotation.h"
#include "internal/slay/grammar_internal.h"

// ============================================================================
// Core annotation attachment
// ============================================================================

static n00b_annotation_t *
heap_annot(n00b_annotation_t annot)
{
    n00b_annotation_t *a = n00b_alloc(n00b_annotation_t);
    *a = annot;
    return a;
}

void
n00b_nt_annotate(n00b_nonterm_t *nt, n00b_annotation_t annot)
{
    // Stage the annotation on the NT.  n00b_grammar_finalize() distributes
    // staged annotations to all of the NT's rules.
    n00b_annotation_t *a = heap_annot(annot);

    if (!nt->pending_annotations.data) {
        nt->pending_annotations = n00b_list_new(n00b_annotation_t *, false);
    }

    n00b_list_push(nt->pending_annotations, a);
}

void
n00b_rule_annotate(n00b_parse_rule_t *rule, n00b_annotation_t annot)
{
    n00b_annotation_t *a = heap_annot(annot);

    if (!rule->annotations.data) {
        rule->annotations = n00b_list_new(n00b_annotation_t *, false);
    }

    n00b_list_push(rule->annotations, a);
}

// ============================================================================
// Scope and declaration helpers
// ============================================================================

void
n00b_nt_scope_open(n00b_nonterm_t *nt,
                   n00b_string_t scope_tag, n00b_child_ref_t name_ref)
{
    n00b_annotation_t a = {0};
    a.kind      = N00B_ANNOT_SCOPE_OPEN;
    a.scope_tag = scope_tag;
    a.name_ref  = name_ref;
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_declares(n00b_nonterm_t *nt,
                 n00b_child_ref_t name_ref, n00b_child_ref_t type_ref)
{
    n00b_annotation_t a = {0};
    a.kind     = N00B_ANNOT_DECLARES;
    a.name_ref = name_ref;
    a.type_ref = type_ref;
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_type_decl(n00b_nonterm_t *nt, n00b_child_ref_t name_ref)
{
    n00b_annotation_t a = {0};
    a.kind     = N00B_ANNOT_TYPE_DECL;
    a.name_ref = name_ref;
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_type(n00b_nonterm_t *nt,
             n00b_child_ref_t name_ref, n00b_string_t type_spec)
{
    n00b_annotation_t a = {0};
    a.kind      = N00B_ANNOT_TYPE;
    a.name_ref  = name_ref;
    a.type_spec = type_spec;
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_assigns(n00b_nonterm_t *nt,
                n00b_child_ref_t name_ref, n00b_child_ref_t value_ref)
{
    n00b_annotation_t a = {0};
    a.kind      = N00B_ANNOT_ASSIGNS;
    a.name_ref  = name_ref;
    a.value_ref = value_ref;
    n00b_nt_annotate(nt, a);
}

// ============================================================================
// Control flow helpers
// ============================================================================

void
n00b_nt_branch(n00b_nonterm_t *nt, n00b_child_ref_t cond_ref,
               n00b_child_ref_t then_ref, n00b_child_ref_t else_ref)
{
    n00b_annotation_t a = {0};
    a.kind      = N00B_ANNOT_BRANCH;
    a.name_ref  = cond_ref;
    a.type_ref  = then_ref;
    a.value_ref = else_ref;
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_loop(n00b_nonterm_t *nt, n00b_child_ref_t cond_ref,
             n00b_child_ref_t body_ref)
{
    n00b_annotation_t a = {0};
    a.kind     = N00B_ANNOT_LOOP;
    a.name_ref = cond_ref;
    a.type_ref = body_ref;
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_jump(n00b_nonterm_t *nt, n00b_string_t jump_kind)
{
    n00b_annotation_t a = {0};
    a.kind      = N00B_ANNOT_JUMP;
    a.scope_tag = jump_kind;
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_capture(n00b_nonterm_t *nt, n00b_string_t tag, bool by_tag)
{
    n00b_annotation_t a = {0};
    a.kind           = N00B_ANNOT_CAPTURE;
    a.scope_tag      = tag;
    a.capture_by_tag = by_tag;
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_infer(n00b_nonterm_t *nt, n00b_string_t constraint_expr)
{
    n00b_annotation_t a = {0};
    a.kind       = N00B_ANNOT_INFER;
    a.infer_expr = constraint_expr;
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_switch(n00b_nonterm_t *nt, n00b_child_ref_t cond_ref,
               n00b_child_ref_t cases_ref)
{
    n00b_annotation_t a = {0};
    a.kind     = N00B_ANNOT_SWITCH;
    a.name_ref = cond_ref;
    a.type_ref = cases_ref;
    n00b_nt_annotate(nt, a);
}

// ============================================================================
// ADT annotation helpers
// ============================================================================

void
n00b_nt_adt(n00b_nonterm_t *nt, n00b_string_t adt_kind,
            n00b_child_ref_t name_ref, n00b_string_t scope_tag)
{
    n00b_annotation_t a = {0};
    a.kind      = N00B_ANNOT_ADT;
    a.adt_kind  = adt_kind;
    a.name_ref  = name_ref;
    a.scope_tag = scope_tag.data ? scope_tag
                                 : (adt_kind.data ? adt_kind
                                                  : n00b_string_empty());
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_field(n00b_nonterm_t *nt,
              n00b_child_ref_t name_ref, n00b_child_ref_t type_ref)
{
    n00b_annotation_t a = {0};
    a.kind     = N00B_ANNOT_FIELD;
    a.name_ref = name_ref;
    a.type_ref = type_ref;
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_method(n00b_nonterm_t *nt,
               n00b_child_ref_t name_ref, n00b_child_ref_t type_ref)
{
    n00b_annotation_t a = {0};
    a.kind     = N00B_ANNOT_METHOD;
    a.name_ref = name_ref;
    a.type_ref = type_ref;
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_inherits(n00b_nonterm_t *nt, n00b_child_ref_t name_ref)
{
    n00b_annotation_t a = {0};
    a.kind     = N00B_ANNOT_INHERITS;
    a.name_ref = name_ref;
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_implements(n00b_nonterm_t *nt, n00b_child_ref_t name_ref)
{
    n00b_annotation_t a = {0};
    a.kind     = N00B_ANNOT_IMPLEMENTS;
    a.name_ref = name_ref;
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_visibility(n00b_nonterm_t *nt, n00b_string_t visibility_spec)
{
    n00b_annotation_t a = {0};
    a.kind            = N00B_ANNOT_VISIBILITY;
    a.visibility_spec = visibility_spec;
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_static(n00b_nonterm_t *nt)
{
    n00b_annotation_t a = {0};
    a.kind = N00B_ANNOT_STATIC;
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_abstract(n00b_nonterm_t *nt)
{
    n00b_annotation_t a = {0};
    a.kind = N00B_ANNOT_ABSTRACT;
    n00b_nt_annotate(nt, a);
}

// ============================================================================
// Codegen annotation helpers
// ============================================================================

void
n00b_nt_operator(n00b_nonterm_t *nt, n00b_string_t op_str)
{
    n00b_annotation_t a = {0};
    a.kind    = N00B_ANNOT_OPERATOR;
    a.op_kind = op_str;
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_literal(n00b_nonterm_t *nt, n00b_string_t lit_kind)
{
    n00b_annotation_t a = {0};
    a.kind    = N00B_ANNOT_LITERAL;
    a.op_kind = lit_kind;
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_call(n00b_nonterm_t *nt, n00b_child_ref_t func_ref,
             n00b_child_ref_t args_ref)
{
    n00b_annotation_t a = {0};
    a.kind     = N00B_ANNOT_CALL;
    a.name_ref = func_ref;
    a.type_ref = args_ref;
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_varref(n00b_nonterm_t *nt, n00b_child_ref_t name_ref)
{
    n00b_annotation_t a = {0};
    a.kind     = N00B_ANNOT_VARREF;
    a.name_ref = name_ref;
    n00b_nt_annotate(nt, a);
}

// ============================================================================
// Formatting annotation helpers
// ============================================================================

void
n00b_nt_indent(n00b_nonterm_t *nt)
{
    n00b_annotation_t a = {0};
    a.kind = N00B_ANNOT_INDENT;
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_group(n00b_nonterm_t *nt)
{
    n00b_annotation_t a = {0};
    a.kind = N00B_ANNOT_GROUP;
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_concat(n00b_nonterm_t *nt)
{
    n00b_annotation_t a = {0};
    a.kind = N00B_ANNOT_CONCAT;
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_blankline(n00b_nonterm_t *nt)
{
    n00b_annotation_t a = {0};
    a.kind = N00B_ANNOT_BLANKLINE;
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_softline(n00b_nonterm_t *nt, n00b_child_ref_t child_ref)
{
    n00b_annotation_t a = {0};
    a.kind     = N00B_ANNOT_SOFTLINE;
    a.name_ref = child_ref;
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_hardline(n00b_nonterm_t *nt, n00b_child_ref_t child_ref)
{
    n00b_annotation_t a = {0};
    a.kind     = N00B_ANNOT_HARDLINE;
    a.name_ref = child_ref;
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_pp_newline(n00b_nonterm_t *nt, n00b_child_ref_t child_ref)
{
    n00b_annotation_t a = {0};
    a.kind     = N00B_ANNOT_NEWLINE;
    a.name_ref = child_ref;
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_pp_space(n00b_nonterm_t *nt, n00b_child_ref_t child_ref)
{
    n00b_annotation_t a = {0};
    a.kind     = N00B_ANNOT_SPACE;
    a.name_ref = child_ref;
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_nospace(n00b_nonterm_t *nt, n00b_child_ref_t child_ref)
{
    n00b_annotation_t a = {0};
    a.kind     = N00B_ANNOT_NOSPACE;
    a.name_ref = child_ref;
    n00b_nt_annotate(nt, a);
}

void
n00b_nt_pp_align(n00b_nonterm_t *nt, n00b_child_ref_t child_ref)
{
    n00b_annotation_t a = {0};
    a.kind     = N00B_ANNOT_ALIGN;
    a.name_ref = child_ref;
    n00b_nt_annotate(nt, a);
}

// ============================================================================
// Rule-level annotation convenience functions
// ============================================================================

void
n00b_rule_scope_open(n00b_parse_rule_t *rule,
                     n00b_string_t scope_tag, n00b_child_ref_t name_ref)
{
    n00b_annotation_t a = {0};
    a.kind      = N00B_ANNOT_SCOPE_OPEN;
    a.scope_tag = scope_tag;
    a.name_ref  = name_ref;
    n00b_rule_annotate(rule, a);
}

void
n00b_rule_declares(n00b_parse_rule_t *rule,
                   n00b_child_ref_t name_ref, n00b_child_ref_t type_ref)
{
    n00b_annotation_t a = {0};
    a.kind     = N00B_ANNOT_DECLARES;
    a.name_ref = name_ref;
    a.type_ref = type_ref;
    n00b_rule_annotate(rule, a);
}

void
n00b_rule_type_decl(n00b_parse_rule_t *rule, n00b_child_ref_t name_ref)
{
    n00b_annotation_t a = {0};
    a.kind     = N00B_ANNOT_TYPE_DECL;
    a.name_ref = name_ref;
    n00b_rule_annotate(rule, a);
}

void
n00b_rule_type(n00b_parse_rule_t *rule,
               n00b_child_ref_t name_ref, n00b_string_t type_spec)
{
    n00b_annotation_t a = {0};
    a.kind      = N00B_ANNOT_TYPE;
    a.name_ref  = name_ref;
    a.type_spec = type_spec;
    n00b_rule_annotate(rule, a);
}

void
n00b_rule_assigns(n00b_parse_rule_t *rule,
                  n00b_child_ref_t name_ref, n00b_child_ref_t value_ref)
{
    n00b_annotation_t a = {0};
    a.kind      = N00B_ANNOT_ASSIGNS;
    a.name_ref  = name_ref;
    a.value_ref = value_ref;
    n00b_rule_annotate(rule, a);
}

void
n00b_rule_branch(n00b_parse_rule_t *rule, n00b_child_ref_t cond_ref,
                 n00b_child_ref_t then_ref, n00b_child_ref_t else_ref)
{
    n00b_annotation_t a = {0};
    a.kind      = N00B_ANNOT_BRANCH;
    a.name_ref  = cond_ref;
    a.type_ref  = then_ref;
    a.value_ref = else_ref;
    n00b_rule_annotate(rule, a);
}

void
n00b_rule_loop(n00b_parse_rule_t *rule, n00b_child_ref_t cond_ref,
               n00b_child_ref_t body_ref)
{
    n00b_annotation_t a = {0};
    a.kind     = N00B_ANNOT_LOOP;
    a.name_ref = cond_ref;
    a.type_ref = body_ref;
    n00b_rule_annotate(rule, a);
}

void
n00b_rule_jump(n00b_parse_rule_t *rule, n00b_string_t jump_kind)
{
    n00b_annotation_t a = {0};
    a.kind      = N00B_ANNOT_JUMP;
    a.scope_tag = jump_kind;
    n00b_rule_annotate(rule, a);
}

void
n00b_rule_switch(n00b_parse_rule_t *rule, n00b_child_ref_t cond_ref,
                 n00b_child_ref_t cases_ref)
{
    n00b_annotation_t a = {0};
    a.kind     = N00B_ANNOT_SWITCH;
    a.name_ref = cond_ref;
    a.type_ref = cases_ref;
    n00b_rule_annotate(rule, a);
}

void
n00b_rule_capture(n00b_parse_rule_t *rule, n00b_string_t tag, bool by_tag)
{
    n00b_annotation_t a = {0};
    a.kind           = N00B_ANNOT_CAPTURE;
    a.scope_tag      = tag;
    a.capture_by_tag = by_tag;
    n00b_rule_annotate(rule, a);
}

void
n00b_rule_concat(n00b_parse_rule_t *rule)
{
    n00b_annotation_t a = {0};
    a.kind = N00B_ANNOT_CONCAT;
    n00b_rule_annotate(rule, a);
}
