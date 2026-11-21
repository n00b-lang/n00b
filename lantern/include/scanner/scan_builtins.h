#pragma once

#include "scanner/scanner.h"

extern core_option_t(core_string_t) scan_string_double(parse_scanner_t *s);

extern core_option_t(core_string_t) scan_string_single(parse_scanner_t *s);

extern core_option_t(core_string_t) scan_integer(parse_scanner_t *s);

extern core_option_t(core_string_t) scan_float(parse_scanner_t *s);

extern core_option_t(core_string_t) scan_identifier(parse_scanner_t *s);
