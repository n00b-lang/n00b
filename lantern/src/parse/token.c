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

        copy->text      = core_string_dup(item->text);
        copy->kind      = item->kind;
        copy->owns_text = true;
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

        core_free(t);
        t = next;
    }
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
    copy->leading_trivia  = clone_trivia(token->leading_trivia);
    copy->trailing_trivia = clone_trivia(token->trailing_trivia);
    copy->owns_value      = core_option_is_set(copy->value);
    copy->owns_literal_type = core_option_is_set(copy->literal_type);
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
