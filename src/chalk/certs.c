/** @file src/chalk/certs.c — X.509 certificate codec (sidecar variant).
 *
 *  Cert bytes are immutable; libchalk handles certs identically to
 *  model sidecars (`<cert>.chalk` sibling file), differing only in
 *  the codec id stamped into extract_result.codec.
 *
 *  Cert metadata extraction (subject, issuer, serial, etc., done by
 *  chalk's OpenSSL-linked utils/certs.c) is intentionally not ported:
 *  none of those fields are in libchalk's six-key set, and dropping
 *  it lets libchalk skip the OpenSSL dependency. */

#include "n00b.h"
#include "chalk/n00b_chalk.h"
#include "internal/chalk/sidecar_internal.h"

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_certs_insert_buffer(n00b_buffer_t *bytes, n00b_chalk_mark_t *mark)
{
    return n00b_chalk_sidecar_insert_impl(bytes, mark);
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_certs_delete_buffer(n00b_buffer_t *bytes)
{
    return n00b_chalk_sidecar_delete_impl(bytes);
}

n00b_result_t(n00b_chalk_extract_result_t *)
n00b_chalk_certs_extract_buffer(n00b_buffer_t *bytes)
{
    // Cert bytes never carry the mark.
    (void)bytes;
    return n00b_result_err(n00b_chalk_extract_result_t *, 1);
}

n00b_result_t(n00b_buffer_t *)
n00b_chalk_certs_hash_buffer(n00b_buffer_t *bytes)
{
    return n00b_chalk_sidecar_hash_impl(bytes);
}

#include "internal/chalk/file_io.h"

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_certs_insert_file(n00b_string_t *path, n00b_chalk_mark_t *mark)
{
    return n00b_chalk_file_insert_via(path, mark,
                                      n00b_chalk_certs_insert_buffer);
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_certs_delete_file(n00b_string_t *path)
{
    return n00b_chalk_file_delete_via(path, n00b_chalk_certs_delete_buffer);
}

n00b_result_t(n00b_chalk_extract_result_t *)
n00b_chalk_certs_extract_file(n00b_string_t *path)
{
    auto rr = n00b_chalk_read_sidecar(path);
    if (n00b_result_is_err(rr)) {
        return n00b_result_err(n00b_chalk_extract_result_t *, 1);
    }
    return n00b_chalk_sidecar_parse_bytes(n00b_result_get(rr),
                                          N00B_CHALK_CODEC_SIDECAR_CERT);
}

n00b_result_t(n00b_buffer_t *)
n00b_chalk_certs_hash_file(n00b_string_t *path)
{
    // Plain SHA-256 of the cert bytes — no transformation needed.
    return n00b_chalk_hash_file_stream(path);
}
