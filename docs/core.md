# n00b Core Library

## Overview

The n00b core library provides the foundational runtime infrastructure:
memory allocation with garbage collection, type-safe generic containers,
threading and synchronization primitives, and algebraic data types.

The library is organized into six layers:

1. **Platform** &mdash; cross-platform types, TLS, atomics, futex.
2. **Memory** &mdash; allocation hierarchy (raw &rarr; arena &rarr;
   pool), mmap registry, GC, stop-the-world synchronization.
3. **Containers** &mdash; type-safe generic list, stack, array, linked
   list, dictionary, tree, and interval tree.
4. **Algebraic types** &mdash; option, result, variant, tuple.
5. **Utilities** &mdash; hashing, layout solver, buffer, string,
   random, time, alignment, variadic support.
6. **Runtime** &mdash; initialization, thread management, per-thread
   state.

### Design principles

- **Type safety via `typeid()`.** Every generic container is uniquely
  tagged at compile time by its element type(s).  No casts needed at
  call sites.
- **Allocation hierarchy.** Public macros (`n00b_alloc`, etc.) hide the
  internal allocation pipeline.  Never call `_n00b_alloc_raw()` directly.
- **Public APIs never return bare pointers.** Fallible operations return
  `n00b_result_t(T)`, optional values return `n00b_option_t(T)`, arrays
  return `n00b_array_t(T)`.
- **Minimal locking.** Spinlocks for hot paths, futex for thread
  synchronization, lock-free reads on dictionaries.
- **Keyword arguments.** Functions that allocate accept `.allocator` and
  other options via ncc's `_kargs` extension.

---

## Platform &mdash; `core/platform.h`

Cross-platform abstractions for threading, TLS, timing, and system
queries.  Automatically detects the platform and architecture:

| Macro | Platforms |
|-------|-----------|
| `BASE_PLATFORM_MACOS` / `_LINUX` / `_WINDOWS` | OS detection |
| `BASE_ARCH_ARM64` / `_X86_64` | Architecture detection |
| `BASE_ALIGN` | 16-byte alignment |

Key inline functions:

```c
void      *base_tls_get(base_tls_key_t key);
void       base_tls_set(base_tls_key_t key, void *value);
void       base_nanosleep_ns(uint64_t ns);
uint64_t   base_monotonic_ms(void);
uint64_t   base_monotonic_ns(void);
size_t     base_page_size(void);
```

### Atomics &mdash; `core/atomic.h`

Wrappers with explicit memory ordering:

```c
n00b_atomic_load(x)              // acquire
n00b_atomic_store(x, y)          // release
n00b_atomic_add(x, y)            // acq_rel
n00b_atomic_cas(x, y, z)         // acq_rel
n00b_atomic_or(x, y)             // acq_rel
n00b_atomic_read_then_set(x, y)  // exchange
n00b_atomic_fence()              // acq_rel barrier
```

### Futex &mdash; `core/futex.h`

Cross-platform fast userspace mutex:

```c
void n00b_futex_init(n00b_futex_t *futex);
int  n00b_futex_wait(n00b_futex_t *futex, uint32_t expected, uint64_t nsec);
int  n00b_futex_wake(n00b_futex_t *futex, bool all);
void n00b_futex_wait_for_value(volatile n00b_futex_t *futex, uint32_t value);
void n00b_futex_wait_on_mask(n00b_futex_t *futex, uint32_t mask);
```

Linux uses `SYS_futex`; macOS uses `__ulock_wait2()` / `__ulock_wake()`.

---

## Memory allocation

### Public API &mdash; `core/alloc.h`

Always use these macros &mdash; never call `_n00b_alloc_raw()` directly:

```c
n00b_alloc(T, ...)                  // Single object of type T
n00b_alloc_array(T, N, ...)         // Array of N elements
n00b_alloc_flex(T1, T2, N, ...)     // Struct T1 + trailing array of N T2
n00b_alloc_size(n, sz, ...)         // Raw byte allocation (when no type available)

n00b_free(ptr)                      // Deallocation
```

