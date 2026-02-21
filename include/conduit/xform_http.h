/**
 * @file xform_http.h
 * @brief HTTP/1.1 parser transform conduit.
 *
 * Incremental state-machine parser for HTTP/1.1 request and response
 * messages.  Consumes raw bytes (`n00b_buffer_t *`) and emits typed
 * parse events (`n00b_http_parse_event_t *`).
 *
 * Supports:
 * - Request and response parsing (auto-detect mode)
 * - Content-Length identity bodies
 * - Chunked transfer-encoding
 * - Configurable header/body size limits
 *
 * ### Usage
 *
 * ```c
 * auto r = n00b_conduit_http_parse_new(c, upstream,
 *     .mode = N00B_HTTP_MODE_REQUEST);
 * auto xf = n00b_result_get(r);
 * ```
 */
#pragma once

#include "conduit/xform_types.h"

// ============================================================================
// HTTP event types
// ============================================================================

typedef enum {
    N00B_HTTP_EVENT_REQUEST_LINE,  /**< Method + URI + version parsed. */
    N00B_HTTP_EVENT_RESPONSE_LINE, /**< Status code + reason parsed. */
    N00B_HTTP_EVENT_HEADER,        /**< Single header name:value. */
    N00B_HTTP_EVENT_HEADERS_DONE,  /**< All headers received. */
    N00B_HTTP_EVENT_BODY_CHUNK,    /**< Body data chunk. */
    N00B_HTTP_EVENT_COMPLETE,      /**< Full message done. */
    N00B_HTTP_EVENT_ERROR,         /**< Parse error. */
} n00b_http_event_type_t;

typedef struct {
    n00b_http_event_type_t type;
    union {
        struct {
            const char *method;
            size_t      method_len;
            const char *uri;
            size_t      uri_len;
            uint8_t     version_major;
            uint8_t     version_minor;
        } request_line;

        struct {
            uint16_t    status;
            const char *reason;
            size_t      reason_len;
            uint8_t     version_major;
            uint8_t     version_minor;
        } response_line;

        struct {
            const char *name;
            size_t      name_len;
            const char *value;
            size_t      value_len;
        } header;

        struct {
            const uint8_t *data;
            size_t         len;
        } body_chunk;

        struct {
            const char *reason;
        } error;
    };
} n00b_http_parse_event_t;

// ============================================================================
// Parser mode
// ============================================================================

typedef enum {
    N00B_HTTP_MODE_REQUEST,  /**< Parse HTTP requests. */
    N00B_HTTP_MODE_RESPONSE, /**< Parse HTTP responses. */
    N00B_HTTP_MODE_AUTO,     /**< Auto-detect from first line. */
} n00b_http_mode_t;

// ============================================================================
// Type instantiations
// ============================================================================

n00b_option_decl(n00b_http_parse_event_t *);
N00B_CONDUIT_FULL_IMPL(n00b_http_parse_event_t *);
N00B_CONDUIT_XFORM_IMPL(n00b_buffer_t *, n00b_http_parse_event_t *);

// ============================================================================
// Parser state (internal)
// ============================================================================

typedef enum {
    N00B_HTTP_STATE_START,
    N00B_HTTP_STATE_HEADER_LINE,
    N00B_HTTP_STATE_BODY_IDENTITY,
    N00B_HTTP_STATE_BODY_CHUNKED,
    N00B_HTTP_STATE_CHUNK_SIZE,
    N00B_HTTP_STATE_CHUNK_DATA,
    N00B_HTTP_STATE_CHUNK_TRAILER,
    N00B_HTTP_STATE_TRAILER,
    N00B_HTTP_STATE_COMPLETE,
    N00B_HTTP_STATE_ERROR,
} n00b_http_state_t;

typedef struct {
    n00b_http_state_t  state;
    n00b_http_mode_t   mode;

    uint8_t   *accum;
    size_t     accum_len;
    size_t     accum_cap;

    size_t     content_length;
    size_t     body_remaining;
    size_t     chunk_remaining;
    bool       chunked;
    bool       has_content_length;

    size_t     max_header_size;
    size_t     max_headers;
    size_t     max_body_size;
    bool       strict;

    size_t     header_bytes;
    size_t     header_count;
    size_t     body_bytes;

    bool       init;
} n00b_http_parse_cookie_t;

// ============================================================================
// API
// ============================================================================

/**
 * @brief Create an HTTP parser transform (buffer -> events).
 *
 * @param c        Conduit instance.
 * @param upstream Upstream topic producing `n00b_buffer_t *` payloads.
 * @kw mode            Parser mode (default: request).
 * @kw max_header_size Maximum size for a single header line (default 8192).
 * @kw max_headers     Maximum number of headers (default 100).
 * @kw max_body_size   Maximum total body size (0 = unlimited).
 * @kw strict          Enable strict HTTP/1.1 compliance.
 *
 * @return Result with xform pointer on success.
 */
extern n00b_result_t(n00b_conduit_xform_t(n00b_buffer_t *, n00b_http_parse_event_t *) *)
n00b_conduit_http_parse_new(
    n00b_conduit_t                        *c,
    n00b_conduit_topic_t(n00b_buffer_t *) *upstream)
    _kargs {
        n00b_http_mode_t mode            = N00B_HTTP_MODE_REQUEST;
        size_t           max_header_size = 8192;
        size_t           max_headers     = 100;
        size_t           max_body_size   = 0;
        bool             strict          = false;
    };
