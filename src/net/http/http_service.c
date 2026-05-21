/*
 * http_service.c — Small local HTTP/1 service/router.
 */

#include "net/http/http_service.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include "internal/win32_sockets.h"
#define N00B_HTTP_SOCK_T   SOCKET
#define N00B_HTTP_BAD_SOCK INVALID_SOCKET
#define N00B_HTTP_SOCK_ERR WSAGetLastError()
#define N00B_HTTP_CLOSE(s) closesocket((SOCKET)(s))
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define N00B_HTTP_SOCK_T   int
#define N00B_HTTP_BAD_SOCK (-1)
#define N00B_HTTP_SOCK_ERR errno
#define N00B_HTTP_CLOSE(s) close(s)
#endif

#include "adt/list.h"
#include "core/alloc.h"
#include "core/thread.h"

typedef struct {
    n00b_string_t       *method;
    n00b_string_t       *path;
    n00b_http_handler_fn handler;
    void                *user_data;
} n00b_http_route_t;

typedef struct {
    n00b_string_t *name;
    n00b_string_t *value;
} n00b_http_header_t;

struct n00b_http_request {
    n00b_string_t                 *method;
    n00b_string_t                 *path;
    n00b_string_t                 *query;
    n00b_list_t(n00b_http_header_t *) headers;
    n00b_buffer_t                 *body;
};

struct n00b_http_response_writer {
    uint16_t                        status;
    n00b_list_t(n00b_http_header_t *) headers;
    n00b_buffer_t                  *body;
};

struct n00b_http_service {
    n00b_string_t                 *bind_host;
    uint16_t                       bind_port;
    uint16_t                       actual_port;
    size_t                         max_header_bytes;
    size_t                         max_body_bytes;
    int                            backlog;
    n00b_conduit_service_t        *worker_service;
    n00b_allocator_t              *allocator;
    n00b_list_t(n00b_http_route_t *) routes;
    n00b_thread_t                 *listener_thread;
    N00B_HTTP_SOCK_T               listener_fd;
    _Atomic(bool)                  started;
    _Atomic(bool)                  stopping;
};

typedef struct {
    n00b_http_service_t *svc;
    N00B_HTTP_SOCK_T     fd;
} n00b_http_client_job_t;

static bool
str_eq(n00b_string_t *a, n00b_string_t *b)
{
    if (a == nullptr || b == nullptr) return false;
    if (a->u8_bytes != b->u8_bytes) return false;
    if (a->u8_bytes == 0) return true;
    return memcmp(a->data, b->data, a->u8_bytes) == 0;
}

static bool
ascii_eq_ci(n00b_string_t *a, n00b_string_t *b)
{
    if (a == nullptr || b == nullptr) return false;
    if (a->u8_bytes != b->u8_bytes) return false;
    for (size_t i = 0; i < a->u8_bytes; i++) {
        char ca = a->data[i];
        char cb = b->data[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + ('a' - 'A'));
        if (ca != cb) return false;
    }
    return true;
}

static n00b_buffer_t *
buf_from_cstr(const char *s)
{
    return n00b_buffer_from_bytes((char *)s, (int64_t)strlen(s));
}

static void
headers_push(n00b_list_t(n00b_http_header_t *) *headers,
             n00b_string_t *name,
             n00b_string_t *value)
{
    n00b_http_header_t *h = n00b_alloc(n00b_http_header_t);
    h->name  = name;
    h->value = value;
    n00b_list_push(*headers, h);
}

static n00b_string_t *
headers_get(n00b_list_t(n00b_http_header_t *) *headers, n00b_string_t *name)
{
    size_t n = n00b_list_len(*headers);
    for (size_t i = 0; i < n; i++) {
        n00b_http_header_t *h = n00b_list_get(*headers, i);
        if (ascii_eq_ci(h->name, name)) {
            return h->value;
        }
    }
    return nullptr;
}

static bool
send_all(N00B_HTTP_SOCK_T fd, const char *p, size_t n)
{
    size_t off = 0;
    while (off < n) {
#ifdef _WIN32
        int chunk = (n - off) > INT32_MAX ? INT32_MAX : (int)(n - off);
        int rc = send(fd, p + off, chunk, 0);
#else
        ssize_t rc = send(fd, p + off, n - off, 0);
#endif
        if (rc <= 0) {
            return false;
        }
        off += (size_t)rc;
    }
    return true;
}

