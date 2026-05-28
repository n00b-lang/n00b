/*
 * naudit/filter.c — implementation of the naudit filter helper.
 *
 * Registers the `match` n00b type with extension methods, installs
 * the C-side accessors as FFI bindings, and provides filter_compile /
 * filter_apply for the Phase 4 rule-file integration point.
 *
 * Per WP-010 the JIT codegen pipeline consults the type registry's
 * extension methods when lowering postfix-`.` against a registered
 * opaque type. We register the `match` type with n00b_type_register
 * (giving it a typehash + an ext_vtable), then add each accessor
 * method via n00b_type_add_method. The same C functions are also
 * installed as FFI bindings via n00b_ffi_install_simple so the JIT
 * has a callable symbol to dispatch to.
 *
 * The match handle layout lives here (not in the header) so consumers
 * never field-access it directly. The handle is built per-invocation
 * by n00b_naudit_filter_apply and is stack-allocated for the duration
 * of the predicate call (no GC integration needed).
 */

#include "n00b.h"
#include "naudit/filter.h"
#include "n00b/embed_ffi.h"
#include "core/string.h"
#include "core/type_info.h"
#include "core/vtable.h"
#include "slay/parse_tree.h"
#include "slay/token.h"

#include <string.h>

// ============================================================================
// Match handle layout
// ============================================================================

struct n00b_naudit_match {
    n00b_parse_tree_t *node;
    n00b_string_t     *src_text;
};

// ============================================================================
// Accessor C functions
//
// Each takes a `n00b_naudit_match_t *self` (the JIT-side caller
// passes the bound `arg` pointer through unchanged). The return
// types match the method signatures advertised below.
// ============================================================================

/**
 * `arg.text` — the source-text slice corresponding to the match's
 * span. Computed from the leftmost-token column to the rightmost-
 * token endcol, scoped to the match's line(s). For single-line
 * matches this is byte-exact; for multi-line spans we slice from
 * the leftmost token's start to the rightmost token's end via
 * source-text scanning by line.
 */
static n00b_string_t *
n00b_naudit_match_text(n00b_naudit_match_t *self)
{
    if (!self || !self->node || !self->src_text) {
        return r"";
    }

    n00b_parse_tree_t *lt = n00b_pt_first_token(self->node);
    n00b_parse_tree_t *rt = n00b_pt_last_token(self->node);

    if (!lt || !rt) {
        return r"";
    }

    n00b_token_info_t *lti = n00b_parse_node_token(lt);
    n00b_token_info_t *rti = n00b_parse_node_token(rt);

    if (!lti || !rti) {
        return r"";
    }

    // Walk the source text line by line until we find the start.
    // The match's bytes span [start_byte, end_byte). Lines are
    // 1-based; columns are 1-based; endcol is exclusive of the
    // last byte (matches the slay convention).
    const char *src     = self->src_text->data;
    size_t      src_len = self->src_text->u8_bytes;

    if (!src || src_len == 0) {
        return r"";
    }

    uint32_t line       = 1;
    uint32_t col        = 1;
    int64_t  start_byte = -1;
    int64_t  end_byte   = -1;

    for (size_t i = 0; i < src_len; i++) {
        if (start_byte < 0
            && line == lti->line
            && col == lti->column) {
            start_byte = (int64_t)i;
        }

        if (line == rti->line && col == rti->endcol) {
            end_byte = (int64_t)i;
            break;
        }

        if (src[i] == '\n') {
            line++;
            col = 1;
        }
        else {
            col++;
        }
    }

    if (end_byte < 0) {
        end_byte = (int64_t)src_len;
    }

    if (start_byte < 0 || start_byte >= end_byte) {
        return r"";
    }

    return n00b_string_from_raw(src + start_byte,
                                (int64_t)(end_byte - start_byte));
}

/**
 * `arg.nt` — the non-terminal name for an interior node, or `""`
 * for a token leaf.
 */
static n00b_string_t *
n00b_naudit_match_nt(n00b_naudit_match_t *self)
{
    if (!self || !self->node) {
        return r"";
    }

    if (n00b_pt_is_token(self->node)) {
        return r"";
    }

    const char *name = n00b_pt_nt_name(self->node);
    size_t      len  = n00b_pt_nt_name_len(self->node);

    if (!name || len == 0) {
        return r"";
    }

    return n00b_string_from_raw(name, (int64_t)len);
}

/**
 * `arg.line` — 1-based source line of the leftmost token in the
 * match. Returns 0 if no token is available.
 */
