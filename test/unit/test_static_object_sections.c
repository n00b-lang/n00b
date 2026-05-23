#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/arena.h"
#include "core/gc.h"
#include "core/gc_map.h"
#include "core/mmaps.h"
#include "core/runtime.h"
#include "core/stw.h"
#include "core/static_objects.h"

#define ARENA_OPTS(a) &(n00b_alloc_opts_t){.allocator = (n00b_allocator_t *)(a)}

#define STATIC_SECTION_MUT_TINFO   UINT64_C(0x5354435345430001)
#define STATIC_SECTION_RO_TINFO    UINT64_C(0x5354435345430002)
#define STATIC_SECTION_EXTRA_TINFO UINT64_C(0x5354434558540001)
#define STATIC_SECTION_NONE_TINFO  UINT64_C(0x5354434743000001)
#define STATIC_SECTION_ALL_TINFO   UINT64_C(0x5354434743000002)
#define STATIC_SECTION_CB_TINFO    UINT64_C(0x5354434743000003)
#define STATIC_SECTION_NROOT_TINFO UINT64_C(0x5354434743000004)
#define STATIC_SECTION_NLEAF_TINFO UINT64_C(0x5354434743000005)
#define STATIC_SECTION_SPARSE_TINFO UINT64_C(0x5354434743000006)

#define STATIC_SECTION_MUT_ID   UINT64_C(0x535443530001)
#define STATIC_SECTION_RO_ID    UINT64_C(0x535443530002)
#define STATIC_SECTION_EXTRA_ID UINT64_C(0x535443450001)
#define STATIC_SECTION_NONE_ID  UINT64_C(0x535443470001)
#define STATIC_SECTION_ALL_ID   UINT64_C(0x535443470002)
#define STATIC_SECTION_CB_ID    UINT64_C(0x535443470003)
#define STATIC_SECTION_NROOT_ID UINT64_C(0x535443470004)
#define STATIC_SECTION_NLEAF_ID UINT64_C(0x535443470005)
#define STATIC_SECTION_SPARSE_ID UINT64_C(0x535443470006)

typedef struct {
    uint64_t value;
    void    *next;
} section_target_t;

typedef struct {
    void    *leaf;
    uint64_t tag;
} section_nested_root_t;

typedef struct {
    void    *child;
    uint64_t tag;
} section_nested_leaf_t;

typedef struct {
    void    *left;
    uint64_t scalar;
    void    *right;
    uint64_t tag;
} section_sparse_item_t;

static int       section_mutable_value = 17;
static const int section_readonly_value = 23;
static uint64_t  section_none_words[2];
static void     *section_all_ptrs[2];
static void     *section_callback_words[4];
static section_nested_root_t section_nested_root;
static section_nested_leaf_t section_nested_leaf;
static section_sparse_item_t section_sparse_items[2];

static const uint64_t section_sparse_offsets[] = {0, 2};
static n00b_gc_struct_layout_t section_sparse_layout = {
    .stride       = sizeof(section_sparse_item_t) / sizeof(void *),
    .count        = 2,
    .offset_count = 2,
    .offsets      = section_sparse_offsets,
};

_Static_assert(sizeof(section_sparse_item_t) % sizeof(void *) == 0,
               "sparse scan test item must be word-sized");

static void
section_mark_word_2_cb(n00b_gc_map_t *m, void *user)
{
    (void)user;
    n00b_gc_map_mark(m, 2);
}

N00B_STATIC_OBJECT_DESCRIPTOR_FOR(n00b_test_static_section_mut_desc,
                                  section_mutable_value,
                                  STATIC_SECTION_MUT_TINFO,
                                  N00B_STATIC_OBJECT_F_MUTABLE,
                                  N00B_GC_SCAN_KIND_NONE,
                                  nullptr,
                                  nullptr,
                                  STATIC_SECTION_MUT_ID);

N00B_STATIC_OBJECT_DESCRIPTOR_FOR(n00b_test_static_section_ro_desc,
                                  section_readonly_value,
                                  STATIC_SECTION_RO_TINFO,
                                  N00B_STATIC_OBJECT_F_READONLY,
                                  N00B_GC_SCAN_KIND_NONE,
                                  nullptr,
                                  nullptr,
                                  STATIC_SECTION_RO_ID);

N00B_STATIC_OBJECT_DESCRIPTOR_FOR(n00b_test_static_section_none_desc,
                                  section_none_words,
                                  STATIC_SECTION_NONE_TINFO,
                                  N00B_STATIC_OBJECT_F_MUTABLE,
                                  N00B_GC_SCAN_KIND_NONE,
                                  nullptr,
                                  nullptr,
                                  STATIC_SECTION_NONE_ID);