static bool
send_cstr(N00B_HTTP_SOCK_T fd, const char *s)
{
    return send_all(fd, s, strlen(s));
}

static bool
read_request_bytes(n00b_http_service_t *svc,
                   N00B_HTTP_SOCK_T     fd,
                   char               **out,
                   size_t              *out_len,
                   size_t              *header_end,
                   int                 *status_out)
{
    size_t cap = 4096;
    size_t len = 0;
    char  *buf = n00b_alloc_array(char, cap + 1);
    size_t hdr = (size_t)-1;

    while (hdr == (size_t)-1) {
        if (len == cap) {
            cap *= 2;
            char *next = n00b_alloc_array(char, cap + 1);
            memcpy(next, buf, len);
            buf = next;
        }

#ifdef _WIN32
        int rc = recv(fd, buf + len, (int)(cap - len), 0);
#else
        ssize_t rc = recv(fd, buf + len, cap - len, 0);
#endif
        if (rc <= 0) {
            *status_out = 400;
            return false;
        }
        len += (size_t)rc;
        buf[len] = '\0';

        for (size_t i = 0; i + 4 <= len; i++) {
            if (buf[i] == '\r' && buf[i + 1] == '\n'
                && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                hdr = i + 4;
                break;
            }
        }

        if (hdr == (size_t)-1 && len > svc->max_header_bytes) {
            *status_out = 431;
            return false;
        }
    }

    if (hdr > svc->max_header_bytes) {
        *status_out = 431;
        return false;
    }

    *out        = buf;
    *out_len    = len;
    *header_end = hdr;
    return true;
}

static char *
copy_span(const char *start, size_t len)
{
    char *out = n00b_alloc_array(char, len + 1);
    if (len != 0) {
        memcpy(out, start, len);
    }
    out[len] = '\0';
    return out;
}

static char *
find_line_end(char *p, char *end)
{
    while (p + 1 < end) {
        if (p[0] == '\r' && p[1] == '\n') {
            return p;
        }
        p++;
    }
    return nullptr;
}

static bool
parse_content_length(n00b_http_request_t *req, size_t *out)
{
    n00b_string_t *v = headers_get(&req->headers, r"content-length");
    if (v == nullptr) {
        *out = 0;
        return true;
    }

    size_t n = 0;
    for (size_t i = 0; i < v->u8_bytes; i++) {
        char c = v->data[i];
        if (c < '0' || c > '9') return false;
        n = n * 10 + (size_t)(c - '0');
    }
    *out = n;
    return true;
}

static bool
parse_request(n00b_http_service_t *svc,
              N00B_HTTP_SOCK_T     fd,
              n00b_http_request_t **req_out,
              int                 *status_out)
{
    char  *raw        = nullptr;
    size_t raw_len    = 0;
    size_t header_end = 0;

    if (!read_request_bytes(svc, fd, &raw, &raw_len, &header_end, status_out)) {
        return false;
    }

    n00b_http_request_t *req = n00b_alloc(n00b_http_request_t);
    req->headers = n00b_list_new(n00b_http_header_t *);
    req->query   = n00b_string_empty();
    req->body    = n00b_buffer_empty();

    char *headers_end = raw + header_end;
    char *line_end    = find_line_end(raw, headers_end);
    if (line_end == nullptr) {
        *status_out = 400;
        return false;
    }

    char *method_end = raw;
    while (method_end < line_end && *method_end != ' ') method_end++;
    if (method_end == raw || method_end == line_end) {
        *status_out = 400;
        return false;
    }
    char *target = method_end + 1;
    char *target_end = target;
    while (target_end < line_end && *target_end != ' ') target_end++;
    if (target_end == target || target_end == line_end) {
        *status_out = 400;
        return false;
    }

    req->method = n00b_string_from_raw(raw, method_end - raw);

    char *query = target;
    while (query < target_end && *query != '?') query++;
    if (query < target_end) {
        req->path  = n00b_string_from_raw(target, query - target);
        req->query = n00b_string_from_raw(query + 1, target_end - query - 1);
    }
    else {
        req->path = n00b_string_from_raw(target, target_end - target);
    }

    char *p = line_end + 2;
    while (p + 1 < headers_end && !(p[0] == '\r' && p[1] == '\n')) {
        char *eol = find_line_end(p, headers_end);
        if (eol == nullptr) {
            *status_out = 400;
            return false;
        }
        char *colon = p;
        while (colon < eol && *colon != ':') colon++;
        if (colon == eol) {
            *status_out = 400;
            return false;
        }
        char *v_start = colon + 1;
        while (v_start < eol && (*v_start == ' ' || *v_start == '\t')) {
            v_start++;
        }
        char *v_end = eol;
        while (v_end > v_start
               && (v_end[-1] == ' ' || v_end[-1] == '\t')) {
            v_end--;
        }
        headers_push(&req->headers,
                     n00b_string_from_raw(p, colon - p),
                     n00b_string_from_raw(v_start, v_end - v_start));
        p = eol + 2;
    }

    size_t content_length = 0;
    if (!parse_content_length(req, &content_length)) {
        *status_out = 400;
        return false;
    }
    if (content_length > svc->max_body_bytes) {
        *status_out = 413;
        return false;
    }

    while (raw_len - header_end < content_length) {
        size_t needed = header_end + content_length;
        char  *next   = n00b_alloc_array(char, needed + 1);
        memcpy(next, raw, raw_len);
        raw = next;

#ifdef _WIN32
        int rc = recv(fd, raw + raw_len, (int)(needed - raw_len), 0);
#else
        ssize_t rc = recv(fd, raw + raw_len, needed - raw_len, 0);
#endif
        if (rc <= 0) {
            *status_out = 400;
            return false;
        }
        raw_len += (size_t)rc;
        raw[raw_len] = '\0';
    }

    if (content_length != 0) {
        req->body = n00b_buffer_from_bytes(raw + header_end,
                                           (int64_t)content_length);
    }

    *req_out = req;
    return true;
}

