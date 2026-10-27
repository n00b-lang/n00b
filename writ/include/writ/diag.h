#pragma once

#include "diag/record.h"

typedef enum {
#define WRIT_DIAG(id, code, severity, text) WRIT_D_##id,
#include "writ/diag_codes.def"
#undef WRIT_DIAG
    WRIT_D_COUNT
} writ_diag_code_t;

typedef enum {
#define WRIT_TEXT(id, text) WRIT_TEXT_##id,
#include "writ/diag_texts.def"
#undef WRIT_TEXT
    WRIT_TEXT_COUNT
} writ_text_t;
