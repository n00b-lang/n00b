#include "lib/byteorder.h"
#include "harness.h"

#include <string.h>

int main(void)
{
    uint8_t bytes[10] = {0};
    const uint8_t two[] = {0x34, 0x12};
    const uint8_t four[] = {0x78, 0x56, 0x34, 0x12};
    const uint8_t eight[] = {
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
    };
    core_le_store_u16(bytes + 1, UINT16_C(0x1234));
    check("unaligned little-endian u16 bytes",
          !memcmp(bytes + 1, two, sizeof(two))
          && core_le_load_u16(bytes + 1) == UINT16_C(0x1234));
    core_le_store_u32(bytes + 1, UINT32_C(0x12345678));
    check("unaligned little-endian u32 bytes",
          !memcmp(bytes + 1, four, sizeof(four))
          && core_le_load_u32(bytes + 1) == UINT32_C(0x12345678));
    core_le_store_u64(bytes + 1, UINT64_C(0x1122334455667788));
    check("unaligned little-endian u64 bytes",
          !memcmp(bytes + 1, eight, sizeof(eight))
          && core_le_load_u64(bytes + 1) == UINT64_C(0x1122334455667788));
    return 0;
}
