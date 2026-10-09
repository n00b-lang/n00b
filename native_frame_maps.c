#include "native_frame_maps.h"

#include <limits.h>
#include <string.h>

#include "llvm_stack_maps.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/byteorder.h"
#include "lib/list.h"
#include "lib/object_emit.h"

typedef struct {
    char *function;
    char *descriptor;
    bool has_sentinel;
    bool invalid;
    int32_t sentinel_offset;
    core_list_t(int32_t) roots;
} native_frame_t;
core_list_decl(native_frame_t);

struct n00bb_native_frames_t {
    core_target_object_format_t format;
    core_list_t(native_frame_t) functions;
};

static bool
add_location(void *opaque, const n00bb_llvm_stack_location_t *location)
{
    n00bb_native_frames_t *frames = opaque;
    native_frame_t *frame = nullptr;
    for (size_t i = 0; i < frames->functions.len; i++) {
        native_frame_t *candidate = &frames->functions.data[i];
        if (strlen(candidate->function) == location->function.length
            && !memcmp(candidate->function, location->function.bytes,
                       location->function.length)) {
            frame = candidate;
            break;
        }
    }
    if (!frame) {
        const uint8_t *name = location->function.bytes;
        size_t length = location->function.length;
        if (!length || (frames->format == CORE_TARGET_OBJECT_MACH_O
                        && name[0] != '_')) return false;
        core_buffer_t descriptor = {0};
        if (frames->format == CORE_TARGET_OBJECT_MACH_O)
            core_buffer_append(&descriptor, "_", 1);
        core_buffer_append(&descriptor, "__n00bb_gc_frame.",
                           sizeof("__n00bb_gc_frame.") - 1);
        core_buffer_append(&descriptor, (const char *)name
            + (frames->format == CORE_TARGET_OBJECT_MACH_O),
            length - (frames->format == CORE_TARGET_OBJECT_MACH_O));
        native_frame_t next = {
            .function = core_strndup((const char *)name, length),
            .descriptor = core_strdup(descriptor.data),
            .roots = core_list_new(int32_t),
        };
        core_buffer_release(&descriptor);
        if (!next.function || !next.descriptor || !next.roots.data) {
            core_free(next.function);
            core_free(next.descriptor);
            core_list_free(next.roots);
            return false;
        }
        core_list_push(frames->functions, next);
        frame = &frames->functions.data[frames->functions.len - 1];
    }
    bool mappable = location->kind == N00BB_LLVM_STACKMAP_DIRECT
        && location->size_bytes == 8
        && location->frame_offset < 0 && !(location->frame_offset & 7)
        && (location->dwarf_register == 6
            || location->dwarf_register == 29);
    if (location->id == N00BB_LLVM_FRAME_SENTINEL_ID) {
        if (frame->has_sentinel) return false;
        frame->has_sentinel = true;
        frame->sentinel_offset = location->frame_offset;
    }
    if (!mappable) {
        frame->invalid = true;
        return true;
    }
    if (location->id != N00BB_LLVM_FRAME_SENTINEL_ID) {
        for (size_t i = 0; i < frame->roots.len; i++)
            if (frame->roots.data[i] == location->frame_offset) return true;
        core_list_push(frame->roots, location->frame_offset);
    }
    return true;
}

n00bb_native_frames_t *
n00bb_native_frames_new(core_target_object_format_t format)
{
    if (format != CORE_TARGET_OBJECT_MACH_O
        && format != CORE_TARGET_OBJECT_ELF
        && format != CORE_TARGET_OBJECT_COFF) return nullptr;
    n00bb_native_frames_t *frames = core_alloc(n00bb_native_frames_t);
    if (!frames) return nullptr;
    *frames = (n00bb_native_frames_t){
        .format = format,
        .functions = core_list_new(native_frame_t),
    };
    if (!frames->functions.data) {
        core_free(frames);
        return nullptr;
    }
    return frames;
}

bool
n00bb_native_frames_add(n00bb_native_frames_t *frames,
                        const void *object, size_t object_bytes)
{
    bool found = false;
    return frames && n00bb_llvm_stack_maps_visit(object, object_bytes,
        frames->format, add_location, frames, &found);
}