All allocation macros accept keyword arguments:

| Keyword | Default | Purpose |
|---------|---------|---------|
| `.allocator` | `nullptr` | Use specific allocator (nullptr = runtime default) |
| `.no_scan` | `false` | Don't scan this allocation for GC pointers |
| `.mem_debug` | `false` | Enable memory debug tracking |
| `.debug_taint` | `false` | Enable taint tracking |

Every allocation embeds `__FILE__:__LINE__` via `N00B_LOC_STRING()`.

**Example:**

```c
// Allocate a single struct:
my_struct_t *s = n00b_alloc(my_struct_t);

// Allocate an array of 100 ints:
int *arr = n00b_alloc_array(int, 100);

// Allocate with a specific allocator:
char *buf = n00b_alloc_array(char, 4096, .allocator = my_arena);

// Flexible array member:
my_flex_t *f = n00b_alloc_flex(my_flex_t, int, 50);
```

### Allocator interface &mdash; `core/alloc_base.h`

```c
typedef struct n00b_allocator_t {
    n00b_calloc_fn          zero_alloc;      // (self, size, params) -> void*
    n00b_free_fn            free;
    n00b_allocator_destroy_fn destroy;
    const char             *debug_name;
    uint8_t                 add_inline_header : 1;
    uint8_t                 __system : 1;    // Skip STW checks
    uint8_t                 __md_pool : 1;   // Is metadata pool
    uint8_t                 hidden : 1;      // Not in mmap tree
    n00b_allocator_t       *metadata_pool;
    n00b_dict_untyped_t    *metadata;
    void                   *opaque[];        // Allocator-specific data
} n00b_allocator_t;
```

### Allocation metadata &mdash; `core/alloc_mdata.h`

Each allocation carries metadata for GC scanning:

| Field | Purpose |
|-------|---------|
| `guard` | Magic value (`n00b_gc_guard`) for validation |
| `tinfo` | Type information string |
| `alloc_len` | Allocation size |
| `ptr_words` | Pointer count for GC scanning |
| `no_scan` | Skip this allocation during GC |
| `moved` | Set during GC relocation |

Metadata can be stored inline (prepended to data) or out-of-band
(in a separate metadata pool), depending on the allocator.

### Arena allocator &mdash; `core/arena.h`

Bump-pointer allocator with linked-list segment management:

```c
typedef struct n00b_arena_t {
    n00b_base_allocator_t   vtable;
    _Atomic(char *)         next_alloc;         // Bump pointer
    _Atomic(n00b_segment_t *) current_segment;
    uint32_t                collection_enabled : 1;
    // ...
} n00b_arena_t;
```

Allocation is a simple `next_alloc += size`.  When the current segment
is exhausted, a new segment is linked in &mdash; or, if
`collection_enabled`, a GC cycle is triggered.

### Pool allocator &mdash; `core/pool.h`

Fixed-size slab allocator with lock-free free lists (Treiber stacks):

```c
typedef struct n00b_pool_t {
    n00b_base_allocator_t vtable;
    n00b_llstack_t        free_lists[4];    // 4 size classes (power-of-2)
    n00b_pool_page_t     *page_table;
    _Atomic uint32_t      lock;
} n00b_pool_t;
```

Used internally for metadata pools, GC root lists, and other
system-level allocations that must not participate in GC.

### Memory mapping registry &mdash; `core/mmaps.h`

Global interval-tree tracking all mapped memory regions:

```c
n00b_mmap_info_t *n00b_mmap_register(n00b_mmap_ctx_t *ctx, uint64_t start,
                                       uint64_t end, n00b_mmap_rec_kind_t kind, +);

void n00b_mmap_unregister(n00b_mmap_ctx_t *ctx, n00b_mmap_info_t *info);

n00b_mmap_info_t *n00b_mmap_by_address(n00b_mmap_ctx_t *ctx, void *addr);

n00b_allocator_t *n00b_mem_get_allocator(void *ptr);
```

