#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "runtime_dwarf.h"

typedef struct {
    const uint8_t *line;
    uint64_t line_bytes;
    uintptr_t bias;
    bool loaded;
    bool available;
} debug_image_t;

static debug_image_t image;

#if defined(__linux__)
#include "builtin_os_syscall.h"

extern const uint8_t __ehdr_start[] __attribute__((weak));

static bool
map_file(const char *path, const uint8_t **bytes, uint64_t *length)
{
    long fd = n00b_linux_syscall6(__NR_openat, -100, (long)path, 0, 0, 0, 0);
    if (fd < 0) return false;
    long size = n00b_linux_syscall6(__NR_lseek, fd, 0, 2, 0, 0, 0);
    long mapped = size > 0
        ? n00b_linux_syscall6(__NR_mmap, 0, size, 1, 2, fd, 0) : -1;
    n00b_linux_syscall6(__NR_close, fd, 0, 0, 0, 0, 0);
    if (mapped < 0 && mapped > -4096) return false;
    if (size <= 0) return false;
    *bytes = (const uint8_t *)mapped;
    *length = (uint64_t)size;
    return true;
}
#elif defined(__APPLE__)
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <sys/mman.h>
#include <unistd.h>

extern const struct mach_header_64 _mh_execute_header;

static bool
map_file(const char *path, const uint8_t **bytes, uint64_t *length)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    off_t size = lseek(fd, 0, SEEK_END);
    void *mapped = size > 0
        ? mmap(NULL, (size_t)size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    close(fd);
    if (mapped == MAP_FAILED) return false;
    *bytes = mapped;
    *length = (uint64_t)size;
    return true;
}
#endif

static uint32_t
read_u32(const uint8_t *at)
{
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16)
        | ((uint32_t)at[3] << 24);
}

#if defined(__linux__)
static uint16_t
read_u16(const uint8_t *at)
{
    return (uint16_t)(at[0] | (at[1] << 8));
}

static uint64_t
read_u64(const uint8_t *at)
{
    return (uint64_t)read_u32(at) | ((uint64_t)read_u32(at + 4) << 32);
}
#endif

static bool
same_name(const char *name, uint64_t limit, const char *wanted)
{
    uint64_t i = 0;
    for (; i < limit && wanted[i]; i++)
        if (name[i] != wanted[i]) return false;
    return !wanted[i] && (i == limit || !name[i]);
}

#if defined(__linux__)
static bool
find_debug_line(const uint8_t *file, uint64_t length)
{
    if (length < 64 || read_u32(file) != 0x464c457fu || file[4] != 2)
        return false;
    uint64_t program_headers = read_u64(file + 32);
    uint64_t section_headers = read_u64(file + 40);
    uint16_t program_entry = read_u16(file + 54);
    uint16_t program_count = read_u16(file + 56);
    uint16_t section_entry = read_u16(file + 58);
    uint16_t section_count = read_u16(file + 60);
    uint16_t names_index = read_u16(file + 62);
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
            if (read_u32(header) == 1 && read_u64(header + 8) == 0) {
                base_address = read_u64(header + 16);
                break;
            }
        }
    const uint8_t *names_header = file + section_headers
        + (uint64_t)names_index * section_entry;
    uint64_t names_offset = read_u64(names_header + 24);
    uint64_t names_size = read_u64(names_header + 32);
    if (names_offset > length || names_size > length - names_offset)
        return false;
    for (uint16_t i = 0; i < section_count; i++) {
        const uint8_t *header = file + section_headers
            + (uint64_t)i * section_entry;
        uint32_t name = read_u32(header);
        uint64_t offset = read_u64(header + 24);
        uint64_t size = read_u64(header + 32);
        if (name >= names_size || offset > length || size > length - offset)
            continue;
        if (same_name((const char *)file + names_offset + name,
                      names_size - name, ".debug_line")) {
            image.line = file + offset;
            image.line_bytes = size;
            image.bias = __ehdr_start
                ? (uintptr_t)__ehdr_start - (uintptr_t)base_address : 0;
            return true;
        }
    }
    return false;
}

static void
load_image(void)
{
    const uint8_t *file = NULL;
    uint64_t length = 0;
    image.available = map_file("/proc/self/exe", &file, &length)
        && find_debug_line(file, length);
}
#elif defined(__APPLE__)
static bool
find_debug_line(const uint8_t *file, uint64_t length)
{
    if (length < sizeof(struct mach_header_64)
        || read_u32(file) != MH_MAGIC_64) return false;
    const struct mach_header_64 *header = (const void *)file;
    uint64_t at = sizeof(struct mach_header_64);
    uint64_t text_address = 0;
    bool text_found = false;
    for (uint32_t i = 0; i < header->ncmds; i++) {
        if (at > length || length - at < 8) return false;
        uint32_t command = read_u32(file + at);
        uint32_t size = read_u32(file + at + 4);
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
                if (same_name(sections[s].sectname, 16, "__debug_line")
                    && sections[s].offset <= length
                    && sections[s].size <= length - sections[s].offset) {
                    image.line = file + sections[s].offset;
                    image.line_bytes = sections[s].size;
                }
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
    char executable[1024];
    uint32_t size = sizeof executable;
    if (_NSGetExecutablePath(executable, &size)) return;
    size_t length = 0;
    while (executable[length]) length++;
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
    image.available = map_file(bundle, &file, &file_length)
        && find_debug_line(file, file_length);
}
#else
static void
load_image(void)
{
}
#endif

typedef struct {
    const uint8_t *at;
    const uint8_t *end;
    bool failed;
} cursor_t;

static uint8_t
take_u8(cursor_t *c)
{
    if (c->at >= c->end) {
        c->failed = true;
        return 0;
    }
    return *c->at++;
}

static uint64_t
take_bytes(cursor_t *c, unsigned count)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < count; i++)
        value |= (uint64_t)take_u8(c) << (8 * i);
    return value;
}

