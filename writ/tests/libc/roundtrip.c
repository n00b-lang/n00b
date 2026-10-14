#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "libc_writ.h"

static int failures;

static void
expect(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", what);
        failures++;
    }
}

static void
formats(void)
{
    libc_bytes_t buf = {0};
    int written = 0;
    expect(libc_snprintf("hello, 100%", 11, &buf, &written) == libc_snprintf_ok, "snprintf succeeds");
    expect(buf.length == 11 && !memcmp(buf.data, "hello, 100%", 11), "snprintf sizes by query and copies the text");
    expect(written == 11, "snprintf reports the full length");
    libc_bytes_free(&buf);
    expect(libc_vsnprintf("through va_list", 15, &buf, &written) == libc_vsnprintf_ok, "vsnprintf succeeds");
    expect(buf.length == 15 && !memcmp(buf.data, "through va_list", 15), "vsnprintf goes through the forwarder");
    libc_bytes_free(&buf);
    expect(libc_snprintf("a\0b", 3, &buf, &written) == LIBC_ARGUMENT_ERROR, "interior NUL is rejected");
}

static void
numbers(void)
{
    size_t end = 0;
    long value = 0;
    int detail = 0;
    expect(libc_strtol("123x", 4, 10, &end, &value, &detail) == libc_strtol_ok, "strtol parses");
    expect(value == 123 && end == 3, "strtol reports the value and where it stopped");
    expect(libc_strtol("99999999999999999999999", 23, 10, &end, &value, &detail) == libc_strtol_error,
           "strtol overflow is an error");
    expect(detail == ERANGE, "overflow reports ERANGE");
    errno = ERANGE;
    expect(libc_strtol("7", 1, 10, &end, &value, &detail) == libc_strtol_ok && value == 7,
           "a stale errno is reset before the call");
}

static void
files(void)
{
    int fd = -1;
    int detail = 0;
    expect(libc_open("/dev/null", 9, O_RDONLY, NULL, 0, &fd, &detail) == libc_open_ok && fd >= 0,
           "open without a mode argument");
    close(fd);
    unsigned int mode = 0600;
    expect(libc_open("/nonexistent/writ", 17, O_RDONLY | O_CREAT, &mode, 1, &fd, &detail) == libc_open_error,
           "open with a mode argument fails in a missing directory");
    expect(detail == ENOENT, "open reports ENOENT");
    libc_str_t args[] = {{"one", 3}, {"two", 3}};
    expect(libc_execl("/nonexistent/writ", 17, "writ", 4, args, 2, &detail) == libc_execl_error,
           "execl of a missing program fails");
    expect(detail == ENOENT, "execl reports ENOENT");
    libc_str_t many[9] = {{"a", 1}, {"a", 1}, {"a", 1}, {"a", 1}, {"a", 1}, {"a", 1}, {"a", 1}, {"a", 1}, {"a", 1}};
    expect(libc_execl("/nonexistent/writ", 17, "writ", 4, many, 9, &detail) == LIBC_ARGUMENT_ERROR,
           "more arguments than the widest call shim is an argument error");
}

int
main(void)
{
    formats();
    numbers();
    files();
    if (!failures) {
        printf("libc ok\n");
    }
    return failures != 0;
}