Region kinds:

| Kind | Purpose |
|------|---------|
| `n00b_mmap_static` | Static data segments |
| `n00b_mmap_arena` | Arena allocation regions |
| `n00b_mmap_managed_segment` | GC-managed arena segments |
| `n00b_mmap_sys_segment` | System allocator segments |
| `n00b_mmap_stack` | Thread stacks |
| `n00b_mmap_pool` | Pool allocator pages |

---

## Garbage collection &mdash; `core/gc.h`

Copying/compacting collector for arena allocators:

```c
void n00b_collect(n00b_arena_t *arena);

#define n00b_gc_register_root(var) \
    _n00b_gc_register_root(&(var), sizeof(var) / sizeof(void *))

void _n00b_gc_unregister_root(void *addr);
```

### Collection algorithm

1. **Setup** &mdash; allocate a to-space arena (at least the size of
   the from-space; doubled if many segments exist).
2. **Root scanning** &mdash; scan registered roots, thread stacks, and
   argv/envp for pointers into the from-space.
3. **Worklist** &mdash; copy marked allocations to the to-space, update
   all pointer fields.  A memo table (old&rarr;new) prevents
   double-copying.
4. **Swap** &mdash; replace the from-space segments with to-space.
5. **Cleanup** &mdash; unmap old segments.

### Root sources

1. User-registered roots (`n00b_gc_register_root()`)
2. All registered thread stacks
3. `n00b_runtime_t::argv` and `envp`

---

## Stop-the-world &mdash; `core/stw.h`

Thread suspension for safe GC:

```c
#define n00b_stop_the_world()    _n00b_stop_the_world(N00B_LOC_STRING())
#define n00b_restart_the_world() _n00b_restart_the_world(N00B_LOC_STRING())

void n00b_thread_checkin(void);
```

How it works:
1. Caller acquires STW lock (CAS on `runtime->stw`).
2. Sets the STW flag in each thread's futex.
3. Each thread calls `n00b_thread_checkin()` at safe points; if the STW
   flag is set, the thread blocks.
4. Once all threads are suspended, GC runs.
5. Caller resets futexes; threads wake and check in.

### Blocking operations

```c
n00b_run_blocking(...) {
    // Saves stack state via setjmp
    // Suspends thread (safe for GC to scan stack)
    // Runs blocking operation
    // Resumes thread
}
```

This macro wraps blocking system calls so the thread is marked as
suspended (GC-safe) while blocked.

---

## Thread management &mdash; `core/thread.h`

### Per-thread state

```c
typedef struct n00b_thread_t {
    union {
        struct { int32_t id; int32_t generation; } parts;
        uint64_t unique_id;
    } id_info;

    void              *stack_base;
    void              *stack_top;
    n00b_mmap_info_t  *stack_map;
    pthread_t          pthread_id;
    n00b_futex_t       self_lock;
    // ...
} n00b_thread_t;

extern thread_local n00b_thread_t __n00b_thread_self;
```

### Thread-local access

```c
n00b_thread_t *n00b_thread_self(void);
int64_t        n00b_thread_unique_id(void);   // (slot << 32) | generation
int32_t        n00b_thread_id(void);           // Slot index
```

### Stack tracking

```c
void n00b_capture_stack_base(n00b_thread_t *thread, n00b_runtime_t *rt);
void n00b_capture_stack_top(n00b_thread_t *thread);
```

Stack base is captured once at thread registration.  Stack top is
updated at GC safe points to bound the region the collector must scan.

---

## Generic containers

All containers follow the same pattern:

```c
n00b_<container>_decl(T);                // Declare the struct type
n00b_<container>_t(T) x = n00b_<container>_new(T);  // Instantiate
n00b_<container>_<op>(x, ...);           // Use
n00b_<container>_free(x);                // Free
```

