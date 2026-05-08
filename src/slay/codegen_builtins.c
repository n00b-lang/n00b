/**
 * @file codegen_builtins.c
 * @brief Built-in function implementations for the n00b codegen.
 *
 * C runtime helpers that get imported into MIR and called from JIT'd
 * code. The dispatch function checks function names against a small
 * table and emits the appropriate MIR import + call.
 */

#include "n00b.h"
#include "slay/codegen_builtins.h"
#include "slay/codegen.h"
#include "core/string.h"
#include "core/type_info.h"
#include "text/strings/string_ops.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

// ============================================================================
// Type tag → type hash mapping for vtable dispatch.
// ============================================================================

static uint64_t
type_tag_to_hash(n00b_cg_type_tag_t tag)
{
    switch (tag) {
    case N00B_CG_STRING: return typehash(n00b_string_t *);
    case N00B_CG_OPTION: return typehash(n00b_rt_option_t *);
    case N00B_CG_RESULT: return typehash(n00b_rt_result_t *);
    // TODO: LIST, DICT, etc. when registered.
    default:             return 0;
    }
}

// ============================================================================
// C runtime helpers — called from JIT'd code via MIR import.
// These must have stable ABIs (no _kargs, no ncc extensions).
// ============================================================================

void
n00b_builtin_print_i64(int64_t val)
{
    printf("%" PRId64 "\n", val);
}

void
n00b_builtin_print_u64(uint64_t val)
{
    printf("%" PRIu64 "\n", val);
}

void
n00b_builtin_print_f64(double val)
{
    printf("%g\n", val);
}

void
n00b_builtin_print_bool(int64_t val)
{
    printf("%s\n", val ? "true" : "false");
}

void
n00b_builtin_print_str(void *str_ptr)
{
    if (!str_ptr) {
        printf("nil\n");
        return;
    }

    n00b_string_t *s = (n00b_string_t *)str_ptr;
    printf("%.*s\n", (int)s->u8_bytes, s->data);
}

void
n00b_builtin_print_nil(void)
{
    printf("nil\n");
}

// ============================================================================
// String runtime helpers — thin wrappers with stable C ABI (no _kargs).
// ============================================================================

void *
n00b_builtin_str_concat(void *a, void *b)
{
    return n00b_unicode_str_cat((n00b_string_t *)a, (n00b_string_t *)b);
}

int64_t
n00b_builtin_str_eq(void *a, void *b)
{
    return n00b_unicode_str_eq((n00b_string_t *)a, (n00b_string_t *)b);
}

int64_t
n00b_builtin_str_len(void *s)
{
    if (!s) {
        return 0;
    }

    return (int64_t)((n00b_string_t *)s)->codepoints;
}

// n00b_rt_option_t and n00b_rt_result_t are defined in codegen_builtins.h.

// --- Option helpers ---

void *
n00b_builtin_option_some(uint64_t val)
{
    n00b_rt_option_t *o = n00b_alloc(n00b_rt_option_t);
    o->has_value = true;
    o->value     = val;
    return o;
}

void *
n00b_builtin_option_none(void)
{
    n00b_rt_option_t *o = n00b_alloc(n00b_rt_option_t);
    o->has_value = false;
    o->value     = 0;
    return o;
}

int64_t
n00b_builtin_option_is_set(void *opt)
{
    if (!opt) {
        return 0;
    }

    return ((n00b_rt_option_t *)opt)->has_value ? 1 : 0;
}

uint64_t
n00b_builtin_option_unwrap(void *opt)
{
    if (!opt || !((n00b_rt_option_t *)opt)->has_value) {
        fprintf(stderr, "n00b: unwrap failed on none\n");
        abort();
    }

    return ((n00b_rt_option_t *)opt)->value;
}

void
n00b_builtin_print_option(void *opt)
{
    if (!opt || !((n00b_rt_option_t *)opt)->has_value) {
        printf("none\n");
        return;
    }

    // For now, print the raw payload as an integer.
    // TODO: type-aware printing of the inner value.
    printf("some(%llu)\n", (unsigned long long)((n00b_rt_option_t *)opt)->value);
}

