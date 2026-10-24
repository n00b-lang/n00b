#pragma once

#include <stdbool.h>
#include "lib/string.h"
#include "parse/symtab.h"

static inline bool
symbol_is_private(const parse_sym_entry_t *symbol)
{
    return symbol && core_string_eq(symbol->visibility,
                                    CORE_STRING_STATIC("private"));
}

static inline bool
member_is_private(const parse_sym_entry_t *member,
                  const parse_sym_entry_t *owner)
{
    return symbol_is_private(member) || symbol_is_private(owner);
}
