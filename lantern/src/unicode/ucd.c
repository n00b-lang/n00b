#include "private/unicode/ucd.h"
#include "private/unicode/ucd_tables.h"

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/list.h"
#include "unicode/encoding.h"

enum {
    HANGUL_S_BASE  = 0xAC00,
    HANGUL_L_BASE  = 0x1100,
    HANGUL_V_BASE  = 0x1161,
    HANGUL_T_BASE  = 0x11A7,
    HANGUL_L_COUNT = 19,
    HANGUL_V_COUNT = 21,
    HANGUL_T_COUNT = 28,
    HANGUL_N_COUNT = HANGUL_V_COUNT * HANGUL_T_COUNT,
    HANGUL_S_COUNT = HANGUL_L_COUNT * HANGUL_N_COUNT,
};

typedef core_list_t(uint32_t) codepoints_t;

static void
push(codepoints_t *list, uint32_t cp)
{
    core_list_push(*list, cp);
}

static bool
in_ranges(const ucd_range_t *ranges, size_t count, uint32_t cp)
{
    size_t low = 0;
    size_t high = count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (cp < ranges[mid].first) high = mid;
        else if (cp > ranges[mid].last) low = mid + 1;
        else return true;
    }
    return false;
}

static uint8_t
range_value(const ucd_valued_range_t *ranges, size_t count, uint32_t cp)
{
    size_t low = 0;
    size_t high = count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (cp < ranges[mid].first) high = mid;
        else if (cp > ranges[mid].last) low = mid + 1;
        else return ranges[mid].value;
    }
    return 0;
}

static const ucd_mapping_t *
mapping(const ucd_mapping_t *rows, size_t count, uint32_t cp)
{
    size_t low = 0;
    size_t high = count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (cp < rows[mid].cp) high = mid;
        else if (cp > rows[mid].cp) low = mid + 1;
        else return &rows[mid];
    }
    return nullptr;
}

core_string_t
ucd_version(void)
{
    return ucd_unicode_version;
}

bool
ucd_is_xid_start(core_codepoint_t cp)
{
    return in_ranges(ucd_xid_start, ucd_xid_start_count, cp);
}

bool
ucd_is_xid_continue(core_codepoint_t cp)
{
    return in_ranges(ucd_xid_continue, ucd_xid_continue_count, cp);
}

bool
ucd_is_extended_pictographic(core_codepoint_t cp)
{
    return in_ranges(ucd_extended_pictographic,
                     ucd_extended_pictographic_count, cp);
}

uint8_t
ucd_display_width(core_codepoint_t cp)
{
    if (in_ranges(ucd_zero_width, ucd_zero_width_count, cp)) return 0;
    return in_ranges(ucd_wide, ucd_wide_count, cp) ? 2 : 1;
}

uint8_t
ucd_script(core_codepoint_t cp)
{
    return range_value(ucd_scripts, ucd_scripts_count, cp);
}

static char
loose_byte(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool
loose_skip(char c)
{
    return c == '_' || c == '-' || c == ' ';
}

static bool
loose_equal(core_string_t a, core_string_t b)
{
    size_t i = 0;
    size_t j = 0;
    for (;;) {
        while (i < a.u8_bytes && loose_skip(a.data[i])) i++;
        while (j < b.u8_bytes && loose_skip(b.data[j])) j++;
        if (i == a.u8_bytes || j == b.u8_bytes)
            return i == a.u8_bytes && j == b.u8_bytes;
        if (loose_byte(a.data[i++]) != loose_byte(b.data[j++])) return false;
    }
}

bool
ucd_script_named(core_string_t name, uint8_t *script)
{
    for (size_t i = 0; i < ucd_script_names_count; i++) {
        if (loose_equal(name, ucd_script_names[i].long_name)
            || loose_equal(name, ucd_script_names[i].short_name)) {
            *script = (uint8_t)i;
            return true;
        }
    }
    return false;
}

static uint8_t
combining_class(uint32_t cp)
{
    return range_value(ucd_combining_class, ucd_combining_class_count, cp);
}

static void
decompose(codepoints_t *out, uint32_t cp, bool compat)
{
    if (cp >= HANGUL_S_BASE && cp < HANGUL_S_BASE + HANGUL_S_COUNT) {
        uint32_t index = cp - HANGUL_S_BASE;
        push(out, HANGUL_L_BASE + index / HANGUL_N_COUNT);
        push(out, HANGUL_V_BASE + index % HANGUL_N_COUNT / HANGUL_T_COUNT);
        if (index % HANGUL_T_COUNT)
            push(out, HANGUL_T_BASE + index % HANGUL_T_COUNT);
        return;
    }
    const ucd_mapping_t *row = mapping(ucd_decompositions,
                                       ucd_decompositions_count, cp);
    if (!row || (row->compat && !compat)) {
        push(out, cp);
        return;
    }
    for (uint8_t i = 0; i < row->length; i++)
        decompose(out, ucd_decompositions_pool[row->offset + i], compat);
}

static void
reorder(codepoints_t *list)
{
    for (size_t i = 1; i < list->len; i++) {
        uint32_t cp = list->data[i];
        uint8_t ccc = combining_class(cp);
        if (!ccc) continue;
        size_t j = i;
        while (j > 0 && combining_class(list->data[j - 1]) > ccc) {
            list->data[j] = list->data[j - 1];
            j--;
        }
        list->data[j] = cp;
    }
}

static uint32_t
compose_pair(uint32_t first, uint32_t second)
{
    if (first >= HANGUL_L_BASE && first < HANGUL_L_BASE + HANGUL_L_COUNT
        && second >= HANGUL_V_BASE
        && second < HANGUL_V_BASE + HANGUL_V_COUNT)
        return HANGUL_S_BASE
            + ((first - HANGUL_L_BASE) * HANGUL_V_COUNT
               + second - HANGUL_V_BASE) * HANGUL_T_COUNT;
    if (first >= HANGUL_S_BASE && first < HANGUL_S_BASE + HANGUL_S_COUNT
        && (first - HANGUL_S_BASE) % HANGUL_T_COUNT == 0
        && second > HANGUL_T_BASE
        && second < HANGUL_T_BASE + HANGUL_T_COUNT)
        return first + second - HANGUL_T_BASE;
    size_t low = 0;
    size_t high = ucd_compositions_count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        const ucd_composition_t *row = &ucd_compositions[mid];
        if (first < row->first
            || (first == row->first && second < row->second))
            high = mid;
        else if (first > row->first || second > row->second) low = mid + 1;
        else return row->composite;
    }
    return 0;
}

