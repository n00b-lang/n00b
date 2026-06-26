/* test_http_default_handler.c — exercise the n00b_http_service default
 * (fallback) handler: a handler invoked when an incoming request matches no
 * registered exact (method, path) route, in place of the automatic 404.
 *
 * Covers:
 *   1. A request to a registered exact route hits the exact handler and the
 *      default handler is NOT called.
 *   2. A request to an unmatched dynamic path hits the default handler, which
 *      gets to emit its own body/status.
 *   3. With NO default handler registered, an unmatched path still 404s
 *      (the existing automatic behavior is preserved).
 *
 * Drives real requests through the conduit-based unix-socket listener using
 * the conduit-based unix HTTP client n00b_http_request_unix_sync(), mirroring
 * test_http_service_unix.c.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/file.h"
#include "net/http/http_client.h"
#include "net/http/http_service.h"
#include "util/path.h"

typedef struct {
    int count;
} hit_state_t;

static const char *KNOWN_BODY    = "exact-route";
static const char *FALLBACK_BODY = "fallback";

static void
known_handler(n00b_http_request_t         *req,
              n00b_http_response_writer_t *resp,
              void                        *user_data)
{
    (void)req;
    hit_state_t *state = user_data;
    state->count++;
    n00b_http_response_writer_status(resp, 200);
    n00b_http_response_writer_text(resp,
                                   n00b_string_from_cstr(KNOWN_BODY),
                                   .content_type = r"text/plain");
}

static void
default_handler(n00b_http_request_t         *req,
                n00b_http_response_writer_t *resp,
                void                        *user_data)
{
    (void)req;
    hit_state_t *state = user_data;
    state->count++;
    n00b_http_response_writer_status(resp, 200);
    n00b_http_response_writer_text(resp,
                                   n00b_string_from_cstr(FALLBACK_BODY),
                                   .content_type = r"text/plain");
}

static n00b_string_t *
unique_socket_path(void)
{
    n00b_string_t *prefix = n00b_string_from_cstr("libn00b-http-default-");
    n00b_string_t *suffix = n00b_string_from_cstr(".sock");
    n00b_string_t *path   = n00b_new_temp_path(prefix, suffix);
    (void)n00b_file_unlink(path, .ignore_missing = true);
    return path;
}

static bool
body_equals(n00b_http_response_t *resp, const char *expect)
{
    n00b_buffer_t *body = n00b_http_response_body(resp);
    if (body == nullptr) {
        return false;
    }
    if (body->byte_len != strlen(expect)) {
        return false;
    }
    return memcmp(body->data, expect, strlen(expect)) == 0;
}

static void
test_default_handler(void)
{
    hit_state_t    known_state   = {};
    hit_state_t    default_state = {};
    n00b_string_t *sock          = unique_socket_path();

    n00b_http_service_t *svc = n00b_http_service_new(.socket_path = sock,
                                                     .socket_mode = 0600);
    assert(n00b_result_is_ok(
        n00b_http_service_route(svc, r"GET", r"/known",
                                known_handler, &known_state)));
    assert(n00b_result_is_ok(
        n00b_http_service_set_default_handler(svc, default_handler,
                                              &default_state)));
    auto sr = n00b_http_service_start(svc);
    assert(n00b_result_is_ok(sr));

    // 1. Exact route hits the exact handler; default NOT called.
    auto kr = n00b_http_request_unix_sync(sock, r"/known");
    assert(n00b_result_is_ok(kr));
    n00b_http_response_t *kresp = n00b_result_get(kr);
    assert(n00b_http_response_status(kresp) == 200);
    assert(body_equals(kresp, KNOWN_BODY));
    assert(known_state.count == 1);
    assert(default_state.count == 0);
    printf("  [PASS] exact route hits exact handler (default not called)\n");

    // 2. Unmatched dynamic path falls back to the default handler.
    auto dr = n00b_http_request_unix_sync(sock, r"/unknown/dynamic/123");
    assert(n00b_result_is_ok(dr));
    n00b_http_response_t *dresp = n00b_result_get(dr);
    assert(n00b_http_response_status(dresp) == 200);
    assert(body_equals(dresp, FALLBACK_BODY));
    assert(default_state.count == 1);
    assert(known_state.count == 1);
    printf("  [PASS] unmatched path hits default handler\n");

    n00b_http_service_stop(svc);
    (void)n00b_file_unlink(sock, .ignore_missing = true);
}

static void
test_no_default_still_404(void)
{
    hit_state_t    known_state = {};
    n00b_string_t *sock        = unique_socket_path();

    n00b_http_service_t *svc = n00b_http_service_new(.socket_path = sock,
                                                     .socket_mode = 0600);
    assert(n00b_result_is_ok(
        n00b_http_service_route(svc, r"GET", r"/known",
                                known_handler, &known_state)));
    auto sr = n00b_http_service_start(svc);
    assert(n00b_result_is_ok(sr));

    // With no default handler, an unmatched path still 404s.
    auto nr = n00b_http_request_unix_sync(sock, r"/unknown/dynamic/123");
    assert(n00b_result_is_ok(nr));
    assert(n00b_http_response_status(n00b_result_get(nr)) == 404);
    assert(known_state.count == 0);
    printf("  [PASS] no default handler → unmatched path still 404\n");

    n00b_http_service_stop(svc);
    (void)n00b_file_unlink(sock, .ignore_missing = true);
}

static void
test_405_preserved_with_default(void)
{
    /* Path matches a route but the method does not: the automatic 405 is
     * preserved and the default handler is NOT called, even though one is
     * registered. The GET-only unix client drives a GET against a POST-only
     * route to force the method mismatch. */
    hit_state_t    known_state   = {};
    hit_state_t    default_state = {};
    n00b_string_t *sock          = unique_socket_path();

    n00b_http_service_t *svc = n00b_http_service_new(.socket_path = sock,
                                                     .socket_mode = 0600);
    assert(n00b_result_is_ok(
        n00b_http_service_route(svc, r"POST", r"/known",
                                known_handler, &known_state)));
    assert(n00b_result_is_ok(
        n00b_http_service_set_default_handler(svc, default_handler,
                                              &default_state)));
    auto sr = n00b_http_service_start(svc);
    assert(n00b_result_is_ok(sr));

    auto mr = n00b_http_request_unix_sync(sock, r"/known"); // GET vs POST route
    assert(n00b_result_is_ok(mr));
    assert(n00b_http_response_status(n00b_result_get(mr)) == 405);
    assert(known_state.count == 0);   // wrong-method route handler not run
    assert(default_state.count == 0); // 405 preserved; default NOT invoked
    printf("  [PASS] method mismatch → 405 preserved (default not called)\n");

    n00b_http_service_stop(svc);
    (void)n00b_file_unlink(sock, .ignore_missing = true);
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime = {};
    n00b_init(&runtime, argc, argv);

    printf("Running http_service default-handler tests...\n");
    test_default_handler();
    test_405_preserved_with_default();
    test_no_default_still_404();
    printf("All http_service default-handler tests passed.\n");

    n00b_shutdown();
    return 0;
}
