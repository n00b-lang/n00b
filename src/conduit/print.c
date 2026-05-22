/*
 * print.c - Print API implementation.
 *
 * Converts objects to strings via vtable dispatch and writes the
 * resulting UTF-8 bytes to managed file descriptors or conduit topics.
 */

#include "conduit/print.h"
#include "conduit/write.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/type_info.h"
#include "core/string.h"
#include "core/buffer.h"
#include "text/strings/format.h"
#include "text/strings/string_convert.h"
#include "text/strings/string_ops.h"
#include "text/strings/fmt_numbers.h"
#include <string.h>
#include <stdio.h>

// ============================================================================
// n00b_to_string
// ============================================================================

n00b_string_t *
n00b_to_string(void *obj)
{
    if (!obj) {
        return n00b_string_from_raw("(null)", 6);
    }

    n00b_option_t(n00b_vtable_entry) fn_opt =
        n00b_obj_core_method(obj, N00B_BI_TO_STRING);

    if (n00b_option_is_set(fn_opt)) {
        typedef n00b_string_t *(*to_string_fn)(void *);
        return ((to_string_fn)n00b_option_get(fn_opt))(obj);
    }

    // Fallback: "<typename@0xADDR>"
    auto info_opt = n00b_type_info_for(obj);
    const char *name = "unknown";

    if (n00b_option_is_set(info_opt)) {
        name = n00b_option_get(info_opt)->name;
    }

    n00b_string_t *prefix = n00b_string_from_raw("<", 1);
    n00b_string_t *tname  = n00b_string_from_raw(name, (int64_t)strlen(name));
    n00b_string_t *at     = n00b_fmt_pointer(obj);
    n00b_string_t *suffix = n00b_string_from_raw(">", 1);

    n00b_string_t *s = n00b_unicode_str_cat(prefix, tname);
    s                = n00b_unicode_str_cat(s, at);
    s                = n00b_unicode_str_cat(s, suffix);

    return s;
}

// ============================================================================
// Topic lookup for print
// ============================================================================

static n00b_conduit_topic_t(n00b_buffer_t *) *
get_print_topic(int fd)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    if (!rt) {
        return nullptr;
    }

    if (fd == 1) {
        return (n00b_conduit_topic_t(n00b_buffer_t *) *)rt->stdout_topic;
    }
    if (fd == 2) {
        return (n00b_conduit_topic_t(n00b_buffer_t *) *)rt->stderr_topic;
    }

    return nullptr;
}

// ============================================================================
// Shared write helper
// ============================================================================

static void
do_print_string(n00b_string_t *s, n00b_option_t(n00b_string_t *) end, int fd,
                n00b_conduit_topic_t(n00b_buffer_t *) *topic, bool sync)
{
    n00b_string_t *end_str = n00b_option_is_set(end)
        ? n00b_option_get(end)
        : n00b_string_from_raw("\n", 1);

    s = n00b_unicode_str_cat(s, end_str);

    if (!topic) {
        topic = get_print_topic(fd);
    }

    if (!topic) {
        return;
    }

    n00b_buffer_t *buf = n00b_buffer_from_bytes(s->data, (int64_t)s->u8_bytes);
    n00b_write(n00b_buffer_t *, topic, buf, .sync = sync);
}

// ============================================================================
// n00b_print
// ============================================================================

void
n00b_print(void *obj) _kargs
{
    n00b_conduit_topic_t(n00b_buffer_t *) *topic = nullptr;
    n00b_option_t(n00b_string_t *)          end   = n00b_option_none(n00b_string_t *);
    int                                    fd    = 1;
    bool                                   sync  = true;
}
{
    n00b_string_t *s = n00b_to_string(obj);

    do_print_string(s, end, fd, topic, sync);
}

// ============================================================================
// n00b_printf
// ============================================================================

void
n00b_printf(const char *fmt, +) _kargs
{
    n00b_conduit_topic_t(n00b_buffer_t *) *topic = nullptr;
    int                                    fd    = 1;
    n00b_option_t(n00b_string_t *)          end   = n00b_option_none(n00b_string_t *);
    bool                                   sync  = true;
}
{
    n00b_string_t *s = _n00b_format_impl(fmt, (int32_t)strlen(fmt), vargs);
    do_print_string(s, end, fd, topic, sync);
}