Type safety is enforced by `typeid()` &mdash; each unique type
combination gets its own struct definition.

### List &mdash; `core/list.h`

Growable dynamic array with spinlock:

```c
n00b_list_t(T)   x  = n00b_list_new(T);
n00b_list_t(T)   x  = n00b_list_new_cap(T, 64);

n00b_list_push(x, val);
T val = n00b_list_pop(x);
n00b_list_push_front(x, val);
T val = n00b_list_pop_front(x);

n00b_list_insert(x, i, val);
n00b_list_delete(x, i);
T val = n00b_list_get(x, i);          // Bounds-checked
n00b_list_set(x, i, val);             // Bounds-checked

n00b_list_find(x, val);               // Linear search, returns index
n00b_list_sort(x, cmp);               // qsort
n00b_list_remove_all(x, val);
n00b_list_clear(x);

n00b_list_foreach(x, var) { ... }     // Unlocked iteration

n00b_array_t(T) a = n00b_list_to_array(T, x);  // Move to fixed array
```

All mutating operations acquire the spinlock.  `foreach` is explicitly
unlocked &mdash; the caller must ensure exclusive access.

**Example:**

```c
n00b_list_decl(int);
n00b_list_t(int) nums = n00b_list_new(int);

n00b_list_push(nums, 42);
n00b_list_push(nums, 17);
n00b_list_push(nums, 99);

n00b_list_sort(nums, int_cmp);

n00b_list_foreach(nums, n) {
    printf("%d\n", n);
}

n00b_list_free(nums);
```

### Stack &mdash; `core/stack.h`

LIFO stack (same backing as list):

```c
n00b_stack_t(T) s = n00b_stack_new(T);

n00b_stack_push(s, val);
T val = n00b_stack_pop(s);
T val = n00b_stack_peek(s);
bool empty = n00b_stack_is_empty(s);
n00b_stack_clear(s);
n00b_stack_foreach(s, var) { ... }
```

### Array &mdash; `core/array.h`

Fixed-capacity, no lock:

```c
n00b_array_t(T) a = n00b_array_new(T, 100);

T val = n00b_array_get(a, i);
n00b_array_set(a, i, val);         // Extends len if i >= len
n00b_array_t(T) b = n00b_array_clone(a);
n00b_array_foreach(a, var) { ... }
```

### Linked list &mdash; `core/llist.h`

Intrusive doubly-linked list (not thread-safe):

```c
n00b_linked_list_decl(T);

n00b_linked_list_append(lptr, item);
T first = n00b_linked_list_first(lptr);
node = n00b_linked_list_next(nptr);
T val = n00b_linked_list_node_contents(nptr);
```

### Lock-free stack &mdash; `core/llstack.h`

Treiber stack with ABA protection (used internally for pool free lists):

```c
n00b_llstack_push_node(n00b_llstack_t *llstack, n00b_llstack_node_t *item);
void *n00b_llstack_pop_node(n00b_llstack_t *llstack);
```

### Dictionary &mdash; `core/dict.h`, `core/dict_untyped.h`

Open-addressing hash table with robin-hood probing.  Lock-free reads,
single-writer migration:

```c
n00b_dict_untyped_t *d = n00b_dict_untyped_new(hash_fn, allocator);

void *old = _n00b_dict_untyped_put(d, key, value);
void *val = _n00b_dict_untyped_get(d, key, &found);
bool ok   = _n00b_dict_untyped_replace(d, key, value);
bool ok   = _n00b_dict_untyped_add(d, key, value);  // Fails if exists
bool ok   = _n00b_dict_untyped_remove(d, key);
```

The hash table auto-resizes when load exceeds a threshold.  During
migration, readers see a consistent snapshot; a futex gates concurrent
writers.

### Tree &mdash; `core/tree.h`

N-ary tree with typed nodes and leaves:

