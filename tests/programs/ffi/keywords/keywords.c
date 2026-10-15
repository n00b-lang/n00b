#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct OpenOpts {
    uint32_t present;
    uint32_t timeout_ms;
    bool readonly;
    uint32_t cache_kb;
} OpenOpts;

typedef struct Limits {
    int64_t max_rows;
    int64_t min_rows;
} Limits;

typedef struct Sized {
    uint32_t size;
    uint32_t flags;
} Sized;

typedef struct Plain {
    int depth;
} Plain;

typedef struct db_state {
    OpenOpts opts;
    int open;
} db_state;

static db_state databases[4];

db_state *
db_open(const char *path, OpenOpts *opts)
{
    (void)path;
    for (size_t i = 0; i < sizeof databases / sizeof databases[0]; i++) {
        if (!databases[i].open) {
            databases[i] = (db_state){.opts = *opts, .open = 1};
            return &databases[i];
        }
    }
    return NULL;
}

uint32_t
db_present(db_state *db)
{
    return db->opts.present;
}

uint32_t
db_timeout(db_state *db)
{
    return db->opts.timeout_ms;
}

uint32_t
db_cache(db_state *db)
{
    return db->opts.cache_kb;
}

int64_t
db_limit(db_state *db, Limits *limits)
{
    (void)db;
    return limits->max_rows * 1000 + limits->min_rows;
}

uint32_t
db_flags(db_state *db, Sized *sized)
{
    (void)db;
    return sized->size * 100 + sized->flags;
}

int
db_depth(db_state *db, Plain *plain)
{
    (void)db;
    return plain->depth;
}

void
db_close(db_state *db)
{
    if (db) db->open = 0;
}
