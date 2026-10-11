#include <cstdint>
#include <cstdlib>
#include <vector>

int main(int argc, char **)
{
    const std::int64_t base = argc - 1;
    std::vector<std::int64_t> items;
    for (std::int64_t i = 0; i < 100000; i++) items.push_back(i + base);
    if (items.size() != 100000) std::abort();
    std::int64_t sum = 0;
    for (std::int64_t i = 0; i < 100000; i++) sum += items[i] - base;
    if (sum != 4999950000) std::abort();
    for (std::int64_t i = 0; i < 50000; i++) {
        if (items.back() - base != 99999 - i) std::abort();
        items.pop_back();
    }
    if (items.size() != 50000) std::abort();
}
