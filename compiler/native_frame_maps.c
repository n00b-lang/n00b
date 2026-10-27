#include "native_frame_maps.h"

#include <limits.h>
#include <string.h>

#include "llvm_stack_maps.h"
#include "native_frame_saves.h"
#include "lib/alloc.h"
#include "lib/byteorder.h"
#include "lib/dict.h"
#include "lib/list.h"
#include "lib/object_emit.h"

typedef struct {
    int32_t offset;
    uint32_t bytes;
} native_root_t;
core_list_decl(native_root_t);

typedef struct {
    core_string_t *function;
    uint64_t code_bytes;
    int32_t record;
    bool recorded;
    bool invalid;
    core_list_t(native_root_t) roots;
} native_frame_t;
core_list_decl(native_frame_t);

struct n00bb_native_frames_t {
    core_target_object_format_t format;
    core_list_t(native_frame_t) functions;
    core_dict_t names;
};

typedef struct {
    n00bb_native_frames_t *frames;
    const core_list_t(n00bb_frame_saves_t) *saves;
    size_t current;
} frame_visit_t;

static void
add_root(native_frame_t *frame, int64_t offset, uint64_t bytes)
{
    int64_t start = offset;
    int64_t end = offset + (int64_t)bytes;
    if ((offset & 7) || (bytes & 7) || !bytes || (end > frame->record && start < (int64_t)frame->record + 16)
        || start < INT32_MIN || end - start > UINT32_MAX) {
        frame->invalid = true;
        return;
    }
    for (size_t i = 0; i < frame->roots.len; i++) {
        native_root_t *root = &frame->roots.data[i];
        if (root->offset != start) continue;
        if (root->bytes < end - start) root->bytes = (uint32_t)(end - start);
        return;
    }
    core_list_push(frame->roots, ((native_root_t){
        .offset = (int32_t)start, .bytes = (uint32_t)(end - start),
    }));
}

static bool
add_function(void *opaque, const n00bb_llvm_stack_function_t *function)
{
    frame_visit_t *visit = opaque;
    n00bb_native_frames_t *frames = visit->frames;
    core_string_t name = core_string_borrow(
        (const char *)function->name.bytes, function->name.length);
    if (!name.u8_bytes || (frames->format == CORE_TARGET_OBJECT_MACH_O
                           && name.data[0] != '_')) return false;
    bool found = false;
    uintptr_t index = (uintptr_t)core_dict_get(&frames->names, &name, &found);
    if (found) {
        visit->current = index;
        return true;
    }
    native_frame_t next = {
        .function = core_dict_string_key_copy(name),
        .code_bytes = function->code_bytes,
        .roots = core_list_new(native_root_t),
    };
    if (!next.function || !next.function->data || !next.roots.data) {
        if (next.function) core_dict_string_key_free(next.function);
        core_list_free(next.roots);
        return false;
    }
    const n00bb_frame_saves_t *saves = n00bb_frame_saves_find(visit->saves,
        function->section, function->offset);
    next.recorded = saves != nullptr;
    next.record = saves ? saves->record : 0;
    for (uint32_t i = 0; saves && i < saves->count; i++)
        add_root(&next, saves->slots[i], 8);
    visit->current = frames->functions.len;
    core_list_push(frames->functions, next);
    core_dict_put(&frames->names, next.function, visit->current);
    return true;
}

static bool
add_location(void *opaque, const n00bb_llvm_stack_location_t *location)
{
    frame_visit_t *visit = opaque;
    native_frame_t *frame = &visit->frames->functions.data[visit->current];
    if (location->id == N00BB_LLVM_FRAME_CONSERVATIVE_ID
        || location->kind != N00BB_LLVM_STACKMAP_DIRECT
        || (location->dwarf_register != 29 && location->dwarf_register != 6)
        || location->size_bytes != 8) frame->invalid = true;
    else add_root(frame, location->frame_offset, 8);
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
    core_dict_init_string(&frames->names);
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
    if (!frames) return false;
    core_list_t(n00bb_frame_saves_t) saves = core_list_new(n00bb_frame_saves_t);
    frame_visit_t visit = {.frames = frames, .saves = &saves};
    n00bb_llvm_stack_visitor_t visitor = {
        .function = add_function, .location = add_location,
        .context = &visit,
    };
    bool found = false;
    bool ok = n00bb_frame_saves_collect(object, object_bytes, frames->format,
                                        &saves)
        && n00bb_llvm_stack_maps_visit(object, object_bytes, frames->format,
                                       &visitor, &found);
    core_list_free(saves);
    return ok;
}

