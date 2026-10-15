#pragma once

#include "compiler_module_literal.h"

typedef struct n00bb_ffi n00bb_ffi_t;

typedef struct {
    core_string_t name;
    core_string_t contents;
} n00bb_ffi_file_t;

n00bb_ffi_t     *n00bb_ffi_new(void);
void             n00bb_ffi_free(n00bb_ffi_t *ffi);
bool             n00bb_ffi_add_contract_dir(n00bb_ffi_t *ffi, core_string_t dir);
bool             n00bb_ffi_generate(void *ffi, core_translation_unit_t *unit, core_string_t name,
                                    n00bb_literal_origin_t origin, core_string_t text, core_buffer_t *source);
size_t           n00bb_ffi_input_count(const n00bb_ffi_t *ffi);
core_string_t    n00bb_ffi_input_at(const n00bb_ffi_t *ffi, size_t index);
size_t           n00bb_ffi_shim_count(const n00bb_ffi_t *ffi);
n00bb_ffi_file_t n00bb_ffi_shim_at(const n00bb_ffi_t *ffi, size_t index);
size_t           n00bb_ffi_library_count(const n00bb_ffi_t *ffi);
core_string_t    n00bb_ffi_library_at(const n00bb_ffi_t *ffi, size_t index);
size_t           n00bb_ffi_framework_count(const n00bb_ffi_t *ffi);
core_string_t    n00bb_ffi_framework_at(const n00bb_ffi_t *ffi, size_t index);
size_t           n00bb_ffi_search_path_count(const n00bb_ffi_t *ffi);
core_string_t    n00bb_ffi_search_path_at(const n00bb_ffi_t *ffi, size_t index);
