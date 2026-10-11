#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <unordered_map>
#include <vector>

struct Key {
    std::int64_t a, b, c, d;
    bool operator==(const Key &) const = default;
};

struct KeyHash {
    std::size_t operator()(const Key &key) const noexcept {
        return std::hash<std::int64_t>{}(key.a);
    }
};

static inline void keep_materialized(const void *pointer) {
    asm volatile("" : : "r"(pointer) : "memory");
}

int main(int argc, char **)
{
    const auto started = std::chrono::steady_clock::now();
    const std::int64_t base = argc - 1;
    std::unordered_map<std::int64_t, std::int64_t> integers;
    std::unordered_map<Key, std::int64_t, KeyHash> records;
    for (std::int64_t i = 0; i < 10000; i++) {
        integers.emplace(i + base, i * 3);
        records.emplace(Key{i + base, i + 1, i + 2, i + 3}, i * 3);
    }
    std::int64_t total = 0;
    for (int pass = 0; pass < 100; pass++) {
        std::vector<std::int64_t> int_keys, int_values, record_values;
        std::vector<Key> record_keys;
        int_keys.reserve(integers.size());
        int_values.reserve(integers.size());
        record_keys.reserve(records.size());
        record_values.reserve(records.size());
        for (const auto &[key, value] : integers) {
            int_keys.push_back(key);
            int_values.push_back(value);
        }
        for (const auto &[key, value] : records) {
            record_keys.push_back(key);
            record_values.push_back(value);
        }
        keep_materialized(int_keys.data());
        keep_materialized(int_values.data());
        keep_materialized(record_keys.data());
        keep_materialized(record_values.data());
        total += int_keys.front() - base + int_values.back()
            + record_keys.front().a - base + record_values.back();
    }
    if (total <= 0) std::abort();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    std::fprintf(stderr, "view_seconds=%.6f\n",
                 std::chrono::duration<double>(elapsed).count());
}
