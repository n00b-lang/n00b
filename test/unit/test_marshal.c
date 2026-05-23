#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdatomic.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/alloc_mdata.h"
#include "core/arena.h"
#include "core/buffer.h"
#include "core/gc.h"
#include "core/gc_map.h"
#include "core/mmaps.h"
#include "core/runtime.h"
#include "core/stw.h"
#include "core/thread.h"
#include "util/marshal.h"

#define ARENA_OPTS(a) &(n00b_alloc_opts_t){.allocator = (n00b_allocator_t *)(a)}

typedef struct marshal_node_t {
    uint64_t               tag;
    uint64_t               scalar;
    struct marshal_node_t *next;
    struct marshal_node_t *alias;
} marshal_node_t;

typedef struct {
    uint64_t  tag;
    uint64_t *static_ref;
} marshal_static_ref_t;

typedef struct {
    uint64_t  tag;
    uint64_t *ptr;
} marshal_callback_ref_t;

typedef struct {
    struct marshal_node_t *real_ptr;
    uint64_t               scalar_tail;
} marshal_limited_scan_t;

#define TEST_MARSHAL_OP_ALLOC  UINT32_C(0xe11cbab0)
#define TEST_MARSHAL_OP_CPATCH UINT32_C(0xe31cbab0)
#define TEST_MARSHAL_OP_SPATCH UINT32_C(0xe41cbab0)
#define TEST_MARSHAL_OP_STOP   UINT32_C(0xe51cbab0)

typedef struct {
    uint64_t marshal_magic;
    uint32_t version;
    uint32_t base_address;
    uint32_t root_offset;
    uint32_t flags;
} test_marshal_stream_header_t;

typedef struct {
    uint32_t op;
    uint32_t flags;
    uint64_t vaddr;
    uint64_t user_len;
    uint64_t payload_len;
    uint64_t tinfo;
    uint32_t ptr_words;
    uint32_t scan_kind;
    uint32_t no_scan;
    uint32_t is_array;
} test_marshal_alloc_record_t;

typedef struct {
    uint32_t op;
    uint32_t reserved;
    uint64_t vaddr;
    uint64_t value;
} test_marshal_cpatch_record_t;

typedef struct {
    uint32_t op;
    uint32_t check_len;
    uint64_t vaddr;
    uint64_t static_addr;
    uint64_t static_start;
    uint64_t static_len;
    uint64_t object_id;
    uint8_t  check[16];
} test_marshal_spatch_record_t;

typedef struct {
    n00b_arena_t    *arena;
    _Atomic uint32_t run;
    _Atomic uint32_t done;
    _Atomic uint32_t collections;
    uint32_t         delay_iters;
} gc_request_t;

static uint64_t
stress_value(size_t i)
{
    return UINT64_C(0x0f0e0d0c00000000) | (uint64_t)i;
}

static void *
gc_request_worker(void *arg)
{
    gc_request_t *request = arg;

    while (!atomic_load(&request->run)) {
        n00b_thread_checkin();
    }

    for (uint32_t i = 0; i < request->delay_iters; i++) {
        __asm__ __volatile__("" ::: "memory");
    }

    n00b_stop_the_world();
    n00b_collect(request->arena);
    n00b_restart_the_world();
    atomic_fetch_add(&request->collections, 1);
    atomic_store(&request->done, 1);

    return nullptr;
}

static n00b_thread_t *
start_gc_request(gc_request_t *request, n00b_arena_t *arena, uint32_t delay_iters)
{
    *request = (gc_request_t){.arena = arena, .delay_iters = delay_iters};

    auto result = n00b_thread_spawn(gc_request_worker, request);
    assert(n00b_result_is_ok(result));

    return n00b_result_get(result);
}

static n00b_buffer_t *
buffer_slice(n00b_buffer_t *buf, int64_t start, int64_t len)
{
    _n00b_buffer_rlock(buf);
    n00b_buffer_t *result = n00b_buffer_from_bytes(buf->data + start, len);
    _n00b_buffer_unlock(buf);

    return result;
}

static n00b_buffer_t *
buffer_copy_with_extra(n00b_buffer_t *buf, int64_t extra)
{
    _n00b_buffer_rlock(buf);
    int64_t len   = (int64_t)buf->byte_len;
    char   *bytes = n00b_alloc_array(char, (size_t)(len + extra));
    memcpy(bytes, buf->data, (size_t)len);
    memset(bytes + len, 0x5a, (size_t)extra);
    _n00b_buffer_unlock(buf);

    return n00b_buffer_from_bytes(bytes, len + extra);
}