```c
n00b_tree_decl(NodeType, LeafType);

auto node  = n00b_tree_node(N, L, value);
auto leaf  = n00b_tree_leaf(N, L, value);
n00b_tree_add_child(parent, N, L, child);
```

### Interval tree &mdash; `core/interval_tree.h`

AVL-balanced interval tree (used for the mmap registry):

```c
n00b_interval_tree_t *t = n00b_new_interval_tree(allocator);

n00b_result_t(n00b_interval_node_t *)
n00b_interval_insert(t, low, high, data);

n00b_result_t(int)
n00b_interval_delete(t, target);
```

---

## Algebraic data types

### Option &mdash; `core/option.h`

```c
n00b_option_t(T)  opt = n00b_option_set(T, value);     // Some(value)
n00b_option_t(T)  opt = n00b_option_none(T);            // None

bool has = n00b_option_is_set(opt);
T    val = n00b_option_get(opt);                         // UB if none
T    val = n00b_option_get_or_else(opt, fallback);

n00b_option_match(opt, set_expr, none_expr);

n00b_option_t(T)  opt = n00b_option_from_nullable(T, ptr);  // nullptr → none
```

### Result &mdash; `core/result.h`

```c
n00b_result_t(T)  r = n00b_result_ok(T, value);
n00b_result_t(T)  r = n00b_result_err(T, errno_val);

bool ok  = n00b_result_is_ok(r);
bool err = n00b_result_is_err(r);
T    val = n00b_result_get(r);                // UB if err
int  e   = n00b_result_get_err(r);            // UB if ok
T    val = n00b_result_get_or_else(r, fallback);

n00b_result_match(r, ok_expr, err_expr);
```

#### POSIX wrappers

```c
n00b_result_t(int)   r = n00b_check_posix(open("/tmp/f", O_RDONLY));
n00b_result_t(void*) r = n00b_check_mmap(mmap(...));
n00b_result_t(long)  r = n00b_check_sysconf(_SC_PAGESIZE);
```

### Variant &mdash; `core/variant.h`

Tagged union with compile-time type checking:

```c
n00b_variant_decl(int, double, char *);

n00b_variant_t(int, double, char *) v = n00b_variant_ctor(0, int, 42);

bool is_int = n00b_variant_is(v, 0);
int  val    = n00b_variant_get(v, int);

n00b_variant_match(v, handler);
```

### Tuple &mdash; `core/tuple.h`

Fixed-size heterogeneous record:

```c
n00b_tuple_decl(int, double, char *);

auto t = n00b_tuple(42, 3.14, "hello");

int    a = n00b_tuple_get(t, 0);   // 42
double b = n00b_tuple_get(t, 1);   // 3.14
char  *c = n00b_tuple_get(t, 2);   // "hello"
```

Field access via `constexpr_paste()` generates `item_0`, `item_1`, etc.

---

## String and buffer

### String &mdash; `core/string.h`

Immutable UTF-8 string (by-value struct):

```c
typedef struct n00b_string_t {
    int64_t  u8_bytes;     // UTF-8 byte count
    char    *data;         // UTF-8 data (not NUL-terminated in general)
    int64_t  codepoints;   // Codepoint count
    void    *styling;      // Rich text metadata (opaque)
} n00b_string_t;

n00b_string_t n00b_string_from_raw(n00b_allocator_t *alloc,
                                     const char *src, int64_t byte_len,
                                     int64_t cp_count);
n00b_string_t n00b_string_empty(n00b_allocator_t *alloc);
```

See [docs/strings.md](strings.md) for the full string operations library.

### Buffer &mdash; `core/buffer.h`

Mutable growable byte buffer with spinlock:

```c
n00b_buffer_t *n00b_buffer_empty(void);
n00b_buffer_t *n00b_buffer_from_bytes(const char *data, int64_t len);
void           n00b_buffer_free(n00b_buffer_t *buf);
```

Key operations include hex encode/decode, searching, slicing, and
conversion to `n00b_string_t`.

---

## Hashing &mdash; `core/hash.h`

