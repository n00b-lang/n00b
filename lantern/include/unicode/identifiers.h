#pragma once

#include "core.h"

static inline bool
unicode_is_id_start(core_codepoint_t cp)
{
    return (cp >= 'A' && cp <= 'Z')
        || (cp >= 'a' && cp <= 'z')
        || cp == '_'
        || cp >= 0x80;
}

static inline bool
unicode_is_id_continue(core_codepoint_t cp)
{
    return unicode_is_id_start(cp)
        || (cp >= '0' && cp <= '9');
}
