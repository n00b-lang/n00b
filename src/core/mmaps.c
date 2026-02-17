#define N00B_MEM_INTERNAL_API

#include "n00b.h"
#include "core/mmaps.h"
#include "core/alloc_mdata.h"
#include "core/alloc.h"
#include "core/atomic.h"
#include "core/futex.h"
#include "core/align.h"
#include "core/pool.h"
#include "core/thread.h"

// TODO: fix this
// #include "io/print.h"
#include <stdio.h>
#define n00b_fprintf fprintf

#include <sys/mman.h>
#include <unistd.h>

#define N00B_MMAPS_PTR_SEED 0xdeadbeefu

#if !defined(N00B_MMAPS_START_PAGES)
#define N00B_MMAPS_START_PAGES 16
#endif

static n00b_mmap_node_t *mmaps_insert_raw(n00b_mmap_ctx_t     *ctx,
                                          void                *startp,
                                          uint64_t             blen,
                                          n00b_mmap_rec_kind_t kind,
                                          uint64_t             binary_offset);

static inline bool
overlaps(n00b_mmap_node_t *ancestor, n00b_mmap_node_t *n)
{
    if (n->end <= ancestor->start) {
        return false;
    }

    if (n->start >= ancestor->end) {
        return false;
    }

    return true;
}

static inline uint32_t
treap_next_priority(n00b_mmap_ctx_t *ctx)
{
    uint32_t x = ctx->treap_seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    if (!x) {
        x = 0x9e3779b9u;
    }
    ctx->treap_seed = x;
    return x;
}

static inline void
recompute_bounds(n00b_mmap_node_t *n)
{
    n00b_mmap_node_t *l   = n00b_atomic_load(&n->left);
    n00b_mmap_node_t *r   = n00b_atomic_load(&n->right);
    uint64_t          min = n->start;
    uint64_t          max = n->end;

    if (l) {
        if (l->subtree_min < min) {
            min = l->subtree_min;
        }
        if (l->subtree_max > max) {
            max = l->subtree_max;
        }
    }

    if (r) {
        if (r->subtree_min < min) {
            min = r->subtree_min;
        }
        if (r->subtree_max > max) {
            max = r->subtree_max;
        }
    }

    n->subtree_min = min;
    n->subtree_max = max;
}

static inline void
propagate_bounds_up(n00b_mmap_node_t *n)
{
    while (n) {
        recompute_bounds(n);
        n = n00b_atomic_load(&n->parent);
    }
}

static inline n00b_mmap_node_t *
find_overlap(n00b_mmap_node_t *root, n00b_mmap_node_t *needle)
{
    while (root) {
        if (overlaps(root, needle)) {
            return root;
        }

        n00b_mmap_node_t *l = n00b_atomic_load(&root->left);
        if (l && l->subtree_max > needle->start) {
            root = l;
            continue;
        }

        root = n00b_atomic_load(&root->right);
    }

    return NULL;
}

static n00b_mmap_node_t *
rotate_left(n00b_mmap_node_t *x)
{
    n00b_mmap_node_t *y = n00b_atomic_load(&x->right);
    n00b_mmap_node_t *p = n00b_atomic_load(&x->parent);
    n00b_mmap_node_t *b = y ? n00b_atomic_load(&y->left) : NULL;

    assert(y);

    n00b_atomic_store(&x->right, b);
    if (b) {
        n00b_atomic_store(&b->parent, x);
    }

    n00b_atomic_store(&y->left, x);
    n00b_atomic_store(&x->parent, y);

    if (p) {
        if (n00b_atomic_load(&p->left) == x) {
            n00b_atomic_store(&p->left, y);
        }
        else {
            n00b_atomic_store(&p->right, y);
        }
    }
    n00b_atomic_store(&y->parent, p);

    recompute_bounds(x);
    recompute_bounds(y);
    return y;
}

static n00b_mmap_node_t *
rotate_right(n00b_mmap_node_t *y)
{
    n00b_mmap_node_t *x = n00b_atomic_load(&y->left);
    n00b_mmap_node_t *p = n00b_atomic_load(&y->parent);
    n00b_mmap_node_t *b = x ? n00b_atomic_load(&x->right) : NULL;

    assert(x);

    n00b_atomic_store(&y->left, b);
    if (b) {
        n00b_atomic_store(&b->parent, y);
    }

    n00b_atomic_store(&x->right, y);
    n00b_atomic_store(&y->parent, x);

    if (p) {
        if (n00b_atomic_load(&p->left) == y) {
            n00b_atomic_store(&p->left, x);
        }
        else {
            n00b_atomic_store(&p->right, x);
        }
    }
    n00b_atomic_store(&x->parent, p);

    recompute_bounds(y);
    recompute_bounds(x);
    return x;
}

