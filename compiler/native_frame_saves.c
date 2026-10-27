#include "native_frame_saves.h"

#include "lib/byteorder.h"
#include "lib/object_section.h"

enum { N00BB_WIN64_UNWIND_CODES = 256 };

static bool
arm64_compact_saves(uint32_t encoding, n00bb_frame_saves_t *saves)
{
    if ((encoding & 0x0f000000u) != 0x04000000u) return false;
    int32_t slot = -8;
    for (uint32_t pair = 1; pair <= 0x10; pair <<= 1) {
        if (!(encoding & pair)) continue;
        saves->slots[saves->count++] = slot;
        saves->slots[saves->count++] = slot - 8;
        slot -= 16;
    }
    return true;
}

static bool
x86_64_compact_saves(uint32_t encoding, n00bb_frame_saves_t *saves)
{
    if ((encoding & 0x0f000000u) != 0x01000000u) return false;
    int32_t base = -8 * (int32_t)((encoding >> 16) & 0xff);
    for (uint32_t i = 0; i < 5; i++)
        if ((encoding >> (3 * i)) & 7)
            saves->slots[saves->count++] = base + 8 * (int32_t)i;
    return true;
}

static bool
relocated_function(const uint8_t *object, size_t object_bytes,
                   core_target_object_format_t format,
                   const core_object_section_t *section, size_t at,
                   uint32_t *home, uint64_t *offset)
{
    for (uint32_t i = 0; i < section->relocation_count; i++) {
        core_object_relocation_t relocation;
        if (core_object_relocation_at(section, format, i, &relocation)
            && relocation.section_offset == at)
            return core_object_relocation_target(object, object_bytes, format,
                section, &relocation, home, offset);
    }
    return false;
}

static bool
collect_compact(const uint8_t *object, size_t object_bytes,
                core_list_t(n00bb_frame_saves_t) *out)
{
    core_object_section_t section;
    if (!core_object_section_find(object, object_bytes,
            CORE_TARGET_OBJECT_MACH_O, CORE_STRING_STATIC("__LD"),
            CORE_STRING_STATIC("__compact_unwind"), &section)) return true;
    if (section.size % 32) return false;
    for (size_t at = 0; at < section.size; at += 32) {
        n00bb_frame_saves_t saves = {0};
        if (!relocated_function(object, object_bytes,
                CORE_TARGET_OBJECT_MACH_O, &section, at, &saves.section,
                &saves.offset)) continue;
        uint32_t encoding = core_le_load_u32(section.bytes + at + 12);
        bool known = section.machine == CORE_OBJECT_MACHINE_MACHO_ARM64
            ? arm64_compact_saves(encoding, &saves)
            : section.machine == CORE_OBJECT_MACHINE_MACHO_X86_64
                && x86_64_compact_saves(encoding, &saves);
        if (known && !core_list_try_push(*out, saves)) return false;
    }
    return true;
}

static size_t
encoded_bytes(uint8_t encoding)
{
    switch (encoding & 0x0f) {
    case 0x00: return 8;
    case 0x02: case 0x0a: return 2;
    case 0x03: case 0x0b: return 4;
    case 0x04: case 0x0c: return 8;
    default: return 0;
    }
}

typedef struct {
    int64_t data_align;
    size_t pointer_bytes;
    bool augmented;
    size_t instructions;
    size_t end;
} cie_t;

static bool
parse_cie(const uint8_t *bytes, size_t size, size_t start, cie_t *cie)
{
    if (start + 4 > size) return false;
    uint32_t length = core_le_load_u32(bytes + start);
    if (!length || length == 0xffffffffu || length > size - start - 4
        || length < 4 || core_le_load_u32(bytes + start + 4))
        return false;
    core_byte_cursor_t c = {.at = bytes + start + 8,
                            .end = bytes + start + 4 + length};
    uint8_t version = core_cursor_u8(&c);
    const uint8_t *augmentation = c.at;
    while (c.at < c.end && *c.at) c.at++;
    core_cursor_skip(&c, 1);
    core_cursor_uleb(&c);
    int64_t data_align = core_cursor_sleb(&c);
    if (version == 1) core_cursor_u8(&c);
    else core_cursor_uleb(&c);
    *cie = (cie_t){.data_align = data_align, .pointer_bytes = 8,
                   .end = (size_t)(c.end - bytes)};
    if (*augmentation == 'z') {
        uint64_t data_bytes = core_cursor_uleb(&c);
        if (c.failed || data_bytes > (uint64_t)(c.end - c.at)) return false;
        const uint8_t *data_end = c.at + data_bytes;
        for (const uint8_t *letter = augmentation + 1;
             letter < c.end && *letter; letter++) {
            if (*letter == 'R') {
                cie->pointer_bytes = encoded_bytes(core_cursor_u8(&c));
                if (!cie->pointer_bytes) return false;
            } else if (*letter == 'P') {
                size_t bytes_needed = encoded_bytes(core_cursor_u8(&c));
                if (!bytes_needed) return false;
                core_cursor_skip(&c, bytes_needed);
            } else if (*letter == 'L') core_cursor_u8(&c);
            else if (*letter != 'S' && *letter != 'B') return false;
        }
        c.at = data_end;
        cie->augmented = true;
    } else if (*augmentation) return false;
    cie->instructions = (size_t)(c.at - bytes);
    return !c.failed;
}

