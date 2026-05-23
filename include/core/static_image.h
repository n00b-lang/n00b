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

typedef struct n00b_static_image_builder_t {
    const n00b_static_image_request_t *request;
    char                              *expr;
    char                              *decls;
    char                              *error;
    n00b_static_image_status_t         status;
} n00b_static_image_builder_t;

typedef n00b_static_image_status_t (*n00b_static_initializer_fn)(
    n00b_static_image_builder_t *);

extern const char *
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

extern void
n00b_static_image_builder_set_expr(n00b_static_image_builder_t *builder,
                                   const char *fmt, ...);

extern void
n00b_static_image_builder_append(n00b_static_image_builder_t *builder,
                                 const char *fmt, ...);

extern n00b_static_image_status_t
n00b_static_image_builder_fail(n00b_static_image_builder_t *builder,
                               n00b_static_image_status_t status,
                               const char *fmt, ...);

extern n00b_static_image_status_t
n00b_static_image_build(const n00b_static_image_request_t *request,
                        n00b_static_image_builder_t *builder);
