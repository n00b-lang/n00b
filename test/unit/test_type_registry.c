#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stddef.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/alloc_mdata.h"
#include "core/runtime.h"
#include "core/hash.h"
#include "core/type_info.h"
#include "core/buffer.h"
#include "core/string.h"
#include "adt/dict_untyped.h"
#include "text/strings/string_convert.h"

// A dummy type for dynamic registration tests.
typedef struct test_widget_t {
    int   x;
    int   y;
    char *label;
} test_widget_t;

// A dummy constructor for vtable tests.
static void
test_widget_ctor(void *self, void *params)
{
    (void)self;
    (void)params;
}

// A dummy finalizer for vtable tests.
static void
test_widget_dtor(void *self, void *params)
{
    (void)self;
    (void)params;
}

// ============================================================================
// 1. Built-in type lookup succeeds
// ============================================================================

static void
test_builtin_lookup(void)
{
    assert(n00b_option_is_set(n00b_type_lookup(typehash(uint64_t))));
    assert(n00b_option_is_set(n00b_type_lookup(typehash(n00b_string_t))));
    assert(n00b_option_is_set(n00b_type_lookup(typehash(n00b_buffer_t))));
    assert(n00b_option_is_set(n00b_type_lookup(typehash(double))));
    assert(n00b_option_is_set(n00b_type_lookup(typehash(bool))));

    printf("  [PASS] builtin lookup\n");
}

// ============================================================================
// 2. Name field correct
// ============================================================================

static void
test_name_field(void)
{
    auto info_opt = n00b_type_lookup(typehash(uint64_t));
    assert(n00b_option_is_set(info_opt));
    assert(strcmp(n00b_option_get(info_opt)->name, "uint64_t") == 0);

    info_opt = n00b_type_lookup(typehash(n00b_string_t));
    assert(n00b_option_is_set(info_opt));
    assert(strcmp(n00b_option_get(info_opt)->name, "n00b_string_t") == 0);

    printf("  [PASS] name field\n");
}

// ============================================================================
// 3. Unregistered hash returns nullptr
// ============================================================================

static void
test_unregistered_lookup(void)
{
    assert(!n00b_option_is_set(n00b_type_lookup(0xDEADBEEFCAFEBABEULL)));

    printf("  [PASS] unregistered lookup\n");
}

// ============================================================================
// 4. Dynamic registration + duplicate returns false
// ============================================================================

static void
test_dynamic_registration(void)
{
    bool ok = N00B_TYPE_REGISTER(test_widget_t);
    assert(ok);

    // Duplicate registration should fail.
    bool dup = N00B_TYPE_REGISTER(test_widget_t);
    assert(!dup);

    // Lookup should work now.
    auto info_opt = n00b_type_lookup(typehash(test_widget_t));
    assert(n00b_option_is_set(info_opt));
    n00b_type_info_t *info = n00b_option_get(info_opt);
    assert(strcmp(info->name, "test_widget_t") == 0);
    assert(info->alloc_len == sizeof(test_widget_t));

    printf("  [PASS] dynamic registration\n");
}

// ============================================================================
// 5. n00b_obj_typehash on n00b_alloc(T) matches typehash(T)
// ============================================================================

static void
test_obj_typehash(void)
{
    uint64_t *p = n00b_alloc(uint64_t);
    assert(n00b_obj_typehash(p) == typehash(uint64_t));

    n00b_buffer_t *buf = n00b_alloc(n00b_buffer_t);
    assert(n00b_obj_typehash(buf) == typehash(n00b_buffer_t));

    printf("  [PASS] obj typehash\n");
}

// ============================================================================
// 6. n00b_type_info_for returns correct info
// ============================================================================

static void
test_type_info_for(void)
{
    uint64_t *p = n00b_alloc(uint64_t);
    auto info_opt = n00b_type_info_for(p);
    assert(n00b_option_is_set(info_opt));
    assert(strcmp(n00b_option_get(info_opt)->name, "uint64_t") == 0);

    printf("  [PASS] type_info_for\n");
}

// ============================================================================
// 7. n00b_obj_core_method returns nullptr when slot is empty
// ============================================================================

