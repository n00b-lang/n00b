#include "n00b.h"
#include "core/type_info.h"
#include "core/string.h"
#include "core/buffer.h"
#include "core/hash.h"
#include "core/dict_untyped.h"
#include "core/interval_tree.h"
#include "render/plane.h"
#include "render/canvas.h"
#include "table/table.h"
#include "io/subproc.h"
#include "strings/string_convert.h"
#include "strings/string_ops.h"
#include "strings/fmt_numbers.h"

// ============================================================================
// Value-type wrappers
//
// These are only needed when the underlying function takes its "self"
// argument by value, but the vtable dispatch passes a pointer to the
// object.  Pointer-based functions are stored directly.
// ============================================================================

// --- n00b_string_t (40-byte value type, passed by pointer in vtable) --------

static n00b_uint128_t
vt_string_hash(n00b_string_t *self)
{
    return n00b_string_hash(*self);
}

static n00b_string_t
vt_string_copy(n00b_string_t *self)
{
    return n00b_unicode_str_copy(*self);
}

static n00b_string_t
vt_string_add(n00b_string_t *a, n00b_string_t *b)
{
    return n00b_unicode_str_cat(*a, *b);
}

static n00b_string_t
vt_string_to_literal(n00b_string_t *self)
{
    return n00b_unicode_str_to_literal(*self);
}

static int64_t
vt_string_len(n00b_string_t *self)
{
    return (int64_t)self->codepoints;
}

static n00b_string_t
vt_string_to_string(n00b_string_t *self)
{
    return *self;
}

// --- Primitives (word-sized value types) ------------------------------------

static n00b_uint128_t
vt_word_hash(void *self)
{
    return n00b_hash_word(*(void **)self);
}

static n00b_string_t
vt_uint8_to_string(uint8_t *self)
{
    return n00b_fmt_uint((uint64_t)*self);
}

static n00b_string_t
vt_int32_to_string(int32_t *self)
{
    return n00b_unicode_str_from_int((int64_t)*self);
}

static n00b_string_t
vt_uint32_to_string(uint32_t *self)
{
    return n00b_fmt_uint((uint64_t)*self);
}

static n00b_string_t
vt_int64_to_string(int64_t *self)
{
    return n00b_unicode_str_from_int(*self);
}

static n00b_string_t
vt_uint64_to_string(uint64_t *self)
{
    return n00b_fmt_uint(*self);
}

static n00b_string_t
vt_double_to_string(double *self)
{
    return n00b_fmt_float(*self);
}

static n00b_string_t
vt_bool_to_string(bool *self)
{
    return n00b_fmt_bool(*self, .word = true);
}

// ============================================================================
// Registration
// ============================================================================