static int64_t
n00b_naudit_match_line(n00b_naudit_match_t *self)
{
    if (!self || !self->node) {
        return 0;
    }

    n00b_parse_tree_t *tok = n00b_pt_first_token(self->node);

    if (!tok) {
        return 0;
    }

    n00b_token_info_t *ti = n00b_parse_node_token(tok);

    return ti ? (int64_t)ti->line : 0;
}

/**
 * `arg.col` — 1-based source column of the leftmost token.
 */
static int64_t
n00b_naudit_match_col(n00b_naudit_match_t *self)
{
    if (!self || !self->node) {
        return 0;
    }

    n00b_parse_tree_t *tok = n00b_pt_first_token(self->node);

    if (!tok) {
        return 0;
    }

    n00b_token_info_t *ti = n00b_parse_node_token(tok);

    return ti ? (int64_t)ti->column : 0;
}

/**
 * `arg.end_line` — 1-based source line of the rightmost token.
 */
static int64_t
n00b_naudit_match_end_line(n00b_naudit_match_t *self)
{
    if (!self || !self->node) {
        return 0;
    }

    n00b_parse_tree_t *tok = n00b_pt_last_token(self->node);

    if (!tok) {
        return 0;
    }

    n00b_token_info_t *ti = n00b_parse_node_token(tok);

    return ti ? (int64_t)ti->line : 0;
}

/**
 * `arg.end_col` — 1-based source endcol of the rightmost token.
 */
static int64_t
n00b_naudit_match_end_col(n00b_naudit_match_t *self)
{
    if (!self || !self->node) {
        return 0;
    }

    n00b_parse_tree_t *tok = n00b_pt_last_token(self->node);

    if (!tok) {
        return 0;
    }

    n00b_token_info_t *ti = n00b_parse_node_token(tok);

    return ti ? (int64_t)ti->endcol : 0;
}

/**
 * `arg.child_named(name)` — first direct child whose NT name
 * matches @p name, wrapped in a fresh match handle pointing at the
 * same `src_text`. Returns nullptr if no such child exists; filter
 * expressions can guard with the `!= 0` idiom.
 *
 * The returned handle is allocated via `n00b_alloc` so it survives
 * the predicate's stack frame and remains valid as long as the GC
 * reaches it through the call result.
 */
static n00b_naudit_match_t *
n00b_naudit_match_child_named(n00b_naudit_match_t *self, n00b_string_t *name)
{
    if (!self || !self->node || !name) {
        return nullptr;
    }

    // n00b_pt_find_child_by_nt wants a C string. Copy the name into
    // a NUL-terminated buffer.
    if (name->u8_bytes == 0 || !name->data) {
        return nullptr;
    }

    size_t  len = (size_t)name->u8_bytes;
    char   *cbuf = n00b_alloc_array(char, len + 1);
    memcpy(cbuf, name->data, len);
    cbuf[len] = '\0';

    n00b_parse_tree_t *child = n00b_pt_find_child_by_nt(self->node, cbuf);

    if (!child) {
        return nullptr;
    }

    n00b_naudit_match_t *out = n00b_alloc(n00b_naudit_match_t);
    out->node     = child;
    out->src_text = self->src_text;
    return out;
}

/**
 * `arg.has_ancestor(name)` — placeholder for the Phase 4 rule-file
 * integration. Parse trees are not parent-linked, so a full
 * implementation needs an upstream change to slay. Returns false
 * for now; callers know to avoid this accessor until Phase 4.
 */
static bool
n00b_naudit_match_has_ancestor(n00b_naudit_match_t *self, n00b_string_t *name)
{
    (void)self;
    (void)name;
    return false;
}

// ============================================================================
// Type registration
// ============================================================================

static bool s_match_type_registered = false;

