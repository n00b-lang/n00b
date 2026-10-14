#include "syntax.h"

enum {
    NO_BREAK       = -1,
    BLOCK_INDENT   = 4,
    CLAUSE_INDENT  = 2,
    GLUE_BEFORE    = 1,
    GLUE_AFTER     = 2,
    SPACED_COLON   = 4,
};

typedef struct {
    int32_t break_indent;
    uint8_t glue;
} token_style_t;

typedef struct {
    const writ_unit_t *unit;
    writ_str_t         text;
    uint32_t           first;
    uint32_t           end;
    token_style_t     *styles;
} formatter_t;

static uint32_t
token_at(const formatter_t *f, uint32_t offset)
{
    uint32_t low = 0;
    uint32_t high = f->end;
    while (low < high) {
        uint32_t mid = (low + high) / 2;
        if (f->unit->tokens.data[f->first + mid].start < offset) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return low;
}

static writ_str_t
token_text(const formatter_t *f, uint32_t index)
{
    writ_loc_t loc = f->unit->tokens.data[f->first + index];
    return (writ_str_t){f->text.data + loc.start, loc.end - loc.start};
}

static bool
token_is(const formatter_t *f, uint32_t index, const char *text)
{
    return index < f->end && writ_str_eq(token_text(f, index), writ_cstr(text));
}

static bool
is_block(writ_node_kind_t kind)
{
    switch (kind) {
    case WRIT_N_STRUCT:
    case WRIT_N_ENUM:
    case WRIT_N_FLAGS:
    case WRIT_N_WHEN:
    case WRIT_N_ELSE:
    case WRIT_N_REFINE:
    case WRIT_N_IMPL:
    case WRIT_N_INTERFACE:
    case WRIT_N_CONVENTIONS:
    case WRIT_N_CHANNEL:
    case WRIT_N_PROFILE:
    case WRIT_N_BINDING:
        return true;
    default:
        return false;
    }
}

static bool
is_member(writ_node_kind_t parent, writ_node_kind_t child)
{
    switch (parent) {
    case WRIT_N_STRUCT:
        return child == WRIT_N_FIELD;
    case WRIT_N_ENUM:
    case WRIT_N_FLAGS:
        return child == WRIT_N_ENUM_ITEM;
    case WRIT_N_CONVENTIONS:
        return child != WRIT_N_PATTERN;
    case WRIT_N_CHANNEL:
        return child == WRIT_N_CHANNEL_SOURCE || child == WRIT_N_VALID_UNTIL;
    case WRIT_N_BINDING:
        return child != WRIT_N_REF;
    case WRIT_N_WHEN:
        return child != WRIT_N_ELSE;
    default:
        return true;
    }
}

static bool
is_clause_owner(writ_node_kind_t kind)
{
    return kind == WRIT_N_FN || kind == WRIT_N_CALLBACK;
}

static bool
is_signature_part(writ_node_kind_t kind)
{
    return kind == WRIT_N_PARAM || kind == WRIT_N_VARARGS || kind == WRIT_N_SELF_PARAM || kind == WRIT_N_RESULT;
}

static void
glue_range(formatter_t *f, uint32_t from, uint32_t to)
{
    for (uint32_t i = from; i + 1 < to && i + 1 < f->end; i++) {
        f->styles[i].glue |= GLUE_AFTER;
    }
}

static uint32_t
node_first(const formatter_t *f, const writ_node_t *node)
{
    return token_at(f, node->loc.start);
}

static uint32_t
node_end(const formatter_t *f, const writ_node_t *node)
{
    return token_at(f, node->loc.end);
}

static void
mark_line(formatter_t *f, const writ_node_t *node, int32_t indent)
{
    uint32_t first = node_first(f, node);
    if (first < f->end) {
        f->styles[first].break_indent = indent;
    }
}

static void style_node(formatter_t *f, uint32_t id, int32_t indent, bool line_item);

static void
style_block(formatter_t *f, uint32_t id, const writ_node_t *node, int32_t indent)
{
    uint32_t open = node_first(f, node);
    uint32_t end = node_end(f, node);
    while (open < end && !token_is(f, open, "{")) {
        open++;
    }
    int depth = 0;
    uint32_t close = open;
    for (; close < end; close++) {
        depth += token_is(f, close, "{") ? 1 : token_is(f, close, "}") ? -1 : 0;
        if (!depth) {
            break;
        }
    }
    if (close < f->end && close < end) {
        f->styles[close].break_indent = indent;
    }
    uint32_t next_line = open + 1;
    for (uint32_t i = 0; i < node->child_count; i++) {
        uint32_t child = writ_node_child(f->unit, id, i);
        const writ_node_t *part = writ_node(f->unit, child);
        bool member = is_member(node->kind, part->kind) && node_first(f, part) > open && node_first(f, part) < close;
        style_node(f, child, member ? indent + BLOCK_INDENT : indent, false);
        if (member) {
            f->styles[next_line].break_indent = indent + BLOCK_INDENT;
            next_line = node_end(f, part);
            next_line += token_is(f, next_line, ",");
        }
    }
}

static void
style_sequence(formatter_t *f, uint32_t id, const writ_node_t *node)
{
    uint32_t next_line = node_first(f, node);
    for (uint32_t i = 0; i < node->child_count; i++) {
        uint32_t child = writ_node_child(f->unit, id, i);
        style_node(f, child, 0, false);
        if (next_line < f->end) {
            f->styles[next_line].break_indent = 0;
        }
        next_line = node_end(f, writ_node(f->unit, child));
    }
}

static void
style_node(formatter_t *f, uint32_t id, int32_t indent, bool line_item)
{
    const writ_node_t *node = writ_node(f->unit, id);
    if (line_item) {
        mark_line(f, node, indent);
    }
    uint32_t first = node_first(f, node);
    uint32_t end = node_end(f, node);
    switch (node->kind) {
    case WRIT_N_TYPE_POINTER:
    case WRIT_N_TYPE_ARRAY:
    case WRIT_N_EXPR_NEG:
    case WRIT_N_EXPR_BITNOT:
    case WRIT_N_EXPR_DEREF:
        f->styles[first].glue |= GLUE_AFTER;
        break;
    case WRIT_N_TYPE_BYTES:
        f->styles[first].glue |= GLUE_AFTER;
        break;
    case WRIT_N_HEADER:
    case WRIT_N_PLATFORM:
    case WRIT_N_PATTERN:
    case WRIT_N_EXPR_VERSION:
    case WRIT_N_QNAME:
    case WRIT_N_SELF_PARAM:
        glue_range(f, first, end);
        break;
    case WRIT_N_LANG:
        glue_range(f, first + 1, end);
        break;
    case WRIT_N_GENERIC_ARGS:
        f->styles[first].glue |= GLUE_BEFORE | GLUE_AFTER;
        if (end > first && end - 1 < f->end) {
            f->styles[end - 1].glue |= GLUE_BEFORE;
        }
        break;
    case WRIT_N_ENUM:
    case WRIT_N_FLAGS:
        if (token_is(f, first + 2, ":")) {
            f->styles[first + 2].glue |= SPACED_COLON;
        }
        break;
    case WRIT_N_EXTENT_CHOICE:
        for (uint32_t i = first; i < end; i++) {
            if (token_is(f, i, ":")) {
                f->styles[i].glue |= SPACED_COLON;
            }
        }
        break;
    default:
        break;
    }
    if (is_block(node->kind)) {
        style_block(f, id, node, indent);
        return;
    }
    if (node->kind == WRIT_N_FILE) {
        style_sequence(f, id, node);
        return;
    }
    for (uint32_t i = 0; i < node->child_count; i++) {
        uint32_t child = writ_node_child(f->unit, id, i);
        const writ_node_t *part = writ_node(f->unit, child);
        bool clause = is_clause_owner(node->kind) && !is_signature_part(part->kind);
        style_node(f, child, clause ? indent + CLAUSE_INDENT : indent, clause);
    }
}

static bool
word_like(writ_str_t text)
{
    char c = text.length ? text.data[0] : 0;
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c >= '0' && c <= '9') || c == '\'';
}