static void
test_empty_core_method(void)
{
    uint64_t *p = n00b_alloc(uint64_t);

    // uint64_t is registered with all-nullptr core vtable.
    n00b_vtable_entry fn = n00b_obj_core_method(p, N00B_BI_CONSTRUCTOR);
    assert(fn == nullptr);

    fn = n00b_obj_core_method(p, N00B_BI_FINALIZER);
    assert(fn == nullptr);

    printf("  [PASS] empty core method\n");
}

// ============================================================================
// 8. Core method set via N00B_CORE_METHOD is accessible
// ============================================================================

static void
test_core_method_set(void)
{
    // Register a type with a constructor and finalizer.
    typedef struct { int dummy; } test_core_method_type_t;

    bool ok = n00b_type_register(
        typehash(test_core_method_type_t),
        &(n00b_type_info_t){
            .name      = "test_core_method_type_t",
            .alloc_len = sizeof(test_core_method_type_t),
            N00B_CORE_METHOD(N00B_BI_CONSTRUCTOR, test_widget_ctor),
            N00B_CORE_METHOD(N00B_BI_FINALIZER, test_widget_dtor),
        });
    assert(ok);

    auto cm_opt = n00b_type_lookup(typehash(test_core_method_type_t));
    assert(n00b_option_is_set(cm_opt));
    n00b_type_info_t *info = n00b_option_get(cm_opt);
    assert(info->core_vtable[N00B_BI_CONSTRUCTOR] == (n00b_vtable_entry)test_widget_ctor);
    assert(info->core_vtable[N00B_BI_FINALIZER] == (n00b_vtable_entry)test_widget_dtor);
    assert(info->core_vtable[N00B_BI_TO_STRING] == nullptr);

    printf("  [PASS] core method set\n");
}

// ============================================================================
// 9. n00b_type_add_method creates ext_vtable
// ============================================================================

static void
test_add_extension_method(void)
{
    auto ext_opt = n00b_type_lookup(typehash(test_widget_t));
    assert(n00b_option_is_set(ext_opt));
    n00b_type_info_t *info = n00b_option_get(ext_opt);

    // Initially no extension vtable.
    assert(!n00b_option_is_set(info->ext_vtable));

    // Add a method with params to exercise deep-copy.
    n00b_method_param_t local_params[2] = {
        {.type_hash = typehash(int), .type_name = "int"},
        {.type_hash = typehash(char *), .type_name = "char *"},
    };

    n00b_method_t m = {
        .fn          = (n00b_vtable_entry)test_widget_ctor,
        .name        = "do_thing",
        .return_type = {.type_hash = 0, .type_name = "void"},
        .params      = {.data = local_params, .len = 2, .cap = 2},
    };
    bool ok = n00b_type_add_method(typehash(test_widget_t), &m);
    assert(ok);

    // ext_vtable should now be set.
    assert(n00b_option_is_set(info->ext_vtable));

    n00b_array_t(n00b_method_t) *methods = n00b_option_get(info->ext_vtable);
    assert(methods->len == 1);
    assert(methods->data[0].fn == (n00b_vtable_entry)test_widget_ctor);
    assert(strcmp(methods->data[0].name, "do_thing") == 0);

    // Verify deep-copied strings are independent of locals.
    assert(methods->data[0].name != m.name);
    assert(strcmp(methods->data[0].return_type.type_name, "void") == 0);
    assert(methods->data[0].return_type.type_name != m.return_type.type_name);

    // Verify params were deep-copied.
    assert(methods->data[0].params.len == 2);
    assert(methods->data[0].params.data != local_params);
    assert(strcmp(methods->data[0].params.data[0].type_name, "int") == 0);
    assert(methods->data[0].params.data[0].type_name != local_params[0].type_name);
    assert(strcmp(methods->data[0].params.data[1].type_name, "char *") == 0);

    printf("  [PASS] add extension method\n");
}

// ============================================================================
// 10. Header size unchanged
// ============================================================================

