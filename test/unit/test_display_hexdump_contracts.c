#include <assert.h>
#include <stdio.h>

#include "n00b.h"
#include "core/runtime.h"
#include "display/hexdump.h"
#include "internal/display/hexdump_contracts.h"

static void
test_regions_for_partial_and_full_lines(void)
{
    n00b_hexdump_t *hd = n00b_hexdump_new(.width = 80);
    assert(hd != nullptr);

    n00b_hexdump_line_regions_t regions = {};
    n00b_hexdump_describe_line_regions(hd, 8, &regions);

    assert(regions.offset_start == 0);
    assert(regions.offset_end == hd->offset_cols);
    assert(regions.ascii_start == hd->ascii_start);
    assert(regions.ascii_end == hd->ascii_start + 8);

    n00b_hexdump_describe_line_regions(hd, hd->cpl + 16, &regions);
    assert(regions.ascii_end == hd->ascii_start + hd->cpl);

    n00b_hexdump_describe_line_regions(hd, 0, &regions);
    assert(regions.ascii_end == regions.ascii_start);

    n00b_hexdump_destroy(hd);
    printf("  [PASS] hexdump line region contracts\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running display hexdump contract tests...\n");

    test_regions_for_partial_and_full_lines();

    printf("Display hexdump contract tests passed.\n");
    n00b_shutdown();
    return 0;
}