N00B_STATIC_OBJECT_DESCRIPTOR_FOR(n00b_test_static_section_all_desc,
                                  section_all_ptrs,
                                  STATIC_SECTION_ALL_TINFO,
                                  N00B_STATIC_OBJECT_F_MUTABLE,
                                  N00B_GC_SCAN_KIND_ALL,
                                  nullptr,
                                  nullptr,
                                  STATIC_SECTION_ALL_ID);

N00B_STATIC_OBJECT_DESCRIPTOR_FOR(n00b_test_static_section_cb_desc,
                                  section_callback_words,
                                  STATIC_SECTION_CB_TINFO,
                                  N00B_STATIC_OBJECT_F_MUTABLE,
                                  N00B_GC_SCAN_KIND_CALLBACK,
                                  section_mark_word_2_cb,
                                  nullptr,
                                  STATIC_SECTION_CB_ID);

N00B_STATIC_OBJECT_DESCRIPTOR_FOR(n00b_test_static_section_nested_root_desc,
                                  section_nested_root,
                                  STATIC_SECTION_NROOT_TINFO,
                                  N00B_STATIC_OBJECT_F_MUTABLE,
                                  N00B_GC_SCAN_KIND_ALL,
                                  nullptr,
                                  nullptr,
                                  STATIC_SECTION_NROOT_ID);

N00B_STATIC_OBJECT_DESCRIPTOR_FOR(n00b_test_static_section_nested_leaf_desc,
                                  section_nested_leaf,
                                  STATIC_SECTION_NLEAF_TINFO,
                                  N00B_STATIC_OBJECT_F_MUTABLE,
                                  N00B_GC_SCAN_KIND_ALL,
                                  nullptr,
                                  nullptr,
                                  STATIC_SECTION_NLEAF_ID);

N00B_STATIC_OBJECT_DESCRIPTOR_FOR(n00b_test_static_section_sparse_desc,
                                  section_sparse_items,
                                  STATIC_SECTION_SPARSE_TINFO,
                                  N00B_STATIC_OBJECT_F_MUTABLE,
                                  N00B_GC_SCAN_KIND_CALLBACK,
                                  n00b_gc_scan_cb_struct_layout,
                                  &section_sparse_layout,
                                  STATIC_SECTION_SPARSE_ID);

extern void *n00b_test_static_section_extra_addr(void);

typedef struct {
    bool found_mutable;
    bool found_readonly;
    bool found_extra;
} seen_descs_t;

static void
collect_desc(const n00b_static_object_desc_t *desc, void *user)
{
    seen_descs_t *seen = user;

    switch (desc->object_id) {
    case STATIC_SECTION_MUT_ID:
        assert(desc->start == &section_mutable_value);
        assert(desc->len == sizeof(section_mutable_value));
        assert(desc->tinfo == STATIC_SECTION_MUT_TINFO);
        assert(desc->flags & N00B_STATIC_OBJECT_F_MUTABLE);
        assert(desc->scan_kind == N00B_GC_SCAN_KIND_NONE);
        seen->found_mutable = true;
        break;
    case STATIC_SECTION_RO_ID:
        assert(desc->start == &section_readonly_value);
        assert(desc->len == sizeof(section_readonly_value));
        assert(desc->tinfo == STATIC_SECTION_RO_TINFO);
        assert(desc->flags & N00B_STATIC_OBJECT_F_READONLY);
        assert(desc->scan_kind == N00B_GC_SCAN_KIND_NONE);
        seen->found_readonly = true;
        break;
    case STATIC_SECTION_EXTRA_ID:
        assert(desc->start == n00b_test_static_section_extra_addr());
        assert(desc->len == sizeof(int));
        assert(desc->tinfo == STATIC_SECTION_EXTRA_TINFO);
        assert(desc->flags & N00B_STATIC_OBJECT_F_MUTABLE);
        assert(desc->scan_kind == N00B_GC_SCAN_KIND_NONE);
        seen->found_extra = true;
        break;
    default:
        break;
    }
}

static n00b_alloc_range_t *
lookup_range(void *addr)
{
    auto opt = n00b_mmap_range_by_address(addr);
    assert(n00b_option_is_set(opt));
    return n00b_option_get(opt);
}