// --- Result helpers ---

void *
n00b_builtin_result_ok(uint64_t val)
{
    n00b_rt_result_t *r = n00b_alloc(n00b_rt_result_t);
    r->is_ok       = true;
    r->payload     = val;
    r->err_code    = 0;
    r->err_message = NULL;
    return r;
}

void *
n00b_builtin_result_err_code(int64_t code)
{
    n00b_rt_result_t *r = n00b_alloc(n00b_rt_result_t);
    r->is_ok       = false;
    r->payload     = 0;
    r->err_code    = code;
    r->err_message = NULL;
    return r;
}

void *
n00b_builtin_result_err_msg(void *msg)
{
    n00b_rt_result_t *r = n00b_alloc(n00b_rt_result_t);
    r->is_ok       = false;
    r->payload     = 0;
    r->err_code    = 0;
    r->err_message = NULL;

    if (msg) {
        n00b_string_t *s = (n00b_string_t *)msg;
        // Store a copy of the string data as a C string.
        char *buf = n00b_alloc_size(1, s->u8_bytes + 1);
        memcpy(buf, s->data, s->u8_bytes);
        buf[s->u8_bytes] = '\0';
        r->err_message = buf;
    }

    return r;
}

int64_t
n00b_builtin_result_is_ok(void *res)
{
    if (!res) {
        return 0;
    }

    return ((n00b_rt_result_t *)res)->is_ok ? 1 : 0;
}

uint64_t
n00b_builtin_result_unwrap(void *res)
{
    if (!res || !((n00b_rt_result_t *)res)->is_ok) {
        n00b_rt_result_t *r = (n00b_rt_result_t *)res;

        if (r && r->err_message) {
            fprintf(stderr, "n00b: unwrap failed on err(%lld, \"%s\")\n",
                    (long long)r->err_code, r->err_message);
        }
        else if (r) {
            fprintf(stderr, "n00b: unwrap failed on err(%lld)\n",
                    (long long)r->err_code);
        }
        else {
            fprintf(stderr, "n00b: unwrap failed on null result\n");
        }

        abort();
    }

    return ((n00b_rt_result_t *)res)->payload;
}

void
n00b_builtin_print_result(void *res)
{
    if (!res) {
        printf("err(null)\n");
        return;
    }

    n00b_rt_result_t *r = (n00b_rt_result_t *)res;

    if (r->is_ok) {
        printf("ok(%llu)\n", (unsigned long long)r->payload);
    }
    else if (r->err_message) {
        printf("err(%lld, \"%s\")\n", (long long)r->err_code, r->err_message);
    }
    else {
        printf("err(%lld)\n", (long long)r->err_code);
    }
}

// ============================================================================
// Built-in print dispatch: import the right helper and emit a call.
// ============================================================================

