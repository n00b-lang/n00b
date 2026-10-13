#include <stdint.h>
#include <string.h>

#include "preprocessor.h"
#include "rich_string.h"

typedef enum {
    RICH_RECORD_STYLE,
    RICH_RECORD_POSITIONAL,
    RICH_RECORD_NAMED,
} rich_record_kind_t;

typedef struct {
    rich_record_kind_t kind;
    uint32_t offset;
    uint32_t end;
    bool open_end;
    core_string_t tag;
    uint32_t position;
    core_string_t name;
    core_string_t format;
    bool has_format;
} rich_record_t;

typedef struct {
    core_string_t tag;
    uint32_t offset;
} rich_open_tag_t;
core_list_decl(rich_record_t);
core_list_decl(rich_open_tag_t);

static bool
string_is(core_string_t value, const char *text)
{
    size_t length = strlen(text);
    return value.u8_bytes == length
        && !memcmp(value.data, text, length);
}

static bool
tags_match(core_string_t a, core_string_t b)
{
    static const char *const groups[][5] = {
        {"b", "bold", nullptr},
        {"i", "italic", nullptr},
        {"u", "underline", nullptr},
        {"uu", "2u", nullptr},
        {"st", "strike", "strikethrough", nullptr},
        {"r", "reverse", nullptr},
        {"dim", "faint", nullptr},
        {"upper", "up", nullptr},
        {"lower", "l", nullptr},
        {"title", "t", nullptr},
        {"caps", "allcaps", nullptr},
    };
    if (core_string_eq(a, b)) return true;
    for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
        bool matches_a = false, matches_b = false;
        for (size_t j = 0; groups[i][j]; j++) {
            matches_a |= string_is(a, groups[i][j]);
            matches_b |= string_is(b, groups[i][j]);
        }
        if (matches_a && matches_b) return true;
    }
    return false;
}

static bool
append_decoded_text(core_string_t source, size_t start, size_t length,
                    core_buffer_t *text)
{
    core_buffer_t *quoted = core_buffer_empty();
    if (!quoted) return false;
    core_buffer_putc(quoted, '"');
    size_t end = start + length;
    for (size_t i = start; i < end;) {
        if (source.data[i] == '\\' && i + 1 < end) {
            if (i + 2 < end && (uint8_t)source.data[i + 1] == 0xc2
                && ((uint8_t)source.data[i + 2] == 0xab
                    || (uint8_t)source.data[i + 2] == 0xbb)) {
                core_buffer_puts(quoted,
                    (uint8_t)source.data[i + 2] == 0xab
                        ? "\\u00AB" : "\\u00BB");
                i += 3;
                continue;
            }
            if (source.data[i + 1] == '[') {
                core_buffer_puts(quoted, "\\x5B");
                i += 2;
                continue;
            }
            core_buffer_append(quoted, source.data + i, 2);
            i += 2;
            continue;
        }
        core_buffer_putc(quoted, source.data[i++]);
    }
    core_buffer_putc(quoted, '"');
    core_string_t spelling = core_string_borrow(quoted->data,
                                                quoted->byte_len);
    core_buffer_t *decoded = core_buffer_empty();
    bool ok = decoded && n00bb_decode_string_literal(spelling, decoded)
        && decoded->byte_len <= UINT32_MAX - text->byte_len;
    if (ok) core_buffer_append(text, decoded->data, decoded->byte_len);
    core_buffer_free(decoded);
    core_buffer_free(quoted);
    return ok;
}

static void
append_json_string(core_buffer_t *output, core_string_t value)
{
    static const char hex[] = "0123456789abcdef";
    core_buffer_putc(output, '"');
    for (size_t i = 0; i < value.u8_bytes; i++) {
        uint8_t c = (uint8_t)value.data[i];
        if (c == '"' || c == '\\') {
            core_buffer_putc(output, '\\');
            core_buffer_putc(output, (char)c);
        }
        else if (c < 0x20) {
            core_buffer_puts(output, "\\u00");
            core_buffer_putc(output, hex[c >> 4]);
            core_buffer_putc(output, hex[c & 0xf]);
        }
        else core_buffer_putc(output, (char)c);
    }
    core_buffer_putc(output, '"');
}

