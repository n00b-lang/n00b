#pragma once

#include "scanner/lex.h"

typedef struct scan_unicode_profile_t scan_unicode_profile_t;

typedef struct {
    core_string_t normalized;
    core_string_t diagnostic;
    size_t        diagnostic_start;
    size_t        diagnostic_end;
} scan_unicode_identifier_result_t;

scan_unicode_profile_t *
scan_unicode_profile_compile(const scan_lex_identifier_profile_t *spec,
                             char *error, size_t error_size);
void scan_unicode_profile_free(scan_unicode_profile_t *profile);
bool scan_unicode_profile_is_start(const scan_unicode_profile_t *profile,
                                   core_codepoint_t cp);
bool scan_unicode_profile_is_continue(const scan_unicode_profile_t *profile,
                                      core_codepoint_t cp);
bool scan_unicode_profile_process(const scan_unicode_profile_t *profile,
                                  core_string_t spelling,
                                  scan_unicode_identifier_result_t *result);
core_string_t scan_unicode_profile_version(void);
