#pragma once

#include "core.h"

enum {
    UCD_SCRIPT_UNKNOWN   = 0,
    UCD_SCRIPT_COMMON    = 1,
    UCD_SCRIPT_INHERITED = 2,
};

typedef enum {
    UCD_NFD,
    UCD_NFC,
    UCD_NFKD,
    UCD_NFKC,
} ucd_form_t;

core_string_t ucd_version(void);
bool ucd_is_xid_start(core_codepoint_t cp);
bool ucd_is_xid_continue(core_codepoint_t cp);
bool ucd_is_extended_pictographic(core_codepoint_t cp);
uint8_t ucd_display_width(core_codepoint_t cp);
uint8_t ucd_script(core_codepoint_t cp);
bool ucd_script_named(core_string_t name, uint8_t *script);
core_string_t ucd_normalize(core_string_t text, ucd_form_t form);
core_string_t ucd_skeleton(core_string_t text);