static void
append_record_json(core_buffer_t *output, const rich_record_t *record)
{
    if (record->kind == RICH_RECORD_STYLE) {
        core_buffer_puts(output, "{\"tag\":");
        append_json_string(output, record->tag);
        core_buffer_printf(output, ",\"start\":%u,\"end\":",
                           record->offset);
        if (record->open_end) core_buffer_puts(output, "null}");
        else core_buffer_printf(output, "%u}", record->end);
        return;
    }
    core_buffer_printf(output, "{\"offset\":%u,", record->offset);
    if (record->kind == RICH_RECORD_POSITIONAL)
        core_buffer_printf(output, "\"position\":%u", record->position);
    else {
        core_buffer_puts(output, "\"name\":");
        append_json_string(output, record->name);
    }
    if (record->has_format) {
        core_buffer_puts(output, ",\"format\":");
        append_json_string(output, record->format);
    }
    core_buffer_putc(output, '}');
}

static bool
parse_substitution(core_string_t body, uint32_t offset,
                   uint32_t *next_auto, bool *saw_auto, bool *saw_explicit,
                   rich_record_t *record, n00bb_rich_string_error_t *error)
{
    size_t selector_end = body.u8_bytes;
    bool has_format = false;
    for (size_t i = 1; i < body.u8_bytes; i++) {
        if (body.data[i] == ':') {
            selector_end = i;
            has_format = true;
            break;
        }
    }
    core_string_t selector = core_string_borrow(body.data + 1,
                                                selector_end - 1);
    core_string_t format = has_format
        ? core_string_borrow(body.data + selector_end + 1,
                             body.u8_bytes - selector_end - 1)
        : (core_string_t){0};
    bool digits = selector.u8_bytes != 0;
    uint64_t position = 0;
    for (size_t i = 0; i < selector.u8_bytes; i++) {
        unsigned char c = selector.data[i];
        if (c < '0' || c > '9') {
            digits = false;
            break;
        }
        position = position * 10 + (c - '0');
        if (position > UINT32_MAX) {
            *error = N00BB_RICH_STRING_INDEX_OVERFLOW;
            return false;
        }
    }
    if (!selector.u8_bytes || digits) {
        if (digits && *saw_auto) {
            *error = N00BB_RICH_STRING_MIXED_NUMBERING;
            return false;
        }
        if (!digits && *saw_explicit) {
            *error = N00BB_RICH_STRING_MIXED_NUMBERING;
            return false;
        }
        if (digits) *saw_explicit = true;
        else {
            *saw_auto = true;
            position = (*next_auto)++;
        }
        record->kind = RICH_RECORD_POSITIONAL;
        record->position = (uint32_t)position;
    }
    else {
        record->kind = RICH_RECORD_NAMED;
        record->name = selector;
    }
    record->offset = offset;
    record->has_format = has_format;
    record->format = format;
    return true;
}

static bool
close_tag(core_string_t tag, uint32_t offset,
          core_list_t(rich_open_tag_t) *open,
          core_list_t(rich_record_t) *records)
{
    for (size_t i = open->len; i > 0; i--) {
        rich_open_tag_t *item = &open->data[i - 1];
        if (!tags_match(item->tag, tag)) continue;
        core_list_push(*records, ((rich_record_t){
            .kind = RICH_RECORD_STYLE,
            .offset = item->offset,
            .end = offset,
            .tag = item->tag,
        }));
        core_list_delete(*open, i - 1);
        return true;
    }
    return true;
}

static void
close_all(uint32_t offset, core_list_t(rich_open_tag_t) *open,
          core_list_t(rich_record_t) *records)
{
    for (size_t i = 0; i < open->len; i++)
        core_list_push(*records, ((rich_record_t){
            .kind = RICH_RECORD_STYLE,
            .offset = open->data[i].offset,
            .end = offset,
            .tag = open->data[i].tag,
        }));
    open->len = 0;
}

static void
finish_open_tags(core_list_t(rich_open_tag_t) *open,
                 core_list_t(rich_record_t) *records)
{
    for (size_t i = 0; i < open->len; i++)
        core_list_push(*records, ((rich_record_t){
            .kind = RICH_RECORD_STYLE,
            .offset = open->data[i].offset,
            .open_end = true,
            .tag = open->data[i].tag,
        }));
    open->len = 0;
}

static bool
parse_tag(core_string_t body, uint32_t offset, uint32_t *next_auto,
          bool *saw_auto, bool *saw_explicit,
          core_list_t(rich_open_tag_t) *open,
          core_list_t(rich_record_t) *records,
          n00bb_rich_string_error_t *error)
{
    if (!body.u8_bytes) return true;
    if (body.data[0] == '#') {
        rich_record_t record = {0};
        if (!parse_substitution(body, offset, next_auto, saw_auto,
                                saw_explicit, &record, error)) return false;
        core_list_push(*records, record);
        return true;
    }
    if (body.data[0] == '/') {
        if (body.u8_bytes == 1) {
            close_all(offset, open, records);
            return true;
        }
        return close_tag(core_string_borrow(body.data + 1,
                body.u8_bytes - 1), offset, open, records);
    }
    core_list_push(*open, ((rich_open_tag_t){body, offset}));
    return true;
}

