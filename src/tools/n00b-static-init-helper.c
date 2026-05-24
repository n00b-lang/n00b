#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "n00b.h"
#include "core/rwlock.h"
#include "core/static_image.h"
#include "core/type_info.h"

extern void n00b_init_simple(int argc, char *argv[]);
extern void n00b_shutdown(void);

typedef struct {
    char                    *type_name;
    uint64_t                 type_hash;
    char                    *symbol_prefix;
    char                    *entry_attr;
    char                    *container_kind;
    char                    *container_target;
    char                    *element_type_name;
    uint64_t                 element_type_hash;
    uint64_t                 data_type_hash;
    uint64_t                 container_len;
    uint64_t                 container_cap;
    char                    *element_scan_kind;
    char                    *element_scan_cb;
    char                    *element_scan_user;
    char                    *element_shape_decl;
    char                    *identity_namespace;
    char                    *identity_object_key;
    char                    *identity_payload_key;
    bool                     readonly;
    bool                     have_abi;
    n00b_static_image_abi_t  abi;
    uint64_t                 expected_arg_count;
    n00b_static_init_arg_t  *args;
    uint64_t                 arg_count;
    char                   **cinit_items;
    uint64_t                 cinit_count;
    uint64_t                 cinit_cap;
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
append_cinit(helper_request_t *req, char *expr)
{
    if (req->cinit_count == req->cinit_cap) {
        uint64_t new_cap = req->cinit_cap ? req->cinit_cap * 2u : 8u;
        char **new_items =
            realloc(req->cinit_items, (size_t)new_cap * sizeof(*new_items));
        if (!new_items) {
            return false;
        }
        req->cinit_items = new_items;
        req->cinit_cap   = new_cap;
    }

    req->cinit_items[req->cinit_count++] = expr;
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

    if (strcmp(kind, "cinit") == 0) {
        uint64_t len = 0;
        if (!parse_u64_token(next_token(&p), &len)) {
            free((void *)arg.name);
            return false;
        }

        char *hex = trim(p);
        bool  ok  = false;
        char *expr = hex_decode_exact(hex, len, true, &ok);
        free((void *)arg.name);
        if (!ok) {
            return false;
        }
        if (!append_cinit(req, expr)) {
            free(expr);
            return false;
        }
        return true;
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
    uint64_t line_no = 0;

    for (char *line = input; line;) {
        char *next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        line_no++;

        line = trim(line);
        if (!*line || strcmp(line, "NCC_STATIC_INIT 1") == 0
            || strcmp(line, "end") == 0) {
            line = next;
            continue;
        }

        if (strncmp(line, "type_hex ", 9) == 0) {
            free(req->type_name);
            req->type_name = hex_decode_cstr(trim(line + 9));
            if (!req->type_name) {
                fprintf(stderr, "bad request line %llu: invalid type_hex",
                        (unsigned long long)line_no);
                return false;
            }
        }
        else if (strncmp(line, "type_hash ", 10) == 0) {
            if (!parse_u64_token(trim(line + 10), &req->type_hash)) {
                fprintf(stderr, "bad request line %llu: invalid type_hash",
                        (unsigned long long)line_no);
                return false;
            }
        }
        else if (strncmp(line, "prefix ", 7) == 0) {
            free(req->symbol_prefix);
            req->symbol_prefix = dup_cstr(trim(line + 7));
            if (!req->symbol_prefix) {
                fprintf(stderr, "bad request line %llu: invalid prefix",
                        (unsigned long long)line_no);
                return false;
            }
        }
        else if (strncmp(line, "container_kind ", 15) == 0) {
            free(req->container_kind);
            req->container_kind = dup_cstr(trim(line + 15));
            if (!req->container_kind) {
                fprintf(stderr, "bad request line %llu: invalid container_kind",
                        (unsigned long long)line_no);
                return false;
            }
        }
        else if (strncmp(line, "container_target ", 17) == 0) {
            free(req->container_target);
            req->container_target = dup_cstr(trim(line + 17));
            if (!req->container_target) {
                fprintf(stderr, "bad request line %llu: invalid container_target",
                        (unsigned long long)line_no);
                return false;
            }
        }
        else if (strncmp(line, "element_type_hex ", 17) == 0) {
            free(req->element_type_name);
            req->element_type_name = hex_decode_cstr(trim(line + 17));
            if (!req->element_type_name) {
                fprintf(stderr, "bad request line %llu: invalid element_type_hex",
                        (unsigned long long)line_no);
                return false;
            }
        }
        else if (strncmp(line, "element_type_hash ", 18) == 0) {
            if (!parse_u64_token(trim(line + 18), &req->element_type_hash)) {
                fprintf(stderr, "bad request line %llu: invalid element_type_hash",
                        (unsigned long long)line_no);
                return false;
            }
        }
        else if (strncmp(line, "data_type_hash ", 15) == 0) {
            if (!parse_u64_token(trim(line + 15), &req->data_type_hash)) {
                fprintf(stderr, "bad request line %llu: invalid data_type_hash",
                        (unsigned long long)line_no);
                return false;
            }
        }
        else if (strncmp(line, "readonly ", 9) == 0) {
            uint64_t value = 0;
            if (!parse_u64_token(trim(line + 9), &value)) {
                fprintf(stderr, "bad request line %llu: invalid readonly",
                        (unsigned long long)line_no);
                return false;
            }
            req->readonly = value != 0;
        }
        else if (strncmp(line, "len ", 4) == 0) {
            if (!parse_u64_token(trim(line + 4), &req->container_len)) {
                fprintf(stderr, "bad request line %llu: invalid len",
                        (unsigned long long)line_no);
                return false;
            }
        }
        else if (strncmp(line, "cap ", 4) == 0) {
            if (!parse_u64_token(trim(line + 4), &req->container_cap)) {
                fprintf(stderr, "bad request line %llu: invalid cap",
                        (unsigned long long)line_no);
                return false;
            }
        }
        else if (strncmp(line, "abi ", 4) == 0) {
            if (!parse_abi_line(line, req)) {
                fprintf(stderr, "bad request line %llu: invalid abi",
                        (unsigned long long)line_no);
                return false;
            }
        }
        else if (strncmp(line, "entry_attr_hex ", 15) == 0) {
            free(req->entry_attr);
            req->entry_attr = hex_decode_cstr(trim(line + 15));
            if (!req->entry_attr) {
                fprintf(stderr, "bad request line %llu: invalid entry_attr_hex",
                        (unsigned long long)line_no);
                return false;
            }
        }
        else if (strncmp(line, "identity_namespace_hex ", 23) == 0) {
            free(req->identity_namespace);
            req->identity_namespace = hex_decode_cstr(trim(line + 23));
            if (!req->identity_namespace) {
                fprintf(stderr,
                        "bad request line %llu: invalid identity_namespace_hex",
                        (unsigned long long)line_no);
                return false;
            }
        }
        else if (strncmp(line, "identity_object_key_hex ", 24) == 0) {
            free(req->identity_object_key);
            req->identity_object_key = hex_decode_cstr(trim(line + 24));
            if (!req->identity_object_key) {
                fprintf(stderr,
                        "bad request line %llu: invalid identity_object_key_hex",
                        (unsigned long long)line_no);
                return false;
            }
        }
        else if (strncmp(line, "identity_payload_key_hex ", 25) == 0) {
            free(req->identity_payload_key);
            req->identity_payload_key = hex_decode_cstr(trim(line + 25));
            if (!req->identity_payload_key) {
                fprintf(stderr,
                        "bad request line %llu: invalid identity_payload_key_hex",
                        (unsigned long long)line_no);
                return false;
            }
        }
        else if (strncmp(line, "element_scan_kind ", 18) == 0) {
            free(req->element_scan_kind);
            req->element_scan_kind = dup_cstr(trim(line + 18));
            if (!req->element_scan_kind) {
                fprintf(stderr, "bad request line %llu: invalid element_scan_kind",
                        (unsigned long long)line_no);
                return false;
            }
        }
        else if (strncmp(line, "element_scan_cb_hex ", 20) == 0) {
            free(req->element_scan_cb);
            req->element_scan_cb = hex_decode_cstr(trim(line + 20));
            if (!req->element_scan_cb) {
                fprintf(stderr, "bad request line %llu: invalid element_scan_cb_hex",
                        (unsigned long long)line_no);
                return false;
            }
        }
        else if (strncmp(line, "element_scan_user_hex ", 22) == 0) {
            free(req->element_scan_user);
            req->element_scan_user = hex_decode_cstr(trim(line + 22));
            if (!req->element_scan_user) {
                fprintf(stderr,
                        "bad request line %llu: invalid element_scan_user_hex",
                        (unsigned long long)line_no);
                return false;
            }
        }
        else if (strcmp(line, "element_shape_decl_hex") == 0) {
            free(req->element_shape_decl);
            req->element_shape_decl = dup_cstr("");
            if (!req->element_shape_decl) {
                fprintf(stderr,
                        "bad request line %llu: invalid element_shape_decl_hex",
                        (unsigned long long)line_no);
                return false;
            }
        }
        else if (strncmp(line, "element_shape_decl_hex ", 23) == 0) {
            free(req->element_shape_decl);
            req->element_shape_decl = hex_decode_cstr(trim(line + 23));
            if (!req->element_shape_decl) {
                fprintf(stderr,
                        "bad request line %llu: invalid element_shape_decl_hex",
                        (unsigned long long)line_no);
                return false;
            }
        }
        else if (strncmp(line, "arg_count ", 10) == 0) {
            if (!parse_u64_token(trim(line + 10), &req->expected_arg_count)) {
                fprintf(stderr, "bad request line %llu: invalid arg_count",
                        (unsigned long long)line_no);
                return false;
            }
        }
        else if (strncmp(line, "arg ", 4) == 0) {
            if (!parse_arg_line(line, req)) {
                fprintf(stderr, "bad request line %llu: invalid arg",
                        (unsigned long long)line_no);
                return false;
            }
        }
        else {
            fprintf(stderr, "bad request line %llu: unknown field '%s'",
                    (unsigned long long)line_no, line);
            return false;
        }

        line = next;
    }

    if (!req->type_name || req->type_hash == 0 || !req->symbol_prefix
        || !req->entry_attr || !req->have_abi
        || req->arg_count + req->cinit_count != req->expected_arg_count) {
        fprintf(stderr,
                "bad request: missing required fields or arg_count mismatch "
                "(type=%d type_hash=%llu prefix=%d entry_attr=%d abi=%d "
                "args=%llu cinit=%llu expected=%llu)",
                req->type_name != NULL, (unsigned long long)req->type_hash,
                req->symbol_prefix != NULL, req->entry_attr != NULL,
                req->have_abi, (unsigned long long)req->arg_count,
                (unsigned long long)req->cinit_count,
                (unsigned long long)req->expected_arg_count);
        return false;
    }

    return true;
}

static void
free_request(helper_request_t *req)
{
    free(req->type_name);
    free(req->symbol_prefix);
    free(req->entry_attr);
    free(req->container_kind);
    free(req->container_target);
    free(req->element_type_name);
    free(req->element_scan_kind);
    free(req->element_scan_cb);
    free(req->element_scan_user);
    free(req->element_shape_decl);
    free(req->identity_namespace);
    free(req->identity_object_key);
    free(req->identity_payload_key);
    for (uint64_t i = 0; i < req->arg_count; i++) {
        free((void *)req->args[i].name);
        if (req->args[i].kind == N00B_STATIC_INIT_ARG_BYTES) {
            free((void *)req->args[i].bytes.data);
        }
    }
    free(req->args);
    for (uint64_t i = 0; i < req->cinit_count; i++) {
        free(req->cinit_items[i]);
    }
    free(req->cinit_items);
}

static void
emit_c_string_literal(const char *s)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '\\':
            fputs("\\\\", stdout);
            break;
        case '"':
            fputs("\\\"", stdout);
            break;
        case '\n':
            fputs("\\n", stdout);
            break;
        case '\r':
            fputs("\\r", stdout);
            break;
        case '\t':
            fputs("\\t", stdout);
            break;
        default:
            if (*p >= 0x20 && *p < 0x7f) {
                putchar((char)*p);
            }
            else {
                printf("\\%03o", (unsigned)*p);
            }
            break;
        }
    }
    putchar('"');
}