static void
test_header_size(void)
{
    // n00b_alloc_type_info_t changed from char* to uint64_t.
    // Both are 8 bytes on 64-bit, so header layout is unchanged.
    assert(sizeof(n00b_alloc_type_info_t) == sizeof(void *));
    assert(sizeof(n00b_alloc_type_info_t) == 8);

    // Verify the full inline header size hasn't shifted.
    // The struct contains: guard(8) + tinfo(8) + alloc_len(4) + bitfields(4)
    // + cached_hash(16) + flexible array, aligned to N00B_ALIGN.
    assert(sizeof(n00b_inline_hdr_t) == N00B_ALLOC_HDR_SZ);

    printf("  [PASS] header size\n");
}

// ============================================================================
// 11. Vtable hash dispatch for uint64_t
// ============================================================================

static void
test_vtable_hash_uint64(void)
{
    uint64_t *p = n00b_alloc(uint64_t);
    *p = 42;

    // Hash slot should be populated.
    n00b_vtable_entry fn = n00b_obj_core_method(p, N00B_BI_HASH);
    assert(fn != nullptr);

    // n00b_hash with fn=nullptr should dispatch through the vtable.
    n00b_uint128_t h1 = n00b_hash(p, nullptr);
    assert(h1 != 0);

    // Second call should return the cached value.
    n00b_uint128_t h2 = n00b_hash(p, nullptr);
    assert(h1 == h2);

    printf("  [PASS] vtable hash uint64\n");
}

// ============================================================================
// 12. String hash via vtable
// ============================================================================

static void
test_vtable_hash_string(void)
{
    n00b_string_t *s = n00b_unicode_str_from_int(12345);

    n00b_vtable_entry fn = n00b_obj_core_method(s, N00B_BI_HASH);
    assert(fn != nullptr);

    n00b_uint128_t h = n00b_hash(s, nullptr);
    assert(h != 0);

    printf("  [PASS] vtable hash string\n");
}

// ============================================================================
// 13. Buffer vtable slots are populated
// ============================================================================

static void
test_buffer_vtable_slots(void)
{
    auto buf_opt = n00b_type_lookup(typehash(n00b_buffer_t));
    assert(n00b_option_is_set(buf_opt));
    n00b_type_info_t *info = n00b_option_get(buf_opt);

    // Constructor registered (n00b_buffer_init via N00B_CTOR_KARGS).
    assert(info->core_vtable[N00B_BI_CONSTRUCTOR] != nullptr);
    assert(info->ctor_takes_kargs);
    assert(info->core_vtable[N00B_BI_FINALIZER] != nullptr);
    assert(info->core_vtable[N00B_BI_HASH] != nullptr);
    assert(info->core_vtable[N00B_BI_LEN] != nullptr);
    assert(info->core_vtable[N00B_BI_COPY] != nullptr);
    assert(info->core_vtable[N00B_BI_ADD] != nullptr);
    assert(info->core_vtable[N00B_BI_TO_STRING] != nullptr);

    printf("  [PASS] buffer vtable slots\n");
}

// ============================================================================
// 14. Primitive to_string slot
// ============================================================================

static void
test_primitive_to_string(void)
{
    auto i64_opt = n00b_type_lookup(typehash(int64_t));
    assert(n00b_option_is_set(i64_opt));
    assert(n00b_option_get(i64_opt)->core_vtable[N00B_BI_TO_STRING] != nullptr);

    auto u64_opt = n00b_type_lookup(typehash(uint64_t));
    assert(n00b_option_is_set(u64_opt));
    assert(n00b_option_get(u64_opt)->core_vtable[N00B_BI_TO_STRING] != nullptr);

    auto dbl_opt = n00b_type_lookup(typehash(double));
    assert(n00b_option_is_set(dbl_opt));
    assert(n00b_option_get(dbl_opt)->core_vtable[N00B_BI_TO_STRING] != nullptr);

    auto bool_opt = n00b_type_lookup(typehash(bool));
    assert(n00b_option_is_set(bool_opt));
    assert(n00b_option_get(bool_opt)->core_vtable[N00B_BI_TO_STRING] != nullptr);

    printf("  [PASS] primitive to_string\n");
}

// ============================================================================
// 15. String vtable slots are populated
// ============================================================================