static n00b_cg_val_t
codegen_builtin_print(n00b_cg_session_t *s,
                      n00b_cg_val_t     *args,
                      int32_t            n_args)
{
    if (n_args < 1) {
        // print() with no args → print empty line.
        n00b_cg_import_func(s, "n00b_builtin_print_nil",
                             (void *)n00b_builtin_print_nil,
                             .ret = N00B_CG_VOID);
        n00b_cg_emit_call(s, "n00b_builtin_print_nil", NULL, 0,
                           .ret = N00B_CG_VOID);
        return N00B_CG_VOID_VAL;
    }

    // Dispatch based on the argument's type tag.
    n00b_cg_val_t       arg  = args[0];
    n00b_cg_type_tag_t  tag  = arg.type_tag;
    const char         *name = NULL;
    void               *addr = NULL;
    n00b_cg_type_tag_t  param_type = N00B_CG_I64;

    switch (tag) {
    case N00B_CG_BOOL:
        name       = "n00b_builtin_print_bool";
        addr       = (void *)n00b_builtin_print_bool;
        param_type = N00B_CG_I64; // bool is backed by i64
        break;

    case N00B_CG_F32:
    case N00B_CG_F64:
        name       = "n00b_builtin_print_f64";
        addr       = (void *)n00b_builtin_print_f64;
        param_type = N00B_CG_F64;
        break;

    case N00B_CG_STRING:
    case N00B_CG_PTR:
        name       = "n00b_builtin_print_str";
        addr       = (void *)n00b_builtin_print_str;
        param_type = N00B_CG_I64; // Pointer as i64 for MIR compat.
        break;

    case N00B_CG_OPTION:
        name       = "n00b_builtin_print_option";
        addr       = (void *)n00b_builtin_print_option;
        param_type = N00B_CG_I64;
        break;

    case N00B_CG_RESULT:
        name       = "n00b_builtin_print_result";
        addr       = (void *)n00b_builtin_print_result;
        param_type = N00B_CG_I64;
        break;

    case N00B_CG_NIL:
    case N00B_CG_VOID:
        name       = "n00b_builtin_print_nil";
        addr       = (void *)n00b_builtin_print_nil;
        n00b_cg_import_func(s, name, addr, .ret = N00B_CG_VOID);
        n00b_cg_emit_call(s, name, NULL, 0, .ret = N00B_CG_VOID);
        return N00B_CG_VOID_VAL;

    default:
        // All integer types: i8, i16, i32, i64, u8, u16, u32, u64.
        name       = "n00b_builtin_print_i64";
        addr       = (void *)n00b_builtin_print_i64;
        param_type = N00B_CG_I64;
        break;
    }

    n00b_cg_type_tag_t param_types[] = {param_type};
    n00b_cg_import_func(s, name, addr,
                         .ret         = N00B_CG_VOID,
                         .param_types = param_types,
                         .n_params    = 1);
    n00b_cg_emit_call(s, name, &arg, 1, .ret = N00B_CG_VOID);

    return N00B_CG_VOID_VAL;
}

// ============================================================================
// Built-in len(): dispatch based on argument type.
// ============================================================================

static n00b_cg_val_t
codegen_builtin_len(n00b_cg_session_t *s,
                    n00b_cg_val_t     *args,
                    int32_t            n_args)
{
    if (n_args < 1) {
        return N00B_CG_VOID_VAL;
    }

    n00b_cg_val_t      arg = args[0];
    n00b_cg_type_tag_t tag = arg.type_tag;

    switch (tag) {
    case N00B_CG_STRING: {
        n00b_cg_type_tag_t pt[] = {N00B_CG_I64};
        n00b_cg_import_func(s, "n00b_builtin_str_len",
                             (void *)n00b_builtin_str_len,
                             .ret = N00B_CG_I64,
                             .param_types = pt, .n_params = 1);
        return n00b_cg_emit_call(s, "n00b_builtin_str_len", &arg, 1,
                                  .ret = N00B_CG_I64);
    }
    default:
        // TODO: list, dict, etc.
        return N00B_CG_VOID_VAL;
    }
}

// ============================================================================
// Option/Result builtin codegen helpers.
// Each imports the C runtime helper and emits a MIR call.
// ============================================================================

// some(val) -> option[T]
static n00b_cg_val_t
codegen_builtin_some(n00b_cg_session_t *s, n00b_cg_val_t *args, int32_t n_args)
{
    if (n_args < 1) {
        return N00B_CG_VOID_VAL;
    }

    n00b_cg_type_tag_t pt[] = {N00B_CG_I64};
    n00b_cg_import_func(s, "n00b_builtin_option_some",
                         (void *)n00b_builtin_option_some,
                         .ret = N00B_CG_I64, .param_types = pt, .n_params = 1);
    n00b_cg_val_t r = n00b_cg_emit_call(s, "n00b_builtin_option_some",
                                          args, 1, .ret = N00B_CG_I64);
    r.type_tag = N00B_CG_OPTION;
    return r;
}

// none -> option[T]
static n00b_cg_val_t
codegen_builtin_none(n00b_cg_session_t *s)
{
    n00b_cg_import_func(s, "n00b_builtin_option_none",
                         (void *)n00b_builtin_option_none,
                         .ret = N00B_CG_I64);
    n00b_cg_val_t r = n00b_cg_emit_call(s, "n00b_builtin_option_none",
                                          NULL, 0, .ret = N00B_CG_I64);
    r.type_tag = N00B_CG_OPTION;
    return r;
}

