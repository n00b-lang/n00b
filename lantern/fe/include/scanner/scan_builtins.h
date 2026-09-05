#pragma once

#include "scanner/scanner.h"

extern ncc_option_t(ncc_string_t) ncc_scan_string_double(ncc_scanner_t *s);

extern ncc_option_t(ncc_string_t) ncc_scan_string_single(ncc_scanner_t *s);

extern ncc_option_t(ncc_string_t) ncc_scan_string_raw(ncc_scanner_t *s, const char *quote);

extern ncc_option_t(ncc_string_t) ncc_scan_integer(ncc_scanner_t *s);

extern ncc_option_t(ncc_string_t) ncc_scan_float(ncc_scanner_t *s);

extern bool ncc_scan_number(ncc_scanner_t *s,
                              int32_t int_tid,
                              int32_t float_tid);

extern ncc_option_t(ncc_string_t) ncc_scan_identifier(ncc_scanner_t *s);