static void
test_string_vtable_slots(void)
{
    auto str_opt = n00b_type_lookup(typehash(n00b_string_t));
    assert(n00b_option_is_set(str_opt));
    n00b_type_info_t *info = n00b_option_get(str_opt);

    assert(info->core_vtable[N00B_BI_HASH] != nullptr);
    assert(info->core_vtable[N00B_BI_COPY] != nullptr);
    assert(info->core_vtable[N00B_BI_ADD] != nullptr);
    assert(info->core_vtable[N00B_BI_TO_LITERAL] != nullptr);
    assert(info->core_vtable[N00B_BI_LEN] != nullptr);

    // String now has a constructor registered.
    assert(info->core_vtable[N00B_BI_CONSTRUCTOR] != nullptr);
    assert(info->ctor_takes_kargs);
    assert(info->core_vtable[N00B_BI_FINALIZER] == nullptr);

    printf("  [PASS] string vtable slots\n");
}

// ============================================================================
// 16. Core method lookup for all builtin hash slots
// ============================================================================

static void
test_all_builtins_have_hash(void)
{
    // Every registered primitive should have a HASH slot.
    uint64_t hashes[] = {
        typehash(uint8_t),
        typehash(int32_t),
        typehash(uint32_t),
        typehash(int64_t),
        typehash(uint64_t),
        typehash(double),
        typehash(bool),
        typehash(n00b_string_t),
        typehash(n00b_buffer_t),
    };

    for (size_t i = 0; i < sizeof(hashes) / sizeof(hashes[0]); i++) {
        auto h_opt = n00b_type_lookup(hashes[i]);
        assert(n00b_option_is_set(h_opt));
        assert(n00b_option_get(h_opt)->core_vtable[N00B_BI_HASH] != nullptr);
    }

    printf("  [PASS] all builtins have hash\n");
}

// ============================================================================
// 17. Auto-constructor dispatch via vtable
// ============================================================================

// A type with a simple constructor for testing auto-dispatch.
typedef struct test_auto_ctor_t {
    int    magic;
    double value;
} test_auto_ctor_t;

static void
test_auto_ctor_init(void *obj)
{
    test_auto_ctor_t *self = (test_auto_ctor_t *)obj;
    self->magic = 0xCAFE;
    self->value = 3.14;
}

static void
test_auto_constructor_dispatch(void)
{
    // Register the type with a constructor.
    bool ok = n00b_type_register(
        typehash(test_auto_ctor_t),
        &(n00b_type_info_t){
            .name      = "test_auto_ctor_t",
            .alloc_len = sizeof(test_auto_ctor_t),
            N00B_CORE_METHOD(N00B_BI_CONSTRUCTOR, test_auto_ctor_init),
        });
    assert(ok);

    // n00b_alloc should auto-call the constructor.
    test_auto_ctor_t *obj = n00b_alloc(test_auto_ctor_t);
    assert(obj != nullptr);
    assert(obj->magic == 0xCAFE);
    assert(obj->value == 3.14);

    printf("  [PASS] auto-constructor dispatch\n");
}

// ============================================================================
// 18. Auto-finalizer dispatch via vtable
// ============================================================================

static int test_dtor_called = 0;

typedef struct test_auto_dtor_t {
    int x;
} test_auto_dtor_t;

static void
test_auto_dtor_fn(void *obj)
{
    (void)obj;
    test_dtor_called++;
}

static void
test_auto_finalizer_dispatch(void)
{
    bool ok = n00b_type_register(
        typehash(test_auto_dtor_t),
        &(n00b_type_info_t){
            .name      = "test_auto_dtor_t",
            .alloc_len = sizeof(test_auto_dtor_t),
            N00B_CORE_METHOD(N00B_BI_FINALIZER, test_auto_dtor_fn),
        });
    assert(ok);

    test_dtor_called = 0;
    test_auto_dtor_t *obj = n00b_alloc(test_auto_dtor_t);
    obj->x = 42;

    n00b_free(obj);
    assert(test_dtor_called == 1);

    printf("  [PASS] auto-finalizer dispatch\n");
}

// ============================================================================
// 19. No-constructor type: alloc returns zeroed memory, no crash
// ============================================================================

static void
test_no_constructor_type(void)
{
    // uint64_t has no constructor registered.
    uint64_t *p = n00b_alloc(uint64_t);
    assert(p != nullptr);
    assert(*p == 0);

    printf("  [PASS] no-constructor type\n");
}

