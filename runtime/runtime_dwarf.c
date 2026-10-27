#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "runtime_dwarf.h"
#include "builtin_os.h"
#include "lib/byteorder.h"

typedef struct {
    const uint8_t *line;
    uint64_t line_bytes;
    const uint8_t *info;
    uint64_t info_bytes;
    const uint8_t *abbrev;
    uint64_t abbrev_bytes;
    const uint8_t *str;
    uint64_t str_bytes;
    const uint8_t *ranges;
    uint64_t ranges_bytes;
    uintptr_t bias;
    bool loaded;
    bool available;
} debug_image_t;

static debug_image_t image;

#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
static bool same_name(const char *name, uint64_t limit, const char *wanted);

static void
note_section(const char *name, uint64_t limit, const uint8_t *bytes,
             uint64_t size)
{
    while (limit && (*name == '.' || *name == '_')) {
        name++;
        limit--;
    }
    if (same_name(name, limit, "debug_line")) {
        image.line = bytes;
        image.line_bytes = size;
    }
    else if (same_name(name, limit, "debug_info")) {
        image.info = bytes;
        image.info_bytes = size;
    }
    else if (same_name(name, limit, "debug_abbrev")) {
        image.abbrev = bytes;
        image.abbrev_bytes = size;
    }
    else if (same_name(name, limit, "debug_str")) {
        image.str = bytes;
        image.str_bytes = size;
    }
    else if (same_name(name, limit, "debug_ranges")) {
        image.ranges = bytes;
        image.ranges_bytes = size;
    }
}

#if defined(__linux__)
extern const uint8_t __ehdr_start[] __attribute__((weak));
#elif defined(__APPLE__)
#include <mach-o/loader.h>

extern const struct mach_header_64 _mh_execute_header;
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static bool
same_name(const char *name, uint64_t limit, const char *wanted)
{
    uint64_t i = 0;
    for (; i < limit && wanted[i]; i++)
        if (name[i] != wanted[i]) return false;
    return !wanted[i] && (i == limit || !name[i]);
}
#endif

#if defined(__linux__)
static bool
find_debug_line(const uint8_t *file, uint64_t length)
{
    if (length < 64 || core_le_load_u32(file) != 0x464c457fu || file[4] != 2)
        return false;
    uint64_t program_headers = core_le_load_u64(file + 32);
    uint64_t section_headers = core_le_load_u64(file + 40);
    uint16_t program_entry = core_le_load_u16(file + 54);
    uint16_t program_count = core_le_load_u16(file + 56);
    uint16_t section_entry = core_le_load_u16(file + 58);
    uint16_t section_count = core_le_load_u16(file + 60);
    uint16_t names_index = core_le_load_u16(file + 62);
    if (section_entry < 64 || names_index >= section_count
        || section_headers > length
        || (uint64_t)section_count * section_entry
            > length - section_headers) return false;
    uint64_t base_address = 0;
    if (program_entry >= 56 && program_headers <= length
        && (uint64_t)program_count * program_entry
            <= length - program_headers)
        for (uint16_t i = 0; i < program_count; i++) {
            const uint8_t *header = file + program_headers
                + (uint64_t)i * program_entry;
            if (core_le_load_u32(header) == 1 && core_le_load_u64(header + 8) == 0) {
                base_address = core_le_load_u64(header + 16);
                break;
            }
        }
    const uint8_t *names_header = file + section_headers
        + (uint64_t)names_index * section_entry;
    uint64_t names_offset = core_le_load_u64(names_header + 24);
    uint64_t names_size = core_le_load_u64(names_header + 32);
    if (names_offset > length || names_size > length - names_offset)
        return false;
    for (uint16_t i = 0; i < section_count; i++) {
        const uint8_t *header = file + section_headers
            + (uint64_t)i * section_entry;
        uint32_t name = core_le_load_u32(header);
        uint64_t offset = core_le_load_u64(header + 24);
        uint64_t size = core_le_load_u64(header + 32);
        if (name >= names_size || offset > length || size > length - offset)
            continue;
        note_section((const char *)file + names_offset + name,
                     names_size - name, file + offset, size);
    }
    image.bias = __ehdr_start
        ? (uintptr_t)__ehdr_start - (uintptr_t)base_address : 0;
    return image.line != NULL;
}