static bool
space_between(const formatter_t *f, uint32_t previous, uint32_t next)
{
    writ_str_t a = token_text(f, previous);
    writ_str_t b = token_text(f, next);
    if ((f->styles[previous].glue & GLUE_AFTER) || (f->styles[next].glue & GLUE_BEFORE)) {
        return false;
    }
    static const char *const tight_before[] = {",", ";", ")", "]", ".", "::"};
    static const char *const tight_after[] = {"(", "[", ".", "::", "@", "&", "~"};
    for (size_t i = 0; i < sizeof(tight_before) / sizeof(tight_before[0]); i++) {
        if (writ_str_eq(b, writ_cstr(tight_before[i]))) {
            return false;
        }
    }
    for (size_t i = 0; i < sizeof(tight_after) / sizeof(tight_after[0]); i++) {
        if (writ_str_eq(a, writ_cstr(tight_after[i]))) {
            return false;
        }
    }
    if (writ_str_eq(b, writ_cstr(":"))) {
        return (f->styles[next].glue & SPACED_COLON) != 0;
    }
    if (writ_str_eq(b, writ_cstr("("))) {
        return !(word_like(a) || writ_str_eq(a, writ_cstr(")")));
    }
    return true;
}

typedef struct {
    core_buffer_t *out;
    int32_t        indent;
    bool           at_line_start;
} writer_t;