static n00b_http_route_t *
find_route(n00b_http_service_t *svc,
           n00b_string_t       *method,
           n00b_string_t       *path,
           bool                *path_found)
{
    *path_found = false;
    size_t n = n00b_list_len(svc->routes);
    for (size_t i = 0; i < n; i++) {
        n00b_http_route_t *r = n00b_list_get(svc->routes, i);
        if (str_eq(r->path, path)) {
            *path_found = true;
            if (str_eq(r->method, method)) {
                return r;
            }
        }
    }
    return nullptr;
}

static const char *
reason_phrase(uint16_t status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    default:  return "OK";
    }
}

static void
send_response(N00B_HTTP_SOCK_T fd, n00b_http_response_writer_t *resp)
{
    char head[256];
    size_t body_len = resp->body ? resp->body->byte_len : 0;
    snprintf(head,
             sizeof(head),
             "HTTP/1.1 %u %s\r\nContent-Length: %zu\r\nConnection: close\r\n",
             (unsigned)resp->status,
             reason_phrase(resp->status),
             body_len);
    send_cstr(fd, head);

    size_t n = n00b_list_len(resp->headers);
    for (size_t i = 0; i < n; i++) {
        n00b_http_header_t *h = n00b_list_get(resp->headers, i);
        send_all(fd, h->name->data, h->name->u8_bytes);
        send_cstr(fd, ": ");
        send_all(fd, h->value->data, h->value->u8_bytes);
        send_cstr(fd, "\r\n");
    }

    send_cstr(fd, "\r\n");
    if (body_len != 0) {
        send_all(fd, resp->body->data, body_len);
    }
}

static n00b_http_response_writer_t *
response_new(void)
{
    n00b_http_response_writer_t *resp = n00b_alloc(n00b_http_response_writer_t);
    resp->status  = 200;
    resp->headers = n00b_list_new(n00b_http_header_t *);
    resp->body    = n00b_buffer_empty();
    return resp;
}

static void
simple_error(N00B_HTTP_SOCK_T fd, uint16_t status, const char *body)
{
    n00b_http_response_writer_t *resp = response_new();
    resp->status = status;
    resp->body   = buf_from_cstr(body);
    headers_push(&resp->headers, r"content-type", r"text/plain");
    send_response(fd, resp);
}

static void
handle_client(n00b_http_service_t *svc, N00B_HTTP_SOCK_T fd)
{
    n00b_http_request_t *req    = nullptr;
    int                  status = 400;
    if (!parse_request(svc, fd, &req, &status)) {
        simple_error(fd, (uint16_t)status, "bad request");
        N00B_HTTP_CLOSE(fd);
        return;
    }

    bool path_found = false;
    n00b_http_route_t *route = find_route(svc, req->method, req->path,
                                           &path_found);
    if (route == nullptr) {
        if (path_found) {
            simple_error(fd, 405, "method not allowed");
        }
        else {
            simple_error(fd, 404, "not found");
        }
        N00B_HTTP_CLOSE(fd);
        return;
    }

    n00b_http_response_writer_t *resp = response_new();
    route->handler(req, resp, route->user_data);
    send_response(fd, resp);
    N00B_HTTP_CLOSE(fd);
}

