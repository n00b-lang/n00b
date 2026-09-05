#pragma once

#include "parse/types.h"
#include "unicode/identifiers.h"
#include "unicode/properties.h"

bool ncc_codepoint_matches_class(int32_t cp, ncc_char_class_t cc);
