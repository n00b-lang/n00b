#pragma once

/** @file n00b_chalk_pyc.h — Python bytecode (.pyc) codec. */

#include <n00b.h>
#include <chalk/n00b_chalk_codec.h>

n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_pyc_insert_buffer(n00b_buffer_t *bytes, n00b_chalk_mark_t *mark);
n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_pyc_delete_buffer(n00b_buffer_t *bytes);
n00b_result_t(n00b_chalk_extract_result_t *)
    n00b_chalk_pyc_extract_buffer(n00b_buffer_t *bytes);
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_pyc_hash_buffer(n00b_buffer_t *bytes);

n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_pyc_insert_file(n00b_string_t *path, n00b_chalk_mark_t *mark);
n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_pyc_delete_file(n00b_string_t *path);
n00b_result_t(n00b_chalk_extract_result_t *)
    n00b_chalk_pyc_extract_file(n00b_string_t *path);
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_pyc_hash_file(n00b_string_t *path);
