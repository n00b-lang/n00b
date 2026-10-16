#pragma once

#include "compiler_module_literal.h"
#include "lib/diagnostic.h"
#include "lib/list.h"

core_list_decl(core_buffer_t);

typedef struct n00bb_ffi n00bb_ffi_t;

typedef struct {
    core_string_t name;
    core_string_t contents;
} n00bb_ffi_file_t;

n00bb_ffi_t     *n00bb_ffi_new(void);
void             n00bb_ffi_free(n00bb_ffi_t *ffi);
bool             n00bb_ffi_add_contract_dir(n00bb_ffi_t *ffi, core_string_t dir);
bool             n00bb_ffi_generate(void *ffi, core_translation_unit_t *unit, core_string_t name,
                                    core_string_t module, n00bb_literal_origin_t origin, core_string_t text,
                                    core_buffer_t *source, n00bb_native_declarations_t *natives,
                                    n00bb_line_origins_t *origins);
size_t           n00bb_ffi_input_count(const n00bb_ffi_t *ffi);
core_string_t    n00bb_ffi_input_at(const n00bb_ffi_t *ffi, size_t index);
bool             n00bb_ffi_write_headers(const n00bb_ffi_t *ffi, core_string_t output);
size_t           n00bb_ffi_shim_count(const n00bb_ffi_t *ffi);
n00bb_ffi_file_t n00bb_ffi_shim_at(const n00bb_ffi_t *ffi, size_t index);
size_t           n00bb_ffi_archive_count(const n00bb_ffi_t *ffi);
core_string_t    n00bb_ffi_archive_at(const n00bb_ffi_t *ffi, size_t index);
size_t           n00bb_ffi_library_count(const n00bb_ffi_t *ffi);
core_string_t    n00bb_ffi_library_at(const n00bb_ffi_t *ffi, size_t index);
size_t           n00bb_ffi_framework_count(const n00bb_ffi_t *ffi);
core_string_t    n00bb_ffi_framework_at(const n00bb_ffi_t *ffi, size_t index);
size_t           n00bb_ffi_search_path_count(const n00bb_ffi_t *ffi);
core_string_t    n00bb_ffi_search_path_at(const n00bb_ffi_t *ffi, size_t index);
bool             n00bb_ffi_compile_shims(n00bb_ffi_t *ffi, const char *triple, const char *executable_path,
                                         core_list_t(core_buffer_t) *bitcode, core_diagnostics_t *diagnostics);
