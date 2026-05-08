#include "vendor/linenoise.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *linenoiseEditMore = "linenoise editing is unavailable on Windows";

static char *
ln_strdup(const char *s)
{
    size_t len = strlen(s);
    char  *out = malloc(len + 1);

    if (!out) {
        return NULL;
    }

    memcpy(out, s, len + 1);
    return out;
}

int
linenoiseEditStart(struct linenoiseState *l, int stdin_fd, int stdout_fd,
                   char *buf, size_t buflen, const char *prompt)
{
    (void)stdin_fd;
    (void)stdout_fd;
    if (!l || !buf || buflen == 0) {
        return -1;
    }
    memset(l, 0, sizeof(*l));
    l->buf    = buf;
    l->buflen = buflen;
    l->prompt = prompt;
    buf[0]    = '\0';
    return 0;
}

char *
linenoiseEditFeed(struct linenoiseState *l)
{
    (void)l;
    return NULL;
}

void
linenoiseEditStop(struct linenoiseState *l)
{
    (void)l;
}

void
linenoiseHide(struct linenoiseState *l)
{
    (void)l;
}

void
linenoiseShow(struct linenoiseState *l)
{
    (void)l;
}

char *
linenoise(const char *prompt)
{
    char   stack_buf[4096];
    size_t len;

    if (prompt) {
        fputs(prompt, stdout);
        fflush(stdout);
    }

    if (!fgets(stack_buf, sizeof(stack_buf), stdin)) {
        return NULL;
    }

    len = strlen(stack_buf);
    while (len > 0 && (stack_buf[len - 1] == '\n' || stack_buf[len - 1] == '\r')) {
        stack_buf[--len] = '\0';
    }

    return ln_strdup(stack_buf);
}

void
linenoiseFree(void *ptr)
{
    if (ptr && ptr != linenoiseEditMore) {
        free(ptr);
    }
}

void
linenoiseSetCompletionCallback(linenoiseCompletionCallback *fn)
{
    (void)fn;
}

void
linenoiseSetHintsCallback(linenoiseHintsCallback *fn)
{
    (void)fn;
}

void
linenoiseSetFreeHintsCallback(linenoiseFreeHintsCallback *fn)
{
    (void)fn;
}

void
linenoiseAddCompletion(linenoiseCompletions *lc, const char *str)
{
    (void)lc;
    (void)str;
}

int
linenoiseHistoryAdd(const char *line)
{
    (void)line;
    return 1;
}

int
linenoiseHistorySetMaxLen(int len)
{
    (void)len;
    return 1;
}

int
linenoiseHistorySave(const char *filename)
{
    (void)filename;
    return 0;
}

int
linenoiseHistoryLoad(const char *filename)
{
    (void)filename;
    return 0;
}

void
linenoiseClearScreen(void)
{
}

void
linenoiseSetMultiLine(int ml)
{
    (void)ml;
}

void
linenoisePrintKeyCodes(void)
{
}

void
linenoiseMaskModeEnable(void)
{
}

void
linenoiseMaskModeDisable(void)
{
}