static void
client_job(void *arg)
{
    n00b_http_client_job_t *job = arg;
    handle_client(job->svc, job->fd);
}

static void *
listener_main(void *arg)
{
    n00b_http_service_t *svc = arg;

    while (!n00b_atomic_load(&svc->stopping)) {
        struct sockaddr_in addr = {};
#ifdef _WIN32
        int addr_len = sizeof(addr);
        SOCKET client = accept(svc->listener_fd, (struct sockaddr *)&addr,
                               &addr_len);
#else
        socklen_t addr_len = sizeof(addr);
        int client = accept(svc->listener_fd, (struct sockaddr *)&addr,
                            &addr_len);
#endif
        if (client == N00B_HTTP_BAD_SOCK) {
            if (n00b_atomic_load(&svc->stopping)) break;
            continue;
        }
        if (n00b_atomic_load(&svc->stopping)) {
            N00B_HTTP_CLOSE(client);
            break;
        }

        if (svc->worker_service != nullptr) {
            n00b_http_client_job_t *job = n00b_alloc(n00b_http_client_job_t);
            job->svc = svc;
            job->fd  = client;
            auto r = n00b_conduit_service_submit(svc->worker_service,
                                                  client_job,
                                                  job);
            if (n00b_result_is_err(r)) {
                handle_client(svc, client);
            }
        }
        else {
            handle_client(svc, client);
        }
    }

    return nullptr;
}

static void
wake_listener(n00b_http_service_t *svc)
{
    if (svc == nullptr || svc->actual_port == 0) {
        return;
    }

    N00B_HTTP_SOCK_T fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == N00B_HTTP_BAD_SOCK) {
        return;
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(svc->actual_port);
    if (inet_pton(AF_INET, svc->bind_host->data, &addr.sin_addr) == 1) {
        (void)connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    }
    N00B_HTTP_CLOSE(fd);
}

n00b_http_service_t *
n00b_http_service_new()
    _kargs {
        n00b_string_t          *bind_host        = nullptr;
        uint16_t                bind_port        = 0;
        size_t                  max_header_bytes = 16384;
        size_t                  max_body_bytes   = 1048576;
        int                     backlog          = 128;
        n00b_conduit_service_t *worker_service   = nullptr;
        n00b_allocator_t       *allocator        = nullptr;
    }
{
    n00b_http_service_t *svc = n00b_alloc_with_opts(
        n00b_http_service_t,
        &(n00b_alloc_opts_t){.allocator = allocator});
    svc->bind_host        = bind_host ? bind_host : r"127.0.0.1";
    svc->bind_port        = bind_port;
    svc->max_header_bytes = max_header_bytes;
    svc->max_body_bytes   = max_body_bytes;
    svc->backlog          = backlog <= 0 ? 128 : backlog;
    svc->worker_service   = worker_service;
    svc->allocator        = allocator;
    svc->routes           = n00b_list_new(n00b_http_route_t *);
    svc->listener_fd      = N00B_HTTP_BAD_SOCK;
    return svc;
}

n00b_result_t(bool)
n00b_http_service_route(n00b_http_service_t *svc,
                        n00b_string_t       *method,
                        n00b_string_t       *path,
                        n00b_http_handler_fn handler,
                        void                *user_data)
{
    if (svc == nullptr || method == nullptr || path == nullptr
        || handler == nullptr || n00b_atomic_load(&svc->started)) {
        return n00b_result_err(bool, EINVAL);
    }

    n00b_http_route_t *route = n00b_alloc(n00b_http_route_t);
    route->method    = method;
    route->path      = path;
    route->handler   = handler;
    route->user_data = user_data;
    n00b_list_push(svc->routes, route);
    return n00b_result_ok(bool, true);
}

n00b_result_t(bool)
n00b_http_service_start(n00b_http_service_t *svc)
{
    if (svc == nullptr) {
        return n00b_result_err(bool, EINVAL);
    }
    if (n00b_atomic_load(&svc->started)) {
        return n00b_result_ok(bool, true);
    }

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return n00b_result_err(bool, N00B_HTTP_SOCK_ERR);
    }
