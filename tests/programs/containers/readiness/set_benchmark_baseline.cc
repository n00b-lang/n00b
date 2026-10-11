#include <cstdint>
#include <cstdlib>
#include <unordered_set>

int main(int argc, char **)
{
    const std::int64_t base = argc - 1;
    std::unordered_set<std::int64_t> items;
    for (std::int64_t i = 0; i < 100000; i++)
        if (!items.emplace(i + base).second) std::abort();
    if (items.size() != 100000) std::abort();
    for (std::int64_t i = 0; i < 100000; i++)
        if (!items.contains(i + base)) std::abort();
    for (std::int64_t i = 100000; i < 200000; i++)
        if (items.contains(i + base)) std::abort();
    for (std::int64_t i = 1; i < 100000; i += 2)
        if (items.erase(i + base) != 1) std::abort();
    if (items.size() != 50000) std::abort();
    std::size_t count = 0;
    for (auto value : items) {
        if ((value - base) % 2 != 0) std::abort();
        count++;
    }
    if (count != 50000) std::abort();
}
