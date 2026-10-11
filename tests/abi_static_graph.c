#include "abi_static_graph.h"

#include "lib/alloc.h"
#include "lib/object_section.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void
check_target(const char *triple, const char *segment, const char *name)
{
    core_target_desc_t target;
    assert(core_target_profile(triple, 0, &target));
    const uint8_t root[16] = {1, 2, 3, 4};
    const uint8_t child[16] = {5, 6, 7, 8};
    const char *aliases[] = {"n00bb_test_root_alias"};
    const n00bb_static_graph_node_t nodes[] = {
        {.bytes = root, .size = sizeof root, .alignment = 8,
         .symbol = "n00bb_test_root", .aliases = aliases,
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
    const char *prefix = target.object_format == CORE_TARGET_OBJECT_MACH_O
        ? "_" : "";
    char native_root[64], native_alias[64];
    assert(snprintf(native_root, sizeof native_root, "%sn00bb_test_root",
                    prefix) > 0);
    assert(snprintf(native_alias, sizeof native_alias,
                    "%sn00bb_test_root_alias", prefix) > 0);
    for (uint32_t i = 0; i < 8; i++) {
        core_object_symbol_t symbol;
        if (!core_object_symbol_at(object, object_size,
                target.object_format, &section, i, &symbol)) continue;
        if (symbol.name.length == strlen(native_root)
            && !memcmp(symbol.name.bytes, native_root,
                       symbol.name.length)) {
            assert(symbol.section_offset == 0 && symbol.exported);
            found_root = true;
        }
        if (symbol.name.length == strlen(native_alias)
            && !memcmp(symbol.name.bytes, native_alias,
                       symbol.name.length)) {
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
    check_target("x86_64-linux", nullptr, ".data.rel.ro.n00b_lit");
    check_target("arm64-macos", "__DATA_CONST", "__n00b_lits");
    check_target("x86_64-windows", nullptr, ".rdata$n00b");
    return 0;
}