void
n00b_print_mmap_tree(n00b_mmap_node_t *node, int level)
{
    char *name;

    if (!node) {
        return;
    }

    switch (node->kind) {
    case n00b_mmap_root:
        name = "Root    ";
        break;
    case n00b_mmap_static:
        name = "Static  ";
        break;
    case n00b_mmap_arena:
        name = "Arena   ";
        break;
    case n00b_mmap_managed_segment:
        name = "Managed ";
        break;
    case n00b_mmap_sys_segment:
        name = "System  ";
        break;
    case n00b_mmap_zero_page:
        name = "No-map  ";
        break;
    case n00b_mmap_unmanaged:
        name = "mmap()  ";
        break;
    case n00b_mmap_stack:
        name = "\e[31;1;4mStack   \e[0m";
        break;
    case n00b_mmap_internal:
        name = "Internal";
        break;
    case n00b_mmap_pool:
        name = "Pool    ";
        break;
    case n00b_mmap_api_mmap:
        name = "Direct  ";
        break;
    default:
        abort();
    }

    for (int i = 0; i < level; i++) {
        fputc(' ', stderr);
    }

    n00b_fprintf(stderr,
                 "%s %p (span: %p-%p) ",
                 name,
                 node,
                 (void *)node->subtree_min,
                 (void *)node->subtree_max);

    if (node->kind != n00b_mmap_root) {
        n00b_fprintf(stderr, "%p-%p %s", (void *)node->start, (void *)node->end, node->file);
    }

    fputc('\n', stdout);
    n00b_print_mmap_tree(node->left, level + 1);
    n00b_print_mmap_tree(node->right, level + 1);
}

#if 0
static void
sanity_check(n00b_mmap_node_t *n)
{
    if (!n) {
        return;
    }

    n00b_mmap_node_t *l = n00b_atomic_load(&n->left);
    n00b_mmap_node_t *r = n00b_atomic_load(&n->right);

    if (n->kind == n00b_mmap_root) {
        sanity_check(l);
        sanity_check(r);
        return;
    }

    assert(n->start < n->end);

    if (!l) {
        assert(n->start == n->subtree_min);
    }
    else {
        assert(l->parent == n);
        assert(n->subtree_min == l->subtree_min);
        assert(l->subtree_max <= n->start);
        assert(n->start > n->subtree_min);
        sanity_check(l);
    }

    if (!r) {
        if (n->end != n->subtree_max) {
            n00b_print_mmap_tree(n, 0);
        }
        assert(n->end == n->subtree_max);
    }
    else {
        assert(r->parent == n);
        assert(n->subtree_max == r->subtree_max);
        assert(r->subtree_min >= n->end);
        assert(n->end < n->subtree_max);
        sanity_check(r);
    }
}
#else
#define sanity_check(x)
#endif

static void n00b_mmaps_remove_base(n00b_mmap_ctx_t *ctx, n00b_mmap_node_t *node);

static void
insert_under_node(n00b_mmap_ctx_t *ctx, n00b_mmap_node_t *ancestor, n00b_mmap_node_t *n)
{
    // This handles the first non-root node.
    n00b_mmap_node_t *cur;

    cur = n00b_atomic_load(&ancestor->left);
    if (!cur) {
        n00b_atomic_store(&n->parent, ancestor);
        n00b_atomic_store(&ancestor->left, n);
        ancestor->subtree_min = n->start;
        ancestor->subtree_max = n->end;
        n->parent             = ancestor;
        return;
    }

    while (true) {
        n00b_mmap_node_t *hit = find_overlap(cur, n);
        if (!hit) {
            break;
        }
        n00b_mmaps_remove_base(ctx, hit);
        cur = n00b_atomic_load(&ancestor->left);
        if (!cur) {
            n00b_atomic_store(&n->parent, ancestor);
            n00b_atomic_store(&ancestor->left, n);
            ancestor->subtree_min = n->start;
            ancestor->subtree_max = n->end;
            return;
        }
    }

    while (true) {
        if (n->start < cur->start) {
            n00b_mmap_node_t *l = n00b_atomic_load(&cur->left);
            if (l) {
                cur = l;
                continue;
            }
            n00b_atomic_store(&cur->left, n);
            break;
        }
        else {
            n00b_mmap_node_t *r = n00b_atomic_load(&cur->right);
            if (r) {
                cur = r;
                continue;
            }
            n00b_atomic_store(&cur->right, n);
            break;
        }
    }

    n00b_atomic_store(&n->parent, cur);

    while (true) {
        n00b_mmap_node_t *p = n00b_atomic_load(&n->parent);
        if (p->kind == n00b_mmap_root) {
            break;
        }
        if (p->priority >= n->priority) {
            break;
        }
        if (n00b_atomic_load(&p->right) == n) {
            n = rotate_left(p);
        }
        else {
            n = rotate_right(p);
        }
    }

    propagate_bounds_up(n);
}