static void
compose(codepoints_t *list)
{
    if (!list->len) return;
    size_t starter = 0;
    int last_class = combining_class(list->data[0]) ? 256 : 0;
    size_t kept = 1;
    for (size_t i = 1; i < list->len; i++) {
        uint32_t cp = list->data[i];
        int cp_class = combining_class(cp);
        uint32_t composite = compose_pair(list->data[starter], cp);
        if (composite && (last_class < cp_class || last_class == 0)) {
            list->data[starter] = composite;
            continue;
        }
        if (!cp_class) starter = kept;
        last_class = cp_class;
        list->data[kept++] = cp;
    }
    list->len = kept;
}

static bool
decode(core_string_t text, codepoints_t *out, bool compat, bool canonical)
{
    uint32_t at = 0;
    while (at < text.u8_bytes) {
        int32_t cp = unicode_utf8_decode(text.data, (uint32_t)text.u8_bytes,
                                         &at);
        if (cp < 0) return false;
        if (canonical) decompose(out, (uint32_t)cp, compat);
        else push(out, (uint32_t)cp);
    }
    return true;
}

static core_string_t
encode(const codepoints_t *list)
{
    core_buffer_t out = {0};
    char bytes[4];
    for (size_t i = 0; i < list->len; i++)
        core_buffer_append(&out, bytes,
                           unicode_utf8_encode(list->data[i], bytes));
    core_string_t result = core_string_dup(core_buffer_string(&out));
    core_buffer_release(&out);
    return result;
}

static bool
normalize_into(core_string_t text, ucd_form_t form, codepoints_t *out)
{
    bool compat = form == UCD_NFKD || form == UCD_NFKC;
    if (!decode(text, out, compat, true)) return false;
    reorder(out);
    if (form == UCD_NFC || form == UCD_NFKC) compose(out);
    return true;
}

core_string_t
ucd_normalize(core_string_t text, ucd_form_t form)
{
    codepoints_t list = core_list_new(uint32_t);
    core_string_t result = normalize_into(text, form, &list)
        ? encode(&list) : (core_string_t){0};
    core_list_free(list);
    return result;
}

core_string_t
ucd_skeleton(core_string_t text)
{
    codepoints_t first = core_list_new(uint32_t);
    if (!normalize_into(text, UCD_NFD, &first)) {
        core_list_free(first);
        return (core_string_t){0};
    }
    codepoints_t mapped = core_list_new(uint32_t);
    for (size_t i = 0; i < first.len; i++) {
        const ucd_mapping_t *row = mapping(ucd_confusables,
                                           ucd_confusables_count,
                                           first.data[i]);
        if (!row) {
            decompose(&mapped, first.data[i], false);
            continue;
        }
        for (uint8_t j = 0; j < row->length; j++)
            decompose(&mapped, ucd_confusables_pool[row->offset + j], false);
    }
    core_list_free(first);
    reorder(&mapped);
    core_string_t result = encode(&mapped);
    core_list_free(mapped);
    return result;
}