static uint64_t
take_uleb(cursor_t *c)
{
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        uint8_t byte = take_u8(c);
        value |= (uint64_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) return value;
    }
    c->failed = true;
    return value;
}

static int64_t
take_sleb(cursor_t *c)
{
    int64_t value = 0;
    unsigned shift = 0;
    uint8_t byte = 0;
    do {
        byte = take_u8(c);
        value |= (int64_t)(byte & 0x7f) << shift;
        shift += 7;
    } while ((byte & 0x80) && shift < 64);
    if (shift < 64 && (byte & 0x40)) value |= -((int64_t)1 << shift);
    return value;
}

static const char *
take_string(cursor_t *c, uint64_t *length)
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
file_entry(cursor_t directories, cursor_t files, uint64_t index,
           n00bb_source_line_t *result)
{
    for (uint64_t current = 1; !files.failed; current++) {
        result->file = take_string(&files, &result->file_length);
        if (!result->file || !result->file_length) return false;
        uint64_t directory = take_uleb(&files);
        take_uleb(&files);
        take_uleb(&files);
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
search_unit(cursor_t unit, uint64_t target, n00bb_source_line_t *result)
{
    uint16_t version = (uint16_t)take_bytes(&unit, 2);
    if (version < 2 || version > 4) return false;
    uint64_t header_length = take_bytes(&unit, 4);
    if (unit.failed || header_length > (uint64_t)(unit.end - unit.at))
        return false;
    cursor_t program = {unit.at + header_length, unit.end, false};
    uint8_t minimum_length = take_u8(&unit);
    if (version >= 4) take_u8(&unit);
    uint8_t default_is_stmt = take_u8(&unit);
    int8_t line_base = (int8_t)take_u8(&unit);
    uint8_t line_range = take_u8(&unit);
    uint8_t opcode_base = take_u8(&unit);
    if (unit.failed || !line_range || !opcode_base) return false;
    const uint8_t *standard_lengths = unit.at;
    unit.at += opcode_base - 1;
    cursor_t directories = unit;
    for (uint64_t length = 0;;) {
        const char *directory = take_string(&unit, &length);
        if (!directory || !length) break;
    }
    if (unit.failed) return false;
    cursor_t files = unit;
    uint64_t address = 0, file = 1, previous_address = 0, previous_file = 1;
    int64_t line = 1, previous_line = 1;
    uint64_t column = 0, previous_column = 0;
    bool have_previous = false;
    (void)default_is_stmt;
    while (!program.failed && program.at < program.end) {
        uint8_t opcode = take_u8(&program);
        bool emit = false, end_sequence = false;
        if (opcode >= opcode_base) {
            uint8_t adjusted = (uint8_t)(opcode - opcode_base);
            address += (uint64_t)(adjusted / line_range) * minimum_length;
            line += line_base + adjusted % line_range;
            emit = true;
        }
        else if (opcode == 0) {
            uint64_t length = take_uleb(&program);
            const uint8_t *next = program.at + length;
            if (length > (uint64_t)(program.end - program.at)) return false;
            uint8_t sub = length ? take_u8(&program) : 0;
            if (sub == 1) {
                emit = true;
                end_sequence = true;
            }
            else if (sub == 2) address = take_bytes(&program,
                (unsigned)(length - 1 > 8 ? 8 : length - 1));
            program.at = next;
        }
        else if (opcode == 1) emit = true;
        else if (opcode == 2)
            address += take_uleb(&program) * minimum_length;
        else if (opcode == 3) line += take_sleb(&program);
        else if (opcode == 4) file = take_uleb(&program);
        else if (opcode == 8)
            address += (uint64_t)((255 - opcode_base) / line_range)
                * minimum_length;
        else if (opcode == 9) address += take_bytes(&program, 2);
        else if (opcode == 5) column = take_uleb(&program);
        else if (opcode == 12) take_uleb(&program);
        else if (opcode != 6 && opcode != 7 && opcode != 10 && opcode != 11)
            for (uint8_t i = 0; i < standard_lengths[opcode - 1]; i++)
                take_uleb(&program);
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
    cursor_t all = {image.line, image.line + image.line_bytes, false};
    while (!all.failed && all.at < all.end) {
        uint64_t length = take_bytes(&all, 4);
        if (length >= 0xfffffff0u) return false;
        if (all.failed || length > (uint64_t)(all.end - all.at)) return false;
        cursor_t unit = {all.at, all.at + length, false};
        if (search_unit(unit, target, result)) return true;
        all.at += length;
    }
    return false;
}