// ============================================================================
// 20. skip_ctor prevents auto-init
// ============================================================================

static void
test_skip_ctor(void)
{
    // test_auto_ctor_t has a constructor that sets magic=0xCAFE.
    // skip_ctor karg removed: n00b_alloc always runs the registered ctor.
    // This test now verifies ctor ran (magic==0xCAFE).
    test_auto_ctor_t *obj = n00b_alloc(test_auto_ctor_t);
    assert(obj != nullptr);
    assert(obj->magic == 0xCAFE);

    printf("  [PASS] alloc runs ctor (skip_ctor removed)\n");
}

// ============================================================================
// 21. n00b_new calls explicit init, skips vtable ctor
// ============================================================================

static void
test_n00b_new_macro(void)
{
    // Use n00b_buffer_new which allocates and initializes the buffer.
    n00b_buffer_t *buf = n00b_buffer_new(100);
    assert(buf != nullptr);
    assert(n00b_buffer_len(buf) == 100);
    assert(buf->alloc_len >= 100);
    assert(buf->lock != nullptr);

    n00b_free(buf);

    printf("  [PASS] n00b_new macro\n");
}

// ============================================================================
// 22. Lock offset cleanup on free
// ============================================================================

static void
test_lock_offset_cleanup(void)
{
    // Buffer has lock_offset registered. Create via n00b_buffer_new
    // so the lock is properly set up.
    n00b_buffer_t *buf = n00b_buffer_new(16);
    assert(buf != nullptr);
    assert(buf->lock != nullptr);

    // n00b_free should clean up the lock via lock_offset before calling
    // the vtable finalizer (n00b_buffer_free). No crash = success.
    n00b_free(buf);

    printf("  [PASS] lock offset cleanup\n");
}

// ============================================================================
// 23. Buffer vtable finalizer: n00b_free runs n00b_buffer_free
// ============================================================================

static void
test_buffer_vtable_finalizer(void)
{
    n00b_buffer_t *buf = n00b_buffer_new(64);
    assert(buf != nullptr);
    assert(buf->data != nullptr);
    assert(n00b_buffer_len(buf) == 64);

    // n00b_free dispatches the vtable finalizer (n00b_buffer_free),
    // which frees buf->data.
    n00b_free(buf);

    printf("  [PASS] buffer vtable finalizer\n");
}

// ============================================================================
// 24. lock_offset field is set for buffer, not for dict
// ============================================================================

static void
test_lock_offset_registration(void)
{
    auto buf_lo_opt = n00b_type_lookup(typehash(n00b_buffer_t));
    assert(n00b_option_is_set(buf_lo_opt));
    n00b_type_info_t *buf_info = n00b_option_get(buf_lo_opt);
    assert(n00b_option_is_set(buf_info->lock_offset));

    uint32_t offset = n00b_option_get(buf_info->lock_offset);
    assert(offset == (uint32_t)offsetof(n00b_buffer_t, lock));

    // dict_untyped has no lock_offset.
    auto dict_lo_opt = n00b_type_lookup(typehash(n00b_dict_untyped_t));
    assert(n00b_option_is_set(dict_lo_opt));
    n00b_type_info_t *dict_info = n00b_option_get(dict_lo_opt);
    assert(!n00b_option_is_set(dict_info->lock_offset));

    printf("  [PASS] lock_offset registration\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running type registry tests...\n");

    test_builtin_lookup();
    test_name_field();
    test_unregistered_lookup();
    test_dynamic_registration();
    test_obj_typehash();
    test_type_info_for();
    test_empty_core_method();
    test_core_method_set();
    test_add_extension_method();
    test_header_size();
    test_vtable_hash_uint64();
    test_vtable_hash_string();
    test_buffer_vtable_slots();
    test_primitive_to_string();
    test_string_vtable_slots();
    test_all_builtins_have_hash();
    test_auto_constructor_dispatch();
    test_auto_finalizer_dispatch();
    test_no_constructor_type();
    test_skip_ctor();
    test_n00b_new_macro();
    test_lock_offset_cleanup();
    test_buffer_vtable_finalizer();
    test_lock_offset_registration();

    printf("All type registry tests passed.\n");
    n00b_shutdown();
    return 0;
}