static void
load_image(void)
{
    const uint8_t *file = NULL;
    uint64_t length = 0;
    image.available = n00b_os_map_file("/proc/self/exe", &file, &length)
        && find_debug_line(file, length);
}
#elif defined(_WIN32)
static bool
find_debug_line(const uint8_t *file, uint64_t length)
{
    if (length < 64 || file[0] != 'M' || file[1] != 'Z') return false;
    uint32_t signature = core_le_load_u32(file + 60);
    if (signature > length || length - signature < 24
        || core_le_load_u32(file + signature) != 0x00004550u) return false;
    const uint8_t *coff = file + signature + 4;
    uint16_t section_count = core_le_load_u16(coff + 2);
    uint64_t symbols = core_le_load_u32(coff + 8);
    uint64_t symbol_count = core_le_load_u32(coff + 12);
    uint16_t optional_bytes = core_le_load_u16(coff + 16);
    const uint8_t *optional = coff + 20;
    uint64_t sections_at = (uint64_t)(optional - file) + optional_bytes;
    if (optional_bytes < 32 || sections_at > length
        || core_le_load_u16(optional) != 0x20b
        || (uint64_t)section_count * 40 > length - sections_at) return false;
    uint64_t image_base = core_le_load_u64(optional + 24);
    uint64_t strings = symbols + symbol_count * 18;
    for (uint16_t i = 0; i < section_count; i++) {
        const uint8_t *header = file + sections_at + (uint64_t)i * 40;
        const char *name = (const char *)header;
        uint64_t limit = 8;
        if (header[0] == '/') {
            uint64_t offset = 0;
            for (int at = 1; at < 8 && header[at] >= '0' && header[at] <= '9';
                 at++)
                offset = offset * 10 + (uint64_t)(header[at] - '0');
            if (!symbols || strings >= length || offset >= length - strings)
                continue;
            name = (const char *)file + strings + offset;
            limit = length - strings - offset;
        }
        uint64_t virtual_size = core_le_load_u32(header + 8);
        uint64_t raw_size = core_le_load_u32(header + 16);
        uint64_t raw = core_le_load_u32(header + 20);
        uint64_t bytes = virtual_size && virtual_size < raw_size
            ? virtual_size : raw_size;
        if (raw <= length && bytes <= length - raw)
            note_section(name, limit, file + raw, bytes);
    }
    if (!image.line) return false;
    image.bias = (uintptr_t)GetModuleHandleW(NULL) - (uintptr_t)image_base;
    return true;
}

static uint8_t executable_path[32768];

static void
load_image(void)
{
    if (!n00b_os_executable_path(executable_path, sizeof executable_path))
        return;
    const uint8_t *file = NULL;
    uint64_t length = 0;
    image.available = n00b_os_map_file((const char *)executable_path, &file,
                                       &length)
        && find_debug_line(file, length);
}
#elif defined(__APPLE__)
static bool
find_debug_line(const uint8_t *file, uint64_t length)
{
    if (length < sizeof(struct mach_header_64)
        || core_le_load_u32(file) != MH_MAGIC_64) return false;
    const struct mach_header_64 *header = (const void *)file;
    uint64_t at = sizeof(struct mach_header_64);
    uint64_t text_address = 0;
    bool text_found = false;
    for (uint32_t i = 0; i < header->ncmds; i++) {
        if (at > length || length - at < 8) return false;
        uint32_t command = core_le_load_u32(file + at);
        uint32_t size = core_le_load_u32(file + at + 4);
        if (size < 8 || size > length - at) return false;
        if (command == LC_SEGMENT_64
            && size >= sizeof(struct segment_command_64)) {
            const struct segment_command_64 *segment =
                (const void *)(file + at);
            if (same_name(segment->segname, 16, "__TEXT")) {
                text_address = segment->vmaddr;
                text_found = true;
            }
            const struct section_64 *sections =
                (const void *)(file + at + sizeof(*segment));
            uint64_t section_count = (size - sizeof(*segment))
                / sizeof(struct section_64);
            if (section_count > segment->nsects)
                section_count = segment->nsects;
            for (uint64_t s = 0; s < section_count; s++)
                if (sections[s].offset <= length
                    && sections[s].size <= length - sections[s].offset)
                    note_section(sections[s].sectname, 16,
                                 file + sections[s].offset, sections[s].size);
        }
        at += size;
    }
    if (!image.line || !text_found) return false;
    image.bias = (uintptr_t)&_mh_execute_header - (uintptr_t)text_address;
    return true;
}

