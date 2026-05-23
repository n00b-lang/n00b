#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "n00b.h"
#include "core/static_image.h"
#include "core/type_info.h"

extern void n00b_init_simple(int argc, char *argv[]);
extern void n00b_shutdown(void);

typedef struct {
    char                    *type_name;
    uint64_t                 type_hash;
    char                    *symbol_prefix;
    char                    *entry_attr;
    bool                     readonly;
    bool                     have_abi;
    n00b_static_image_abi_t  abi;
    uint64_t                 expected_arg_count;
    n00b_static_init_arg_t  *args;
    uint64_t                 arg_count;
} helper_request_t;

static char *
read_stdin(void)
{
    size_t cap = 4096;
    size_t len = 0;
    char  *buf = malloc(cap);

    if (!buf) {
        return NULL;
    }

    int c;
    while ((c = getchar()) != EOF) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *new_buf = realloc(buf, cap);
            if (!new_buf) {
                free(buf);
                return NULL;
            }
            buf = new_buf;
        }
        buf[len++] = (char)c;
    }
    buf[len] = '\0';
    return buf;
}

static char *
dup_cstr(const char *s)
{
    size_t len = strlen(s);
    char  *out = malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, s, len + 1);
    return out;
}

static char *
trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }

    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return s;
}

static char *
next_token(char **cursor)
{
    char *p = *cursor;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (!*p) {
        *cursor = p;
        return NULL;
    }

    char *start = p;
    while (*p && !isspace((unsigned char)*p)) {
        p++;
    }
    if (*p) {
        *p++ = '\0';
    }
    *cursor = p;
    return start;
}

static int
hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void *
hex_decode_exact(const char *hex, uint64_t len, bool nul_term, bool *ok)
{
    size_t hex_len = strlen(hex);
    if (hex_len != (size_t)(len * 2)) {
        *ok = false;
        return NULL;
    }

    unsigned char *out = calloc((size_t)len + (nul_term ? 1u : 0u) + 1u, 1);
    if (!out) {
        *ok = false;
        return NULL;
    }

    for (uint64_t i = 0; i < len; i++) {
        int hi = hex_value(hex[i * 2]);
        int lo = hex_value(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            free(out);
            *ok = false;
            return NULL;
        }
        out[i] = (unsigned char)((hi << 4) | lo);
    }

    *ok = true;
    return out;
}

static char *
hex_decode_cstr(const char *hex)
{
    size_t hex_len = strlen(hex);
    if ((hex_len % 2u) != 0) {
        return NULL;
    }

    bool ok = false;
    return hex_decode_exact(hex, (uint64_t)(hex_len / 2u), true, &ok);
}

static bool
parse_u64_token(const char *text, uint64_t *out)
{
    if (!text || !*text) {
        return false;
    }

    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 0);
    if (end == text) {
        return false;
    }
    *out = (uint64_t)value;
    return true;
}

static bool
parse_i64_token(const char *text, int64_t *out)
{
    if (!text || !*text) {
        return false;
    }

    char *end = NULL;
    long long value = strtoll(text, &end, 0);
    if (end == text) {
        return false;
    }
    *out = (int64_t)value;
    return true;
}

static bool
append_arg(helper_request_t *req, n00b_static_init_arg_t arg)
{
    n00b_static_init_arg_t *new_args =
        realloc(req->args, (size_t)(req->arg_count + 1u) * sizeof(*req->args));
    if (!new_args) {
        return false;
    }

    req->args = new_args;
    req->args[req->arg_count++] = arg;
    return true;
}

static bool
parse_abi_line(char *line, helper_request_t *req)
{
    char *p = line;
    (void)next_token(&p);

    uint64_t pointer_bytes = 0;
    uint64_t size_t_bytes  = 0;
    uint64_t char_bits     = 0;
    uint64_t endian        = 0;

    if (!parse_u64_token(next_token(&p), &pointer_bytes)
        || !parse_u64_token(next_token(&p), &size_t_bytes)
        || !parse_u64_token(next_token(&p), &char_bits)
        || !parse_u64_token(next_token(&p), &endian)) {
        return false;
    }

    req->abi = (n00b_static_image_abi_t){
        .version       = N00B_STATIC_IMAGE_CONTRACT_VERSION,
        .pointer_bytes = (uint8_t)pointer_bytes,
        .size_t_bytes  = (uint8_t)size_t_bytes,
        .char_bits     = (uint8_t)char_bits,
        .endian        = (uint8_t)endian,
    };
    req->have_abi = true;
    return true;
}

