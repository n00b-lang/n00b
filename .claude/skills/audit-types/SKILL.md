---
name: audit-types
description: Audit bare pointers and sentinel returns against n00b_option_t/n00b_result_t conventions. Also identifies option_t uses that should be result_t for better error propagation with the ! operator. Additionally audits bare `char *` usage (should be `n00b_string_t` or `n00b_buffer_t *`) and bare array patterns (should be `n00b_list_t` or `n00b_array_t`).
---

# Type Safety Audit

Audit the files or directories specified in `$ARGUMENTS` for violations of the
project's type-safety conventions (see `docs/best_practices.md` Error Handling)
and data type conventions (see `include/core/string.h`, `include/core/buffer.h`,
`include/core/list.h`, and `include/core/array.h`).

If no arguments are given, ask the user which area to audit.

## Rules

Read `docs/best_practices.md` (the Error Handling section) and `include/core/option.h`
and `include/core/result.h` before starting, to refresh on the exact API surface.
Also read `include/core/string.h`, `include/core/buffer.h`, `include/core/list.h`,
and `include/core/array.h` for the replacement types used in categories 7–8.

### 1. Bare pointer returns that should use `n00b_result_t(T *)`

A function returning `T *` where `nullptr` means **an error occurred** must
return `n00b_result_t(T *)` instead. The error branch carries an `n00b_err_t`
(int, typically errno or a project error enum).

**How to detect:** Look for patterns like:
- `return nullptr;` or `return NULL;` in error paths (bad args, alloc failure, syscall failure)
- Call sites doing `if (!ptr)` or `if (ptr == NULL)` and treating it as an error
- Functions whose doc/comments mention "returns NULL on failure/error"

### 2. Bare pointer returns that should use `n00b_option_t(T *)`

