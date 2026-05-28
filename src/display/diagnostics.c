#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "n00b.h"
#include "internal/display/diagnostics.h"

typedef struct {
    bool                      initialized;
    bool                      enabled;
    bool                      owned_stream;
    n00b_display_diag_level_t level;
    FILE                     *stream;
} n00b_display_diag_state_t;

static n00b_display_diag_state_t g_diag = {
    .initialized = false,
    .enabled     = false,
    .owned_stream = false,
    .level       = N00B_DISPLAY_DIAG_OFF,
    .stream      = nullptr,
};

static int
ascii_tolower(int c)
{
    if (c >= 'A' && c <= 'Z') {
        return c + ('a' - 'A');
    }
    return c;
}

static bool
streq_ci(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }
    while (*a && *b) {
        if (ascii_tolower((unsigned char)*a) != ascii_tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static bool
is_falsey(const char *s)
{
    if (!s || !*s) {
        return true;
    }
    return streq_ci(s, "0")
        || streq_ci(s, "off")
        || streq_ci(s, "false")
        || streq_ci(s, "no");
}

static bool
is_truthy(const char *s)
{
    if (!s || !*s) {
        return false;
    }
    return streq_ci(s, "1")
        || streq_ci(s, "on")
        || streq_ci(s, "true")
        || streq_ci(s, "yes");
}

static n00b_display_diag_level_t
parse_level(const char *s, n00b_display_diag_level_t fallback)
{
    if (!s || !*s) {
        return fallback;
    }
    if (streq_ci(s, "off") || streq_ci(s, "0")) {
        return N00B_DISPLAY_DIAG_OFF;
    }
    if (streq_ci(s, "error") || streq_ci(s, "1")) {
        return N00B_DISPLAY_DIAG_ERROR;
    }
    if (streq_ci(s, "info") || streq_ci(s, "2")) {
        return N00B_DISPLAY_DIAG_INFO;
    }
    if (streq_ci(s, "trace") || streq_ci(s, "3")) {
        return N00B_DISPLAY_DIAG_TRACE;
    }
    return fallback;
}

static const char *
level_to_cstr(n00b_display_diag_level_t level)
{
    switch (level) {
    case N00B_DISPLAY_DIAG_ERROR:
        return "error";
    case N00B_DISPLAY_DIAG_INFO:
        return "info";
    case N00B_DISPLAY_DIAG_TRACE:
        return "trace";
    default:
        return "off";
    }
}

static void
diag_close_owned_stream(void)
{
    if (g_diag.owned_stream && g_diag.stream) {
        fclose(g_diag.stream);
    }
    g_diag.owned_stream = false;
    g_diag.stream       = nullptr;
}

static void
diag_ensure_initialized(void)
{
    if (g_diag.initialized) {
        return;
    }
    n00b_display_diag_init();
}

void
n00b_display_diag_init(void)
{
    if (g_diag.initialized) {
        return;
    }

    g_diag.initialized = true;
    g_diag.enabled     = false;
    g_diag.level       = N00B_DISPLAY_DIAG_OFF;
    g_diag.stream      = nullptr;
    g_diag.owned_stream = false;

    const char *diag_env = getenv("N00B_DISPLAY_DIAG");
    if (!diag_env || !*diag_env || is_falsey(diag_env)) {
        return;
    }

    g_diag.enabled = true;
    g_diag.level   = N00B_DISPLAY_DIAG_INFO;

    if (is_truthy(diag_env) || streq_ci(diag_env, "stderr")) {
        g_diag.stream = stderr;
    }
    else if (streq_ci(diag_env, "stdout")) {
        g_diag.stream = stdout;
    }
    else {
        FILE *fp = fopen(diag_env, "a");
        if (fp) {
            g_diag.stream       = fp;
            g_diag.owned_stream = true;
        }
        else {
            g_diag.stream = stderr;
        }
    }

    const char *level_env = getenv("N00B_DISPLAY_DIAG_LEVEL");
    g_diag.level = parse_level(level_env, g_diag.level);
    if (g_diag.level == N00B_DISPLAY_DIAG_OFF) {
        g_diag.enabled = false;
    }
}

void
n00b_display_diag_shutdown(void)
{
    if (!g_diag.initialized) {
        return;
    }

    diag_close_owned_stream();
    g_diag.enabled      = false;
    g_diag.level        = N00B_DISPLAY_DIAG_OFF;
    g_diag.initialized  = false;
}

void
n00b_display_diag_set_level(n00b_display_diag_level_t level)
{
    diag_ensure_initialized();

    if (level > N00B_DISPLAY_DIAG_TRACE) {
        level = N00B_DISPLAY_DIAG_TRACE;
    }

    g_diag.level   = level;
    g_diag.enabled = level != N00B_DISPLAY_DIAG_OFF;

    if (g_diag.enabled && !g_diag.stream) {
        g_diag.stream = stderr;
    }
}

void
n00b_display_diag_set_stream(FILE *stream)
{
    diag_ensure_initialized();
    diag_close_owned_stream();
    g_diag.stream = stream;
}

bool
n00b_display_diag_would_log(n00b_display_diag_level_t level)
{
    diag_ensure_initialized();
    if (level == N00B_DISPLAY_DIAG_OFF) {
        return false;
    }
    return g_diag.enabled
        && g_diag.stream != nullptr
        && level <= g_diag.level;
}

void
n00b_display_diag_log(n00b_display_diag_level_t level,
                       const char               *component,
                       const char               *fmt, ...)
{
    if (!n00b_display_diag_would_log(level) || !fmt) {
        return;
    }

    const char *tag = component ? component : "display";
    FILE       *out = g_diag.stream ? g_diag.stream : stderr;

    fprintf(out, "[display:%s:%s] ", tag, level_to_cstr(level));

    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);

    fputc('\n', out);
    fflush(out);
}