typedef struct {
    bool aarch64;
    int64_t data_align;
    uint16_t cfa_register;
    int64_t cfa_offset;
    bool frame_based;
    int64_t frame_offset;
    bool stopped;
    uint32_t saved_count;
    uint16_t saved_registers[N00BB_FRAME_SAVE_SLOTS];
    int64_t saved_offsets[N00BB_FRAME_SAVE_SLOTS];
} cfi_state_t;

static bool
callee_saved(const cfi_state_t *state, uint64_t reg)
{
    return state->aarch64 ? reg >= 19 && reg <= 28
                          : reg == 3 || (reg >= 12 && reg <= 15);
}

static void
note_cfa(cfi_state_t *state)
{
    if (state->cfa_register == (state->aarch64 ? 29 : 6)) {
        state->frame_based = true;
        state->frame_offset = state->cfa_offset;
    } else if (state->frame_based) state->stopped = true;
}

static void
note_saved(cfi_state_t *state, uint64_t reg, int64_t offset)
{
    if (!callee_saved(state, reg)) return;
    for (uint32_t i = 0; i < state->saved_count; i++)
        if (state->saved_registers[i] == reg) return;
    if (state->saved_count == N00BB_FRAME_SAVE_SLOTS) {
        state->stopped = true;
        return;
    }
    state->saved_registers[state->saved_count] = (uint16_t)reg;
    state->saved_offsets[state->saved_count++] = offset;
}

static bool
run_cfi(cfi_state_t *state, const uint8_t *bytes, size_t at, size_t end)
{
    core_byte_cursor_t c = {.at = bytes + at, .end = bytes + end};
    while (!state->stopped && !c.failed && c.at < c.end) {
        uint8_t op = core_cursor_u8(&c);
        switch (op >> 6) {
        case 1: continue;
        case 2:
            note_saved(state, op & 0x3f,
                       (int64_t)core_cursor_uleb(&c) * state->data_align);
            continue;
        case 3:
            state->stopped = true;
            continue;
        default: break;
        }
        uint64_t reg = 0;
        switch (op) {
        case 0x00: case 0x2d: break;
        case 0x02: core_cursor_skip(&c, 1); break;
        case 0x03: core_cursor_skip(&c, 2); break;
        case 0x04: core_cursor_skip(&c, 4); break;
        case 0x05:
            reg = core_cursor_uleb(&c);
            note_saved(state, reg,
                       (int64_t)core_cursor_uleb(&c) * state->data_align);
            break;
        case 0x06: case 0x0a: case 0x0b:
            state->stopped = true;
            break;
        case 0x07: case 0x08: case 0x2e:
            core_cursor_uleb(&c);
            break;
        case 0x0c:
            state->cfa_register = (uint16_t)core_cursor_uleb(&c);
            state->cfa_offset = (int64_t)core_cursor_uleb(&c);
            note_cfa(state);
            break;
        case 0x0d:
            state->cfa_register = (uint16_t)core_cursor_uleb(&c);
            note_cfa(state);
            break;
        case 0x0e:
            state->cfa_offset = (int64_t)core_cursor_uleb(&c);
            note_cfa(state);
            break;
        case 0x10: case 0x16:
            core_cursor_uleb(&c);
            core_cursor_skip(&c, core_cursor_uleb(&c));
            break;
        case 0x11:
            reg = core_cursor_uleb(&c);
            note_saved(state, reg, core_cursor_sleb(&c) * state->data_align);
            break;
        case 0x12:
            state->cfa_register = (uint16_t)core_cursor_uleb(&c);
            state->cfa_offset = core_cursor_sleb(&c) * state->data_align;
            note_cfa(state);
            break;
        case 0x13:
            state->cfa_offset = core_cursor_sleb(&c) * state->data_align;
            note_cfa(state);
            break;
        case 0x09: case 0x14:
            core_cursor_uleb(&c);
            core_cursor_uleb(&c);
            break;
        case 0x15:
            core_cursor_uleb(&c);
            core_cursor_sleb(&c);
            break;
        default:
            return false;
        }
    }
    return !c.failed;
}

