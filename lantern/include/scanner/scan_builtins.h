#pragma once

#include "scanner/scanner.h"

extern core_option_t(core_string_t) scan_string_double(parse_scanner_t *s);

extern core_option_t(core_string_t) scan_string_single(parse_scanner_t *s);

extern core_option_t(core_string_t)
    scan_string_raw(parse_scanner_t *s, core_string_t quote);

extern core_option_t(core_string_t) scan_integer(parse_scanner_t *s);

extern core_option_t(core_string_t) scan_float(parse_scanner_t *s);

extern bool scan_number(parse_scanner_t *s,
                              int32_t int_tid,
                              int32_t float_tid);

extern core_option_t(core_string_t) scan_identifier(parse_scanner_t *s);
