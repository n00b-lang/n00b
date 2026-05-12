// Paired header for test_regex_fuzz_compare.c. Declares the data shapes that
// mirror the Rust FuzzEntry / RegexCrateEntry structs and the public entry
// points.
// NOTE: External regex / serde / threading symbols referenced by the .c are
// forward-declared inside the .c file as `extern`s, not here, since they are
// part of the broader ported project and will be reconciled in phase 2.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "text/regex/regex.h"

//.
// `input` is a UTF-8 byte buffer; `input_len` is its length in bytes.
// Rust strings are guaranteed UTF-8, so we store the bytes plus length and
// also ensure the buffer is NUL-terminated for ergonomic C consumers.
typedef struct {
    char *pattern;       // owned, NUL-terminated
    char *input;         // owned, NUL-terminated (length = input_len)
    size_t input_len;
} fuzz_entry_t;

//.
// `has_matches == false` corresponds to Rust's None; otherwise `matches` /
// `match_count` describe the array (length zero is allowed and represents an
// empty Vec, i.e. Some(vec![])).
typedef struct {
    char *pattern;
    char *input;
    size_t input_len;
    bool has_matches;
    n00b_regex_match_t *matches;
    size_t match_count;
} regex_crate_entry_t;

// Core helpers.
n00b_regex_match_t *regex_find_multiline(const char *pattern,
                                         const uint8_t *input,
                                         size_t input_len,
                                         size_t *out_count);

void record_regex_crate(const char *filename);

// Test entry points (originally Rust #[test] #[ignore] functions).
void fuzz_npm(void);
void fuzz_pypi(void);
void fuzz_regexlib(void);
void fuzz_stackoverflow(void);
void fuzz_uniq(void);
