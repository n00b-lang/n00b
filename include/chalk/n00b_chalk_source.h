#pragma once

/** @file n00b_chalk_source.h — Source-code (interpreted languages) codec.
 *
 *  Embeds the mark as a single-line comment using the language's
 *  comment syntax (`#` for shell/python/ruby/perl, `//` for js/go,
 *  `--` for lua, etc.). Language is inferred from a shebang line or
 *  the file extension. */

#include <n00b.h>
#include <chalk/n00b_chalk_codec.h>

n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_source_insert_buffer(n00b_buffer_t *bytes,
                                    n00b_chalk_mark_t *mark);
n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_source_delete_buffer(n00b_buffer_t *bytes);
n00b_result_t(n00b_chalk_extract_result_t *)
    n00b_chalk_source_extract_buffer(n00b_buffer_t *bytes);
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_source_hash_buffer(n00b_buffer_t *bytes);

n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_source_insert_file(n00b_string_t *path,
                                  n00b_chalk_mark_t *mark);
n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_source_delete_file(n00b_string_t *path);
n00b_result_t(n00b_chalk_extract_result_t *)
    n00b_chalk_source_extract_file(n00b_string_t *path);
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_source_hash_file(n00b_string_t *path);
