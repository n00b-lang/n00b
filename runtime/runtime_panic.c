#if defined(__linux__)
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

#include "builtin_os.h"
#include "n00b-abi-v1.h"
#include "runtime_dwarf.h"
#include "runtime_panic.h"

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
#elif defined(_WIN32)
__attribute__((section("n00b_code$a"), used)) static const code_entry_t
    code_map_first = {0};
__attribute__((section("n00b_code$z"), used)) static const code_entry_t
    code_map_end = {0};
#define CODE_MAP_FIRST (&code_map_first + 1)
#define CODE_MAP_END (&code_map_end)
#else
#define CODE_MAP_FIRST ((const code_entry_t *)0)
#define CODE_MAP_END ((const code_entry_t *)0)
#endif

enum {
    MAX_TRACE_FRAMES = 128,
    WORKING_DIRECTORY_BYTES = 4096,
    REPORT_BUFFER_BYTES = 4096,
    OPERAND_VALUE_BYTES = 16,
    FAILURE_EXIT_STATUS = 101,
};

typedef unsigned __int128 wide_t;

typedef struct {
    const uint8_t *data;
    uint64_t length;
} text_t;

#define TEXT(literal) \
    ((text_t){(const uint8_t *)(literal), sizeof(literal) - 1})

typedef struct {
    text_t name;
    text_t directory;
    text_t file;
    uint32_t line;
    uint32_t column;
} trace_frame_t;

typedef struct {
    n00b_abi_string_v1 text;
    uint32_t format;
    uint32_t width;
} operand_site_t;

typedef struct {
    n00b_abi_string_v1 path;
    n00b_abi_string_v1 text;
    n00b_abi_string_v1 detail;
    uint32_t kind;
    uint32_t line;
    uint32_t column;
    uint32_t operand_count;
    const operand_site_t *operands;
} failure_site_t;

typedef struct {
    n00b_abi_string_v1 text;
    const failure_site_t *site;
    const uint8_t *values;
    uint64_t first;
    uint64_t second;
    uint64_t third;
    uint32_t kind;
} failure_record_t;

typedef struct {
    text_t head;
    text_t tail;
    bool joined;
} path_t;

typedef struct {
    path_t path;
    uint64_t skip;
    uint32_t line;
    uint32_t column;
    bool present;
} place_t;

__attribute__((weak)) uint8_t n00b_panic_trap = 0;

__attribute__((weak)) void
n00b_ffi_panic_hook(void)
{
}

static failure_record_t pending_failure;
static trace_frame_t trace_frames[MAX_TRACE_FRAMES];
static text_t working_directory;
static uint8_t report_bytes[REPORT_BUFFER_BYTES];
static uint64_t report_used;
static _Atomic uint64_t failing_thread;
static uintptr_t fault_pc;
static const uintptr_t *fault_frame;

static void
write_all(const uint8_t *bytes, uint64_t length)
{
    while (length) {
        uint32_t chunk = length > 0x40000000u ? 0x40000000u : (uint32_t)length;
        int32_t written = n00bb_runtime_write_bytes(2, bytes, chunk);
        if (written <= 0) return;
        bytes += written;
        length -= (uint64_t)written;
    }
}

static void
flush(void)
{
    write_all(report_bytes, report_used);
    report_used = 0;
}

static void
put(text_t text)
{
    if (report_used + text.length > sizeof report_bytes) flush();
    if (text.length > sizeof report_bytes) {
        write_all(text.data, text.length);
        return;
    }
    for (uint64_t i = 0; i < text.length; i++)
        report_bytes[report_used++] = text.data[i];
}

static void
put_byte(uint8_t byte)
{
    put((text_t){&byte, 1});
}

static text_t
string_text(const n00b_abi_string_v1 *value)
{
    while (value) {
        uint64_t selector = value->words[1];
        uint64_t tag = selector >> N00B_ABI_STRING_TAG_SHIFT;
        if (tag == N00B_ABI_STRING_RICH) {
            value = (const n00b_abi_string_v1 *)value->rich_value.body;
            continue;
        }
        if (tag == N00B_ABI_STRING_SHORT)
            return (text_t){value->short_value.bytes,
                value->short_value.selector
                    & N00B_ABI_STRING_SHORT_LENGTH_MASK};
        return (text_t){value->long_value.data,
                        selector & N00B_ABI_STRING_LONG_LENGTH_MASK};
    }
    return (text_t){0};
}

static text_t
text_from(text_t text, uint64_t at)
{
    if (at >= text.length) return (text_t){0};
    return (text_t){text.data + at, text.length - at};
}