static void
newline(writer_t *w, int32_t indent)
{
    core_buffer_putc(w->out, '\n');
    w->indent = indent;
    w->at_line_start = true;
}

static void
put(writer_t *w, writ_str_t text)
{
    if (w->at_line_start) {
        for (int32_t i = 0; i < w->indent; i++) {
            core_buffer_putc(w->out, ' ');
        }
        w->at_line_start = false;
    }
    core_buffer_append(w->out, text.data, text.length);
}

static void
gap(const formatter_t *f, writer_t *w, size_t from, size_t to, int32_t next_indent, bool first)
{
    bool seen_newline = first;
    uint32_t newlines = 0;
    for (size_t at = from; at < to; at++) {
        char c = f->text.data[at];
        if (c == '\n') {
            newlines++;
            seen_newline = true;
            continue;
        }
        if (c != '#') {
            continue;
        }
        size_t end = at;
        while (end < to && f->text.data[end] != '\n') {
            end++;
        }
        writ_str_t comment = {f->text.data + at, end - at};
        while (comment.length && (comment.data[comment.length - 1] == ' ' || comment.data[comment.length - 1] == '\r')) {
            comment.length--;
        }
        if (!seen_newline) {
            core_buffer_puts(w->out, "  ");
            core_buffer_append(w->out, comment.data, comment.length);
        } else {
            if (!w->at_line_start) {
                newline(w, next_indent);
            }
            if (newlines > 1 && w->out->byte_len) {
                core_buffer_putc(w->out, '\n');
            }
            w->indent = next_indent;
            put(w, comment);
        }
        newline(w, next_indent);
        newlines = 0;
        at = end - 1;
    }
    if (newlines > 1 && w->out->byte_len && !first) {
        if (!w->at_line_start) {
            core_buffer_putc(w->out, '\n');
        }
        core_buffer_putc(w->out, '\n');
        w->at_line_start = true;
    }
}

static writ_str_t
format_root(const writ_unit_t *unit, const writ_unit_root_t *root)
{
    writ_session_t *session = unit->session;
    formatter_t f = {
        .unit  = unit,
        .text  = writ_source_at(session, root->source)->text,
        .first = root->first_token,
        .end   = root->last_token - root->first_token,
    };
    f.styles = writ_alloc(session, sizeof(token_style_t) * (f.end + 1));
    for (uint32_t i = 0; i < f.end; i++) {
        f.styles[i] = (token_style_t){NO_BREAK, 0};
    }
    style_node(&f, root->root, 0, false);
    writer_t w = {.out = core_buffer_empty(), .at_line_start = true};
    int32_t item_indent = 0;
    size_t cursor = 0;
    for (uint32_t i = 0; i < f.end; i++) {
        writ_loc_t loc = unit->tokens.data[f.first + i];
        int32_t breaks = f.styles[i].break_indent;
        gap(&f, &w, cursor, loc.start, breaks != NO_BREAK ? breaks : item_indent + BLOCK_INDENT, i == 0);
        if (breaks != NO_BREAK) {
            item_indent = breaks;
            if (!w.at_line_start) {
                newline(&w, breaks);
            }
            w.indent = breaks;
        } else if (i && !w.at_line_start && space_between(&f, i - 1, i)) {
            core_buffer_putc(w.out, ' ');
        }
        put(&w, token_text(&f, i));
        cursor = loc.end;
    }
    gap(&f, &w, cursor, f.text.length, item_indent, false);
    if (!w.at_line_start) {
        core_buffer_putc(w.out, '\n');
    }
    return writ_str_from_buffer(session, w.out);
}

writ_status_t
writ_format_unit(const writ_unit_t *unit, const writ_output_t *output)
{
    if (!unit || !output || !output->write_file) {
        return WRIT_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < unit->roots.len; i++) {
        const writ_unit_root_t *root = &unit->roots.data[i];
        writ_status_t status = output->write_file(output->context, writ_source_at(unit->session, root->source)->path,
                                                  format_root(unit, root));
        if (status != WRIT_OK) {
            return status;
        }
    }
    return WRIT_OK;
}
