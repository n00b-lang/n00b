#pragma once

/** @file n00b_chalk_elf.h — Primary ELF codec. */

#include <n00b.h>
#include <chalk/n00b_chalk_codec.h>

n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_elf_insert_buffer(n00b_buffer_t *bytes, n00b_chalk_mark_t *mark);
n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_elf_delete_buffer(n00b_buffer_t *bytes);
n00b_result_t(n00b_chalk_extract_result_t *)
    n00b_chalk_elf_extract_buffer(n00b_buffer_t *bytes);
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_elf_hash_buffer(n00b_buffer_t *bytes);

n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_elf_insert_file(n00b_string_t *path, n00b_chalk_mark_t *mark);
n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_elf_delete_file(n00b_string_t *path);
n00b_result_t(n00b_chalk_extract_result_t *)
    n00b_chalk_elf_extract_file(n00b_string_t *path);
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_elf_hash_file(n00b_string_t *path);

/**
 * @brief Fallback ELF codec: lightweight header scan, no full parsing.
 *
 * Only `extract` and `hash` are supported. `insert` and `delete`
 * return n00b_result_err — chalk's fallback codec doesn't write.
 */
n00b_result_t(n00b_chalk_extract_result_t *)
    n00b_chalk_elf_fallback_extract_buffer(n00b_buffer_t *bytes);
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_elf_fallback_hash_buffer(n00b_buffer_t *bytes);
n00b_result_t(n00b_chalk_extract_result_t *)
    n00b_chalk_elf_fallback_extract_file(n00b_string_t *path);
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_elf_fallback_hash_file(n00b_string_t *path);
