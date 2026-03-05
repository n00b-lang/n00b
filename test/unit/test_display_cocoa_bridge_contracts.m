#include <assert.h>
#include <stdio.h>

#include "internal/display/cocoa_bridge_contracts.h"

static void
print_layout(const char *label, const n00b_cocoa_bridge_layout_t *layout)
{
    printf("%s abi=%u text_style=%u rcell=%u event=%u\n",
           label,
           (unsigned)layout->abi_version,
           (unsigned)layout->text_style_size,
           (unsigned)layout->rcell_size,
           (unsigned)layout->event_size);
}

static void
test_bridge_layout_contracts(void)
{
    n00b_cocoa_bridge_layout_t canonical = n00b_cocoa_bridge_layout_canonical();
    n00b_cocoa_bridge_layout_t bridge = n00b_cocoa_bridge_layout_bridge();
    const char *mismatch = nullptr;

    if (!n00b_cocoa_bridge_layout_match(&canonical, &bridge, &mismatch)) {
        print_layout("canonical", &canonical);
        print_layout("bridge", &bridge);
        fprintf(stderr,
                "bridge layout mismatch at field '%s'\n",
                mismatch ? mismatch : "unknown");
        assert(false);
    }

    printf("  [PASS] cocoa bridge layout contracts\n");
}

int
main(void)
{
    printf("Running display cocoa bridge-contract tests...\n");
    test_bridge_layout_contracts();
    printf("Display cocoa bridge-contract tests passed.\n");
    return 0;
}
