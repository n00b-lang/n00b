#ifndef IR_ARCHIVE_H
#define IR_ARCHIVE_H

#include "ir/ir.h"
#include "lib/buffer.h"

typedef ir_module_t *(*ir_module_lookup_fn)(void *context, const char *name);

bool ir_modules_save(ir_module_t *const *modules, size_t count,
                     core_buffer_t *out);
bool ir_modules_load(ir_ctx_t *ctx, const uint8_t *data, size_t length,
                     const ir_profile_t *const *profiles,
                     size_t profile_count, ir_module_lookup_fn lookup,
                     void *lookup_context, ir_module_t **out,
                     size_t capacity, size_t *count);

#endif
