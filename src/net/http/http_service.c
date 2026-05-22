/*
 * http_service.c — Small local HTTP/1 service/router.
 */

#include "net/http/http_service.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdarg.h>
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
    bool                 has_spec;
    n00b_http_route_spec_t spec;
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
    bool                           discovery_enabled;
    n00b_http_discovery_info_t     discovery;
    n00b_thread_t                 *listener_thread;
    N00B_HTTP_SOCK_T               listener_fd;
    _Atomic(bool)                  started;
    _Atomic(bool)                  stopping;
};

typedef struct {
    n00b_http_service_t *svc;
    N00B_HTTP_SOCK_T     fd;
} n00b_http_client_job_t;

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} n00b_http_json_buf_t;

static void
json_buf_init(n00b_http_json_buf_t *b)
{
    b->cap  = 4096;
    b->len  = 0;
    b->data = n00b_alloc_array(char, b->cap);
    b->data[0] = '\0';
}

static void
json_buf_reserve(n00b_http_json_buf_t *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap) return;

    size_t next_cap = b->cap;
    while (b->len + extra + 1 > next_cap) {
        next_cap *= 2;
    }

    char *next = n00b_alloc_array(char, next_cap);
    if (b->len != 0) {
        memcpy(next, b->data, b->len);
    }
    next[b->len] = '\0';
    b->data = next;
    b->cap  = next_cap;
}

static void
json_buf_append_raw(n00b_http_json_buf_t *b, const char *s, size_t n)
{
    json_buf_reserve(b, n);
    if (n != 0) {
        memcpy(b->data + b->len, s, n);
    }
    b->len += n;
    b->data[b->len] = '\0';
}

static void
json_buf_append_cstr(n00b_http_json_buf_t *b, const char *s)
{
    if (s == nullptr) {
        s = "";
    }
    json_buf_append_raw(b, s, strlen(s));
}

static void
json_buf_append_char(n00b_http_json_buf_t *b, char c)
{
    json_buf_reserve(b, 1);
    b->data[b->len++] = c;
    b->data[b->len]   = '\0';
}

static void
json_buf_append_fmt(n00b_http_json_buf_t *b, const char *fmt, ...)
{
    char    stack[128];
    va_list args;

    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(stack, sizeof(stack), fmt, args);
    va_end(args);

    if (needed < 0) {
        va_end(copy);
        return;
    }

    if ((size_t)needed < sizeof(stack)) {
        va_end(copy);
        json_buf_append_raw(b, stack, (size_t)needed);
        return;
    }

    char *heap = n00b_alloc_array(char, (size_t)needed + 1);
    vsnprintf(heap, (size_t)needed + 1, fmt, copy);
    va_end(copy);
    json_buf_append_raw(b, heap, (size_t)needed);
}

static void
json_buf_append_json_n00b_string(n00b_http_json_buf_t *b, n00b_string_t *s)
{
    json_buf_append_char(b, '"');
    if (s != nullptr) {
        for (size_t i = 0; i < s->u8_bytes; i++) {
            unsigned char c = (unsigned char)s->data[i];
            switch (c) {
            case '"':  json_buf_append_cstr(b, "\\\""); break;
            case '\\': json_buf_append_cstr(b, "\\\\"); break;
            case '\b': json_buf_append_cstr(b, "\\b");  break;
            case '\f': json_buf_append_cstr(b, "\\f");  break;
            case '\n': json_buf_append_cstr(b, "\\n");  break;
            case '\r': json_buf_append_cstr(b, "\\r");  break;
            case '\t': json_buf_append_cstr(b, "\\t");  break;
            default:
                if (c < 0x20) {
                    json_buf_append_fmt(b, "\\u%04x", (unsigned)c);
                }
                else {
                    json_buf_append_char(b, (char)c);
                }
                break;
            }
        }
    }
    json_buf_append_char(b, '"');
}

static void
json_buf_append_json_cstr(n00b_http_json_buf_t *b, const char *s)
{
    json_buf_append_json_n00b_string(b, n00b_string_from_cstr(s ? s : ""));
}

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

static bool
str_is_empty(n00b_string_t *s)
{
    return s == nullptr || s->u8_bytes == 0;
}

static n00b_string_t **
copy_string_ptr_array(n00b_string_t **src, size_t n)
{
    if (src == nullptr || n == 0) {
        return nullptr;
    }

    n00b_string_t **dst = n00b_alloc_array(n00b_string_t *, n);
    memcpy(dst, src, n * sizeof(n00b_string_t *));
    return dst;
}

