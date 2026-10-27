#include <stddef.h>

typedef struct db_state {
    int seed;
    int open;
} db_state;

typedef struct iter_state {
    db_state *db;
    int position;
    int open;
} iter_state;

static db_state databases[128];
static iter_state iterators[128];

db_state *
source_open(int seed)
{
    for (size_t i = 0; i < sizeof databases / sizeof databases[0]; i++) {
        if (!databases[i].open) {
            databases[i] = (db_state){.seed = seed, .open = 1};
            return &databases[i];
        }
    }
    return NULL;
}

void
source_close(db_state *db)
{
    db->open = 0;
}

iter_state *
iter_new(db_state *db)
{
    for (size_t i = 0; i < sizeof iterators / sizeof iterators[0]; i++) {
        if (!iterators[i].open) {
            iterators[i] = (iter_state){.db = db, .open = 1};
            return &iterators[i];
        }
    }
    return NULL;
}

int
iter_next(iter_state *iter)
{
    if (!iter->db->open) {
        return -1;
    }
    return iter->db->seed + iter->position++;
}

void
iter_close(iter_state *iter)
{
    iter->open = 0;
}
