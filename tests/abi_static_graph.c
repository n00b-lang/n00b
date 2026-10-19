#include "abi_static_graph.h"

#include "lib/alloc.h"
#include "lib/object_section.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void
check_target(core_string_t triple, core_string_t segment, core_string_t name)
{
    core_target_desc_t target;
    assert(core_target_profile(triple, 0, &target));
    const uint8_t root[16] = {1, 2, 3, 4};
    const uint8_t child[16] = {5, 6, 7, 8};
    const core_string_t aliases[] = {
        CORE_STRING_STATIC("n00bb_test_root_alias"),
    };
    const n00bb_static_graph_node_t nodes[] = {
        {.bytes = root, .size = sizeof root, .alignment = 8,
         .symbol = CORE_STRING_STATIC("n00bb_test_root"), .aliases = aliases,
         .alias_count = 1},
        {.bytes = child, .size = sizeof child, .alignment = 32},
    };
    const n00bb_static_graph_edge_t edges[] = {
        {.source = 0, .source_offset = 8, .target = 1,
         .target_offset = 1},
        {.source = 1, .source_offset = 8, .target = 0},
    };
    uint8_t *object = nullptr;
    size_t object_size = 0;
    assert(n00bb_static_graph_emit(&target, nodes, 2, edges, 2,
                                   &object, &object_size));
    assert(object && object_size);
    core_object_section_t section;
    assert(core_object_section_find(object, object_size,
                                    target.object_format, segment, name,
                                    &section));
    assert(section.size == 32 + sizeof child);
    assert(section.alignment == 32);
    assert(section.relocation_count == 2);
    bool found_root = false, found_alias = false;
    bool mach_o = target.object_format == CORE_TARGET_OBJECT_MACH_O;
    core_string_t native_root = mach_o
        ? CORE_STRING_STATIC("_n00bb_test_root")
        : CORE_STRING_STATIC("n00bb_test_root");
    core_string_t native_alias = mach_o
        ? CORE_STRING_STATIC("_n00bb_test_root_alias")
        : CORE_STRING_STATIC("n00bb_test_root_alias");
    for (uint32_t i = 0; i < 8; i++) {
        core_object_symbol_t symbol;
        if (!core_object_symbol_at(object, object_size,
                target.object_format, &section, i, &symbol)) continue;
        core_string_t symbol_name = core_string_borrow(
            (const char *)symbol.name.bytes, symbol.name.length);
        if (core_string_eq(symbol_name, native_root)) {
            assert(symbol.section_offset == 0 && symbol.exported);
            found_root = true;
        }
        if (core_string_eq(symbol_name, native_alias)) {
            assert(symbol.section_offset == 0 && symbol.exported);
            found_alias = true;
        }
    }
    assert(found_root && found_alias);
    assert(!memcmp(section.bytes, root, 8));
    assert(!memcmp(section.bytes + 32, child, 8));
    for (uint32_t i = 0; i < section.relocation_count; i++) {
        core_object_absolute64_relocation_t relocation;
        assert(core_object_absolute64_relocation_at(&section,
            target.object_format, i, &relocation));
        uint64_t target_offset = relocation.addend_bits
            - (relocation.target_is_section ? section.virtual_address : 0);
        if (relocation.section_offset == 8)
            assert(target_offset == 33);
        else {
            assert(relocation.section_offset == 40);
            assert(target_offset == 0);
        }
    }
    core_free(object);

    const n00bb_static_graph_edge_t repeated[] = {
        edges[0], edges[1], edges[0],
    };
    object = nullptr;
    object_size = 0;
    assert(n00bb_static_graph_emit(&target, nodes, 2, repeated, 3,
                                   &object, &object_size));
    assert(core_object_section_find(object, object_size,
                                    target.object_format, segment, name,
                                    &section));
    assert(section.relocation_count == 2);
    core_free(object);

    const n00bb_static_graph_edge_t conflicting[] = {
        edges[0],
        {.source = 0, .source_offset = 8, .target = 0},
    };
    object = nullptr;
    object_size = 0;
    assert(!n00bb_static_graph_emit(&target, nodes, 2, conflicting, 2,
                                    &object, &object_size));
    assert(!object && !object_size);

    const n00bb_static_graph_edge_t invalid = {
        .source = 0, .source_offset = sizeof root - 1, .target = 1,
    };
    object = nullptr;
    object_size = 0;
    assert(!n00bb_static_graph_emit(&target, nodes, 2, &invalid, 1,
                                    &object, &object_size));
    assert(!object && !object_size);
}

int main(void)
{
    check_target(CORE_STRING_STATIC("x86_64-linux"), (core_string_t){},
                 CORE_STRING_STATIC(".data.rel.ro.n00b_lit"));
    check_target(CORE_STRING_STATIC("arm64-macos"),
                 CORE_STRING_STATIC("__DATA_CONST"),
                 CORE_STRING_STATIC("__n00b_lits"));
    check_target(CORE_STRING_STATIC("x86_64-windows"), (core_string_t){},
                 CORE_STRING_STATIC(".rdata$n00b"));
    return 0;
}