static text_t
text_until(text_t text, uint64_t at)
{
    if (at < text.length) text.length = at;
    return text;
}

static bool
text_starts(text_t text, text_t prefix)
{
    if (text.length < prefix.length) return false;
    for (uint64_t i = 0; i < prefix.length; i++)
        if (text.data[i] != prefix.data[i]) return false;
    return true;
}

static bool
text_equal(text_t left, text_t right)
{
    return left.length == right.length && text_starts(left, right);
}

static uint64_t
last_dot(text_t text)
{
    for (uint64_t at = text.length; at > 0; at--)
        if (text.data[at - 1] == '.') return at;
    return 0;
}

static text_t
cut_at(text_t text, uint8_t stop)
{
    for (uint64_t i = 0; i < text.length; i++)
        if (text.data[i] == stop) return text_until(text, i);
    return text;
}

static text_t
module_part(text_t name)
{
    uint64_t at = last_dot(name);
    if (!at) return (text_t){0};
    return cut_at(text_until(name, at - 1), '$');
}

static text_t
function_part(text_t name)
{
    return cut_at(cut_at(text_from(name, last_dot(name)), '$'), '#');
}

static bool
library_frame(text_t name)
{
    static const text_t roots[] = {
        TEXT("builtin"), TEXT("sys"), TEXT("text"), TEXT("unicode"),
    };
    text_t module = module_part(name);
    for (size_t i = 0; i < sizeof roots / sizeof roots[0]; i++) {
        if (text_equal(module, roots[i])) return true;
        if (text_starts(module, roots[i])
            && module.length > roots[i].length
            && module.data[roots[i].length] == '.') return true;
    }
    return false;
}

static bool
reporter_frame(text_t name)
{
    return text_equal(module_part(name), TEXT("builtin.panic"));
}

static bool
shown_frame(text_t name)
{
    if (!name.length || text_equal(name, TEXT("main"))
        || reporter_frame(name)) return false;
    text_t function = function_part(name);
    if (text_equal(function, TEXT("__n00bb_module_init"))) return true;
    if (text_starts(function, TEXT("__n00bb_"))) return false;
    return !library_frame(name) || !text_starts(function, TEXT("__"));
}

static text_t
source_name(text_t name)
{
    text_t function = function_part(name);
    if (text_equal(function, TEXT("__n00bb_module_init")))
        return module_part(name);
    return function;
}

static void
put_unsigned(wide_t value)
{
    uint8_t digits[40];
    uint64_t start = sizeof digits;
    do {
        digits[--start] = (uint8_t)('0' + (uint8_t)(value % 10));
        value /= 10;
    } while (value);
    put((text_t){digits + start, sizeof digits - start});
}

static void
put_signed(int64_t value)
{
    if (value < 0) {
        put_byte('-');
        put_unsigned((wide_t)(0 - (uint64_t)value));
        return;
    }
    put_unsigned((wide_t)value);
}