static void
test_section_enumeration(void)
{
    seen_descs_t seen = {0};
    size_t count = n00b_static_objects_enumerate(collect_desc, &seen);

    assert(count >= 3);
    assert(seen.found_mutable);
    assert(seen.found_readonly);
    assert(seen.found_extra);

    printf("  [PASS] static_object_section_enumeration\n");
}

static void
test_auto_registration(void)
{
    n00b_alloc_range_t *mut = lookup_range(&section_mutable_value);
    assert(mut->kind == n00b_mmap_static);
    assert(mut->start == &section_mutable_value);
    assert(mut->len == sizeof(section_mutable_value));
    assert(mut->tinfo == STATIC_SECTION_MUT_TINFO);
    assert(mut->object_id == STATIC_SECTION_MUT_ID);
    assert(mut->flags & N00B_STATIC_OBJECT_F_MUTABLE);
    assert(mut->scan_kind == N00B_GC_SCAN_KIND_NONE);

    n00b_alloc_range_t *ro = lookup_range((void *)&section_readonly_value);
    assert(ro->kind == n00b_mmap_static);
    assert(ro->start == &section_readonly_value);
    assert(ro->len == sizeof(section_readonly_value));
    assert(ro->tinfo == STATIC_SECTION_RO_TINFO);
    assert(ro->object_id == STATIC_SECTION_RO_ID);
    assert(ro->flags & N00B_STATIC_OBJECT_F_READONLY);
    assert(ro->scan_kind == N00B_GC_SCAN_KIND_NONE);

    void *extra_addr = n00b_test_static_section_extra_addr();
    n00b_alloc_range_t *extra = lookup_range(extra_addr);
    assert(extra->kind == n00b_mmap_static);
    assert(extra->start == extra_addr);
    assert(extra->len == sizeof(int));
    assert(extra->tinfo == STATIC_SECTION_EXTRA_TINFO);
    assert(extra->object_id == STATIC_SECTION_EXTRA_ID);
    assert(extra->flags & N00B_STATIC_OBJECT_F_MUTABLE);
    assert(extra->scan_kind == N00B_GC_SCAN_KIND_NONE);

    printf("  [PASS] static_object_auto_registration\n");
}

static uint64_t *
save_words(n00b_arena_t *arena, uint64_t n, uint64_t *src)
{
    uint64_t *buf = n00b_alloc_array_with_opts(
        uint64_t,
        n,
        &(n00b_alloc_opts_t){
            .allocator = (n00b_allocator_t *)arena,
            .scan_kind = N00B_GC_SCAN_KIND_NONE,
        });

    for (uint64_t i = 0; i < n; i++) {
        buf[i] = src[i];
    }

    return buf;
}

static uint64_t *
save_ptr_words(n00b_arena_t *arena, uint64_t n, void **src)
{
    uint64_t *buf = n00b_alloc_array_with_opts(
        uint64_t,
        n,
        &(n00b_alloc_opts_t){
            .allocator = (n00b_allocator_t *)arena,
            .scan_kind = N00B_GC_SCAN_KIND_NONE,
        });

    for (uint64_t i = 0; i < n; i++) {
        buf[i] = (uint64_t)(uintptr_t)src[i];
    }

    return buf;
}

