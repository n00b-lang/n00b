#include "parse/token.h"
#include "lib/alloc.h"
#include <assert.h>

static parse_trivia_t *
clone_trivia(const parse_trivia_t *source)
{
    parse_trivia_t *head = nullptr;
    parse_trivia_t **next = &head;

    for (const parse_trivia_t *item = source; item; item = item->next) {
        parse_trivia_t *copy = core_alloc(parse_trivia_t);

        *copy = *item;
        copy->text = core_string_dup(item->text);
        copy->region_name = item->region_name.data
            ? core_string_dup(item->region_name) : (core_string_t){0};
        copy->regions = nullptr;
        parse_trivia_region_link_t **region_tail = &copy->regions;
        for (const parse_trivia_region_link_t *link = item->regions;
             link; link = link->next) {
            *region_tail = core_alloc(parse_trivia_region_link_t);
            **region_tail = *link;
            (*region_tail)->next = nullptr;
            region_tail = &(*region_tail)->next;
        }
        copy->owns_text = true;
        copy->owns_region_name = copy->region_name.data != nullptr;
        copy->next = nullptr;
        *next           = copy;
        next            = &copy->next;
    }

    return head;
}

void
parse_trivia_free(parse_trivia_t *t)
{
    while (t) {
        parse_trivia_t *next = t->next;

        if (t->owns_text) {
            core_free(t->text.data);
        }
        if (t->owns_region_name) {
            core_free(t->region_name.data);
        }
        while (t->regions) {
            parse_trivia_region_link_t *next_region = t->regions->next;
            core_free(t->regions);
            t->regions = next_region;
        }

        core_free(t);
        t = next;
    }
}

size_t
parse_trivia_region_count(const parse_trivia_t *trivia)
{
    size_t result = 0;
    for (const parse_trivia_region_link_t *link = trivia ? trivia->regions : nullptr;
         link; link = link->next) {
        result++;
    }
    return result;
}

uint64_t
parse_trivia_region_at(const parse_trivia_t *trivia, size_t index)
{
    const parse_trivia_region_link_t *link = trivia ? trivia->regions : nullptr;
    while (link && index) {
        link = link->next;
        index--;
    }
    return link ? link->region_id : 0;
}

bool
parse_trivia_in_region(const parse_trivia_t *trivia, uint64_t region_id)
{
    for (const parse_trivia_region_link_t *link = trivia ? trivia->regions : nullptr;
         link; link = link->next) {
        if (link->region_id == region_id) {
            return true;
        }
    }
    return false;
}

static core_option_t(core_string_t)
dup_option(core_option_t(core_string_t) value)
{
    return core_option_is_set(value)
        ? core_option_set(core_string_t,
                          core_string_dup(core_option_get(value)))
        : core_option_none(core_string_t);
}

static void
free_option(core_option_t(core_string_t) *value, bool *owned)
{
    if (*owned && core_option_is_set(*value)) {
        core_free(core_option_get(*value).data);
    }
    *value = core_option_none(core_string_t);
    *owned = false;
}

static void
store_option(core_option_t(core_string_t) *slot, bool *owned,
             core_option_t(core_string_t) value, bool take)
{
    free_option(slot, owned);
    *slot = value;
    *owned = take && core_option_is_set(value);
}

parse_token_info_t *
parse_token_new(void)
{
    return core_alloc(parse_token_info_t);
}

parse_token_extras_t *
parse_token_extras_for_update(parse_token_info_t *token)
{
    if (!token->extras) {
        token->extras = core_alloc(parse_token_extras_t);
    }
    return token->extras;
}

void
parse_token_free_contents(parse_token_info_t *token)
{
    if (!token) {
        return;
    }

    parse_token_extras_t *extras = token->extras;

    if (extras) {
        free_option(&extras->value, &extras->owns_value);
        free_option(&extras->file, &extras->owns_file);
        free_option(&extras->literal_type, &extras->owns_literal_type);
        free_option(&extras->postfix_modifier,
                    &extras->owns_postfix_modifier);
        parse_trivia_free(extras->leading_trivia);
        parse_trivia_free(extras->trailing_trivia);
        core_free(extras);
        token->extras = nullptr;
    }

    if (token->fields.holds_source) {
        parse_source_release(token->fields.source);
        token->fields.holds_source = false;
    }
}

void
parse_token_free(parse_token_info_t *token)
{
    parse_token_free_contents(token);
    core_free(token);
}

void
parse_token_set_source(parse_token_info_t *token, uint32_t source)
{
    if (token->fields.value_from_source && token->fields.source != source) {
        core_option_t(core_string_t) value = parse_token_value(token);
        parse_token_set_value(token, dup_option(value), true);
    }
    if (source) {
        parse_source_retain(source);
    }
    if (token->fields.holds_source) {
        parse_source_release(token->fields.source);
    }
    token->fields.source = source;
    token->fields.holds_source = source != 0;
}