A function returning `T *` where `nullptr` means **"not found" / "absent"**
(but that's a normal, non-error outcome) must return `n00b_option_t(T *)`.

**How to detect:** Look for patterns like:
- Lookup/search functions returning nullptr for "not found"
- Call sites doing `if (ptr)` as a presence check, not an error check
- Functions whose doc mentions "returns NULL if not found/absent"

### 3. Sentinel int returns that should use `n00b_result_t(int)`

A function returning `int` where `-1` (or `0`) is a sentinel for failure must
return `n00b_result_t(int)`.

**How to detect:**
- `return -1;` in error paths
- Call sites doing `if (ret < 0)` or `if (ret == -1)`

### 4. Sentinel int returns that should use `n00b_option_t(int)` or similar

A function returning `int` / `int32_t` / `size_t` where `-1` or a magic value
means "absent" (not an error) should use the appropriate `n00b_option_t`.

### 5. `n00b_option_t` that should be `n00b_result_t` (upgrade opportunities)

This is the **key nuance**: some functions currently return `n00b_option_t`
but the "none" case actually represents a failure condition — not mere absence.
These should be `n00b_result_t` because:

- **Error propagation with `!`**: The ncc postfix `!` operator (like Rust's `?`)
  only works with `n00b_result_t`. It unwraps on success or returns the error
  early. This enables clean, composable error handling chains.
- **Error information**: `n00b_result_t` carries an `n00b_err_t` (errno) that
  tells the caller *why* it failed. `n00b_option_t` just says "absent".

**The distinction:**
- `n00b_option_t` = "this value might not exist, and that's fine"
  - Example: `n00b_type_lookup()` — looking up a type that may not be registered
  - Example: `n00b_conduit_fd_get_owner()` — FD might not be managed
- `n00b_result_t` = "this operation can fail, and the caller needs to know why"
  - Example: `n00b_conduit_io_kqueue_ops()` — not "absent", it's "not supported
    on this platform" (ENOTSUP), and callers need to fall back or propagate
  - Example: `n00b_thread_spawn()` — failure has specific errno (ENOMEM, ENXIO)

**How to detect option->result upgrades:**
- `n00b_option_t` returns where "none" represents a platform limitation, resource
  exhaustion, or any condition the caller would want to distinguish from "just
  not there"
- Call sites that check `n00b_option_is_set()` and then have error handling /
  fallback logic (not just "skip if absent")
- Functions where converting to `n00b_result_t` would let callers use `!` for
  cleaner propagation, especially in chains of fallible calls
- Functions where the "none" case could carry useful diagnostic info (which
  errno? which subsystem failed?)

### 6. Unchecked `n00b_result_get()` / `n00b_option_get()` calls

These crash on error/none (null-pointer dereference by design). Every call
must be preceded by an `is_ok` / `is_set` check, or use `get_or_else`, or
use `!` for propagation. Look for:
- `n00b_result_get(some_call(...))` — no intermediate check
- `n00b_option_get(x)` where `x` was never tested with `n00b_option_is_set`

### 7. Bare `char *` that should be `n00b_string_t` or `n00b_buffer_t *`

Internal function boundaries and data structures must not use bare `char *`.
Use `n00b_string_t` (immutable UTF-8 text, passed by value, 40 bytes) for
human-readable strings, or `n00b_buffer_t *` (mutable, growable, heap-allocated)
for binary/mutable byte data. Only FFI/POSIX boundaries are exempt.

**Heuristic for string vs buffer:**
- Text, human-readable, immutable, styled, or display content → `n00b_string_t`
- Binary data, mutable, growable, accumulation buffers → `n00b_buffer_t *`
- When ambiguous, flag for user decision with both options noted

**How to detect:**
- `char *` or `const char *` parameters on internal (non-FFI) functions
- `char *` return types from allocating functions (should return
  `n00b_string_t` or `n00b_buffer_t *`)
- `char *` struct fields, especially with a paired length field (e.g.,
  `char *name; size_t name_len;` → single `n00b_string_t name;`)
- `char **` (array of strings) → should be `n00b_list_t(n00b_string_t)`
  or `n00b_array_t(n00b_string_t)` (see also category 8)
- Separate `char *data, size_t len` parameter pairs → single
  `n00b_string_t` or `n00b_buffer_t *` parameter
- String literal keys and debug names used purely internally

**Exceptions (do NOT flag):**
- FFI/POSIX boundary calls: file paths passed to `open()`/`stat()`/`fopen()`,
  format strings for `printf()`/`snprintf()`, arguments to system calls
- The return value of `n00b_unicode_str_to_cstr()` and similar conversion
  outputs that exist specifically for FFI handoff
- `const char *` parameters on functions whose sole purpose is constructing
  an `n00b_string_t` (e.g., `n00b_string_from_raw()`)

**Key type details for the report:**
- `n00b_string_t` is by-value: `{ char *data; size_t u8_bytes; size_t codepoints; void *styling; }`
- `n00b_buffer_t *` is by-pointer: `{ char *data; size_t byte_len; size_t alloc_len; n00b_rwlock_t *lock; ... }`
- Conversion helpers: `n00b_string_from_raw()` (C string → n00b_string_t),
  `n00b_buffer_to_string()` (buffer → string), `n00b_unicode_str_to_cstr()`
  (string → C string for FFI)
- When migrating `char *` code, replace manual string operations (`strlen`,
  `strcmp`, `strcpy`, `strstr`, `strtok`, `memcpy` on text, etc.) with the
  corresponding `n00b_string_t` / unicode library operations (see
  `include/core/string.h`, `include/strings/string_ops.h`,
  `include/unicode/query.h`). Do not wrap bare C string functions around
  `n00b_string_t` internals.

### 8. Bare arrays that should be `n00b_list_t(T)` or `n00b_array_t(T)`

Do not use bare C arrays or manual `T *` + length pairs for dynamic collections.
Use `n00b_list_t(T)` (locked by default, for shared/multi-threaded access) or
`n00b_array_t(T)` (unlocked by default, single-owner, performance-critical).

**Heuristic for list vs array:**
- Shared across threads, concurrent access, push/pop/insert/delete → `n00b_list_t(T)`
- Single-owner, private, fixed-capacity indexed access → `n00b_array_t(T)`
- When ambiguous, default to `n00b_list_t(T)` (safer)

**How to detect:**
- `T *data` + `size_t`/`int` `len`/`count`/`num`/`size` parameter pairs →
  single `n00b_list_t(T)` or `n00b_array_t(T)` parameter
- `T *data` + `size_t`/`int` `len`/`count`/`num`/`size` struct field pairs →
  replace with container field
- `T arr[N]` where N > 8 in struct definitions → `n00b_array_t(T)`
- `malloc(sizeof(T) * n)` or `calloc(n, sizeof(T))` patterns → should use
  `n00b_list_new(T)` / `n00b_array_new(T, n)` constructors
- `T **` double pointers used as arrays of pointers

**Exceptions (do NOT flag):**
- Small fixed-size arrays (≤8 elements) in structs, e.g., `int fds[2]`
- Compound literals used as temporary arrays with stack duration
- Flexible array members (FAMs) in internal allocation structs
- FFI boundaries where C libraries require bare array parameters
- `n00b_vargs_t` — the project's own checked variadic mechanism

**Migration note:** When replacing bare arrays, use the `n00b_list_t` /
`n00b_array_t` APIs for all element access, iteration, insertion, removal,
sorting, and searching (see `include/core/list.h`, `include/core/array.h`).
Do not index into container internals directly or use bare C patterns like
manual `realloc` + `memcpy` for growth.

## Procedure

1. **Scan** the target files for function signatures returning bare `T *`,
   `int`, or other sentinel-bearing types. Also scan for existing
   `n00b_option_t` returns. Additionally scan for `char *` / `const char *`
   parameters, return types, and struct fields (category 7), and for `T *` +
   length pairs, large fixed arrays, and `malloc`/`calloc` patterns (category 8).

2. **Classify** each finding into one of the 8 categories above. For categories
   7–8, note the heuristic sub-classification (string vs buffer, list vs array)
   and flag ambiguous cases for user decision.

3. **Check call sites** for each function — use Grep/LSP to find all callers.
   This determines impact and helps verify the classification.

4. **Check for missing `n00b_result_decl(T)` / `n00b_option_decl(T)`**. If the
   type hasn't been declared yet, note which header should get the decl.
   Common decls live in `include/core/option.h` and `include/core/result.h`.
   Module-specific decls go in the module's header.

5. **Report** findings grouped by file, with:
   - Function name and current signature
   - Category (1-8 from above)
   - Recommended new return type
   - All call sites that need updating
   - Whether a new `n00b_result_decl` / `n00b_option_decl` is needed
   - For category 5 (option->result upgrades): explain *why* result is better
     (what error info would be carried, whether `!` propagation applies)
   - For category 7: which replacement type (`n00b_string_t` or `n00b_buffer_t *`)
     and the conversion function needed at FFI boundaries
   - For category 8: which container (`n00b_list_t(T)` or `n00b_array_t(T)`),
     whether locking is needed, and whether `n00b_list_decl(T)` /
     `n00b_array_decl(T)` for that element type exists or must be added

6. **Summarize** with counts per category and a suggested implementation order
   (dependency-aware: change leaf functions first, then callers).

## Output format

```
## Findings: <file>

### <function_name> — Category <N>: <short description>

**Current:** `T * function_name(...)`
**Proposed:** `n00b_result_t(T *) function_name(...)`
**New decl needed:** `n00b_result_decl(T *)` in `include/<module>.h`
**Call sites:**
- `src/foo.c:123` — `if (!ptr)` error check
- `test/unit/test_foo.c:45` — `assert(ptr != nullptr)`
**Rationale:** <why this category, what error info to carry>

---
```

Example for categories 7–8:

```
### some_function — Category 7: bare char * → n00b_string_t

**Current:** `void some_function(const char *name, size_t name_len)`
**Proposed:** `void some_function(n00b_string_t name)`
**FFI conversion:** Use `n00b_unicode_str_to_cstr()` at call sites passing to POSIX APIs
**Call sites:**
- `src/core/init.c:42` — passes string literal, use `n00b_string_from_raw()`
- `src/core/config.c:87` — passes `char *` from struct field (also needs migration)
**Rationale:** Paired char*/length params; text content (human-readable name). Strong signal.

---

### my_struct_t — Category 8: bare T* + count → n00b_array_t

**Current (struct fields):**
  `int *values; size_t num_values;`
**Proposed:**
  `n00b_array_t(int) values;`
**Decl needed:** Verify `n00b_array_decl(int)` exists; if not, add to `include/core/array.h`
**Note:** Struct size changes from 16 bytes to sizeof(n00b_array_t) — public API impact
**Call sites:**
- `src/foo.c:200` — `malloc(sizeof(int) * n)` → `n00b_array_new(int, n)`
- `src/foo.c:215` — `s->values[i]` → `n00b_array_get(s->values, i)`
**Rationale:** Dynamic allocation with paired length field; single-owner (private struct),
  so array_t preferred over list_t. No locking needed.

---
```

## Important notes

- **Do NOT make changes.** This skill is audit-only. Report findings for the
  user to review before any code is modified.
- Be thorough: use Grep to find ALL call sites, not just obvious ones.
- Pay attention to functions called from macros or inline functions in headers.
- For conduit code, check both the real implementation and platform-absent stubs.
- Consider the `!` operator: if converting to `n00b_result_t` would let a
  caller collapse 4 lines of error checking into `auto x = foo()!;`, note that
  as a benefit.
- Look at the existing plan file if present (`.claude/plans/`) for context on
  what has already been audited and fixed.
- For category 7: check whether the `char *` crosses an FFI boundary before
  flagging. The test: does the `char *` stay within n00b code, or does it exit
  to C stdlib/OS/third-party?
- For category 7: paired `char *` + `size_t` length fields are the strongest
  signal for replacement. Lone `char *` without length should still be flagged
  but are lower priority.
- For categories 7–8: findings often overlap. A `char **argv` is both category
  7 and 8. Report under whichever is more actionable (typically 7, recommending
  `n00b_list_t(n00b_string_t)`), and cross-reference the other.
- For category 8: `n00b_list_t` and `n00b_array_t` are value types (structs).
  Replacing `T *values; size_t count;` with `n00b_list_t(T) values;` changes
  struct size — note this when the struct is part of a public API.
