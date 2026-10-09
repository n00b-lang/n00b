#include <assert.h>
#include <stddef.h>

#include "builtin_abi.h"

static_assert(sizeof(n00b_abi_range_v1) == 8);
static_assert(sizeof(n00b_abi_string_v1) == 16);
static_assert(sizeof(n00b_abi_type_v1) == 144);
static_assert(sizeof(n00b_abi_field_v1) == 88);
static_assert(sizeof(n00b_abi_trace_v1) == 40);
static_assert(sizeof(n00b_abi_trace_child_v1) == 32);
static_assert(sizeof(n00b_abi_trace_arm_v1) == 16);
static_assert(sizeof(n00b_abi_tag_extractor_v1) == 32);
static_assert(sizeof(n00b_abi_frame_layout_v1) == 24);
static_assert(offsetof(n00b_abi_type_v1, size_bits) == 8);
static_assert(offsetof(n00b_abi_type_v1, components) == 56);
static_assert(offsetof(n00b_abi_type_v1, named_arguments) == 128);

int main(void)
{
    int value = 0;
    assert(n00b_builtin_abi_ref_address(&value) == (uint64_t)(uintptr_t)&value);
    assert(!n00b_builtin_abi_type(0));
    assert(!n00b_builtin_abi_field(0));
    assert(!n00b_builtin_abi_trace(0));
    assert(!n00b_builtin_abi_trace_child(0));
    assert(!n00b_builtin_abi_trace_arm(0));
    assert(!n00b_builtin_abi_tag_extractor(0));
    assert(!n00b_builtin_abi_frame_layout(0));
    assert(!n00b_builtin_abi_bytes(0, UINT32_MAX));
    assert(n00b_builtin_abi_range_index(0, 0, 0) == -1);
    uint8_t body[] = {1, 2, 3};
    n00b_abi_string_v1 text = {0};
    text.short_value.selector = 3;
    assert(!n00b_builtin_abi_string_backing_at(&text));
    text.long_value.data = body;
    text.long_value.selector = (uint64_t)N00B_ABI_STRING_LONG
        << N00B_ABI_STRING_TAG_SHIFT;
    assert(n00b_builtin_abi_string_backing_at(&text) == body);
    text.span_value.selector = (uint64_t)N00B_ABI_STRING_SPAN
        << N00B_ABI_STRING_TAG_SHIFT;
    assert(n00b_builtin_abi_string_backing_at(&text) == body);
    text.rich_value.selector = (uint64_t)N00B_ABI_STRING_RICH
        << N00B_ABI_STRING_TAG_SHIFT;
    assert(n00b_builtin_abi_string_backing_at(&text) == body);
    assert(!n00b_builtin_abi_string_backing_at(NULL));
    return 0;
}
