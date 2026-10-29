#include <stdint.h>

#include "lib/dict.h"
#include "lib/list.h"

core_list_decl(int64_t);

static int64_t
run(int64_t rounds, int64_t size)
{
    int64_t total = 0;
    for (int64_t round = 0; round < rounds; round++) {
        core_list_t(int64_t) items = core_list_new(int64_t);
        core_dict_t table;
        core_dict_init(&table, nullptr, nullptr);
        for (int64_t i = 0; i < size; i++) {
            core_list_push(items, i);
            core_dict_put(&table, (void *)(intptr_t)i, (void *)(intptr_t)(i * 2));
        }
        for (size_t i = 0; i < items.len; i++) {
            bool found = false;
            void *value = core_dict_get(&table, (void *)(intptr_t)items.data[i],
                                        &found);
            if (found) total += (int64_t)(intptr_t)value;
        }
        core_dict_free(&table);
        core_list_free(items);
    }
    return total;
}

int
main(int argc, char **argv)
{
    (void)argv;
    return run(50 * argc, 200000) == 1999990000000 ? 0 : 1;
}
