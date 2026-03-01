// hash.c — Runtime type hashing: n00b_tc_type_t → typehash(C_type).
//
// Maps n00b type-checker types to the same uint64_t hash that the
// compile-time typehash() ncc transform produces for the C representation.

#include "typecheck/hash.h"
#include "typecheck/unify.h"
#include "adt/variant.h"
#include "core/sha256.h"
#include "core/string.h"
#include <string.h>

// ============================================================================
// SHA256 → uint64 (matches ncc's n00b_type_hash_u64)
// ============================================================================

uint64_t
n00b_type_hash_cname(const char *cname)
{
    n00b_sha256_digest_t digest;
    n00b_sha256_hash(cname, strlen(cname), digest);

    // digest[] is uint32_t[8].  Convert first 2 words to big-endian bytes,
    // then pack into uint64.
    uint8_t dbytes[8];

    for (int i = 0; i < 2; i++) {
        uint32_t w        = digest[i];
        dbytes[i * 4 + 0] = (uint8_t)(w >> 24);
        dbytes[i * 4 + 1] = (uint8_t)(w >> 16);
        dbytes[i * 4 + 2] = (uint8_t)(w >> 8);
        dbytes[i * 4 + 3] = (uint8_t)(w);
    }

    uint64_t h = 0;

    for (int i = 0; i < 8; i++) {
        h = (h << 8) | dbytes[i];
    }

    return h;
}

// ============================================================================
// Primitive name → C type name
// ============================================================================

typedef struct {
    const char *n00b_name;
    const char *c_name;
} prim_map_entry_t;

static const prim_map_entry_t prim_map[] = {
    {"int",    "int64_t"},
    {"i64",    "int64_t"},
    {"i8",     "int8_t"},
    {"i16",    "int16_t"},
    {"i32",    "int32_t"},
    {"u8",     "uint8_t"},
    {"u16",    "uint16_t"},
    {"u32",    "uint32_t"},
    {"u64",    "uint64_t"},
    {"f32",    "float"},
    {"f64",    "double"},
    {"bool",   "bool"},
    {"string", "n00b_string_t"},
    {"nil",    "void"},
    {"void",   "void"},
};

#define PRIM_MAP_COUNT (sizeof(prim_map) / sizeof(prim_map[0]))

static const char *
prim_to_cname(n00b_string_t *name)
{
    for (size_t i = 0; i < PRIM_MAP_COUNT; i++) {
        if (name->u8_bytes == strlen(prim_map[i].n00b_name)
            && memcmp(name->data, prim_map[i].n00b_name, name->u8_bytes) == 0) {
            return prim_map[i].c_name;
        }
    }

    return NULL;
}

// ============================================================================
// Parameterized type name → C type name (container only, ignores params)
// ============================================================================

typedef struct {
    const char *n00b_name;
    const char *c_name;
} param_map_entry_t;

static const param_map_entry_t param_map[] = {
    {"list",   "n00b_list_t"},
    {"dict",   "n00b_dict_t"},
    {"set",    "n00b_set_t"},
    {"array",  "n00b_array_t"},
    {"result", "n00b_result_t"},
    {"option", "n00b_option_t"},
    {"ref",    "n00b_ref_t"},
};

#define PARAM_MAP_COUNT (sizeof(param_map) / sizeof(param_map[0]))

static const char *
param_to_cname(n00b_string_t *name)
{
    for (size_t i = 0; i < PARAM_MAP_COUNT; i++) {
        if (name->u8_bytes == strlen(param_map[i].n00b_name)
            && memcmp(name->data, param_map[i].n00b_name, name->u8_bytes) == 0) {
            return param_map[i].c_name;
        }
    }

    return NULL;
}

// ============================================================================
// n00b_tc_type_t → typehash
// ============================================================================

uint64_t
n00b_tc_type_to_hash(n00b_tc_type_t *t)
{
    if (!t) {
        return 0;
    }

    // Chase union-find.
    t = n00b_tc_find(t);

    // Unresolved type variable → n00b_any_t (boxed).
    if (n00b_variant_is_type(t->kind, n00b_tc_var_t)) {
        return n00b_type_hash_cname("n00b_any_t");
    }

    // Primitive type.
    if (n00b_variant_is_type(t->kind, n00b_tc_prim_t)) {
        n00b_tc_prim_t prim = n00b_variant_get(t->kind, n00b_tc_prim_t);
        const char    *cn   = prim_to_cname(prim.name);

        return cn ? n00b_type_hash_cname(cn) : 0;
    }

    // Parameterized type — hash the container's C name.
    // Known containers map to their n00b C type; unknown names (user
    // classes) hash the n00b name directly since the JIT generates
    // struct names deterministically from class name + type params.
    if (n00b_variant_is_type(t->kind, n00b_tc_param_t)) {
        n00b_tc_param_t param = n00b_variant_get(t->kind, n00b_tc_param_t);
        const char     *cn    = param_to_cname(param.name);

        if (cn) {
            return n00b_type_hash_cname(cn);
        }

        // User-defined type: use the n00b name as-is.
        // Null-terminate for hashing.
        char buf[256];
        size_t len = param.name->u8_bytes;

        if (len >= sizeof(buf)) {
            len = sizeof(buf) - 1;
        }

        memcpy(buf, param.name->data, len);
        buf[len] = '\0';

        return n00b_type_hash_cname(buf);
    }

    // Function type → function pointer.
    if (n00b_variant_is_type(t->kind, n00b_tc_fn_t)) {
        return n00b_type_hash_cname("void *");
    }

    // Record, Tuple, Sum → heap-allocated structs.
    if (n00b_variant_is_type(t->kind, n00b_tc_record_t)
        || n00b_variant_is_type(t->kind, n00b_tc_tuple_t)
        || n00b_variant_is_type(t->kind, n00b_tc_sum_t)) {
        return n00b_type_hash_cname("void *");
    }

    return 0;
}
