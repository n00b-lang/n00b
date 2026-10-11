#include <cstdint>
#include <cstdlib>
#include <unordered_map>

int main(int argc, char **)
{
    const std::int64_t base = argc - 1;
    std::unordered_map<std::int64_t, std::int64_t> items;
    for (std::int64_t i = 0; i < 100000; i++)
        if (!items.emplace(i + base, i * 3).second) std::abort();

    std::int64_t sum = 0;
    for (std::int64_t i = 0; i < 100000; i++) {
        auto found = items.find(i + base);
        if (found == items.end()) std::abort();
        sum += found->second;
    }
    if (sum != 14999850000) std::abort();

    for (std::int64_t i = 100000; i < 200000; i++)
        if (items.find(i + base) != items.end()) std::abort();
    for (std::int64_t i = 1; i < 100000; i += 2)
        if (items.erase(i + base) != 1) std::abort();
    if (items.size() != 50000) std::abort();

    std::size_t count = 0;
    for (const auto &[key, value] : items) {
        if (value != (key - base) * 3) std::abort();
        count++;
    }
    if (count != 50000) std::abort();
}
