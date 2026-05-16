#pragma once

/** @file n00b_chalk_macos_wrap.h — Bash-script-wrapper codec.
 *
 *  Wraps a native binary as a self-extracting bash script. Marking
 *  happens against the embedded base64 blob. */

#include <n00b.h>
#include <chalk/n00b_chalk_codec.h>

n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_macos_wrap_insert_buffer(n00b_buffer_t *bytes,
                                        n00b_chalk_mark_t *mark);
n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_macos_wrap_delete_buffer(n00b_buffer_t *bytes);
n00b_result_t(n00b_chalk_extract_result_t *)
    n00b_chalk_macos_wrap_extract_buffer(n00b_buffer_t *bytes);
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_macos_wrap_hash_buffer(n00b_buffer_t *bytes);

n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_macos_wrap_insert_file(n00b_string_t *path,
                                      n00b_chalk_mark_t *mark);
n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_macos_wrap_delete_file(n00b_string_t *path);
n00b_result_t(n00b_chalk_extract_result_t *)
    n00b_chalk_macos_wrap_extract_file(n00b_string_t *path);
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_macos_wrap_hash_file(n00b_string_t *path);
