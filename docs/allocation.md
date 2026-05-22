# Allocation Guide

This is the short operational guide for choosing libn00b allocators. See
`docs/core.md` for the lower-level allocator and GC internals.

## Selection Order

Allocator resolution is intentionally conservative:

1. An explicit `.allocator` kwarg or `n00b_alloc_opts_t`.
2. The current thread's scoped allocator override.
3. The runtime default allocator.

Use explicit allocator kwargs at ownership boundaries. Use the scoped
override for bounded scratch work where pushing allocator plumbing through
every leaf API would be noise.

## Default Allocator

The runtime default allocator is normally the GC-enabled default arena.
Use it for ordinary libn00b objects whose lifetime is managed by the
runtime and whose pointers are visible to GC roots, stacks, runtime
state, or other scanned objects.

Do not cache raw pointers to default-arena objects in unscanned memory.
The collector can move default-arena allocations and only rewrites
pointers it sees while scanning.

## Explicit Allocators

Most higher-level allocating APIs accept `.allocator`. Prefer that when
the returned object belongs to a known owner, such as a conduit, parser,
compiled regex, long-lived endpoint, or caller-owned arena.

For raw allocation helpers, the most explicit forms are:

```c
thing_t *thing = n00b_alloc_with_opts(
    thing_t,
    &(n00b_alloc_opts_t){.allocator = owner_alloc});

char *buf = n00b_alloc_array_with_opts(
    char,
    len,
    &(n00b_alloc_opts_t){.allocator = owner_alloc});
```

If an API accepts `.allocator`, explicit allocator arguments must win over
the scoped allocator and the runtime default.

## Scoped Scratch Allocator

Use `n00b_with_allocator(allocator) { ... }` around high-churn work that
allocates many temporary strings, buffers, arrays, or formatting results:

```c
n00b_with_allocator((n00b_allocator_t *)frame_arena) {
    n00b_string_t *label = n00b_cformat("frame [|#|]", frame_name);
    process_label(label);
}
```

The override is thread-local. The main thread and n00b worker threads have
independent current allocators, and nested scopes restore correctly.

Lifetime rule: objects allocated from the scoped allocator must not escape
the block unless that allocator outlives every escaped object. Restore the
previous allocator before resetting or destroying scratch storage.

GC rule: a scoped allocator pointer to an arena header does not by itself
make the collector scan the whole arena. However, if a pointer to a
visible scratch allocation is found in scanned memory, that scratch
allocation can be scanned and any pointers stored inside it can keep other
GC objects live until the scratch references disappear or the scratch
allocator is reset or destroyed.

## Arenas

Arenas are bump-pointer allocators with segment backing.

Use a GC-enabled arena for normal movable managed objects. This is what
the runtime default usually is.

Use a non-GC arena for bounded scratch or batch lifetime when you can
destroy the whole arena after the work completes. This is the right shape
for wax/crayon raw-gateway per-frame or per-batch temporaries, provided
scratch pointers do not escape into durable endpoint or lifecycle state.

Avoid putting stable cross-thread control structures in movable arenas
unless every pointer to them is visible to the collector.

## Pools

There are two pool-like memory allocators in the checked-in runtime:

- `n00b_pool_t` from `core/pool.h` is the reusable pool allocator. It
  owns pages, divides them into power-of-two size classes, and uses free
  lists for reuse.
- `runtime->slab_allocator` is internal runtime plumbing. It is a hidden,
  system allocator whose allocation path maps memory directly; callers
  should not treat it as the normal pool API.

Use `n00b_pool_t` for stable, allocator-owned service state, fixed-size
churn, and subsystems that need non-moving addresses. Conduit-owned
memory and similar lifecycle state should use the owning conduit or
subsystem allocator explicitly, not a temporary scoped scratch allocator.

Destroy a pool only after all objects and cross-thread references backed
by it are gone. If pool memory stores pointers to GC-managed objects, make
sure the pool is visible and those pointers are scanned, or use explicit
rooting/ownership that keeps pointer rewriting correct.

## System And Hidden Allocators

`system_pool` is a runtime-owned `n00b_pool_t`. The slab allocator is a
separate internal runtime allocator. Both are hidden from GC and suitable
for runtime metadata, root records, locks, and caches that need stable
addresses. Do not use them for ordinary user objects that need GC
traversal.

Hidden allocators are invisible to the GC. Use them only for data that is
opaque to the collector or for runtime-owned metadata whose reachable
managed pointers are tracked elsewhere.

## Scan Shape

Use scan controls when the allocation shape is known:

- `.scan_kind = N00B_GC_SCAN_KIND_NONE` for POD bytes or arrays with no
  GC pointers.
- `.scan_kind = N00B_GC_SCAN_KIND_CALLBACK` when a custom scanner is
  required and the allocator supports out-of-band metadata.
- Leave the default for ordinary objects that may contain pointers.

`no_scan` and `scan_kind` only control descent into an allocation. They do
not change the need to keep pointers to movable objects in scanned memory
if those pointers must be rewritten during GC.

## Practical Defaults

- Ordinary object: default allocator.
- Known durable owner: explicit `.allocator = owner->allocator`.
- Hot temporary frame/batch work: `n00b_with_allocator(scratch)`.
- Cross-thread or lifecycle infrastructure: explicit pool/conduit
  allocator.
- Runtime metadata or global caches requiring stable addresses:
  `system_pool`, only inside runtime/library internals.
