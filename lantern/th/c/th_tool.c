#include <stdio.h>
#include <string.h>

#include "typehash.h"

#ifdef TH_TOOL_NFC
void th_install_nfc(th_ctx *ctx);
#endif

static th_ctx *fresh(void)
{
    th_ctx *ctx = th_ctx_new();

#ifdef TH_TOOL_NFC
    th_install_nfc(ctx);
#endif

    return ctx;
}

static void emit(th_ctx *ctx, th_type *t)
{
    const uint8_t *p;
    size_t         len;

    if (!th_encode(ctx, t, &p, &len)) {
        printf("ERROR\t%s\n", th_last_error(ctx));

        return;
    }

    printf("%llu\t", (unsigned long long)th_hash(ctx, t));

    for (size_t i = 0; i < len; i++) {
        printf("%02x", p[i]);
    }
    printf("\n");
}

static th_type *parse_or_report(th_ctx *ctx, const char *s)
{
    th_error err = {0};
    th_type *t   = th_parse(ctx, s, &err);

    if (!t) {
        printf("ERROR\t%s\n", err.msg ? err.msg : "parse failed");
    }

    return t;
}

static void do_op(th_ctx *ctx, const char *op, char *rest)
{
    if (!strcmp(op, "unify")) {
        char *tab = strchr(rest, '\t');

        if (!tab) {
            printf("ERROR\tunify needs two tab-separated types\n");

            return;
        }
        *tab = 0;

        th_type *a = parse_or_report(ctx, rest);

        if (!a) {
            return;
        }

        th_type *b = parse_or_report(ctx, tab + 1);

        if (!b) {
            return;
        }

        th_type *r = th_unify(ctx, a, b);

        if (!r) {
            printf("BOTTOM\n");

            return;
        }
        emit(ctx, r);

        return;
    }

    th_type *t = parse_or_report(ctx, rest);

    if (!t) {
        return;
    }

    if (!strcmp(op, "instantiate")) {
        emit(ctx, th_instantiate(ctx, t));
    }
    else if (!strcmp(op, "generalize")) {
        emit(ctx, th_generalize(ctx, t));
    }
    else if (!strcmp(op, "freevars")) {
        printf("%zu\n", th_free_vars(ctx, t, nullptr));
    }
    else if (!strcmp(op, "isscheme")) {
        printf("%d\n", th_is_scheme(t) ? 1 : 0);
    }
    else if (!strcmp(op, "isclosed")) {
        printf("%d\n", th_is_closed(ctx, t) ? 1 : 0);
    }
    else if (!strcmp(op, "roundtrip")) {
        emit(ctx, th_clone(ctx, t));
    }
    else {
        printf("ERROR\tunknown op %s\n", op);
    }
}

int main(void)
{
    char line[8192];

    while (fgets(line, sizeof(line), stdin)) {
        size_t n = strlen(line);

        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = 0;
        }

        if (!n) {
            continue;
        }

        th_ctx *ctx = fresh();

        if (line[0] == ':') {
            char *sp = strchr(line, ' ');

            if (!sp) {
                printf("ERROR\tmalformed command\n");
                th_ctx_free(ctx);
                continue;
            }
            *sp = 0;
            do_op(ctx, line + 1, sp + 1);
            th_ctx_free(ctx);
            continue;
        }

        th_type *t = parse_or_report(ctx, line);

        if (t) {
            emit(ctx, t);
        }
        th_ctx_free(ctx);
    }

    return 0;
}