static n00b_buffer_t *
buffer_copy_mutating_alloc(n00b_buffer_t *buf,
                           void (*mutate)(test_marshal_alloc_record_t *rec))
{
    _n00b_buffer_rlock(buf);
    int64_t len   = (int64_t)buf->byte_len;
    char   *bytes = n00b_alloc_array(char, (size_t)len);
    memcpy(bytes, buf->data, (size_t)len);
    _n00b_buffer_unlock(buf);

    test_marshal_alloc_record_t *rec = (void *)(bytes + sizeof(test_marshal_stream_header_t));
    assert(rec->op == TEST_MARSHAL_OP_ALLOC);
    mutate(rec);

    return n00b_buffer_from_bytes(bytes, len);
}

static n00b_buffer_t *
buffer_copy_mutating_record(n00b_buffer_t *buf,
                            uint32_t       wanted_op,
                            void (*mutate)(void *rec))
{
    _n00b_buffer_rlock(buf);
    int64_t len   = (int64_t)buf->byte_len;
    char   *bytes = n00b_alloc_array(char, (size_t)len);
    memcpy(bytes, buf->data, (size_t)len);
    _n00b_buffer_unlock(buf);

    size_t ix = sizeof(test_marshal_stream_header_t);
    while (ix + sizeof(uint32_t) <= (size_t)len) {
        uint32_t op = *(uint32_t *)(bytes + ix);
        if (op == wanted_op) {
            mutate(bytes + ix);
            return n00b_buffer_from_bytes(bytes, len);
        }

        if (op == TEST_MARSHAL_OP_ALLOC) {
            test_marshal_alloc_record_t *rec = (void *)(bytes + ix);
            ix += sizeof(*rec) + (size_t)rec->payload_len;
            continue;
        }
        if (op == TEST_MARSHAL_OP_CPATCH) {
            ix += sizeof(test_marshal_cpatch_record_t);
            continue;
        }
        if (op == TEST_MARSHAL_OP_SPATCH) {
            ix += sizeof(test_marshal_spatch_record_t);
            continue;
        }
        break;
    }

    assert(false && "requested marshal record not found");
    return nullptr;
}

static void
mark_all_cb(n00b_gc_map_t *m, void *user)
{
    (void)user;
    n00b_gc_map_mark_all(m);
}

static void
mutate_unknown_op(test_marshal_alloc_record_t *rec)
{
    rec->op = UINT32_C(0xdeadbeef);
}

static void
mutate_short_payload(test_marshal_alloc_record_t *rec)
{
    assert(rec->payload_len >= sizeof(uint64_t));
    rec->payload_len -= sizeof(uint64_t);
}

static void
mutate_callback_scan(test_marshal_alloc_record_t *rec)
{
    rec->scan_kind = N00B_GC_SCAN_KIND_CALLBACK;
}

static void
mutate_bad_scan_kind(test_marshal_alloc_record_t *rec)
{
    rec->scan_kind = N00B_GC_SCAN_KIND_CALLBACK + 1;
}

static void
mutate_bad_alloc_flags(test_marshal_alloc_record_t *rec)
{
    rec->flags |= UINT32_C(0x80000000);
}

static void
assert_unmarshal_status(n00b_buffer_t *buf, n00b_marshal_status_t status)
{
    n00b_unmarshal_ctx_t *ctx   = n00b_unmarshal_ctx_new();
    n00b_list_t(void *)   roots = n00b_unmarshal_incremental(ctx, buf);
    assert(n00b_list_len(roots) == 0);
    assert(n00b_unmarshal_ctx_status(ctx) == status);
    n00b_unmarshal_ctx_destroy(ctx);
}

static void
set_ptr_words(void *obj, uint32_t ptr_words)
{
    n00b_alloc_info_t info = n00b_find_alloc_info(obj);

    if (info.kind == n00b_alloc_oob) {
        info.hdr.oob->ptr_words = ptr_words;
        if (info.hdr.oob->hcur) {
            info.hdr.oob->hcur->ptr_words = ptr_words;
        }
        return;
    }

    assert(info.kind == n00b_alloc_inline);
    info.hdr.in_line->ptr_words = ptr_words;
}

