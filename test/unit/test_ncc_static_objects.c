#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "adt/array.h"
#include "core/alloc.h"
#include "core/gc_map.h"
#include "core/mmaps.h"
#include "core/runtime.h"
#include "core/static_objects.h"
#include "text/strings/text_style.h"

typedef n00b_array_t(int) generated_int_array_t;
typedef n00b_string_t *n00b_string_ptr_t;
typedef void *generated_ptr_t;

typedef struct {
    n00b_string_ptr_t label;
    uint64_t          tag;
    n00b_string_ptr_t alt;
} generated_labeled_item_t;

static n00b_alloc_range_t *
range_for_address(const void *addr)
{
    auto found = n00b_mmap_range_by_address((void *)addr);
    assert(n00b_option_is_set(found));
    return n00b_option_get(found);
}

static void
test_generated_rstring_descriptors(void)
{
    n00b_string_t *plain  = r"descriptor";
    n00b_string_t *styled = r"[|b|]hi[|/b|]";

    n00b_static_objects_register_all();

    n00b_alloc_range_t *plain_range = range_for_address(plain);
    assert(plain_range->kind == n00b_mmap_static);
    assert(plain_range->len == sizeof(*plain));
    assert(plain_range->scan_kind == N00B_GC_SCAN_KIND_NONE);
    assert(plain_range->flags & N00B_STATIC_OBJECT_F_MUTABLE);
    assert(plain->u8_bytes == 10);
    assert(memcmp(plain->data, "descriptor", 10) == 0);

    n00b_alloc_range_t *styled_range = range_for_address(styled);
    assert(styled_range->kind == n00b_mmap_static);
    assert(styled_range->len == sizeof(*styled));
    assert(styled_range->scan_kind == N00B_GC_SCAN_KIND_NONE);
    assert(styled_range->flags & N00B_STATIC_OBJECT_F_MUTABLE);
    assert(styled->u8_bytes == 2);
    assert(styled->styling != nullptr);

    printf("  [PASS] generated_rstring_descriptors\n");
}

static void
test_generated_scalar_array_descriptor(void)
{
    const n00b_array_t(int) values = [10, 20, 30];

    n00b_static_objects_register_all();

    n00b_alloc_range_t *range = range_for_address(values.data);
    assert(range->kind == n00b_mmap_static);
    assert(range->len == sizeof(int) * 3);
    assert(range->scan_kind == N00B_GC_SCAN_KIND_NONE);
    assert(range->flags & N00B_STATIC_OBJECT_F_MUTABLE);
    assert(values.len == 3);
    assert(values.data[0] == 10);
    assert(values.data[2] == 30);

    printf("  [PASS] generated_scalar_array_descriptor\n");
}

static void
test_generated_pointer_array_descriptor(void)
{
    const n00b_array_t(n00b_string_ptr_t) words =
        [r"alpha", r"[|b|]hi[|/b|]"];
    const n00b_array_t(generated_ptr_t) ptrs =
        [nullptr, nullptr];

    n00b_static_objects_register_all();

    n00b_alloc_range_t *range = range_for_address(words.data);
    assert(range->kind == n00b_mmap_static);
    assert(range->len == sizeof(n00b_string_t *) * 2);
    assert(range->scan_kind == N00B_GC_SCAN_KIND_ALL);
    assert(range->flags & N00B_STATIC_OBJECT_F_MUTABLE);
    assert(words.len == 2);
    assert(words.data[0]->u8_bytes == 5);
    assert(words.data[1]->u8_bytes == 2);
    assert(words.data[1]->styling != nullptr);

    assert(range_for_address(words.data[0])->scan_kind == N00B_GC_SCAN_KIND_NONE);
    assert(range_for_address(words.data[1])->scan_kind == N00B_GC_SCAN_KIND_NONE);

    n00b_alloc_range_t *ptr_range = range_for_address(ptrs.data);
    assert(ptr_range->kind == n00b_mmap_static);
    assert(ptr_range->len == sizeof(generated_ptr_t) * 2);
    assert(ptr_range->scan_kind == N00B_GC_SCAN_KIND_ALL);
    assert(ptr_range->flags & N00B_STATIC_OBJECT_F_MUTABLE);
    assert(ptrs.len == 2);
    assert(ptrs.data[0] == nullptr);
    assert(ptrs.data[1] == nullptr);

    printf("  [PASS] generated_pointer_array_descriptor\n");
}

static void
test_generated_nested_array_descriptor(void)
{
    const n00b_array_t(generated_int_array_t) rows = [[1, 2], [3]];

    n00b_static_objects_register_all();

    n00b_alloc_range_t *range = range_for_address(rows.data);
    assert(range->kind == n00b_mmap_static);
    assert(range->len == sizeof(generated_int_array_t) * 2);
    assert(range->scan_kind == N00B_GC_SCAN_KIND_CALLBACK);
    assert(range->scan_cb == n00b_gc_scan_cb_struct_field);
    assert(range->scan_user != nullptr);
    assert(range->flags & N00B_STATIC_OBJECT_F_MUTABLE);
    assert(rows.len == 2);
    assert(rows.data[0].len == 2);
    assert(rows.data[0].data[1] == 2);
    assert(rows.data[1].len == 1);
    assert(rows.data[1].data[0] == 3);

    assert(range_for_address(rows.data[0].data)->scan_kind == N00B_GC_SCAN_KIND_NONE);
    assert(range_for_address(rows.data[1].data)->scan_kind == N00B_GC_SCAN_KIND_NONE);

    printf("  [PASS] generated_nested_array_descriptor\n");
}

static void
test_generated_aggregate_array_descriptor(void)
{
    const n00b_array_t(generated_labeled_item_t) items =
        [{ .label = r"aggregate", .tag = 44, .alt = nullptr }];

    n00b_static_objects_register_all();

    n00b_alloc_range_t *range = range_for_address(items.data);
    assert(range->kind == n00b_mmap_static);
    assert(range->len == sizeof(generated_labeled_item_t));
    assert(range->scan_kind == N00B_GC_SCAN_KIND_CALLBACK);
    assert(range->scan_cb == n00b_gc_scan_cb_struct_layout);
    assert(range->scan_user != nullptr);
    assert(range->flags & N00B_STATIC_OBJECT_F_MUTABLE);

    n00b_gc_struct_layout_t *layout = range->scan_user;
    assert(layout->count == 1);
    assert(layout->stride == sizeof(generated_labeled_item_t) / sizeof(void *));
    assert(layout->offset_count == 2);
    assert(layout->offsets[0] == offsetof(generated_labeled_item_t, label) / sizeof(void *));
    assert(layout->offsets[1] == offsetof(generated_labeled_item_t, alt) / sizeof(void *));

    assert(items.len == 1);
    assert(items.data[0].tag == 44);
    assert(items.data[0].alt == nullptr);
    assert(items.data[0].label->u8_bytes == 9);
    assert(memcmp(items.data[0].label->data, "aggregate", 9) == 0);
    assert(range_for_address(items.data[0].label)->scan_kind == N00B_GC_SCAN_KIND_NONE);

    printf("  [PASS] generated_aggregate_array_descriptor\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running ncc static object descriptor tests...\n");
    test_generated_rstring_descriptors();
    test_generated_scalar_array_descriptor();
    test_generated_pointer_array_descriptor();
    test_generated_nested_array_descriptor();
    test_generated_aggregate_array_descriptor();

    return 0;
}
