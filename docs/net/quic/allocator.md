# Allocator discipline for the QUIC module

This is a **mandatory rule**, not a suggestion.  Phase 1 PRs that
violate it will be rejected.

## The rule

> All long-lived QUIC state — `n00b_quic_endpoint_t`, `n00b_quic_conn_t`,
> `n00b_quic_chan_t`, `n00b_quic_secret_t`, `n00b_quic_trust_t`, the UDP
> socket handle `n00b_conduit_udp_t`, every per-datagram message
> payload published by the IO backend — **must** be allocated from the
> conduit's allocator (typically the runtime's `conduit_pool`), never
> from the default GC arena.

## Why

The default GC arena is a moving collector.  Allocations there can be
relocated by the GC during a collection cycle.  That is fine for
data structures touched only by the calling thread under the GC's
view of the world.

It is **not** fine for objects whose addresses are held by:

- **picoquic** — it stores `app_ctx` raw pointers it expects to
  remain stable across the entire connection.
- **picotls** — same story for its handshake context.
- **The conduit IO backend** — `n00b_conduit_io_target_t` holds a raw
  pointer to the variant-tagged target (fd_owner, listener, or UDP);
  that pointer is consulted on every readiness event from another
  thread.
- **kqueue / epoll / io_uring `udata`** — the kernel itself stores
  pointers we hand it, which it gives back on event dispatch.

A GC move under any of these holders produces a use-after-free that
manifests as a crash on the *next* dispatch — minutes or hours later,
indeterministic, with a stack that looks unrelated to the bug.

The bug class is documented in
`~/.claude/projects/-Users-viega-n00b/memory/conduit_allocator_audit.md`,
which is the post-mortem on this failure mode in the conduit module.

## How — the canonical pattern

Every `n00b_alloc(T, ...)` for a type covered by the rule passes
`.allocator = c->allocator` (where `c` is the owning conduit), not
the default:

```c
n00b_quic_endpoint_t *ep = n00b_alloc_with_opts(n00b_quic_endpoint_t,
    &(n00b_alloc_opts_t){.allocator = c->allocator});
```

`c->allocator` is set by `n00b_conduit_new()` to a stable, non-moving
pool (`conduit_pool` on the runtime).  The same pool is used by every
internal conduit allocation that crosses thread boundaries.

For payloads published on a topic — including the per-datagram bytes
the UDP conduit hands up via `n00b_conduit_udp_datagram_t::bytes` —
the same allocator is used and the `.no_scan = true` flag is set when
the allocation is opaque bytes (no GC pointers inside):

```c
uint8_t *bytes = n00b_alloc_array_with_opts(uint8_t, len,
    &(n00b_alloc_opts_t){
        .allocator = u->conduit->allocator,
        .no_scan   = true,
    });
```

`.no_scan = true` tells the GC not to traverse the bytes looking for
pointers — there are none, and scanning a 1500-byte UDP payload
sixty thousand times a second is wasteful.

## Anti-patterns (DO NOT DO THESE)

```c
/* WRONG — default arena.  GC may relocate `ep`; picoquic crashes. */
n00b_quic_endpoint_t *ep = n00b_alloc(n00b_quic_endpoint_t);

/* WRONG — system_pool is a metadata pool, not a long-lived data pool.
 * Reserved for the runtime's own bookkeeping. */
n00b_quic_endpoint_t *ep = n00b_alloc_with_opts(n00b_quic_endpoint_t,
    &(n00b_alloc_opts_t){.allocator = &n00b_get_runtime()->system_pool});

/* WRONG — using the default arena for a UDP datagram payload.
 * The IO backend's read callback runs from a service thread; the GC
 * can't safely touch this from there. */
uint8_t *bytes = n00b_alloc_array(uint8_t, n);
```

## Detection

A pre-merge check (forthcoming, not in Phase 1 CI yet) will grep
`src/net/quic/` and `src/conduit/socket_udp.c` for `n00b_alloc(` /
`n00b_alloc_array(` calls without an explicit `.allocator =`
argument.  Any hit is a bug.  Today this is a manual code-review
check; treat it as binding.

## Exceptions

- Test code under `test/unit/` may use the default allocator for
  short-lived test scaffolding — tests live entirely on one thread
  and any GC activity is observable.  But do not deviate from the
  rule for anything that the test then registers with a topic /
  IO backend / picoquic / picotls.

- Stack-allocated buffers used purely as scratch for one syscall
  (e.g., the `uint8_t buf[65536]` in
  `n00b_conduit_udp_dispatch`'s recvfrom loop) do not allocate at
  all and so the rule does not apply.

- The opaque view types parsed out of buffers (e.g.,
  `n00b_quic_frame_t`'s borrowed `payload` slice pointer) are
  *borrowed views*, not allocations.  The rule says nothing about
  them.

## Cross-references

- `MEMORY.md → conduit_allocator_audit.md` — the post-mortem.
- `docs/conduit.md` — IO topology and inbox semantics.
- `~/dd/quic_1.md § 13` — Phase 1's statement of the rule.
- `include/core/alloc.h` — `_n00b_alloc_raw` and the `.allocator` /
  `.no_scan` / `.finalizer` kwargs.
- `include/core/runtime.h` — `n00b_runtime_t::conduit_pool`.
