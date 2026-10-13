#include "abi_string.h"

#include <limits.h>

_Static_assert(CHAR_BIT == 8, "n00b ABI bytes are octets");
_Static_assert(sizeof(uint64_t) == N00BB_ABI_STRING_WORD_BYTES,
               "n00b string words are 64 bits");

#define N00BB_STRING_PAYLOAD_MASK \
    ((UINT64_C(1) << N00BB_ABI_STRING_PAYLOAD_BITS) - 1)

bool
n00bb_abi_string_decode(const uint8_t *value, size_t available,
                         n00bb_abi_string_header_t *out)
{
    if (!value || !out || available < N00BB_ABI_STRING_VALUE_BYTES)
        return false;
    uint64_t high = 0;
    for (unsigned i = 0; i < N00BB_ABI_STRING_WORD_BYTES; i++)
        high |= (uint64_t)value[N00BB_ABI_STRING_WORD_BYTES + i]
                << (i * CHAR_BIT);
    n00bb_abi_string_header_t decoded = {
        .tag = (n00bb_abi_string_tag_t)
            (high >> N00BB_ABI_STRING_TAG_SHIFT),
        .encoding = (n00bb_abi_string_encoding_t)
            ((high >> N00BB_ABI_STRING_ENCODING_SHIFT)
                & ((1u << N00BB_ABI_STRING_ENCODING_BITS) - 1)),
    };
    if (decoded.encoding > N00BB_ABI_STRING_UTF8) return false;
    switch (decoded.tag) {
    case N00BB_ABI_STRING_SHORT:
        decoded.length = value[N00BB_ABI_STRING_VALUE_BYTES - 1]
                         & ((1u << N00BB_ABI_STRING_SHORT_LENGTH_BITS) - 1);
        if (decoded.length > N00BB_ABI_STRING_SHORT_CAPACITY) return false;
        break;
    case N00BB_ABI_STRING_LONG:
    case N00BB_ABI_STRING_SPAN:
        decoded.length = high & N00BB_STRING_PAYLOAD_MASK;
        break;
    case N00BB_ABI_STRING_RICH:
        decoded.rich_reserved = high & N00BB_STRING_PAYLOAD_MASK;
        break;
    }
    *out = decoded;
    return true;
}