#endif

    N00B_HTTP_SOCK_T fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == N00B_HTTP_BAD_SOCK) {
        return n00b_result_err(bool, N00B_HTTP_SOCK_ERR);
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(svc->bind_port);
    if (inet_pton(AF_INET, svc->bind_host->data, &addr.sin_addr) != 1) {
        N00B_HTTP_CLOSE(fd);
        return n00b_result_err(bool, EINVAL);
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int err = N00B_HTTP_SOCK_ERR;
        N00B_HTTP_CLOSE(fd);
        return n00b_result_err(bool, err);
    }
    if (listen(fd, svc->backlog) < 0) {
        int err = N00B_HTTP_SOCK_ERR;
        N00B_HTTP_CLOSE(fd);
        return n00b_result_err(bool, err);
    }

#ifdef _WIN32
    int addr_len = sizeof(addr);
#else
    socklen_t addr_len = sizeof(addr);
#endif
    if (getsockname(fd, (struct sockaddr *)&addr, &addr_len) == 0) {
        svc->actual_port = ntohs(addr.sin_port);
    }
    else {
        svc->actual_port = svc->bind_port;
    }

    svc->listener_fd = fd;
    n00b_atomic_store(&svc->stopping, false);
    n00b_atomic_store(&svc->started, true);

    auto tr = n00b_thread_spawn(listener_main, svc);
    if (n00b_result_is_err(tr)) {
        n00b_atomic_store(&svc->started, false);
        N00B_HTTP_CLOSE(fd);
        svc->listener_fd = N00B_HTTP_BAD_SOCK;
        return n00b_result_err(bool, n00b_result_get_err(tr));
    }
    svc->listener_thread = n00b_result_get(tr);
    return n00b_result_ok(bool, true);
}

void
n00b_http_service_stop(n00b_http_service_t *svc)
{
    if (svc == nullptr || !n00b_atomic_load(&svc->started)) {
        return;
    }
    n00b_atomic_store(&svc->stopping, true);
    wake_listener(svc);
    if (svc->listener_fd != N00B_HTTP_BAD_SOCK) {
        N00B_HTTP_CLOSE(svc->listener_fd);
        svc->listener_fd = N00B_HTTP_BAD_SOCK;
    }
    if (svc->listener_thread != nullptr) {
        n00b_thread_join(svc->listener_thread);
        svc->listener_thread = nullptr;
    }
    n00b_atomic_store(&svc->started, false);
}

uint16_t
n00b_http_service_port(n00b_http_service_t *svc)
{
    return svc == nullptr ? 0 : svc->actual_port;
}

n00b_string_t *
n00b_http_request_method(n00b_http_request_t *req)
{
    return req == nullptr ? nullptr : req->method;
}

n00b_string_t *
n00b_http_request_path(n00b_http_request_t *req)
{
    return req == nullptr ? nullptr : req->path;
}

n00b_string_t *
n00b_http_request_query(n00b_http_request_t *req)
{
    return req == nullptr ? nullptr : req->query;
}

n00b_buffer_t *
n00b_http_request_body(n00b_http_request_t *req)
{
    return req == nullptr ? nullptr : req->body;
}

n00b_string_t *
n00b_http_request_header(n00b_http_request_t *req, n00b_string_t *name)
{
    if (req == nullptr) return nullptr;
    return headers_get(&req->headers, name);
}

void
n00b_http_response_writer_status(n00b_http_response_writer_t *resp,
                                 uint16_t                    status)
{
    if (resp != nullptr) {
        resp->status = status;
    }
}

void
n00b_http_response_writer_header(n00b_http_response_writer_t *resp,
                                 n00b_string_t               *name,
                                 n00b_string_t               *value)
{
    if (resp == nullptr || name == nullptr || value == nullptr) {
        return;
    }
    headers_push(&resp->headers, name, value);
}

void
n00b_http_response_writer_body(n00b_http_response_writer_t *resp,
                               n00b_buffer_t               *body)
{
    if (resp != nullptr) {
        resp->body = body ? body : n00b_buffer_empty();
    }
}

void
n00b_http_response_writer_text(n00b_http_response_writer_t *resp,
                               n00b_string_t               *body,
                               n00b_string_t               *content_type)
{
    if (resp == nullptr) return;
    resp->body = body ? n00b_buffer_from_bytes(body->data,
                                               (int64_t)body->u8_bytes)
                      : n00b_buffer_empty();
    if (content_type != nullptr) {
        n00b_http_response_writer_header(resp, r"content-type", content_type);
    }
}