static void
load_image(void)
{
    uint8_t executable[1024];
    size_t length = n00b_os_executable_path(executable, sizeof executable);
    if (!length) return;
    size_t base = length;
    while (base && executable[base - 1] != '/') base--;
    static const char middle[] = ".dSYM/Contents/Resources/DWARF/";
    char bundle[2048];
    size_t at = 0;
    if (length * 2 + sizeof middle >= sizeof bundle) return;
    for (size_t i = 0; i < length; i++) bundle[at++] = executable[i];
    for (size_t i = 0; middle[i]; i++) bundle[at++] = middle[i];
    for (size_t i = base; i < length; i++) bundle[at++] = executable[i];
    bundle[at] = 0;
    const uint8_t *file = NULL;
    uint64_t file_length = 0;
    image.available = n00b_os_map_file(bundle, &file, &file_length)
        && find_debug_line(file, file_length);
}
#else
static void
load_image(void)
{
}
#endif

static const char *
take_string(core_byte_cursor_t *c, uint64_t *length)
{
    const char *start = (const char *)c->at;
    while (c->at < c->end && *c->at) c->at++;
    if (c->at >= c->end) {
        c->failed = true;
        return NULL;
    }
    *length = (uint64_t)((const char *)c->at - start);
    c->at++;
    return start;
}

static bool
file_entry(core_byte_cursor_t directories, core_byte_cursor_t files, uint64_t index,
           n00bb_source_line_t *result)
{
    for (uint64_t current = 1; !files.failed; current++) {
        result->file = take_string(&files, &result->file_length);
        if (!result->file || !result->file_length) return false;
        uint64_t directory = core_cursor_uleb(&files);
        core_cursor_uleb(&files);
        core_cursor_uleb(&files);
        if (current != index) continue;
        result->directory = NULL;
        result->directory_length = 0;
        for (uint64_t d = 1; directory && d <= directory; d++) {
            result->directory = take_string(&directories,
                                            &result->directory_length);
            if (!result->directory || !result->directory_length) {
                result->directory = NULL;
                result->directory_length = 0;
                break;
            }
        }
        return !files.failed;
    }
    return false;
}

