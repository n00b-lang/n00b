#include <assert.h>
#include <stdint.h>

void n00bb_bootstrap_hash_scalar_known(uint32_t type_field, uint64_t payload,
                                       uint32_t bits, uint32_t floating,
                                       uint64_t result[2]);

int main(void)
{
    const uint64_t values[] = {0, 1, UINT64_C(0x7fffffffffffffff)};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        uint64_t first[2] = {0};
        uint64_t second[2] = {0};
        n00bb_bootstrap_hash_scalar_known(0, values[i], 64, 0, first);
        n00bb_bootstrap_hash_scalar_known(0, values[i], 64, 0, second);
        assert(first[0] == second[0] && first[1] == second[1]);
    }
    uint64_t positive_zero[2] = {0};
    uint64_t negative_zero[2] = {0};
    n00bb_bootstrap_hash_scalar_known(0, 0, 64, 1, positive_zero);
    n00bb_bootstrap_hash_scalar_known(0, UINT64_C(0x8000000000000000),
                                      64, 1, negative_zero);
    assert(positive_zero[0] == negative_zero[0]
           && positive_zero[1] == negative_zero[1]);
    return 0;
}
