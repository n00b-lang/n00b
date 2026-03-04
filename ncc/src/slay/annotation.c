// annotation.c - Production annotations for grammar non-terminals
//
// Provides programmatic annotation helpers that attach semantic
// information (scope, declarations, control flow, ADT, formatting)
// to non-terminals.  Each convenience function creates a stack-local
// ncc_annotation_t, fills the relevant fields, and calls
// ncc_nt_annotate() which heap-allocates a copy and prepends it to
// the non-terminal's annotation linked list.
//
// All string fields are ncc_string_t (value type, GC-managed .data)
// so no strdup/free is needed.

#include "slay/annotation.h"
#include "internal/slay/grammar_internal.h"

// ============================================================================
// Core annotation attachment
// ============================================================================

static ncc_annotation_t *
heap_annot(ncc_annotation_t annot)
{
    ncc_annotation_t *a = ncc_alloc(ncc_annotation_t);
    *a = annot;
    return a;
}

void
ncc_nt_annotate(ncc_nonterm_t *nt, ncc_annotation_t annot)
{
    // Stage the annotation on the NT.  ncc_grammar_finalize() distributes
    // staged annotations to all of the NT's rules.
    ncc_annotation_t *a = heap_annot(annot);

    if (!nt->pending_annotations.data) {
        nt->pending_annotations = ncc_list_new(ncc_annotation_ptr_t, false);
    }

    ncc_list_push(nt->pending_annotations, a);
}

void
ncc_rule_annotate(ncc_parse_rule_t *rule, ncc_annotation_t annot)
{
    ncc_annotation_t *a = heap_annot(annot);

    if (!rule->annotations.data) {
        rule->annotations = ncc_list_new(ncc_annotation_ptr_t, false);
    }

    ncc_list_push(rule->annotations, a);
}

// ============================================================================
// Scope and declaration helpers
// ============================================================================