// ok(val) -> result[T]
static n00b_cg_val_t
codegen_builtin_ok(n00b_cg_session_t *s, n00b_cg_val_t *args, int32_t n_args)
{
    if (n_args < 1) {
        return N00B_CG_VOID_VAL;
    }

    n00b_cg_type_tag_t pt[] = {N00B_CG_I64};
    n00b_cg_import_func(s, "n00b_builtin_result_ok",
                         (void *)n00b_builtin_result_ok,
                         .ret = N00B_CG_I64, .param_types = pt, .n_params = 1);
    n00b_cg_val_t r = n00b_cg_emit_call(s, "n00b_builtin_result_ok",
                                          args, 1, .ret = N00B_CG_I64);
    r.type_tag = N00B_CG_RESULT;
    return r;
}

// err(code) or err("message") -> result[T]
static n00b_cg_val_t
codegen_builtin_err(n00b_cg_session_t *s, n00b_cg_val_t *args, int32_t n_args)
{
    if (n_args < 1) {
        return N00B_CG_VOID_VAL;
    }

    const char         *name;
    void               *addr;
    n00b_cg_type_tag_t  pt_tag;

    if (args[0].type_tag == N00B_CG_STRING) {
        name   = "n00b_builtin_result_err_msg";
        addr   = (void *)n00b_builtin_result_err_msg;
        pt_tag = N00B_CG_I64; // pointer as i64
    }
    else {
        name   = "n00b_builtin_result_err_code";
        addr   = (void *)n00b_builtin_result_err_code;
        pt_tag = N00B_CG_I64;
    }

    n00b_cg_type_tag_t pt[] = {pt_tag};
    n00b_cg_import_func(s, name, addr,
                         .ret = N00B_CG_I64, .param_types = pt, .n_params = 1);
    n00b_cg_val_t r = n00b_cg_emit_call(s, name, args, 1, .ret = N00B_CG_I64);
    r.type_tag = N00B_CG_RESULT;
    return r;
}

// is_set?(opt) -> bool
static n00b_cg_val_t
codegen_builtin_is_set(n00b_cg_session_t *s, n00b_cg_val_t *args, int32_t n_args)
{
    if (n_args < 1) {
        return N00B_CG_VOID_VAL;
    }

    n00b_cg_type_tag_t pt[] = {N00B_CG_I64};
    n00b_cg_import_func(s, "n00b_builtin_option_is_set",
                         (void *)n00b_builtin_option_is_set,
                         .ret = N00B_CG_I64, .param_types = pt, .n_params = 1);
    n00b_cg_val_t r = n00b_cg_emit_call(s, "n00b_builtin_option_is_set",
                                          args, 1, .ret = N00B_CG_I64);
    r.type_tag = N00B_CG_BOOL;
    return r;
}

// ok?(res) -> bool
static n00b_cg_val_t
codegen_builtin_ok_check(n00b_cg_session_t *s, n00b_cg_val_t *args, int32_t n_args)
{
    if (n_args < 1) {
        return N00B_CG_VOID_VAL;
    }

    n00b_cg_type_tag_t pt[] = {N00B_CG_I64};
    n00b_cg_import_func(s, "n00b_builtin_result_is_ok",
                         (void *)n00b_builtin_result_is_ok,
                         .ret = N00B_CG_I64, .param_types = pt, .n_params = 1);
    n00b_cg_val_t r = n00b_cg_emit_call(s, "n00b_builtin_result_is_ok",
                                          args, 1, .ret = N00B_CG_I64);
    r.type_tag = N00B_CG_BOOL;
    return r;
}

// err?(res) -> bool (negation of ok?)
static n00b_cg_val_t
codegen_builtin_err_check(n00b_cg_session_t *s, n00b_cg_val_t *args, int32_t n_args)
{
    n00b_cg_val_t ok = codegen_builtin_ok_check(s, args, n_args);

    if (ok.kind == N00B_CG_VAL_VOID) {
        return ok;
    }

    n00b_cg_val_t r = n00b_cg_emit_not(s, ok);
    r.type_tag = N00B_CG_BOOL;
    return r;
}