128-bit hashing via XXH3:

```c
n00b_uint128_t n00b_hash_word(void *value);
n00b_uint128_t n00b_hash_cstring(void *value);
n00b_uint128_t n00b_hash(void *obj, n00b_hash_fn fn);
n00b_uint128_t n00b_string_hash(n00b_string_t s);
n00b_uint128_t n00b_buffer_hash(n00b_buffer_t *b);
```

All hash functions return `n00b_uint128_t` for collision resistance.

---

## Layout solver &mdash; `core/layout.h`

1D constraint-based layout engine used by the table library:

```c
typedef struct {
    n00b_layout_dim_t min, max, pref;
    int64_t priority;
    int64_t flex_multiple;
    int64_t child_gap;
} n00b_layout_t;

typedef struct {
    int64_t computed_min, computed_max, computed_pref;
    int64_t size;                    // Output: final computed size
} n00b_layout_result_t;

void n00b_layout_calculate(n00b_layout_t *items,
                            n00b_layout_result_t *results,
                            size_t count, int64_t available);
```

`n00b_layout_dim_t` supports both absolute (`int64_t`) and percentage
(`double`) dimensions.

### Algorithm

1. Resolve percentage dimensions against available space.
2. Assign each item its minimum (or preferred if larger).
3. Grow items toward their maximum, smallest-first.
4. Distribute remaining space proportionally by `flex_multiple`.
5. If over-constrained: shrink flex items first (largest-first), then
   rigid items.
6. If still over: force-crop by ascending priority (lowest dropped
   first).

---

## Variadic support &mdash; `core/vargs.h`

ncc's `+` variadic transform packs arguments into:

```c
typedef struct n00b_vargs_t {
    unsigned int nargs;
    unsigned int cur_ix;
    void       **args;
} n00b_vargs_t;
```

At call sites, ncc rewrites `f(a, b, c)` to pass a compound literal:
`f(&(n00b_vargs_t){.nargs=3, .cur_ix=0, .args=(void*[]){a, b, c}})`.

See the [ncc documentation](ncc.md) for details on the `+` extension.

---

## Runtime initialization &mdash; `core/runtime.h`

```c
typedef struct n00b_runtime_t {
    n00b_array_t(char *)    argv, envp;
    n00b_mmap_ctx_t         mmaps;
    _Atomic(n00b_allocator_t *) default_allocator;
    n00b_arena_t           *default_arena;
    n00b_pool_t             gc_root_pool;
    n00b_list_t(n00b_gc_root_t) gc_roots;
    _Atomic(n00b_thread_t *) thread_list[N00B_THREADS_MAX];
    n00b_futex_t            stw;
    // ...
} n00b_runtime_t;

void n00b_init(n00b_runtime_t *rt, int argc, char *argv[], +);
    // keyword args:
    //   .allocator      = nullptr  (nullptr = GC'd arena)
    //   .envp           = nullptr
    //   .numeric_locale = ""
    //   .fd_limit       = 0
    //   .max_threads    = N00B_THREADS_MAX

n00b_allocator_t *n00b_default_allocator(void);
n00b_allocator_t *n00b_slab_allocator(void);
```

### Initialization steps

1. Cache page size, generate GC guard value.
2. Initialize mmap registry with interval tree.
3. Set up slab allocator (hidden system pool for metadata).
4. Initialize GC root pool.
5. Register main thread (capture stack base).
6. Create default arena (GC-enabled) or use provided allocator.

**Example:**

```c
int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    // Runtime is now live: allocator, GC, threading all ready.
    // ...

    return 0;
}
```

---

## Utility modules

### Random &mdash; `core/random.h`

```c
void     n00b_random_bytes(char *buf, size_t len);
uint64_t n00b_rand64(void);
uint32_t n00b_rand32(void);
```

Linux uses `getrandom(2)`; macOS/BSD uses `arc4random_buf()`.

### Time &mdash; `core/time.h`

