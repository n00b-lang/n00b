#define N00B_MEM_INTERNAL_API

#include "n00b.h"
#include "core/mmaps.h"
#include "core/memory_info.h"
#include "core/alloc_mdata.h"
#include "core/atomic.h"
#include "core/futex.h"
#include "core/align.h"
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
    n00b_mmap_free_t *fl    = n00b_atomic_load(&ctx->free_nodes);
    uint64_t          start = (uint64_t)startp;
    uint64_t          end   = start + blen;

    if (fl) {
        n = (n00b_mmap_node_t *)fl;
        n00b_atomic_store(&ctx->free_nodes, n00b_atomic_load(&fl->next));
    }
    else {
        n = ctx->current_pool->next_node;
        ctx->current_pool->next_node++;
    }

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

static inline bool
pool_at_end(n00b_mmap_pool_t *pool)
{
    return pool->next_node + sizeof(n00b_mmap_node_t) > pool->end;
}

static inline void
mmap_lock(n00b_mmap_ctx_t *ctx)
{
    while (n00b_atomic_or(&ctx->lock, 1))
        ;
}

static inline void
mmap_unlock(n00b_mmap_ctx_t *ctx)
{
    n00b_atomic_store(&ctx->lock, 0);
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
n00b_mmaps_lookup(n00b_mmap_ctx_t *ctx, void *addr)
{
    n00b_mmap_node_t *result;

    mmap_read_lock(ctx);
    result = lookup_under_node(&ctx->root, (uint64_t)addr);
    mmap_read_unlock(ctx);

    return result;
}

static inline n00b_mmap_pool_t *
alloc_mmap_pool(n00b_mmap_ctx_t *ctx)
{
    uint32_t          len = n00b_page_size * ctx->next_pool_sz;
    n00b_mmap_pool_t *pool;

    ctx->next_pool_sz <<= 1;

    pool = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

    assert(pool != MAP_FAILED);

    pool->next_node   = &pool->nodes[0];
    pool->end         = (n00b_mmap_node_t *)(((char *)pool) + len);
    pool->prev_pool   = ctx->current_pool;
    ctx->current_pool = pool;

    mmaps_insert_raw(ctx, pool, len, n00b_mmap_internal, 0);
    return pool;
}

n00b_mmap_node_t *
n00b_mmaps_register_mem_map(n00b_mmap_ctx_t     *ctx,
                            void                *start,
                            uint64_t             blen,
                            uint64_t             binary_offset,
                            n00b_mmap_rec_kind_t kind,
                            bool                 definitely_unique)
{
    n00b_mmap_node_t *result;

    mmap_write_lock(ctx);

    if (!ctx->free_nodes && pool_at_end(ctx->current_pool)) {
        alloc_mmap_pool(ctx);
    }

    if (!definitely_unique) {
        result = lookup_under_node(&ctx->root, (uint64_t)start);
        if (result) {
            mmap_write_unlock(ctx);
            return result;
        }
    }

    result = mmaps_insert_raw(ctx, start, blen, kind, binary_offset);

    mmap_write_unlock(ctx);

    return result;
}

void
n00b_mmaps_initialize(n00b_mmap_ctx_t *ctx)
{
    *ctx = (n00b_mmap_ctx_t){
        .initial_pool = nullptr,
        .current_pool = nullptr,
        .free_nodes   = nullptr,
        .root         = {
                    .subtree_min = ~0,
                    .subtree_max = 0,
                    .start       = ~0,
                    .end         = 0,
                    .priority    = UINT32_MAX,
                    .kind        = n00b_mmap_root,
        },
        .lock         = 0,
        .next_pool_sz = N00B_MMAPS_START_PAGES,
        .treap_seed   = N00B_MMAPS_PTR_SEED ^ (uint32_t)(uintptr_t)ctx,
    };

    alloc_mmap_pool(ctx);
    ctx->initial_pool = ctx->current_pool;
    n00b_load_static_ranges();
}

void
n00b_mmaps_destroy(n00b_mmap_ctx_t *ctx)
{
    n00b_mmap_pool_t *p = ctx->initial_pool;

    while (p) {
        n00b_mmap_pool_t *next = p->prev_pool;
        munmap(p, ((uint64_t)p->end) - (uint64_t)p);
        p = next;
    }
}

static void
n00b_mmaps_remove_base(n00b_mmap_ctx_t *ctx, n00b_mmap_node_t *node)
{
    assert(node->kind != n00b_mmap_root);

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

    n00b_mmap_node_t *parent  = n00b_atomic_load(&node->parent);
    n00b_mmap_free_t *fl_item = (void *)node;

    if (parent) {
        if (n00b_atomic_load(&parent->left) == node) {
            n00b_atomic_store(&parent->left, NULL);
        }
        else {
            n00b_atomic_store(&parent->right, NULL);
        }
        propagate_bounds_up(parent);
    }

    memset(node, 0, sizeof(n00b_mmap_node_t));
    n00b_atomic_store(&fl_item->next, n00b_atomic_load(&ctx->free_nodes));
    n00b_atomic_store(&ctx->free_nodes, fl_item);
}

void
n00b_mmaps_remove(n00b_mmap_ctx_t *ctx, n00b_mmap_node_t *node)
{
    mmap_write_lock(ctx);
    n00b_mmaps_remove_base(ctx, node);
    mmap_write_unlock(ctx);
}

bool
n00b_mmap_is_arena_segment(n00b_mmap_info_t *map)
{
    switch (map->kind) {
    case n00b_mmap_managed_segment:
    case n00b_mmap_sys_segment:
        return true;
    default:
        return false;
    }
}

bool
n00b_mmap_is_arena(n00b_mmap_info_t *map)
{
    switch (map->kind) {
    case n00b_mmap_arena:
        return true;
    default:
        return false;
    }
}

n00b_mmap_rec_kind_t
n00b_mmap_get_kind(n00b_mmap_info_t *map)
{
    return map->kind;
}

n00b_mmap_ctx_t n00b_global_mem_maps;

n00b_mmap_info_t *
n00b_mmap_by_address(void *addr)
{
    return n00b_mmaps_lookup(&n00b_global_mem_maps, addr);
}

// Register a memory map in the global registry, as opposed to using the
// underlying data structure.
n00b_mmap_info_t *
n00b_register_mmap(const void          *startp,
                   const void          *endp,
                   const char          *file,
                   n00b_allocator_t    *allocator,
                   uint64_t             binary_offset,
                   intptr_t             slide,
                   n00b_mmap_rec_kind_t kind,
                   uint64_t             order_id,
                   bool                 definitely_unique)
{
    n00b_mmap_info_t *result;
    uint64_t          start = (uint64_t)startp;
    uint64_t          end   = (uint64_t)endp;

    result = n00b_mmaps_register_mem_map(&n00b_global_mem_maps,
                                         (void *)start,
                                         end - start,
                                         binary_offset,
                                         kind,
                                         definitely_unique);

    if (!result->allocator) {
        assert(allocator);
        result->allocator = allocator;
        result->file      = (char *)file;
        result->slide     = slide;
    }
    else {
        if (allocator) {
            assert(allocator == result->allocator);
        }
    }

    result->order_id = order_id;

    return result;
}

void
n00b_unregister_mmap(void *start)
{
    n00b_mmap_info_t *map = n00b_mmap_by_address(start);

    assert(map);
    n00b_mmaps_remove(&n00b_global_mem_maps, map);
}

static void
remove_arena_segments(n00b_mmap_node_t *n, n00b_arena_t *arena)
{
    if (!n) {
        return;
    }
    remove_arena_segments(n->left, arena);
    remove_arena_segments(n->right, arena);
}

// This needs to be done better, so we don't risk blowing the stack.
// When we add and remove, we don't have to keep backtracking
// state. When we walk, we do!
void
n00b_mmaps_slow_rm_arena_segments(n00b_arena_t *arena)
{
    assert(arena);
    mmap_lock(&n00b_global_mem_maps);
    remove_arena_segments(&n00b_global_mem_maps.root, arena);
    mmap_unlock(&n00b_global_mem_maps);
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
        kind = n00b_mmap_pool;
        sz   = n00b_page_align(sz);
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

    n00b_register_mmap(result, ((char *)result) + sz, loc, allocator, 0, 0, kind, 0, true);

    return result;
}

void
n00b_munmap(void *addr)
{
    n00b_mmap_info_t *mm    = n00b_mmap_by_address(addr);
    void             *start = (void *)mm->start;
    size_t            len   = mm->end - mm->start;

    n00b_mmaps_remove(&n00b_global_mem_maps, mm);
    munmap(start, len);
}