// unwrap(opt_or_res) -> T (aborts on none/err)
static n00b_cg_val_t
codegen_builtin_unwrap(n00b_cg_session_t *s, n00b_cg_val_t *args, int32_t n_args)
{
    if (n_args < 1) {
        return N00B_CG_VOID_VAL;
    }

    const char *name;
    void       *addr;

    if (args[0].type_tag == N00B_CG_OPTION) {
        name = "n00b_builtin_option_unwrap";
        addr = (void *)n00b_builtin_option_unwrap;
    }
    else {
        name = "n00b_builtin_result_unwrap";
        addr = (void *)n00b_builtin_result_unwrap;
    }

    n00b_cg_type_tag_t pt[] = {N00B_CG_I64};
    n00b_cg_import_func(s, name, addr,
                         .ret = N00B_CG_I64, .param_types = pt, .n_params = 1);
    return n00b_cg_emit_call(s, name, args, 1, .ret = N00B_CG_I64);
}

// ============================================================================
// Public dispatch: check if a function name is a built-in.
// ============================================================================

bool
n00b_codegen_builtin_call(n00b_cg_session_t *s,
                          const char        *func_name,
                          n00b_cg_val_t     *args,
                          int32_t            n_args,
                          n00b_cg_val_t     *out)
{
    if (strcmp(func_name, "print") == 0) {
        *out = codegen_builtin_print(s, args, n_args);
        return true;
    }

    if (strcmp(func_name, "len") == 0) {
        *out = codegen_builtin_len(s, args, n_args);
        return true;
    }

    if (strcmp(func_name, "some") == 0) {
        *out = codegen_builtin_some(s, args, n_args);
        return true;
    }

    if (strcmp(func_name, "none") == 0) {
        *out = codegen_builtin_none(s);
        return true;
    }

    if (strcmp(func_name, "ok") == 0) {
        *out = codegen_builtin_ok(s, args, n_args);
        return true;
    }

    if (strcmp(func_name, "err") == 0) {
        *out = codegen_builtin_err(s, args, n_args);
        return true;
    }

    return false;
}

// ============================================================================
// Method dispatch via vtable.
//
// For method calls (expr.name(args)), the receiver is args[0].
// Look up the method on the receiver's type via the type registry.
// If found, import and call it.
// ============================================================================

bool
n00b_codegen_method_dispatch(n00b_cg_session_t *s,
                             const char        *method_name,
                             n00b_cg_val_t     *args,
                             int32_t            n_args,
                             n00b_cg_val_t     *out)
{
    if (n_args < 1) {
        return false;
    }

    uint64_t hash = type_tag_to_hash(args[0].type_tag);

    if (!hash) {
        return false;
    }

    n00b_option_t(n00b_vtable_entry) method_opt =
        n00b_type_method_lookup(hash, method_name);

    if (!n00b_option_is_set(method_opt)) {
        return false;
    }

    n00b_vtable_entry fn = n00b_option_get(method_opt);

    // Build a unique import name. Sanitize '?' → 'Q' since MIR
    // identifiers don't allow '?'.
    size_t method_len = strlen(method_name);
    size_t name_len   = 5 + method_len + 1 + 20; // "_vtm_" + name + "_" + hash digits
    char  *import_name = n00b_alloc_size(1, name_len + 1);

    snprintf(import_name, name_len + 1, "_vtm_%s_%llu",
             method_name, (unsigned long long)hash);

    for (char *p = import_name; *p; p++) {
        if (*p == '?') {
            *p = 'Q';
        }
    }

    n00b_cg_type_tag_t pt[] = {N00B_CG_I64};
    n00b_cg_import_func(s, import_name, (void *)fn,
                         .ret = N00B_CG_I64,
                         .param_types = pt, .n_params = 1);

    *out = n00b_cg_emit_call(s, import_name, args, 1, .ret = N00B_CG_I64);
    return true;
}
