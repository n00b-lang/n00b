#include "compiler_baked.h"

#include <string.h>

#include "n00b-abi-v1.h"
#include "lib/alloc.h"
#include "lib/buffer.h"

static core_buffer_t baked_capture;

void
n00bb_set_baked_capture(const uint8_t *data, size_t size)
{
    core_buffer_release(&baked_capture);
    if (data && size)
        core_buffer_append(&baked_capture, (const char *)data, size);
}

bool
n00bb_has_baked_capture(void)
{
    return baked_capture.byte_len != 0;
}

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t offset;
} capture_reader_t;

static bool
read_word(capture_reader_t *reader, size_t bytes, uint64_t *out)
{
    if (reader->size - reader->offset < bytes) return false;
    uint64_t value = 0;
    for (size_t i = 0; i < bytes; i++)
        value |= (uint64_t)reader->data[reader->offset + i] << (8 * i);
    reader->offset += bytes;
    *out = value;
    return true;
}

static bool
read_bytes(capture_reader_t *reader, uint64_t size, const uint8_t **out)
{
    if (reader->size - reader->offset < size) return false;
    *out = reader->data + reader->offset;
    reader->offset += (size_t)size;
    return true;
}


static char *
read_string(capture_reader_t *reader)
{
    uint64_t length = 0;
    const uint8_t *bytes = nullptr;
    return read_word(reader, 8, &length) && read_bytes(reader, length, &bytes)
        ? core_strndup((const char *)bytes, (size_t)length) : nullptr;
}

static bool
read_type_name(capture_reader_t *reader, char **out)
{
    uint64_t length = 0;
    const uint8_t *bytes = nullptr;
    *out = nullptr;
    if (!read_word(reader, 8, &length) || !read_bytes(reader, length, &bytes))
        return false;
    if (!length) return true;
    *out = core_strndup((const char *)bytes, (size_t)length);
    return *out != nullptr;
}

static bool
read_slot(capture_reader_t *reader, uint64_t node_count,
          n00bb_static_graph_slot_t *slot)
{
    uint64_t node = 0, offset = 0, table = 0, kind = 0;
    if (!read_word(reader, 4, &node) || !read_word(reader, 8, &offset)
        || !read_word(reader, 4, &table) || !read_word(reader, 4, &kind)
        || node >= node_count) return false;
    *slot = (n00bb_static_graph_slot_t){
        .node = (size_t)node, .offset = (size_t)offset,
        .table = (uint32_t)table, .kind = (uint32_t)kind,
    };
    if (table == N00B_ABI_TABLE_TYPE)
        return read_word(reader, 8, &slot->typehash)
            && read_type_name(reader, &slot->name) && slot->name;
    if (table == N00B_ABI_TABLE_NATIVE_BINDING) {
        slot->name = read_string(reader);
        return slot->name && *slot->name;
    }
    slot->module = read_string(reader);
    slot->name = slot->module ? read_string(reader) : nullptr;
    if (table == N00B_ABI_TABLE_FUNCTION && slot->name) return true;
    core_free(slot->module);
    core_free(slot->name);
    return false;
}


static bool
read_root_identifier(capture_reader_t *reader, core_buffer_t *identifier)
{
    uint64_t module_length = 0, name_length = 0;
    const uint8_t *module = nullptr, *name = nullptr;
    if (!read_word(reader, 8, &module_length)
        || !read_bytes(reader, module_length, &module)
        || !read_word(reader, 8, &name_length)
        || !read_bytes(reader, name_length, &name)) return false;
    core_buffer_printf(identifier, "%.*s:%.*s", (int)module_length,
        (const char *)module, (int)name_length, (const char *)name);
    return identifier->data != nullptr;
}

static bool
read_failure(capture_reader_t *reader, n00bb_link_analysis_t *analysis)
{
    core_buffer_t root = {0}, path = {0};
    bool ok = read_root_identifier(reader, &root);
    uint64_t kind = 1;
    while (ok && (ok = read_word(reader, 4, &kind)) && kind) {
        uint64_t index = 0;
        char *name = kind == 1 ? read_string(reader) : nullptr;
        if (kind == 1 && name)
            core_buffer_printf(&path, "%s%s", path.byte_len ? "." : "", name);
        else if (kind == 2 && read_word(reader, 8, &index))
            core_buffer_printf(&path, "[%llu]", (unsigned long long)index);
        else ok = false;
        core_free(name);
    }
    char *message = ok ? read_string(reader) : nullptr;
    ok = ok && message && reader->offset == reader->size;
    if (ok) {
        analysis->baked_failure_root = core_strndup(root.data, root.byte_len);
        analysis->baked_failure_path = path.byte_len
            ? core_strndup(path.data, path.byte_len) : core_strndup("", 0);
        analysis->baked_failure_message = message;
    }
    else core_free(message);
    core_buffer_release(&root);
    core_buffer_release(&path);
    return ok;
}

