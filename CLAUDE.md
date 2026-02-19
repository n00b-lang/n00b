# N00b Project

## Overview

N00b is a systems programming project consisting of:
1. **ncc** — a C compiler wrapping clang that adds language extensions via AST transformations
2. **libn00b** — a runtime library providing type-safe generic data structures, memory management with garbage collection, and concurrency primitives

The project targets **C23** and uses ncc's extensions throughout.

## Key documentation

Read the relevant doc before starting work in an area:

- **`docs/best_practices.md`** — Coding standards, C23 idioms, memory ownership, API design, error handling, testing, ncc extension usage, build options
- **`docs/ncc.md`** — Compiler architecture, language extensions reference, parser internals, adding new extensions
- **`docs/strings.md`** — String library: `n00b_string_t`, styling, rich markup, formatting
- **`docs/unicode.md`** — Unicode library: encoding, properties, algorithms, iteration

## Current status

The project is in active development, doing a **cherry-pick merge** of multiple implementations: porting a prototype rewrite back into the main codebase. Many components are being brought online incrementally — expect some files that don't yet compile or have incomplete implementations.

### Code migration rules

- **Minimal changes.** A lot of the code is very robust; don't make unnecessary changes.
- **Always add unit tests.** The prior implementations generally lacked them.
- **Always add Doxygen documentation in headers** (not source files). Make sure prototypes have explicit parameter names. Produce descriptive module overviews if missing.

## Build system

**Never run `meson setup` directly on the top-level project.** The build enforces this.

```bash
bash build.sh                                                   # full build
N00B_TEST=1 bash build.sh                                       # build + test
N00B_CLEAN=1 N00B_TEST=1 bash build.sh                          # clean rebuild + test
N00B_BUILD_BOOTSTRAP=1 bash build.sh                             # force bootstrap rebuild
N00B_CLEAN=1 N00B_BUILD_BOOTSTRAP=1 N00B_TEST=1 bash build.sh   # clean all + test
```

After an initial build:
```bash
meson compile -C build_debug test_tuple                  # rebuild one target
meson test -C build_debug --print-errorlogs tuple        # run one test
meson test -C build_debug --print-errorlogs --suite unit # run unit suite
```

See `docs/best_practices.md` § Building for build options and bootstrap-only builds.

## Documentation rules

- Doxygen comments go in **header files only**, not source files.
- Use `@kw param_name description` for keyword arguments (not `@param`).
- Omit trivial function descriptions — only document when it adds value.
- Be concise yet complete. Use `@details` if merited.
- Add `@pre` / `@post` wherever meaningful.
- Use markdown in comments (readers consume generated output, not raw comments).
- *When asked to clean up documentation, look for violations of these rules and fix them.*

## Testing

**Run tests via:** `N00B_TEST=1 bash build.sh`

To add a new test:
1. Create `test/unit/test_<name>.c` (see pattern in `docs/best_practices.md` § Testing)
2. Add executable + test entry in `meson.build` (use `test_common_kwargs`)
3. Verify: `N00B_TEST=1 bash build.sh`

## Gotchas

- **Never run `meson setup` directly** on the top-level project — use `build.sh`.
- **Rebuild bootstrap after changes** to `bootstrap/`: `N00B_BUILD_BOOTSTRAP=1 bash build.sh`
- **`typeid()` produces SHA256-based identifiers** — struct names in error messages will be long hex strings.
- **Macro counter expressions are parenthesized** — `(0)`, `((0) + 1)`, etc. Transforms evaluating these must handle nested `constexpr_eval()`.
- **Don't auto-format ncc-extended headers** — `.clang-format` doesn't understand ncc syntax.
- The project uses **jj (Jujutsu)** alongside git — *NEVER* use git commands directly without direction from the user.