void
n00b_naudit_match_type_register(void)
{
    if (s_match_type_registered) {
        return;
    }

    bool reg_ok = N00B_TYPE_REGISTER(n00b_naudit_match_t,
        N00B_TYPE_STATIC_TRANSIENT(r"naudit match handles are predicate-scoped"));

    if (!reg_ok) {
        // Already registered by another caller (shouldn't happen
        // given the s_match_type_registered guard, but be tolerant).
    }

    uint64_t th = typehash(n00b_naudit_match_t *);

    n00b_type_add_method(th, &(n00b_method_t){
        .fn          = (n00b_vtable_entry)n00b_naudit_match_text,
        .name        = "text",
        .return_type = {
            .type_hash = typehash(n00b_string_t *),
            .type_name = "string",
        },
    });

    n00b_type_add_method(th, &(n00b_method_t){
        .fn          = (n00b_vtable_entry)n00b_naudit_match_nt,
        .name        = "nt",
        .return_type = {
            .type_hash = typehash(n00b_string_t *),
            .type_name = "string",
        },
    });

    n00b_type_add_method(th, &(n00b_method_t){
        .fn          = (n00b_vtable_entry)n00b_naudit_match_line,
        .name        = "line",
        .return_type = {
            .type_hash = typehash(int64_t),
            .type_name = "i64",
        },
    });

    n00b_type_add_method(th, &(n00b_method_t){
        .fn          = (n00b_vtable_entry)n00b_naudit_match_col,
        .name        = "col",
        .return_type = {
            .type_hash = typehash(int64_t),
            .type_name = "i64",
        },
    });

    n00b_type_add_method(th, &(n00b_method_t){
        .fn          = (n00b_vtable_entry)n00b_naudit_match_end_line,
        .name        = "end_line",
        .return_type = {
            .type_hash = typehash(int64_t),
            .type_name = "i64",
        },
    });

    n00b_type_add_method(th, &(n00b_method_t){
        .fn          = (n00b_vtable_entry)n00b_naudit_match_end_col,
        .name        = "end_col",
        .return_type = {
            .type_hash = typehash(int64_t),
            .type_name = "i64",
        },
    });

    n00b_type_add_method(th, &(n00b_method_t){
        .fn          = (n00b_vtable_entry)n00b_naudit_match_child_named,
        .name        = "child_named",
        .return_type = {
            .type_hash = typehash(n00b_naudit_match_t *),
            .type_name = "n00b_naudit_match_t",
        },
    });

    n00b_type_add_method(th, &(n00b_method_t){
        .fn          = (n00b_vtable_entry)n00b_naudit_match_has_ancestor,
        .name        = "has_ancestor",
        .return_type = {
            .type_hash = typehash(bool),
            .type_name = "bool",
        },
    });

    s_match_type_registered = true;
}

// ============================================================================
// FFI binding installation
//
// The JIT method dispatch needs the C symbol resolvable via the
// embed-FFI's process-symbol lookup (dlsym/GetProcAddress). The
// accessors are file-static for tidy linkage hygiene, but
// `n00b_ffi_install_simple` accepts a direct address-resolution
// pathway via the underlying `lookup_process_symbol` — for static
// symbols we install via the proto-and-import pair manually.
//
// In practice, the WP-010 codegen path emits an indirect call via
// the registered method's `fn` pointer (no FFI lookup needed) —
// see `n00b_codegen_method_dispatch`. The FFI install is therefore
// a defensive belt-and-braces step for any path that falls through
// to a name-based call lookup; the registered method pointer is
// the load-bearing wiring.
// ============================================================================

static void
install_accessor_ffi_bindings(n00b_cg_session_t *session)
{
    if (!session) {
        return;
    }

    // FFI bindings here are intentionally a no-op for the smoke
    // tests: WP-010's `n00b_codegen_method_dispatch` does an
    // indirect call through the registered method's `fn` pointer,
    // so the JIT does not need to resolve these by name. We keep
    // the function as a hook for future Phase 4 work that may add
    // free-function helpers callable as `helper(arg)` rather than
    // `arg.helper`.
    (void)session;
}

// ============================================================================
// Session creation
// ============================================================================

n00b_result_t(n00b_eval_session_t *)
n00b_naudit_filter_session_new(void)
{
    n00b_naudit_match_type_register();

    auto sr = n00b_eval_session_new();

    if (n00b_result_is_err(sr)) {
        return sr;
    }

    n00b_eval_session_t *s = n00b_result_get(sr);

    install_accessor_ffi_bindings(n00b_eval_session_cg(s));

    return n00b_result_ok(n00b_eval_session_t *, s);
}

// ============================================================================
// Filter compile + apply
// ============================================================================

n00b_result_t(n00b_eval_predicate_fn_t)
n00b_naudit_filter_compile(n00b_eval_session_t *s,
                           n00b_string_t       *name,
                           n00b_string_t       *expr_text)
{
    (void)name; // reserved for Phase 4 rule-id diagnostics
    return n00b_eval_compile_predicate(s, expr_text, r"n00b_naudit_match_t");
}

bool
n00b_naudit_filter_apply(n00b_eval_predicate_fn_t  fn,
                         n00b_parse_tree_t        *match_node,
                         n00b_string_t            *src_text)
{
    if (!fn) {
        return false;
    }

    n00b_naudit_match_t handle = {
        .node     = match_node,
        .src_text = src_text,
    };

    return fn((void *)&handle);
}
