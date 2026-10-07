#include "abi_string.h"

typedef struct {
    unsigned char bytes[15];
    unsigned char selector;
} short_string;

typedef struct {
    const unsigned char *data;
    uint64_t selector;
} long_string;

typedef struct {
    const unsigned char *body;
    uint64_t selector;
} rich_string;

typedef union {
    short_string short_value;
    long_string long_value;
    rich_string rich_value;
    unsigned long long words[2];
} string_value;

_Static_assert(sizeof(string_value) == 16, "string width");
_Static_assert(_Alignof(string_value) == 8, "string alignment");

const string_value short_abc = {
    .short_value = {.bytes = {'a', 'b', 'c'}, .selector =
        (N00BB_ABI_STRING_ASCII << N00BB_ABI_STRING_SHORT_LENGTH_BITS) | 3},
};

const string_value long_ascii_16 = {
    .long_value = {.data = (const unsigned char *)"abcdefghijklmnop",
                   .selector =
        ((uint64_t)N00BB_ABI_STRING_LONG << N00BB_ABI_STRING_TAG_SHIFT)
        | ((uint64_t)N00BB_ABI_STRING_ASCII
           << N00BB_ABI_STRING_ENCODING_SHIFT) | 16},
};

const string_value span_ascii_5 = {
    .long_value = {.data = (const unsigned char *)"abcdefghij" + 2,
                   .selector =
        ((uint64_t)N00BB_ABI_STRING_SPAN << N00BB_ABI_STRING_TAG_SHIFT)
        | ((uint64_t)N00BB_ABI_STRING_ASCII
           << N00BB_ABI_STRING_ENCODING_SHIFT) | 5},
};

const string_value rich_utf8 = {
    .rich_value = {.body = (const unsigned char *)"rich",
                   .selector =
        ((uint64_t)N00BB_ABI_STRING_RICH << N00BB_ABI_STRING_TAG_SHIFT)
        | ((uint64_t)N00BB_ABI_STRING_UTF8
           << N00BB_ABI_STRING_ENCODING_SHIFT)},
};

static int match_bytes(const unsigned char *actual,
                       const unsigned char *expected, unsigned count)
{
    for (unsigned i = 0; i < count; i++)
        if (actual[i] != expected[i]) return 0;
    return 1;
}

int main(void)
{
    static const unsigned char short_bytes[16] = {
        'a', 'b', 'c', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x13,
    };
    static const unsigned char long_high_word[8] = {
        16, 0, 0, 0, 0, 0, 0, 0x50,
    };
    static const unsigned char span_high_word[8] = {
        5, 0, 0, 0, 0, 0, 0, 0x90,
    };
    static const unsigned char rich_high_word[8] = {
        0, 0, 0, 0, 0, 0, 0, 0xe0,
    };
    const unsigned char *short_data = (const unsigned char *)&short_abc;
    const unsigned char *long_data = (const unsigned char *)&long_ascii_16;
    const unsigned char *span_data = (const unsigned char *)&span_ascii_5;
    const unsigned char *rich_data = (const unsigned char *)&rich_utf8;

    n00bb_abi_string_header_t header = {0};
    if (!n00bb_abi_string_decode(short_data, sizeof(short_abc), &header)
        || header.tag != N00BB_ABI_STRING_SHORT
        || header.encoding != N00BB_ABI_STRING_ASCII
        || header.length != 3
        || header.rich_reserved != 0)
        return 1;
    if (!n00bb_abi_string_decode(long_data, sizeof(long_ascii_16), &header)
        || header.tag != N00BB_ABI_STRING_LONG
        || header.encoding != N00BB_ABI_STRING_ASCII
        || header.length != 16)
        return 1;
    if (!n00bb_abi_string_decode(span_data, sizeof(span_ascii_5), &header)
        || header.tag != N00BB_ABI_STRING_SPAN
        || header.encoding != N00BB_ABI_STRING_ASCII
        || header.length != 5)
        return 1;
    if (!n00bb_abi_string_decode(rich_data, sizeof(rich_utf8), &header)
        || header.tag != N00BB_ABI_STRING_RICH
        || header.encoding != N00BB_ABI_STRING_UTF8
        || header.rich_reserved != 0)
        return 1;

    unsigned char bad_short[16] = {0};
    bad_short[15] = 15;
    header.length = 99;
    if (n00bb_abi_string_decode(bad_short, sizeof(bad_short), &header)
        || header.length != 99)
        return 1;
    bad_short[15] = 0x30;
    if (n00bb_abi_string_decode(bad_short, sizeof(bad_short), &header)
        || header.length != 99)
        return 1;
    if (n00bb_abi_string_decode(nullptr, sizeof(short_abc), &header)
        || n00bb_abi_string_decode(short_data, sizeof(short_abc), nullptr)
        || n00bb_abi_string_decode(short_data,
            N00BB_ABI_STRING_VALUE_BYTES - 1, &header)
        || header.length != 99)
        return 1;

    unsigned char rich_with_aux[16] = {0};
    rich_with_aux[8] = 7;
    rich_with_aux[15] = 0xc0;
    if (!n00bb_abi_string_decode(rich_with_aux, sizeof(rich_with_aux),
                                 &header)
        || header.tag != N00BB_ABI_STRING_RICH
        || header.rich_reserved != 7 || header.length != 0)
        return 1;

    unsigned char max_length[16] = {0};
    for (unsigned i = 8; i < 15; i++) max_length[i] = 0xff;
    max_length[15] = 0x5f;
    if (!n00bb_abi_string_decode(max_length, sizeof(max_length), &header)
        || header.length != ((UINT64_C(1) << 60) - 1))
        return 1;

    return match_bytes(short_data, short_bytes, 16)
        && match_bytes(long_data + 8, long_high_word, 8)
        && match_bytes(span_data + 8, span_high_word, 8)
        && match_bytes(rich_data + 8, rich_high_word, 8)
        && long_ascii_16.long_value.data[15] == 'p'
        && span_ascii_5.long_value.data[0] == 'c'
        ? 0 : 1;
}