static void
put_os_error(int64_t code)
{
    static const struct {
        int code;
        text_t text;
    } known[] = {
        {ENOENT, TEXT("no such file or directory")},
        {EACCES, TEXT("permission denied")},
        {EISDIR, TEXT("it is a directory")},
        {EINVAL, TEXT("invalid argument")},
        {EMFILE, TEXT("too many open files")},
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (code == -known[i].code) {
            put(known[i].text);
            return;
        }
    put(TEXT("operating system error "));
    put_signed(code < 0 ? -code : code);
}

static wide_t
masked(wide_t bits, uint32_t width)
{
    if (!width || width >= 128) return bits;
    return bits & (((wide_t)1 << width) - 1);
}

static void
put_signed_bits(wide_t bits, uint32_t width)
{
    uint32_t size = !width || width > 128 ? 128 : width;
    wide_t value = masked(bits, size);
    if (!((value >> (size - 1)) & 1)) {
        put_unsigned(value);
        return;
    }
    if (size < 128) value |= ~(((wide_t)1 << size) - 1);
    put_byte('-');
    put_unsigned((wide_t)0 - value);
}

static void
put_fraction(double value)
{
    uint8_t digits[7] = {'.'};
    uint64_t used = 0;
    for (uint64_t i = 0; i < 6; i++) {
        value *= 10.0;
        int64_t digit = (int64_t)value;
        if (digit < 0) digit = 0;
        if (digit > 9) digit = 9;
        digits[i + 1] = (uint8_t)('0' + digit);
        value -= (double)digit;
        if (digit) used = i + 1;
    }
    if (!used) {
        put(TEXT(".0"));
        return;
    }
    put((text_t){digits, used + 1});
}

static void
put_float(double value, uint64_t bits)
{
    bool negative = (bits >> 63) & 1;
    double magnitude = negative ? -value : value;
    if (((bits >> 52) & 0x7ff) == 0x7ff
        && (bits & UINT64_C(0x000fffffffffffff))) {
        put(TEXT("nan"));
        return;
    }
    if (negative) put_byte('-');
    if (((bits >> 52) & 0x7ff) == 0x7ff) {
        put(TEXT("infinity"));
        return;
    }
    if (magnitude == 0.0) {
        put(TEXT("0.0"));
        return;
    }
    double scaled = magnitude;
    int64_t exponent = 0;
    while (scaled >= 10.0) {
        scaled /= 10.0;
        exponent++;
    }
    while (scaled < 1.0) {
        scaled *= 10.0;
        exponent--;
    }
    if (exponent < -4 || exponent >= 16) {
        uint64_t first = (uint64_t)scaled;
        put_unsigned(first);
        put_fraction(scaled - (double)first);
        put_byte('e');
        put_signed(exponent);
        return;
    }
    uint64_t whole = (uint64_t)magnitude;
    put_unsigned(whole);
    put_fraction(magnitude - (double)whole);
}

static void
put_char(uint64_t code)
{
    uint8_t bytes[6] = {'\''};
    uint64_t used = 1;
    if (code < 0x80) bytes[used++] = (uint8_t)code;
    else if (code < 0x800) {
        bytes[used++] = (uint8_t)(0xc0 | (code >> 6));
        bytes[used++] = (uint8_t)(0x80 | (code & 0x3f));
    }
    else if (code < 0x10000 && (code < 0xd800 || code > 0xdfff)) {
        bytes[used++] = (uint8_t)(0xe0 | (code >> 12));
        bytes[used++] = (uint8_t)(0x80 | ((code >> 6) & 0x3f));
        bytes[used++] = (uint8_t)(0x80 | (code & 0x3f));
    }
    else if (code >= 0x10000 && code <= 0x10ffff) {
        bytes[used++] = (uint8_t)(0xf0 | (code >> 18));
        bytes[used++] = (uint8_t)(0x80 | ((code >> 12) & 0x3f));
        bytes[used++] = (uint8_t)(0x80 | ((code >> 6) & 0x3f));
        bytes[used++] = (uint8_t)(0x80 | (code & 0x3f));
    }
    put((text_t){bytes, used});
    put_byte('\'');
}

static void
put_value(const uint8_t *slot, uint32_t format, uint32_t width)
{
    uint64_t low;
    uint64_t high;
    __builtin_memcpy(&low, slot, sizeof low);
    __builtin_memcpy(&high, slot + sizeof low, sizeof high);
    wide_t wide = ((wide_t)high << 64) | low;
    switch (format) {
    case N00BB_OPERAND_SIGNED:
        put_signed_bits(wide, width);
        return;
    case N00BB_OPERAND_UNSIGNED:
        put_unsigned(masked(wide, width));
        return;
    case N00BB_OPERAND_BOOL:
        put(low ? TEXT("true") : TEXT("false"));
        return;
    case N00BB_OPERAND_FLOAT: {
        double value;
        __builtin_memcpy(&value, slot, sizeof value);
        put_float(value, low);
        return;
    }
    case N00BB_OPERAND_STRING: {
        n00b_abi_string_v1 value;
        __builtin_memcpy(&value, slot, sizeof value);
        put_byte('"');
        put(string_text(&value));
        put_byte('"');
        return;
    }
    case N00BB_OPERAND_CHAR:
        put_char(low);
        return;
    default:
        put_byte('?');
    }
}

static void
put_operand(const failure_record_t *failure, uint32_t index)
{
    const failure_site_t *site = failure->site;
    if (index >= site->operand_count || !failure->values) {
        put_byte('?');
        return;
    }
    const operand_site_t *shape = &site->operands[index];
    put_value(failure->values + (uint64_t)index * OPERAND_VALUE_BYTES,
              shape->format, shape->width);
}

static void
put_operand_notes(const failure_record_t *failure)
{
    const failure_site_t *site = failure->site;
    for (uint32_t i = 0; i < site->operand_count; i++) {
        put(i ? TEXT(", ") : TEXT(" ("));
        put_byte('`');
        put(string_text(&site->operands[i].text));
        put(TEXT("` is "));
        put_operand(failure, i);
    }
    if (site->operand_count) put_byte(')');
}

static void
put_invariant(uint64_t invariant, text_t text)
{
    static const text_t reasons[] = {
        [N00BB_INVARIANT_UNTYPED_ALLOCATION] =
            TEXT("an allocation has no type"),
        [N00BB_INVARIANT_UNKNOWN_ALLOCATOR] =
            TEXT("an allocation names an unknown allocator"),
        [N00BB_INVARIANT_NATIVE_CALL_WITHOUT_MUTATOR] =
            TEXT("native code called a n00b function from a thread the "
                 "runtime does not know"),
        [N00BB_INVARIANT_UNKNOWN_FINALIZER] =
            TEXT("an object has no finalizer for its type"),
        [N00BB_INVARIANT_METADATA_REJECTED] =
            TEXT("the program's type metadata was rejected at startup"),
    };
    if (!invariant || invariant >= sizeof reasons / sizeof reasons[0]) {
        put(text);
        return;
    }
    put(TEXT("internal runtime check failed: "));
    put(reasons[invariant]);
}

static void
put_runtime_message(const failure_record_t *failure)
{
    text_t text = string_text(&failure->text);
    switch (failure->kind) {
    case N00BB_FAILURE_STACK_OVERFLOW:
        put(TEXT("stack overflow"));
        return;
    case N00BB_FAILURE_OUT_OF_MEMORY:
        put(TEXT("out of memory while allocating "));
        put_unsigned(failure->first);
        put(TEXT(" bytes"));
        return;
    case N00BB_FAILURE_INDEX:
        put(TEXT("index "));
        put_signed((int64_t)failure->first);
        put(TEXT(" is out of range for "));
        put(text);
        put(TEXT(" of length "));
        put_unsigned(failure->second);
        return;
    case N00BB_FAILURE_RANGE:
        put(TEXT("range "));
        put_signed((int64_t)failure->first);
        put(TEXT(".."));
        put_signed((int64_t)failure->second);
        put(TEXT(" is out of bounds for "));
        put(text);
        put(TEXT(" of length "));
        put_unsigned(failure->third);
        return;
    case N00BB_FAILURE_INVARIANT:
        put_invariant(failure->first, text);
        return;
    case N00BB_FAILURE_OS:
        put(text);
        put(TEXT(": "));
        put_os_error((int64_t)failure->first);
        return;
    default:
        put(text);
    }
}

static void
put_site_message(const failure_record_t *failure)
{
    const failure_site_t *site = failure->site;
    text_t text = string_text(&site->text);
    text_t detail = string_text(&site->detail);
    switch (failure->kind) {
    case N00BB_FAILURE_REQUIRE:
    case N00BB_FAILURE_ASSERT:
        put(failure->kind == N00BB_FAILURE_REQUIRE ? TEXT("requirement `")
                                                   : TEXT("assertion `"));
        put(text);
        put(TEXT("` failed"));
        put_operand_notes(failure);
        return;
    case N00BB_FAILURE_UNREACHABLE:
        put(TEXT("reached code that `"));
        put(text);
        put(TEXT("` marks as unreachable"));
        return;
    case N00BB_FAILURE_INDEX:
        put(TEXT("index "));
        put_operand(failure, 0);
        put(TEXT(" is out of range for "));
        put(detail);
        if (site->operand_count == 1) return;
        put(TEXT(" of length "));
        put_operand(failure, 1);
        return;
    case N00BB_FAILURE_DIVIDE_BY_ZERO:
        put(TEXT("division of "));
        put_operand(failure, 0);
        put(TEXT(" by zero"));
        return;
    case N00BB_FAILURE_REMAINDER_BY_ZERO:
        put(TEXT("remainder of "));
        put_operand(failure, 0);
        put(TEXT(" divided by zero"));
        return;
    case N00BB_FAILURE_OVERFLOW:
        put_byte('`');
        if (site->operand_count == 1) {
            put(text);
            put_operand(failure, 0);
        }
        else {
            put_operand(failure, 0);
            put_byte(' ');
            put(text);
            put_byte(' ');
            put_operand(failure, 1);
        }
        put(TEXT("` overflows `"));
        put(detail);
        put_byte('`');
        return;
    case N00BB_FAILURE_SHIFT:
        put(TEXT("shift amount "));
        put_operand(failure, 0);
        put(TEXT(" is out of range for `"));
        put(detail);
        put(TEXT("`, which has "));
        put_operand(failure, 1);
        put(TEXT(" bits"));
        return;
    case N00BB_FAILURE_CAST:
        put(TEXT("cast of "));
        put_operand(failure, 0);
        put(TEXT(" to `"));
        put(detail);
        put(TEXT("` is out of range"));
        return;
    case N00BB_FAILURE_ENUM:
        put_operand(failure, 0);
        put(TEXT(" is not a value of enum `"));
        put(detail);
        put_byte('`');
        return;
    case N00BB_FAILURE_VARIANT:
        if (text.length) {
            put(TEXT("this `"));
            put(detail);
            put(TEXT("` value does not hold the `"));
            put(text);
            put(TEXT("` arm"));
            return;
        }
        put(TEXT("tag "));
        put_operand(failure, 0);
        put(TEXT(" is not an arm of `"));
        put(detail);
        put_byte('`');
        return;
    case N00BB_FAILURE_UNINITIALIZED:
        put(TEXT("global `"));
        put(text_from(text, last_dot(text)));
        put(TEXT("` is read before it is initialized"));
        return;
    case N00BB_FAILURE_ALLOCATION_SIZE:
        put_byte('`');
        put(detail);
        put(TEXT("` of "));
        put_operand(failure, 0);
        put(TEXT(" elements is too large to allocate"));
        return;
    case N00BB_FAILURE_OUT_OF_MEMORY:
        if (site->operand_count == 1) {
            put(TEXT("out of memory while allocating "));
            put_operand(failure, 0);
            put(TEXT(" bytes"));
            return;
        }
        put(TEXT("out of memory while allocating `"));
        put(detail);
        put_byte('`');
        return;
    case N00BB_FAILURE_UNCAUGHT:
        if (!site->operand_count) {
            put(TEXT("uncaught failure of type `"));
            put(detail);
            put_byte('`');
            return;
        }
        put(TEXT("uncaught failure "));
        put_operand(failure, 0);
        return;
    case N00BB_FAILURE_ZERO_STEP:
        put(TEXT("range step is zero"));
        return;
    case N00BB_FAILURE_MISSING_RETURN:
        put_byte('`');
        put(text);
        put(TEXT("` reached its end without returning a value"));
        return;
    case N00BB_FAILURE_SHAPE:
        put(text);
        put(TEXT(": expected "));
        put_operand(failure, 0);
        put(TEXT(", found "));
        put_operand(failure, 1);
        return;
    case N00BB_FAILURE_NULL_CALLBACK:
        put(TEXT("call through `"));
        put(detail);
        put(TEXT("` that was never set"));
        return;
    case N00BB_FAILURE_LENGTH:
        put(TEXT("length "));
        put_operand(failure, 0);
        put(TEXT(" is out of range for `"));
        put(detail);
        put_byte('`');
        return;
    default:
        put(TEXT("internal runtime check failed: "));
        put(text);
    }
}

static uint64_t
path_length(path_t path)
{
    return path.head.length + path.joined + path.tail.length;
}

static uint8_t
path_byte(path_t path, uint64_t at)
{
    if (at < path.head.length) return path.head.data[at];
    at -= path.head.length;
    if (path.joined) {
        if (!at) return '/';
        at--;
    }
    return path.tail.data[at];
}

static uint64_t
relative_skip(path_t path)
{
    uint64_t n = working_directory.length;
    if (!n || path_length(path) <= n + 1 || path_byte(path, n) != '/')
        return 0;
    for (uint64_t i = 0; i < n; i++)
        if (path_byte(path, i) != working_directory.data[i]) return 0;
    return n + 1;
}

static place_t
make_place(path_t path, uint32_t line, uint32_t column)
{
    return (place_t){.path = path, .skip = relative_skip(path),
                     .line = line, .column = column, .present = true};
}

static place_t
frame_place(const trace_frame_t *frame)
{
    if (!frame->file.length || !frame->line) return (place_t){0};
    path_t path = {.head = frame->file};
    if (frame->file.data[0] != '/' && frame->directory.length)
        path = (path_t){frame->directory, frame->file, true};
    return make_place(path, frame->line, frame->column);
}

static bool
place_equal(place_t left, place_t right)
{
    if (left.present != right.present) return false;
    if (!left.present) return true;
    uint64_t length = path_length(left.path) - left.skip;
    if (left.line != right.line || left.column != right.column
        || length != path_length(right.path) - right.skip) return false;
    for (uint64_t i = 0; i < length; i++)
        if (path_byte(left.path, left.skip + i)
            != path_byte(right.path, right.skip + i)) return false;
    return true;
}

static void
put_place(place_t place)
{
    uint64_t skip = place.skip;
    if (skip < place.path.head.length) {
        put(text_from(place.path.head, skip));
        skip = 0;
    }
    else skip -= place.path.head.length;
    if (place.path.joined) {
        if (!skip) put_byte('/');
        else skip--;
    }
    put(text_from(place.path.tail, skip));
    put_byte(':');
    put_unsigned(place.line);
    if (!place.column) return;
    put_byte(':');
    put_unsigned(place.column);
}

static place_t
failure_place(const failure_record_t *failure, uint64_t count)
{
    if (failure->site) {
        text_t path = string_text(&failure->site->path);
        if (path.length && failure->site->line)
            return make_place((path_t){.head = path}, failure->site->line,
                              failure->site->column);
    }
    place_t fallback = {0};
    for (uint64_t i = 0; i < count; i++) {
        text_t name = trace_frames[i].name;
        place_t place = frame_place(&trace_frames[i]);
        if (reporter_frame(name) || !place.present) continue;
        if (!library_frame(name)) return place;
        if (!fallback.present) fallback = place;
    }
    return fallback;
}

static void
put_repeats(uint64_t repeats)
{
    if (repeats == 1) put(TEXT("  ... the frame above repeats once\n"));
    if (repeats < 2) return;
    put(TEXT("  ... the frame above repeats "));
    put_unsigned(repeats);
    put(TEXT(" more times\n"));
}

static void
put_backtrace(uint64_t count)
{
    put(TEXT("backtrace:\n"));
    text_t previous_name = {0};
    place_t previous_place = {0};
    bool shown_any = false;
    uint64_t repeats = 0;
    for (uint64_t i = 0; i < count; i++) {
        text_t name = trace_frames[i].name;
        place_t place = frame_place(&trace_frames[i]);
        if (!shown_frame(name) || (!place.present && library_frame(name)))
            continue;
        text_t shown = source_name(name);
        if (shown_any && text_equal(shown, previous_name)
            && place_equal(place, previous_place)) {
            repeats++;
            continue;
        }
        put_repeats(repeats);
        repeats = 0;
        shown_any = true;
        previous_name = shown;
        previous_place = place;
        put(TEXT("  at "));
        put(shown);
        if (place.present) {
            put(TEXT(" ("));
            put_place(place);
            put_byte(')');
        }
        put_byte('\n');
    }
    put_repeats(repeats);
}

static void
put_report(const failure_record_t *failure, uint64_t count)
{
    if (failure->kind == N00BB_FAILURE_STACK_OVERFLOW && count > 1
        && text_equal(trace_frames[0].name, trace_frames[1].name)) {
        for (uint64_t i = 1; i < count; i++)
            trace_frames[i - 1] = trace_frames[i];
        count--;
    }
    put(TEXT("panic: "));
    if (failure->site) put_site_message(failure);
    else put_runtime_message(failure);
    put_byte('\n');
    place_t place = failure_place(failure, count);
    if (place.present) {
        put(TEXT("  --> "));
        put_place(place);
        put_byte('\n');
    }
    put_backtrace(count);
    flush();
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
#if !defined(_WIN32)
    Dl_info symbol = {0};
    if (best && dladdr((const void *)address, &symbol) && symbol.dli_saddr
        && (uintptr_t)symbol.dli_saddr > (uintptr_t)best->start)
        return NULL;
#endif
    return best;
}

static bool
is_main(const code_entry_t *entry)
{
    return entry && entry->length == 4 && entry->name[0] == 'm'
        && entry->name[1] == 'a' && entry->name[2] == 'i'
        && entry->name[3] == 'n';
}

static text_t
bytes_text(const char *bytes, uint64_t length)
{
    return (text_t){(const uint8_t *)bytes, bytes ? length : 0};
}

static void
locate_frame(trace_frame_t *frame, const n00bb_source_line_t *source)
{
    frame->directory = bytes_text(source->directory,
                                  source->directory_length);
    frame->file = bytes_text(source->file, source->file_length);
    frame->line = source->line;
    frame->column = source->column;
}

static const code_entry_t *
record_frame(uint64_t *count, uintptr_t address)
{
    n00bb_inline_frame_t inlined[MAX_TRACE_FRAMES];
    size_t inlined_count = n00bb_runtime_inline_frames(address, inlined,
        MAX_TRACE_FRAMES);
    n00bb_source_line_t source = {0};
    bool located = n00bb_runtime_source_line(address, &source);
    for (size_t i = 0; i < inlined_count && *count < MAX_TRACE_FRAMES - 1;
         i++) {
        trace_frame_t *frame = &trace_frames[(*count)++];
        *frame = (trace_frame_t){
            .name = bytes_text(inlined[i].name, inlined[i].name_length),
        };
        if (located) locate_frame(frame, &source);
        source = inlined[i].call;
        located = source.line != 0;
    }
    trace_frame_t *frame = &trace_frames[(*count)++];
    *frame = (trace_frame_t){0};
    const code_entry_t *entry = function_at(address);
    if (entry) frame->name = bytes_text(entry->name, entry->length);
    if (located) locate_frame(frame, &source);
    return entry;
}

#if defined(_WIN32)
static uint64_t unwind_frames(const uintptr_t *frame);
#endif

static bool
frame_on_stack(const uintptr_t *frame)
{
    return n00b_os_on_stack(frame, 2 * sizeof(uintptr_t));
}

static uint64_t
collect_frames(const uintptr_t *frame, uintptr_t first_pc)
{
#if defined(_WIN32)
    if (!first_pc) return unwind_frames(frame);
#endif
    uint64_t count = 0;
    if (first_pc && is_main(record_frame(&count, first_pc))) return count;
    while (frame && count < MAX_TRACE_FRAMES && frame_on_stack(frame)) {
        uintptr_t return_address = frame[1];
        if (!return_address
            || is_main(record_frame(&count, return_address - 1)))
            break;
        const uintptr_t *caller = (const uintptr_t *)frame[0];
        if (caller <= frame) break;
        frame = caller;
    }
    return count;
}

static uint8_t working_directory_bytes[WORKING_DIRECTORY_BYTES];

static text_t
current_directory(void)
{
    uint64_t length = n00b_os_current_directory(working_directory_bytes,
        sizeof working_directory_bytes);
    return bytes_text((const char *)working_directory_bytes, length);
}

[[noreturn]] static void
terminate(void)
{
    n00b_ffi_panic_hook();
    if (n00b_panic_trap) {
        __builtin_debugtrap();
        __builtin_trap();
    }
    n00b_os_exit(FAILURE_EXIT_STATUS);
}

static void
claim_failure(void)
{
    uint64_t self = n00b_os_thread_id();
    uint64_t expected = 0;
    if (atomic_compare_exchange_strong(&failing_thread, &expected, self))
        return;
    if (expected == self) {
        flush();
        put(TEXT("panic: another failure occurred while reporting a "
                 "failure\n"));
        flush();
        terminate();
    }
    for (;;) n00b_os_thread_yield();
}

[[noreturn]] static void
report(const uintptr_t *frame, uintptr_t first_pc)
{
    uint64_t count = collect_frames(frame, first_pc);
    working_directory = current_directory();
    put_report(&pending_failure, count);
    terminate();
}

__attribute__((noinline, cold)) void
n00bb_runtime_report_failure(uint32_t kind, uint64_t first, uint64_t second,
                             uint64_t third, n00b_abi_string_v1 text)
{
    claim_failure();
    pending_failure = (failure_record_t){
        .text = text, .first = first, .second = second, .third = third,
        .kind = kind,
    };
    report((const uintptr_t *)__builtin_frame_address(0), 0);
}

__attribute__((noinline, cold)) void
n00bb_runtime_check_failed(const void *site, const void *values)
{
    claim_failure();
    const failure_site_t *checked = site;
    pending_failure = (failure_record_t){
        .site = checked, .values = values,
        .kind = checked ? checked->kind : N00BB_FAILURE_INVARIANT,
    };
    report((const uintptr_t *)__builtin_frame_address(0), 0);
}

__attribute__((noinline, cold)) void
n00bb_runtime_fail(uint32_t kind, uint64_t value)
{
    claim_failure();
    pending_failure = (failure_record_t){.first = value, .kind = kind};
    if (fault_pc) report(fault_frame, fault_pc);
    report((const uintptr_t *)__builtin_frame_address(0), 0);
}

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static uint64_t
unwind_frames(const uintptr_t *frame)
{
    uint64_t count = 0;
    CONTEXT context;
    RtlCaptureContext(&context);
    while (count < MAX_TRACE_FRAMES && frame_on_stack(
               (const uintptr_t *)(uintptr_t)context.Rsp)) {
        DWORD64 image = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip,
                                                            &image, NULL);
        if (function) {
            void *handler_data = NULL;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, function,
                             &context, &handler_data, &establisher, NULL);
        }
        else {
            context.Rip = *(const DWORD64 *)(uintptr_t)context.Rsp;
            context.Rsp += sizeof(DWORD64);
        }
        if (!context.Rip) break;
        if (context.Rsp <= (DWORD64)(uintptr_t)frame) continue;
        DWORD64 caller_image = 0;
        PRUNTIME_FUNCTION caller = RtlLookupFunctionEntry(context.Rip - 1,
                                                          &caller_image, NULL);
        const code_entry_t *entry = function_at((uintptr_t)context.Rip - 1);
        if (!caller || !entry || (uintptr_t)entry->start
                != (uintptr_t)(caller_image + caller->BeginAddress))
            continue;
        if (is_main(record_frame(&count, (uintptr_t)context.Rip - 1))) break;
    }
    return count;
}