static bool
search_unit(core_byte_cursor_t unit, uint64_t target, n00bb_source_line_t *result)
{
    uint16_t version = (uint16_t)core_cursor_le(&unit, 2);
    if (version < 2 || version > 4) return false;
    uint64_t header_length = core_cursor_le(&unit, 4);
    if (unit.failed || header_length > (uint64_t)(unit.end - unit.at))
        return false;
    core_byte_cursor_t program = {unit.at + header_length, unit.end, false};
    uint8_t minimum_length = core_cursor_u8(&unit);
    if (version >= 4) core_cursor_u8(&unit);
    uint8_t default_is_stmt = core_cursor_u8(&unit);
    int8_t line_base = (int8_t)core_cursor_u8(&unit);
    uint8_t line_range = core_cursor_u8(&unit);
    uint8_t opcode_base = core_cursor_u8(&unit);
    if (unit.failed || !line_range || !opcode_base) return false;
    const uint8_t *standard_lengths = unit.at;
    unit.at += opcode_base - 1;
    core_byte_cursor_t directories = unit;
    for (uint64_t length = 0;;) {
        const char *directory = take_string(&unit, &length);
        if (!directory || !length) break;
    }
    if (unit.failed) return false;
    core_byte_cursor_t files = unit;
    uint64_t address = 0, file = 1, previous_address = 0, previous_file = 1;
    int64_t line = 1, previous_line = 1;
    uint64_t column = 0, previous_column = 0;
    bool have_previous = false;
    (void)default_is_stmt;
    while (!program.failed && program.at < program.end) {
        uint8_t opcode = core_cursor_u8(&program);
        bool emit = false, end_sequence = false;
        if (opcode >= opcode_base) {
            uint8_t adjusted = (uint8_t)(opcode - opcode_base);
            address += (uint64_t)(adjusted / line_range) * minimum_length;
            line += line_base + adjusted % line_range;
            emit = true;
        }
        else if (opcode == 0) {
            uint64_t length = core_cursor_uleb(&program);
            const uint8_t *next = program.at + length;
            if (length > (uint64_t)(program.end - program.at)) return false;
            uint8_t sub = length ? core_cursor_u8(&program) : 0;
            if (sub == 1) {
                emit = true;
                end_sequence = true;
            }
            else if (sub == 2) address = core_cursor_le(&program,
                (unsigned)(length - 1 > 8 ? 8 : length - 1));
            program.at = next;
        }
        else if (opcode == 1) emit = true;
        else if (opcode == 2)
            address += core_cursor_uleb(&program) * minimum_length;
        else if (opcode == 3) line += core_cursor_sleb(&program);
        else if (opcode == 4) file = core_cursor_uleb(&program);
        else if (opcode == 8)
            address += (uint64_t)((255 - opcode_base) / line_range)
                * minimum_length;
        else if (opcode == 9) address += core_cursor_le(&program, 2);
        else if (opcode == 5) column = core_cursor_uleb(&program);
        else if (opcode == 12) core_cursor_uleb(&program);
        else if (opcode != 6 && opcode != 7 && opcode != 10 && opcode != 11)
            for (uint8_t i = 0; i < standard_lengths[opcode - 1]; i++)
                core_cursor_uleb(&program);
        if (!emit) continue;
        if (have_previous && previous_address <= target && target < address
            && previous_line > 0) {
            result->line = (uint32_t)previous_line;
            result->column = (uint32_t)previous_column;
            return file_entry(directories, files, previous_file, result);
        }
        have_previous = !end_sequence;
        previous_address = address;
        previous_file = file;
        previous_line = line;
        previous_column = column;
        if (end_sequence) {
            address = 0;
            file = 1;
            line = 1;
            column = 0;
        }
    }
    return false;
}

bool
n00bb_runtime_source_line(uintptr_t address, n00bb_source_line_t *result)
{
    if (!image.loaded) {
        image.loaded = true;
        load_image();
    }
    if (!image.available || address < image.bias) return false;
    uint64_t target = address - image.bias;
    core_byte_cursor_t all = {image.line, image.line + image.line_bytes, false};
    while (!all.failed && all.at < all.end) {
        uint64_t length = core_cursor_le(&all, 4);
        if (length >= 0xfffffff0u) return false;
        if (all.failed || length > (uint64_t)(all.end - all.at)) return false;
        core_byte_cursor_t unit = {all.at, all.at + length, false};
        if (search_unit(unit, target, result)) return true;
        all.at += length;
    }
    return false;
}

enum {
    TAG_SUBPROGRAM = 0x2e,
    TAG_INLINED_SUBROUTINE = 0x1d,
    AT_NAME = 0x03,
    AT_STMT_LIST = 0x10,
    AT_LOW_PC = 0x11,
    AT_HIGH_PC = 0x12,
    AT_ABSTRACT_ORIGIN = 0x31,
    AT_SPECIFICATION = 0x47,
    AT_RANGES = 0x55,
    AT_CALL_COLUMN = 0x57,
    AT_CALL_FILE = 0x58,
    AT_CALL_LINE = 0x59,
    AT_LINKAGE_NAME = 0x6e,
    AT_MIPS_LINKAGE_NAME = 0x2007,
    FORM_ADDR = 0x01,
    FORM_BLOCK2 = 0x03,
    FORM_BLOCK4 = 0x04,
    FORM_DATA2 = 0x05,
    FORM_DATA4 = 0x06,
    FORM_DATA8 = 0x07,
    FORM_STRING = 0x08,
    FORM_BLOCK = 0x09,
    FORM_BLOCK1 = 0x0a,
    FORM_DATA1 = 0x0b,
    FORM_FLAG = 0x0c,
    FORM_SDATA = 0x0d,
    FORM_STRP = 0x0e,
    FORM_UDATA = 0x0f,
    FORM_REF_ADDR = 0x10,
    FORM_REF1 = 0x11,
    FORM_REF2 = 0x12,
    FORM_REF4 = 0x13,
    FORM_REF8 = 0x14,
    FORM_REF_UDATA = 0x15,
    FORM_INDIRECT = 0x16,
    FORM_SEC_OFFSET = 0x17,
    FORM_EXPRLOC = 0x18,
    FORM_FLAG_PRESENT = 0x19,
    FORM_REF_SIG8 = 0x20,
    MAX_DIE_DEPTH = 64,
};

