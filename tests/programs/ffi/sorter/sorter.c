#include <stddef.h>
#include <stdint.h>

typedef int (*compare_fn)(const void *a, const void *b);

int
nth_sorted(const uint8_t *items, size_t count, size_t n, compare_fn compare)
{
    uint8_t copy[64];
    if (count > sizeof copy || n >= count) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        copy[i] = items[i];
    }
    for (size_t i = 1; i < count; i++) {
        uint8_t value = copy[i];
        size_t j = i;
        while (j > 0 && compare(&copy[j - 1], &value) > 0) {
            copy[j] = copy[j - 1];
            j--;
        }
        copy[j] = value;
    }
    return copy[n];
}

uint8_t
byte_at(uintptr_t address)
{
    return *(const uint8_t *)address;
}
