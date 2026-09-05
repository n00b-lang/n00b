#ifndef IR_INTERNAL_H
#define IR_INTERNAL_H

#include "ir/ir.h"

#define IR_CHUNK 16384

typedef struct ir_chunk {
    struct ir_chunk *next;
    size_t           used;
    size_t           cap;
    unsigned char    data[];
} ir_chunk;

struct ir_ctx_t {
    ir_chunk *chunks;
    th_ctx   *types;
};

typedef struct {
    uint64_t  hash;
    th_type  *type;
} ir_typeslot_t;

struct ir_module_t {
    ir_ctx_t           *ctx;
    const char         *name;
    const char         *target;
    const ir_profile_t *profile;
    ir_props_t          props;
    ir_annots_t         annots;

    ir_typeslot_t *types;
    size_t         n_types;
    size_t         cap_types;

    uint64_t *scalar_hashes;
    bool      scalars_ready;

    const char **files;
    size_t       n_files;
    size_t       cap_files;

    ir_global_t **globals;
    size_t        n_globals;
    size_t        cap_globals;

    ir_func_t **funcs;
    size_t      n_funcs;
    size_t      cap_funcs;
};

void *ir_grow(ir_ctx_t *ctx, void *p, size_t *cap, size_t need, size_t elem);

#endif