static bool
collect_eh_frame(const uint8_t *object, size_t object_bytes,
                 core_list_t(n00bb_frame_saves_t) *out)
{
    core_object_section_t section;
    if (!core_object_section_find(object, object_bytes, CORE_TARGET_OBJECT_ELF,
            (core_string_t){}, CORE_STRING_STATIC(".eh_frame"), &section))
        return true;
    bool aarch64 = section.machine == CORE_OBJECT_MACHINE_ELF_AARCH64;
    if (!aarch64 && section.machine != CORE_OBJECT_MACHINE_ELF_X86_64) return true;
    const uint8_t *bytes = section.bytes;
    size_t size = section.size;
    for (size_t start = 0; start + 4 <= size;) {
        uint32_t length = core_le_load_u32(bytes + start);
        if (!length) break;
        if (length == 0xffffffffu || length > size - start - 4) return false;
        size_t end = start + 4 + length;
        uint32_t id = length >= 4 ? core_le_load_u32(bytes + start + 4) : 0;
        if (id && id <= start + 4) {
            cie_t cie;
            n00bb_frame_saves_t saves = {0};
            size_t field = start + 8;
            if (parse_cie(bytes, size, start + 4 - id, &cie)
                && field + 2 * cie.pointer_bytes <= end
                && relocated_function(object, object_bytes,
                    CORE_TARGET_OBJECT_ELF, &section, field, &saves.section,
                    &saves.offset)) {
                core_byte_cursor_t c = {
                    .at = bytes + field + 2 * cie.pointer_bytes,
                    .end = bytes + end,
                };
                if (cie.augmented) core_cursor_skip(&c, core_cursor_uleb(&c));
                bool body = !c.failed;
                cfi_state_t state = {
                    .aarch64 = aarch64,
                    .data_align = cie.data_align,
                };
                if (body && run_cfi(&state, bytes, cie.instructions, cie.end)
                    && run_cfi(&state, bytes, (size_t)(c.at - bytes), end)
                    && state.frame_based) {
                    for (uint32_t i = 0; i < state.saved_count; i++)
                        saves.slots[saves.count++] = (int32_t)(
                            state.frame_offset + state.saved_offsets[i]);
                    if (!core_list_try_push(*out, saves)) return false;
                }
            }
        }
        start = end;
    }
    return true;
}

typedef struct {
    uint32_t depth;
    int64_t frame;
    bool framed;
    bool record;
    uint32_t count;
    uint8_t registers[N00BB_FRAME_SAVE_SLOTS];
    int64_t depths[N00BB_FRAME_SAVE_SLOTS];
} win64_frame_t;

static bool
win64_nonvolatile(uint8_t reg)
{
    return reg == 3 || reg == 6 || reg == 7 || reg >= 12;
}

static bool
win64_push(win64_frame_t *frame, uint8_t reg)
{
    frame->depth += 8;
    if (reg == 5 && frame->depth == 16) frame->record = true;
    if (!win64_nonvolatile(reg)) return true;
    if (frame->count == N00BB_FRAME_SAVE_SLOTS) return false;
    frame->registers[frame->count] = reg;
    frame->depths[frame->count++] = frame->depth;
    return true;
}