static void
test_cycle_shared_and_collision(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);

    marshal_node_t *a = n00b_alloc_with_opts(marshal_node_t, ARENA_OPTS(arena));
    marshal_node_t *b = n00b_alloc_with_opts(marshal_node_t, ARENA_OPTS(arena));

    a->tag    = 0xa0a0;
    a->scalar = (UINT64_C(0x12345678) << 32) | UINT64_C(0x00000042);
    a->next   = b;
    a->alias  = b;

    b->tag    = 0xb0b0;
    b->scalar = 99;
    b->next   = a;
    b->alias  = b;

    n00b_buffer_t *buf = n00b_marshal(a, .base_address = 0x12345678u);
    assert(buf != nullptr);
    assert(n00b_buffer_len(buf) > 0);

    marshal_node_t *root = n00b_unmarshal_one(buf, .target_arena = arena);
    assert(root != nullptr);
    assert(root != a);
    assert(root->tag == 0xa0a0);
    assert(root->scalar == ((UINT64_C(0x12345678) << 32) | UINT64_C(0x00000042)));
    assert(root->next != nullptr);
    assert(root->next == root->alias);
    assert(root->next->tag == 0xb0b0);
    assert(root->next->next == root);
    assert(root->next->alias == root->next);

    n00b_gc_register_root(root);
    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();
    assert(root->tag == 0xa0a0);
    assert(root->next->next == root);
    assert(root->next == root->alias);
    n00b_gc_unregister_root(root);

    printf("  [PASS] cycle_shared_and_collision\n");
}

static void
test_static_pointer_patch(void)
{
    static uint64_t static_words[4] = {
        UINT64_C(0x0102030405060708),
        UINT64_C(0x1112131415161718),
        UINT64_C(0x2122232425262728),
        UINT64_C(0x3132333435363738),
    };

    (void)n00b_mmap_register(static_words,
                             static_words + 4,
                             n00b_mmap_static,
                             .file              = "test_marshal_static_words",
                             .order_id          = UINT64_C(0x6006),
                             .definitely_unique = false);
    (void)n00b_static_object_register(static_words,
                                      sizeof(static_words),
                                      0,
                                      .scan_kind = N00B_GC_SCAN_KIND_NONE,
                                      .object_id = UINT64_C(0x60060001));

    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);
    marshal_static_ref_t *src = n00b_alloc_with_opts(marshal_static_ref_t,
                                                     ARENA_OPTS(arena));
    src->tag        = 0xcafe;
    src->static_ref = &static_words[1];

    n00b_buffer_t *buf = n00b_marshal(src, .base_address = 0x3456789au);
    assert(buf != nullptr);

    marshal_static_ref_t *copy = n00b_unmarshal_one(buf, .target_arena = arena);
    assert(copy != nullptr);
    assert(copy != src);
    assert(copy->tag == 0xcafe);
    assert(copy->static_ref == &static_words[1]);
    assert(*copy->static_ref == static_words[1]);

    n00b_gc_register_root(copy);
    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();
    assert(copy->static_ref == &static_words[1]);
    n00b_gc_unregister_root(copy);

    printf("  [PASS] static_pointer_patch\n");
}

static void
test_ptr_words_limits_scan_extent(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);

    marshal_node_t *decoy = n00b_alloc_with_opts(marshal_node_t, ARENA_OPTS(arena));
    decoy->tag            = 0xd0d0;
    decoy->scalar         = 88;
    decoy->next           = nullptr;
    decoy->alias          = nullptr;

    marshal_limited_scan_t *src = n00b_alloc_with_opts(marshal_limited_scan_t,
                                                       ARENA_OPTS(arena));
    src->real_ptr    = decoy;
    src->scalar_tail = (uint64_t)(uintptr_t)decoy;
    set_ptr_words(src, 1);

    n00b_buffer_t *buf = n00b_marshal(src, .base_address = 0x65432110u);
    assert(buf != nullptr);

    marshal_limited_scan_t *copy = n00b_unmarshal_one(buf, .target_arena = arena);
    assert(copy != nullptr);
    assert(copy != src);
    assert(copy->real_ptr != nullptr);
    assert(copy->real_ptr != decoy);
    assert(copy->real_ptr->tag == decoy->tag);
    assert(copy->scalar_tail == (uint64_t)(uintptr_t)decoy);
    assert(copy->scalar_tail != (uint64_t)(uintptr_t)copy->real_ptr);

    n00b_alloc_info_t copy_info = n00b_find_alloc_info(copy);
    if (copy_info.kind == n00b_alloc_oob) {
        assert(copy_info.hdr.oob->ptr_words == 1);
    }
    else {
        assert(copy_info.kind == n00b_alloc_inline);
        assert(copy_info.hdr.in_line->ptr_words == 1);
    }

    printf("  [PASS] ptr_words_limits_scan_extent\n");
}

