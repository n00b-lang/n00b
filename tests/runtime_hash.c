#include <assert.h>
#include <stdint.h>

void n00bb_bootstrap_hash_integer_64(uint64_t value, uint64_t result[2]);
void n00bb_bootstrap_hash_scalar_known(uint32_t type_field, uint64_t payload,
                                       uint32_t bits, uint32_t floating,
                                       uint64_t result[2]);

int main(void)
{
    const uint64_t values[] = {
        0, 1, UINT64_C(0x7fffffffffffffff),
        UINT64_C(0x8000000000000000), UINT64_MAX
    };
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        uint64_t fast[2] = {0};
        uint64_t generic[2] = {0};
        n00bb_bootstrap_hash_integer_64(values[i], fast);
        n00bb_bootstrap_hash_scalar_known(0, values[i], 64, 0, generic);
        assert(fast[0] == generic[0] && fast[1] == generic[1]);
    }
    return 0;
}