```c
void    n00b_capture_timestamp(n00b_duration_t *output);
int64_t n00b_ns_from_duration(n00b_duration_t *d);
int64_t n00b_ns_minus(n00b_duration_t *d1, n00b_duration_t *d2);
int64_t n00b_ns_timestamp(void);
```

### Alignment &mdash; `core/align.h`

```c
void    *n00b_align_to_page_start(void *addr);
uint64_t n00b_page_align(uint64_t n);
uint64_t n00b_align(uint64_t n);     // Align to N00B_ALIGN (16)
uint64_t n00b_align_floor(uint64_t n, uint64_t base);
uint64_t n00b_align_ceil(uint64_t n, uint64_t base);
```

### Macros &mdash; `core/macros.h`

```c
N00B_LOC_STRING()               // "__FILE__:__LINE__"
N00B_VA_COUNT(...)              // Count variadic arguments
N00B_MAP(macro, ...)            // Apply macro to each argument
N00B_MAP_COUNT(macro, N, ...)   // MAP with counter
N00B_EVAL(...)                  // Deep recursive macro expansion
```

### Memory introspection &mdash; `core/memory_info.h`

```c
size_t n00b_address_is_probable_cstring(void *addr, size_t *bytelen,
                                         size_t min_len);
```

Heuristically detects probable UTF-8 C strings by scanning for valid
UTF-8 sequences with a NUL terminator within the current page.

---

## Cross-cutting patterns

### Generic container pattern

All containers follow this template:

```c
// Declare the type (typically in a header):
n00b_list_decl(int);

// Create an instance:
n00b_list_t(int) nums = n00b_list_new(int);

// Use it:
n00b_list_push(nums, 42);
int val = n00b_list_pop(nums);

// Free it:
n00b_list_free(nums);
```

The `*_decl()` macro expands a struct with a `typeid()`-based tag.
Each unique type combination gets its own struct &mdash; this is how
type safety is enforced without language-level generics.

### Keyword arguments

Most functions that allocate accept optional keyword arguments:

```c
n00b_alloc(my_struct_t, .allocator = arena, .no_scan = true)
n00b_list_new_cap(int, 256, .allocator = my_alloc)
n00b_init(&rt, argc, argv, .fd_limit = 1024)
```

### Thread safety summary

| Component | Mechanism |
|-----------|-----------|
| List, stack | Spinlock (mutating ops only) |
| Array | No lock (caller-managed) |
| Linked list | No lock |
| Dictionary | Lock-free reads, futex for migration |
| Pool free list | Lock-free Treiber stack |
| Arena | Atomic bump pointer, spinlock for segments |
| Mmap registry | TID-based reentrant spinlock |
| GC | Stop-the-world (futex-based) |

---

## Quick reference

| Task | API |
|------|-----|
| Initialize runtime | `n00b_init(&rt, argc, argv)` |
| Allocate object | `n00b_alloc(T)` |
| Allocate array | `n00b_alloc_array(T, N)` |
| Free allocation | `n00b_free(ptr)` |
| Register GC root | `n00b_gc_register_root(var)` |
| Trigger collection | `n00b_collect(arena)` |
| Stop the world | `n00b_stop_the_world()` |
| Create list | `n00b_list_new(T)` |
| Push to list | `n00b_list_push(list, val)` |
| Create stack | `n00b_stack_new(T)` |
| Create array | `n00b_array_new(T, N)` |
| Create option | `n00b_option_set(T, val)` / `n00b_option_none(T)` |
| Create result | `n00b_result_ok(T, val)` / `n00b_result_err(T, e)` |
| POSIX call check | `n00b_check_posix(syscall())` |
| Hash string | `n00b_string_hash(s)` |
| Hash buffer | `n00b_buffer_hash(b)` |
| Layout solve | `n00b_layout_calculate(items, results, n, avail)` |
| Get thread self | `n00b_thread_self()` |
| Default allocator | `n00b_default_allocator()` |