typedef struct {
    uint64_t value;
    const char *text;
    uint64_t text_length;
    bool present;
    bool constant;
} attribute_value_t;

typedef struct {
    uint64_t tag;
    bool children;
    attribute_value_t name;
    attribute_value_t linkage_name;
    attribute_value_t origin;
    attribute_value_t low_pc;
    attribute_value_t high_pc;
    attribute_value_t ranges;
    attribute_value_t call_file;
    attribute_value_t call_line;
    attribute_value_t call_column;
    attribute_value_t stmt_list;
} die_t;

typedef struct {
    const uint8_t *start;
    const uint8_t *end;
    core_byte_cursor_t abbrevs;
    uint64_t base;
} unit_t;

static bool
find_abbrev(core_byte_cursor_t table, uint64_t code, uint64_t *tag, bool *children,
            core_byte_cursor_t *specs)
{
    while (!table.failed && table.at < table.end) {
        uint64_t current = core_cursor_uleb(&table);
        if (!current) return false;
        *tag = core_cursor_uleb(&table);
        *children = core_cursor_u8(&table) != 0;
        *specs = table;
        for (;;) {
            uint64_t attribute = core_cursor_uleb(&table);
            uint64_t form = core_cursor_uleb(&table);
            if (table.failed || (!attribute && !form)) break;
        }
        if (current == code) return !table.failed;
    }
    return false;
}

static bool
read_form(core_byte_cursor_t *c, uint64_t form, attribute_value_t *out)
{
    *out = (attribute_value_t){.present = true};
    switch (form) {
    case FORM_ADDR: out->value = core_cursor_le(c, 8); break;
    case FORM_DATA1: case FORM_REF1: case FORM_FLAG:
        out->value = core_cursor_le(c, 1);
        out->constant = true;
        break;
    case FORM_DATA2: case FORM_REF2:
        out->value = core_cursor_le(c, 2);
        out->constant = true;
        break;
    case FORM_DATA4: case FORM_REF4:
        out->value = core_cursor_le(c, 4);
        out->constant = true;
        break;
    case FORM_DATA8: case FORM_REF8: case FORM_REF_SIG8:
        out->value = core_cursor_le(c, 8);
        out->constant = true;
        break;
    case FORM_SDATA:
        out->value = (uint64_t)core_cursor_sleb(c);
        out->constant = true;
        break;
    case FORM_UDATA: case FORM_REF_UDATA:
        out->value = core_cursor_uleb(c);
        out->constant = true;
        break;
    case FORM_REF_ADDR: case FORM_SEC_OFFSET:
        out->value = core_cursor_le(c, 4);
        break;
    case FORM_STRP: {
        uint64_t offset = core_cursor_le(c, 4);
        if (offset >= image.str_bytes) return false;
        core_byte_cursor_t strings = {image.str + offset, image.str + image.str_bytes,
                            false};
        out->text = take_string(&strings, &out->text_length);
        break;
    }
    case FORM_STRING:
        out->text = take_string(c, &out->text_length);
        break;
    case FORM_FLAG_PRESENT: out->value = 1; break;
    case FORM_BLOCK1: c->at += core_cursor_u8(c); break;
    case FORM_BLOCK2: c->at += core_cursor_le(c, 2); break;
    case FORM_BLOCK4: c->at += core_cursor_le(c, 4); break;
    case FORM_BLOCK: case FORM_EXPRLOC: c->at += core_cursor_uleb(c); break;
    case FORM_INDIRECT: return read_form(c, core_cursor_uleb(c), out);
    default: return false;
    }
    if (c->at > c->end) c->failed = true;
    return !c->failed;
}

