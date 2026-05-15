#pragma once

/** @file n00b_chalk_macho.h — Mach-O codec. */

#include <n00b.h>
#include <chalk/n00b_chalk_codec.h>

n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_macho_insert_buffer(n00b_buffer_t *bytes, n00b_chalk_mark_t *mark);
n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_macho_delete_buffer(n00b_buffer_t *bytes);
n00b_result_t(n00b_chalk_extract_result_t *)
    n00b_chalk_macho_extract_buffer(n00b_buffer_t *bytes);
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_macho_hash_buffer(n00b_buffer_t *bytes);

n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_macho_insert_file(n00b_string_t *path, n00b_chalk_mark_t *mark);
n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_macho_delete_file(n00b_string_t *path);
n00b_result_t(n00b_chalk_extract_result_t *)
    n00b_chalk_macho_extract_file(n00b_string_t *path);
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_macho_hash_file(n00b_string_t *path);

/** Detect the code-signature state of a Mach-O binary. Useful to a
 *  caller that wants to know whether re-signing will be needed after
 *  insertion. */
typedef enum {
    N00B_CHALK_MACHO_SIG_NONE,
    N00B_CHALK_MACHO_SIG_ADHOC,
    N00B_CHALK_MACHO_SIG_CERT,
    N00B_CHALK_MACHO_SIG_MALFORMED,
} n00b_chalk_macho_sig_kind_t;

n00b_chalk_macho_sig_kind_t
    n00b_chalk_macho_signature_kind(n00b_buffer_t *bytes);

/** Strip an existing signature so the caller can re-sign after their
 *  own modifications. Returns the new artifact bytes. */
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_macho_strip_signature(n00b_buffer_t *bytes);