enum { frame_row_bytes = 32, frame_row_whole = 1 };

static bool
frame_emitted(const n00bb_native_frames_t *frames,
              const native_frame_t *frame)
{
    return frame->recorded && frame->code_bytes <= UINT32_MAX
        && (!frame->invalid || frames->format == CORE_TARGET_OBJECT_COFF);
}

static size_t
frame_root_total(const native_frame_t *frame)
{
    return frame->invalid ? 0 : frame->roots.len;
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
        if (!frame_emitted(frames, frame)) continue;
        if (frame_root_total(frame) > UINT32_MAX - root_count
            || count >= UINT32_MAX / 2) return false;
        count++;
        root_count += frame_root_total(frame);
    }
    if (count > (SIZE_MAX - 8) / frame_row_bytes
        || root_count > (SIZE_MAX - 8 - count * frame_row_bytes) / 8)
        return false;
    size_t bytes = 8 + count * frame_row_bytes + root_count * 8;
    uint8_t *data = core_alloc_size(bytes, 1);
    core_object_emit_symbol_t *symbols = core_alloc_array(
        core_object_emit_symbol_t, count + 1);
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
        if (!frame_emitted(frames, frame)) continue;
        size_t at = 8 + row * frame_row_bytes;
        core_le_store_u32(data + at + 8, (uint32_t)frame->code_bytes);
        size_t roots = frame_root_total(frame);
        core_le_store_u32(data + at + 12, (uint32_t)roots);
        core_le_store_u32(data + at + 16, (uint32_t)root);
        core_le_store_u32(data + at + 20, (uint32_t)frame->record);
        core_le_store_u32(data + at + 24,
                          frame->invalid ? frame_row_whole : 0);
        for (size_t j = 0; j < roots; j++) {
            uint8_t *entry = data + 8 + count * frame_row_bytes
                + (root + j) * 8;
            core_le_store_u32(entry, (uint32_t)frame->roots.data[j].offset);
            core_le_store_u32(entry + 4, frame->roots.data[j].bytes);
        }
        symbols[row] = (core_object_emit_symbol_t){
            .name = *frame->function, .section_index = 0,
            .exported = true,
        };
        relocations[row] = (core_object_emit_absolute64_t){
            .section_index = 1, .section_offset = at,
            .symbol_index = (uint32_t)row,
            .symbol_relocation = frames->format == CORE_TARGET_OBJECT_MACH_O,
        };
        root += roots;
        row++;
    }
    symbols[count] = (core_object_emit_symbol_t){
        .name = frames->format == CORE_TARGET_OBJECT_MACH_O
            ? CORE_STRING_STATIC("___n00bb_gc_native_frames")
            : CORE_STRING_STATIC("__n00bb_gc_native_frames"),
        .section_index = 1, .size = bytes, .exported = true,
    };
    const core_object_emit_section_t section = {
        .name = frames->format == CORE_TARGET_OBJECT_MACH_O
            ? CORE_STRING_STATIC("__n00b_frames")
            : frames->format == CORE_TARGET_OBJECT_COFF
                ? CORE_STRING_STATIC(".n00b$F")
                : CORE_STRING_STATIC(".data.rel.ro.n00b_frames"),
        .segment = frames->format == CORE_TARGET_OBJECT_MACH_O
            ? CORE_STRING_STATIC("__DATA_CONST") : (core_string_t){},
        .bytes = data, .size = bytes, .alignment = 8,
    };
    bool ok = core_object_emit(target, &section, 1, symbols,
        count + 1, relocations, count, image, image_bytes);
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
        core_dict_string_key_free(frame->function);
        core_list_free(frame->roots);
    }
    core_list_free(frames->functions);
    core_dict_free(&frames->names);
    core_free(frames);
}