void
n00b_register_builtin_types(void)
{
    // Primitives.
    N00B_TYPE_REGISTER(uint8_t,
        N00B_CORE_METHOD(N00B_BI_HASH, vt_word_hash),
        N00B_CORE_METHOD(N00B_BI_TO_STRING, vt_uint8_to_string),
    );
    N00B_TYPE_REGISTER(int32_t,
        N00B_CORE_METHOD(N00B_BI_HASH, vt_word_hash),
        N00B_CORE_METHOD(N00B_BI_TO_STRING, vt_int32_to_string),
    );
    N00B_TYPE_REGISTER(uint32_t,
        N00B_CORE_METHOD(N00B_BI_HASH, vt_word_hash),
        N00B_CORE_METHOD(N00B_BI_TO_STRING, vt_uint32_to_string),
    );
    N00B_TYPE_REGISTER(int64_t,
        N00B_CORE_METHOD(N00B_BI_HASH, vt_word_hash),
        N00B_CORE_METHOD(N00B_BI_TO_STRING, vt_int64_to_string),
    );
    N00B_TYPE_REGISTER(uint64_t,
        N00B_CORE_METHOD(N00B_BI_HASH, vt_word_hash),
        N00B_CORE_METHOD(N00B_BI_TO_STRING, vt_uint64_to_string),
    );
    N00B_TYPE_REGISTER(double,
        N00B_CORE_METHOD(N00B_BI_HASH, vt_word_hash),
        N00B_CORE_METHOD(N00B_BI_TO_STRING, vt_double_to_string),
    );
    N00B_TYPE_REGISTER(bool,
        N00B_CORE_METHOD(N00B_BI_HASH, vt_word_hash),
        N00B_CORE_METHOD(N00B_BI_TO_STRING, vt_bool_to_string),
    );

    // n00b_string_t (value type — uses wrappers).
    N00B_TYPE_REGISTER(n00b_string_t,
        N00B_CORE_METHOD(N00B_BI_HASH, vt_string_hash),
        N00B_CORE_METHOD(N00B_BI_COPY, vt_string_copy),
        N00B_CORE_METHOD(N00B_BI_ADD, vt_string_add),
        N00B_CORE_METHOD(N00B_BI_TO_LITERAL, vt_string_to_literal),
        N00B_CORE_METHOD(N00B_BI_LEN, vt_string_len),
        N00B_CORE_METHOD(N00B_BI_TO_STRING, vt_string_to_string),
    );

    // n00b_buffer_t — kargs constructor, lock cleanup, vtable finalizer.
    N00B_TYPE_REGISTER(n00b_buffer_t,
        N00B_CORE_METHOD(N00B_BI_CONSTRUCTOR, n00b_buffer_init),
        N00B_CTOR_KARGS,
        N00B_LOCK_FIELD(n00b_buffer_t, lock),
        N00B_CORE_METHOD(N00B_BI_FINALIZER, n00b_buffer_free),
        N00B_CORE_METHOD(N00B_BI_HASH, n00b_buffer_hash),
        N00B_CORE_METHOD(N00B_BI_COPY, n00b_buffer_copy),
        N00B_CORE_METHOD(N00B_BI_ADD, n00b_buffer_add),
        N00B_CORE_METHOD(N00B_BI_LEN, n00b_buffer_len),
        N00B_CORE_METHOD(N00B_BI_TO_STRING, n00b_buffer_to_string),
    );

    // n00b_dict_untyped_t — kargs constructor, lock-free.
    N00B_TYPE_REGISTER(n00b_dict_untyped_t,
        N00B_CORE_METHOD(N00B_BI_CONSTRUCTOR, n00b_dict_untyped_init),
        N00B_CTOR_KARGS,
    );

    // n00b_interval_tree_t — kargs constructor, lock cleanup.
    N00B_TYPE_REGISTER(n00b_interval_tree_t,
        N00B_CORE_METHOD(N00B_BI_CONSTRUCTOR, n00b_interval_tree_init),
        N00B_CTOR_KARGS,
        N00B_LOCK_FIELD(n00b_interval_tree_t, lock),
    );

    // table, canvas, plane — kargs constructors, lock cleanup.
    N00B_TYPE_REGISTER(n00b_table_t,
        N00B_CORE_METHOD(N00B_BI_CONSTRUCTOR, n00b_table_init),
        N00B_CTOR_KARGS,
        N00B_LOCK_FIELD(n00b_table_t, lock),
    );
    N00B_TYPE_REGISTER(n00b_canvas_t,
        N00B_CORE_METHOD(N00B_BI_CONSTRUCTOR, n00b_canvas_init),
        N00B_CTOR_KARGS,
        N00B_LOCK_FIELD(n00b_canvas_t, lock),
    );
    N00B_TYPE_REGISTER(n00b_plane_t,
        N00B_CORE_METHOD(N00B_BI_CONSTRUCTOR, n00b_plane_init),
        N00B_CTOR_KARGS,
        N00B_LOCK_FIELD(n00b_plane_t, lock),
    );

#ifndef _WIN32
    // n00b_subproc_t — kargs constructor, no lock.
    N00B_TYPE_REGISTER(n00b_subproc_t,
        N00B_CORE_METHOD(N00B_BI_CONSTRUCTOR, n00b_subproc_init),
        N00B_CTOR_KARGS,
    );
#endif
}
