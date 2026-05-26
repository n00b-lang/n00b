/**
 * @file static_image.h
 * @brief Build-time static-image contract helpers.
 */
#pragma once

#include "n00b.h"
#include "core/type_info.h"

typedef enum n00b_static_image_status_t : uint8_t {
    N00B_STATIC_IMAGE_OK = 0,
    N00B_STATIC_IMAGE_ERR_NULL_REQUEST,
    N00B_STATIC_IMAGE_ERR_VERSION,
    N00B_STATIC_IMAGE_ERR_ABI,
    N00B_STATIC_IMAGE_ERR_PAYLOAD,
    N00B_STATIC_IMAGE_ERR_ARGUMENT,
    N00B_STATIC_IMAGE_ERR_UNREGISTERED_TYPE,
    N00B_STATIC_IMAGE_ERR_UNSUPPORTED_POLICY,
    N00B_STATIC_IMAGE_ERR_SCAN_KIND,
    N00B_STATIC_IMAGE_ERR_NO_INITIALIZER,
    N00B_STATIC_IMAGE_ERR_INITIALIZER,
} n00b_static_image_status_t;

/**
 * @brief Accumulator passed to a registered static initializer.
 *
 * `expr` and `decls` are written by the initializer; `error` is set by
 * `n00b_static_image_builder_fail()` (or by the dispatch path) when
 * `status` is non-OK.  All three are n00b strings — the public surface
 * does not expose raw `char *` buffers.
 */
typedef struct n00b_static_image_builder_t {
    const n00b_static_image_request_t *request;
    n00b_string_t                     *expr;
    n00b_string_t                     *decls;
    n00b_string_t                     *error;
    n00b_static_image_status_t         status;
} n00b_static_image_builder_t;

typedef n00b_static_image_status_t (*n00b_static_initializer_fn)(
    n00b_static_image_builder_t *);

extern n00b_string_t *
n00b_static_image_status_name(n00b_static_image_status_t status);

extern bool
n00b_static_image_abi_matches_host(const n00b_static_image_abi_t *abi);

extern n00b_static_image_status_t
n00b_static_image_validate_request(const n00b_static_image_request_t *request);

extern void
n00b_static_image_builder_init(n00b_static_image_builder_t *builder,
                               const n00b_static_image_request_t *request);

extern void
n00b_static_image_builder_destroy(n00b_static_image_builder_t *builder);

/**
 * @brief Set the builder's `expr` field to a caller-formatted string.
 *
 * The caller is expected to pre-format with `n00b_cformat()` /
 * `n00b_format()` if substitution is needed; this entry point takes
 * the finished n00b string and stores it.
 */
extern void
n00b_static_image_builder_set_expr(n00b_static_image_builder_t *builder,
                                   n00b_string_t *expr);

/**
 * @brief Append `chunk` to the builder's `decls` field.
 *
 * Concatenates `chunk` onto whatever the initializer has accumulated
 * so far.  Pre-format via `n00b_cformat()` / `n00b_format()` if you
 * need substitution.
 */
extern void
n00b_static_image_builder_append(n00b_static_image_builder_t *builder,
                                 n00b_string_t *chunk);

/**
 * @brief Record a static-image build failure.
 *
 * Stores @p msg on the builder, marks the builder with @p status, and
 * returns @p status so callers can `return` the call directly.
 */
extern n00b_static_image_status_t
n00b_static_image_builder_fail(n00b_static_image_builder_t *builder,
                               n00b_static_image_status_t status,
                               n00b_string_t *msg);

extern n00b_static_image_status_t
n00b_static_image_build(const n00b_static_image_request_t *request,
                        n00b_static_image_builder_t *builder);