static void
put_hex_field(uint8_t *line, uint64_t *used, const char *label,
              size_t label_bytes, uint64_t value)
{
    for (size_t i = 0; i < label_bytes; i++) line[(*used)++] = (uint8_t)label[i];
    static const char digits[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4)
        line[(*used)++] = (uint8_t)digits[(value >> shift) & 15];
}

static LONG CALLBACK
windows_fault(EXCEPTION_POINTERS *exception)
{
    DWORD code = exception->ExceptionRecord->ExceptionCode;
    CONTEXT *context = exception->ContextRecord;
    if (code == EXCEPTION_STACK_OVERFLOW) {
        fault_pc = (uintptr_t)context->Rip;
        fault_frame = (const uintptr_t *)context->Rbp;
        n00bb_runtime_fail(N00BB_FAILURE_STACK_OVERFLOW, 0);
    }
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        uint8_t line[160];
        uint64_t used = 0;
        uintptr_t image = (uintptr_t)GetModuleHandleW(nullptr);
        ULONG_PTR *info = exception->ExceptionRecord->ExceptionInformation;
        put_hex_field(line, &used, "access violation: image+0x", 26,
                      (uint64_t)(context->Rip - image));
        put_hex_field(line, &used, info[0] ? " writing 0x" : " reading 0x",
                      11, (uint64_t)info[1]);
        line[used++] = '\n';
        write_all(line, used);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void
n00bb_runtime_install_failure_handlers(void)
{
    static atomic_bool installed;
    ULONG reserve = 1u << 16;
    SetThreadStackGuarantee(&reserve);
    if (!atomic_exchange(&installed, true))
        AddVectoredExceptionHandler(1, windows_fault);
}
#else
#include <signal.h>
#include <stdlib.h>

enum { ALTERNATE_STACK_BYTES = 1u << 18, STACK_FAULT_WINDOW = 1u << 16 };

static void
stack_fault(int number, siginfo_t *info, void *raw_context)
{
    ucontext_t *context = raw_context;
#if defined(__APPLE__) && defined(__aarch64__)
    uintptr_t sp = (uintptr_t)context->uc_mcontext->__ss.__sp;
    uintptr_t pc = (uintptr_t)context->uc_mcontext->__ss.__pc;
    uintptr_t fp = (uintptr_t)context->uc_mcontext->__ss.__fp;
#elif defined(__APPLE__) && defined(__x86_64__)
    uintptr_t sp = (uintptr_t)context->uc_mcontext->__ss.__rsp;
    uintptr_t pc = (uintptr_t)context->uc_mcontext->__ss.__rip;
    uintptr_t fp = (uintptr_t)context->uc_mcontext->__ss.__rbp;
#elif defined(__linux__) && defined(__aarch64__)
    uintptr_t sp = (uintptr_t)context->uc_mcontext.sp;
    uintptr_t pc = (uintptr_t)context->uc_mcontext.pc;
    uintptr_t fp = (uintptr_t)context->uc_mcontext.regs[29];
#elif defined(__linux__) && defined(__x86_64__)
    uintptr_t sp = (uintptr_t)context->uc_mcontext.gregs[REG_RSP];
    uintptr_t pc = (uintptr_t)context->uc_mcontext.gregs[REG_RIP];
    uintptr_t fp = (uintptr_t)context->uc_mcontext.gregs[REG_RBP];
#else
#error "unsupported stack fault architecture"
#endif
    uintptr_t address = (uintptr_t)info->si_addr;
    if (address + STACK_FAULT_WINDOW < sp
        || address > sp + STACK_FAULT_WINDOW) {
        signal(number, SIG_DFL);
        return;
    }
    fault_pc = pc;
    fault_frame = (const uintptr_t *)fp;
    n00bb_runtime_fail(N00BB_FAILURE_STACK_OVERFLOW, 0);
}

static atomic_bool handlers_installed;

void
n00bb_runtime_install_failure_handlers(void)
{
    stack_t alternate = {
        .ss_sp = malloc(ALTERNATE_STACK_BYTES),
        .ss_size = ALTERNATE_STACK_BYTES,
    };
    if (alternate.ss_sp) sigaltstack(&alternate, NULL);
    if (atomic_exchange(&handlers_installed, true)) return;
    struct sigaction action = {0};
    action.sa_sigaction = stack_fault;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
}
#endif