static void
test_section_range_metadata(void)
{
    n00b_alloc_range_t *none = lookup_range(section_none_words);
    assert(none->kind == n00b_mmap_static);
    assert(none->start == section_none_words);
    assert(none->len == sizeof(section_none_words));
    assert(none->tinfo == STATIC_SECTION_NONE_TINFO);
    assert(none->object_id == STATIC_SECTION_NONE_ID);
    assert(none->scan_kind == N00B_GC_SCAN_KIND_NONE);
    assert(none->flags & N00B_STATIC_OBJECT_F_MUTABLE);

    n00b_alloc_range_t *all = lookup_range(section_all_ptrs);
    assert(all->kind == n00b_mmap_static);
    assert(all->start == section_all_ptrs);
    assert(all->len == sizeof(section_all_ptrs));
    assert(all->tinfo == STATIC_SECTION_ALL_TINFO);
    assert(all->object_id == STATIC_SECTION_ALL_ID);
    assert(all->scan_kind == N00B_GC_SCAN_KIND_ALL);
    assert(all->flags & N00B_STATIC_OBJECT_F_MUTABLE);

    n00b_alloc_range_t *cb = lookup_range(section_callback_words);
    assert(cb->kind == n00b_mmap_static);
    assert(cb->start == section_callback_words);
    assert(cb->len == sizeof(section_callback_words));
    assert(cb->tinfo == STATIC_SECTION_CB_TINFO);
    assert(cb->object_id == STATIC_SECTION_CB_ID);
    assert(cb->scan_kind == N00B_GC_SCAN_KIND_CALLBACK);
    assert(cb->scan_cb == section_mark_word_2_cb);
    assert(cb->flags & N00B_STATIC_OBJECT_F_MUTABLE);

    n00b_alloc_range_t *root = lookup_range(&section_nested_root);
    assert(root->kind == n00b_mmap_static);
    assert(root->start == &section_nested_root);
    assert(root->len == sizeof(section_nested_root));
    assert(root->tinfo == STATIC_SECTION_NROOT_TINFO);
    assert(root->object_id == STATIC_SECTION_NROOT_ID);
    assert(root->scan_kind == N00B_GC_SCAN_KIND_ALL);

    n00b_alloc_range_t *leaf = lookup_range(&section_nested_leaf);
    assert(leaf->kind == n00b_mmap_static);
    assert(leaf->start == &section_nested_leaf);
    assert(leaf->len == sizeof(section_nested_leaf));
    assert(leaf->tinfo == STATIC_SECTION_NLEAF_TINFO);
    assert(leaf->object_id == STATIC_SECTION_NLEAF_ID);
    assert(leaf->scan_kind == N00B_GC_SCAN_KIND_ALL);

    n00b_alloc_range_t *sparse = lookup_range(section_sparse_items);
    assert(sparse->kind == n00b_mmap_static);
    assert(sparse->start == section_sparse_items);
    assert(sparse->len == sizeof(section_sparse_items));
    assert(sparse->tinfo == STATIC_SECTION_SPARSE_TINFO);
    assert(sparse->object_id == STATIC_SECTION_SPARSE_ID);
    assert(sparse->scan_kind == N00B_GC_SCAN_KIND_CALLBACK);
    assert(sparse->scan_cb == n00b_gc_scan_cb_struct_layout);
    assert(sparse->scan_user == &section_sparse_layout);

    printf("  [PASS] static_object_range_metadata\n");
}

