#include "diag/text.h"

#include <string.h>

static void
put_items(core_buffer_t *out, const core_text_catalog_t *catalog,
          const core_text_arg_t *arg, uint32_t separator, uint32_t last,
          bool quoted)
{
    for (size_t i = 0; i < arg->count; i++) {
        if (i)
            core_text_expand(out, catalog,
                             i + 1 == arg->count ? last : separator,
                             CORE_TEXT_NO_ARGS);
        if (quoted)
            core_text_expand(out, catalog, catalog->quoted,
                             CORE_TEXT_ARGS(CORE_TEXT_STRING(arg->items[i])));
        else core_buffer_append_string(out, arg->items[i]);
    }
}

static void
put_argument(core_buffer_t *out, const core_text_catalog_t *catalog,
             const core_text_arg_t *arg)
{
    switch (arg->kind) {
    case CORE_TEXT_ARG_TEXT:
        core_buffer_append_string(out, arg->text);
        break;
    case CORE_TEXT_ARG_NUMBER:
        core_buffer_printf(out, "%lld", (long long)arg->number);
        break;
    case CORE_TEXT_ARG_ERRNO:
        core_buffer_printf(out, "%s", strerror((int)arg->number));
        break;
    case CORE_TEXT_ARG_PHRASE:
        core_text_expand(out, catalog, arg->phrase, CORE_TEXT_NO_ARGS);
        break;
    case CORE_TEXT_ARG_NAMES:
        put_items(out, catalog, arg, catalog->list_separator,
                  catalog->list_and, true);
        break;
    case CORE_TEXT_ARG_CHOICES:
        put_items(out, catalog, arg, catalog->list_separator,
                  catalog->list_or, true);
        break;
    case CORE_TEXT_ARG_LIST:
        put_items(out, catalog, arg, catalog->list_separator,
                  catalog->list_and, false);
        break;
    case CORE_TEXT_ARG_ALTERNATIVES:
        put_items(out, catalog, arg, catalog->list_separator,
                  catalog->list_or, false);
        break;
    case CORE_TEXT_ARG_CLAUSES:
        put_items(out, catalog, arg, catalog->clause_separator,
                  catalog->clause_separator, false);
        break;
    case CORE_TEXT_ARG_NATIVE:
        core_buffer_printf(out, "%s", arg->native ? arg->native : "");
        break;
    case CORE_TEXT_ARG_CHARACTER:
        core_buffer_putc(out, (char)arg->number);
        break;
    case CORE_TEXT_ARG_CODEPOINT:
        core_buffer_printf(out, "%04llX", (unsigned long long)arg->number);
        break;
    }
}

static bool
counted(const core_text_arg_t *arg, int64_t *count)
{
    switch (arg->kind) {
    case CORE_TEXT_ARG_NUMBER:
        *count = arg->number;
        return true;
    case CORE_TEXT_ARG_NAMES:
    case CORE_TEXT_ARG_CHOICES:
    case CORE_TEXT_ARG_LIST:
    case CORE_TEXT_ARG_ALTERNATIVES:
        *count = (int64_t)arg->count;
        return true;
    default:
        return false;
    }
}

void
core_text_expand(core_buffer_t *out, const core_text_catalog_t *catalog,
                 uint32_t id, core_text_args_t args)
{
    if (!catalog || id >= catalog->count) return;
    core_string_t form = catalog->forms[id].one;
    for (size_t i = 0; i < args.count; i++) {
        int64_t count = 0;
        if (!counted(&args.at[i], &count)) continue;
        if (count != 1) form = catalog->forms[id].other;
        break;
    }
    for (size_t at = 0; at < form.u8_bytes; at++) {
        char c = form.data[at];
        size_t end = at + 1;
        size_t slot = 0;
        while (c == '{' && end < form.u8_bytes && form.data[end] >= '0'
               && form.data[end] <= '9')
            slot = slot * 10 + (size_t)(form.data[end++] - '0');
        if (c == '{' && end == at + 1 && end < form.u8_bytes
            && form.data[end] == '{') {
            core_buffer_putc(out, '{');
            at = end;
        }
        else if (c == '{' && end > at + 1 && end < form.u8_bytes
                 && form.data[end] == '}') {
            if (slot < args.count) put_argument(out, catalog, &args.at[slot]);
            at = end;
        }
        else core_buffer_putc(out, c);
    }
}

core_string_t
core_text_render(const core_text_catalog_t *catalog, uint32_t id,
                 core_text_args_t args)
{
    core_buffer_t *out = core_buffer_empty();
    core_text_expand(out, catalog, id, args);
    return core_buffer_take_string(out);
}

core_string_t
core_text_form(const core_text_catalog_t *catalog, uint32_t id)
{
    return catalog && id < catalog->count ? catalog->forms[id].one
                                          : (core_string_t){0};
}