static void
mutate_ptr_words_too_large(test_marshal_alloc_record_t *rec)
{
    rec->ptr_words = (uint32_t)(rec->user_len / sizeof(uint64_t)) + 1;
}

static void
mutate_cpatch_unaligned(void *rec)
{
    ((test_marshal_cpatch_record_t *)rec)->vaddr += 1;
}

static void
mutate_spatch_unaligned(void *rec)
{
    ((test_marshal_spatch_record_t *)rec)->vaddr += 1;
}

static void
test_bad_ptr_words_rejected(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);
    marshal_node_t *node = n00b_alloc_with_opts(marshal_node_t, ARENA_OPTS(arena));
    node->tag            = 0xf00d;
    node->scalar         = 5;
    node->next           = nullptr;
    node->alias          = nullptr;

    n00b_buffer_t *buf = n00b_marshal(node, .base_address = 0x76543210u);
    assert(buf != nullptr);
    n00b_buffer_t *bad = buffer_copy_mutating_alloc(buf, mutate_ptr_words_too_large);
    assert_unmarshal_status(bad, N00B_MARSHAL_ERR_BAD_STREAM);

    printf("  [PASS] bad_ptr_words_rejected\n");
}

static void
test_callback_scan_boundary(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);

    marshal_callback_ref_t *src = n00b_alloc_size_with_opts(
        1,
        sizeof(marshal_callback_ref_t),
        &(n00b_alloc_opts_t){
            .allocator = (n00b_allocator_t *)arena,
            .scan_kind = N00B_GC_SCAN_KIND_CALLBACK,
            .scan_cb   = mark_all_cb,
        });
    src->tag = 0xcbcb;
    src->ptr = nullptr;

    n00b_marshal_ctx_t *mctx = n00b_marshal_ctx_new();
    assert(n00b_marshal_incremental(mctx, src) == nullptr);
    assert(n00b_marshal_ctx_status(mctx) == N00B_MARSHAL_ERR_UNSUPPORTED_SCAN_POLICY);
    n00b_marshal_ctx_destroy(mctx);

    marshal_node_t *plain = n00b_alloc_with_opts(marshal_node_t, ARENA_OPTS(arena));
    plain->tag            = 0xeeee;
    plain->scalar         = 7;
    plain->next           = nullptr;
    plain->alias          = nullptr;

    n00b_buffer_t *buf = n00b_marshal(plain, .base_address = 0x456789abu);
    assert(buf != nullptr);
    n00b_buffer_t *bad = buffer_copy_mutating_alloc(buf, mutate_callback_scan);
    assert_unmarshal_status(bad, N00B_MARSHAL_ERR_UNSUPPORTED_SCAN_POLICY);

    printf("  [PASS] callback_scan_boundary\n");
}