static __attribute__((noinline)) void
test_section_gc_scan_policies_inner(n00b_arena_t *arena)
{
    section_target_t *none_target = n00b_alloc_with_opts(section_target_t,
                                                         ARENA_OPTS(arena));
    none_target->value            = UINT64_C(0x5354415449430001);
    section_none_words[0]         = (uint64_t)(uintptr_t)none_target;
    section_none_words[1]         = UINT64_C(0xBADC0FFEE0DDF00D);
    uint64_t *none_saved          = save_words(arena, 2, section_none_words);

    section_target_t *all0 = n00b_alloc_with_opts(section_target_t,
                                                  ARENA_OPTS(arena));
    section_target_t *all1 = n00b_alloc_with_opts(section_target_t,
                                                  ARENA_OPTS(arena));
    all0->value            = UINT64_C(0x5354415449430002);
    all1->value            = UINT64_C(0x5354415449430003);
    section_all_ptrs[0]    = all0;
    section_all_ptrs[1]    = all1;

    section_target_t *cb_target = n00b_alloc_with_opts(section_target_t,
                                                       ARENA_OPTS(arena));
    section_target_t *cb_decoy  = n00b_alloc_with_opts(section_target_t,
                                                       ARENA_OPTS(arena));
    cb_target->value            = UINT64_C(0x5354415449430004);
    cb_decoy->value             = UINT64_C(0x5354415449430005);
    for (int i = 0; i < 4; i++) {
        section_callback_words[i] = cb_decoy;
    }
    section_callback_words[2] = cb_target;
    uint64_t *cb_saved = save_ptr_words(arena, 4, section_callback_words);

    section_target_t *nested_target = n00b_alloc_with_opts(section_target_t,
                                                           ARENA_OPTS(arena));
    nested_target->value            = UINT64_C(0x5354415449430006);
    section_nested_root             = (section_nested_root_t){
        .leaf = &section_nested_leaf,
        .tag  = UINT64_C(0xABCDEF0000000001),
    };
    section_nested_leaf = (section_nested_leaf_t){
        .child = nested_target,
        .tag   = UINT64_C(0xABCDEF0000000002),
    };

    section_target_t *sparse_left0 = n00b_alloc_with_opts(section_target_t,
                                                          ARENA_OPTS(arena));
    section_target_t *sparse_right0 = n00b_alloc_with_opts(section_target_t,
                                                           ARENA_OPTS(arena));
    section_target_t *sparse_left1 = n00b_alloc_with_opts(section_target_t,
                                                          ARENA_OPTS(arena));
    section_target_t *sparse_right1 = n00b_alloc_with_opts(section_target_t,
                                                           ARENA_OPTS(arena));
    section_target_t *sparse_decoy = n00b_alloc_with_opts(section_target_t,
                                                          ARENA_OPTS(arena));
    sparse_left0->value  = UINT64_C(0x5354415449430007);
    sparse_right0->value = UINT64_C(0x5354415449430008);
    sparse_left1->value  = UINT64_C(0x5354415449430009);
    sparse_right1->value = UINT64_C(0x535441544943000a);
    sparse_decoy->value  = UINT64_C(0x535441544943000b);

    section_sparse_items[0] = (section_sparse_item_t){
        .left   = sparse_left0,
        .scalar = (uint64_t)(uintptr_t)sparse_decoy,
        .right  = sparse_right0,
        .tag    = UINT64_C(0xABCDEF0000000003),
    };
    section_sparse_items[1] = (section_sparse_item_t){
        .left   = sparse_left1,
        .scalar = (uint64_t)(uintptr_t)sparse_decoy,
        .right  = sparse_right1,
        .tag    = UINT64_C(0xABCDEF0000000004),
    };
    uint64_t sparse_saved_scalar0_not = ~section_sparse_items[0].scalar;
    uint64_t sparse_saved_scalar1_not = ~section_sparse_items[1].scalar;

    void *none_root   = section_none_words;
    void *all_root    = section_all_ptrs;
    void *cb_root     = section_callback_words;
    void *nested_root = &section_nested_root;
    void *sparse_root = section_sparse_items;

    n00b_gc_register_root(none_root);
    n00b_gc_register_root(all_root);
    n00b_gc_register_root(cb_root);
    n00b_gc_register_root(nested_root);
    n00b_gc_register_root(sparse_root);

    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();

    n00b_gc_unregister_root(none_root);
    n00b_gc_unregister_root(all_root);
    n00b_gc_unregister_root(cb_root);
    n00b_gc_unregister_root(nested_root);
    n00b_gc_unregister_root(sparse_root);

    assert(section_none_words[0] == none_saved[0]);
    assert(section_none_words[0] != (uint64_t)(uintptr_t)none_target);
    assert(none_target->value == UINT64_C(0x5354415449430001));

    assert(section_all_ptrs[0] == all0);
    assert(section_all_ptrs[1] == all1);
    assert(all0->value == UINT64_C(0x5354415449430002));
    assert(all1->value == UINT64_C(0x5354415449430003));

    assert(section_callback_words[2] == cb_target);
    for (int i = 0; i < 4; i++) {
        if (i != 2) {
            assert((uint64_t)(uintptr_t)section_callback_words[i] == cb_saved[i]);
            assert(section_callback_words[i] != cb_decoy);
        }
    }
    assert(cb_target->value == UINT64_C(0x5354415449430004));
    assert(cb_decoy->value == UINT64_C(0x5354415449430005));

    assert(section_nested_root.leaf == &section_nested_leaf);
    assert(section_nested_leaf.child == nested_target);
    assert(nested_target->value == UINT64_C(0x5354415449430006));

    assert(section_sparse_items[0].left == sparse_left0);
    assert(section_sparse_items[0].right == sparse_right0);
    assert(section_sparse_items[1].left == sparse_left1);
    assert(section_sparse_items[1].right == sparse_right1);
    assert(section_sparse_items[0].scalar == ~sparse_saved_scalar0_not);
    assert(section_sparse_items[1].scalar == ~sparse_saved_scalar1_not);
    assert(section_sparse_items[0].scalar != (uint64_t)(uintptr_t)sparse_decoy);
    assert(section_sparse_items[1].scalar != (uint64_t)(uintptr_t)sparse_decoy);
    assert(sparse_left0->value == UINT64_C(0x5354415449430007));
    assert(sparse_right0->value == UINT64_C(0x5354415449430008));
    assert(sparse_left1->value == UINT64_C(0x5354415449430009));
    assert(sparse_right1->value == UINT64_C(0x535441544943000a));
    assert(sparse_decoy->value == UINT64_C(0x535441544943000b));
}

static void
test_section_gc_scan_policies(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 8192, .use_gc = true);
    test_section_gc_scan_policies_inner(arena);
    printf("  [PASS] static_object_gc_scan_policies\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running static object section tests...\n");

    test_section_enumeration();
    test_auto_registration();
    test_section_range_metadata();
    test_section_gc_scan_policies();

    printf("All static object section tests passed.\n");
    return 0;
}
