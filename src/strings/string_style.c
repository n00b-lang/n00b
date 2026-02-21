#include "strings/string_style.h"
#include "strings/style_registry.h"
#include "core/alloc.h"
#include <string.h>

// ===================================================================
// Internal helpers
// ===================================================================

static n00b_string_style_info_t *
clone_info(const n00b_string_style_info_t *src, int64_t extra_records,
           n00b_allocator_t *allocator)
{
    int64_t old_n = src ? src->num_styles : 0;
    int64_t new_n = old_n + extra_records;

    n00b_string_style_info_t *info =
        n00b_alloc_flex_with_opts(n00b_string_style_info_t, n00b_style_record_t, new_n,
                                  &(n00b_alloc_opts_t){.allocator = allocator});
    info->num_styles = new_n;

    if (src) {
        info->base_style = src->base_style;
        if (old_n > 0) {
            memcpy(info->styles, src->styles,
                   (size_t)old_n * sizeof(n00b_style_record_t));
        }
    }

    return info;
}

// ===================================================================
// Attach
// ===================================================================

n00b_string_t
n00b_str_set_base_style(n00b_string_t s, const n00b_text_style_t *style)
    _kargs { n00b_allocator_t *allocator = nullptr; }
{
    n00b_string_style_info_t *old  = (n00b_string_style_info_t *)s.styling;
    n00b_string_style_info_t *info = clone_info(old, 0, allocator);
    info->base_style               = n00b_str_style_copy(style,
                                                          .allocator = allocator);

    n00b_string_t result = s;
    result.styling       = info;
    return result;
}

n00b_string_t
n00b_str_add_style(n00b_string_t s, const n00b_text_style_t *style,
                    size_t start, n00b_option_t(size_t) end_opt)
    _kargs {
        const char       *tag       = nullptr;
        n00b_allocator_t *allocator = nullptr;
    }
{
    n00b_string_style_info_t *old  = (n00b_string_style_info_t *)s.styling;
    n00b_string_style_info_t *info = clone_info(old, 1, allocator);

    int64_t idx              = info->num_styles - 1;
    info->styles[idx].info   = style
        ? n00b_str_style_copy(style, .allocator = allocator)
        : nullptr;
    info->styles[idx].tag    = tag;
    info->styles[idx].start  = start;
    info->styles[idx].end    = end_opt;

    n00b_string_t result = s;
    result.styling       = info;
    return result;
}

// ===================================================================
// Query
// ===================================================================

n00b_option_t(n00b_string_style_info_t *)
n00b_str_get_style_info(n00b_string_t s)
{
    return n00b_option_from_nullable(n00b_string_style_info_t *,
                                     (n00b_string_style_info_t *)s.styling);
}

n00b_text_style_t *
n00b_str_resolve_style_at(n00b_string_t s, size_t byte_pos)
    _kargs { n00b_allocator_t *allocator = nullptr; }
{
    n00b_string_style_info_t *info = (n00b_string_style_info_t *)s.styling;

    if (!info) {
        return n00b_str_style_new(.allocator = allocator);
    }

    n00b_text_style_t *acc;

    if (info->base_style) {
        acc = n00b_str_style_copy(info->base_style, .allocator = allocator);
    }
    else {
        acc = n00b_str_style_new(.allocator = allocator);
    }

    for (int64_t i = 0; i < info->num_styles; i++) {
        n00b_style_record_t *rec = &info->styles[i];

        if (byte_pos < rec->start) {
            continue;
        }

        if (n00b_option_is_set(rec->end) && byte_pos >= n00b_option_get(rec->end)) {
            continue;
        }

        // Lazy-resolve deferred tag on first access.
        if (!rec->info && rec->tag) {
            auto tag_opt = (rec->tag[0] == '@')
                ? n00b_str_role_lookup(rec->tag)
                : n00b_str_style_lookup(rec->tag);
            if (n00b_option_is_set(tag_opt)) {
                rec->info = n00b_option_get(tag_opt);
            }
        }
        if (!rec->info) {
            continue;
        }

        n00b_text_style_t *merged = n00b_str_style_merge(acc, rec->info,
                                                          .allocator = allocator);
        n00b_free(acc);
        acc = merged;
    }

    return acc;
}

// ===================================================================
// Strip
// ===================================================================

n00b_string_t
n00b_str_strip_styles(n00b_string_t s)
{
    n00b_string_t result = s;
    result.styling       = nullptr;
    return result;
}
