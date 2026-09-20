#pragma once

#include "scanner/lex.h"

typedef enum : uint8_t {
    SCAN_LEX_CONFORMANCE_BOUNDARY,
    SCAN_LEX_CONFORMANCE_TRUNCATION,
    SCAN_LEX_CONFORMANCE_INCREMENTAL_SPLIT,
    SCAN_LEX_CONFORMANCE_LONGEST_PREFIX,
    SCAN_LEX_CONFORMANCE_MALFORMED,
    SCAN_LEX_CONFORMANCE_CONCURRENT,
} scan_lex_conformance_kind_t;

typedef struct {
    size_t                      id;
    scan_lex_conformance_kind_t kind;
    scan_lex_decl_kind_t        declaration;
    core_string_t               spelling;
    core_string_t               origin;
    core_string_t               source;
    size_t                      input_bytes;
    size_t                      split_byte;
    scan_status_t               expected_status;
    bool                        recovery;
} scan_lex_conformance_case_t;

typedef struct scan_lex_conformance_suite_t scan_lex_conformance_suite_t;

extern scan_lex_conformance_suite_t *
scan_lex_conformance_generate(const scan_lex_t *program, size_t max_cases);

extern size_t scan_lex_conformance_count(
    const scan_lex_conformance_suite_t *suite);

extern bool scan_lex_conformance_case(
    const scan_lex_conformance_suite_t *suite, size_t index,
    scan_lex_conformance_case_t *out);

extern uint64_t scan_lex_conformance_fingerprint(
    const scan_lex_conformance_suite_t *suite);

extern void scan_lex_conformance_free(scan_lex_conformance_suite_t *suite);