bool
n00bb_native_frames_emit(const n00bb_native_frames_t *frames,
                         const core_target_desc_t *target,
                         uint8_t **image, size_t *image_bytes)
{
    if (!frames || !target || !image || !image_bytes
        || frames->format != target->object_format
        || target->endian != CORE_TARGET_LITTLE_ENDIAN
        || target->pointer_bits != 64) return false;
    size_t count = 0, root_count = 0;
    for (size_t i = 0; i < frames->functions.len; i++) {
        const native_frame_t *frame = &frames->functions.data[i];
        if (!frame->has_sentinel) continue;
        size_t roots = frame->invalid ? 0 : frame->roots.len;
        if ((!frame->invalid && !roots)
            || roots > UINT32_MAX - root_count
            || count >= UINT32_MAX / 2) return false;
        count++;
        root_count += roots;
    }
    if (count > (SIZE_MAX - 8) / 24
        || root_count > (SIZE_MAX - 8 - count * 24) / 4) return false;
    size_t bytes = 8 + count * 24 + root_count * 4;
    uint8_t *data = core_alloc_size(bytes, 1);
    core_object_emit_symbol_t *symbols = core_alloc_array(
        core_object_emit_symbol_t, count * 2 + 1);
    core_object_emit_absolute64_t *relocations = count
        ? core_alloc_array(core_object_emit_absolute64_t, count) : nullptr;
    if (!data || !symbols || (count && !relocations)) {
        core_free(data);
        core_free(symbols);
        core_free(relocations);
        return false;
    }
    memset(data, 0, bytes);
    core_le_store_u32(data, (uint32_t)count);
    core_le_store_u32(data + 4, (uint32_t)root_count);
    size_t row = 0, root = 0;
    for (size_t i = 0; i < frames->functions.len; i++) {
        const native_frame_t *frame = &frames->functions.data[i];
        if (!frame->has_sentinel) continue;
        size_t roots = frame->invalid ? 0 : frame->roots.len;
        size_t at = 8 + row * 24;
        core_le_store_u32(data + at + 8,
                          (uint32_t)frame->sentinel_offset);
        core_le_store_u32(data + at + 12, (uint32_t)roots);
        core_le_store_u32(data + at + 16, (uint32_t)root);
        for (size_t j = 0; j < roots; j++)
            core_le_store_u32(data + 8 + count * 24 + (root + j) * 4,
                              (uint32_t)frame->roots.data[j]);
        symbols[row] = (core_object_emit_symbol_t){
            .name = frame->function, .section_index = 0,
            .exported = true,
        };
        symbols[count + row] = (core_object_emit_symbol_t){
            .name = frame->descriptor, .section_index = 1,
            .section_offset = at, .exported = true,
        };
        relocations[row] = (core_object_emit_absolute64_t){
            .section_index = 1, .section_offset = at,
            .symbol_index = (uint32_t)row,
            .symbol_relocation = frames->format == CORE_TARGET_OBJECT_MACH_O,
        };
        root += roots;
        row++;
    }
    symbols[count * 2] = (core_object_emit_symbol_t){
        .name = frames->format == CORE_TARGET_OBJECT_MACH_O
            ? "___n00bb_gc_native_frames" : "__n00bb_gc_native_frames",
        .section_index = 1, .exported = true,
    };
    const core_object_emit_section_t section = {
        .name = frames->format == CORE_TARGET_OBJECT_MACH_O
            ? "__n00b_frames" : frames->format == CORE_TARGET_OBJECT_COFF
                ? ".n00b$F" : ".data.rel.ro.n00b_frames",
        .segment = frames->format == CORE_TARGET_OBJECT_MACH_O
            ? "__DATA_CONST" : nullptr,
        .bytes = data, .size = bytes, .alignment = 8,
    };
    bool ok = core_object_emit(target, &section, 1, symbols,
        count * 2 + 1, relocations, count, image, image_bytes);
    core_free(data);
    core_free(symbols);
    core_free(relocations);
    return ok;
}

void
n00bb_native_frames_free(n00bb_native_frames_t *frames)
{
    if (!frames) return;
    for (size_t i = 0; i < frames->functions.len; i++) {
        native_frame_t *frame = &frames->functions.data[i];
        core_free(frame->function);
        core_free(frame->descriptor);
        core_list_free(frame->roots);
    }
    core_list_free(frames->functions);
    core_free(frames);
}