static bool
read_die(const unit_t *unit, core_byte_cursor_t *c, die_t *die, bool *null_entry)
{
    *die = (die_t){0};
    *null_entry = false;
    uint64_t code = core_cursor_uleb(c);
    if (c->failed) return false;
    if (!code) {
        *null_entry = true;
        return true;
    }
    core_byte_cursor_t specs = {0};
    if (!find_abbrev(unit->abbrevs, code, &die->tag, &die->children, &specs))
        return false;
    for (;;) {
        uint64_t attribute = core_cursor_uleb(&specs);
        uint64_t form = core_cursor_uleb(&specs);
        if (specs.failed) return false;
        if (!attribute && !form) return true;
        attribute_value_t value = {0};
        if (!read_form(c, form, &value)) return false;
        switch (attribute) {
        case AT_NAME: die->name = value; break;
        case AT_LINKAGE_NAME: case AT_MIPS_LINKAGE_NAME:
            die->linkage_name = value;
            break;
        case AT_ABSTRACT_ORIGIN: case AT_SPECIFICATION:
            die->origin = value;
            break;
        case AT_LOW_PC: die->low_pc = value; break;
        case AT_HIGH_PC: die->high_pc = value; break;
        case AT_RANGES: die->ranges = value; break;
        case AT_CALL_FILE: die->call_file = value; break;
        case AT_CALL_LINE: die->call_line = value; break;
        case AT_CALL_COLUMN: die->call_column = value; break;
        case AT_STMT_LIST: die->stmt_list = value; break;
        default: break;
        }
    }
}

static bool
die_contains(const unit_t *unit, const die_t *die, uint64_t target)
{
    if (die->low_pc.present && die->high_pc.present) {
        uint64_t high = die->high_pc.constant
            ? die->low_pc.value + die->high_pc.value : die->high_pc.value;
        return die->low_pc.value <= target && target < high;
    }
    if (!die->ranges.present || die->ranges.value >= image.ranges_bytes)
        return false;
    core_byte_cursor_t ranges = {image.ranges + die->ranges.value,
                       image.ranges + image.ranges_bytes, false};
    uint64_t base = unit->base;
    while (!ranges.failed) {
        uint64_t start = core_cursor_le(&ranges, 8);
        uint64_t end = core_cursor_le(&ranges, 8);
        if (ranges.failed || (!start && !end)) return false;
        if (start == UINT64_MAX) {
            base = end;
            continue;
        }
        if (base + start <= target && target < base + end) return true;
    }
    return false;
}

static void
die_name(const unit_t *unit, const die_t *die, const char **name,
         uint64_t *length)
{
    die_t current = *die;
    for (unsigned hops = 0; hops < 4; hops++) {
        if (current.linkage_name.text) {
            *name = current.linkage_name.text;
            *length = current.linkage_name.text_length;
            return;
        }
        if (!current.origin.present) break;
        core_byte_cursor_t at = {unit->start + current.origin.value, unit->end, false};
        bool null_entry = false;
        die_t origin = {0};
        if (at.at >= unit->end || !read_die(unit, &at, &origin, &null_entry)
            || null_entry) break;
        current = origin;
    }
    *name = current.name.text;
    *length = current.name.text_length;
}

static bool
unit_file(uint64_t stmt_list, uint64_t index, n00bb_source_line_t *result)
{
    if (stmt_list >= image.line_bytes) return false;
    core_byte_cursor_t all = {image.line + stmt_list, image.line + image.line_bytes,
                    false};
    uint64_t length = core_cursor_le(&all, 4);
    if (all.failed || length > (uint64_t)(all.end - all.at)) return false;
    core_byte_cursor_t unit = {all.at, all.at + length, false};
    uint16_t version = (uint16_t)core_cursor_le(&unit, 2);
    if (version < 2 || version > 4) return false;
    core_cursor_le(&unit, 4);
    core_cursor_u8(&unit);
    if (version >= 4) core_cursor_u8(&unit);
    core_cursor_u8(&unit);
    core_cursor_u8(&unit);
    core_cursor_u8(&unit);
    uint8_t opcode_base = core_cursor_u8(&unit);
    if (unit.failed || !opcode_base) return false;
    unit.at += opcode_base - 1;
    core_byte_cursor_t directories = unit;
    for (uint64_t skipped = 0;;) {
        const char *directory = take_string(&unit, &skipped);
        if (!directory || !skipped) break;
    }
    return !unit.failed && file_entry(directories, unit, index, result);
}

