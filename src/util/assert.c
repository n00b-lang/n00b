#include "n00b.h"
#include "conduit/print.h"
#include "core/exit.h"
#include "core/string.h"
#include "util/assert.h"

[[noreturn]] void
_n00b_assert_failed(const char *expr,
                    const char *func,
                    const char *file,
                    int         line)
{
    n00b_eprintf("ASSERTION FAILED: «#»\nFunction: «#»\nLocation: «#»:«#»",
                 n00b_string_from_cstr(expr),
                 n00b_string_from_cstr(func),
                 n00b_string_from_cstr(file),
                 (int64_t)line);
    n00b_abort();
}

[[noreturn]] void
_n00b_require_failed(const char *cond,
                     const char *msg,
                     const char *func,
                     const char *file,
                     int         line)
{
    n00b_eprintf("REQUIREMENT FAILED: «#»\nMessage: «#»\nFunction: «#»\nLocation: «#»:«#»",
                 n00b_string_from_cstr(cond),
                 n00b_string_from_cstr(msg ? msg : ""),
                 n00b_string_from_cstr(func),
                 n00b_string_from_cstr(file),
                 (int64_t)line);
    n00b_abort();
}
