/*
 * naudit CLI entry.
 *
 * Thin `main()` shell: bootstrap the libn00b runtime via
 * `n00b_init_simple`, convert `char *argv[]` to
 * `n00b_string_t *audit_argv[]` (per § 2.2 — the `main` signature
 * is the standard C exception), and hand off to the library-shaped
 * `n00b_audit_run_cli` (declared in `include/naudit/naudit.h`).
 *
 * The bulk of the CLI logic lives in `src/naudit/cli.c` so the same
 * code path is reachable from the regression test
 * (`test/unit/test_naudit_cli.c`) without spawning a subprocess.
 *
 * Exit-code contract:
 *   - 0  no violations found.
 *   - 1  at least one violation.
 *   - 2  internal error (a human-readable diagnostic has already
 *        been emitted to stderr by the library entry).
 *
 * The CLI binary's name is `naudit` (set by the `executable(...)`
 * target in the root `meson.build`); the source file is `naudit.c`
 * per the project's snake_case naming convention (NCC.md "Naming
 * and structure").
 */

#include "n00b.h"
#include "core/runtime.h"
#include "core/alloc.h"
#include "core/string.h"
#include "conduit/print.h"
#include "adt/result.h"

#include "naudit/naudit.h"
#include "naudit/errors.h"

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);

    if (!n00b_audit_module_init()) {
        return 2;
    }

    if (argc <= 0 || !argv) {
        return 2;
    }

    /*
     * Convert each argv entry to n00b_string_t * per § 2.2. The
     * argv array itself is replaced by a same-shape array of
     * n00b_string_t pointers; argc stays as-is so the library entry
     * sees the conventional shape.
     */
    n00b_string_t **audit_argv = n00b_alloc_array(n00b_string_t *,
                                                  (size_t)(argc + 1));
    for (int i = 0; i < argc; i++) {
        audit_argv[i] = n00b_string_from_cstr(argv[i]);
    }
    audit_argv[argc] = nullptr;

    n00b_result_t(int) r = n00b_audit_run_cli(argc, audit_argv);
    if (n00b_result_is_err(r)) {
        int err = n00b_result_get_err(r);
        n00b_eprintf("n00b-audit: «#»", n00b_audit_err_str(err));
        return 2;
    }
    return n00b_result_get(r);
}