parse_token_info_t *
parse_token_copy(const parse_token_info_t *token, bool with_trivia)
{
    if (!token) {
        return nullptr;
    }

    parse_token_info_t *copy = parse_token_new();

    copy->fields = token->fields;
    copy->fields.holds_source = false;
    parse_token_set_source(copy, token->fields.source);

    if (token->extras) {
        parse_token_extras_t *from = token->extras;
        parse_token_extras_t *to = parse_token_extras_for_update(copy);

        *to = *from;
        to->value = dup_option(from->value);
        to->file = dup_option(from->file);
        to->literal_type = dup_option(from->literal_type);
        to->postfix_modifier = dup_option(from->postfix_modifier);
        to->owns_value = core_option_is_set(to->value);
        to->owns_file = core_option_is_set(to->file);
        to->owns_literal_type = core_option_is_set(to->literal_type);
        to->owns_postfix_modifier = core_option_is_set(to->postfix_modifier);
        to->leading_trivia = with_trivia
            ? clone_trivia(from->leading_trivia) : nullptr;
        to->trailing_trivia = with_trivia
            ? clone_trivia(from->trailing_trivia) : nullptr;
    }

    return copy;
}

core_option_t(core_string_t)
parse_token_value(const parse_token_info_t *token)
{
    if (token->fields.value_from_source) {
        core_string_t text = parse_source_value(token->fields.source,
                                                token->fields.value_offset);

        if (!text.data) {
            return core_option_none(core_string_t);
        }
        core_string_t value = {0};
        if (text.u8_bytes) {
            value = text;
        }
        return core_option_set(core_string_t, value);
    }
    return token->extras ? token->extras->value
                         : core_option_none(core_string_t);
}

void
parse_token_set_span(parse_token_info_t *token, size_t start, size_t end)
{
    if (token->fields.value_from_source
        && end - start != token->fields.end_byte - token->fields.start_byte) {
        core_option_t(core_string_t) value = parse_token_value(token);
        parse_token_set_value(token, dup_option(value), true);
    }
    token->fields.start_byte = (uint32_t)start;
    token->fields.end_byte = (uint32_t)end;
}

bool
parse_token_has_value(const parse_token_info_t *token)
{
    return token->fields.value_from_source
        || (token->extras && core_option_is_set(token->extras->value));
}

void
parse_token_set_value(parse_token_info_t *token,
                      core_option_t(core_string_t) value, bool owned)
{
    token->fields.value_from_source = false;
    if (!token->extras && !core_option_is_set(value)) {
        return;
    }
    parse_token_extras_t *extras = parse_token_extras_for_update(token);
    store_option(&extras->value, &extras->owns_value, value, owned);
}

bool
parse_token_use_source_value(parse_token_info_t *token)
{
    uint32_t offset = 0;

    if (!parse_source_store_value(token->fields.source,
                                  token->fields.start_byte,
                                  token->fields.end_byte, &offset)) {
        return false;
    }
    if (token->extras) {
        free_option(&token->extras->value, &token->extras->owns_value);
    }
    token->fields.value_offset = offset;
    token->fields.value_from_source = true;
    return true;
}

core_option_t(core_string_t)
parse_token_file(const parse_token_info_t *token)
{
    if (token->extras && core_option_is_set(token->extras->file)) {
        return token->extras->file;
    }
    return parse_source_name(token->fields.source);
}

void
parse_token_set_file(parse_token_info_t *token,
                     core_option_t(core_string_t) file, bool owned)
{
    if (!token->extras && !core_option_is_set(file)) {
        return;
    }
    parse_token_extras_t *extras = parse_token_extras_for_update(token);
    store_option(&extras->file, &extras->owns_file, file, owned);
}

static bool
derived_position(const parse_token_info_t *token, size_t offset,
                 uint32_t *line, uint32_t *column)
{
    return token->fields.source
        && parse_source_position(token->fields.source, offset, line, column);
}

uint32_t
parse_token_line(const parse_token_info_t *token)
{
    if (token->extras && token->extras->has_position) {
        return token->extras->line;
    }
    uint32_t line = 0;
    derived_position(token, token->fields.start_byte, &line, nullptr);
    return line;
}

uint32_t
parse_token_column(const parse_token_info_t *token)
{
    if (token->extras && token->extras->has_position) {
        return token->extras->column;
    }
    uint32_t column = 0;
    derived_position(token, token->fields.start_byte, nullptr, &column);
    return column;
}

uint32_t
parse_token_endline(const parse_token_info_t *token)
{
    if (token->extras && token->extras->has_position) {
        return token->extras->endline;
    }
    uint32_t line = 0;
    derived_position(token, token->fields.end_byte, &line, nullptr);
    return line;
}

