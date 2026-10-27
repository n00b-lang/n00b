#pragma once

#include "diag/diagnostics.h"
#include "parse/pwz.h"
#include "scanner/scanner.h"

typedef struct {
    core_diagnostics_t *diagnostics;
    const core_translation_unit_t *unit;
    parse_grammar_t *grammar;
    parse_token_stream_t *tokens;
    parse_pwz_parser_t *parser;
    parse_pwz_disambig_fn disambiguator;
    core_string_t text;
    core_diag_location_t end_of_file;
} n00bb_syntax_input_t;

void n00bb_report_syntax_errors(const n00bb_syntax_input_t *input);
void n00bb_report_lexical_error(core_diagnostics_t *diagnostics,
                                const core_translation_unit_t *unit,
                                core_string_t text,
                                const scan_diagnostic_t *diagnostic);
void n00bb_report_lexical_warning(core_diagnostics_t *diagnostics,
                                  const core_translation_unit_t *unit,
                                  core_string_t text,
                                  const scan_warning_t *warning);
