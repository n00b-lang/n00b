#ifndef N00BB_ABI_STRING_H
#define N00BB_ABI_STRING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    N00BB_ABI_STRING_WORD_BYTES = 8,
    N00BB_ABI_STRING_VALUE_BYTES = 2 * N00BB_ABI_STRING_WORD_BYTES,
    N00BB_ABI_STRING_TAG_BITS = 2,
    N00BB_ABI_STRING_ENCODING_BITS = 2,
    N00BB_ABI_STRING_SHORT_LENGTH_BITS = 4,
    N00BB_ABI_STRING_WORD_BITS = N00BB_ABI_STRING_WORD_BYTES * 8,
    N00BB_ABI_STRING_TAG_SHIFT = N00BB_ABI_STRING_WORD_BITS
                                 - N00BB_ABI_STRING_TAG_BITS,
    N00BB_ABI_STRING_ENCODING_SHIFT = N00BB_ABI_STRING_TAG_SHIFT
                                      - N00BB_ABI_STRING_ENCODING_BITS,
    N00BB_ABI_STRING_PAYLOAD_BITS = N00BB_ABI_STRING_ENCODING_SHIFT,
    N00BB_ABI_STRING_SHORT_CAPACITY = 14,
};

typedef enum {
    N00BB_ABI_STRING_SHORT = 0,
    N00BB_ABI_STRING_LONG = 1,
    N00BB_ABI_STRING_SPAN = 2,
    N00BB_ABI_STRING_RICH = 3,
} n00bb_abi_string_tag_t;

typedef enum {
    N00BB_ABI_STRING_BINARY = 0,
    N00BB_ABI_STRING_ASCII = 1,
    N00BB_ABI_STRING_UTF8 = 2,
} n00bb_abi_string_encoding_t;

typedef struct {
    n00bb_abi_string_tag_t tag;
    n00bb_abi_string_encoding_t encoding;
    uint64_t length;
    uint64_t rich_reserved;
} n00bb_abi_string_header_t;

/* Decode the tag and length/auxiliary bits of a little-endian 16-byte string
 * value. Pointer bytes remain in value[0..7] and require relocation and
 * pool validation by the containing metadata reader. Invalid encodings and
 * short lengths fail without modifying out. */
bool n00bb_abi_string_decode(const uint8_t *value, size_t available,
                             n00bb_abi_string_header_t *out);

#endif
