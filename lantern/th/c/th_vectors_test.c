#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "typehash.h"

static int fails;

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

static void check_line(char *line, int lineno)
{
    char *tab1 = strchr(line, '\t');

    if (!tab1) {
        return;
    }

    *tab1 = 0;

    char *stream = tab1 + 1;
    char *tab2   = strchr(stream, '\t');

    if (!tab2) {
        return;
    }

    *tab2 = 0;

    const char *type = line;
    const char *want = tab2 + 1;
    th_ctx     *ctx  = fresh();
    th_error    err  = {0};
    th_type    *t    = th_parse(ctx, type, &err);

    if (!t) {
        printf("FAIL  line %d: %s does not parse\n", lineno, type);
        fails++;
        th_ctx_free(ctx);

        return;
    }

    char got[32];

    snprintf(got, sizeof(got), "%llu",
             (unsigned long long)th_hash(ctx, t));

    if (strcmp(got, want)) {
        printf("FAIL  line %d: %s hashes %s, spec says %s\n", lineno, type,
               got, want);
        fails++;
    }

    if (*stream) {
        const uint8_t *p;
        size_t         len;

        if (!th_encode(ctx, t, &p, &len)) {
            printf("FAIL  line %d: %s does not encode\n", lineno, type);
            fails++;
        }
        else {
            char hex[4096];
            size_t at = 0;

            for (size_t i = 0; i < len && at + 2 < sizeof(hex); i++) {
                at += (size_t)snprintf(hex + at, sizeof(hex) - at, "%02x",
                                       p[i]);
            }

            if (strcmp(hex, stream)) {
                printf("FAIL  line %d: %s encodes\n    %s\n  spec says\n"
                       "    %s\n",
                       lineno, type, hex, stream);
                fails++;
            }
        }
    }

    th_ctx_free(ctx);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "vectors.tsv";
    FILE       *f    = fopen(path, "rb");

    if (!f) {
        printf("FAIL  cannot open %s\n", path);

        return 1;
    }

    char line[8192];
    int  n = 0;
    int  lineno = 0;

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);

        lineno++;

        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = 0;
        }

        if (!len) {
            continue;
        }

        check_line(line, lineno);
        n++;
    }

    fclose(f);

    if (!fails) {
        printf("typehash: %d spec vectors match\n", n);
    }

    return fails ? 1 : 0;
}
