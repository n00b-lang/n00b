#pragma once

#include "parse/types.h"
#include "unicode/identifiers.h"
#include "unicode/properties.h"

bool parse_codepoint_matches_class(int32_t cp, parse_char_class_t cc);
