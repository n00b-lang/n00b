#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "runtime_dwarf.h"

int32_t n00bb_runtime_write_bytes(int32_t fd, const uint8_t *bytes,
                                  uint32_t count);

typedef struct {
    const void *start;
    const char *name;
    uint64_t length;
} code_entry_t;

#if defined(__APPLE__)
extern const code_entry_t code_map_first
    __asm("section$start$__DATA$__n00b_code");
extern const code_entry_t code_map_end
    __asm("section$end$__DATA$__n00b_code");
#define CODE_MAP_FIRST (&code_map_first)
#define CODE_MAP_END (&code_map_end)
#elif defined(__ELF__)
extern const code_entry_t __start_n00b_code[] __attribute__((weak));
extern const code_entry_t __stop_n00b_code[] __attribute__((weak));
#define CODE_MAP_FIRST __start_n00b_code
#define CODE_MAP_END __stop_n00b_code
#else
#define CODE_MAP_FIRST ((const code_entry_t *)0)
#define CODE_MAP_END ((const code_entry_t *)0)
#endif

enum { MAX_TRACE_FRAMES = 128 };

static void
write_all(const char *text, uint64_t length)
{
    while (length) {
        uint32_t chunk = length > 0x40000000u ? 0x40000000u : (uint32_t)length;
        int32_t written = n00bb_runtime_write_bytes(2, (const uint8_t *)text,
                                                    chunk);
        if (written <= 0) return;
        text += written;
        length -= (uint64_t)written;
    }
}

static void
write_text(const char *text)
{
    uint64_t length = 0;
    while (text[length]) length++;
    write_all(text, length);
}

static void
write_hex(uintptr_t value)
{
    char digits[2 + sizeof(uintptr_t) * 2];
    digits[0] = '0';
    digits[1] = 'x';
    for (size_t i = 0; i < sizeof(uintptr_t) * 2; i++) {
        unsigned nibble = (unsigned)(value >> ((sizeof(uintptr_t) * 2 - 1 - i)
                                               * 4)) & 0xfu;
        digits[2 + i] = (char)(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
    }
    write_all(digits, sizeof digits);
}

static void
write_decimal(uint32_t value)
{
    char digits[10];
    size_t at = sizeof digits;
    do {
        digits[--at] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    write_all(digits + at, sizeof digits - at);
}

static const code_entry_t *
function_at(uintptr_t address)
{
    const code_entry_t *best = NULL;
    for (const code_entry_t *entry = CODE_MAP_FIRST;
         entry && entry < CODE_MAP_END; entry++) {
        uintptr_t start = (uintptr_t)entry->start;
        if (start && start <= address
            && (!best || start > (uintptr_t)best->start)) best = entry;
    }
    return best;
}

static bool
is_main(const code_entry_t *entry)
{
    return entry->length == 4 && entry->name[0] == 'm'
        && entry->name[1] == 'a' && entry->name[2] == 'i'
        && entry->name[3] == 'n';
}

static void
write_trace(const uintptr_t *frame)
{
    write_text("backtrace:\n");
    for (size_t depth = 0; frame && depth < MAX_TRACE_FRAMES; depth++) {
        uintptr_t return_address = frame[1];
        if (!return_address) break;
        const code_entry_t *entry = function_at(return_address - 1);
        write_text("  at ");
        if (entry) write_all(entry->name, entry->length);
        else write_hex(return_address);
        n00bb_source_line_t source = {0};
        if (n00bb_runtime_source_line(return_address - 1, &source)) {
            write_text(" (");
            if (source.directory) {
                write_all(source.directory, source.directory_length);
                write_text("/");
            }
            write_all(source.file, source.file_length);
            write_text(":");
            write_decimal(source.line);
            write_text(")");
        }
        write_text("\n");
        if (entry && is_main(entry)) break;
        const uintptr_t *caller = (const uintptr_t *)frame[0];
        if (caller <= frame) break;
        frame = caller;
    }
}

__attribute__((noinline, noreturn, cold)) void
n00bb_runtime_panic_trace(void)
{
    write_trace((const uintptr_t *)__builtin_frame_address(0));
    __builtin_trap();
}

__attribute__((noinline, noreturn, cold)) void
n00bb_runtime_trap(const char *reason, uint64_t length)
{
    write_text("panic: ");
    write_all(reason, length);
    write_text("\n");
    write_trace((const uintptr_t *)__builtin_frame_address(0));
    __builtin_trap();
}