static bool
parse_arg_line(char *line, helper_request_t *req)
{
    char *p = line;
    (void)next_token(&p);

    char *name = next_token(&p);
    char *kind = next_token(&p);
    if (!name || !kind) {
        return false;
    }

    n00b_static_init_arg_t arg = {
        .name = strcmp(name, "-") == 0 ? NULL : dup_cstr(name),
    };
    if (arg.name == NULL && strcmp(name, "-") != 0) {
        return false;
    }

    if (strcmp(kind, "bytes") == 0) {
        uint64_t len = 0;
        if (!parse_u64_token(next_token(&p), &len)) {
            free((void *)arg.name);
            return false;
        }

        char *hex = trim(p);
        if (len != 0 && !*hex) {
            free((void *)arg.name);
            return false;
        }

        bool ok = false;
        void *data = hex_decode_exact(hex, len, false, &ok);
        if (!ok) {
            free((void *)arg.name);
            return false;
        }

        arg.kind       = N00B_STATIC_INIT_ARG_BYTES;
        arg.bytes.data = data;
        arg.bytes.len  = len;
        return append_arg(req, arg);
    }

    if (strcmp(kind, "int") == 0) {
        int64_t value = 0;
        if (!parse_i64_token(next_token(&p), &value)) {
            free((void *)arg.name);
            return false;
        }
        arg.kind    = N00B_STATIC_INIT_ARG_INT;
        arg.integer = value;
        return append_arg(req, arg);
    }

    if (strcmp(kind, "bool") == 0) {
        uint64_t value = 0;
        if (!parse_u64_token(next_token(&p), &value)) {
            free((void *)arg.name);
            return false;
        }
        arg.kind    = N00B_STATIC_INIT_ARG_BOOL;
        arg.boolean = value != 0;
        return append_arg(req, arg);
    }

    free((void *)arg.name);
    return false;
}

static bool
parse_request(char *input, helper_request_t *req)
{
    for (char *line = input; line;) {
        char *next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }

        line = trim(line);
        if (!*line || strcmp(line, "NCC_STATIC_INIT 1") == 0
            || strcmp(line, "end") == 0) {
            line = next;
            continue;
        }

        if (strncmp(line, "type_hex ", 9) == 0) {
            free(req->type_name);
            req->type_name = hex_decode_cstr(trim(line + 9));
        }
        else if (strncmp(line, "type_hash ", 10) == 0) {
            if (!parse_u64_token(trim(line + 10), &req->type_hash)) {
                return false;
            }
        }
        else if (strncmp(line, "prefix ", 7) == 0) {
            free(req->symbol_prefix);
            req->symbol_prefix = dup_cstr(trim(line + 7));
        }
        else if (strncmp(line, "readonly ", 9) == 0) {
            uint64_t value = 0;
            if (!parse_u64_token(trim(line + 9), &value)) {
                return false;
            }
            req->readonly = value != 0;
        }
        else if (strncmp(line, "abi ", 4) == 0) {
            if (!parse_abi_line(line, req)) {
                return false;
            }
        }
        else if (strncmp(line, "entry_attr_hex ", 15) == 0) {
            free(req->entry_attr);
            req->entry_attr = hex_decode_cstr(trim(line + 15));
        }
        else if (strncmp(line, "arg_count ", 10) == 0) {
            if (!parse_u64_token(trim(line + 10), &req->expected_arg_count)) {
                return false;
            }
        }
        else if (strncmp(line, "arg ", 4) == 0) {
            if (!parse_arg_line(line, req)) {
                return false;
            }
        }
        else {
            return false;
        }

        line = next;
    }

    return req->type_name && req->type_hash != 0 && req->symbol_prefix
        && req->entry_attr && req->have_abi
        && req->arg_count == req->expected_arg_count;
}

static void
free_request(helper_request_t *req)
{
    free(req->type_name);
    free(req->symbol_prefix);
    free(req->entry_attr);
    for (uint64_t i = 0; i < req->arg_count; i++) {
        free((void *)req->args[i].name);
        if (req->args[i].kind == N00B_STATIC_INIT_ARG_BYTES) {
            free((void *)req->args[i].bytes.data);
        }
    }
    free(req->args);
}

int
main(int argc, char **argv)
{
    char *input = read_stdin();
    helper_request_t parsed = {0};

    if (!input || !parse_request(input, &parsed)) {
        fprintf(stderr, "bad n00b static initializer helper request");
        free(input);
        free_request(&parsed);
        return 2;
    }

    n00b_init_simple(argc, argv);

    n00b_gc_scan_kind_t scan_kind = N00B_GC_SCAN_KIND_DEFAULT;
    n00b_static_layout_opt_t layout_opt =
        n00b_type_static_layout(parsed.type_hash);
    if (n00b_option_is_set(layout_opt)) {
        scan_kind = n00b_option_get(layout_opt)->scan_kind;
    }

    n00b_static_image_request_t request = {
        .version            = N00B_STATIC_IMAGE_CONTRACT_VERSION,
        .type_hash          = parsed.type_hash,
        .type_name          = parsed.type_name,
        .symbol_prefix      = parsed.symbol_prefix,
        .entry_attr         = parsed.entry_attr,
        .payload_kind       = N00B_STATIC_IMAGE_PAYLOAD_NONE,
        .payload            = NULL,
        .payload_len        = 0,
        .args               = parsed.args,
        .arg_count          = parsed.arg_count,
        .target_abi         = parsed.abi,
        .object_flags       = parsed.readonly ? N00B_STATIC_OBJECT_F_READONLY
                                               : N00B_STATIC_OBJECT_F_MUTABLE,
        .required_scan_kind = scan_kind,
    };

    n00b_static_image_builder_t builder;
    n00b_static_image_status_t status =
        n00b_static_image_build(&request, &builder);
    if (status != N00B_STATIC_IMAGE_OK) {
        fprintf(stderr, "%s", builder.error
                                ? builder.error
                                : n00b_static_image_status_name(status));
        n00b_static_image_builder_destroy(&builder);
        n00b_shutdown();
        free(input);
        free_request(&parsed);
        return 3;
    }

    printf("NCC_STATIC_INIT_OK %s\n%s", builder.expr, builder.decls);

    n00b_static_image_builder_destroy(&builder);
    n00b_shutdown();
    free(input);
    free_request(&parsed);
    return 0;
}
