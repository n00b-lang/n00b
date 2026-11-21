#include "handoff.h"

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/os.h"

#define HANDOFF_VARIABLE CORE_STRING_STATIC("N00BB_HANDOFF")
#define CACHE_EXCLUDE CORE_STRING_STATIC("cache_exclude")

static n00bb_handoff_t handoff;
static bool loaded;

static void
put(core_buffer_t *out, core_string_t name, core_string_t value)
{
    core_buffer_append_string(out, name);
    core_buffer_printf(out, "=%zu:", value.u8_bytes);
    core_buffer_append_string(out, value);
    core_buffer_putc(out, ';');
}

static bool
take(core_string_t *rest, core_string_t *name, core_string_t *value)
{
    int64_t equals = core_string_find_byte(*rest, '=');
    int64_t colon = core_string_find_byte(*rest, ':');
    if (equals <= 0 || colon <= equals) return false;
    int64_t length = 0;
    core_string_t digits = core_string_slice(*rest, (size_t)equals + 1,
                                             (size_t)(colon - equals - 1));
    if (!core_string_parse_i64(digits, 10, &length, nullptr) || length < 0)
        return false;
    size_t start = (size_t)colon + 1;
    if (start + (size_t)length >= rest->u8_bytes
        || rest->data[start + (size_t)length] != ';')
        return false;
    *name = core_string_slice(*rest, 0, (size_t)equals);
    *value = core_string_slice(*rest, start, (size_t)length);
    *rest = core_string_slice(*rest, start + (size_t)length + 1,
                              rest->u8_bytes - start - (size_t)length - 1);
    return true;
}

static void
load(void)
{
    loaded = true;
    core_string_t text = core_os_environment(HANDOFF_VARIABLE);
    core_string_t rest = text;
    core_string_t name = {};
    core_string_t value = {};
    while (rest.u8_bytes && take(&rest, &name, &value)) {
        if (core_string_eq(name, CACHE_EXCLUDE))
            n00bb_handoff_set(&handoff.cache_exclude, value);
    }
    core_free(text.data);
}

n00bb_handoff_t *
n00bb_handoff(void)
{
    if (!loaded) load();
    return &handoff;
}

void
n00bb_handoff_set(core_string_t *field, core_string_t value)
{
    core_string_t copy = value.data ? core_string_dup(value) : value;
    core_free(field->data);
    *field = copy;
}

bool
n00bb_handoff_pass(void)
{
    const n00bb_handoff_t *record = n00bb_handoff();
    core_buffer_t out = {0};
    if (record->cache_exclude.u8_bytes)
        put(&out, CACHE_EXCLUDE, record->cache_exclude);
    bool passed = out.byte_len
        ? core_os_set_environment(HANDOFF_VARIABLE, core_buffer_string(&out))
        : core_os_unset_environment(HANDOFF_VARIABLE);
    core_buffer_release(&out);
    return passed;
}