static size_t
unit_inline_frames(const unit_t *unit, core_byte_cursor_t c, uint64_t target,
                   n00bb_inline_frame_t *frames, size_t capacity)
{
    die_t chain[MAX_DIE_DEPTH];
    size_t chain_length = 0;
    size_t depth = 0;
    size_t matched_depth = 0;
    uint64_t stmt_list = 0;
    bool in_subprogram = false;
    while (!c.failed && c.at < unit->end) {
        die_t die = {0};
        bool null_entry = false;
        if (!read_die(unit, &c, &die, &null_entry)) return 0;
        if (null_entry) {
            if (!depth) break;
            depth--;
            if (in_subprogram && depth < matched_depth) break;
            continue;
        }
        if (die.stmt_list.present) stmt_list = die.stmt_list.value;
        bool candidate = (die.tag == TAG_SUBPROGRAM && !in_subprogram)
            || (die.tag == TAG_INLINED_SUBROUTINE && in_subprogram
                && depth == matched_depth);
        if (candidate && die_contains(unit, &die, target)
            && chain_length < MAX_DIE_DEPTH) {
            chain[chain_length++] = die;
            in_subprogram = true;
            matched_depth = depth + 1;
        }
        if (die.children) depth++;
    }
    if (chain_length < 2) return 0;
    size_t count = 0;
    for (size_t i = chain_length - 1; i >= 1 && count < capacity; i--) {
        n00bb_inline_frame_t *frame = &frames[count++];
        *frame = (n00bb_inline_frame_t){0};
        die_name(unit, &chain[i], &frame->name, &frame->name_length);
        frame->call.line = (uint32_t)chain[i].call_line.value;
        frame->call.column = (uint32_t)chain[i].call_column.value;
        if (chain[i].call_file.present)
            unit_file(stmt_list, chain[i].call_file.value, &frame->call);
    }
    return count;
}

size_t
n00bb_runtime_inline_frames(uintptr_t address, n00bb_inline_frame_t *frames,
                            size_t capacity)
{
    if (!image.loaded) {
        image.loaded = true;
        load_image();
    }
    if (!image.available || !image.info || !image.abbrev
        || address < image.bias || !capacity) return 0;
    uint64_t target = address - image.bias;
    core_byte_cursor_t all = {image.info, image.info + image.info_bytes, false};
    while (!all.failed && all.at < all.end) {
        const uint8_t *start = all.at;
        uint64_t length = core_cursor_le(&all, 4);
        if (all.failed || length >= 0xfffffff0u
            || length > (uint64_t)(all.end - all.at)) return 0;
        core_byte_cursor_t c = {all.at, all.at + length, false};
        all.at += length;
        uint16_t version = (uint16_t)core_cursor_le(&c, 2);
        uint64_t abbrev_offset = core_cursor_le(&c, 4);
        uint8_t address_size = core_cursor_u8(&c);
        if (c.failed || version < 2 || version > 4 || address_size != 8
            || abbrev_offset >= image.abbrev_bytes) continue;
        unit_t unit = {
            .start = start, .end = c.end,
            .abbrevs = {image.abbrev + abbrev_offset,
                        image.abbrev + image.abbrev_bytes, false},
        };
        core_byte_cursor_t probe = c;
        die_t root = {0};
        bool null_entry = false;
        if (!read_die(&unit, &probe, &root, &null_entry) || null_entry)
            continue;
        unit.base = root.low_pc.present ? root.low_pc.value : 0;
        if ((root.low_pc.present || root.ranges.present)
            && !die_contains(&unit, &root, target)) continue;
        size_t count = unit_inline_frames(&unit, c, target, frames, capacity);
        if (count) return count;
    }
    return 0;
}
