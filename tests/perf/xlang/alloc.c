#include <stdint.h>

#include "lib/alloc.h"

typedef struct node_t {
    int64_t value;
    struct node_t *next;
} node_t;

static int64_t
run(int64_t rounds, int64_t length)
{
    int64_t total = 0;
    for (int64_t round = 0; round < rounds; round++) {
        node_t *head = nullptr;
        for (int64_t i = 0; i < length; i++) {
            node_t *node = core_alloc(node_t);
            node->value = i;
            node->next = head;
            head = node;
        }
        while (head) {
            node_t *next = head->next;
            total += head->value;
            core_free(head);
            head = next;
        }
    }
    return total;
}

int
main(int argc, char **argv)
{
    (void)argv;
    return run(60 * argc, 100000) == 299997000000 ? 0 : 1;
}