void
ncc_nt_scope_open(ncc_nonterm_t *nt,
                   ncc_string_t scope_tag, ncc_child_ref_t name_ref)
{
    ncc_annotation_t a = {0};
    a.kind      = NCC_ANNOT_SCOPE_OPEN;
    a.scope_tag = scope_tag;
    a.name_ref  = name_ref;
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_declares(ncc_nonterm_t *nt,
                 ncc_child_ref_t name_ref, ncc_child_ref_t type_ref)
{
    ncc_annotation_t a = {0};
    a.kind     = NCC_ANNOT_DECLARES;
    a.name_ref = name_ref;
    a.type_ref = type_ref;
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_type_decl(ncc_nonterm_t *nt, ncc_child_ref_t name_ref)
{
    ncc_annotation_t a = {0};
    a.kind     = NCC_ANNOT_TYPE_DECL;
    a.name_ref = name_ref;
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_type(ncc_nonterm_t *nt,
             ncc_child_ref_t name_ref, ncc_string_t type_spec)
{
    ncc_annotation_t a = {0};
    a.kind      = NCC_ANNOT_TYPE;
    a.name_ref  = name_ref;
    a.type_spec = type_spec;
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_assigns(ncc_nonterm_t *nt,
                ncc_child_ref_t name_ref, ncc_child_ref_t value_ref)
{
    ncc_annotation_t a = {0};
    a.kind      = NCC_ANNOT_ASSIGNS;
    a.name_ref  = name_ref;
    a.value_ref = value_ref;
    ncc_nt_annotate(nt, a);
}

// ============================================================================
// Control flow helpers
// ============================================================================

void
ncc_nt_branch(ncc_nonterm_t *nt, ncc_child_ref_t cond_ref,
               ncc_child_ref_t then_ref, ncc_child_ref_t else_ref)
{
    ncc_annotation_t a = {0};
    a.kind      = NCC_ANNOT_BRANCH;
    a.name_ref  = cond_ref;
    a.type_ref  = then_ref;
    a.value_ref = else_ref;
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_loop(ncc_nonterm_t *nt, ncc_child_ref_t cond_ref,
             ncc_child_ref_t body_ref)
{
    ncc_annotation_t a = {0};
    a.kind     = NCC_ANNOT_LOOP;
    a.name_ref = cond_ref;
    a.type_ref = body_ref;
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_jump(ncc_nonterm_t *nt, ncc_string_t jump_kind)
{
    ncc_annotation_t a = {0};
    a.kind      = NCC_ANNOT_JUMP;
    a.scope_tag = jump_kind;
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_capture(ncc_nonterm_t *nt, ncc_string_t tag, bool by_tag)
{
    ncc_annotation_t a = {0};
    a.kind           = NCC_ANNOT_CAPTURE;
    a.scope_tag      = tag;
    a.capture_by_tag = by_tag;
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_infer(ncc_nonterm_t *nt, ncc_string_t constraint_expr)
{
    ncc_annotation_t a = {0};
    a.kind       = NCC_ANNOT_INFER;
    a.infer_expr = constraint_expr;
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_switch(ncc_nonterm_t *nt, ncc_child_ref_t cond_ref,
               ncc_child_ref_t cases_ref)
{
    ncc_annotation_t a = {0};
    a.kind     = NCC_ANNOT_SWITCH;
    a.name_ref = cond_ref;
    a.type_ref = cases_ref;
    ncc_nt_annotate(nt, a);
}

// ============================================================================
// ADT annotation helpers
// ============================================================================

void
ncc_nt_adt(ncc_nonterm_t *nt, ncc_string_t adt_kind,
            ncc_child_ref_t name_ref, ncc_string_t scope_tag)
{
    ncc_annotation_t a = {0};
    a.kind      = NCC_ANNOT_ADT;
    a.adt_kind  = adt_kind;
    a.name_ref  = name_ref;
    a.scope_tag = scope_tag.data ? scope_tag
                                 : (adt_kind.data ? adt_kind
                                                  : ncc_string_empty());
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_field(ncc_nonterm_t *nt,
              ncc_child_ref_t name_ref, ncc_child_ref_t type_ref)
{
    ncc_annotation_t a = {0};
    a.kind     = NCC_ANNOT_FIELD;
    a.name_ref = name_ref;
    a.type_ref = type_ref;
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_method(ncc_nonterm_t *nt,
               ncc_child_ref_t name_ref, ncc_child_ref_t type_ref)
{
    ncc_annotation_t a = {0};
    a.kind     = NCC_ANNOT_METHOD;
    a.name_ref = name_ref;
    a.type_ref = type_ref;
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_inherits(ncc_nonterm_t *nt, ncc_child_ref_t name_ref)
{
    ncc_annotation_t a = {0};
    a.kind     = NCC_ANNOT_INHERITS;
    a.name_ref = name_ref;
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_implements(ncc_nonterm_t *nt, ncc_child_ref_t name_ref)
{
    ncc_annotation_t a = {0};
    a.kind     = NCC_ANNOT_IMPLEMENTS;
    a.name_ref = name_ref;
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_visibility(ncc_nonterm_t *nt, ncc_string_t visibility_spec)
{
    ncc_annotation_t a = {0};
    a.kind            = NCC_ANNOT_VISIBILITY;
    a.visibility_spec = visibility_spec;
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_static(ncc_nonterm_t *nt)
{
    ncc_annotation_t a = {0};
    a.kind = NCC_ANNOT_STATIC;
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_abstract(ncc_nonterm_t *nt)
{
    ncc_annotation_t a = {0};
    a.kind = NCC_ANNOT_ABSTRACT;
    ncc_nt_annotate(nt, a);
}

// ============================================================================
// Codegen annotation helpers
// ============================================================================

void
ncc_nt_operator(ncc_nonterm_t *nt, ncc_string_t op_str)
{
    ncc_annotation_t a = {0};
    a.kind    = NCC_ANNOT_OPERATOR;
    a.op_kind = op_str;
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_literal(ncc_nonterm_t *nt, ncc_string_t lit_kind)
{
    ncc_annotation_t a = {0};
    a.kind    = NCC_ANNOT_LITERAL;
    a.op_kind = lit_kind;
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_call(ncc_nonterm_t *nt, ncc_child_ref_t func_ref,
             ncc_child_ref_t args_ref)
{
    ncc_annotation_t a = {0};
    a.kind     = NCC_ANNOT_CALL;
    a.name_ref = func_ref;
    a.type_ref = args_ref;
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_varref(ncc_nonterm_t *nt, ncc_child_ref_t name_ref)
{
    ncc_annotation_t a = {0};
    a.kind     = NCC_ANNOT_VARREF;
    a.name_ref = name_ref;
    ncc_nt_annotate(nt, a);
}

// ============================================================================
// Reclassify annotation helpers
// ============================================================================

void
ncc_nt_reclassify(ncc_nonterm_t *nt,
                   ncc_child_ref_t guard_ref,
                   ncc_string_t guard_text,
                   int64_t new_tid)
{
    ncc_annotation_t a = {0};
    a.kind           = NCC_ANNOT_RECLASSIFY;
    a.type_ref       = guard_ref;
    a.scope_tag      = guard_text;
    a.reclassify_tid = new_tid;
    ncc_nt_annotate(nt, a);
    nt->has_reclassify = true;
}

void
ncc_nt_reclassify_list(ncc_nonterm_t *nt,
                        ncc_child_ref_t guard_ref,
                        ncc_string_t guard_text,
                        ncc_child_ref_t list_ref,
                        int64_t new_tid)
{
    ncc_annotation_t a = {0};
    a.kind           = NCC_ANNOT_RECLASSIFY_LIST;
    a.type_ref       = guard_ref;
    a.name_ref       = list_ref;
    a.scope_tag      = guard_text;
    a.reclassify_tid = new_tid;
    ncc_nt_annotate(nt, a);
    nt->has_reclassify = true;
}

// ============================================================================
// Formatting annotation helpers
// ============================================================================

void
ncc_nt_indent(ncc_nonterm_t *nt)
{
    ncc_annotation_t a = {0};
    a.kind = NCC_ANNOT_INDENT;
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_group(ncc_nonterm_t *nt)
{
    ncc_annotation_t a = {0};
    a.kind = NCC_ANNOT_GROUP;
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_concat(ncc_nonterm_t *nt)
{
    ncc_annotation_t a = {0};
    a.kind = NCC_ANNOT_CONCAT;
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_blankline(ncc_nonterm_t *nt)
{
    ncc_annotation_t a = {0};
    a.kind = NCC_ANNOT_BLANKLINE;
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_softline(ncc_nonterm_t *nt, ncc_child_ref_t child_ref)
{
    ncc_annotation_t a = {0};
    a.kind     = NCC_ANNOT_SOFTLINE;
    a.name_ref = child_ref;
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_hardline(ncc_nonterm_t *nt, ncc_child_ref_t child_ref)
{
    ncc_annotation_t a = {0};
    a.kind     = NCC_ANNOT_HARDLINE;
    a.name_ref = child_ref;
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_pp_newline(ncc_nonterm_t *nt, ncc_child_ref_t child_ref)
{
    ncc_annotation_t a = {0};
    a.kind     = NCC_ANNOT_NEWLINE;
    a.name_ref = child_ref;
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_pp_space(ncc_nonterm_t *nt, ncc_child_ref_t child_ref)
{
    ncc_annotation_t a = {0};
    a.kind     = NCC_ANNOT_SPACE;
    a.name_ref = child_ref;
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_nospace(ncc_nonterm_t *nt, ncc_child_ref_t child_ref)
{
    ncc_annotation_t a = {0};
    a.kind     = NCC_ANNOT_NOSPACE;
    a.name_ref = child_ref;
    ncc_nt_annotate(nt, a);
}

void
ncc_nt_pp_align(ncc_nonterm_t *nt, ncc_child_ref_t child_ref)
{
    ncc_annotation_t a = {0};
    a.kind     = NCC_ANNOT_ALIGN;
    a.name_ref = child_ref;
    ncc_nt_annotate(nt, a);
}

// ============================================================================
// Rule-level annotation convenience functions
// ============================================================================

void
ncc_rule_scope_open(ncc_parse_rule_t *rule,
                     ncc_string_t scope_tag, ncc_child_ref_t name_ref)
{
    ncc_annotation_t a = {0};
    a.kind      = NCC_ANNOT_SCOPE_OPEN;
    a.scope_tag = scope_tag;
    a.name_ref  = name_ref;
    ncc_rule_annotate(rule, a);
}

void
ncc_rule_declares(ncc_parse_rule_t *rule,
                   ncc_child_ref_t name_ref, ncc_child_ref_t type_ref)
{
    ncc_annotation_t a = {0};
    a.kind     = NCC_ANNOT_DECLARES;
    a.name_ref = name_ref;
    a.type_ref = type_ref;
    ncc_rule_annotate(rule, a);
}

void
ncc_rule_type_decl(ncc_parse_rule_t *rule, ncc_child_ref_t name_ref)
{
    ncc_annotation_t a = {0};
    a.kind     = NCC_ANNOT_TYPE_DECL;
    a.name_ref = name_ref;
    ncc_rule_annotate(rule, a);
}

void
ncc_rule_type(ncc_parse_rule_t *rule,
               ncc_child_ref_t name_ref, ncc_string_t type_spec)
{
    ncc_annotation_t a = {0};
    a.kind      = NCC_ANNOT_TYPE;
    a.name_ref  = name_ref;
    a.type_spec = type_spec;
    ncc_rule_annotate(rule, a);
}

void
ncc_rule_assigns(ncc_parse_rule_t *rule,
                  ncc_child_ref_t name_ref, ncc_child_ref_t value_ref)
{
    ncc_annotation_t a = {0};
    a.kind      = NCC_ANNOT_ASSIGNS;
    a.name_ref  = name_ref;
    a.value_ref = value_ref;
    ncc_rule_annotate(rule, a);
}

void
ncc_rule_branch(ncc_parse_rule_t *rule, ncc_child_ref_t cond_ref,
                 ncc_child_ref_t then_ref, ncc_child_ref_t else_ref)
{
    ncc_annotation_t a = {0};
    a.kind      = NCC_ANNOT_BRANCH;
    a.name_ref  = cond_ref;
    a.type_ref  = then_ref;
    a.value_ref = else_ref;
    ncc_rule_annotate(rule, a);
}

void
ncc_rule_loop(ncc_parse_rule_t *rule, ncc_child_ref_t cond_ref,
               ncc_child_ref_t body_ref)
{
    ncc_annotation_t a = {0};
    a.kind     = NCC_ANNOT_LOOP;
    a.name_ref = cond_ref;
    a.type_ref = body_ref;
    ncc_rule_annotate(rule, a);
}

void
ncc_rule_jump(ncc_parse_rule_t *rule, ncc_string_t jump_kind)
{
    ncc_annotation_t a = {0};
    a.kind      = NCC_ANNOT_JUMP;
    a.scope_tag = jump_kind;
    ncc_rule_annotate(rule, a);
}

void
ncc_rule_switch(ncc_parse_rule_t *rule, ncc_child_ref_t cond_ref,
                 ncc_child_ref_t cases_ref)
{
    ncc_annotation_t a = {0};
    a.kind     = NCC_ANNOT_SWITCH;
    a.name_ref = cond_ref;
    a.type_ref = cases_ref;
    ncc_rule_annotate(rule, a);
}

void
ncc_rule_capture(ncc_parse_rule_t *rule, ncc_string_t tag, bool by_tag)
{
    ncc_annotation_t a = {0};
    a.kind           = NCC_ANNOT_CAPTURE;
    a.scope_tag      = tag;
    a.capture_by_tag = by_tag;
    ncc_rule_annotate(rule, a);
}

void
ncc_rule_concat(ncc_parse_rule_t *rule)
{
    ncc_annotation_t a = {0};
    a.kind = NCC_ANNOT_CONCAT;
    ncc_rule_annotate(rule, a);
}
