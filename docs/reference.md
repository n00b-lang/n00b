v# The n00b Language Reference
v0.1 (Draft in progress)
Oct 28, 2026

# Introduction

n00b is a modern, statically typed language, focused on being beginner friendly, meaning both easy to read, and easy to write. As we expect most code in the future will be written by agents, we strive to be as useful to humans as possible by:

1. Being incredibly easy to read and write, at any skill level.
2. Providing thorough static checking for types, while allowing custom error checking rules that leverage modern static analysis.
3. Encouraging use of the 'best' tool for the job -- tooling to facilitate better cross-language interoperability, and make it easy to embed DSLs.

In short, n00b aims to be, in a sense, a spiritual successor to Python -- the glue language of its era, focusing on the overall user experience when people need to look over their agents' shoulder, or wish to incrementally build expertise.

This document is the language reference for n00b. It informally specifies the language's syntax and semantics, including its lexical rules, type system, required static checks, and runtime behavior.

Note that the source distribution contains definition files for the [lexer](../grammar/n00b.lex) and the [parser](../grammar/n00b.bnf) that are used programatically to implement the front end. It is the definitive source for syntactic correctness; this document covers the semantics. Currently, we're not embedding the grammar here, to avoid maintainance issues while the language is still in early development.

Currently, the only reference implementation is the distribution n00b compiler. The project plans to extend it build a separate REPL-based implementation, add backends targeting C and WASM (the current backend lowers to LLVM MIR), and eventually rewrite the compiler in pure-n00b compiler.

All semantics in this document should apply to all of those implementations, though as we build the REPL, there may be some unavoidable emergent differences. If so, we will document them here.

For convenience, I will often use **compile time** to refer generally to mean "when processing the code in preparation for first execution". That kind of phrasing is meant to cover any REPL or other interpreter; again, exceptions will be explicit.

## Conventions and Notation

Language keywords and source fragments appear in `monospace`. Text is meant to describe expected semantics; deviations from what's read should be assumed to be a bug, most likely in the implementation, but possibly in the specification.

The words **must** and **must not** state requirements. **May** describes permitted behavior. Recommendations and implementation choices are identified as such and do not impose a conformance requirement.

If actual behavior differs, it should be considered a bug; please let me know about it.

Similarly, if something important is missing from this spec, it should NOT be considered an implementation-defined behavior. Instead, consider it a bug and let me know.

## High-level language overview

Strong static type checking helps create reliable programs. At the same time, we know that developers often would prefer to not have to write types when writing code themselves. This is especially true for beginners, where the cognitive load involved with learning to program is already high.

n00b addresses those needs by performing full-module, bi-directional type inferencing, to minimize the situations where types need to be explicitly written. Additionally, n00b tooling is strongly encouraged to normalize code, in part by explicitly adding type (and variable) declarations, to document the language's view of the code.

This applies to the compiler too-- by default, we will re-write source files to add types.

Philisophically, n00b wants everyone to be able to read code, and be able to build expertise. So we try to be as permissive where it's possible to do so without ambiguity or confusion, and then rely on tooling to normalize for consistency.

The language aims to make it easy to get performant, robust software, but allows for flexibility. For instance, the language provides its own standard library that comes with full garbage collection. However, it's possible to run without the standard library, or to use other memory managers to forgo the garbage collector.

To best support all those use cases, without putting a huge management burden on users who don't want it, n00b aggressively analyzes code, and generally will manage storage in the way it thinks best, until an explicit allocator is provided. The language will ensure deallocation automatically when it can prove lifetime through static analysis.

And for cases where it cannot, the standard library is written with explicit calls to a built-in `free()` function that gets transparently routed to a handler for whatever allocator was used. Users whose applications rely solely on garbage collection need not ever explicitly deallocate memory.

The standard library is outside the scope of this reference, as is tool-specific documentation (including the command line).

## Hello World

The `builtin` package is automatically available, so the following works:

```n00b
print("Hello, world!")
```

```bash
n00b build hello.n
./hello
Hello, world!
```

Strings allow for substitution via the `format` call:

```n00b
word = "again"
version = 2

print("Hello, [#] (version number [#])".format(word, version))
```

```bash
n00b build hello.n
./hello
Hello, again (version number 2)
```

The compiler should automatically transform your source code, however, to the following:

```n00b
var word:    string = "again"
var version: u8     = 2

print("Hello, [#] (version number [#])".format(word, version))
```

Or, we could do this without formatting:
```n00b
version = 3
print("Hello world, (version number ", version, ")")
```
# Build and execution

Here's a rough overview of the process for building (compiling) a program:

- Source files are *tokenized* -- broken into discrete units (called tokens); e.g., operators, integers, strings, comments, floats, identifiers, whitespace, and so on.
- Source files are then *parsed* -- The parser translates tokens into a tree that represents an unchecked module. At this point, if syntactic rules were broken, n00b will produce errors.
- The [*n00b preprocessor*](#The Preprocessor) gathers basic facts needed to perform analysis, and does some basic substitutions.
- The *initial checking* pass does a first set of semantic checks, as it takes the parse tree, and converts it to a n00b-specific intermediate representation (NIR)-- a graph-based representation of the program.
- The NIR lowering pass does full semantic checking on a module, including how it uses its dependencies.
- The *backend lowering* pass generates code to pass off to a backend to finish the compilation process. Currently, the only backend is LLVM bitcode.
- [Static pre-initialization](#Static pre-initialization) does initial folding and literal caching.
- [*Comptime execution*](#Comptime execution) runs on modules, to let the user precompute data.
- *Object code generation* produces per-module object files, and caches information for [incrmental compilation](#Incremental Compilation).
- *Program linking* assembles a final binary from object code, possibly performing [link-time optimizations](#Link Time Optimization).

While n00b currently is a proper compiler that generates machine code, there are plans to also produde a *REPL* (read-evaluate-print loop)-- an interactive version of the language. The REPL will still leverage fully compiled modules, but the user-facing interactive code will use a JIT backend based on MIR (a well known JIT backend).

Eventually, we'd like to also have a WASM backend, and a pure C backend (mainly for an eventual re-implemented n00b-native compiler, so that we can generate a portable bootstrap compiler).

Lexical tokens have a programatic specification in [n00b.lex](../grammars/n00b.lex).
The n00b syntax is defined in [n00b.bnf](../grammars/n00b.bnf).

## The Preprocessor
The n00b preprocessor is very different from the C preprocessor:

- The n00b preprocessor is integrated into the language syntax, instead of being its own language.
- It requires all code to be syntacticcally valid (C does not require this for code that is conditionallly compiled out of the program).
- It does not support macro definition.

Here's what the preprocessor does:.
1. Evaluates a subset of constant expressions (see below)
2. Evaluates *preprocessor builtin* variables and functions, like `debug_enabled` and `preproc_env()`.
3. Resolves an object's [`import`ed](#import Statement) dependencies, making sure they've been processed enough to support module analysis.
4. Detects inconsistent use of [syntax skins](#Syntax Skins).
5. Flags code that needs parentheses (i.e., violates our [operator precedence](#Operator Precedence) rules).
6. Evaluates compile-time conditional blocks, which start with a [`when` statement](#when Statement).
7. For blocks of code where conditional evaluation did NOT select a branch, removes blocks of code from the parse.

Consider the following code:
```n00b
const egg_value = "Deltron3030"

when preproc_defines("APP_VERSION") and preproc_value("APP_VERSION") == egg_value {
  print("Rap battle easter egg enabled!")
}
else {
  print("Welcome!")
}
```

If you place it in `example.n` and just run it like this:
```bash
n00b run example.n
```

You'll get:
```bash
Welcome!
```

But, running as such:
```bash
n00b run --DEFINE APP_VERSION=Deltron3030 example.n
```

Will give you:
```bash
Rap battle easter egg enabled!
```

Preprocessor conditionals can only evaluate simple expressions:
1. They cannot call function calls, except for preprocessor-builtin functions.
2. They cannot use variables in the source text unless they are similarly simple, and defined in the same module.
3. They can only use values of type `int`, `string`, `bool` and `float`.

Preprocessor definitions are expected to be valid literals, without any literal modifiers, and are parsed and typed based on that.

A complete list of preprocessor builtins is available in the appendix.

## Static pre-initialization

Before full compile time execution, the static initialization phase does the following:
1. Integer folding.
2. Literal "pre-baking". We pre-lay out template representations for strings and container literals.
3. For modified literals, the appropriate `litmod` handler may run.

`litmod` handlers only run for container types statically if all their component items are themselves statically available literals; otherwise, the handler is called when needed.

## Comptime execution

n00b has full support for compile-time execution. You could even write a program that is a web server throughout compilation, and then, on shutdown, either outputs a binary that does nothing, or 'fail' the compilation.

That's not the kind of thing comptime is intended for, but it should hint at the power. You can do whatever work you want, even if it's side effecting.

When developing library code, you should not assume that `comptime` execution will happen, particularly because it's possible to skip compile time excecution with the `--no-comptime` flag.

Comptime execution runs in import order. With incremental compilation, dependent module comptime initialization can get cached.

When building an executable, once any comptime module initializers run, if the top module has a `comptime_main()` entry point defined, it then runs.

There is an additional mechanism for separating `comptime` code from runtime that works in module initializers, the builtin predicate `comptime() -> bool`. It returns `true`; it returns `false` during runtime.

This allows you to do things like:

```n00b
unicode_cache : Dict[char, Unicode_Properties]

if comptime() {
   let unicode_db = download_unicode_db()
   unicode_cache  = compute_unicode_character_info_cache()
}
```

Obviously that's pseudo-code, but at comptime, the downloading and precomputation would happen, and pre-bake the cache into the binary.

And then, with link-time optimization on, `download_unicode_db()` and `compute_unicode_character_info_cache()` would get compiled out of the binary (assuming they're not used anywhere else).

If `comptime` execution exits normally, then comptime "succeeds". Any module-level variables that were set at compile time that may be used at runtime are stored as starting values.

Note that, for variables marked as thread-local, the starting values are taken from the main thread's value, and are available for any thread that launches.

## Modules and Packages
Source code lives in modules; modules can live inside a package. Packages may have sub-packages, which must be imported seprately from their package.

System packages get installed with a tool chain, but additional items can be added to the package search path, by using the `N00B_PATH` environment variable.

Directories inside `N00B_PATH` should be separated by `:` on all platforms except Windows, where `;` is used.

Packages can be imported as symbols via the [`import` statement](#import Statement), or their symbols can be directly adopted with the [`bring` statement](#bring Statement).

Package definition is done via an export specification. While you may customize the rules with a [`package` statement](#package Statement), the default rules are simple: public functions, types and const variables are exported (visible names). Nothing else is visible.

The compiler treats the `builtin` module specially-- any of its public contents are automatically visible, as if the module imported the symbol directly in the namespace. All other modules require explicit importing to use.

## Program runtime entry points
When running a program, module initializers always run first in import order, ending with the entry-point module's initializer. After all initializers run, if a `main()` is defined inside that module, then that will be run. Otherwise, the program exits.

Using `main()` is optional-- if it does not exist, the entry-point module's initializer is simply considered the main body of the program.

In Python, there's a common idiom for a module to behave differently based on whether it's being used as a program entry point, instead of as a module... comparing against the module name, which gets a special value inside the entry point module.

For `n00b`, the correct idiom is to use `main()` entry point for command-specific items.

## Incremental Compilation
n00b supports incremental compilation. That means, as you make changes to a code base, n00b doesn't have to re-process modules your changed code is dependent on. Changed modules get fully reprocessed. Modules that haven't changed, but are dependent on changed code, do need partial re-processing.

Incremental compilation is important for n00b, because it peforms a lot of code analysis at compile time, and it helps tremendously when we can avoid redoing analysis.

To support incremental compilation, n00b retains a *compilation cache*, which keeps per-module code and metadata and code. Modules avoid re-processing costs unless one of the following happens:

- The source code changes or moves.
- The interfaces the module uses change.
- Source code for inlined functions used in the module change.
- The compiler version is different.
- Different build flags are used.
- The target platform is different.

Module cycles are cached as a single module. Multiple cached copies can be kept, generally for different sets of build flags.

The following environment variables control the cache:

| Variable | Effect |
| N00B_CACHE=<dir> | Specify the cache location; default is toolchain specific |
| N00B_NO_OBJECT_CACHE=1 | Disable the object cache |

## Link Time Optimization
When creating a final executable, n00b supports link-time optimization, when supported by the underlying compiler back-end (right now, LLVM, which does support it). That means, the compiler performs additional anlysis, leveraging cross-module knowedge to improve performance and remove unnecessary code.

For instance, additional inlining may be performed.

Code that does dynamic module loading may not work with link-time optimizations on.

This analysis does increase compilation time. By default, LTO happens for release builds (`--release`). You can disable it in release builds with `--no-lto`. To enable it for another build, use the `--lto` flag.

## Program startup
Module initializers consist of the code outside of functions. Such code is meant to be run once, and only once, in any execution, on first import.

When a n00b program starts, module initializers run first, based on *import order*, which is the lexical order in which modules are accessed. Even when a module isn't fully imported (e.g., with a `bring` statement), the initializer still runs.

Excepting `when` statements, `import` and `bring` must appear in top-level code. The order in which they appear after conditional compilation defines the *import order* for that module.

Modules may be cyclicly imported, but import order for module cycles is dependent on which module in a cycle is imported first; it's possible to get different semantics based on where the import enters the cycle.

Therefore, cyclic imports should be avoided when possible, and used carefully when necessary.

## Heap memory
n00b generally takes responsibility for memory management, including heap memory. The default language runtime implements a *garbage collector*.

The built-in garbage collector is precise, generating full stack maps. It also is a mark-and-sweep style collector, that does not relocate allocations.

At compile time, you can disable garbage collection, in which case the languages uses malloc-style memory pooling.

The builtin `free()` function does not need to be called when garbage collection is used, and is effectively a no-op. However, with GC disabled, `free()`'s semantics change. Specifically:

- If the programmer used the [`new` operator](#new Operator) to make the allocation, but the compiler chose not to use the heap, then `free()` is still a no-op.
- Otherwise, `free()` returns the memory for the allocation.

Disabling GC is *unsafe* and not recommended. While the built-in libraries should be written to assume `free()` is necessary, it's best not to count on it.

Additionally, you will be able to supply your own allocator to provide per-`new` site. However, this is not yet implemented.

## Language tooling
I'll write this text later. Currently, check the help; everything we have so far is in the `n00b` binary.

But the short-term intent is to have the compiler:
1. Auto-format code BY DEFAULT, adding type-annotations in place of inference.
2. Apply a custom local model to attempt to auto-fix basic errors (esp syntax errors).

## REPL evaluation

We designed the language with the intent of producing a n00b REPL environment for interactive experimentation. In fact, earlier prototypes implemented a REPL first.

For the most part, REPL and compiler semantics should be the same. Yet, there are a few differences.

The bulk of the differences not only are expected but should be non-issues for the typical user:

1. A compiled program may always access locally defined module-level symbols without regard to where those symbols are defined. In a REPL, top-level module statements are run immediately, and thus require the definition to appear statically before use, in those situations.
2. Similarly, imports in a REPL must appear lexically before they are used.
3. Initializer execution order with respect to imports is as if a compiled program dyamically imported them. We may fix a static import-before-use requirement globally.
4. REPL gets no `comptime()`-- the `comptime()` predicate ALWAYS returns `false` and `comptime_main()` is never automaticall run, even if defined.
5. The `REPL` context is considered to be a root module's initializer code; `main()` does not get automatically run during REPL evaluation.

But, there is one more meaningful difference we expect, that might have some impact. In some cases, n00b compiler performs deep static analysis in order to prove correctness. And, if it cannot prove correctness, it will fail to compile statically.

However, full module-level static analysis only happens when the module is textually complete, which is not reasonable for a REPL.

There, we will still attempt to do incremental analysis whenever feasible, but I expect to defer at least some of the checking to actual execution.

Ideally, the analyses will be able to behave the same for all practical purposes, but it may be incredibly challenging to match the same precision level for some things, in which case the REPL might end up accepting programs that cannot compile.

Still, such cases ideally will be uncommon.

The current plan is to use the MIR JIT-ing framework for the REPL implementation, with all imports being fully compiled before import, so that only the interactive context itself is being JIT'd. Thus, analysis differences will be limited to the REPL context itself, not its dependencies.

# Syntax

The semantics described in this document are based on a canonical source code representation, starting with the lexical syntax. That's to be expected, but there are two things I feel are important to note:

1. n00b encourages tooling to rewrite code to annotate implicit types, and normalize syntactic elements. For most simple cases, the user should not have to write unnecessary types, but should benefit from seeing what the system knows about a program.

2. Over time, I plan to experiment with a different canonical form for code that is more appropriate in the modern world. Nonetheless, any such changes to the underlying representation will always have a firm, bidirectional mapping to the canonical source code representation specified.

Canonical n00b source files are UTF-8 and any identifiers must be NFC-normalized. Bi-directional control characters outside of appropriate literals are currently not allowed.

## Syntax Skins

n00b supports limited *syntax skins*, meaning it accepts multiple common idioms for some language features, so that people new to the language will more likely have an easier time. That includes:

- Accepting brace-based blocks, as well as Pythonic indentation-based blocks.
- Accepting unambiguous alternate keywords and operators (like `not` vs `!`, and `or` vs `||`).
- Accepting both `#` and `//` for line comments.

In each of these cases, the language does have a preferred representation (braces, `!` and `or`, and '#'), which tooling can enforce, but the language does promote consistency within a single translation unit (source file).

Tooling generally will want to see consistency, unless they're configured to enforce a preference. Such automatic translation is currently part of the compiler's formatter, which will likely be auto-applied during compilation.


## Comments

n00b has skinned comment syntax, allow  ng both styles of line comment that are common in today's languages, `//` and `#`. The language does not consider any trailing newline to be part of the comment.

n00b also offers a nestible multi-line block comment. Such comments always start with `/*` and end with `*/`, but the number of asterisks used to open the comment are the number required to end the comment (more is an error).

For instance, the following works in n00b, but would be an error in other C-family languages:

```n00b
/** /* hi! */ **/

Currently, n00b tooling does not prefer a documentation comment syntax, but we do expect to add one. We will probably use structure inside block comments.

```
## Code regions (experimental)

Additionally, n00b allows comment-like syntax for defining code regions that scope comments. This can help tooling isplay comments out-of-band, beside the code they're describing. Additionally,, tooling can force re-visiting of comments when code changes.

The expectation is that tooling generally will manage these markers, and they should be out of sight for users.

Bare code region markers start with `#[#` and end with `#]#`. Regions can also be given names, in which case the name goes after the square bracket character, both with the starting and ending delimiter.

These are not considered comments, and they must always appear on their own line.

Code regions can nest and overlap.

Given code regions are considered experimental, they may change before 1.0. Feedback is welcome.

## Whitespace token rules

 Multiple spaces (U+0020) are combined into a single whitespace token. Newlines always get their own token, and must consist of either `\n` (U+000A) or `\r\n` (U+000D followed by U+000A).

NO other unicode whitespace characters are treated as whitespace or newlines -- including tabs. The compiler treats any such characters outside of fence literals as errors, including `\r` appearing outside the canonical `\r\n` sequence.

## Identifiers

Identifiers use Unicode's XIdStart and XIdContinue character classes, with the common addition of the underscore (`_`) permitted in any position. Ientifiers must be NFC normalized.

Names are case sensitive; `Foo` and `foo` are different identifiers.

### Reserved names

Names with two leading underscores are reserved for the compiler and builtin library. In many cases, you can define a function using a double underscore, but only if it is already a n00b-supplied callback.

For instance, when defining a `Queue` data type, you can define a constructor that runs on instantiation by defining an `__init()` method taking the type: 

```n00b
func __init(q: Queue, initial_size : uint = 16, max_size : uint? = nil)
{
    require(max_size >= uint)
    ...
}
```
### Naming conventions

n00b strives to maximize readability, which is why we strongly encourage using the underscore (`_`) to represent a space when an identifier is constructed from multiple words.

It is true that `CamelCulture` is about as clear as `snake_style`. However, we note that camel casing becomes hard to read when abbreviations are involved, which happens often when trying to balance clarity and brevity.

For instance, if building a "red/black tree", many would want to abbreviate Red/Black to RB. In camel country, this would be `RBTree`, which is a lot of consecutive consonants; `RB_Tree` is clearer. Similarly, instead of `TLSConnection`, `TLS_Connection` are easier to read.

Additionally, we highly recommend types use `Mixed_Case`, and variables use `lower_case`.

Preprocessor constants, which should be rare compared to C, generally should be `ALL_CAPS`.

Constants and enumeration values we recommend be either `lower_case` or `ALL_CAPS`; the standard for our own code is `lower_case`.

### 'Magic' variables

There are two variables that are built-in to n00b, that have special semantics.

First, `result` is only used in functions, and can be used to set a default value. When set, a `return` statement with no argument will implicitly be rewritten as `return result`.

It's a first-class variable that you can use in any expression where a variable of the return type would be appropriate.

For example:

```n00b
func is_valid_string(s: string)
{
    result = false

    if (len(s) == 0) { return }
    if (s[0] != '!') { return }
    ...
    return true // overrides that pesky default
}
```

The second magic variable is the *discard variable*, written as: `_`. You can assign to it, but you cannot read from it.

It means that the matching target is not needed, and should be thrown away. This can be used to suppress errors about unused values:

```n00b
// socket.write() returns byte written; `!` propogates an error if there is one,
// so we may not care.
_ = socket.write(to_write)!
```

### Reserved names

Some names have special meaning in n00b source code, than can prevent them from being used as identifiers. These fall into two categories:

- *Reserved names* are names that can never be used as identifiers in any context. Many of them are special keywords, but a few are special reserved literal values.

- *Soft keywords* are names that may be used as variables in contexts where they don't have special meaning. Usually it's still a bad idea to use them as identifiers, but there are cases where it can make sense, especially for keywords that are syntax-skinned.

For instance, `func` is the language-preferred way to denote the start of a function, but `fn` and `function` are allowable skins. If you follow convention, you might want to use `fn` as a variable name that holds a callback.

The list of reserved keywords:

`if``, `elif`, `elseif`, `else`, `when`, `for`, `in`, `while`, `switch`, `typecase`, `case`, `try`, `fail`, `type`, `alias`, `context`, `with`, `to`, `until`, `ref`, `cast`, `unsafe_cast`, `record`, `variant`, `enum`, `flagset`, `trait`, `effect`, `package`, `const`, `let`, `var`, `type`, `return`, `result`, `import`, `export`, `public`, `private`,  `func`, `or`, `and`, `new`, `callback`, `call`, `not`

The list of reserved literals:
`nil`, `true`, `false`, `null`, `None`, `True`, `False`

The list of soft keywords:

`i`, `u`, `bits`, `call`, `handler`, `setup`, `layout`, `align`, `step`, `out`, `inline`, `typeof`, `provides`, `enter`, `exit`, `ensure`, `as`, `thread`, `litmod`, `modify`, `consume`, `where`, `count`, `by`, `union`, `from`, `all`, `struct`, `object`, `view`, `fn`, `function`, `not`

The categorization of keywords will probably change before 1.0, so please try to avoid using soft keywords of two or more letters (`i` and `u` will definitely remain soft keywords).

## Operators, Delimiters and Punctuation

Many operators are associated language-supported *traits*. In those cases, custom types overload operators by implementing the API associated with the trait. See the list of standard traits in Section ??.

### Braces / brackets
| Syntax | Uses |
|--|--|
| `()` | Function parameters, grouping, tuple literals, anonymous record literals |
| `[]` | Array indexing, type parameters, sequence literals |
| `{}` | Code blocks, dictionary and set literals |

### Punctuation marks
| Mark | Semantic use |
|--|--|
| `;` | Separates statements |
| `:` | Denote an explit type; start Python-style blocks; slice / range separator, key / value separator |
| `?` | Postfix type modifier-- turns the type into an *option type* |
| `->` | Denotes return types or statement expressions |
| `'` | Generic types and literal modifiers |
| `,` | Item separator |

### Prefix operators
| Operator | Description |
|--|--|
| `!` | Boolean negation (skinned with `not`) ; also note the `!` postfix operator. |
| `-` | Integer negation. |
| `^` | Bit negation for flagsets and unsigned integers |

### Comparison operators
| Operator | Description |
|--|--|
| `==` | Equality testing |
| `!=` | Inequality testing (`x != y` <=> `!(x == y)`) |
| `>` | Greater than |
| `<` | Less than |
| `>=` | Greater than or equal |
| `<=` | Less than or equal |

### Binary math operators

Math operators on integers perform math on standard aligned sizes: 8, 16, 32, 64 or 128 bits, and may be signed or unsigned.

Addition and subtraction operators prohibit overflow or underflow; currently, instead of wrapping, n00b will `panic()` (an API is available for wrapping semantics).

The raw integer multiplication operator may lead to a doubling of the output size; multiplication of two 64-bit values will usually result in a 128-bit output.

That does not happen with the augmented multiplication operator, since augmented operators always store the result in place of the left hand side; the language will generally attempt to ensure the storage size is sufficient to avoid truncation, however.

Division by 0 results in a panic.

We may change semantics for integer division to yield a float, and may change modulus semantics for negative numbers.

| Operator | Description |
|--|--|
| + | Addition (including concatenation) |
| - | Subtraction |
| * | Multiplication |
| / | Integer Division |
| % | Modulus (clock math) |
| += | Augmented addition |
| -= | Augmented subtraction |
| *= | Augmented multiplication |
| /= | Augmented division |
| %= | Augmented modulus |

### Boolean logic (bitwise) binary ops

We may add keyword-based skinning for some of these, but due to augmented assignment variants, the language preference would still be for the symbolic operators.

Note that sign extention in bitwise operators is often a source of subtle error for non-experts. For that reason, we only define these operators on `flagset`s and unsigned integer types.

However, the standard library will have APIs for signed integer types for those who want them.

Currently, there are no operators for bit rotation; again, these will be available as API calls in the standard library. 

| Operator | Description |
|--|--|
| & | Bitwise `and` |
| \| | Bitwise `or` |
| ^ | Bitwise `xor` |
| << | `shift left` |
| >> | `shift right` |
| &= | Augmented bitwise `and` |
| \|= | Augmented bitwise `or` |
| ^= | Augmented bitwise `xor` |
| <<= | Augmented shift left |
| >>= | Augmented shift right |
### Postfix operators
| Operator | Description |
|--|--|
| ! | Result unwrap-or-propogate |
| . | Member access; dynamic dispatch |

Member access is classified as a postfix operator because it has a restricted second argument: it does not allow free-form expressions, only member fields, submodule names and trait methods.

Similarly, array indexing and function calls are postfix operators in those contexts.

### Operator Precedence
n00b has *partial operator precedence*. In many languages, every operator is ordered so that any two operators appearing together have an explicit definition of which operator is going to "bind tighter".

For instance, anyone who had PEDMAS drilled into them in grade school knows that multiplication "binds tighter" than addition (i.e., has higher precedence), which is why we all expect:
```n00b
2 * 7 - 1
```
to give us 13, not 12.

However, what about:
```n00b
2 << 7 - 1
```
Is that `(2 << 7) - 1`, i.e., 255 or `2 << (7 - 1)`, i.e., 128?

Partial precedence means that we respect "obvious" precedence relationships, such as the ones we learned in grade school.

But, if there's any chance that two operators appearing in the same expression might have an ambiguous interpretation, we require parentheses to clarify.

For instance, here's a bit more realistic example of using the left-shift operator (`<<`) ambiguously:
```n00b
let a = 1 << n - 1
```
When you run this through n00b you will get:
```bash
error: `<<` followed by `-` is not allowed, because this could be read more than one way

    1 | let a = 1 << n - 1
      |           ^^   ^

  help: add parentheses to say which one you mean:
    1 | let a = (1 << n) - 1
    1 | let a = 1 << (n - 1)
```
Instead of a full precedence table, we break operators up into related groups:

| Group | Operators | Example |
|--|--|--|
| **Postfix** | `x.field`, `x[i]`, `x[a:b]`, `f(...)`, `x!` | `items[0].name` |
| **Sign** | prefix `-`, `+`, `~` | `-x` |
| **Not** | prefix `not` / `!` | `not done` |
| **Word prefix** | `cast[T]`, `unsafe_cast[T]`, `ref` | `cast[f64](n)` |
| **Multiplicative** | `*`, `/` | `a * b` |
| **Remainder** | `%` | `i % n` |
| **Additive** | `+`, `-` | `a + b` |
| **Shift** | `<<`, `>>` | `1 << n` |
| **Bit-and** | `&` | `flags & mask` |
| **Bit-xor** | `^` | `a ^ b` |
| **Bit-or** | `\|` | `a \| b` |
| **Comparison** | `<`, `<=`, `>`, `>=`, `==`, `!=` | `i < n` |
| **And** | `and` / `&&` | `ok and ready` |
| **Or** | `or` / `\|\|` | `ok or retry` |

Together, *Additive*, *Remainder* and *Multiplicitive* are the **Arithmetic** operators. The **Bitwise** operators are *Shift*, *Bit-and*, *Bit-xor*, and *Bit-or*.

Here's the rules between groups:

| Group | You can skip parentheses when mixed with |
|--|--|
| **Postfix** | all other groups |
| **Sign** | all other groups |
| **Not** | all groups except Comparison |
| **Cast** | Postfix, Sign, Not |
| **Multiplicative** | Postfix, Sign, Not, Additive, Comparison, And, Or |
| **Remainder** | Postfix, Sign, Not, Additive, Comparison, And, Or |
| **Additive** | Postfix, Sign, Not, Multiplicative, Remainder, Comparison, And, Or |
| **Shift** | Postfix, Sign, Not, Comparison, And, Or |
| **Bit-and** | Postfix, Sign, Not, Comparison, And, Or |
| **Bit-xor** | Postfix, Sign, Not, Comparison, And, Or |
| **Bit-or** | Postfix, Sign, Not, Comparison, And, Or |
| **Comparison** | Postfix, Sign, Arithmetic, Bitwise, And, Or |
| **And** | Postfix, Sign, Not, Arithmetic, Bitwise, Comparison |
| **Or** | Postfix, Sign, Not, Arithmetic, Bitwise, Comparison |

Together, **Sign**, **Not** and **Cast** operators are all **Prefix** operators. Everything else takes two arguments, and are often called **Binary** operators.

Note, when you combine prefix operators, they're applied from closed to the value, out. For example, `-cast[i64](x)` means `-(cast[i64](x))`.

Prefix and postfix operators can be chained together freely, without parentheses.
Otherwise, chaining is sometimes allowed inside operator groups, but often not. For instance, we don't accept chained comparisons like `x < 12 < y`; we require parentheses, or `x < 12 and 12 < y`. 

This table shows what chaining is allowed for binary operators, without parentheses:

| Operator | Chains with |
| `+` | `+` or `-` |
| `-` | `+` or `-` |
| `*` | `*` or `/` |
| & | & |
| \| | \| |
| ^ | ^ |
| `and` / `&&` | `and` / `&&` |
| `or` / `\|\|` | `or` / `\|\|` |

To be clear, division can be unambiguously chained to the end of a string of multiplications, but the division itself cannot be chained. So `/` requires parentheses to have another `*` or `/` appear after it.

`%` cannot be a part of any chains with itself, or with multiplicitive operators.

For those used to C, comparison operators don't work the same way, they work the *sane* way.
For example, in C, `a | 1 == b` is interpreted as `a | (1 == b)`.

In n00b, the operator groups **Comparison**, **And** and **Or** always require boolean results, and so the C precedence rules would make no sense; they implicitly require converting the boolean result to an integer-- that's incredibly unintuitive.

Here are some specific examples of things n00b accepts:

| You write | n00b groups it as |
|--|--|
| `-items[0].count` | `-((items[0]).count)` |
| `a + b * c` | `a + (b * c)` |
| `a - b + c` | `(a - b) + c` |
| `base + i % n` | `base + (i % n)` |
| `a + 1 < b` | `(a + 1) < b` |
| `flags & mask == 0` | `(flags & mask) == 0` |
<| `x < y and y < z` | `(x < y) and (y < z)` |
| `not done and ready` | `(not done) and ready` |

Here are a couple of examples where people might expect to skip parentheses, where we do require them:
| Invalid | Why it's an error |
|--|--|
| `a / b * c` | `(a / b) * c` has different meaning from `a / (b * c)` |
| `0 <= i < n` | Chained comparisons aren't allowed. |
| a and b or c | `(a and b) or c` has different meaning from `a and (b or c)` |
| not p == q | `(not p) == q` has different meaning from `not (p == q)` |
| cast[f64] a / 2.0 | Is that `cast[f64](a) / 2.0` or `cast[f64](a / 2.0)`?? |

## Literal Syntax

While n00b supports common literal types, it has two literal features that are far less common:

- There is a special literal type we call a *fence literal*, that can hold arbitrary content.
- The language allows user defined literal modifiers (which we call *litmods*), which supports a diversity of use cases, including multi-line strings, user-defined data types, and embedded DSLs.

Most kinds of literal syntax support modifiers; only character literals and type literals are excluded.


| Literal kind |  Description | Example | litmods? |
|--|--|--|--|
| quotes |  Traditional string | `"Hello!"` | Yes |
| character | Unicode codepoint; `''` not allowed | `'«'` | NO |
| int | Currently must be in range: 0-2^128 ; `_` allowed between digits as a separator | `1_234_567` | Yes |
| hex | Leading `0x` plus hex ; 0-2^128 and also accepts _ | `0xAbC123` | Yes |
| float | Typical float syntax | `0.314159e1` | Yes |
| bool | Typical boolean value | `true`, `false` | Yes |
| nil | The single value of the `nil` type | `nil` | Yes |
| sequence | Ordered items, such as arrays and lists | `[1, 2, 3]` | Yes |
| set | Set of unordered items; shares braces with dict, but no item pairing | `{ 1, 2, 3 }` | Yes |
| dict | Associative mapping of key / value pairs  | `{ 1 : "a" }` | Yes |
| tuple | Ordered, immutable fixed-size set of values; 2 or more items | `("one", 2, 3.14)` | Yes |
| record | Unordered, data type with fields | `(x = 1, y = -1)` | Yes |
| fence | Flexible literal container; see Section 2.? | `[- Hello! -]` | Yes |
| typespec | A valid type | `Dict[string, 'a]` | NO |

### Literal modifiers
Literal modifiers come after the unadorned literal, and consist of a `'` followed by an identifier; spaces aren't allowed around the `'`.  For instance, the Json litmod can be applied to appropriate syntaxes:

```n00b
j = 12'json
j = 12.5'json
j = "twelve"'json
j = [ 10, 11, 12 ]'json
j = { "item" : 12 }'json
```

All of the above produce a `Json` object, because of the literal modifier.

Note that even litmod must return results that follow n00b's type rules. Importantly, generic containers with unbounded items must have a single, unified type for each variable.

For example, dictionaries may have a different key type from value type, but all keys must be of a uniform type, and all values must be of a uniform type.

Since lists and arrays both require type uniformity, the following isn't valid n00b:
```n00b
[ "one", 2, 3.14, { "object" : nil } ]
```

However, the Json litmod seems to be breaking the rule, since the following works:

```n00b
import text.json

j = [ "one", 2, 3.14, { "object" : nil } ]'json
```

However, the json litmod returns a single object of type `Json`, which is a variant type (aka, a tagged union).

And, in fact, if you poke at the resulting `Json` object, you will find it will contain a list that itself holds four `Json` objects!

So modified literals need to take a valid type, but the static, unadorned type before processing does not matter.

Still, custom litmods cannot change the lexical rules for any syntax type. A modifier on `int` syntax gets to interpret an integer in a valid range; it does not allow the right hand side of `x = π` to be treated as a literal (though, `π` is a valid variable name).

Unmodified literal types will automatically map to the most intuitive type. For numeric types, bools, strings, characters and nil, the mapping is straightforward.

For container types, our inference algorithm attempts to be flexible. Just seeing literal syntax only results in recording a fact, that the literal has a particular syntax type. Individual data types can make themselves compatable with a given syntax type if they're imported into a module, and have, for their data type, implemented the API associated with the syntax.

The type inference algorithm isn't quick to tie the syntax to a type. It looks at other contextual clues as it encounters them.

If, when a module is otherwise processed, multiple in-scope types are still matches, then the system will select the default type for the syntax.

For example, if we have imported our own queue type, `x = [1, 2, 3]` might match the queue type if we then see `x.enqueue(12)`. But, we might not see enough information to distinguish `x`'s type between a `Queue`, `List` or `Array` (fixed-size list).

In this case, the default container type for sequence syntax is `List`.  

If that's not right, you can force `Queue` in multiple ways. For example:

1. You can use a litmod defined alongside the `Queue` type. E.g., `x = [1, 2, 3]'queue`
2. You can explicitly annotate the literal with the type. E.g., `x = [1, 2, 3] : Queue[uint]`
3. You can explicitly declare the type of the variable to which the queue gets assigned: `let x : Queue[uint] = [1, 2, 3]`

Note that litmods are not intended to be for the sole purpose of type annotations. They're intended for special processing of literal text.

### Escape Sequences

The character literal and default string literal supports the following escape sequences:

| Escape | Meaning |
|---|---|
| `\a` | Alert |
| `\b` | Backspace |
| `\f` | Form feed |
| `\n` | Line feed |
| `\r` | Carriage return |
| `\t` | Horizontal tab |
| `\v` | Vertical tab |
| `\0` | U+0000 |
| `\"` | Double quote |
| `\'` | Single quote |
| `\\` | Backslash |
| `\xHH` | Unicode code point given by two hexadecimal digits |
| `\uHHHH` | Unicode code point given by four hexadecimal digits |
| `\UHHHHHHHH` | Unicode code point given by eight hexadecimal digits |
Litmod definitions for string syntax receive raw contents, and may process escapes as they see fit.

### Fence Literals
A fence literal uses as its delimiters, `[-` and `-]`. However, there are additional rules:

- Matching delimiters require matching dash counts on the beginning and ending delimited.
- At least one whitespace character must follow the opening delimiter.
- At least one whitespace character must precede the closing delimiter.
- In the case of an empty delimiter, the require whitespace can be shared by the delimiters.
- The contents of the literal disregard leading and trailing white space.
That means, the following is a valid empty n00b fence literal:

```bash
[- -]
```

However, the following is not valid:

```
[--]
```

Fence literals are designed to hold arbitrary text that does not need any escape processing. They may even contain other fence literals without issue, simply by ensuring the number of `-` signs in the delimeter are greater than any found in the embedded value (otherwise, a delimiter with too many `-` marks will give an error).

Without a litmod, fence literals produce objects of type `string`.

Note that trailing and leading whitespace is UNIVERSALLY stripped from fence literals, but no other stripping happens by default.

For instance, some multi-line literal delimiters will automatically strip common indentation, and trailing white space from strings. The text package contains a `trim` litmod.

Other builtin litmods allow fence literals to hold code for domain-specific languages (DSLs). For instance, there is a builtin `ffi` litmod for fence literals to map external libraries to n00b APIs. 

Similarly, the `langtools` package implements (will implement actually) a  `bnf` litmod for fence literals, allowing you to write simple EBNF grammars, which can be loaded at compile time, and used to implement your own fence litmod for other domain specific languages.

## Statements and expressions

Whereas some functional languages have only expressions, n00b is a traditional procedural language with statements that have a definitive consecutive order, and do not, by default, return a value.

Statements can generally contain one or more expressions.

Statements can be separated in two ways:

1. Newlines
2. Semicolons (';')

Semicolons should be used sparingly, when multiple short statements on one line would promote clarity.

Statements also may end without a newline or semicolon at the end-block delimiter, without a newline or semicolon present. Meaning the function call inside the code block here is a valid statement:

```n00b
if x { call_func() }
```

### 2.9.1 Statement continuation rules

Statements are allowed to span lines, in the following conditions:

1. Spanning lines would have unambiguous meaning.
2. There are no blank lines (empty or having white space) inside the statement.

If two lines of code would independently be correct statements, then continuations are not allowed.

In many cases the rules are intuitive. For instance, it's always safe to break after a binary operator:

```n00b
y = x -
    2
```

And it's clear that this isn't a continuation:
```n00b
foo(x)
bar(y)
```

But it's situational based on context. In the previous example, `)` ends a valid statement, and `bar` is valid for starting a new statement, making a line continuation invalid.

But, the ending parenthesis isn't enough to invalidate the continuation, because this would clearly be one:

```n00b
foo(x)
  .bar(y)
```

That's because the `.` operator is always a binary operator and can never start a statement.

The are a few cases where this policy might be surprising. For example:

```n00b
foo(x)
[1]
```

Literal values are expressions, and expressions on their own can be statements. By our rule, this clearly *could* be two statements, and cannot be a continuation.

But, this is code you'd be unlikely to see; it's more likely to be a wrapped array access.

However, we feel it's best to keep the rule simple. And, in this case, the fact that a literal is not assigned or otherwise used results in a clear warning that should make it clear that the language is interpreting `[1]` as a list literal, not an array indexing. 

### 2.9.2 Statement Expressions

There are times where it's useful to be able to do work inside an expression that really needs multiple statements, but isn't appropriate for a full function call.

In many languages, this is done by having individual statements also double as expressions, and blocks of statements being considered an expression, usually by having the result of the last statement be considered the result of the expression.

That is unintuitive and surprising for people used to statements; n00b requires explicitly denoting that the last statement of a block is to be considered the *value* of a block, which we do with `->`; otherwise, blocks are not allowed to have values.

```n00b
a = { x = f(); y = g(); -> x + y }
```

Branching constructs like `if` and `switch` also can be statement expressions. However, all possible branches of a construct must consist of a statement expression. For instance, you can do:

```n00b
a = if x { -> 12 } else { -> 14 }
```

But it's an error if you were to leave out the `else` branch, or to leave out either `->` operator.

All branches must also return compatable types.

It's an error to have additional statements in a block after the `->` operator, just as `return` may have no statements that follow in a block.

## 2.10 Code blocks

n00b has two syntax skins for code blocks, braces:

```n00b
enum {
  no  = 0
  yes = 1
}
```

And whitespace:
```n00b
enum:
    no  = 0
    yes = 1
```

To allow for whitespace blocks, the language generates `INDENT` and `DEDENT` tokens that track state. The `DEDENT` token flags when there's a matching problem, which is only an error if inside a block using whitespace blocks.

Otherwise, INDENT / DEDENT tokens are silently ignored as whitespace when not processing a whitespace block.

## typespec literals
Types themselves are considered literals. And, a type itself can be data, of type `typespec` (especially valuable for reflection).

Typespec literals primarily take one of the following forms:
| Name | Syntax Example | Description |
|---|---|--|
| Named | `int` | Builtin primitive types, as well as many user-defined built-in types |
| Qualified | time.Timestamp | Follows module hierarchy |
| Function | (x, y) -> int | The overall type associated with a function |
| Parameterized | Dict[string, int] | Types that need other types provided for instantiation |
| Optional | int? | The preceding type, with the option of also not being present |
| Nil | nil | Represents the type that takes no values |
| Variable | `'T` | Represents any single type (often used in parameterized arguments) |

Most user-defined types end up written as *named* or *parameterized* entities.

For instance, there is no special syntax for types created with constructs like `variant`, even if there may be in other languages. The kind of syntax used depends solely on whether there are unresolved types in the declaration that need to be provided in order to instantiate it.

For instance, a JSON type may be declared as:

```n00b
variant Json {
  nil_item:  nil
  int_item:  i128
  fp_item:   float
  str_item:  string
  list_item: List[Json]
  dict_item: Dict[string, Json]
}
```

That type takes no type parameters, so the type is simply `Json`.

Let's say though, that we wanted to make a more *constrained* Json, `Cson`, which was the same except with the restrition that, for any given list or dict, the items contained must be a single uniform type. We could then declare the variant like this:
```n00b
variant Cson {
    nil_item:  nil
    int_item:  i128
    fp_item:   float
    str_item:  string
    list_item: List['T]
    dict_item: Dict[string, 'T]
}
```

Now, when we want to declare a `Cson` object holding a specific type of item, we have to declare a type necessary to instantiate it:

```n00b
let x: Cson[int128] = (list_item = [12_345])
```

If you want to set a branch that isn't parameterized, you can set the type parameter to `nil`:
```n00b
let y: Cson[nil] = (str_item = "Hey there!")
```
However, since the type isn't used on the `str_item` branch, it's not an error to put some other type as a parameter. That's partially because, the type system assumes the contents might be re-defined, and makes sure to lay out the type to hold the storage. So, using `nil` implicitly assumes that, if `list_item` is selected, it cannot have items (or if it does, they are all `nil`), or that if `dict_item` is selected, no values are stored for any key.

There are a few builtin parameterized types that are provided by the system, that have special semantics:
| Type | Description |
|--|--|
| Tuple[] | Describes types for both Python-like ordered tuples, and unordered tuples (aka named) |
| Array[] | Array bounds can be provided with a type decl but aren't part of the type. |
| Ref[] | Reserved for non-null references |
| Atomic[] | Creates a type that must be accessed atomically (must be supported at given size on the machine) |
| i, u, bits | bitsliced integers (see below) |

### Tuple syntax

The way to tell from a type whether a tuple is ordered or not is by the parameters list. Like most type parameters, ordered tuples have positional types. So:
```n00b
tup = (1, "two", 3.14)
```
Might be typed: `Tuple[int, string, f64]`

Whereas, unordered tuples use field names. If you create one as such:
```n00b
tup = (x = 10, y = -1)
```
It might be typed: `Tuple[x: int, y: int]`

### Array types
There is some array-specific syntax you can use when writing an array type.

First, you can, for a one-dimensional array, write the array size after the type, even though it is not part of the type. For example, you can write:
```n00b
const to_hex_map : Array[byte][16] = "0123456789abcdef"
```
But you do not need to do that;
```n00b
const to_hex_map : Array[byte] = "0123456789abcdef"
```
Works just fine.

You can also use an expression to declare a variable size. 


Second, the type system treats multi-dimensional arrays as nesed types, but there's some syntactic sugar there.

Instead of:
```n00b
var matrix: Array[Array[float]]
```
You can write:
```n00b
var matrix: Array[float][][]
```
It's a little obtuse for those coming from other languages, but the rule is:
1. The first parameter is always the ultimate item type of the array.
2. Any subsequent square brackets are interpreted as a dimension specification, like in our first example.
3. The second dimension spec onward implies nested `Array` types.

When providing array dimensions, the rules are typical:
1. once you omit a dimension, you must omit all dimensions.
2. specifiying sizes for `n` dimensions make the array *rectangular* for those `n` dimensions, meaning it will be one contiguous storage block, with fixed dimension sizes.
3. Omitted dimensions (after the first) make the array *ragged* for any unspecified dimensions. Ragged dimensions require you assigning an `Array` object of the appropriate type to instantiate, otherwise are zero-sized; indexing into them will fail.

See [Arrays](#Arrays) for more details on array declarations and semantics.

### Reference types
`Ref['T]` creates a reference type. References stand in for memory addresses, and may not be `nil`. Syntactically, there's nothing special about the type. For semantics on references, including using traits to define custom reference types, see the section, [References](#References).


### Bitslice types
Integers get aligned storage, even when inferred to be small, which avoids unnecessary bitwise operations to set and get values. However, sometimes that's valuable. And, as described in [Layout rules](#Layout rules), n00b defaults to packing anything that's considered a bit-level type (including `bool` items).

If you want integer types to also be paccked, you can get them, up to 128 bits in size, using C-like bitslice syntax, on `i` to get a signed integer, `u` to get an unsigned integer and `bits` to get a type that is just a binary slice of bits:
```n00b
var x: i:10 // 9 bits plus the sign
var y: u:2 // 2 bits, no sign.
var z: bits:100 
```
However, for boolean flags, you might instead consider our `flagset` user-defined type; a special type of `enum`.

### Traits and type constraints
In n00b, traits are interfaces that are automatically matched, much in the way Python handles *duck typing*.

The name provided when defining a trait is either a *named* type or a *parameterized* type, depending on the definition, and can be used in any declaration context (but cannot be used for instantiation).

However, trait types actually get treated as *type constraints*.

Declaring a parameter to be: `x: Sortable` is a shorthand for: `x: 'T where 'T: Sortable`.

If you're providing a `where` clause, you can require multiple constraints on one variable, separated with a `+`, such as: `x: 'T where 'T: Sortable + Hashable`.

Naming the variable is useful when parameters for complex containers need to implement multiple traits:

```n00b
x: dict['K, 'V] where 'K: Hashable and 'V: Some_Int
```

The semantics of traits and declaration syntax is covered in the Section [Traits].

### Function type syntax
[Function declarations](#Function declarations) can contain type information, but:

1. The type is only a subset of what you can declare before the body of the function.
2. The type written-stand alone (e.g., when declaring a callback / `Ref[]`f to a function) is compact; the pieces in a function are spread out-- parameter types go after the parameter name, and return information goes after the parameter list.
3. There are some minor syntactic differences for parameters.

Actual function types take the form: `(...) -> 'R fail: 'F`

Note there's no function name.

The `->` denotes the type returned on a successful result, which can be any type, including `nil`. After `fail:` comes a type that results if the function fails, which may also be `nil` or any other type.

Many languages have a special `Result['R, 'F]` type that requires a lot of bulky decomposition. In n00b, the idea is exactly the same, a union type baked into the language. See [Error Handling](#Error Handling) and the [fail statement](#fail Statement).

Neither functions nor parameters carry effect information; that is a separate parallel analysis that is not considered part of the type. Functions don't carry other modifiers in their type either (e.g., `inline`). Parameters do not include passing modes in the type.

So far as the type system is concerned, there are three types of parameters, always written in the following order:
1. Positional parameters
2. Variable argument parameters (varargs)
3. Keyword arguments (kargs)

Any type can be skipped if not provided. Syntactic rules:
1. positional parameters list only the type name
2. Varargs is denoted by an `*` followed by the type name associated with an *item*
3. The kargs list may be written with an optional Pythonic `**` before the list.
4. Individual kargs can appear in any order for the purposes of checking.
5. The syntax for individual karg fields is the same as for unordered tuples, `field_name : Type`.
6. All keyword args must appear in the type for equality.

The Pythonic `**` to set off keywords is not used in declarations, and will probably be removed from the type, but was provided for additional deliniation, since a declaration is clearly a keyword parameter given a default assignment, but that assignment is not part of the type.

Note that varargs parameters must all have a uniform type. It is a syntactic convenience, allowing you to pass an armitrary number of same-typed parameters. While the type denotes the item type, the implementation gets an `Array[]` of the item type.

Note that call sites do not reference specific type information about a function they're calling.

So, a hypothetical function declared like:
```n00b
func my_print(first: string,
              *rest: string,
              fd:    i32     = stdin_fileno,
              end:   string? = "\n") -> u32 fail: Errno { ... }
```

Will get the type: `(string, *string, fd: i32, end: string?) -> u32 fail: Errno`

### Other type syntax
In some limited cases, n00b allows anonymous union types, which can be written `'A | 'B`. However, due to our type checking philosophy, we not only discourage such types in most situations, we disallow them in many circumstances, and we never infer them.

Particularly, we like to see assignments within a scope be of a uniform type. And when that isn't approppriate, then we prefer explicit `variant` types, where the *arm* of the variant must be explicitly specified when assigning. Here too, we never infer values to be of a variant type; they must be explicitly specified.

The only places we allow anonymous unions are in cases where, in the variable's scope, it will not be rebound to a different type. Plus, n00b does not allow specified types to be specified as general in cases where they are always concrete. So, `int | string` is not too useful as a bare variable, and is disallowed outright.

You can currently declare a parameter to be of type `int | string`, however. For any invocation, the bound type is fixed for the call.


## Type annotations
Type literals can, in some cases, be treated as data. But, they are primarily meant to facilitate type checking and provide documentation.

Common places to use type literals:
- As typing annotations for variables, function parameters, and function return information.
- As typing annotations for fields of user-defined data types.
- As a typing annotation for other literals.
- To specify the target type for any user-supplied casting operations.

The first three uses of type literals are considered *type annotations*, which always involves adding a `:` followed by the type literal that specifies the type of the item being annotated.

For instance:
```n00b
var x = 4
```
Can be annotated with an exact type. Perhaps we want to explicitly communicate we don't mind if `x` goes negative, so we don't want it becoming a `uint`:
```n00b
var x : int = 4
```
Type annotations *declare* a type that must be accurate. They are *checked*, and if anything is inconsistent with the type on checking, then the compiler errors.

## Casting
Unlike type annotations, casting is a user-supplied request for the compiler to convert a value of one type to a different type.

That has very specific rules, and the compiler will check whether the input type may be cooerced into the output type.

There are two casting operators with similar syntax. For instance:
```n00b
int x = 1000
uint y = cast[int] x
```

There are two cast operators, `cast[]` and `unsafe_cast[]`; the semantics of both operators is covered below. 

# Scopes
For any given translation unit, the top-level scope is the *module scope*. Names that you import into the module are accessed through the module scope.

For instance, `import containers.tree` will place the name `containers` in the module scope, and its submodule, and any functions / fields are accessed through field access on that module level symbol.

Functions, types, contexts, and litmods defined inside a module also live in the module scope. The language does *not* support nested functions.

Variables declared at the top-level in module initialization code also live in the module's scope.

Function scopes are nested inside the toplevel module scope. They get full access to symbols in the top-level module scope.

Shadowing a module variable inside a function is possible with an explicit declaration, but all shadowing is discouraged, and resulta in a compiler warning.

Function scopes hold any parameters named in the function, the special `result` variable, and any variables declared in the function (i.e., are not inherited from the parent scope).

While n00b does support nested block scopes, it is *limited*, only for variables explicitly declared with `let` binding.

The reason for this is to be comfortable and familiar to beginners who may have seen Python, where variables:

1. Never need to be declared; and
2. Stay in scope till the end of the function.

In contrast, `let` bindings are available to support more robust code, by assuring one binding per scope entry, for instance, inside a loop.

We do like better checking for those opting into it, which is why only `let` variables are tied to nested blocks.

# Symbol Visibility
In n00b, there are two visibility keywords, `public` and `private`. They're used to switch the default visibility.

`public` means, "exported from this module", and `private` is the opposite. No link-level symbol is avaiable for private symbols.

`public` symbols are always available within the package they're defined in.

A package's manifest controls whether `public` symbols are made available outside the module, having the freedom to restrict visibility to specific 'friend' packages. You can provide a manifest with the [package statement](#package Statement), or accept the default manifest.

| Symbol kind | Default visibility |
|--|--|
| Function | public |
| Type definition (any kind) | public |
| Fields inside a type | public |
| Context manager definition | public |
| litmod definition | public |
| const module variable | public |
| Other module variable | private |

The default manifest implements the default visibility rules, ignoring all cases where non-const module variables were marked `public`. To make such variables visible outside a package, you need to provide an explicit manifest.


# Variable declarations
Variables may be explicitly declared, and, in most cases, they may be implicitly declared. Explicit declarations all declare a binding, which is one of:
| Binding | Semantics |
|--|--|
| var | The variable may be assigned to multiple times, as long as its type doesn't change. |
| let | The variable may only be assigned once for any path through the block in which it's declared, and cannot be used beyond that block. |
| const | At program run-time the value can only be read; during comptime execution it has the same semantics as `var` |
Every variable *must* be initialized in full before it is used. If a type has a constructor, invoking the constructor with the [`new` operator](#new Operator) instantiates the variable.

Other complex object types generally need a full assignment, although arrays with defined bounds are allowed in many cases; see [Arrays](#Arrays).

When explicitly declaring a variable, you can both declare a type, and assign a value. The type is generally optional; the language will do its best to infer a type, and will complain in specific situations where the language requires the programmer to be explicit.

For example:
```n00b
var s: string = "Hello, world!"
```
Is good form, but it's a perfectly valid explicit declaration still if you omit the type:
```n00b
var s = "Hello, world!"
```
An explicit declaration can get even more sparse:

```n00b
var s
...
s = "Hello, world!"
```

In this case, n00b's static checker is perfectly capable of determining that `s` is of type `string`.

But, you can go even more spartan! An *implicit declaration* occurs whenever a new identifier first appears, *as long as it appears as part of an assignment*.

> n00b strictly statically enforces that variables must be defined before use. If the language sees a path through a scope where it cannot prove it, the program won't build.

Implicit declarations assume `var` binding. The idea here is to minimize the burden for beginners, or for people coming from languages like Python who are not used to explicit declarations.

However, n00b's tooling strategy is to automatically annotate source code whenever feasible to show type information, for instance, after a successful compile of a translation unit.

In such cases, the re-writer can also "promote" `var` bindings to `let` bindings if they usage follows single static assignment rules.

### `let` and single assignment
As is traditional, declarations with `let` binding require *single static assignment*. While n00b does enforce single static assignment, it doesn't require it to be at the point of declaration.

If you're the type of person who loves declarations to be at the start of a block, you can do:
```n00b
let x: int

// ... lots of intermediate code

x = y + z // first use.
```
In fact, it's even possible to assign to `x` multiple times, as long as n00b can prove no trip through the block can result in multiple bindings.

For example the following code is fine:

```n00b
let u8_prime: bool
switch(value) {
  case 2, 4, 8, 16, 32, 64, 128:
      u8_prime = true
  else:
      u8_prime = false 
}
```

You can use `let` in a loop without having the variable be fixed for the life of the loop:

```n00b
sum = 0

for i in 1 to 100 {
  let x = fib(i)
  sum += x
}
```
Here, `x` can be `let` bound because it get sets only once per trip through the loop.

However, if we were to declare `x` as a `let` variable above the loop, it wouldn't be allowed, even if we don't assign it until we get in the loop:

```n00b
var sum = 0
let x: u128

for i in 1 to 100 {
    x    = fib(i)   # NOT ALLOWED
    sum += x
}
```
This is not allowed, because `let` variables are *block scoped* (unlike `var` and `const`), and the block in which `x` extends beyond the loop. So, `x` would have to change for every iteration through the loop, which breaks the cardinal rule of `let`.

## `let`: mutability rules
Many people first exposed to `let` constructs assume that *single static assignment* is the same thing as *immutability*.

That's not the case. The `let` promise is that the *variable as stored* will not be changed. However, anything you can access through the variable via reference can be modified.

For example:
```n00b
record Timestamp { sec : u64, nsec : u64 }

let unix_start = (sec = 0, nsec = 0)
```
In that code, we can subsequently read `unix_start.sec` but we cannot change its value, because `record` types are always stored inline, and the `let` promise is that storage can't change in its scope.

However, if we defined `Timestamp` to be an `object` instead:

```n00b
object Timestamp { sec : u64, nsec : u64 }

let unix_start = (sec = 0, nsec = 0)
```

Then `unix_start.sec = 10_000` would 100% work, because `object` types are considered *shared*; what that means in practice is that any variable that holds an `object` type holds a handle pointing to a shared view of the object. That handle cannot change, but the shared fields can be.

This rule would hold true if you declare a `record`, but it has a field that is an `object` type. Specifically, you couldn't assign a new object, but you're free to set pubic values inside any such embedded object, because that lives outside the storage that `let` protects.

When `let` is applied directly to an array, it is always treated as shared; the array must get storage that follows [Array initialization rules](#Array), but once it does, individual elements may be set.

This is because, while n00b MAY choose to store variables declared to be fixed sized arrays inline (e.g. on the stack) it reserves the right to store those variables however it wants. In many situations, arrays will live in the heap, even if they could live on the stack.

This aligns with the fact that  container types naturally want to be *shared*.

**However**, *n00b does* guarantee that arrays inside another data structure are stored inline IF the size is known at compile time. When that is the case, the array is part of the storage of the type it's embedded in; its elements cannot be modified if their storage is definitiely `let` bound.

Consider the following example:
```n00b
const cache_size = 4

record Some_Type {
    cache: Array[int][cache_size]
}

let x: Some_type = (cache = [0, 1, 2, 3])
x.cache[0] = 10 # COMPILE ERROR!
```

In this example, `x.cache` is protected by the `let` because the definition of the record type gave an explicit static size.

However, if the record definition doesn't specify the size, then `let` binding an instance doesn't protect the cache:

```n00b
record Some_Type {
    cache: Array[int]
}

let x: Some_type = (cache = [0, 1, 2, 3])
x.cache[0] = 10 # THIS WORKS!
```
That's because undetermined sizes require dynamic allocation, and imply the array is shared.

## `const` binding rules
In n00b, `const` means, "completely immutable at runtime". Nothing reachable from a `const` object can change... once runtime starts.

But n00b supports `comptime` execution, which largely exists to support precomputation. Some pre-computed values are intended to be pre-initialized, but some are intended to never change!

To support flexible `comptime` semantics, `const` binding's immutibility promise is strictly limited to *runtime execution*.

During compile-time execution, `const` variables have `var` semantics.

Meaning, you *can* do multiple assignments to a `const` variable, as long as those assignments are guarded so that assignments can only be run during comptime, which you can do by putting assignments inside blocks like: `if(comptime()) { }`

Usually this is not necessary; you can declare `const` variable and assign once, using a function call to do the precomputation.

For that reason, I will probably change `const` binding to accept only a single assignment during compile time.

## Storage classes
In most situations, the user doesn't need to specify where a variable should be stored.

Typically:

- `const` variables are stored in static read-only memory.
- Public non-const toplevel module-scope variables are stored in static space.
- Other non-const toplevel module-scope variables will be static if used inside functions, and generally on the execution stack if not.
- Non-const function locals also generally end up on the execution stack.
- The compiler can use the heap instead of the stack if necessary (variable lifetime will live past the stack frame), or in cases where the variable is large.
- If the user specifies an allocator with `new`, that allocator is always used.
- Otherwise, memory initialized via `new` is placed either on the default heap, or the stack, as the compiler sees fit (data shared by threads inevitably will be in the heap).

The only situation where a user needs to specify a storage class is to use thread-local storage.

The basic idea is to make it possible for each thread to have its own private copy of some variables. These private copies are kept in thread-local storage.

To specify thread local storage, you must explicitly declare the variable; put `thread` before the binding:

```n00b
thread var alloc_counter = 0
```
Thread-local storage can only be specified with the `var` binding:
- `const` variables are assigned at compile time and non-editable, so it's pointless to copy immutable values.
- `let` module variables must be assigned during a module's initializer, and then never change; module initializers only ever run once.

Thread local variables can be pre-computed at compilation time too. Any thread-local variable used by the runtime that is set in the main thread of the compile-time program will be initialized from the main thread's ending value, as long as a runtime thread might not overwrite that variable before use.

Generally, to pre-compute a thread-local variable, you can add a comptime-specific code block to the module initializer, and then simply not set the variable in the runtime initializer:

```n00b
thread var starting_seed: uint

if comptime() {
   import util
   
   starting_seed = util.random()
}
```
# Program analysis and checking
The n00b compiler relies heavily on static analysis:

- Modules are staticly, incrementally type checked, and must be sound to compile.
- Control and data flow / dependence information is computed at the module level, and used to enforce correctness.
- Integers go through range analysis, used for automatic sizing and correctness checking.
- Code effects (including user defined effects) are staticly traced throughout the code, and validated.
- n00b's analytics are incremental, always performing whole-program analysis for compiled executables.

We use static analysis to promote more correct code, both by deeper checking than typical, but also by using analysis to automatically document programs with relevant facts.

Beyond any code editing, we add detailed analysis summary data into object files (as data), that gets leveraged in incremental analysis, and in final executable assembly (where we then delete most of the data).

The n00b compiler itself doesn't currently doesn't focus very much on using the analysis to optimize code. Instead, n00b's internal IR gets translated to some other representation -- right now, just LLVM's MIR. There, another layer of analysis does any optimization.

When running in a REPL, we leverage analysis done on anything that gets imported, but the interactice session's content more heavily leverages dynamic checks.

## Type inferencing and checking
In n00b, we have a bi-directional type system, meaning that, as we process a module top-down:
1. Facts we've learned from above get checked for consistency against actual usage.
2. Where we lack hard facts, we infer them based on usage, essentially propogating information back up.

For example, considering the following function:

```
func sum(*items: int) -> int {
  let l: int = item.len()
  
  if l == 1 {
      return items[0]
  }
  if (l != 0) {
      return items[0] + sum(items[1:-1])
  }
  return nil
}
```
We've provided explicit type information for the `sum` function. It's intended to be a varargs function that you can call as such:
```n00b
sum(1, 2, 3, 4, 5)
```
We've specified the varargs type as `int` (our first fact), which means the variable `items` will be an `Array[int]` (our second fact). We see one more fact-- the return type is `int`.

The declaration of `l` gives us a new fact-- `l` is an `int`. 

When the compiler sees the call to `items.len()`, it alredy knows the type of `items`, so *checks* that it's valid to call `len()` on an `Array[int]`. Of course it is.

It then checks the assignment to `l` to make sure `item.len()` can be safely stored in an `int`, which it can. The two conditional tests will check that the comparison operators are comparing compatible types. The first two return statements check that array access and slicing are valid, that the recursive call to `sum()` is valid, that the addition is valid, and that the two `return`'d values are valid.

All of that checks out.

But, when we see the second return statement, we check the type of `nil` (which is `nil`) against the declared return type. Since `nil` is not a valid value for `int`, the types are NOT compatible, so type checking fails!

We could fix this simply by returning `0` instead of `nil`, and our program would check just fine.

Now, let's take our corrected program, and  *erase all our type info*:
```
func sum(*items) {
  l = items.len()

  if (l == 1) {
      return items[0]
  }
  if (l != 0) {
      return items[0] + sum(items[1:-1])
  }
  return 0
}
```
This program also fails to compile, but the way it gets there is different.

It starts out with only the fact that `items` is an array. It doesn't know the item type, and it doesn't know anything about the return.
When it sees us call `item.len()`, it does make sure this is valid to call on an array, but doesn't learn anything new about the item type.

Similarly, returning `items[0]` teaches us nothing new. It's not until we process the addition that we learn something new-- whatever the item type, it *must* be something we can `Add`.

When we see a literal `0`, inference then realizes that items are integers (floats require a decimal point). Knowing nothing else about the inputs, we decide to go with the default `int` type we use when the result can be anything, which is always a 64-bit type.

The `return 0` allows us to *infer* the type, but when doing so, we still check it against everything we know from before. So information is a two way street.

## Example: Generics, Traits and Options
Let's write `sum` a slightly different way, where the `nil` would make sense:

```n00b
func sum(*items) {
    switch items.len() {
        case 0:
            return nil
        case 1:
            return items[0]
        default:
            rest = sum(items[1:-1])
            require rest != nil
            return items[0] + rest
    }
}
```

You will find this function type checks just fine... even though it has very little idea about the type.

We can then call the function multiple ways:
```n00b
let nums = sum(1, 2, 3, 4, 5)
let strs = sum("one ", "two ", "three ", "four ", "five")
```
Currently, `nums` and `strs` will end up being typed `int?` and `string?`, because that's the overall type of the function (though this hopefully will be able to specialize automatically soon).

## Effect system
The analysis engine tracks interesting properties of code, and how those effects propogate in programs. We use this internally for correctness checking, optimization, etc. You can query this information at compile time, and you may define your own effect toos.

n00b tracks two different kinds of effects:
- **Call effects**. For instance, system calls that perform I/O are tagged, and we follow those tags through control flow to understand what pieces side effect, and which don't.
- **Data marks**. This can be used for tracking data flow, for instance, to make sure input validation happens from untrusted input.

Effects are tracked at compile time, so they don't impact performance.

### built-in effects

| Name | Description |
|--|--|
| `input` | Does a function read external input, for instance from a socket? |
| `output` | Does the function output (non-debug output)? |
| `debug` | Does the function perform debug outputting? |
| `env` | Does the function use the program's environment variables or command line args? |
| clock | Does the function read any sort of timestamp? |
| random | Does the function read random bytes? |
| reads_globals | Does the function read module-scoped variables with `var` binding? |
| writes_globals | Does the function write to such variables? |
| `blocks` | Can the call block? |
| spawns | Does the function spawn threads? |
| `allocates` | Does the function allocate memory? |
| `collect` | Can the function trigger the garbage collector? |
| `panics` | Is there behavior that can stop the program, like an indexing error? |
| `noreturn` | Is it possible for the function to call and never return? |
| `impure` | A combination of many of the above, indicating whether the function has observable side effects at the time of execution |
| `nondeterministic` | Tracks when functions perform any kind of computation on non-computed input. |
### Querying effects
n00b provides a compile-time only API for querying effect information. These functions all evaluate to a boolean `true` or `false` at compile-time.

| Builtin | Description |
|---|---|
| `may_effect(f, e)` | some path through function `f` might perform effect `e` |
| `may_effect(e)` | Same, for the current function. |
| `forbids_effect(f, e)` | Returns `true` if the effect cannot happen in `f`. |
| `forbids_effect(e)` | Same, for the current function. |
| `has_effect(f, e)` | Returns true if the effect always happens in every success path (i.e., non-`fail`) |
| `has_effect(e) | Same, for the current function. |
Note that queries over our built-in roll-up groups, `impure` and `nondeterministic` currrently only works with `may_effect()` and `forbids_effect()`.

Additionally, there is a compile-time interface for data flow information:
| `may_read(f, p)` | `f` may read data accessed through its parameter `p` |
| `may_read(p)` | Same, for the current function. |
| `may_write(f, p)` | `f` may change data accessed through `p` |
| `may_write(p)` | Same, for the current function. |
| `may_lend(f, p)` | Determins if any sort of reference to `p` can be held after `f` returns. |
| `may_lend(p)` | Same, for the current function. |
| `may_carry(x, m)` | Tests whether the variable `x` can cary the data mark `m` (see below).|

Both of these APIs work with custom effects.

> Effect queries are evaluated after the preprocessor runs, so you cannot use them inside a preprocessor conditional (e.g., a `when` statement).

While typically n00b requires return values to be used, they do not need to be with this API. If an expression containing an effect query is unused, then the entire statement is treated like a compile-time assertion; if it ever is not true, then compilation will fail.

For instance, a big hot-loop function you might design to never do anything that blocks. You can allow asynchronous I/O, but forbid anything that can possibly wait with: 
```n00b
func compute_it() {
    forbids_effect(blocks)
    ...
}
```

If effect checks are put inside the body of a `trait` declaration, and you explicitly apply those checks to functions defined in the trait, then they must be true for any implementation. For instance, you could define the following trait:
```n00b
trait Render {
    func render(self: 'T) -> string
    forbids_effect(render, impure)
}
```
Since the trait forbids impurity, the following implementation of the trait would fail:

```n00b
object Example {}

func render(self: Example) {
   print("What's up?!")
   return "Lorem Ipsum"
}
```
```sh
example.n:38:1: `render` implements `Render.render`, which must never have effect impure, but:
  example.n:39:5: render calls `builtin.io.print`, which may have effect `impure`.
  render.n:3:5: note: Render.render forbids impure here.
```

### Custom call effects (the `effect` statement)
You absolutely can define your own custom function effects, using an `effect` statement (which is only valid in the top-level of a module).

For instance:
```n00b
// log.n
func log(entry: string) {
    ...
}

effect Audited {
  from: log
  scope: caller
}
```
This effect defines `Audited`, propogating it only to functions that call `log`. 
The `effect` block allows you to specify the followng properties:
| Property | Description |
|--|--|
| `from` | Functions the effect "comes from". |
| scope | The *scope* of the effect is how far through the call chain it propogates. The default is all the way up the call stack (`thread`). You can explicitly specify `caller` to propogate only one level, or `package`, to propogate through any calling functions within the same package |
| handled_by | Allows you to specify another other function that shuts off the effect, overriding the scope |

The `handled_by` property allows you to track begin/end spans of the program's flow, which can help ensure functions are called in a correct order.
Effect queries from the command line
Once you've defined an effect, you can easily see from the command line where the effect is applied using an *effect query*.

For instance, if we want to see which functions in package `PKG` skip logging:

```sh
n00b effects 'may_effect(impure) and not may_effect(Audited)' --in PKG

pkg/handlers.n:12:6  app.handlers.upload
pkg/handlers.n:40:6  app.handlers.purge
2 functions match
```

Since parentheses may have special meaning in your shell, it's best to put your queries inside parentheses.

Without the `--in` flag, the query covers the *default package* -- the application code found in your current working directory. By default, we don't show query results from system packages, unless you explicitly request them with `--in all` or `--in PKG`.

`--in` can also take functions or module names, and can appear multiple times. `--not-in` can be used to rule out things you already used `--in` to include, e.g. `--in unicode --not-in rich`

### Custom data marks
So far, we've discussed *function effects*, which follow control flow from a function, through its call chain.

But it's also very interesting to track how data flows through a program, for instance to ensure proper input validation.

The n00b `effect` system allows you to track data flows by declaring a `mark`:

```n00b
// untrusted_example.n

mark Untrusted {
    from: read_input
    cleared_by: check_path(path)
    rejected_by: open(path_to_file), read_dir(dirname)
}
```
Some notes:
- `from` indicates the return value *from* `read_input` carries the mark.
- Items listed under `cleared_by` or `rejected_by` must be functions (which may be fully qualified), and you must specify the name of the parameter, as declared, that your rule matches.

The data flow analysis survives data dependent computation:
```n00b
import untrusted_example

func bad() {
    let name: string = trim(read_input("HOME"))
    return open(name)
}
```
```
bad_example.n:5:12: error: `name` may carry `Untrusted`, but `path_to_file` of `open` rejects it
  bad_example.n:4:29: note: `name` comes from `read_input`
  bad_example.n:5:24: note: `trim` passes its argument through to its result
  help: check it with `untrusted_example.check_path` first
```

### Applying effects to code you don't control
While `effect` blocks should be provided in the package defining the effect, that's not always possible. In fact, you might want to apply built-in effects to new functions, for instance when wrapping functions with the n00b Foreign Function Interface (FFI).

You can open up any existing effect to add more information about it. For instance, if you were to FFI-wrap a function for uploading metric diagnostics: `send_metric(name: string)`, you could just add under your FFI import:

```n00b
effect output {
    from: send_metric
}
```

That's all you need to do to now have `send_metric()` be tracked as a function that outputs.

Note that for effects that are not built-ins, you should fully dot-qualify the module the effect was originally declared in, otherwise the system will seprately track two effects with the same name, which might get confusing when they both get applied in overlapping places!

# Assignment Operations
n00b supports `=` for direct assignment, as well as "augmented" assignment operators, `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`.

The direct assignment operator directly copies its underlying stored value. That means, reference types copy the reference; value types pass a value.

Specifically, types created with `object` are reference types, as are any values of type `Ref['T]` (which are created via the `ref` keyword). All other types (user-defined or not) have their values copied (but if the value contains references, those references will be copied).

In some cases, an assignment will be handled via trait dispatch. For instance, assignments via indexing, e.g., assignments like `arr[12] = 100` or `dict["foo"] = 12` go through the `Index_Write` trait (i.e., implement `__write()`); slices can also be assiged via the `Index_Write_Slice` trait (by implementing `__write_slice()`).

Ordered, anonymous tuples can be unpacked, like in Python. For instance:

```n00b
a, b, c = (1, "two", 3.14)
```

Parentheses can be optionally added to the left-hand side, which helps if decomposing tuples:

```n00b
(a, b, (c, d, e, f)) =  (1, "two", (3, ".", 1, 4))
```

n00b also reserves the variable `_` for *discarding*, meaning you're not planning on using the value. This can be used in tuple unpacking:
```n00b
(a, _, (b, _, _, c)) = (1, "two", (3, ".", 1, 4))
```
# Function Definitions
## Basic syntax and parameters
In n00b, functions can take positional parameters, variable arguments (aka varargs), and keyword parameters.

In n00b, keyword parameters are optional for the caller, so are parameters that have a *default value* provided at the time the function is declared.

To make sure declarations are clear, we don't allow mixing positional parameters and keyword parameters in the declaration. The rules are:

- Positional parameters (if any) must be declared first.
- If the function takes variable arguments, it must come after any positional parameters, and the name is prefixed with an `*`, like in Python, to make it clear it's a variable argument parameter.
- Keyword parameters (if any) are declared last.

The `varargs` parameter can be thought of as syntactic sugar at a call site to batch items of like type into an array. One consequence, all variable arguments must have a single, uniform type.

Variable arguments can have any number of parameters, including none at all. To define something like `print()` where one parameter is required, you can use a single positional parameter. For instance:

```n00b
func print(first: string, *rest: string, end: string? = "\n", sep: string? = nil)
{
   ...
}
```
Or, if you'd rather make `print()` more general purpose by having it take anything it can convert to a string via a `Printable` trait, you could do:
```n00b
func print(first: Printable, *rest: Printable, end: string? = "\n", sep: string? = nil)
{
   ...
}
```

However, type information can usually be omitted; the compiler attempts to infer parameter types, and the return type.

If your code dynamically dispatches, that requires a trait. When we see the dynamic dispatch, we know that, as long as the function is unambiguously part of a single trait that's in scope, we can infer the trait type.

## Returning success or failure
When functions in n00b finish running, they generally either *succeed* or *fail*. We have first class failure; the classic `return` statement indicates success, and the `fail` statement... failure.

Functions that can both return on success and failure only ever return one thing, never both.

And, each return channel is typed seprately, so you can do things like:

```n00b
func make_thing(handle) {
   let code = some_wrapped_call(handle)

   if (code) {
       fail code
   }
   ...

  return new[Thing](handle)
}
```

When a function might fail, then callers must check whether failure occurred with either a `try` block, or with the `!` operator; see [Calling functions](#Calling functions) below.

In the above code, there's enough information to determine the function's signature automatically, but if you can explicitly declare the two return channels:
```n00b
func make_thing(handle: string) -> Thing fail: int {
    ...
}
```
Here, `->` is common syntax for denoting a return type, but you can also be more explicit with `return:` if you prefer.

Note that the two return channels can be different types. This is similar to languages that conventionally use `Result[success_type, fail_type]` to allow failure to yield a separate type.

Of course, there's no requirement for functions to provide values, successful or not.

It's often the case that a value needs to be returned on success, but failure needs no context, other than, "did this fail or not".

In cases where failure requires no context, the recommended convention is not to use the failure channel at all, but to instead make the return value *optional*.

For instance, consider a traditional string search function that returns a codepoint offset to the first match, but can not match at all. Return a `uint?`:
```n00b
func find_first_match(haystack: string, needle: string) -> uint? {
   var match_start_index: uint
   
   for ... {
      if found {
          return match_start_index
      }
   }

   return nil # no match
}
```

Again, types can be omitted. The checker just figures it out.

The caller still is forced to explicitly handle the error case, but by checking whether the result is `nil` instead of adding bulkier error handling code.

Whenever a function returns no value, the declaration should not write any type, but may write `-> void`. Similarly, when the function can never fail, the declaration shouldn't declare a failure type.

> Functions that return, must return something of a consistent type in all successful paths through the function. Similarly, when a function can fail, all failure paths must yield a consistent type.

## The `result` variable
Occasionally, it's cleaner and more clear for a function to set up a default return value, and only override it in a small fraction of paths.

n00b supports that, by allowing you to assign to a pre-defined variable named `result`.

If your function doesn't explicitly have a return statement, n00b automatically behaves like you wrote, `return result` (unless the function can never return a value).

And, if you exit early from a branch with a bare `return`, the same thing happens; the program treats that as if you'd written `return result`.

You can explicitly re-assign to `result` if you need to override the default, but you can also just `return override`. For example:

```
func dumb_example(input) {
   result = "positive!"

   if input <= 0 {
       if input == 0 {
         return "zero"
       }
       result = "negative."
       return
   }
}

print(dumb_example(-12))
```

Here, you can see we used both override approaches; and the positive case omits an explicit return statement (which we could also do for the negative case).

If you set `result`, but an exit path instead exits via `fail`, `result` is abandoned; we return the failure type instead.

In n00b, `result` is synonymous with a successfully returned value. Meaning, you *cannot* explicitly declare `result` yourself. In fact, you can think of `return zero` as shorthand for `result = 0; return`.

There is *no* corresponding variable for failure; the `fail` statement must always take an explicit value, if used.

## Calling functions
You can call functions directly using typical function calling syntax (i.e., explicitly naming the function and providing an argument list).

Direct calls are the primary way of calling functions, but n00b also allows evaluating callbacks (see [Callbacks](#Callbacks) below), and dynamic dispatch through [traits](#Traits).

If you're not passing arguments to a function, you still need to make sure the function call is explicit by providing parentheses, e.g., `x = get_page_size()`.

When calling a function, it's typical to provide positional parameters in positional order

> For those familiar with functional languages, two things to note. First, we do not allow omitting the parentheses, because that tends to be harder to read for beginners. Also, we explicitly do not support currying / lazy evaluation.

## Parameter passing mechanisms
Function parameters can specify a *parameter passing mode*, which essentially denotes what the function guarantees about how the data will be handled.

Some languages only have a single parameter passing mode-- the value the caller provides is copied from the caller *into* the function's context. This is called *pass-by-value*, and is the default mode for n00b.

Pass-by-value allows the function to make its own changes to the value. But, if you pass in a complex data object by reference (e.g., by passing an object in Java or a pointer in C), the function still can change data inside the object through the provided value-- the thing they get a copy of there is just the object reference (Java) or pointer (C). They do not get a deep copy of all the data being pointed to.

With pass-by-value, if you pass in an integer value (say `42`), the function sees `42` at the start, but can reassign to, say, `24`. That change is not reflected in the caller's copy. Again, pass-by-value *copies in* the underlying variable stored, the way it is stored.

For example, in n00b, data types declared with `record` are value types, and data types declared with `object` are reference types. Meaning `object` is for when multiple holders are going to want to share a single view, and `record` is for when everyone should get their own copy.

A variable containing an `object` type only holds a reference; a variable holding a `record` holds the whole record.

Sometimes, you may explicitly want the function to work with the same copy of a variable that the caller has. This is common enough, that many languages have a separate parameter passing mechanism, **pass-by-reference**. For instance, C++ has them; you denote them with an `&` in the parameter declaration. So does C#, denoted with `ref` in the parameter declaration.

C doesn't have reference parameters, though. Instead, you can simulate it by passing pointers by value. That seems good enough to many people, because that's mostly how one would expect a compiler to implement pass-by-reference.

However, we shouldn't confuse how the language chooses to implement parameter semantics from the *guarantees* the language makes with parameter passing modes.

For instance, Ada has always had multiple parameter passing modes that can potentially be implemented through references, but help with program checking, by explicitly stating how data flows from the caller into the function.

Ada has three parameter passing modes, `in`, `out` and `inout`. The `in` mode is simply pass-by-value, called `in` because you're *copying in* data. With `out` mode, the programmer's mental model is that the function is responsible for initializing the variable for the caller (in fact, is required to do so). With `inout` mode, the caller *must* provide an initialized value, and the function *may* change contents. If it does, the caller will see those changes on return.

In n00b, we go beyond even Ada. The default is akin to ada `in` parameters (but you do not need to write `in`). We also have `out` parameters, and our name for the third option is called `modify`. But we add two other parameter passing modes, `view` (the caller cannot make any field edits; its use will be truly read-only), and `consume` (the caller will never use the data after the callee is done).

Here's the summary view of he modes:

| Mode | Callee can read | Callee can reassign | Callee can edit the value | Caller loses access |
|--|--|--|--|--|
| in | yes | no* | Their own copy only. Object state is writable. | no |
| view | yes | no | Not even for objects / refs! | no |
| modify | yes | yes | yes | no |
| out | no, until assigned | It *must* (expected to init) | yes | no |
| consume | yes | yes | yes | yes |
In Ada, semantics on non-`in` parameters are, "copy in, copy out", which means that, if a function fails, the caller does not see any changes that might have been made before the failure.

For n00b, `out` and `modify` parameters have full reference semantics; while the language doesn't *need* to implement using raw references, you can assume it behaves as if it did. Meaning, writes to such a parameter will be reflected in the caller, even if the call eventually `fail`s.

We do require that `out` parameters get set on any *successful* exit (but not on failure). However, `modify` parameters don't have the same strict requirement on the function. We just require the function both read and write on *some* path.

The caller has more strict requirements for `modify`; it must guarantee that the value is set before-hand, and that the value is subsequently used after the function returns.

### The `view` parameter passing mode

The `view` mode is what many people expect from `pass-by-value`, but do not end up getting, any time a parameter might hold a reference somewhere inside it. It promises deep immutibility, in two senses:

1. No matter how nested or indirect the parameter is, no reference accessed through it will be changed because of that call.
2. No matter what delegation the function does, *no* caller will make any modifications.

To truly keep this promise, `view` must also promise that no references are going to be held after the call returns, in any situation.

Implementing `view` properly requires somewhat heavyweight program analysis, so can slow down build time. However, it not only promotes correctness much better, but also makes it easier to optimize runtime. For instance, when the language sees a `view` parameter, it knows that no data needs to be copied from it for safety. It can always optimize by passing around a reference with no run-time work.

In n00b, there are cases where the compiler may not be able to prove that given code would live up to the guarantee, even though the implementation never would violate the contract. In such a case, n00b is conservative; you'll get a compilation error.

### The `consume` parameter passing mode

The idea behind the `consume` parameter is that, once the function returns, the value is eaten; it won't be used again. Our analysis requires us to prove that, after a value is passed to `consume`, it won't be used again.

This can be useful for making sure finalization code is run in a timely manner, but it's in n00b to promote correctness, by enabling the *typestate pattern*. The basic idea is to statically enforce any kind of state machine, by having the API "consume" the object associated with the old state, and replace it with a value representing the new state.

For example, we could imagine a TLS library where, once you connect, you need to authenticate before you can read / write. And, once you close a connection, you might have access to summary statistics, but not have any API to read or write.

All of this can be statically enforced, as long as the state transition is done in APIs that declare the old state object `consume`, and return a new state object.

The caller's code would then look something like:

```n00b
# The connection can still fail, which we could explicitly destructure with
# a try() block.
let sock       = mytls_package.connect(host = my_host, port = my_port)!
var connection = authenticate(sock, ...)
var summary: TLS_Closed

// Accessing `sock` here is a static error.
connected:
while true {
    # next_state is a variant type, either a replacement TLS connection object
    # (which can be the same object as long as the caller doesn't assume it),
    # or an error object.
    #
    # We can `switch` on variants by naming every arm; `next_state` automatically
    # destructures to the right type properly in each switch branch.

    let next_state = tls_send(connection, msg)
    
    switch(next_state) {
        case err:
          summary = next_state
          break connected
        case connection:
          connection = next_state
    }
    
   // read and write on connection; also, at some point, run something like:
   if close_desired {
        summary = close(connection)
        break connected
   }        
}

// Can still access session summary details if desired.
print(summary.bytes_sent)
// Accessing `sock` or `connection` here is a static error.
```

## Function overloading for direct calls

In n00b, every function declaration must be unambiguously named. This means:

1. One module cannot declare two functions that share a name, even if their parameters are different.
2. If an import contains a name defined locally, the imported function cannot be directly called (though you can rename or fully qualify the path).
3. Every direct function call is statically resolvable when the module is built.

Even though direct calls cannot be overloaded, dynamic dispatch can be; see the section [Traits](#Traits) below for more detail.


# Objects

n00b **doesn't** consider itself an *object-oriented language*. However, n00b does *have objects*.

### Issues with "object-oriented"

In many people's minds, *object-oriented* (OO) means *has inheritance*, where one data type can explicitly *inherit from* one or more existing data types, where that inheritance gets them both an implementation and type all in one go.

Also, many people think objects as containing data and *methods*, where methods are function-like operations, that only operate on instances of the type.

In such languages, methods are often invoked with different syntax from functions; the `.` operator on the object typically accessess fields, but also those methods.

Then, derived types can replace methods in their base types to add specialized behavior. 

In such languages, methods often can implicitly reference the object through a special parameter, like `this` or `self`.

In a career of programming, I've found traditional inheritance is almost never the correct reuse tool. My two biggest issues in a nutshell:

- Mixing implementation inheritance and type inheritance makes it impossible to make STATIC type safety intuitive for function calls. I won't go into detail here, but this is commonly called the *variance problem*.
- Dervied types can very easily break in subtle ways without any compilation errors, if the implentation they rely on in base types changes in a way that invalidates the inheriter's assumptions on the implementation (the *fragile base class problem*). This is a real problem, even if the interface doesn't change at all.

In general, implementation reuse is overrated. What's more valuable though, is to have specific concepts be operations that many types can implement, and then be able to treat objects from many different concrete types through a single, uniform interface, without having to worry about the implementation details, as long as they implement an abstract interface associated with that concept.

For example, we should be able to enumerate items for any data object that is conceptually a container, without having to remember one API per object, and without having to worry about implementation-specific differences.

## The n00b object model
While n00b may not consider itself *object-oriented*, it does have have the notion of `objects`, and some other features that are commonly considered part of the *object-oriented* paradigm:

1. User-defined data objects can be shared-by-default like in `Java`. In n00b, we do that with the [`object` statement](#Objects).
2. We do allow for interfaces that represent conceptual properties of objects, through the [`trait` keyword](#Traits), allowing code to act on a variety of types uniformly as if they were one type (supporting dynamic dispatch).
3. We support [*generic types*](#Generic types) (templates in C++) and *generic functions*, which allows you to write one implementation of a data type (and operations performed on it), all while allowing static type safety.
4. We do support *dynamic dispatch* based on types, but without inheritance or function overloaing.
5. The [`new` operator](#The new operator) instantiates types, and can call type-specific initializers.
6. We support [finalizers](#Finalizers) to tidy up, when allocations are about to become non-accessible.
7. We do support information hiding, such as `private` fields; (see the section [Symbol Visibility](#Symbol Visibility))
8. We do support some forms of implementation re-use along with type composibility, through [*wrapper types*](#Wrapper types).


Despite the above features being associated with the OO paradigm, not only do they all show up in non-OO languages, almost all of them pre-date C++ (which popularized object-oriented programming).

> The exception to both is perhaps wrapper types; they're somewhat novel to n00b, but are more inspired by the *aspect oriented* paradigm, and mathematical composition.

To be clear on differences between n00b and what we consider OO languages, n00b does not support:

- The concept of inheritance.
- A closed set of methods bound to concrete types.
- Function overloading.
- Static classes of any sort.

## Traits
In n00b, a `trait` is an explicitly declared interface, allowing you to treat other concrete types like they're a single, uniform type.

For those familiar with C++ concepts or interfaces in Java or Go, it's in the same family of constructs, but is not quite identical to any of them. It's closer to Rust traits, still with two significant differences:

- n00b traits are *duck typed*. Another way to say this, is the type defined by a trait is open-world; they describe anything that matches it, and don't need to be statically enumerated / known to the trait definition.
- n00b traits are dynamically dispatched (though, the compiler will almost always optimize to static dispatch at link-time, unless dynamic loading is supported).


Builtin traits

Traits allow multiple types to export different implementations of the same function, but with different parameters.

In that case, different modules will export functions with the same name; the different implementations don't even need to import modules containing possible targets directly (as long as the trait definition is somewhere accessible in the scope, even if dot-qualified through another package).


Allocation
Constructors
Finalizers
Dynamic dispatch

# Memory management

# Builtin data types
## Integers
## Floats
## Character Types
## Strings and Buffers
## nil
## Options
## References
## Arrays
## Lists
## Dicts
## Sets
## Atomic
## Lock
## Any

v# User-defined types
## Variants
## Ordered Tuples
## Anonymous Tuples
## Records
## Objects
## Enums
## Flagsets
## Traits
## Callbacks
## Type aliases and distinct types


# Automatic Type Coercions
There are a few cases where n00b will perform automatic coercion.

First, option types get no-cost automatic coercion statically, based on you adding code that proves whether something is non-nil or not.

For example:
```n00b
func get_or_else(opt: 'T?, default: 'T) -> 'T {
    if (opt == nil) {
        return default
    }
    else {
        return opt
    }
}
```
The conditional test for nil-ness automatically coerces `opt` to `nil` in the `true` branch, and to `'T` in the false branch (which is why we can return `opt` directly, instead of having to explicitly unwrap the reference.

The same automatic coercion happens when you're testing types via `typecase`:
```n00b
func get_something(x) {
    typecase(x) {
        case int, uint:
            return x + 4
        case string:
            return x.len()
        default:
            return 0
    }
}
```
We also automatically coerce `Buffer` to `string`. Since `string` is immutable, and `Buffer` is not, this coercion allows them to share read-only operations.

However, this coercion is not zero-cost, unless the analysis can prove a buffer cannot be accessed in its live as a `string`. In such cases, there's some synchronization to copy when needed.

Additionally, you may go back and forth between `'T` and `Atomic['T]`; The `Atomic` type is special in that it really describes the storage and access requirements for fields and variables that are assigned the type. You can go freely between, but via an atomic operation that cannot work on part of the variable.

Types that implement a trait can always be cast to that trait.

Finally, unordered tuples automatically coerce to any structurally compatable `record` and `object` types in their own package visibile to the module, or, from within other packages that have imported the package... as long as there are no private fields and no constructor.

Though discouraged, if `Any` is explicitly assigned to a variable, field or parameter, anything can be placed into it, though retrieving the value always requires a run-time type check via typecase().

# Type Casting Operations
There are cases where you can choose to convert between incompatable types by explicitly casting a value -- an inline declaration to change the type.

n00b has two different cast operators, `cast[T]` and `unsafe_cast[T]`.

The `cast[]` operator is for safe conversions that are well-defined, but are not automatic coercions, to help ensure correctness.

The current list of valid `cast[]`s:
| Destination type (casting to) | Origin type (casting from) | Description |
| `int`/`i64` or `int128` | `f64` / `float` | TRUNCATES any fraction. Since floats are not range-tracked statically, this cannot go into a smaller type or an unsigned type without an unsafe cast. |
| `i32`, `int/i64`, `i128` | `f32` | Truncates. |
| `char` | byte, u8, u16, * | Other int types allowed; traps if too large |
| byte | u8 | Other int types allowed, but trap if too large |
| f64 | f32 | Widening |
| f32 | f64 | Narrowing w/ loss of precision; INF/-INF on overflow |
| string | Buffer | redundant due to automatic conversion rule |
| string | char or byte | Results in a new short-string of the one codepoint |
| int, etc | enum / flagsets | Any conversion to int is okay IF the destination holds the range associated with the enum. The reverse cast is unsafe. |
| Array['T] | List['T] | Results in lock or copy depending on lifetime |
| Atomic['T] | 'T | Redundant with automatic conversion |
| 'T | Atomic['T] | Redundant with automatic conversion |
| Record / object type | Compatunordered tuple | As long as the module has visibility to all fields, otherwise is unsafe |
| 'T | Distinct type created from 'T | Always Allowed |
| Distinct type created from 'T | 'T | Always Allowed |
| 'T | 'T | Allowed, but a warning because it's super redundant |
| Any | 'T | Allowed; 'boxes' the type. In reverse, must be narrowed via a proof via `typecase()`  or `has_type()` |

The `unsafe_cast[]` operator can be used for any types with the same LAYOUT, and has the semantics of directly re-interpreting bits. For numeric types, this means 'same storage size'. Mostly this is used for converting anything that is held by reference to an unsage ref, as an escape hatch in things like garbage collection and other memory managers.

Additionally, `unsafe_cast[]` can be used to force type coercions in types in lieu of a proof, for instance with options, variants and narrowing from any generic type to a type compatable with known constraints. 

## Run-time type information
typeof(expr)` evaluates `expr` and returns a `typespec` descriptor for its most specific runtime type. If `expr` already has type `typespec`, `typeof` describes that descriptor value; it does not inspect the type represented by the descriptor.
Two type descriptors are equal exactly when their deterministic type hashes are equal. Descriptor equality does not test compatibility or unification.
`has_type(value, spec)` returns `bool` and tests whether the value satisfies the type or trait described by `spec`. It is not a type-descriptor equality test, and it evaluates its first argument once.

# Special variables (distribute to functions and assignment?)



# Data layout
# Context Managers
# Litmod definitions
# Wrapping
# Trait Composition

# Control flow
## when
## if
## for
## while
## switch
## typecase
## continue
## break
## Control flow labels


# Operations
## Assignment
## Arithmetic operations
## Bitwise operations
## Allocation
## Taking a reference
## Indexing
## Slices
## Function calls
## Dynamic dispatch
## Hashing
## I/O
## Iteration
## Inlining
## Ranges
## new operator / allocation
## pass
## with

## import Statement
## bring Statement
## package Statement

start() / comptime_start()

# Error Handling
- Try
- !
- fail
Debugging
- assert
- require
- unreachable
# Command line
# FFI
# ASM

# Appendix 1: All type rules

Academic papers describing type systems provide formal definitions for their important type rules.

We like that, but also think it's useful to provide plain-english descriptions of all our important rules.

## What is not considered a type (that may be in other languages)
- Parameter passing modes for functions.
- Effects (including data marks).
- Any other modifier to functions.

## The process
- Typing is applied late in the analysis process, even after we've doing our integer range analysis (whether it's tracking direct values held or bound info on arrays, etc). For those who understand compiler internals, this specifically means, after we've normalized code into an intermediate representation in single-static assignment form.

## Variables
- If a variable is explicitly declared with a type, that type is assumed to be correct; all other information is checked against it, and it's an error to see usage that is inconsistent with a declared type (*typing is bi-directional*).
- The first time we see a variable in a given scope referenced, we assign it a fresh type variable, even if it was explicitly declared. Tracking this seprately from a declared type helps us check to make sure declared types are *exact*.
- The overall type of a variable must always hold, but we keep a detailed picture of what type the varible may have at every point it is used. For instance, in the code `if (...) { x = 2 } else { x = nil }`, we track that `x` is `int` in the `true` branch, and that it is the `nil` type in the second branch. Also, after the code block, the two possible types for `x` are *joined*, meaning we conclude `x` could hold EITHER type.
- When we *join* two types associated with a variable, if one of the types is `nil` and the other is not, then the resulting type is an option type (e.g., `int?`).
- If we see `nil` assigned to a type anywhere in its scope, and see no other type assigned to it, then the end type is `nil`.
- Variables that are module-scoped (i.e., outside of functions) must not be generic in any way (i.e., they cannot need more information than available in the module's source in order to instantiate them).
- Additionally, module-scoped variables cannot be anonymous unions of non-`nil` types (meaning, `x: int | string` is not allowed).

## `typecase`
- `typecase(x)` requires a variable as an argument, where that variable might take different types at runtime, and tries branches in-order until a match is found.
- For each branch of the `typecase` with a concrete type as a case, the variable's type is refined to be the named type, inside the case block (and, if appropriate, inside any other control dependent code).
- The `else` clause does not refine the type of the argument.

## Casts
- Casts must follow the above guidance detailing allowed conversions.
- The cast expression gets the exact return type specified as the type parameter to the `cast` operation.
- `unsafe_cast` is meant for reinterpreting the underlying storage. It therefore requires the input type and output type to be compatable.

## Integer types
- Without a type declaration, integers are tracked as one type until the compiler needs to map a type to storage, at which case we consult range information to select storage. In any examples below, we will write `int` as a type for shorthand, but the exact type selected may have a different size / sign depending on the best fit.
- Arithmetic operations on integers update our range anaysis, but never results in us infering a storage type. For example, if we have two `u64` values multiplied together, where both operands could be any value, then we conclude the result can be anything from `0` to `(2^64-1) * (2^64-1)`
- When an integer type is explicitly declared as a parameter or field in a type, our range analysis assumes anything from the minimum value to the maximum value allowed by the declared type may be passed.
- `+` and `-` operations on 64-bit values never widen a 64-bit type (even if explicitly declared). Instead, the generated code checks for overflow.
- `cast` from variables of type `u64` (`uint`) to `i64` (`int`) are allowed; if range analysis cannot prove safety, a runtime bounds check is inserted.
- `unsafe_cast` between unsigned and signed integer types, while discouraged, is allowed without check. (Note that large positive numbers can become negative going u->i, and going i->u, negative numbers become large positive numbers).
- The 'bitwise' operators are not declared on unsigned types.
- If a variable isn't declared, and our range analysis never sees explicit use of absolute values above `INT64_MAX`, it will be clamped automatically to a 64-bit value.
- Otherwise, when mapping inferred integers to storage, we use a *best fit* policy, meaning we choose the smallest integer type that fully contains the range of possible values. 
- Currently, the language has no direct support for integers that cannot be represented in types larger than supported by the underlying system ABI (usually 128-bit int types).

## Other builtin primitive types
- Floats have no value tracking applied, whatsoever.
- Float literals with an unspecified type are given the `float` type (aka, `f64`).
- Math operations combining an `int` and a `float` automatically result in a `float` type.
- `cast[int](f: float)` results in `f` getting truncated to the nearest integer.
- `Buffer` automatically coerces to `string` when passed as a parameter; this is implementing via copy-on-write when the `Buffer` is in use as a string (i.e., we copy if and when the buffer is subsequently modified).

## Option types
- Testing an option against `nil` decomposes the type for all code that is control-dependent on the outcome. In the code, `if x == nil {} else {}`, the test is considered **proof** that the type of `x` in the `true` branch is `nil`, and that x's type is the non-option component of its type in the `false` branch.
- Assignment of a non-nil value to a variable that might hold an option is considered a **proof** of non-nilness in dependent code.
- Similarly, assignment of `nil` proves nilness.
- Option types are non-nestable. Meaning, `int??` is not a type. For instance, we have a case where, in one branch of a condtional, `x` is assigned from a parameter explicitly typed `int?` and another where we explicitly assign `nil`, the resulting type after the block runs is simply `int?`.


## Function Declarations
- A function's *result type* is the type returned from the function on any path through a function that results in the function's successful return.
- All paths through a function that successfully return must return a uniform (compatable) type.
- The `result` type *is the same as* the type of the auto-declared variable `result`; `return e` assigns `e` to `result` and then returns.
- A function path may also end in failure, whether via an explicit `fail` statement or exception propogation through the postfix error test (the `!` operator).
- All failure paths must share a single uniform type.
- Muturally recursive functions must be analyzed together; if they span multiple modules, those modules are treated as a single component for the sake of analysis (a *strongly connected component*).
- A function whose signature is declared with explicit type variables must have all uses of the same named variable be consistent. Meaning, in `f(x: 'T) -> 'T`, the parameter and the return type must be the same type.
- A function that declares a type variable for a parameter is not allowed if the usage of the parameter requires a concrete type. For instance: `f(x: 'T) { return x + 1 }` is not allowed, because `x` must be some integer type based the addition to the literal `1`.
- If a function does not return an explicit value in its successpaths, it must never return an value in any success path, and is said to have a `void` return.
- The same requirement holds for `fail` return types.

Note that together, the success and fail return types behave as a single `Result[]` type that's become common in other languages.

## Function calls
- Function calls must be consistent with the type associated with the called function.
- Function calls do NOT refine the type of the function being called (excepting mutual recursion).

## Traits
- The `.` operator dispatches to functions using dynamic dispatch, via a trait associated with an object (which must not be ambiguous).
- Traits describe abstractly the interface needed for a match, but is not directly instantiable as a type; only other types that have a conforming implementation can be instantiated.
- For the interface described by a trait, any types that have a full implementation automatically get assigned the trait, meaning any place the type declared by the trait is sued, an object of any matching type can be substituted in.
- Functions associated by a trait must always have a generic first parameter. All types that can possibly implement the trait must be of a compatable type.
- Trait functions dispatch to the most specific function that matches the runtime object type; it's a error if there are cases where no one function would be most specific, or if there is any type mismatch.
- Trait operations for two different concrete types that implement the trait need not have compatable types (return type included).
- The following literal syntax types have an associated trait attached: `[]` (Sequence_Syntax), `{}` (Dict_Syntax), and `""` (String_Syntax).
- Currently, fields are not allowed inside traits, though this may change.

## Special type constructors
- `View['T]` is compatable with 'T as long as no component of 'T can be written to, and is incompatable otherwise.
- `Ref['T]`
- `Atomic['T]`


## User-defined type rules
- enums
- flagsets
- variants
- anonymous unions
- object vs record
- Ordered tuples
- Unordered tuples
- User-defined reference types
- `alias`
- distinct types

## Interaction with language constructs
- Conditional expressions must return `bool`.
- Discard operator
- for destructuring

# Appendix 2: Preprocessor builtins

