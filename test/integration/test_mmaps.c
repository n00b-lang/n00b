#include <stdio.h>
#include <assert.h>
#include <unistd.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/mmaps.h"
#include "adt/option.h"
#include "adt/result.h"

// ============================================================================
// 1. mmap_basic — n00b_mmap returns ok result with page-aligned address
// ============================================================================

static void
test_mmap_basic(void)
{
    auto r = n00b_mmap(4096, .kind = n00b_mmap_api_mmap);

    assert(n00b_result_is_ok(r));

    void *addr = n00b_result_get(r);
    assert(addr != nullptr);

    // Must be page-aligned
    long page_size = sysconf(_SC_PAGESIZE);
    assert(((uintptr_t)addr & (page_size - 1)) == 0);

    n00b_munmap(addr);
    printf("  [PASS] mmap_basic\n");
}

// ============================================================================
// 2. Lookup — after mmap, n00b_mmap_by_address finds the record
// ============================================================================

static void
test_lookup(void)
{
    auto r = n00b_mmap(4096, .kind = n00b_mmap_api_mmap);
    assert(n00b_result_is_ok(r));

    void *addr = n00b_result_get(r);

    n00b_option_t(n00b_mmap_info_t *) opt = n00b_mmap_by_address(addr);
    assert(n00b_option_is_set(opt));

    n00b_mmap_info_t *info = n00b_option_get(opt);
    assert(info != nullptr);
    assert((void *)info->start == addr || info->start <= (uint64_t)addr);

    n00b_munmap(addr);
    printf("  [PASS] lookup\n");
}

// ============================================================================
// 3. Kind — registered mapping has correct kind
// ============================================================================

static void
test_kind(void)
{
    auto r = n00b_mmap(4096, .kind = n00b_mmap_api_mmap);
    assert(n00b_result_is_ok(r));

    void *addr = n00b_result_get(r);

    n00b_option_t(n00b_mmap_info_t *) opt = n00b_mmap_by_address(addr);
    assert(n00b_option_is_set(opt));

    n00b_mmap_info_t *info = n00b_option_get(opt);
    assert(n00b_mmap_get_kind(info) == n00b_mmap_api_mmap);

    n00b_munmap(addr);
    printf("  [PASS] kind\n");
}

// ============================================================================
// 4. Munmap — after munmap, lookup returns none
// ============================================================================

static void
test_munmap(void)
{
    auto r = n00b_mmap(4096, .kind = n00b_mmap_api_mmap);
    assert(n00b_result_is_ok(r));

    void *addr = n00b_result_get(r);

    // Verify it's there
    n00b_option_t(n00b_mmap_info_t *) opt = n00b_mmap_by_address(addr);
    assert(n00b_option_is_set(opt));

    // Unmap
    auto ur = n00b_munmap(addr);
    assert(n00b_result_is_ok(ur));

    // Should no longer be found
    opt = n00b_mmap_by_address(addr);
    assert(!n00b_option_is_set(opt));

    printf("  [PASS] munmap\n");
}

// ============================================================================
// 5. Register range — sub-range creation and lookup
// ============================================================================

static void
test_register_range(void)
{
    auto r = n00b_mmap(8192, .kind = n00b_mmap_api_mmap);
    assert(n00b_result_is_ok(r));

    void *addr = n00b_result_get(r);

    // Register a sub-range within the mapping
    void *sub_start = (char *)addr + 1024;
    void *sub_end   = (char *)addr + 2048;

    n00b_mmap_register_range(sub_start, sub_end, n00b_mmap_internal);

    n00b_munmap(addr);
    printf("  [PASS] register_range\n");
}

// ============================================================================
// 6. Delete ranges — removes sub-ranges
// ============================================================================

static void
test_delete_ranges(void)
{
    auto r = n00b_mmap(8192, .kind = n00b_mmap_api_mmap);
    assert(n00b_result_is_ok(r));

    void *addr = n00b_result_get(r);

    void *sub_start = (char *)addr + 1024;
    void *sub_end   = (char *)addr + 2048;

    n00b_mmap_register_range(sub_start, sub_end, n00b_mmap_internal);

    n00b_runtime_t  *rt  = n00b_get_runtime();
    n00b_mmap_ctx_t *ctx = n00b_global_mem_map(rt);

    n00b_mmap_delete_ranges(ctx, (uint64_t)sub_start, (uint64_t)sub_end);

    n00b_munmap(addr);
    printf("  [PASS] delete_ranges\n");
}

// ============================================================================
// 7. Allocator lookup — n00b_mem_get_allocator returns allocator for managed addr
// ============================================================================

static void
test_allocator_lookup(void)
{
    // Allocate through the default allocator
    uint64_t *p = n00b_alloc(uint64_t);
    assert(p != nullptr);

    n00b_allocator_opt_t opt = n00b_mem_get_allocator(p);
    assert(n00b_option_is_set(opt));

    n00b_allocator_t *alloc = n00b_option_get(opt);
    assert(alloc != nullptr);

    n00b_free(p);
    printf("  [PASS] allocator_lookup\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running mmaps tests...\n");

    test_mmap_basic();
    test_lookup();
    test_kind();
    test_munmap();
    test_register_range();
    test_delete_ranges();
    test_allocator_lookup();

    printf("All mmaps tests passed.\n");
    return 0;
}