bool
n00bb_rich_string_parse(core_string_t raw, core_buffer_t **text_out,
                        core_buffer_t **metadata_out,
                        n00bb_rich_string_error_t *error)
{
    if (error) *error = N00BB_RICH_STRING_INVALID;
    if (!text_out || !metadata_out || !raw.data || raw.u8_bytes < 2
        || raw.data[0] != '"' || raw.data[raw.u8_bytes - 1] != '"')
        return false;
    core_buffer_t *text = core_buffer_empty();
    core_list_t(rich_open_tag_t) open = core_list_new(rich_open_tag_t);
    core_list_t(rich_record_t) records = core_list_new(rich_record_t);
    uint32_t next_auto = 0;
    bool saw_auto = false, saw_explicit = false, ok = text != nullptr;
    n00bb_rich_string_error_t failure = N00BB_RICH_STRING_BAD_ESCAPE;
    size_t content_end = raw.u8_bytes - 1;
    size_t i = 1, text_start = 1;
    while (ok && i < content_end) {
        if (raw.data[i] == '\\') {
            i += i + 1 < content_end ? 2 : 1;
            continue;
        }
        size_t body_start = 0, close_start = 0, close_size = 0;
        if (i + 1 < content_end && (uint8_t)raw.data[i] == 0xc2
            && (uint8_t)raw.data[i + 1] == 0xab) {
            body_start = i + 2;
            close_size = 2;
            for (close_start = body_start; close_start + 1 < content_end;
                 close_start++) {
                if ((uint8_t)raw.data[close_start] == 0xc2
                    && (uint8_t)raw.data[close_start + 1] == 0xbb) break;
            }
            if (close_start + 1 >= content_end) {
                i += 2;
                continue;
            }
        }
        else if (i + 1 < content_end && raw.data[i] == '['
                 && raw.data[i + 1] == '|') {
            body_start = i + 2;
            close_size = 2;
            for (close_start = body_start; close_start + 1 < content_end;
                 close_start++)
                if (raw.data[close_start] == '|' 
                    && raw.data[close_start + 1] == ']') break;
            if (close_start + 1 >= content_end) {
                failure = N00BB_RICH_STRING_UNCLOSED_ASCII_TAG;
                ok = false;
                break;
            }
        }
        else if (i + 2 < content_end && raw.data[i] == '['
                 && raw.data[i + 1] == ':' && raw.data[i + 2] == '#') {
            body_start = i + 2;
            close_size = 2;
            for (close_start = body_start; close_start + 1 < content_end;
                 close_start++)
                if (raw.data[close_start] == ':'
                    && raw.data[close_start + 1] == ']') break;
            if (close_start + 1 >= content_end) {
                failure = N00BB_RICH_STRING_UNCLOSED_ASCII_TAG;
                ok = false;
                break;
            }
        }
        else {
            i++;
            continue;
        }
        if (!append_decoded_text(raw, text_start, i - text_start, text)) {
            ok = false;
            break;
        }
        core_string_t body = core_string_borrow(raw.data + body_start,
                                                close_start - body_start);
        if (text->byte_len > UINT32_MAX
            || !parse_tag(body, (uint32_t)text->byte_len, &next_auto,
                          &saw_auto, &saw_explicit, &open, &records,
                          &failure)) {
            ok = false;
            break;
        }
        i = close_start + close_size;
        text_start = i;
    }
    if (ok && !append_decoded_text(raw, text_start,
                                   content_end - text_start, text)) {
        ok = false;
    }
    if (ok) finish_open_tags(&open, &records);
    core_buffer_t *metadata = ok && records.len ? core_buffer_empty()
        : nullptr;
    if (ok && records.len && !metadata) ok = false;
    if (ok && metadata) {
        core_buffer_printf(metadata,
            "{\"version\":1,\"records\":[");
        for (size_t r = 0; r < records.len; r++) {
            if (r) core_buffer_putc(metadata, ',');
            append_record_json(metadata, &records.data[r]);
        }
        core_buffer_puts(metadata, "]}");
    }
    if (!ok && error) *error = failure;
    core_list_free(open);
    core_list_free(records);
    if (!ok) {
        core_buffer_free(text);
        core_buffer_free(metadata);
        return false;
    }
    *text_out = text;
    *metadata_out = metadata;
    return true;
}
