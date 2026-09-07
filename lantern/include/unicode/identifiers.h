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

static inline bool
unicode_is_pattern_syntax(core_codepoint_t cp)
{
    return (cp >= '!' && cp <= '/')
        || (cp >= ':' && cp <= '@')
        || (cp >= '[' && cp <= '^')
        || cp == '`'
        || (cp >= '{' && cp <= '~');
}

static inline bool
unicode_is_pattern_white_space(core_codepoint_t cp)
{
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r'
        || cp == '\f' || cp == '\v';
}