static n00b_http_param_spec_t *
copy_param_array(n00b_http_param_spec_t *src, size_t n)
{
    if (src == nullptr || n == 0) {
        return nullptr;
    }

    n00b_http_param_spec_t *dst = n00b_alloc_array(n00b_http_param_spec_t, n);
    memcpy(dst, src, n * sizeof(n00b_http_param_spec_t));
    return dst;
}

static n00b_http_response_spec_t *
copy_response_array(n00b_http_response_spec_t *src, size_t n)
{
    if (src == nullptr || n == 0) {
        return nullptr;
    }

    n00b_http_response_spec_t *dst = n00b_alloc_array(n00b_http_response_spec_t, n);
    memcpy(dst, src, n * sizeof(n00b_http_response_spec_t));
    return dst;
}

static n00b_http_route_spec_t
copy_route_spec(const n00b_http_route_spec_t *src)
{
    n00b_http_route_spec_t dst = *src;
    dst.tags = copy_string_ptr_array(src->tags, src->tag_count);
    dst.query_params = copy_param_array(src->query_params,
                                        src->query_param_count);
    dst.responses = copy_response_array(src->responses, src->response_count);
    if (dst.tags == nullptr) {
        dst.tag_count = 0;
    }
    if (dst.query_params == nullptr) {
        dst.query_param_count = 0;
    }
    if (dst.responses == nullptr) {
        dst.response_count = 0;
    }
    return dst;
}

