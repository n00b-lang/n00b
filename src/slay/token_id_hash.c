#include "n00b.h"
#include "core/hash.h"

int64_t
n00b_token_id_from_text(const char *text, size_t len)
{
    return (int64_t)(n00b_xxh3_64bits_raw(text, len) | (1ULL << 63));
}