static bool
win64_unwind(const uint8_t *info, size_t size, win64_frame_t *frame)
{
    if (size < 4 || (info[0] & 7) != 1 || (info[0] >> 3) & 4
        || (info[3] & 15) != 5) return false;
    uint32_t count = info[2];
    if (size < 4 + (size_t)count * 2) return false;
    uint32_t scaled = (uint32_t)(info[3] >> 4) * 16;
    uint32_t lengths[N00BB_WIN64_UNWIND_CODES];
    uint32_t starts[N00BB_WIN64_UNWIND_CODES];
    uint32_t ops = 0;
    for (uint32_t at = 0; at < count; ops++) {
        uint8_t op = info[4 + at * 2 + 1] & 15;
        uint8_t op_info = info[4 + at * 2 + 1] >> 4;
        uint32_t length = op == 1 ? (op_info ? 3 : 2)
            : op == 4 || op == 6 || op == 8 ? 2
            : op == 5 || op == 7 || op == 9 ? 3
            : op <= 3 || op == 10 ? 1 : 0;
        if (!length || at + length > count) return false;
        starts[ops] = at;
        lengths[ops] = length;
        at += length;
    }
    frame->depth = 8;
    int64_t saves[N00BB_FRAME_SAVE_SLOTS];
    uint8_t saved[N00BB_FRAME_SAVE_SLOTS];
    uint32_t save_count = 0;
    while (ops--) {
        const uint8_t *code = info + 4 + starts[ops] * 2;
        uint8_t op = code[1] & 15;
        uint8_t op_info = code[1] >> 4;
        switch (op) {
        case 0:
            if (!win64_push(frame, op_info)) return false;
            break;
        case 1:
            frame->depth += lengths[ops] == 2
                ? core_le_load_u16(code + 2) * 8u
                : core_le_load_u32(code + 2);
            break;
        case 2:
            frame->depth += op_info * 8u + 8;
            break;
        case 3:
            if (frame->framed) return false;
            frame->framed = true;
            frame->frame = -(int64_t)frame->depth + scaled;
            break;
        case 4:
        case 5:
            if (!win64_nonvolatile(op_info)) break;
            if (save_count == N00BB_FRAME_SAVE_SLOTS) return false;
            saved[save_count] = op_info;
            saves[save_count++] = op == 4 ? core_le_load_u16(code + 2) * 8
                                           : core_le_load_u32(code + 2);
            break;
        case 10:
            return false;
        default:
            break;
        }
    }
    for (uint32_t i = 0; i < save_count; i++) {
        if (frame->count == N00BB_FRAME_SAVE_SLOTS) return false;
        frame->registers[frame->count] = saved[i];
        frame->depths[frame->count++] = (int64_t)frame->depth - saves[i];
    }
    return frame->framed && frame->record;
}

static bool
collect_pdata(const uint8_t *object, size_t object_bytes,
              core_list_t(n00bb_frame_saves_t) *out)
{
    uint32_t sections = 0;
    if (!core_object_section_count(object, object_bytes,
            CORE_TARGET_OBJECT_COFF, &sections)) return true;
    for (uint32_t index = 1; index <= sections; index++) {
        core_object_section_t section;
        if (!core_object_section_at(object, object_bytes,
                CORE_TARGET_OBJECT_COFF, index, &section)
            || !core_string_eq(core_string_borrow((const char *)section.name,
                   section.name_length), CORE_STRING_STATIC(".pdata")))
            continue;
        if (section.machine != CORE_OBJECT_MACHINE_COFF_X86_64) return true;
        if (section.size % 12) return false;
        for (size_t at = 0; at < section.size; at += 12) {
            n00bb_frame_saves_t saves = {0};
            uint32_t info_section = 0;
            uint64_t info_offset = 0;
            core_object_section_t info;
            if (!relocated_function(object, object_bytes,
                    CORE_TARGET_OBJECT_COFF, &section, at, &saves.section,
                    &saves.offset)
                || !relocated_function(object, object_bytes,
                    CORE_TARGET_OBJECT_COFF, &section, at + 8, &info_section,
                    &info_offset)
                || !core_object_section_at(object, object_bytes,
                    CORE_TARGET_OBJECT_COFF, info_section, &info)
                || info_offset >= info.size) continue;
            win64_frame_t frame = {0};
            if (!win64_unwind(info.bytes + info_offset,
                              info.size - info_offset, &frame)) continue;
            saves.record = (int32_t)(-16 - frame.frame);
            for (uint32_t i = 0; i < frame.count; i++)
                saves.slots[saves.count++] = (int32_t)(-frame.depths[i]
                                                       - frame.frame);
            if (!core_list_try_push(*out, saves)) return false;
        }
    }
    return true;
}

bool
n00bb_frame_saves_collect(const void *object, size_t object_bytes,
                          core_target_object_format_t format,
                          core_list_t(n00bb_frame_saves_t) *out)
{
    if (!object || !out) return false;
    if (format == CORE_TARGET_OBJECT_MACH_O)
        return collect_compact(object, object_bytes, out);
    if (format == CORE_TARGET_OBJECT_ELF)
        return collect_eh_frame(object, object_bytes, out);
    if (format == CORE_TARGET_OBJECT_COFF)
        return collect_pdata(object, object_bytes, out);
    return true;
}

const n00bb_frame_saves_t *
n00bb_frame_saves_find(const core_list_t(n00bb_frame_saves_t) *list,
                       uint32_t section, uint64_t offset)
{
    for (size_t i = 0; list && i < list->len; i++)
        if (list->data[i].section == section
            && list->data[i].offset == offset) return &list->data[i];
    return nullptr;
}