static n00b_mmap_node_t *
mmaps_insert_raw(n00b_mmap_ctx_t     *ctx,
                 void                *startp,
                 uint64_t             blen,
                 n00b_mmap_rec_kind_t kind,
                 uint64_t             binary_offset)
{
    n00b_mmap_node_t *n;
    uint64_t          start = (uint64_t)startp;
    uint64_t          end   = start + blen;

    n = n00b_alloc(n00b_mmap_node_t, .allocator = (n00b_allocator_t *)&ctx->pool);

    *n = (n00b_mmap_node_t){
        .subtree_min   = start,
        .subtree_max   = end,
        .start         = start,
        .end           = end,
        .priority      = treap_next_priority(ctx),
        .kind          = kind,
        .binary_offset = binary_offset,
    };

    insert_under_node(ctx, &ctx->root, n);

    sanity_check(&ctx->root);

    return n;
}

static inline void
mmap_lock(n00b_mmap_ctx_t *ctx)
{
    int64_t tid      = n00b_thread_unique_id();
    int64_t expected = -1;

    do {
        if (expected == tid) {
            break;
        }
        expected = -1;
    } while (!n00b_cas(&ctx->tid_lock, &expected, tid));
}

static inline void
mmap_unlock(n00b_mmap_ctx_t *ctx)
{
    n00b_atomic_store(&ctx->tid_lock, -1);
}

// Might convert to a rw lock, but for now just a spin lock.
#define mmap_write_lock(ctx)   mmap_lock(ctx)
#define mmap_read_lock(ctx)    mmap_lock(ctx)
#define mmap_write_unlock(ctx) mmap_unlock(ctx)
#define mmap_read_unlock(ctx)  mmap_unlock(ctx)

static inline n00b_mmap_node_t *
lookup_under_node(n00b_mmap_node_t *n, uint64_t addr)
{
    while (true) {
        n00b_mmap_node_t *kid;

        if (addr < n->subtree_min || addr >= n->subtree_max) {
            return nullptr;
        }

        if (addr >= n->start && addr < n->end) {
            return n;
        }

        if (addr < n->start) {
            kid = n00b_atomic_load(&n->left);
        }
        else {
            kid = n00b_atomic_load(&n->right);
        }
        n = kid;
    }
}

n00b_mmap_node_t *
n00b_mmap_lookup(n00b_mmap_ctx_t *ctx, void *addr)
{
    n00b_mmap_node_t *result;

    mmap_read_lock(ctx);
    result = lookup_under_node(&ctx->root, (uint64_t)addr);
    mmap_read_unlock(ctx);

    return result;
}

// In memory_info.h
extern void n00b_load_static_ranges();

void
n00b_mmaps_initialize(n00b_mmap_ctx_t *ctx)
{
    *ctx = (n00b_mmap_ctx_t){
        .pool = {},
        .root         = {
                    .subtree_min = ~0,
                    .subtree_max = 0,
                    .start       = ~0,
                    .end         = 0,
                    .priority    = UINT32_MAX,
                    .kind        = n00b_mmap_root,
        },
        .tid_lock     = -1,
        .treap_seed   = N00B_MMAPS_PTR_SEED ^ (uint32_t)(uintptr_t)ctx,
    };

    n00b_pool_init(&ctx->pool, .__system = true, .hidden = true, .name = "mmaps");
    n00b_load_static_ranges();
}

