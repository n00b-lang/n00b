#pragma once

/** @file n00b_chalk_pe.h — PE (Windows .exe / .dll) codec.
 *
 *  Embeds the chalk mark as a `.chalk` section in the PE image.
 *  Parses + rebuilds via n00b's existing PE support
 *  (`compiler/objfile/pe.h`, `pe_build.h`); the wrapper here only
 *  knows about the chalk-side semantics. */

#include <n00b.h>
#include "parsers/json.h"
#include "adt/dict.h"
#include "adt/result.h"
#include <chalk/n00b_chalk_codec.h>

n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_pe_insert_buffer(n00b_buffer_t *bytes, n00b_chalk_mark_t *mark);
n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_pe_delete_buffer(n00b_buffer_t *bytes);
n00b_result_t(n00b_chalk_extract_result_t *)
    n00b_chalk_pe_extract_buffer(n00b_buffer_t *bytes);
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_pe_hash_buffer(n00b_buffer_t *bytes);

n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_pe_insert_file(n00b_string_t *path, n00b_chalk_mark_t *mark);
n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_pe_delete_file(n00b_string_t *path);
n00b_result_t(n00b_chalk_extract_result_t *)
    n00b_chalk_pe_extract_file(n00b_string_t *path);
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_pe_hash_file(n00b_string_t *path);

/** Detect whether a PE file is Authenticode-signed. Adding a chalk
 *  section invalidates Authenticode; the caller is responsible for
 *  re-signing afterwards. */
typedef enum {
    N00B_CHALK_PE_SIG_NONE,
    N00B_CHALK_PE_SIG_AUTHENTICODE,
} n00b_chalk_pe_sig_kind_t;

n00b_chalk_pe_sig_kind_t
    n00b_chalk_pe_signature_kind(n00b_buffer_t *bytes);
