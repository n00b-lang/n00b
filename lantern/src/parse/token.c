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

parse_token_info_t *
parse_token_clone_for_tree(const parse_token_info_t *token)
{
    if (!token) {
        return nullptr;
    }

    parse_token_info_t *copy = core_alloc(parse_token_info_t);

    *copy = *token;
    copy->value = core_option_is_set(token->value)
                      ? core_option_set(core_string_t,
                                        core_string_dup(
                                            core_option_get(token->value)))
                      : core_option_none(core_string_t);
    copy->file = core_option_is_set(token->file)
                     ? core_option_set(core_string_t,
                                       core_string_dup(
                                           core_option_get(token->file)))
                     : core_option_none(core_string_t);
    copy->literal_type = core_option_is_set(token->literal_type)
                             ? core_option_set(
                                   core_string_t,
                                   core_string_dup(
                                       core_option_get(token->literal_type)))
                             : core_option_none(core_string_t);
    copy->postfix_modifier = core_option_is_set(token->postfix_modifier)
                                 ? core_option_set(core_string_t,
                                     core_string_dup(core_option_get(
                                         token->postfix_modifier)))
                                 : core_option_none(core_string_t);
    copy->leading_trivia  = clone_trivia(token->leading_trivia);
    copy->trailing_trivia = clone_trivia(token->trailing_trivia);
    copy->owns_value      = core_option_is_set(copy->value);
    copy->owns_literal_type = core_option_is_set(copy->literal_type);
    copy->owns_postfix_modifier = core_option_is_set(copy->postfix_modifier);
    copy->owns_file       = core_option_is_set(copy->file);
    copy->tree_owned      = true;

    return copy;
}

void
parse_token_free_from_tree(parse_token_info_t *token)
{
    if (!token || !token->tree_owned) {
        return;
    }

    if (token->owns_value && core_option_is_set(token->value)) {
        core_free(core_option_get(token->value).data);
    }

    if (token->owns_file && core_option_is_set(token->file)) {
        core_free(core_option_get(token->file).data);
    }

    if (token->owns_literal_type
        && core_option_is_set(token->literal_type)) {
        core_free(core_option_get(token->literal_type).data);
    }
    if (token->owns_postfix_modifier
        && core_option_is_set(token->postfix_modifier)) {
        core_free(core_option_get(token->postfix_modifier).data);
    }

    parse_trivia_free(token->leading_trivia);
    parse_trivia_free(token->trailing_trivia);
    core_free(token);
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