static void
n00b_mmaps_remove_base(n00b_mmap_ctx_t *ctx, n00b_mmap_node_t *node)
{
    assert(node->kind != n00b_mmap_root);

    if (node->allocator->hidden) {
        return;
    }

    while (n00b_atomic_load(&node->left) || n00b_atomic_load(&node->right)) {
        n00b_mmap_node_t *l = n00b_atomic_load(&node->left);
        n00b_mmap_node_t *r = n00b_atomic_load(&node->right);

        if (!l) {
            rotate_left(node);
        }
        else if (!r) {
            rotate_right(node);
        }
        else if (l->priority > r->priority) {
            rotate_right(node);
        }
        else {
            rotate_left(node);
        }
    }

    n00b_mmap_node_t *parent = n00b_atomic_load(&node->parent);

    if (parent) {
        if (n00b_atomic_load(&parent->left) == node) {
            n00b_atomic_store(&parent->left, NULL);
        }
        else {
            n00b_atomic_store(&parent->right, NULL);
        }
        propagate_bounds_up(parent);
    }

    n00b_free(node);
}

static inline void
n00b_mmaps_remove(n00b_mmap_ctx_t *ctx, n00b_mmap_node_t *node)
{
    mmap_write_lock(ctx);
    n00b_mmaps_remove_base(ctx, node);
    mmap_write_unlock(ctx);
}

n00b_mmap_info_t *
n00b_mmap_register(void *startp, void *endp, n00b_mmap_rec_kind_t kind) _kargs
{
    n00b_runtime_t   *runtime           = n00b_get_runtime();
    const char       *file              = nullptr;
    n00b_allocator_t *allocator         = nullptr;
    uint64_t          binary_offset     = 0;
    intptr_t          slide             = 0;
    uint64_t          order_id          = 0;
    bool              definitely_unique = true;
}
{
    if (allocator && allocator->hidden) {
        return nullptr;
    }

    n00b_mmap_info_t *result;
    n00b_mmap_ctx_t  *ctx   = n00b_global_mem_map(runtime);
    uint64_t          start = (uint64_t)startp;
    uint64_t          end   = (uint64_t)endp;
    uint64_t          blen  = end - start;

    assert(ctx);

    assert(end > start);

    mmap_write_lock(ctx);

    if (!definitely_unique) {
        result = lookup_under_node(&ctx->root, (uint64_t)start);
        if (result) {
            assert(allocator == result->allocator || !allocator);
            mmap_write_unlock(ctx);
            return result;
        }
    }

    result            = mmaps_insert_raw(ctx, startp, blen, kind, binary_offset);
    result->allocator = allocator;
    result->file      = file;
    result->slide     = slide;
    result->order_id  = order_id;

    mmap_write_unlock(ctx);

    return result;
}

void *
_n00b_mmap(size_t sz, char *loc) _kargs
{
    n00b_allocator_t    *allocator = nullptr;
    n00b_mmap_rec_kind_t kind      = n00b_mmap_api_mmap;
    char                *name      = nullptr;
}
{
    if (name) {
        loc = name;
    }

    if (allocator) {
        sz = n00b_page_align(sz);
    }
    else {
        assert(kind == n00b_mmap_api_mmap || kind == n00b_mmap_arena);

        if (kind == n00b_mmap_arena) {
            sz = n00b_page_size;
        }
        else {
            sz = n00b_page_align(sz);
        }
    }

    void *result = mmap(nullptr, sz, N00B_MPROT, N00B_MFLAG, -1, 0);

    assert(result != MAP_FAILED);

    n00b_mmap_register(result,
                       ((char *)result) + sz,
                       kind,
                       .file      = loc,
                       .allocator = allocator);

    return result;
}

void
n00b_munmap(void *addr) _kargs
{
    n00b_runtime_t *runtime = n00b_get_runtime();
}
{
    n00b_mmap_info_t *map = n00b_mmap_by_address(addr, .runtime = runtime);

    assert(map);

    void  *start = (void *)map->start;
    size_t len   = map->end - map->start;

    n00b_mmaps_remove(n00b_global_mem_map(runtime), map);
    munmap(start, len);
}

n00b_mmap_info_t *
n00b_mmap_by_address(void *addr) _kargs
{
    n00b_runtime_t *runtime = n00b_get_runtime();
}
{
    return n00b_mmap_lookup(n00b_global_mem_map(runtime), addr);
}

n00b_allocator_opt_t
n00b_mem_get_allocator(void *addr) _kargs
{
    n00b_runtime_t *runtime = n00b_get_runtime();
}
{
    n00b_mmap_info_t *mmap = n00b_mmap_lookup(n00b_global_mem_map(runtime), addr);

    if (!mmap || !mmap->allocator) {
        return n00b_option_none(n00b_allocator_t *);
    }
    return n00b_option_set(n00b_allocator_t *, n00b_atomic_load(&mmap->allocator));
}
