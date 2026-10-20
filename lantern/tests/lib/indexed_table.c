#include "lib/indexed_table.h"
#include "harness.h"

#include <string.h>

#define S(x) CORE_STRING_STATIC(x)

static core_hash_value_t
colliding_hash(void *key)
{
    (void)key;
    return 7;
}

static bool
string_key_eq(const void *a, const void *b)
{
    if (a == b) return true;
    if (!a || !b) return false;
    return core_string_eq(*(const core_string_t *)a,
                          *(const core_string_t *)b);
}

int main(void)
{
    int first = 1;
    int later = 2;
    bool inserted = false;
    core_string_t second = S("second");
    core_string_t nil = S("nil");
    core_string_t other = S("other");
    core_string_t alpha_name = S("alpha");
    core_string_t overflow = S("overflow");
    core_string_t one = S("one");
    core_indexed_table_t *table = core_indexed_table_new(
        2, 35, colliding_hash, string_key_eq);
    check("table with reserved prefix", table
          && core_indexed_table_len(table) == 2
          && !core_indexed_table_at(table, 0)
          && !core_indexed_table_at(table, 1));
    check("reserve nonzero prefix row",
          core_indexed_table_reserve(table, 1, &second, &first));
    check("reserve row zero",
          core_indexed_table_reserve(table, 0, &nil, &first));
    check("reserved row keeps its index",
          core_indexed_table_find(table, &nil)->index == 0
          && core_indexed_table_at(table, 1)->index == 1);
    check("occupied and duplicate reserved rows rejected",
          !core_indexed_table_reserve(table, 0, &other, &later)
          && !core_indexed_table_reserve(table, 1, &nil, &later)
          && !core_indexed_table_reserve(table, 2, &other, &later)
          && !core_indexed_table_reserve(table, 0, nullptr, &later));

    char local_bytes[] = "alpha";
    core_string_t local_key = core_string_borrow(local_bytes, 5);
    const core_indexed_entry_t *alpha = core_indexed_table_intern(
        table, &local_key, &first, &inserted);
    check("first encounter appended after prefix",
          alpha && inserted && alpha->index == 2 && alpha->value == &first);
    char other_tu_bytes[] = "alpha";
    core_string_t other_tu_key = core_string_borrow(other_tu_bytes, 5);
    const core_indexed_entry_t *duplicate = core_indexed_table_intern(
        table, &other_tu_key, &later, &inserted);
    check("equal key from another input keeps first row and value",
          !inserted && duplicate == alpha && duplicate->key == &local_key
          && duplicate->value == &first);
    const core_indexed_entry_t *reserved_duplicate =
        core_indexed_table_intern(table, &nil, &later, &inserted);
    check("reserved identity deduplicates without replacement",
          !inserted && reserved_duplicate->index == 0
          && reserved_duplicate->value == &first);

    const core_indexed_entry_t *stable = alpha;
    char key_bytes[33][16];
    core_string_t keys[33];
    for (size_t i = 0; i < 33; i++) {
        int length = snprintf(key_bytes[i], sizeof(key_bytes[i]),
                              "key-%zu", i);
        keys[i] = core_string_borrow(key_bytes[i], (size_t)length);
        const core_indexed_entry_t *entry = core_indexed_table_intern(
            table, &keys[i], &later, &inserted);
        check("colliding key added in input order",
              entry && inserted && entry->index == i + 3);
    }
    check("entry pointer survives row and dictionary growth",
          core_indexed_table_at(table, 2) == stable
          && core_indexed_table_find(table, &alpha_name) == stable);
    check("index cap rejects a new row without changing the table",
          !core_indexed_table_intern(table, &overflow, &later, &inserted)
          && !inserted && core_indexed_table_len(table) == 36
          && !core_indexed_table_find(table, &overflow));
    check("duplicate remains available at index cap",
          core_indexed_table_intern(table, &other_tu_key, &later,
                                     &inserted) == stable && !inserted);
    check("invalid key and out-of-range row fail",
          !core_indexed_table_intern(table, nullptr, &later, &inserted)
          && !core_indexed_table_at(table, 36));

    core_indexed_table_t *dummy = core_indexed_table_new(
        1, 1, colliding_hash, string_key_eq);
    const core_indexed_entry_t *only = core_indexed_table_intern(
        dummy, &one, &first, nullptr);
    check("unfilled reserved row remains a hole",
          !core_indexed_table_at(dummy, 0) && only && only->index == 1);
    check("invalid reserved count is rejected",
          !core_indexed_table_new(2, 0, colliding_hash,
                                  string_key_eq));

    core_index_remap_t *remap = core_index_remap_new(3, UINT32_MAX);
    uint32_t final = 99;
    check("unassigned index is not zero",
          !core_index_remap_get(remap, 0, &final) && final == 99);
    check("zero and maximum final indexes are legal",
          core_index_remap_set(remap, 0, 0)
          && core_index_remap_set(remap, 2, UINT32_MAX)
          && core_index_remap_get(remap, 0, &final) && final == 0
          && core_index_remap_get(remap, 2, &final)
          && final == UINT32_MAX);
    check("remap rejects reassignment and out-of-range accesses",
          !core_index_remap_set(remap, 0, 1)
          && !core_index_remap_set(remap, 3, 1)
          && !core_index_remap_get(remap, 1, &final)
          && !core_index_remap_get(remap, 3, &final)
          && !core_index_remap_get(remap, 0, nullptr));
    core_index_remap_t *narrow = core_index_remap_new(1, 0x7fffffff);
    check("client-defined final index limit is enforced",
          !core_index_remap_set(narrow, 0, 0x80000000)
          && core_index_remap_set(narrow, 0, 0x7fffffff));
    core_index_remap_t *empty = core_index_remap_new(0, 0);
    check("empty remap has no addressable rows",
          core_index_remap_len(empty) == 0
          && !core_index_remap_set(empty, 0, 0));

    core_index_remap_free(empty);
    core_index_remap_free(narrow);
    core_index_remap_free(remap);
    core_indexed_table_free(dummy);
    core_indexed_table_free(table);
    return report("lib-indexed-table");
}
