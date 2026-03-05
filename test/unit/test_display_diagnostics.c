#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "n00b.h"
#include "core/runtime.h"
#include "internal/display/diagnostics.h"

static void
read_file(FILE *fp, char *buf, size_t buf_sz)
{
    assert(fp);
    assert(buf_sz > 0);

    fflush(fp);
    rewind(fp);

    size_t n = fread(buf, 1, buf_sz - 1, fp);
    buf[n] = '\0';
}

static void
test_diagnostics_gating(void)
{
    unsetenv("N00B_DISPLAY_DIAG");
    unsetenv("N00B_DISPLAY_DIAG_LEVEL");

    n00b_display_diag_shutdown();
    assert(!n00b_display_diag_would_log(N00B_DISPLAY_DIAG_ERROR));

    FILE *tmp = tmpfile();
    assert(tmp != nullptr);

    n00b_display_diag_set_stream(tmp);
    n00b_display_diag_set_level(N00B_DISPLAY_DIAG_OFF);
    assert(!n00b_display_diag_would_log(N00B_DISPLAY_DIAG_ERROR));
    n00b_display_diag_log(N00B_DISPLAY_DIAG_ERROR,
                           "test",
                           "should-not-appear");

    char buf[512];
    read_file(tmp, buf, sizeof(buf));
    assert(strlen(buf) == 0);

    n00b_display_diag_set_level(N00B_DISPLAY_DIAG_INFO);
    assert(n00b_display_diag_would_log(N00B_DISPLAY_DIAG_ERROR));
    assert(n00b_display_diag_would_log(N00B_DISPLAY_DIAG_INFO));
    assert(!n00b_display_diag_would_log(N00B_DISPLAY_DIAG_TRACE));

    n00b_display_diag_log(N00B_DISPLAY_DIAG_INFO,
                           "test",
                           "hello-info");
    n00b_display_diag_log(N00B_DISPLAY_DIAG_TRACE,
                           "test",
                           "hello-trace");

    read_file(tmp, buf, sizeof(buf));
    assert(strstr(buf, "hello-info") != nullptr);
    assert(strstr(buf, "hello-trace") == nullptr);

    fclose(tmp);
    n00b_display_diag_shutdown();

    printf("  [PASS] diagnostics gating and sink policy\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running display diagnostics tests...\n");
    test_diagnostics_gating();

    printf("Display diagnostics tests passed.\n");
    n00b_shutdown();
    return 0;
}