static n00b_http_discovery_info_t
copy_discovery_info(const n00b_http_discovery_info_t *src)
{
    n00b_http_discovery_info_t dst = *src;

    if (dst.service_id == nullptr) {
        dst.service_id = r"service";
    }
    if (dst.service_name == nullptr) {
        dst.service_name = dst.service_id;
    }
    if (dst.service_version == nullptr) {
        dst.service_version = r"0.0.0";
    }
    if (dst.api_version == nullptr) {
        dst.api_version = r"v1";
    }
    if (dst.openapi_path == nullptr) {
        dst.openapi_path = r"/openapi.json";
    }
    if (dst.health_path == nullptr) {
        dst.health_path = r"/healthz";
    }

    dst.schema_paths = copy_string_ptr_array(src->schema_paths,
                                             src->schema_path_count);
    dst.capabilities = copy_string_ptr_array(src->capabilities,
                                             src->capability_count);
    if (dst.schema_paths == nullptr) {
        dst.schema_path_count = 0;
    }
    if (dst.capabilities == nullptr) {
        dst.capability_count = 0;
    }
    return dst;
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
        size_t digit = (size_t)(c - '0');
        if (n > (SIZE_MAX - digit) / 10) return false;
        n = n * 10 + digit;
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
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 422: return "Unprocessable Entity";
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

static n00b_buffer_t *
json_buf_to_buffer(n00b_http_json_buf_t *b)
{
    return n00b_buffer_from_bytes(b->data, (int64_t)b->len);
}

static void
json_member_name(n00b_http_json_buf_t *b, bool *first, const char *name)
{
    if (!*first) {
        json_buf_append_char(b, ',');
    }
    *first = false;
    json_buf_append_json_cstr(b, name);
    json_buf_append_char(b, ':');
}

static void
append_raw_schema(n00b_http_json_buf_t *b, n00b_string_t *schema)
{
    if (!str_is_empty(schema)) {
        json_buf_append_raw(b, schema->data, schema->u8_bytes);
    }
    else {
        json_buf_append_cstr(b, "{}");
    }
}

static void
append_method_name(n00b_http_json_buf_t *b, n00b_string_t *method)
{
    json_buf_append_char(b, '"');
    if (method != nullptr) {
        for (size_t i = 0; i < method->u8_bytes; i++) {
            char c = method->data[i];
            if (c >= 'A' && c <= 'Z') {
                c = (char)(c + ('a' - 'A'));
            }
            json_buf_append_char(b, c);
        }
    }
    json_buf_append_char(b, '"');
}

static void
append_string_array(n00b_http_json_buf_t *b,
                    n00b_string_t       **items,
                    size_t                count)
{
    json_buf_append_char(b, '[');
    for (size_t i = 0; i < count; i++) {
        if (i != 0) {
            json_buf_append_char(b, ',');
        }
        json_buf_append_json_n00b_string(b, items[i]);
    }
    json_buf_append_char(b, ']');
}

static void
append_parameters(n00b_http_json_buf_t      *b,
                  n00b_http_param_spec_t   *params,
                  size_t                    count)
{
    json_buf_append_char(b, '[');
    for (size_t i = 0; i < count; i++) {
        n00b_http_param_spec_t *p = &params[i];
        if (i != 0) {
            json_buf_append_char(b, ',');
        }

        json_buf_append_char(b, '{');
        bool first = true;

        json_member_name(b, &first, "name");
        json_buf_append_json_n00b_string(b, p->name);

        json_member_name(b, &first, "in");
        json_buf_append_json_n00b_string(
            b,
            p->location == nullptr ? r"query" : p->location);

        json_member_name(b, &first, "required");
        json_buf_append_cstr(b, p->required ? "true" : "false");

        if (!str_is_empty(p->description)) {
            json_member_name(b, &first, "description");
            json_buf_append_json_n00b_string(b, p->description);
        }

        json_member_name(b, &first, "schema");
        append_raw_schema(b, p->schema_json);
        json_buf_append_char(b, '}');
    }
    json_buf_append_char(b, ']');
}

static void
append_request_body(n00b_http_json_buf_t    *b,
                    n00b_http_body_spec_t   *body)
{
    json_buf_append_char(b, '{');
    bool first = true;

    json_member_name(b, &first, "required");
    json_buf_append_cstr(b, body->required ? "true" : "false");

    json_member_name(b, &first, "content");
    json_buf_append_char(b, '{');
    json_buf_append_json_n00b_string(
        b,
        body->content_type == nullptr ? r"application/json"
                                      : body->content_type);
    json_buf_append_cstr(b, ":{\"schema\":");
    append_raw_schema(b, body->schema_json);
    json_buf_append_cstr(b, "}}");
    json_buf_append_char(b, '}');
}

static void
append_responses(n00b_http_json_buf_t        *b,
                 n00b_http_response_spec_t  *responses,
                 size_t                      count)
{
    json_buf_append_char(b, '{');

    if (responses == nullptr || count == 0) {
        json_buf_append_cstr(b, "\"200\":{\"description\":\"OK\"}");
        json_buf_append_char(b, '}');
        return;
    }

    for (size_t i = 0; i < count; i++) {
        n00b_http_response_spec_t *r = &responses[i];
        if (i != 0) {
            json_buf_append_char(b, ',');
        }

        json_buf_append_char(b, '"');
        json_buf_append_fmt(b, "%u", (unsigned)r->status);
        json_buf_append_cstr(b, "\":{");

        bool first = true;
        json_member_name(b, &first, "description");
        json_buf_append_json_n00b_string(
            b,
            r->description == nullptr ? r"response" : r->description);

        json_member_name(b, &first, "content");
        json_buf_append_char(b, '{');
        json_buf_append_json_n00b_string(
            b,
            r->content_type == nullptr ? r"application/json" : r->content_type);
        json_buf_append_cstr(b, ":{\"schema\":");
        append_raw_schema(b, r->schema_json);
        json_buf_append_cstr(b, "}}");
        json_buf_append_char(b, '}');
    }

    json_buf_append_char(b, '}');
}

static n00b_http_route_spec_t *
route_spec_for_openapi(n00b_http_route_t *route,
                       n00b_http_route_spec_t *fallback)
{
    if (route->has_spec) {
        return &route->spec;
    }

    *fallback = (n00b_http_route_spec_t){
        .method    = route->method,
        .path      = route->path,
        .handler   = route->handler,
        .user_data = route->user_data,
    };
    return fallback;
}

static void
append_operation(n00b_http_json_buf_t *b, n00b_http_route_t *route)
{
    n00b_http_route_spec_t fallback = {};
    n00b_http_route_spec_t *spec = route_spec_for_openapi(route, &fallback);
    json_buf_append_char(b, '{');
    bool first = true;

    if (!str_is_empty(spec->operation_id)) {
        json_member_name(b, &first, "operationId");
        json_buf_append_json_n00b_string(b, spec->operation_id);
    }

    if (!str_is_empty(spec->summary)) {
        json_member_name(b, &first, "summary");
        json_buf_append_json_n00b_string(b, spec->summary);
    }

    if (spec->tags != nullptr && spec->tag_count != 0) {
        json_member_name(b, &first, "tags");
        append_string_array(b, spec->tags, spec->tag_count);
    }

    if (spec->query_params != nullptr && spec->query_param_count != 0) {
        json_member_name(b, &first, "parameters");
        append_parameters(b, spec->query_params, spec->query_param_count);
    }

    if (!str_is_empty(spec->request_body.content_type)
        || !str_is_empty(spec->request_body.schema_json)
        || spec->request_body.required) {
        json_member_name(b, &first, "requestBody");
        append_request_body(b, &spec->request_body);
    }

    json_member_name(b, &first, "responses");
    append_responses(b, spec->responses, spec->response_count);

    json_buf_append_char(b, '}');
}

static bool
previous_route_path(n00b_http_service_t *svc, size_t pos, n00b_string_t *path)
{
    for (size_t i = 0; i < pos; i++) {
        n00b_http_route_t *r = n00b_list_get(svc->routes, i);
        if (str_eq(r->path, path)) {
            return true;
        }
    }
    return false;
}

static bool
previous_route_method(n00b_http_service_t *svc,
                      size_t               pos,
                      n00b_string_t       *path,
                      n00b_string_t       *method)
{
    for (size_t i = 0; i < pos; i++) {
        n00b_http_route_t *r = n00b_list_get(svc->routes, i);
        if (str_eq(r->path, path) && str_eq(r->method, method)) {
            return true;
        }
    }
    return false;
}

static n00b_buffer_t *
build_openapi_json(n00b_http_service_t *svc)
{
    n00b_http_json_buf_t b;
    json_buf_init(&b);

    json_buf_append_cstr(&b, "{\"openapi\":\"3.1.0\",\"info\":{");
    json_buf_append_cstr(&b, "\"title\":");
    json_buf_append_json_n00b_string(&b, svc->discovery.service_name);
    json_buf_append_cstr(&b, ",\"version\":");
    json_buf_append_json_n00b_string(&b, svc->discovery.service_version);
    json_buf_append_cstr(&b, "},\"paths\":{");

    bool first_path = true;
    size_t n = n00b_list_len(svc->routes);
    for (size_t i = 0; i < n; i++) {
        n00b_http_route_t *route = n00b_list_get(svc->routes, i);
        if (previous_route_path(svc, i, route->path)) {
            continue;
        }

        if (!first_path) {
            json_buf_append_char(&b, ',');
        }
        first_path = false;

        json_buf_append_json_n00b_string(&b, route->path);
        json_buf_append_char(&b, ':');
        json_buf_append_char(&b, '{');

        bool first_method = true;
        for (size_t j = 0; j < n; j++) {
            n00b_http_route_t *method_route = n00b_list_get(svc->routes, j);
            if (!str_eq(method_route->path, route->path)) {
                continue;
            }
            if (previous_route_method(svc, j,
                                      method_route->path,
                                      method_route->method)) {
                continue;
            }

            if (!first_method) {
                json_buf_append_char(&b, ',');
            }
            first_method = false;

            append_method_name(&b, method_route->method);
            json_buf_append_char(&b, ':');
            append_operation(&b, method_route);
        }

        json_buf_append_char(&b, '}');
    }

    json_buf_append_cstr(&b, "}}");
    return json_buf_to_buffer(&b);
}

static n00b_buffer_t *
build_well_known_json(n00b_http_service_t *svc)
{
    n00b_http_json_buf_t b;
    json_buf_init(&b);

    json_buf_append_char(&b, '{');
    bool first = true;

    json_member_name(&b, &first, "service");
    json_buf_append_json_n00b_string(&b, svc->discovery.service_id);

    json_member_name(&b, &first, "service_name");
    json_buf_append_json_n00b_string(&b, svc->discovery.service_name);

    json_member_name(&b, &first, "service_version");
    json_buf_append_json_n00b_string(&b, svc->discovery.service_version);

    json_member_name(&b, &first, "api_version");
    json_buf_append_json_n00b_string(&b, svc->discovery.api_version);

    json_member_name(&b, &first, "health");
    json_buf_append_json_n00b_string(&b, svc->discovery.health_path);

    json_member_name(&b, &first, "openapi");
    json_buf_append_json_n00b_string(&b, svc->discovery.openapi_path);

    json_member_name(&b, &first, "schemas");
    append_string_array(&b, svc->discovery.schema_paths,
                        svc->discovery.schema_path_count);

    json_member_name(&b, &first, "capabilities");
    append_string_array(&b, svc->discovery.capabilities,
                        svc->discovery.capability_count);

    json_buf_append_char(&b, '}');
    return json_buf_to_buffer(&b);
}

static void
json_body_response(n00b_http_response_writer_t *resp, n00b_buffer_t *body)
{
    n00b_http_response_writer_status(resp, 200);
    n00b_http_response_writer_header(resp, r"content-type", r"application/json");
    n00b_http_response_writer_body(resp, body);
}

static void
openapi_handler(n00b_http_request_t *req,
                n00b_http_response_writer_t *resp,
                void *user_data)
{
    (void)req;
    n00b_http_service_t *svc = user_data;
    json_body_response(resp, build_openapi_json(svc));
}

static void
well_known_handler(n00b_http_request_t *req,
                   n00b_http_response_writer_t *resp,
                   void *user_data)
{
    (void)req;
    n00b_http_service_t *svc = user_data;
    json_body_response(resp, build_well_known_json(svc));
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

static n00b_result_t(bool)
register_route_internal(n00b_http_service_t           *svc,
                        n00b_string_t                 *method,
                        n00b_string_t                 *path,
                        n00b_http_handler_fn           handler,
                        void                          *user_data,
                        const n00b_http_route_spec_t  *spec)
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
    route->has_spec  = false;
    route->spec      = (n00b_http_route_spec_t){};
    if (spec != nullptr) {
        route->has_spec = true;
        route->spec     = copy_route_spec(spec);
        route->spec.method    = method;
        route->spec.path      = path;
        route->spec.handler   = handler;
        route->spec.user_data = user_data;
    }
    n00b_list_push(svc->routes, route);
    return n00b_result_ok(bool, true);
}

n00b_http_param_spec_t
n00b_http_query_param(n00b_string_t *name)
    _kargs {
        bool           required    = false;
        n00b_string_t *schema_json = nullptr;
        n00b_string_t *description = nullptr;
    }
{
    return (n00b_http_param_spec_t){
        .name        = name,
        .location    = r"query",
        .required    = required,
        .schema_json = schema_json,
        .description = description,
    };
}

n00b_http_body_spec_t
n00b_http_json_body(n00b_string_t *schema_json)
    _kargs {
        bool required = true;
    }
{
    return (n00b_http_body_spec_t){
        .content_type = r"application/json",
        .schema_json  = schema_json,
        .required     = required,
    };
}

n00b_http_response_spec_t
n00b_http_json_response(uint16_t status)
    _kargs {
        n00b_string_t *description = nullptr;
        n00b_string_t *schema_json = nullptr;
    }
{
    return (n00b_http_response_spec_t){
        .status       = status,
        .description  = description,
        .content_type = r"application/json",
        .schema_json  = schema_json,
    };
}

n00b_result_t(bool)
n00b_http_route(n00b_http_service_t *svc,
                n00b_http_endpoint_t endpoint)
{
    n00b_http_route_spec_t spec = {
        .method            = endpoint.method,
        .path              = endpoint.path,
        .handler           = endpoint.handler,
        .user_data         = endpoint.user_data,
        .operation_id      = endpoint.id,
        .summary           = endpoint.summary,
        .tags              = endpoint.tags.items,
        .tag_count         = endpoint.tags.count,
        .query_params      = endpoint.query.items,
        .query_param_count = endpoint.query.count,
        .request_body      = endpoint.body,
        .responses         = endpoint.responses.items,
        .response_count    = endpoint.responses.count,
    };

    return n00b_http_service_route_spec(svc, &spec);
}

n00b_result_t(bool)
n00b_http_get(n00b_http_service_t *svc,
              n00b_http_endpoint_t endpoint)
{
    endpoint.method = r"GET";
    return n00b_http_route(svc, endpoint);
}

n00b_result_t(bool)
n00b_http_post(n00b_http_service_t *svc,
               n00b_http_endpoint_t endpoint)
{
    endpoint.method = r"POST";
    return n00b_http_route(svc, endpoint);
}

n00b_result_t(bool)
n00b_http_discover(n00b_http_service_t       *svc,
                   n00b_http_discovery_doc_t  doc)
{
    n00b_http_discovery_info_t info = {
        .service_id          = doc.service_id,
        .service_name        = doc.name,
        .service_version     = doc.version,
        .api_version         = doc.api_version,
        .openapi_path        = doc.openapi_path,
        .health_path         = doc.health_path,
        .schema_paths        = doc.schemas.items,
        .schema_path_count   = doc.schemas.count,
        .capabilities        = doc.capabilities.items,
        .capability_count    = doc.capabilities.count,
    };

    return n00b_http_service_enable_discovery(svc, &info);
}

n00b_result_t(bool)
n00b_http_service_route(n00b_http_service_t *svc,
                        n00b_string_t       *method,
                        n00b_string_t       *path,
                        n00b_http_handler_fn handler,
                        void                *user_data)
{
    return register_route_internal(svc, method, path, handler, user_data,
                                   nullptr);
}

n00b_result_t(bool)
n00b_http_service_route_spec(n00b_http_service_t           *svc,
                             const n00b_http_route_spec_t *spec)
{
    if (spec == nullptr) {
        return n00b_result_err(bool, EINVAL);
    }

    return register_route_internal(svc,
                                   spec->method,
                                   spec->path,
                                   spec->handler,
                                   spec->user_data,
                                   spec);
}

static n00b_string_t *
well_known_service_path(n00b_string_t *service_id)
{
    const char *prefix = "/.well-known/";
    size_t prefix_len = strlen(prefix);
    size_t id_len = service_id == nullptr ? 0 : service_id->u8_bytes;
    char *path = n00b_alloc_array(char, prefix_len + id_len + 1);

    memcpy(path, prefix, prefix_len);
    if (id_len != 0) {
        memcpy(path + prefix_len, service_id->data, id_len);
    }
    path[prefix_len + id_len] = '\0';
    return n00b_string_from_cstr(path);
}

n00b_result_t(bool)
n00b_http_service_enable_discovery(n00b_http_service_t               *svc,
                                   const n00b_http_discovery_info_t *info)
{
    if (svc == nullptr || info == nullptr || svc->discovery_enabled
        || n00b_atomic_load(&svc->started)) {
        return n00b_result_err(bool, EINVAL);
    }

    svc->discovery = copy_discovery_info(info);
    svc->discovery_enabled = true;

    n00b_http_response_list_t openapi_responses = n00b_http_responses(
        n00b_http_json_response(200,
                                .description = r"OpenAPI document",
                                .schema_json = r"{\"type\":\"object\",\"required\":[\"openapi\",\"info\",\"paths\"]}"));
    n00b_http_response_list_t service_responses = n00b_http_responses(
        n00b_http_json_response(200,
                                .description = r"Well-known service descriptor",
                                .schema_json = r"{\"type\":\"object\",\"required\":[\"service\",\"openapi\",\"health\"]}"));

    auto r = n00b_http_get(svc,
                           (n00b_http_endpoint_t){
                               .path      = svc->discovery.openapi_path,
                               .handler   = openapi_handler,
                               .user_data = svc,
                               .id        = r"getOpenAPI",
                               .summary   = r"Get OpenAPI document",
                               .tags      = n00b_http_tags(r"system"),
                               .responses = openapi_responses,
                           });
    if (n00b_result_is_err(r)) {
        return r;
    }

    r = n00b_http_get(svc,
                      (n00b_http_endpoint_t){
                          .path      = r"/.well-known/openapi.json",
                          .handler   = openapi_handler,
                          .user_data = svc,
                          .id        = r"getWellKnownOpenAPI",
                          .summary   = r"Get well-known OpenAPI document",
                          .tags      = n00b_http_tags(r"system"),
                          .responses = openapi_responses,
                      });
    if (n00b_result_is_err(r)) {
        return r;
    }

    n00b_string_t *service_path = well_known_service_path(svc->discovery.service_id);
    return n00b_http_get(svc,
                         (n00b_http_endpoint_t){
                             .path      = service_path,
                             .handler   = well_known_handler,
                             .user_data = svc,
                             .id        = r"getWellKnownService",
                             .summary   = r"Get well-known service descriptor",
                             .tags      = n00b_http_tags(r"system"),
                             .responses = service_responses,
                         });
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
                               n00b_string_t               *body)
    _kargs {
        n00b_string_t *content_type = nullptr;
    }
{
    if (resp == nullptr) return;
    resp->body = body ? n00b_buffer_from_bytes(body->data,
                                               (int64_t)body->u8_bytes)
                      : n00b_buffer_empty();
    if (content_type == nullptr) {
        content_type = r"text/plain";
    }
    if (content_type != nullptr) {
        n00b_http_response_writer_header(resp, r"content-type", content_type);
    }
}
