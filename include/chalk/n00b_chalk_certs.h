#pragma once

/** @file n00b_chalk_certs.h — X.509 certificate codec.
 *
 *  Cert bytes are immutable; insertion/deletion behave as sidecar
 *  operations (writing/removing `<cert>.chalk`). Extract may return
 *  the mark either from the sidecar (if present) or report no-mark. */

#include <n00b.h>
#include <chalk/n00b_chalk_codec.h>

n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_certs_insert_buffer(n00b_buffer_t *bytes,
                                   n00b_chalk_mark_t *mark);
n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_certs_delete_buffer(n00b_buffer_t *bytes);
n00b_result_t(n00b_chalk_extract_result_t *)
    n00b_chalk_certs_extract_buffer(n00b_buffer_t *bytes);
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_certs_hash_buffer(n00b_buffer_t *bytes);

n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_certs_insert_file(n00b_string_t *path,
                                 n00b_chalk_mark_t *mark);
n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_certs_delete_file(n00b_string_t *path);
n00b_result_t(n00b_chalk_extract_result_t *)
    n00b_chalk_certs_extract_file(n00b_string_t *path);
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_certs_hash_file(n00b_string_t *path);
