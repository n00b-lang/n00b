# N00b Project

## Overview

N00b is a systems programming project consisting of:
1. **ncc** — a C compiler wrapping an underlying C compiler (typically clang) that adds language extensions via AST transformations
2. **libn00b** — a runtime library providing type-safe generic data structures, memory management with garbage collection, and concurrency primitives

The project targets **C23** and uses ncc's extensions (typeid, keyword arguments, constexpr builtins, error propagation, etc.) throughout the runtime library headers.

## Current Status

The project is in active development, currently doing a **cherry-pick merge** of multiple implementations: porting a prototype rewrite back into the main codebase. The focus is on getting the memory allocator and GC working. Many components are being brought online incrementally — expect some files that don't yet compile or have incomplete implementations.

### Code migration

- When we are migrating code, put a heavy emphasis on minimal changes. A lot of the code is very robust, and we don't want to make unnecessary changes.
- Always add ample unit testing; that generally was not done.
- Always add Doxygen style documentation in headers, and make sure prototypes have explicit names on fixed parameters. Descriptive module overviews should also be produced if missing in the source material.

## Build System

**Never run `meson setup` directly on the top-level project.** The build enforces this.

### Build commands

```bash
# Full build (bootstrap compiler + main project):
bash build.sh

# Build and run all tests:
N00B_TEST=1 bash build.sh

# Clean rebuild:
N00B_CLEAN=1 bash build.sh

# Clean rebuild + test:
N00B_CLEAN=1 N00B_TEST=1 bash build.sh

# Force bootstrap rebuild:
N00B_BUILD_BOOTSTRAP=1 bash build.sh

# Force bootstrap rebuild + test:
N00B_CLEAN=1 N00B_BUILD_BOOTSTRAP=1 N00B_TEST=1 bash build.sh
```

### Build process

1. **Bootstrap phase**: Builds `ncc-bootstrap` in `bootstrap/build_bootstrap/`, installs to `bin/ncc-bootstrap`
2. **Main phase**: Uses `ncc-bootstrap` as `CC` to build `libn00b.a` and test executables in `build_debug/`

### Documentation

Unless otherwise asked, we should always:

- Produce Doxygen documentation inside the header files (only)
- Comment code that is not "obvious", without overcommenting.

For Doxygen comments:

- Make sure function keyword arguments are documented with: `@kw param_name description`
- Make sure module and function documentation documents its purpose, and relationships to other components.
-  ALWAYS omit trivial function descriptions (when it's self-evident / simple).
- When we do document, always be concise, yet complete. Use @details if merited.
- Add explicit preconditions and postconditions via @pre and @post wherever possible.
- Use markdown, under the assumption people will be consuming generated content, not direct comments.

*When asked to clean up documentation, look for violations of the above rules and fix them.*

### Running tests

**Always use `build.sh` to run tests:**
```bash
# Run all unit tests:
N00B_TEST=1 bash build.sh

# Run a specific test suite or test (after an initial build):
meson test -C build_debug --print-errorlogs                # all tests
meson test -C build_debug --print-errorlogs --suite unit    # unit suite only
meson test -C build_debug --print-errorlogs tuple           # single test by name
```

Tests are registered with Meson's native `test()` infrastructure in the `unit` suite, so `meson test` handles discovery, execution, and reporting.

### Building individual targets

After an initial `bash build.sh`, you can rebuild individual targets:
```bash
meson compile -C build_debug test_tuple
meson compile -C build_debug test_tree test_variant test_result
```

### Bootstrap compiler only

```bash
cd bootstrap
meson setup builddir      # first time only
meson compile -C builddir
```

### Build options (meson.options)

- `dev_mode` — enables `N00B_DEV` define for diagnostic routines
- `enable_debug` — enables `N00B_DEBUG` for debug logging
- `use_memcheck` — `off`/`on`/`strict` lightweight heap checks
- `use_asan` / `use_ubsan` — sanitizers
- `show_gc_stats` — GC statistics output
- `enable_lto` — link-time optimization

## Architecture

### Bootstrap Compiler (`bootstrap/`)

ncc is a source-to-source compiler. Pipeline:
1. Preprocess with underlying C compiler (`-E`)
2. Tokenize (`lex.c`)
3. Parse with packrat parser (`parse.c`, `parse_support.c`)
4. Apply semantic transforms (`xform_*.c`)
5. Emit transformed C code (`emit.c`)
6. Compile emitted code with underlying C compiler

Key files:
- `bootstrap/src/compile_packrat.c` — main compilation driver
- `bootstrap/src/parse.c` — grammar definition (packrat parser)
- `bootstrap/src/parse_support.c` — keyword registration, parser utilities
- `bootstrap/include/branch_symbols.h` — symbolic branch names for transforms
- `bootstrap/include/parse_internal.h` — parser internals, keyword externs
- `bootstrap/src/xform_*.c` — one file per language extension transform

### NCC Language Extensions

| Extension | Syntax | Purpose |
|-----------|--------|---------|
| `typeid()` | `typeid("prefix", T, ...)` | Generates unique identifier from type info (SHA256-based) |
| `typestr()` | `typestr(T)` | Produces string literal of encoded type ID |
| `constexpr_eval()` | `constexpr_eval(expr)` | Compile-time integer evaluation |
| `constexpr_max/min()` | `constexpr_max(a, b, ...)` | Compile-time min/max |
| `constexpr_paste()` | `constexpr_paste("prefix_", n)` | Compile-time identifier concatenation |
| `_kargs` | `void f(int x) _kargs { int opt = 0; };` | Keyword arguments with defaults |
| `!` (postfix) | `result_expr!` | Rust-like error propagation (early return) |
| `once` | `once void f() { ... }` | Thread-safe single initialization (should *not* be used in the code base) |
| `package` | `package name;` | Namespace/visibility control (should *not* be used a this time; needs to be rethought)|
| Variadic `+` | `void f(int, +)` | Variadic arguments, fully checked (including optional static typing) |

Many of the language extensions have evolved beyond the code bases we are merging. For example, you may see `keywords` blocks with a different syntax that need to move to `kargs`. In some code, variadics will be handled via macros; that needs to migrate to the new approach.

### Runtime Library (`include/core/`, `src/core/`)

Type-safe generic containers implemented via macros + `typeid()`:
- `n00b_list_t(T)` — growable list with spinlock
- `n00b_stack_t(T)` — LIFO stack
- `n00b_array_t(T)` — fixed-size array
- `n00b_tree_t(N, L)` — N-ary tree (node type, leaf type)
- `n00b_variant_t(T1, T2, ...)` — tagged union
- `n00b_tuple_decl(T1, T2, ...)` — type-safe tuples
- `n00b_option_t(T)` — optional values
- `n00b_result_t(T)` — result type with error propagation

Memory management: custom allocators, arena allocation, pool allocator, GC with stop-the-world collection.

Most of the code we are merging will need to be migrated to use these structures. Most code we are porting was previously not type safe, and was using `void *` based stuff. We need to 

## Coding Standards

### Warnings

- **All warnings should be fixed in the source code**, not suppressed via compiler flags or configuration. Do not add `-Wno-*` flags to `meson.build` or `build.sh` to hide warnings — fix the underlying code instead.
- The only acceptable warning suppressions in the build config are for warnings that are genuinely unavoidable (e.g., `-Wno-unused-parameter` for callback signatures imposed by an external API).

### Formatting

A `.clang-format` is provided. Key settings:
- **4-space indentation**, no tabs
- **Column limit: 96**
- **Return type on its own line** for top-level function definitions
- **Brace after function** on new line; control structures on same line (K&R variant)
- **Pointer alignment: right** (`char *x`, not `char* x`)
- **Align** consecutive assignments, declarations, macros, and bitfields across comments
- **Break before binary operators**

```c
static char *
my_function(int arg1, char *arg2)
{
    if (condition) {
        // ...
    }
    else {
        // ...
    }
}
```

### Naming Conventions

| Kind | Convention | Example |
|------|-----------|---------|
| Public functions | `n00b_` prefix, snake_case | `n00b_list_push()` |
| Internal functions | `_n00b_` prefix | `_n00b_alloc_raw()` |
| Public types | `n00b_*_t` suffix | `n00b_allocator_t` |
| Generic type macros | `n00b_*_t(T)` | `n00b_list_t(int)` |
| Public macros/constants | `N00B_` prefix, UPPER_SNAKE | `N00B_THREADS_MAX` |
| Function-like macros | `n00b_` prefix, snake_case | `n00b_alloc()` |
| Internal macros | `_n00b_` prefix | `_n00b_stack_lock()` |
| Block-local temp vars | `_bl_` prefix | `_bl_r`, `_bl_nc` |
| Files | snake_case | `memory_info.c` |

### Headers

- Use `#pragma once` (never `#ifndef` guards)
- Doxygen-style documentation: `/** @file`, `@brief`, `@param`, `@return`, `@code`/`@endcode`
- Section separators: `// ====...====` (full-width)
- Include `"n00b.h"` first in implementation files for forward typedefs

### Memory Management

- **Always use the highest-level allocation API available.** Never call `_n00b_alloc_raw()` directly — it is an internal implementation detail. Use the public macros instead:
  - `n00b_alloc(T, ...)` — allocate a single `T`
  - `n00b_alloc_array(T, n, ...)` — allocate an array of `n` elements of type `T`
  - `n00b_alloc_flex(T1, T2, n, ...)` — allocate a struct `T1` with a trailing flexible array of `n` `T2` elements
  - `n00b_alloc_size(n, sz, ...)` — raw size allocation (only when no concrete type is available)
- Use `n00b_free(ptr)` for deallocation
- Keyword args for allocator options: `.allocator = x`, `.no_scan = true`
- Every allocation call embeds `__FILE__:__LINE__` via `N00B_LOC_STRING()`

This principle applies broadly: always prefer public/high-level APIs over internal `_n00b_`-prefixed functions. The `_n00b_` prefix signals an internal entry point that should only be called by the macro/wrapper layer above it, not by application or library code. The high-level macros provide type names, source locations, and correct argument marshaling automatically.

### Error Handling

- Prefer `n00b_result_t(T)` for fallible operations
- Use `n00b_result_ok(T, val)` and `n00b_result_err(T, err)` constructors
- POSIX wrappers: `n00b_check_posix()`, `n00b_check_mmap()`, `n00b_check_sysconf()`
- `n00b_option_t(T)` for nullable/absent values

### Generic Container Pattern

All generic containers follow the same template:
```c
// In header:
n00b_stack_decl(int);                          // Declare the struct
n00b_stack_t(int) s = n00b_stack_new(int);     // Instantiate
n00b_stack_push(s, 42);                        // Use
n00b_stack_free(s);                            // Free
```

The `*_decl()` macro expands a struct with a `typeid()`-based tag. Each unique type combination gets its own struct — this is how type safety is enforced without true generics.

## Testing

**Run tests via:** `N00B_TEST=1 bash build.sh`

Tests live in `test/unit/`. Each test is a standalone executable linked against `libn00b.a` and registered with `test()` in `meson.build` under the `unit` suite.

### Adding a new test

1. Create `test/unit/test_<name>.c`
2. Add to `meson.build`:
```meson
<name>_test = executable('test_<name>',
    ['test/unit/test_<name>.c'],
    kwargs: test_common_kwargs,
)
test('<name>', <name>_test, suite: 'unit')
```
3. Verify: `N00B_TEST=1 bash build.sh`

### Test file pattern

```c
#include <stdio.h>
#include <assert.h>
#include "n00b.h"
#include "core/alloc.h"
// ... other includes

static void test_something(void) {
    // setup, assert, teardown
    printf("  [PASS] something\n");
}

int main(int argc, char **argv) {
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);
    printf("Running foo tests...\n");
    test_something();
    printf("All foo tests passed.\n");
    return 0;
}
```

Tests use `assert()` + printf for reporting. No external test framework.

### Current tests (unit suite)

`list`, `stack`, `result`, `variant`, `tuple`, `tree`

## Adding NCC Extensions (Bootstrap Compiler)

To add a new transform to ncc:

1. **Register keyword** in `bootstrap/src/parse_support.c` (`str_list(...)`)
2. **Declare extern** in `bootstrap/include/parse_internal.h`
3. **Add branch symbol** in `bootstrap/include/branch_symbols.h` (if needed)
4. **Add parser rule** in `bootstrap/src/parse.c` (update `declare_nt` count, add `nt_branch`)
5. **Create transform** in `bootstrap/src/xform_<name>.c` (register post/pre-order on target NT)
6. **Register transform** in `bootstrap/src/compile_packrat.c` (extern + call in init)
7. **Add to build** in `bootstrap/meson.build` (`bootstrap_files` list)

## Gotchas

- The top-level `meson.build` refuses to run without `build.sh` (`using_build_script` option). This is intentional to help us remember the blessed build path.
- ncc-bootstrap must be rebuilt and reinstalled (`bash build.sh` with `N00B_BUILD_BOOTSTRAP=1`) after any changes to `bootstrap/`
- `typeid()` produces SHA256-based identifiers — struct names in error messages will be long hex strings
- Macro counter expressions from `N00B_MAP_COUNT` are parenthesized: `(0)`, `((0) + 1)`, etc. — transforms that evaluate these must be able to accept nested constexpr_eval().
- `.clang-format` is configured but ncc extensions may confuse clang-format — don't auto-format ncc-extended headers.
- The project uses jj (Jujutsu) alongside git; *NEVER* use git commands directly without direction from the user.
