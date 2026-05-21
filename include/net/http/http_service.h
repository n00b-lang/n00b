/**
 * @file http_service.h
 * @brief Small local HTTP/1 service/router.
 *
 * This API is intentionally narrow: it is for local tools, endpoint
 * harnesses, and small service slices that need deterministic HTTP/1
 * request handling without production ingress machinery.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "n00b.h"
#include "core/buffer.h"
#include "core/string.h"
#include "adt/result.h"
#include "conduit/service.h"

typedef struct n00b_http_service          n00b_http_service_t;
typedef struct n00b_http_request          n00b_http_request_t;
typedef struct n00b_http_response_writer  n00b_http_response_writer_t;

typedef void (*n00b_http_handler_fn)(n00b_http_request_t         *req,
                                     n00b_http_response_writer_t *resp,
                                     void                        *user_data);

/**
 * @brief Create a local HTTP service.
 *
 * @kw bind_host        Address to bind. Default `127.0.0.1`.
 * @kw bind_port        Port to bind. Use 0 for an ephemeral port.
 * @kw max_header_bytes Maximum request header bytes. Default 16 KiB.
 * @kw max_body_bytes   Maximum request body bytes. Default 1 MiB.
 * @kw backlog          Listen backlog. Default 128.
 * @kw worker_service   Optional conduit service for handler dispatch.
 * @kw allocator        Allocator for service-owned state.
 */
extern n00b_http_service_t *
n00b_http_service_new()
    _kargs {
        n00b_string_t          *bind_host        = nullptr;
        uint16_t                bind_port        = 0;
        size_t                  max_header_bytes = 16384;
        size_t                  max_body_bytes   = 1048576;
        int                     backlog          = 128;
        n00b_conduit_service_t *worker_service   = nullptr;
        n00b_allocator_t       *allocator        = nullptr;
    };

/**
 * @brief Register an exact method/path route.
 *
 * Routes must be registered before `n00b_http_service_start`.
 */
extern n00b_result_t(bool)
n00b_http_service_route(n00b_http_service_t *svc,
                        n00b_string_t       *method,
                        n00b_string_t       *path,
                        n00b_http_handler_fn handler,
                        void                *user_data);

extern n00b_result_t(bool)
n00b_http_service_start(n00b_http_service_t *svc);

extern void
n00b_http_service_stop(n00b_http_service_t *svc);

extern uint16_t
n00b_http_service_port(n00b_http_service_t *svc);

extern n00b_string_t *
n00b_http_request_method(n00b_http_request_t *req);

extern n00b_string_t *
n00b_http_request_path(n00b_http_request_t *req);

extern n00b_string_t *
n00b_http_request_query(n00b_http_request_t *req);

extern n00b_buffer_t *
n00b_http_request_body(n00b_http_request_t *req);

extern n00b_string_t *
n00b_http_request_header(n00b_http_request_t *req, n00b_string_t *name);

extern void
n00b_http_response_writer_status(n00b_http_response_writer_t *resp,
                                 uint16_t                    status);

extern void
n00b_http_response_writer_header(n00b_http_response_writer_t *resp,
                                 n00b_string_t               *name,
                                 n00b_string_t               *value);

extern void
n00b_http_response_writer_body(n00b_http_response_writer_t *resp,
                               n00b_buffer_t               *body);

extern void
n00b_http_response_writer_text(n00b_http_response_writer_t *resp,
                               n00b_string_t               *body,
                               n00b_string_t               *content_type);
