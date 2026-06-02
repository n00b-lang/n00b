# Off-libc shutdown reconciliation (offlibc-tls → leakfix)

## Context

Prior-session work was split across two jj worktrees of the n00b repo, both
forking from PR #106 (`kmow 9d6ff400`):

- **`~/n00b-99-leakfix`** (this branch, = upstream main): carries PR #108
  `nmrn a45ce0a1` — "off-libc allocator migration + two-phase runtime init".
  The *startup* + allocator + foreign-thread half.
- **`~/n00b-offlibc-tls`** (`offlibc-tls-migration`, `zqkm 5bb55f33`): the
  *shutdown* + diagnostics half — "finish #104 off-libc thread_local
  migration + shutdown fix + restore n00b_lru + gc site-census".

Only #108 was merged to upstream. **Upstream is broken** because #108 changed
how the runtime / `n00b_thread_self()` resolve at exit, but the matching
shutdown-epilogue handling (change **B** below) lives only in `offlibc-tls`
and was never merged.

### Revisions
- FORK (PR #106): `kmow`
- LEAKFIX tip: `rnvz` (note: `rnvz` itself is an EMPTY no-op "#103 meson"
  rebase artifact on top of `#108 nmrn`)
- OFFLIBC tip: `zqkm` (bookmark `offlibc-tls-migration`)

### Root cause (verified in `src/core/init.c`)
LEAKFIX `n00b_shutdown(void)` (old form) does NOT clear `n00b_default_runtime`
and does NOT migrate the main thread's GC-frame chain to
`_n00b_bootstrap_thread`. After `n00b_shutdown()` returns, ncc auto-gc-roots
epilogues (`gc_stack_pop → n00b_thread_self`) still run in the caller and
dereference the now-dead runtime (CLI pattern: a returned stack-local
`n00b_runtime_t`) → **EXC_BAD_ACCESS on exit** + possible `gc.c` frame-chain
assert. OFFLIBC's `n00b_shutdown() _kargs` fixes both.

### Already shared — no action
Changes (A) string-scratch → `n00b_thread_t` and (C) restore `n00b_lru` are
**byte-identical** in both tips (`lru.c/h`, `string.c`, `time.c/h`). #108
already carries them.

---

## Tasks

### 1. Apply change (B): the shutdown fix  — CRITICAL (unbreaks upstream)
- [ ] `src/core/init.c`: replace `n00b_shutdown(void)` with offlibc's
      `n00b_shutdown() _kargs { .runtime }` body — add the `.runtime` param,
      the bootstrap-thread `gc_stack_top`/`gc_stack_policy` migration, and the
      `n00b_default_runtime = n00b_option_none(...)` clear at the end.
- [ ] `src/core/init.c`: add `n00b_shutdown_simple(void)` (plain-C AOT shim).
- [ ] `include/core/runtime.h`: take ONLY the `n00b_shutdown` `_kargs` decl +
      the `n00b_shutdown_simple` decl. **Do NOT** take runtime.h wholesale —
      keep leakfix's `live_slot_bits` / `callstack_base_set` /
      `foreign_reap_lock` fields (#108 work offlibc never had).

### 2. Reconcile AOT-shim callers (DIVERGENT-CONFLICT — by hand, not raw pick)
Offlibc renames `n00b_shutdown()` → `n00b_shutdown_simple()` (and drops the old
`extern`). Leakfix independently rewrote these files (`malloc→n00b_alloc`,
mmap-based reads). Orthogonal logically, conflicting textually — **keep
leakfix's allocator refactor, apply only the shutdown-call rename on top.**
- [ ] `src/tools/n00b.c`
- [ ] `src/tools/naudit-grammar-bake.c`
- [ ] `src/tools/n00b-static-init-helper.c`

### 3. Add regression tests + meson wiring
- [ ] `test/unit/test_string_scratch_raw_worker.c` (safe, additive).
- [ ] `test/unit/test_shutdown_gc_stack.c` — depends on task 1 (`.runtime`
      `_kargs` form); add only after task 1 lands.
- [ ] `meson.build`: add offlibc's two test targets; KEEP leakfix's existing
      `thread_self_foreign` + `thread_self_bench`. Additive, different sections.

### 4. Build + verify
- [ ] Build clean.
- [ ] Run unit suite. Offlibc reported "+8 vs clean #106, zero regressions";
      confirm no regressions on leakfix.

### 5. Audit before landing (per ~/CLAUDE.md, touches libn00b core)
- [ ] Route the change description through `prompt-auditor`.
- [ ] Then run `n00b-code-auditor`; address findings.

### 6. Housekeeping (independent of the merge)
- [ ] Abandon the empty `rnvz` "#103 meson" no-op commit.
- [ ] Commit the leakfix working-copy WIP separately (display-rewrite
      artifacts, naudit fixtures, `include/core/mmaps.h`,
      `src/display/render/backend_registry.c`, `.gitignore`) — unrelated to
      this reconciliation.

---

## DO NOT pick (regressions / leakfix already ahead)
- **`src/core/gc.c` site-census (D)** — leakfix already has a SUPERSET
  (live-vs-leaked + byte breakdown census) PLUS the #99 OOB-underflow /
  `mmap_static` SIGBUS guards. Offlibc's count-only census DROPS those guards.
  (`zqkm`'s message claims it preserved the #99 guards; the diff shows it did
  not — do not trust that claim.)
- **`thread.c` / `thread.h` / runtime.h foreign-thread fields / `init.c`
  early-init** — offlibc merely lacks #108's work; keep leakfix.
- **All LEAKFIX-AHEAD cleanups** (demangle, rpc, module_loader, display
  backends, repo hygiene: `.gitignore`/`.mcp.json`/`SKILL.md`/
  `SESSION_RECKONING.md`/`TODO.md`) — keep as-is.