static bool
has_identity(const helper_request_t *req)
{
    return req->identity_namespace && req->identity_namespace[0]
        && req->identity_object_key && req->identity_object_key[0]
        && req->identity_payload_key && req->identity_payload_key[0];
}

static void
emit_identity_decls(const helper_request_t *req, bool include_object_identity)
{
    if (!has_identity(req)) {
        return;
    }

    printf("static const n00b_static_identity_t %s_payload_id={"
           ".version=1u,"
           ".kind=N00B_STATIC_IDENTITY_NCC_STATIC_IMAGE_PAYLOAD,"
           ".namespace_id=",
           req->symbol_prefix);
    emit_c_string_literal(req->identity_namespace);
    printf(",.object_key=");
    emit_c_string_literal(req->identity_payload_key);
    printf("};");

    if (!include_object_identity) {
        return;
    }

    printf("static const n00b_static_identity_t %s_obj_id={"
           ".version=1u,"
           ".kind=N00B_STATIC_IDENTITY_NCC_STATIC_IMAGE_OBJECT,"
           ".namespace_id=",
           req->symbol_prefix);
    emit_c_string_literal(req->identity_namespace);
    printf(",.object_key=");
    emit_c_string_literal(req->identity_object_key);
    printf("};");
}

static void
emit_request_identity_fields(const helper_request_t *req)
{
    if (!has_identity(req)) {
        printf(".identity_namespace=nullptr,.identity_object_key=nullptr,"
               ".identity_payload_key=nullptr,");
        return;
    }

    printf(".identity_namespace=");
    emit_c_string_literal(req->identity_namespace);
    printf(",.identity_object_key=");
    emit_c_string_literal(req->identity_object_key);
    printf(",.identity_payload_key=");
    emit_c_string_literal(req->identity_payload_key);
    printf(",");
}

