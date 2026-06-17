#ifndef N00B_TEST_PORTABILITY_H
#define N00B_TEST_PORTABILITY_H

#if defined(_WIN32)

#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int
n00b_test_asprintf(char **out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list copy;
    va_copy(copy, ap);

    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);

    if (needed < 0) {
        va_end(ap);
        *out = NULL;
        return -1;
    }

    char *buf = malloc((size_t)needed + 1);

    if (!buf) {
        va_end(ap);
        *out = NULL;
        return -1;
    }

    int written = vsnprintf(buf, (size_t)needed + 1, fmt, ap);
    va_end(ap);

    if (written < 0) {
        free(buf);
        *out = NULL;
        return -1;
    }

    *out = buf;
    return written;
}

#define asprintf n00b_test_asprintf

static int
n00b_test_setenv(const char *name, const char *value, int overwrite)
{
    if (!overwrite) {
        char  *existing     = NULL;
        size_t existing_len = 0;

        if (_dupenv_s(&existing, &existing_len, name) == 0) {
            if (existing != NULL) {
                free(existing);
                return 0;
            }
        }
        free(existing);
    }

    return _putenv_s(name, value ? value : "");
}

static int
n00b_test_unsetenv(const char *name)
{
    return _putenv_s(name, "");
}

static int
n00b_test_mkdir(const char *path, int mode)
{
    (void)mode;
    return _mkdir(path);
}

static int
n00b_test_fcntl(int fd, int cmd, ...)
{
    (void)fd;
    (void)cmd;
    return 0;
}

static void *
n00b_test_memmem(const void *haystack,
                 size_t      haystack_len,
                 const void *needle,
                 size_t      needle_len)
{
    if (needle_len == 0) {
        return (void *)haystack;
    }
    if (haystack_len < needle_len) {
        return NULL;
    }

    const unsigned char *h = haystack;
    const unsigned char *n = needle;
    size_t              limit = haystack_len - needle_len;

    for (size_t i = 0; i <= limit; i++) {
        if (h[i] == n[0] && memcmp(h + i, n, needle_len) == 0) {
            return (void *)(h + i);
        }
    }

    return NULL;
}

static char
n00b_test_mkdtemp_digit(unsigned value)
{
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    return digits[value % 36u];
}

static char *
n00b_test_mkdtemp(char *tmpl)
{
    if (!tmpl) {
        errno = EINVAL;
        return NULL;
    }

    size_t len = strlen(tmpl);
    if (len < 6u) {
        errno = EINVAL;
        return NULL;
    }

    char *x = tmpl + len - 6u;
    for (size_t i = 0; i < 6u; i++) {
        if (x[i] != 'X') {
            errno = EINVAL;
            return NULL;
        }
    }

    unsigned seed = (unsigned)_getpid() ^ (unsigned)(uintptr_t)tmpl;
    for (unsigned attempt = 0; attempt < 1000000u; attempt++) {
        unsigned value = seed + attempt;
        for (size_t i = 0; i < 6u; i++) {
            x[i] = n00b_test_mkdtemp_digit(value);
            value /= 36u;
        }

        if (_mkdir(tmpl) == 0) {
            return tmpl;
        }
        if (errno != EEXIST) {
            return NULL;
        }
    }

    errno = EEXIST;
    return NULL;
}

#ifndef F_GETFL
#define F_GETFL 3
#endif
#ifndef F_SETFL
#define F_SETFL 4
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif
#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#endif

#define chdir _chdir
#define fcntl n00b_test_fcntl
#define getcwd _getcwd
#define memmem n00b_test_memmem
#define mkdir(path, mode) n00b_test_mkdir((path), (mode))
#define mkdtemp n00b_test_mkdtemp
#define pclose _pclose
#define pipe(fds) _pipe((fds), 65536, _O_BINARY)
#define popen _popen
#define rmdir _rmdir
#ifndef setenv
#define setenv n00b_test_setenv
#endif
#ifndef unsetenv
#define unsetenv n00b_test_unsetenv
#endif

#endif

#endif