static void
test_malformed_stream_hardening(void)
{
    static uint64_t static_words[2] = {
        UINT64_C(0x4142434445464748),
        UINT64_C(0x5152535455565758),
    };

    (void)n00b_mmap_register(static_words,
                             static_words + 2,
                             n00b_mmap_static,
                             .file              = "test_marshal_bad_spatch_words",
                             .order_id          = UINT64_C(0x6007),
                             .definitely_unique = false);
    (void)n00b_static_object_register(static_words,
                                      sizeof(static_words),
                                      0,
                                      .scan_kind = N00B_GC_SCAN_KIND_NONE,
                                      .object_id = UINT64_C(0x60070001));

    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);
    marshal_node_t *node = n00b_alloc_with_opts(marshal_node_t, ARENA_OPTS(arena));
    node->tag            = 0xabab;
    node->scalar         = (UINT64_C(0x56789abc) << 32) | UINT64_C(5);
    node->next           = nullptr;
    node->alias          = nullptr;

    marshal_static_ref_t *static_ref = n00b_alloc_with_opts(marshal_static_ref_t,
                                                            ARENA_OPTS(arena));
    static_ref->tag        = 0xfefe;
    static_ref->static_ref = &static_words[0];

    n00b_buffer_t *buf = n00b_marshal(node, .base_address = 0x56789abcu);
    assert(buf != nullptr);

    n00b_buffer_t *trailing = buffer_copy_with_extra(buf, 1);
    assert_unmarshal_status(trailing, N00B_MARSHAL_ERR_BAD_STREAM);

    n00b_buffer_t *unknown_op = buffer_copy_mutating_alloc(buf, mutate_unknown_op);
    assert_unmarshal_status(unknown_op, N00B_MARSHAL_ERR_BAD_STREAM);

    n00b_buffer_t *short_payload = buffer_copy_mutating_alloc(buf, mutate_short_payload);
    assert_unmarshal_status(short_payload, N00B_MARSHAL_ERR_BAD_STREAM);

    n00b_buffer_t *bad_kind = buffer_copy_mutating_alloc(buf, mutate_bad_scan_kind);
    assert_unmarshal_status(bad_kind, N00B_MARSHAL_ERR_BAD_STREAM);

    n00b_buffer_t *bad_flags = buffer_copy_mutating_alloc(buf, mutate_bad_alloc_flags);
    assert_unmarshal_status(bad_flags, N00B_MARSHAL_ERR_BAD_STREAM);

    n00b_buffer_t *bad_cpatch = buffer_copy_mutating_record(buf,
                                                            TEST_MARSHAL_OP_CPATCH,
                                                            mutate_cpatch_unaligned);
    assert_unmarshal_status(bad_cpatch, N00B_MARSHAL_ERR_BAD_STREAM);

    n00b_buffer_t *spatch_stream = n00b_marshal(static_ref,
                                                .base_address = 0x56789abcu);
    assert(spatch_stream != nullptr);
    n00b_buffer_t *bad_spatch = buffer_copy_mutating_record(spatch_stream,
                                                            TEST_MARSHAL_OP_SPATCH,
                                                            mutate_spatch_unaligned);
    assert_unmarshal_status(bad_spatch, N00B_MARSHAL_ERR_BAD_STREAM);

    _n00b_buffer_rlock(buf);
    int64_t len   = (int64_t)buf->byte_len;
    char   *bytes = n00b_alloc_array(char, (size_t)len);
    memcpy(bytes, buf->data, (size_t)len);
    _n00b_buffer_unlock(buf);
    ((test_marshal_stream_header_t *)bytes)->marshal_magic ^= UINT64_C(0x55);

    n00b_unmarshal_ctx_t *ctx = n00b_unmarshal_ctx_new();
    n00b_buffer_t        *c1  = n00b_buffer_from_bytes(bytes, 4);
    n00b_list_t(void *) roots = n00b_unmarshal_incremental(ctx, c1);
    assert(n00b_list_len(roots) == 0);
    assert(n00b_unmarshal_ctx_status(ctx) == N00B_MARSHAL_ERR_INCOMPLETE_STREAM);

    n00b_buffer_t *c2 = n00b_buffer_from_bytes(bytes + 4, len - 4);
    roots = n00b_unmarshal_incremental(ctx, c2);
    assert(n00b_list_len(roots) == 0);
    assert(n00b_unmarshal_ctx_status(ctx) == N00B_MARSHAL_ERR_BAD_STREAM);
    n00b_unmarshal_ctx_destroy(ctx);

    printf("  [PASS] malformed_stream_hardening\n");
}

static void
test_single_root_context_boundary(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);
    marshal_node_t *a = n00b_alloc_with_opts(marshal_node_t, ARENA_OPTS(arena));
    marshal_node_t *b = n00b_alloc_with_opts(marshal_node_t, ARENA_OPTS(arena));

    a->tag = 1;
    b->tag = 2;

    n00b_marshal_ctx_t *ctx = n00b_marshal_ctx_new();
    assert(n00b_marshal_incremental(ctx, a, .close = false) != nullptr);
    assert(n00b_marshal_incremental(ctx, b, .close = true) == nullptr);
    assert(n00b_marshal_ctx_status(ctx) == N00B_MARSHAL_ERR_CONTEXT_CLOSED);
    n00b_marshal_ctx_destroy(ctx);

    printf("  [PASS] single_root_context_boundary\n");
}

