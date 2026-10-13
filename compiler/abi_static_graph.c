#include "abi_static_graph.h"

#include "lib/alloc.h"
#include "lib/object_emit.h"

#include <stdlib.h>
#include <string.h>

static int
edge_order(const void *left, const void *right)
{
    const n00bb_static_graph_edge_t *a = left, *b = right;
    if (a->source != b->source) return a->source < b->source ? -1 : 1;
    if (a->source_offset != b->source_offset)
        return a->source_offset < b->source_offset ? -1 : 1;
    if (a->target != b->target) return a->target < b->target ? -1 : 1;
    if (a->target_offset != b->target_offset)
        return a->target_offset < b->target_offset ? -1 : 1;
    return 0;
}

static const char *
native_symbol_name(const core_target_desc_t *target, const char *name,
                   char **owned)
{
    if (target->object_format != CORE_TARGET_OBJECT_MACH_O) return name;
    size_t length = strlen(name);
    if (length > SIZE_MAX - 2) return nullptr;
    char *prefixed = core_alloc_size(1, length + 2);
    if (!prefixed) return nullptr;
    prefixed[0] = '_';
    memcpy(prefixed + 1, name, length + 1);
    *owned = prefixed;
    return prefixed;
}

bool
n00bb_static_graph_emit(const core_target_desc_t *target,
    const n00bb_static_graph_node_t *nodes, size_t node_count,
    const n00bb_static_graph_edge_t *edges, size_t edge_count,
    uint8_t **object, size_t *object_size)
{
    if (!target || !nodes || !node_count || (edge_count && !edges)
        || !object || !object_size || node_count > UINT32_MAX)
        return false;

    const char *section_name = nullptr;
    const char *segment = nullptr;
    switch (target->object_format) {
    case CORE_TARGET_OBJECT_ELF:
        section_name = ".data.rel.ro.n00b_lit";
        break;
    case CORE_TARGET_OBJECT_MACH_O:
        section_name = "__n00b_lits";
        segment = "__DATA_CONST";
        break;
    case CORE_TARGET_OBJECT_COFF:
        section_name = ".rdata$n00b";
        break;
    default:
        return false;
    }

    size_t *offsets = core_alloc_array(size_t, node_count);
    size_t exported_count = 0;
    for (size_t i = 0; i < node_count; i++) {
        size_t primary = nodes[i].symbol != nullptr;
        if (exported_count > SIZE_MAX - primary
            || nodes[i].alias_count > SIZE_MAX - exported_count - primary)
            return false;
        exported_count += nodes[i].alias_count
            + (nodes[i].symbol != nullptr);
    }
    if (exported_count == SIZE_MAX) return false;
    core_object_emit_symbol_t *symbols = core_alloc_array(
        core_object_emit_symbol_t, exported_count + 1);
    char **owned_names = core_alloc_array(char *, exported_count);
    core_object_emit_absolute64_t *relocations = edge_count
        ? core_alloc_array(core_object_emit_absolute64_t, edge_count)
        : nullptr;
    n00bb_static_graph_edge_t *ordered_edges = edge_count
        ? core_alloc_array(n00bb_static_graph_edge_t, edge_count)
        : nullptr;
    uint8_t *data = nullptr;
    bool valid = offsets && symbols && owned_names
        && (!edge_count || (relocations && ordered_edges));
    if (valid && edge_count) {
        memcpy(ordered_edges, edges, edge_count * sizeof(*edges));
        qsort(ordered_edges, edge_count, sizeof(*ordered_edges), edge_order);
    }
    size_t total = 0;
    uint32_t section_alignment = 1;
    size_t symbol_count = 1;
    if (valid) symbols[0] = (core_object_emit_symbol_t){
        .name = "__n00bb_graph_data", .section_index = 1,
    };
    for (size_t i = 0; valid && i < node_count; i++) {
        const n00bb_static_graph_node_t *node = &nodes[i];
        size_t alignment = node->alignment;
        if (!node->bytes || !node->size || !alignment
            || (alignment & (alignment - 1))
            || total > SIZE_MAX - (alignment - 1)) {
            valid = false;
            break;
        }
        size_t start = (total + alignment - 1) & ~(alignment - 1);
        if (node->size > SIZE_MAX - start) {
            valid = false;
            break;
        }
        offsets[i] = start;
        total = start + node->size;
        if (alignment > section_alignment) section_alignment = alignment;
        if (node->symbol) {
            const char *name = native_symbol_name(target, node->symbol,
                &owned_names[symbol_count - 1]);
            if (!name) { valid = false; break; }
            symbols[symbol_count++] = (core_object_emit_symbol_t){
                .name = name,
                .section_index = 1,
                .section_offset = start,
                .exported = true,
            };
        }
        for (size_t a = 0; a < node->alias_count; a++) {
            if (!node->aliases || !node->aliases[a]) {
                valid = false;
                break;
            }
            const char *name = native_symbol_name(target, node->aliases[a],
                &owned_names[symbol_count - 1]);
            if (!name) { valid = false; break; }
            symbols[symbol_count++] = (core_object_emit_symbol_t){
                .name = name,
                .section_index = 1,
                .section_offset = start,
                .exported = true,
            };
        }
    }
    if (valid) {
        data = core_alloc_array(uint8_t, total);
        valid = data != nullptr;
    }
    if (valid) {
        memset(data, 0, total);
        for (size_t i = 0; i < node_count; i++)
            memcpy(data + offsets[i], nodes[i].bytes, nodes[i].size);
    }
    size_t relocation_count = 0;
    for (size_t i = 0; valid && i < edge_count; i++) {
        const n00bb_static_graph_edge_t *edge = &ordered_edges[i];
        if (i && edge->source == ordered_edges[i - 1].source
            && edge->source_offset == ordered_edges[i - 1].source_offset) {
            valid = edge->target == ordered_edges[i - 1].target
                && edge->target_offset == ordered_edges[i - 1].target_offset;
            continue;
        }
        if (edge->source >= node_count || edge->target >= node_count
            || edge->source_offset > nodes[edge->source].size
            || sizeof(uint64_t) > nodes[edge->source].size
                - edge->source_offset
            || edge->target_offset >= nodes[edge->target].size
            || offsets[edge->source] > SIZE_MAX - edge->source_offset
            || offsets[edge->target] > SIZE_MAX - edge->target_offset) {
            valid = false;
            break;
        }
        size_t source = offsets[edge->source] + edge->source_offset;
        memset(data + source, 0, sizeof(uint64_t));
        relocations[relocation_count++] = (core_object_emit_absolute64_t){
            .section_index = 1,
            .section_offset = source,
            .symbol_index = 0,
            .addend_bits = offsets[edge->target] + edge->target_offset,
        };
    }
    if (valid) {
        core_object_emit_section_t section = {
            .name = section_name,
            .segment = segment,
            .bytes = data,
            .size = total,
            .alignment = section_alignment,
            .writable = false,
        };
        valid = core_object_emit(target, &section, 1, symbols, symbol_count,
                                 relocations, relocation_count, object,
                                 object_size);
    }
    core_free(data);
    core_free(ordered_edges);
    core_free(relocations);
    for (size_t i = 0; owned_names && i < exported_count; i++)
        core_free(owned_names[i]);
    core_free(owned_names);
    core_free(symbols);
    core_free(offsets);
    return valid;
}