bool
n00bb_baked_graph_load(n00bb_link_analysis_t *analysis)
{
    if (!baked_capture.byte_len) return true;
    capture_reader_t reader = {
        .data = (const uint8_t *)baked_capture.data,
        .size = baked_capture.byte_len,
    };
    uint64_t node_count = 0, edge_count = 0, root_count = 0;
    bool ok = read_word(&reader, 8, &node_count);
    if (ok && node_count == UINT64_MAX) return read_failure(&reader, analysis);
    for (uint64_t i = 0; ok && i < node_count; i++) {
        uint64_t size = 0, alignment = 0, typehash = 0, flags = 0;
        const uint8_t *bytes = nullptr;
        char *type_name = nullptr;
        ok = read_word(&reader, 8, &size) && read_word(&reader, 4, &alignment)
            && read_word(&reader, 8, &typehash)
            && read_type_name(&reader, &type_name)
            && read_word(&reader, 4, &flags)
            && read_bytes(&reader, size, &bytes);
        uint8_t *copy = ok && size ? core_alloc_array(uint8_t, size) : nullptr;
        if (ok && size && !copy) ok = false;
        if (ok && size) memcpy(copy, bytes, size);
        if (ok) core_list_push(analysis->static_graph_nodes,
            ((n00bb_captured_graph_node_t){
                .node = {
                    .bytes = copy,
                    .size = (size_t)size,
                    .alignment = (uint32_t)alignment,
                },
                .typehash = typehash,
                .type_name = type_name,
                .immutable = flags & N00B_ABI_BAKED_NODE_IMMUTABLE,
                .array_backing = flags & N00B_ABI_BAKED_NODE_ARRAY_BACKING,
                .foreign = flags & N00B_ABI_BAKED_NODE_FOREIGN,
            }));
    }
    ok = ok && read_word(&reader, 8, &edge_count);
    for (uint64_t i = 0; ok && i < edge_count; i++) {
        uint64_t source = 0, source_offset = 0, target = 0, target_offset = 0;
        ok = read_word(&reader, 4, &source)
            && read_word(&reader, 8, &source_offset)
            && read_word(&reader, 4, &target)
            && read_word(&reader, 8, &target_offset)
            && source < node_count && target < node_count;
        if (ok) core_list_push(analysis->static_graph_edges,
            ((n00bb_static_graph_edge_t){
                .source = (size_t)source,
                .source_offset = (size_t)source_offset,
                .target = (size_t)target,
                .target_offset = (size_t)target_offset,
            }));
    }
    ok = ok && read_word(&reader, 8, &root_count);
    for (uint64_t i = 0; ok && i < root_count; i++) {
        core_buffer_t identifier = {0};
        uint64_t node = 0;
        ok = read_root_identifier(&reader, &identifier)
            && read_word(&reader, 4, &node) && node < node_count;
        char *name = ok
            ? core_strndup(identifier.data, identifier.byte_len) : nullptr;
        core_buffer_release(&identifier);
        ok = ok && name;
        if (ok) core_list_push(analysis->baked_roots, ((n00bb_baked_root_t){
            .identifier = name,
            .root_index = (size_t)node,
        }));
    }
    uint64_t slot_count = 0;
    ok = ok && read_word(&reader, 8, &slot_count);
    for (uint64_t i = 0; ok && i < slot_count; i++) {
        n00bb_static_graph_slot_t slot;
        ok = read_slot(&reader, node_count, &slot);
        if (ok) core_list_push(analysis->static_graph_slots, slot);
    }
    return ok && reader.offset == reader.size;
}

const char *
n00bb_baked_root_identifier(const n00bb_link_analysis_t *analysis,
                            core_string_t module, core_string_t name)
{
    for (size_t i = 0; analysis && i < analysis->baked_roots.len; i++) {
        const char *identifier = analysis->baked_roots.data[i].identifier;
        size_t length = strlen(identifier);
        if (length == module.u8_bytes + 1 + name.u8_bytes
            && !memcmp(identifier, module.data, module.u8_bytes)
            && identifier[module.u8_bytes] == ':'
            && !memcmp(identifier + module.u8_bytes + 1, name.data,
                       name.u8_bytes))
            return identifier;
    }
    return nullptr;
}
