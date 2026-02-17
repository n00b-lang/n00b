#include <stdio.h>
#include "n00b.h"
#include "core/llist.h"
#include "core/alloc.h"

n00b_linked_list_decl(char *);

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    n00b_linked_list_t(char *) ll;
    n00b_linked_list_zero(ll);
    n00b_linked_list_append(&ll, "hello,");
    n00b_linked_list_append(&ll, "(cruel)");
    n00b_linked_list_append(&ll, "world");
    auto cur = n00b_linked_list_first(&ll);
    while (cur) {
        printf("%s ", n00b_linked_list_node_contents(cur));
        cur = n00b_linked_list_next(cur);
    }

    printf("\n");
}
