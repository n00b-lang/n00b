#include <cstdint>
#include <cstdlib>
#include <unordered_map>

struct Key {
    std::int64_t a, b, c, d;

    bool operator==(const Key &other) const
    {
        return a == other.a && b == other.b && c == other.c && d == other.d;
    }
};

struct Hash {
    std::size_t operator()(const Key &key) const
    {
        return static_cast<std::size_t>(key.a);
    }
};

static Key key_for(std::int64_t i)
{
    return {i, i + 1, i + 2, i + 3};
}

int main(int argc, char **)
{
    const std::int64_t base = argc - 1;
    std::unordered_map<Key, std::int64_t, Hash> items;
    for (std::int64_t i = 0; i < 100000; i++)
        if (!items.emplace(key_for(i + base), i * 3).second) std::abort();

    std::int64_t sum = 0;
    for (std::int64_t i = 0; i < 100000; i++) {
        auto found = items.find(key_for(i + base));
        if (found == items.end()) std::abort();
        sum += found->second;
    }
    if (sum != 14999850000) std::abort();

    for (std::int64_t i = 100000; i < 200000; i++)
        if (items.find(key_for(i + base)) != items.end()) std::abort();
    for (std::int64_t i = 1; i < 100000; i += 2)
        if (items.erase(key_for(i + base)) != 1) std::abort();
    if (items.size() != 50000) std::abort();

    std::size_t count = 0;
    for (const auto &[key, value] : items) {
        if (value != (key.a - base) * 3) std::abort();
        count++;
    }
    if (count != 50000) std::abort();
}