static void
test_gc_pressure_during_round_trip(void)
{
    enum { STRESS_WORDS = 64 * 1024 };

    n00b_arena_t *source = n00b_new_arena(.size = 4096, .use_gc = true);
    uint64_t     *blob   = n00b_alloc_size_with_opts(
        STRESS_WORDS,
        sizeof(uint64_t),
        &(n00b_alloc_opts_t){
            .allocator = (n00b_allocator_t *)source,
            .no_scan   = true,
        });

    for (size_t i = 0; i < STRESS_WORDS; i++) {
        blob[i] = stress_value(i);
    }

    n00b_gc_register_root(blob);

    gc_request_t   marshal_gc;
    n00b_thread_t *marshal_thread = start_gc_request(&marshal_gc, source, 1000000);
    atomic_store(&marshal_gc.run, 1);
    n00b_buffer_t *buf = n00b_marshal(blob, .base_address = 0x23456789u);
    while (!atomic_load(&marshal_gc.done)) {
        n00b_thread_checkin();
    }
    n00b_thread_join(marshal_thread);
    assert(atomic_load(&marshal_gc.collections) == 1);

    assert(buf != nullptr);
    assert(n00b_buffer_len(buf) > (int64_t)(STRESS_WORDS * sizeof(uint64_t)));
    assert(blob[12345] == stress_value(12345));

    n00b_arena_t *target     = n00b_new_arena(.size = 4096, .use_gc = true);
    n00b_segment_t *old_head = target->current_segment;

    n00b_unmarshal_ctx_t *uctx = n00b_unmarshal_ctx_new(.target_arena = target);
    int64_t               len  = n00b_buffer_len(buf);
    int64_t               c1   = 17;
    int64_t               c2   = 4093;

    n00b_buffer_t *chunk = buffer_slice(buf, 0, c1);
    n00b_list_t(void *) roots = n00b_unmarshal_incremental(uctx, chunk);
    assert(n00b_list_len(roots) == 0);
    assert(n00b_unmarshal_ctx_status(uctx) == N00B_MARSHAL_ERR_INCOMPLETE_STREAM);

    chunk = buffer_slice(buf, c1, c2);
    roots = n00b_unmarshal_incremental(uctx, chunk);
    assert(n00b_list_len(roots) == 0);
    assert(n00b_unmarshal_ctx_status(uctx) == N00B_MARSHAL_ERR_INCOMPLETE_STREAM);

    gc_request_t   unmarshal_gc;
    n00b_thread_t *unmarshal_thread = start_gc_request(&unmarshal_gc, target, 1000000);
    atomic_store(&unmarshal_gc.run, 1);
    chunk = buffer_slice(buf, c1 + c2, len - c1 - c2);
    roots = n00b_unmarshal_incremental(uctx, chunk);
    while (!atomic_load(&unmarshal_gc.done)) {
        n00b_thread_checkin();
    }
    n00b_thread_join(unmarshal_thread);
    assert(atomic_load(&unmarshal_gc.collections) == 1);

    assert(n00b_unmarshal_ctx_status(uctx) == N00B_MARSHAL_OK);
    assert(n00b_list_len(roots) == 1);

    uint64_t *copy = n00b_list_get(roots, 0);
    n00b_gc_register_root(copy);

    assert(copy != nullptr);
    assert(copy != blob);
    assert(copy[0] == stress_value(0));
    assert(copy[12345] == stress_value(12345));
    assert(copy[STRESS_WORDS - 1] == stress_value(STRESS_WORDS - 1));
    assert(target->current_segment != old_head);
    assert(target->collection_enabled);

    n00b_stop_the_world();
    n00b_collect(target);
    n00b_restart_the_world();
    assert(copy[12345] == stress_value(12345));

    n00b_gc_unregister_root(copy);
    n00b_gc_unregister_root(blob);
    n00b_unmarshal_ctx_destroy(uctx);

    printf("  [PASS] gc_pressure_during_round_trip\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running marshal tests...\n");
    test_cycle_shared_and_collision();
    test_static_pointer_patch();
    test_ptr_words_limits_scan_extent();
    test_bad_ptr_words_rejected();
    test_callback_scan_boundary();
    test_malformed_stream_hardening();
    test_single_root_context_boundary();
    test_gc_pressure_during_round_trip();
    printf("All marshal tests passed.\n");

    n00b_shutdown();
    return 0;
}