static uint64_t
helper_hash_cstr(const char *s)
{
    uint64_t h = UINT64_C(1469598103934665603);
    while (s && *s) {
        h ^= (unsigned char)*s++;
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static const char *
or_default(const char *s, const char *fallback)
{
    return s && *s ? s : fallback;
}

static void
emit_cinit_items(const helper_request_t *req)
{
    for (uint64_t i = 0; i < req->cinit_count; i++) {
        if (i != 0) {
            putchar(',');
        }
        fputs(req->cinit_items[i], stdout);
    }
    if (req->cinit_count == 0) {
        fputs("0", stdout);
    }
}

static int
emit_list_image(const helper_request_t *req)
{
    if (!req->element_type_name || !req->data_type_hash
        || req->container_cap < req->container_len
        || req->cinit_count != req->container_len) {
        fprintf(stderr, "bad n00b list static initializer request");
        return 6;
    }

    bool pointer_target = req->container_target
                       && strcmp(req->container_target, "pointer") == 0;
    const char *flags = req->readonly ? "N00B_STATIC_OBJECT_F_READONLY"
                                      : "N00B_STATIC_OBJECT_F_MUTABLE";
    const char *obj_const = req->readonly ? "const " : "";
    const char *scan_kind = or_default(req->element_scan_kind,
                                       "N00B_GC_SCAN_KIND_NONE");
    const char *scan_cb = or_default(req->element_scan_cb, "nullptr");
    const char *scan_user = or_default(req->element_scan_user, "nullptr");
    uint64_t data_id = helper_hash_cstr(req->symbol_prefix)
                     ^ UINT64_C(0x6c69737464617461);
    uint64_t lock_id = helper_hash_cstr(req->symbol_prefix)
                     ^ UINT64_C(0x6c6973746c6f636b);
    uint64_t obj_id = helper_hash_cstr(req->symbol_prefix)
                    ^ UINT64_C(0x6c6973746f626a);
    unsigned contract_version = (unsigned)req->abi.version;
    unsigned target_endian    = (unsigned)req->abi.endian;

    printf("NCC_STATIC_INIT_OK ");
    if (pointer_target) {
        printf("&%s_obj\n", req->symbol_prefix);
    }
    else {
        printf("{.data=%s_data,.len=(size_t)%lluULL,.cap=(size_t)%lluULL,"
               ".lock=(n00b_rwlock_t*)%s_lock_storage,"
               ".allocator=nullptr,.scan_kind=%s,"
               ".scan_cb=%s,.scan_user=%s}\n",
               req->symbol_prefix,
               (unsigned long long)req->container_len,
               (unsigned long long)req->container_cap,
               req->symbol_prefix, scan_kind, scan_cb, scan_user);
    }

    emit_identity_decls(req, pointer_target);
    printf("%s", req->element_shape_decl ? req->element_shape_decl : "");
    printf("static %s %s_data[%llu]={",
           req->element_type_name, req->symbol_prefix,
           (unsigned long long)(req->container_cap ? req->container_cap : 1));
    emit_cinit_items(req);
    printf("};");
    printf("static const n00b_static_object_desc_t %s_data_desc={"
           ".start=(const void*)%s_data,"
           ".len=(uint64_t)sizeof(%s_data),"
           ".tinfo=%lluULL,.scan_kind=%s,.scan_cb=%s,.scan_user=%s,"
           ".object_id=%lluULL,.file=__FILE__,.identity=",
           req->symbol_prefix, req->symbol_prefix, req->symbol_prefix,
           (unsigned long long)req->data_type_hash,
           scan_kind, scan_cb, scan_user, (unsigned long long)data_id);
    printf("%s", has_identity(req) ? "&" : "nullptr");
    if (has_identity(req)) {
        printf("%s_payload_id", req->symbol_prefix);
    }
    printf(",.flags=%s};", flags);
    printf("static const n00b_static_object_desc_t * const "
           "%s_data_entry %s=&%s_data_desc;",
           req->symbol_prefix, req->entry_attr ? req->entry_attr : "",
           req->symbol_prefix);

    printf("static unsigned char %s_lock_storage[%llu] "
           "__attribute__((aligned(%llu)))={0};",
           req->symbol_prefix,
           (unsigned long long)sizeof(n00b_rwlock_t),
           (unsigned long long)_Alignof(n00b_rwlock_t));
    printf("static const n00b_static_object_desc_t %s_lock_desc={"
           ".start=(const void*)%s_lock_storage,"
           ".len=(uint64_t)sizeof(%s_lock_storage),"
           ".tinfo=0,.scan_kind=N00B_GC_SCAN_KIND_NONE,"
           ".scan_cb=nullptr,.scan_user=nullptr,"
           ".object_id=%lluULL,.file=__FILE__,.identity=nullptr,"
           ".flags=N00B_STATIC_OBJECT_F_MUTABLE|N00B_STATIC_OBJECT_F_INIT_RWLOCK};",
           req->symbol_prefix, req->symbol_prefix, req->symbol_prefix,
           (unsigned long long)lock_id);
    printf("static const n00b_static_object_desc_t * const "
           "%s_lock_entry %s=&%s_lock_desc;",
           req->symbol_prefix, req->entry_attr ? req->entry_attr : "",
           req->symbol_prefix);

    if (pointer_target) {
        printf("_Static_assert((__builtin_offsetof(%s,data)%%sizeof(void*))==0,"
               "\"list data pointer must be pointer-aligned\");",
               req->type_name);
        printf("static const uint64_t %s_obj_offsets[]={"
               "__builtin_offsetof(%s,data)/sizeof(void*)};",
               req->symbol_prefix, req->type_name);
        printf("static n00b_gc_struct_layout_t %s_obj_shape={"
               ".stride=(sizeof(%s)/sizeof(void*)),.count=1,"
               ".offset_count=1,.offsets=%s_obj_offsets};",
               req->symbol_prefix, req->type_name, req->symbol_prefix);
        printf("static %s%s %s_obj={"
               ".data=%s_data,.len=(size_t)%lluULL,.cap=(size_t)%lluULL,"
               ".lock=(n00b_rwlock_t*)%s_lock_storage,"
               ".allocator=nullptr,.scan_kind=%s,"
               ".scan_cb=%s,.scan_user=%s};",
               obj_const, req->type_name, req->symbol_prefix,
               req->symbol_prefix,
               (unsigned long long)req->container_len,
               (unsigned long long)req->container_cap,
               req->symbol_prefix, scan_kind, scan_cb, scan_user);
        printf("static const n00b_static_object_desc_t %s_obj_desc={"
               ".start=(const void*)&%s_obj,"
               ".len=(uint64_t)sizeof(%s_obj),"
               ".tinfo=%lluULL,.scan_kind=N00B_GC_SCAN_KIND_CALLBACK,"
               ".scan_cb=n00b_gc_scan_cb_struct_layout,"
               ".scan_user=&%s_obj_shape,"
               ".object_id=%lluULL,.file=__FILE__,.identity=",
               req->symbol_prefix, req->symbol_prefix, req->symbol_prefix,
               (unsigned long long)req->type_hash, req->symbol_prefix,
               (unsigned long long)obj_id);
        printf("%s", has_identity(req) ? "&" : "nullptr");
        if (has_identity(req)) {
            printf("%s_obj_id", req->symbol_prefix);
        }
        printf(",.flags=%s};", flags);
        printf("static const n00b_static_object_desc_t * const "
               "%s_obj_entry %s=&%s_obj_desc;",
               req->symbol_prefix, req->entry_attr ? req->entry_attr : "",
               req->symbol_prefix);
    }

    printf("static const n00b_static_image_request_t %s_request={"
           ".version=%u,"
           ".type_hash=%lluULL,.type_name=",
           req->symbol_prefix, contract_version,
           (unsigned long long)req->type_hash);
    emit_c_string_literal(req->type_name);
    printf(",.symbol_prefix=");
    emit_c_string_literal(req->symbol_prefix);
    printf(",.entry_attr=");
    emit_c_string_literal(req->entry_attr ? req->entry_attr : "");
    printf(",.payload_kind=N00B_STATIC_IMAGE_PAYLOAD_NONE,"
           ".payload=nullptr,.payload_len=0,.args=nullptr,.arg_count=0,"
           ".target_abi={.version=%u,"
           ".pointer_bytes=(uint8_t)sizeof(void*),"
           ".size_t_bytes=(uint8_t)sizeof(size_t),.char_bits=8,"
           ".endian=%u},"
           ".object_flags=%s,.required_scan_kind=N00B_GC_SCAN_KIND_CALLBACK,",
           contract_version, target_endian, flags);
    emit_request_identity_fields(req);
    printf("};");
    printf("static const n00b_static_image_dependency_t %s_deps[]={"
           "{.desc=&%s_data_desc,.relocation_offset=0,.role=\"data\"},",
           req->symbol_prefix, req->symbol_prefix);
    if (pointer_target) {
        printf("{.desc=&%s_lock_desc,.relocation_offset=__builtin_offsetof(%s,lock),"
               ".role=\"lock\"}};",
               req->symbol_prefix, req->type_name);
    }
    else {
        printf("{.desc=&%s_lock_desc,.relocation_offset=0,.role=\"lock\"}};",
               req->symbol_prefix);
    }
    if (pointer_target) {
        printf("static const n00b_static_image_response_t %s_response "
               "__attribute__((used))={"
               ".version=%u,"
               ".request=&%s_request,"
               ".object_start=(const void*)&%s_obj,"
               ".object_len=(uint64_t)sizeof(%s_obj),"
               ".scan_kind=N00B_GC_SCAN_KIND_CALLBACK,"
               ".scan_cb=n00b_gc_scan_cb_struct_layout,"
               ".scan_user=&%s_obj_shape,.dependencies=%s_deps,"
               ".dependency_count=2};",
               req->symbol_prefix, contract_version,
               req->symbol_prefix, req->symbol_prefix,
               req->symbol_prefix, req->symbol_prefix, req->symbol_prefix);
    }
    else {
        printf("static const n00b_static_image_response_t %s_response "
               "__attribute__((used))={"
               ".version=%u,"
               ".request=&%s_request,"
               ".object_start=nullptr,.object_len=0,"
               ".scan_kind=N00B_GC_SCAN_KIND_NONE,.scan_cb=nullptr,"
               ".scan_user=nullptr,.dependencies=%s_deps,"
               ".dependency_count=2};",
               req->symbol_prefix, contract_version,
               req->symbol_prefix, req->symbol_prefix);
    }

    return 0;
}

static void
emit_array_identity_decl(const helper_request_t *req)
{
    if (!has_identity(req)) {
        return;
    }

    printf("static const n00b_static_identity_t %s_data_id={"
           ".version=1u,"
           ".kind=N00B_STATIC_IDENTITY_NCC_ARRAY_DATA,"
           ".namespace_id=",
           req->symbol_prefix);
    emit_c_string_literal(req->identity_namespace);
    printf(",.object_key=");
    emit_c_string_literal(req->identity_payload_key);
    printf("};");
}

static int
emit_array_image(const helper_request_t *req)
{
    if (!req->element_type_name || !req->data_type_hash
        || req->container_cap != req->container_len
        || req->cinit_count != req->container_len) {
        fprintf(stderr, "bad n00b array static initializer request");
        return 6;
    }

    const char *flags = req->readonly ? "N00B_STATIC_OBJECT_F_READONLY"
                                      : "N00B_STATIC_OBJECT_F_MUTABLE";
    const char *scan_kind = or_default(req->element_scan_kind,
                                       "N00B_GC_SCAN_KIND_NONE");
    const char *scan_cb = or_default(req->element_scan_cb, "nullptr");
    const char *scan_user = or_default(req->element_scan_user, "nullptr");
    uint64_t data_id = helper_hash_cstr(req->symbol_prefix)
                     ^ UINT64_C(0x6172726179646174);
    unsigned contract_version = (unsigned)req->abi.version;
    unsigned target_endian    = (unsigned)req->abi.endian;

    printf("NCC_STATIC_INIT_OK %s_data\n", req->symbol_prefix);
    emit_array_identity_decl(req);
    printf("%s", req->element_shape_decl ? req->element_shape_decl : "");
    printf("static %s %s_data[%llu]={",
           req->element_type_name, req->symbol_prefix,
           (unsigned long long)(req->container_len ? req->container_len : 1));
    emit_cinit_items(req);
    printf("};");
    printf("static const n00b_static_object_desc_t %s_data_desc={"
           ".start=(const void*)%s_data,"
           ".len=(uint64_t)sizeof(%s_data),"
           ".tinfo=%lluULL,.scan_kind=%s,.scan_cb=%s,.scan_user=%s,"
           ".object_id=%lluULL,.file=__FILE__,.identity=",
           req->symbol_prefix, req->symbol_prefix, req->symbol_prefix,
           (unsigned long long)req->data_type_hash,
           scan_kind, scan_cb, scan_user, (unsigned long long)data_id);
    printf("%s", has_identity(req) ? "&" : "nullptr");
    if (has_identity(req)) {
        printf("%s_data_id", req->symbol_prefix);
    }
    printf(",.flags=%s};", flags);
    printf("static const n00b_static_object_desc_t * const "
           "%s_data_entry %s=&%s_data_desc;",
           req->symbol_prefix, req->entry_attr ? req->entry_attr : "",
           req->symbol_prefix);

    printf("static const n00b_static_image_request_t %s_request={"
           ".version=%u,"
           ".type_hash=%lluULL,.type_name=",
           req->symbol_prefix, contract_version,
           (unsigned long long)req->type_hash);
    emit_c_string_literal(req->type_name);
    printf(",.symbol_prefix=");
    emit_c_string_literal(req->symbol_prefix);
    printf(",.entry_attr=");
    emit_c_string_literal(req->entry_attr ? req->entry_attr : "");
    printf(",.payload_kind=N00B_STATIC_IMAGE_PAYLOAD_NONE,"
           ".payload=nullptr,.payload_len=0,.args=nullptr,.arg_count=0,"
           ".target_abi={.version=%u,"
           ".pointer_bytes=(uint8_t)sizeof(void*),"
           ".size_t_bytes=(uint8_t)sizeof(size_t),.char_bits=8,"
           ".endian=%u},"
           ".object_flags=%s,.required_scan_kind=N00B_GC_SCAN_KIND_NONE,",
           contract_version, target_endian, flags);
    emit_request_identity_fields(req);
    printf("};");
    printf("static const n00b_static_image_dependency_t %s_deps[]={"
           "{.desc=&%s_data_desc,.relocation_offset=0,.role=\"data\"}};",
           req->symbol_prefix, req->symbol_prefix);
    printf("static const n00b_static_image_response_t %s_response "
           "__attribute__((used))={"
           ".version=%u,.request=&%s_request,"
           ".object_start=nullptr,.object_len=0,"
           ".scan_kind=N00B_GC_SCAN_KIND_NONE,.scan_cb=nullptr,"
           ".scan_user=nullptr,.dependencies=%s_deps,"
           ".dependency_count=1};",
           req->symbol_prefix, contract_version, req->symbol_prefix,
           req->symbol_prefix);

    return 0;
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

    if (parsed.container_kind && strcmp(parsed.container_kind, "array") == 0) {
        int status = emit_array_image(&parsed);
        n00b_shutdown();
        free(input);
        free_request(&parsed);
        return status;
    }

    if (parsed.container_kind && strcmp(parsed.container_kind, "list") == 0) {
        int status = emit_list_image(&parsed);
        n00b_shutdown();
        free(input);
        free_request(&parsed);
        return status;
    }

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
        .identity_namespace = parsed.identity_namespace,
        .identity_object_key = parsed.identity_object_key,
        .identity_payload_key = parsed.identity_payload_key,
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
