#include <stddef.h>

typedef struct {
    int   *items;
    size_t len;
    size_t cap;
} my_list_t;