uint32_t
parse_token_endcol(const parse_token_info_t *token)
{
    if (token->extras && token->extras->has_position) {
        return token->extras->endcol;
    }
    uint32_t column = 0;
    derived_position(token, token->fields.end_byte, nullptr, &column);
    return column;
}

void
parse_token_set_position(parse_token_info_t *token, uint32_t line,
                         uint32_t column, uint32_t endline, uint32_t endcol)
{
    parse_token_extras_t *extras = parse_token_extras_for_update(token);
    extras->line = line;
    extras->column = column;
    extras->endline = endline;
    extras->endcol = endcol;
    extras->has_position = true;
}

void
parse_token_copy_location(parse_token_info_t *token,
                          const parse_token_info_t *from)
{
    parse_token_set_source(token, from->fields.source);
    token->fields.start_byte = from->fields.start_byte;
    token->fields.end_byte = from->fields.end_byte;
    if (from->extras && from->extras->has_position) {
        parse_token_set_position(token, from->extras->line,
                                 from->extras->column,
                                 from->extras->endline,
                                 from->extras->endcol);
    }
    else if (token->extras) {
        token->extras->has_position = false;
    }
    if (from->extras && core_option_is_set(from->extras->file)) {
        parse_token_set_file(token, dup_option(from->extras->file), true);
    }
    else {
        parse_token_set_file(token, core_option_none(core_string_t), false);
    }
}

void
parse_token_set_literal_type(parse_token_info_t *token,
                             core_option_t(core_string_t) value, bool owned)
{
    if (!token->extras && !core_option_is_set(value)) {
        return;
    }
    parse_token_extras_t *extras = parse_token_extras_for_update(token);
    store_option(&extras->literal_type, &extras->owns_literal_type, value,
                 owned);
}

void
parse_token_set_postfix_modifier(parse_token_info_t *token,
                                 core_option_t(core_string_t) value,
                                 bool owned)
{
    if (!token->extras && !core_option_is_set(value)) {
        return;
    }
    parse_token_extras_t *extras = parse_token_extras_for_update(token);
    store_option(&extras->postfix_modifier, &extras->owns_postfix_modifier,
                 value, owned);
}

void
parse_token_set_user_info(parse_token_info_t *token, void *info)
{
    if (!token->extras && !info) {
        return;
    }
    parse_token_extras_for_update(token)->user_info = info;
}

void
parse_token_set_leading_trivia(parse_token_info_t *token,
                               parse_trivia_t *trivia)
{
    if (!token->extras && !trivia) {
        return;
    }
    parse_token_extras_for_update(token)->leading_trivia = trivia;
}

void
parse_token_set_trailing_trivia(parse_token_info_t *token,
                                parse_trivia_t *trivia)
{
    if (!token->extras && !trivia) {
        return;
    }
    parse_token_extras_for_update(token)->trailing_trivia = trivia;
}

void
parse_token_set_content_span(parse_token_info_t *token, size_t start,
                             size_t end)
{
    parse_token_extras_t *extras = parse_token_extras_for_update(token);
    extras->content_start_byte = start;
    extras->content_end_byte = end;
    extras->has_content_span = true;
}

void
parse_token_clear_content_span(parse_token_info_t *token)
{
    if (token->extras) {
        token->extras->has_content_span = false;
        token->extras->content_start_byte = 0;
        token->extras->content_end_byte = 0;
    }
}

void
parse_token_set_label_span(parse_token_info_t *token, size_t start,
                           size_t end)
{
    parse_token_extras_t *extras = parse_token_extras_for_update(token);
    extras->label_start_byte = start;
    extras->label_end_byte = end;
    extras->has_label_span = true;
}

void
parse_token_clear_label_span(parse_token_info_t *token)
{
    if (token->extras) {
        token->extras->has_label_span = false;
        token->extras->label_start_byte = 0;
        token->extras->label_end_byte = 0;
    }
}

void
parse_token_set_layout(parse_token_info_t *token, uint32_t depth,
                       uint32_t column)
{
    parse_token_extras_t *extras = parse_token_extras_for_update(token);
    extras->layout_depth = depth;
    extras->layout_column = column;
    extras->has_layout_info = true;
}

int32_t
parse_token_list_build_ptrs(core_list_t(parse_token_info_t) *tl, parse_token_info_ptr_t **out)
{
    assert(tl);
    assert(out);

    int32_t n = (int32_t)tl->len;

    parse_token_info_ptr_t *ptrs = core_alloc_array(parse_token_info_ptr_t, n);

    for (int32_t i = 0; i < n; i++) {
        ptrs[i] = &tl->data[i];
    }

    *out = ptrs;
    return n;
}
