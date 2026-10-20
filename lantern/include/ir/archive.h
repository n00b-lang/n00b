#ifndef IR_ARCHIVE_H
#define IR_ARCHIVE_H

#include "ir/ir.h"
#include "lib/buffer.h"
#include "lib/string.h"

typedef ir_module_t *(*ir_module_lookup_fn)(void *context, core_string_t name);

typedef struct {
    const uint8_t *data;
    size_t length;
} ir_archive_input_t;

bool ir_modules_save(ir_module_t *const *modules, size_t count,
                     core_buffer_t *out);
bool ir_modules_save_references(ir_module_t *const *modules, size_t count,
                                core_buffer_t *out,
                                core_buffer_t *references);
bool ir_modules_load(ir_ctx_t *ctx, const uint8_t *data, size_t length,
                     const ir_profile_t *const *profiles,
                     size_t profile_count, ir_module_lookup_fn lookup,
                     void *lookup_context, ir_module_t **out,
                     size_t capacity, size_t *count);
bool ir_archives_load(ir_ctx_t *ctx, const ir_archive_input_t *inputs,
                      size_t input_count,
                      const ir_profile_t *const *profiles,
                      size_t profile_count, ir_module_lookup_fn lookup,
                      void *lookup_context, ir_module_t **out,
                      size_t capacity, size_t *count);

#endif
