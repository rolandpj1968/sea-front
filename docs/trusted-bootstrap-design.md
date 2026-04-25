# Trusted Bootstrap C++ Compiler: Design Document

## The Problem: Trusting Trust

In 1984, Ken Thompson's Turing Award lecture "Reflections on Trusting Trust"
demonstrated that a compiler can be subverted to inject malicious code — and
that the subversion can propagate invisibly through self-hosting. If compiler
version N is used to compile compiler version N+1, a trojan in N's binary
persists in N+1's binary, even if N+1's source code is clean. The attack is
invisible to source code audits.

Today, every major C++ compiler (GCC, Clang, MSVC) is self-hosting: it requires
an existing C++ compiler to build. The bootstrap chain is circular. There is no
way to verify, from first principles, that the binary you're running corresponds
to the source code you can read.

## The Goal

Build a trusted path from auditable foundations to a production-quality C++
compiler:

```
Auditable trust root (hex, minimal assembler, etc.)
    ↓
Trusted C compiler (via bootstrappable builds chain)
    ↓
THIS PROJECT: C++ compiler written in C
    ↓
GCC and/or Clang compiled from source
    ↓
Production C++ toolchain — fully bootstrapped from trusted roots
```

The end state: a production-quality C++ compiler (GCC or Clang) whose entire
build lineage can be audited and reproduced from human-readable foundations,
with no circular self-hosting dependency.

### Why This Matters

- **Software supply chain security.** Every binary built with a compromised
  compiler is potentially compromised. The compiler is the root of trust for
  all software.
- **Reproducible builds.** Full bootstrap from source is a prerequisite for
  truly reproducible builds — you need to trust the tools, not just the code.
- **The missing link.** The Bootstrappable Builds project
  (https://bootstrappable.org) has made significant progress on a trusted path
  from hex to a C compiler (stage0 → mes → mescc → tinycc → GCC's C subset).
  But C++ remains the major unsolved gap. Modern GCC and Clang require a C++
  compiler to build, so the bootstrap chain currently dead-ends at C.

### The Concrete Bootstrap Chain

The [live-bootstrap](https://github.com/fosslinux/live-bootstrap) project
(the primary implementation of the bootstrappable.org vision) has built an
auditable chain from hex to a working C compiler. The chain as of 2024–2025:

```
hex0-seed (357 bytes of hand-auditable hex)
  -> hex1 -> hex2 -> catm -> M0
  -> M1 -> M2 -> mescc-tools
  -> mes (Scheme interpreter in C subset)
  -> mescc (C compiler in Scheme, hosted on mes)
  -> tcc 0.9.27 (Tiny C Compiler, bootstrapped from mescc)
  -> gcc 4.0.4 (built by tcc)
  -> gcc 4.7.4 (built by gcc 4.0.4)
                                          <-- THE C/C++ CHASM
  -> gcc 13+ (requires a C++ compiler to build)
```

**The chasm**: gcc 4.7.4 is the last gcc version that can be built by a
C-only compiler. gcc 4.8 was the first to require C++ to build itself.
The live-bootstrap project currently bridges this gap by carrying
gcc 4.6 and gcc 4.7.4 as "heritage" C-only compilers — large, complex
artifacts maintained solely for this single bootstrap step.

**Sea-front's role**: replace the heritage gcc rung with a small,
auditable C program:

```
hex0 -> ... -> mescc -> tcc 0.9.27 -> gcc 4.7.4
                                          |
                      sea-front (built by gcc 4.7.4 or tcc)
                                          |
               gcc 4.8+ source ---[sea-front transpiles to C]--> C code
                                          |
                      gcc 4.7.4 or tcc compiles the C output
                                          |
                                  working gcc 4.8+ binary
                                          |
                              modern gcc builds itself
```

**The scaffolding C compiler**: the C compiler that compiles both
sea-front itself and its generated C output is **gcc 4.7.4** (or
potentially tcc, depending on the C dialect requirements of the
generated code). This constrains:

- **sea-front's own source**: must be plain C that gcc 4.7.4 (or tcc)
  can compile. Currently C11-clean; may need to stay within C99 if tcc
  is the target. No GCC extensions, no C11 atomics.
- **sea-front's generated C output**: must be compilable by the same
  scaffolding compiler. This means the generated C should avoid C11
  features that gcc 4.0.4/4.7.4 may not support, and should avoid
  GCC-specific extensions unless they're available across the relevant
  gcc versions.

The DDC (Diverse Double-Compiling) angle: sea-front's trust value comes
from being built by the trusted chain. Sea-front compiled by a modern
gcc has no more trust than gcc itself — the whole point is that
sea-front inherits the mescc/tcc lineage.

### Staged C++ Targets

Sea-front's grammar covers C++17 with C++20/23 annotations, but the
C++ *features* needed at each stage are incremental:

| Stage | Target | C++ Standard | Key Features Needed |
|-------|--------|-------------|---------------------|
| **A** | gcc 4.8 | C++03 | Classes, single inheritance, virtual functions, basic templates, namespaces. gcc 4.8 was written to bootstrap from a minimal C++03 compiler — no lambdas, no `auto`, no move semantics in the bootstrap path. |
| **B** | Modern gcc | C++14 | Lambdas, `auto`, `decltype`, move semantics, `constexpr`, variadic templates, SFINAE, `<type_traits>`. |
| **C** | LLVM/Clang | C++17 | `if constexpr`, fold expressions, structured bindings, `std::optional`, CTAD. |

**Stage A is the bootstrap bridge** — the minimum viable product that
breaks the C/C++ chasm. It requires the least C++ feature support and
targets the most constrained codebase (gcc 4.8's deliberately minimal
C++ usage).

**Stages B and C extend the chain upward.** Once Stage A produces a
working gcc 4.8 binary, that binary can compile more modern gcc
versions. But sea-front can also be used to directly transpile modern
gcc or LLVM source to C, bypassing the intermediate gcc versions. This
requires progressively more C++ features but the same architecture.

The grammar is complete for all stages (C++17 with C++20/23 deltas).
The work is in sema, template instantiation depth, and codegen — not
in parsing.

**C++ backwards compatibility**: C++ is mostly but not perfectly
backwards compatible. Each revision adds features (additive) but also
deprecates and removes some (e.g., C++11 changed `auto` from storage
class to type deduction; C++17 removed trigraphs and `register` as a
storage class). For sea-front this is largely irrelevant: we process
valid source already compiled by gcc/clang, so we accept the superset
grammar and don't need to diagnose deprecated features.

### What This Project Is NOT

- **Not a production compiler.** It doesn't need to be fast, generate optimised
  code, or have good error messages. It just needs to be correct.
- **Not a permanent tool.** Once it compiles GCC or Clang, its job is done.
  The bootstrapped GCC/Clang replaces it for all subsequent use.
- **Not necessarily complete.** It only needs to support the C++ features that
  GCC and Clang's own source code actually uses. But it should handle the
  grammar correctly (see below).

## The Approach: C++ to C Transpiler

The compiler is a **source-to-source transpiler** that reads C++ and emits C.
This approach:

- **Keeps the compiler simple.** No need for machine code generation,
  register allocation, or optimisation. The C compiler handles all of that.
- **Produces auditable output.** The generated C code can be inspected to
  verify correctness.
- **Slots into the existing bootstrap chain.** The output feeds directly into
  a trusted C compiler.
- **Has historical precedent.** Bjarne Stroustrup's original cfront compiler
  (1979–1999) translated C++ to C. This project extends that approach to
  cover modern C++.

### Lessons from cfront

Bjarne Stroustrup's cfront (1979–1993) used **local, mechanical translation**:
each C++ construct was mapped to C in relative isolation. A class became a
struct, a method became a function with an explicit `this` parameter, virtual
dispatch became a function pointer table. Almost macro-like expansion.

This worked well for "C with classes" but the technique broke down as C++
grew:

1. **Exceptions require non-local reasoning.** Stack unwinding means every
   function call site that could throw needs cleanup code to run destructors
   for all live objects in scope. Cfront attempted to use `setjmp`/`longjmp`,
   which was slow, produced enormous output, and couldn't guarantee correct
   destructor ordering in complex cases. Cfront 4.0 attempted to ship
   exception support and was abandoned when it couldn't be made reliable.

2. **Templates cause combinatorial code explosion.** Monomorphisation is
   conceptually straightforward, but cfront's local model had no good
   strategy for deduplicating instantiations across translation units. The
   generated C became unmanageably large.

3. **RAII and destructor scope tracking.** Even without exceptions, correct
   destructor invocation at every scope exit (including early returns,
   `break`, `continue`, `goto`) requires tracking which objects are live
   at each point — a non-local analysis that cfront's mechanical approach
   handled poorly.

**Our approach differs from cfront in a critical way:** rather than local,
mechanical translation, we build a full AST with semantic analysis and
perform whole-program-aware code generation. This gives us the non-local
reasoning needed for scope-based cleanup, template deduplication, and
— eventually — exception support.

**On exceptions and RTTI:** our immediate targets (GCC and Clang) compile
with `-fno-exceptions -fno-rtti`, so these features are not needed for
the bootstrap goal. However, the architecture should not preclude adding
them later. Exception support in a C transpiler is achievable with
whole-program analysis — the transpiler can generate explicit cleanup
chains and `setjmp`/`longjmp`-based unwinding (or platform-specific
mechanisms) when it has full visibility into object lifetimes. This is
a harder problem than anything else in the transpiler, but it is not the
unsolvable problem it was for cfront's local translation model.

## Parser Platform Decision: Hand-Written Recursive Descent

**Decision:** Hand-written recursive descent parser with a hand-written lexer,
implemented entirely in C. No parser generators, no external tools.

**Rationale:**

1. **It's the only proven approach for C++.** Every production C++ parser
   (GCC, Clang, EDG, MSVC) uses hand-written recursive descent. GCC
   originally used bison and abandoned it because C++'s context-sensitivity
   made the grammar unmaintainable. No parser-generator-based C++ parser
   has ever achieved full conformance.

2. **Maximum auditability.** For trusted bootstrapping, every parsing
   decision must be transparent. Hand-written C code has no opaque
   generated tables, no external tool dependencies, no hidden automata.
   Reviewers can trace any construct from input to AST.

3. **Zero external dependencies.** The bootstrap chain does not need to
   trust bison, flex, re2c, or any parser generator. The parser is
   self-contained C compiled by the trusted C compiler.

4. **Natural semantic feedback.** C++ requires the parser to consult the
   symbol table to resolve ambiguities (see `disambiguation-rules.md`).
   The parser needs exactly two semantic oracles:
   - Is this identifier a type-name?
   - Is this identifier a template-name?
   In recursive descent, these are simple function calls at the point of
   ambiguity. In parser generators, this requires fragile hacks.

5. **Tentative parsing is straightforward.** The parser saves its position,
   tries a declaration parse, and backtracks if it fails — natural in
   recursive descent, awkward or impossible with LR/LALR generators.

6. **Template deferred parsing is natural.** In dependent contexts, the
   parser switches to "token collection" mode, building placeholder AST
   nodes. At instantiation time, it re-enters the parser with collected
   tokens and a complete symbol table. GCC and Clang both do this.

**Alternatives considered and rejected:**

| Approach | Why rejected |
|----------|-------------|
| flex/bison (LALR) | C++ is not LALR(k); GCC tried and abandoned this |
| bison GLR mode | Defers disambiguation to post-parse, making it harder |
| PEG / packrat | Ordered choice silently picks wrong parse; memoization broken by semantic context |
| re2c + hand-written | Adds trust dependency for marginal lexer benefit |
| Custom table-driven | Building a parser generator AND a C++ parser — double the work |

**Size estimate:** ~25,000–50,000 lines of C for the full parsing front end
(lexer ~2K, parser ~20K–35K, symbol table + semantic feedback ~5K–10K).
For reference, chibicc is a complete C11 compiler in ~10K lines, and C++
is roughly 3–5x more complex than C in terms of grammar.

## Architecture

```
                    ┌──────────────────────┐
                    │    C++ Source Code    │
                    │   (GCC or Clang)     │
                    └──────────┬───────────┘
                               │
                    ┌──────────▼───────────┐
                    │    Preprocessor       │
                    │  (existing, e.g.     │
                    │   mcpp or cpp)       │
                    └──────────┬───────────┘
                               │
                    ┌──────────▼───────────┐
                    │       Lexer          │
                    └──────────┬───────────┘
                               │
                    ┌──────────▼───────────┐
                    │       Parser         │
                    │  (full C++ grammar,  │
                    │   proper disambig,   │
                    │   deferred nodes)    │
                    └──────────┬───────────┘
                               │
                    ┌──────────▼───────────┐
                    │  Semantic Analysis    │
                    │  (pragmatic subset)  │
                    └──────────┬───────────┘
                               │
                    ┌──────────▼───────────┐
                    │ Template Instantiation│
                    │  (clone AST +        │
                    │   resolve deferred   │
                    │   nodes)             │
                    └──────────┬───────────┘
                               │
                    ┌──────────▼───────────┐
                    │    C Code Generator   │
                    │  (transpile resolved │
                    │   AST to C)          │
                    └──────────┬───────────┘
                               │
                    ┌──────────▼───────────┐
                    │   Trusted C Compiler  │
                    │  (from bootstrap     │
                    │   chain)             │
                    └──────────┬───────────┘
                               │
                    ┌──────────▼───────────┐
                    │  Working GCC/Clang   │
                    │  binary              │
                    └──────────────────────┘
```

## Key Design Decisions

### 1. Full Grammar, Pragmatic Semantics

The C++ grammar (including disambiguation rules) should be implemented
**correctly and completely**. The reasons:

- The disambiguation rules are finite and enumerable — roughly a dozen
  clauses in the standard.
- A correct grammar is stable against changes in the target codebases.
  GCC and Clang are moving targets; a grammar subsetted to today's code
  may break on next year's release.
- The `typename` and `template` keywords provide explicit disambiguation
  signals in dependent contexts — the programmer has already done the
  hard work.

The **semantic rules** (overload resolution, template argument deduction,
implicit conversion ranking) should be implemented pragmatically — enough
to handle what GCC and Clang actually use, grown incrementally as needed.
These rules are enormously complex in the standard (~50 pages for overload
resolution alone) but real-world usage patterns are far narrower than the
full specification. Unlike the grammar, semantic edge cases are unlikely
to appear in new compiler releases without warning.

### 2. Parsing with Deferred Nodes

C++ cannot be parsed without semantic information. The parser must consult
the symbol table to distinguish types from values. Inside template
definitions, some names depend on template parameters and cannot be
resolved until instantiation.

The parser produces an AST with **deferred node types** for unresolvable
constructs:

| AST Node           | Meaning |
|---------------------|---------|
| `DependentName`     | `T::x` — could be type, value, or template |
| `DependentType`     | A type involving template parameters |
| `UnresolvedExpr`    | An expression whose type is not yet known |
| `AmbiguousDecl`     | Parsed as declaration, may be expression |

Template instantiation walks the AST, substitutes template arguments,
and resolves deferred nodes into concrete types/expressions/declarations.

### 3. Two-Phase Name Lookup

Following the standard's model:

- **Phase 1 (template definition):** Parse the template body. Resolve
  non-dependent names immediately. Leave dependent names as deferred
  nodes.
- **Phase 2 (template instantiation):** Substitute template arguments.
  Resolve dependent names in the instantiation context.

### 4. C Translation Strategies

| C++ Feature | C Translation |
|---|---|
| Classes / structs | C structs + separate functions |
| Single inheritance | Embedded base struct as first member |
| Virtual functions | Vtable structs with function pointers; vtable pointer as first member |
| Multiple inheritance | Multiple embedded structs; pointer adjustment on cast |
| Constructors / destructors | Init/cleanup functions, called explicitly |
| RAII / scope cleanup | `goto`-based destructor chains (see below) |
| Templates | Monomorphisation — generate specialised C code per instantiation |
| Namespaces | Name mangling / prefixed identifiers |
| References | Pointers (with const where appropriate) |
| Operator overloading | Named function calls |
| Lambdas | Structs (captured variables) + function pointers |
| Move semantics | Bitwise copy + nullify source (for most types) |
| `auto` / `decltype` | Resolved to concrete types by the transpiler |
| `constexpr` | Evaluated at compile time by the transpiler |
| `enum class` | C enums with prefixed names |
| `std::unique_ptr<T>` | `T*` with generated cleanup code |

### 5. RAII and Scope Cleanup: Goto Destructor Chains

Correct destructor invocation is a foundational concern even without exception
support. Every scope exit — normal fall-through, `return`, `break`,
`continue`, `goto` — must call destructors for all live objects in reverse
construction order.

**The approach: a single linear destructor chain per function, using `goto`
with fall-through.**

For each function, the transpiler generates a chain of destructor calls at the
end of the function, one label per destructible object, in reverse construction
order. Every scope exit is a single `goto` to the appropriate entry point in
the chain. Fall-through handles the rest.

```c
// C++ source:
void f(int x) {
    A a;
    if (x > 0) {
        B b;
        if (x > 10) return;   // must destroy b, then a
        C c;
        return;                // must destroy c, b, then a
    }
    return;                    // must destroy a only
}

// Generated C:
void f(int x) {
    A a; A_ctor(&a);
    if (x > 0) {
        B b; B_ctor(&b);
        if (x > 10) goto cleanup_b;
        C c; C_ctor(&c);
        goto cleanup_c;
    }
    goto cleanup_a;

cleanup_c: C_dtor(&c);
cleanup_b: B_dtor(&b);
cleanup_a: A_dtor(&a);
    return;
}
```

**Properties of this approach:**

- **O(N) code size.** Exactly one destructor call per destructible object in
  the function, regardless of the number of exit points. Every exit is a
  single `goto`. The chain is shared across all exit paths via fall-through.
- **Zero runtime overhead.** No flags, no conditionals, no tracking. Each
  exit path is a direct jump to exactly the right point in the chain.
- **Correct by construction.** The chain is always in reverse construction
  order. Fall-through guarantees that if C is destroyed, B and A are
  destroyed afterwards in the right order.
- **Trivial to generate mechanically.** The transpiler maintains a stack of
  live destructible objects per scope. Each exit point emits
  `goto cleanup_<most_recent>`. The chain is emitted once at the end of
  the function.
- **Composable with exception support.** If return-value-based error
  propagation is added later, an error check is just another conditional
  `goto` into the same chain — no additional machinery needed.

**Nested and parallel scopes** work naturally. When branches construct
different objects, the chains merge back at the common ancestor:

```c
// C++ source:
void g(bool cond) {
    A a;
    if (cond) {
        B b;
    } else {
        C c;
    }
}

// Generated C:
void g(bool cond) {
    A a; A_ctor(&a);
    if (cond) {
        B b; B_ctor(&b);
        goto cleanup_b;
    } else {
        C c; C_ctor(&c);
        goto cleanup_c;
    }

cleanup_c: C_dtor(&c); goto cleanup_a;
cleanup_b: B_dtor(&b);
cleanup_a: A_dtor(&a);
    return;
}
```

Note: `goto` in generated C is not the "considered harmful" kind. The
transpiler generates it mechanically as a compilation target — it
corresponds directly to how compiler backends represent cleanup in their
IR. The generated C is not intended to be maintained by humans.

### 6. Standard Library Strategy

The transpiler needs to provide standard library headers that the target
codebases include. Two approaches:

- **Minimal reimplementation.** Write just enough `<type_traits>`,
  `<memory>`, `<algorithm>`, etc. to satisfy GCC and Clang's needs.
  These headers are themselves C++ — they'd be processed by our own
  transpiler.
- **Leverage existing implementations.** Use an existing libc++ or
  libstdc++ header set, possibly with modifications. These are designed
  to be compiled by standard C++ compilers, which is what we're building.

The former is more work upfront but avoids depending on complex,
heavily-optimised standard library implementations that may use compiler
intrinsics and extensions.

### 7. Name Mangling

The transpiler uses **Itanium C++ ABI name mangling** for generated C
identifiers. Although ABI compatibility is not strictly required (we compile
everything from source), Itanium mangling is the pragmatic choice:

- **Debugging.** Standard tools (`c++filt`, `addr2line`, debuggers) can
  demangle Itanium names. Custom mangling means custom tooling.
- **Linker diagnostics.** Undefined symbol errors are much easier to
  diagnose with standard mangled names.
- **Incremental testing.** During development, linking a transpiled object
  against a conventionally-compiled one (to verify correctness) requires
  matching mangling.
- **Forward compatibility.** If the transpiler grows beyond the bootstrap
  use case, ABI compatibility becomes valuable.

The Itanium mangling scheme is well-specified and covers the full language:
namespaces, templates (with full argument encoding), function signatures,
operators, special members, and a substitution/compression grammar to keep
names tractable. It extends naturally to templates — each instantiation
gets a distinct mangled name encoding its template arguments.

Reference: Itanium C++ ABI, Section 5.1 ("External Names / Mangling").

### 8. No Exceptions, No RTTI (Bootstrap Target)

Both target codebases compile with `-fno-exceptions -fno-rtti`. The
transpiler does not need to support these features for the bootstrap goal:

- `try`, `catch`, `throw`
- `dynamic_cast<>`
- `typeid`, `std::type_info`
- Exception specifications
- Stack unwinding

This eliminates the two hardest C++ features to translate to C and was
the primary reason cfront was abandoned. We do not have this problem
for the immediate goal.

### 9. Future: Exception and RTTI Support

The architecture should not preclude adding exception and RTTI support
in future. Both are achievable within the transpiler's framework.

#### RTTI Translation

C++ RTTI is a **static data structure** encoding the class hierarchy.
The compiler emits a read-only `type_info` record for each type that
needs it. The Itanium ABI defines a family of derived classes:

| ABI Type                     | Represents |
|------------------------------|------------|
| `__fundamental_type_info`    | Built-in types (`int`, `float`, etc.) |
| `__class_type_info`          | Classes with no bases |
| `__si_class_type_info`       | Single, public, non-virtual inheritance |
| `__vmi_class_type_info`      | Multiple, virtual, or non-public inheritance |
| `__pointer_type_info`        | Pointer types |

Each record contains the mangled type name and pointers to base class
records, forming a graph that `dynamic_cast` and exception matching can
walk at runtime.

For our transpiler, these translate directly to C:

```c
// C++ class hierarchy:
//   class Base { virtual ~Base(); };
//   class Derived : public Base { };

// Generated C (read-only data):
static const __class_type_info _ZTI4Base = {
    .vtable = &__class_type_info_vtable,
    .__name = "4Base"
};

static const __si_class_type_info _ZTI7Derived = {
    .vtable = &__si_class_type_info_vtable,
    .__name = "7Derived",
    .__base_type = &_ZTI4Base
};
```

The transpiler has the complete type hierarchy in its AST, so generating
these records is straightforward. `dynamic_cast` becomes a call to a
runtime function that walks the type_info graph.

#### Exception Translation

Two viable approaches for translating exceptions to C, both building on
the goto-based destructor chain already used for RAII:

**Approach A: Return-value error propagation.**

Every function that can throw returns a discriminated result. Every call
site checks for error and branches into the destructor chain on failure:

```c
// C++ source:
Widget make_widget() {
    Gadget g;
    return Widget(g.compute());   // compute() might throw
}

// Generated C:
struct _result_Widget { bool _error; union { Widget _value; _exception _ex; }; };

struct _result_Widget make_widget(void) {
    Gadget g; Gadget_ctor(&g);

    struct _result_int r1 = Gadget_compute(&g);
    if (r1._error) goto cleanup_g_err;

    Widget w; Widget_ctor_int(&w, r1._value);
    Gadget_dtor(&g);
    return (struct _result_Widget){ ._error = false, ._value = w };

cleanup_g_err:
    Gadget_dtor(&g);
    return (struct _result_Widget){ ._error = true, ._ex = r1._ex };
}
```

Properties:
- Zero cost on the happy path (just a branch prediction hint)
- No `setjmp`/`longjmp`, no thread-local state
- Every control flow path is explicit and auditable in the generated C
- Composes naturally with the destructor chain — error propagation is
  just another `goto` into the existing cleanup sequence
- Transforms every potentially-throwing function's signature and every
  call site — verbose in generated code, but the transpiler handles this
  mechanically

**Approach B: setjmp/longjmp with cleanup chains.**

`try` blocks register a `setjmp` marker. `throw` calls `longjmp`.
Destructor chains are invoked explicitly before the jump. Simpler
function signatures but requires thread-local exception state and
has `setjmp` overhead at every `try` block.

**Recommendation:** Approach A (return-value propagation) is preferred
for the trusted bootstrap context. The generated code is entirely
transparent — no hidden control flow, no platform-specific unwinding,
fully auditable. The verbosity is the transpiler's problem, not the
auditor's.

#### Exception Catch Matching

Exception catch matching uses the same RTTI type_info graph. Given a
thrown exception's `type_info*` and a catch clause's `type_info*`:

1. Check pointer identity (same type — fast path)
2. Walk the thrown type's base class chain via `__base_type` /
   `__base_info` pointers
3. If any base matches the catch type, it's a match
4. `catch(...)` matches everything (no type_info check)

This is a simple graph traversal, implemented as a small C runtime
function. No platform-specific machinery required.

## Target Ordering

**GCC first, then Clang/LLVM.**

| | GCC | Clang/LLVM |
|---|---|---|
| C++ standard | C++14 | C++17 |
| Template usage | Heavy | Heavier |
| C++17 features | None | `if constexpr`, fold expressions, structured bindings, `std::optional` |
| Standard library | Uses STL containers | Mostly custom containers (SmallVector, DenseMap, etc.) |
| Codebase size | Large | Larger |

GCC's C++14 requirement is a strict subset of Clang's C++17. Successfully
compiling GCC produces a working `g++` that can serve as an independent
validation tool and a fallback compiler.

## Implementation Stages

### Stage 1: C with Classes
Core object model: structs with methods, constructors/destructors, single
inheritance, virtual dispatch, references, namespaces, operator overloading,
RAII scope cleanup.

### Stage 2: Basic Templates
Class and function templates, specialisation (full and partial), non-type
parameters, default arguments, argument deduction. Deferred AST nodes and
two-phase lookup.

### Stage 3: C++11/14 Features
Move semantics, `auto`/`decltype`, lambdas, `constexpr`, `static_assert`,
variadic templates, SFINAE/`enable_if`, alias templates, `= default`/`= delete`.

### Stage 4: C++17 Features (for Clang)
`if constexpr`, structured bindings, fold expressions, `inline` variables,
`std::optional`, `[[nodiscard]]`, CTAD.

### Stage 5: Standard Library
Minimal implementations of required headers: `<type_traits>`, `<memory>`,
`<algorithm>`, `<functional>`, `<string>`, `<tuple>`, `<utility>`, and
container types needed by each target.

### Stage 6: Integration and Validation
Compile GCC from source. Run GCC's test suite on the result.
Compile Clang/LLVM from source. Run Clang's test suite on the result.

## The Broader Bootstrap Chain

This project addresses one link in a larger chain. The full trusted
bootstrap path would look something like:

```
Hex monitor (~512 bytes, hand-auditable)
    ↓
stage0 — minimal assembler
    ↓
stage1 — macro assembler
    ↓
mes — Scheme interpreter (in assembly)
    ↓
mescc — C compiler (in Scheme, running on mes)
    ↓
tinycc — small C compiler (compiled by mescc)
    ↓
GCC (C only) — compiled by tinycc
    ↓
THIS PROJECT — C++ to C transpiler (compiled by GCC-C or tinycc)
    ↓
GCC (full, C++) — compiled via transpiler + trusted C compiler
    ↓
Clang/LLVM — compiled via transpiler + trusted C compiler
    ↓
Full trusted toolchain
```

Every link in this chain is either hand-auditable (hex, small assembly)
or compiled by a tool from the previous stage. No circular dependencies.
No self-hosting. Full auditability from first principles.

## Open Questions

1. **Preprocessor choice.** Use an existing preprocessor (mcpp, ucpp) or
   implement one? An existing one is pragmatic but adds a trust dependency.
   A minimal preprocessor in C is tractable and closes the trust gap.

2. **Build system.** GCC and Clang both use complex build systems
   (autoconf/make and CMake respectively). The transpiler needs to
   integrate with or replace these. A purpose-built build driver that
   understands the necessary compilation steps may be needed.

3. **Compiler intrinsics.** Both GCC and Clang source code may use
   `__builtin_*` functions, `__attribute__` annotations, and other
   compiler-specific extensions. These need to be catalogued and
   either supported or worked around.

4. **Self-referential bootstrapping.** GCC's build process compiles
   itself three times (stage1, stage2, stage3) and compares results.
   Our transpiler only needs to produce a working stage1 binary —
   that binary then handles its own stage2/stage3 self-bootstrap.

5. **Linker and assembler.** The trust chain also includes the linker
   and assembler. GNU binutils or a trusted equivalent is needed.
   The Bootstrappable Builds project addresses this separately.

6. **Testing strategy.** How to validate correctness incrementally
   before attempting the full GCC/Clang build. Unit tests for language
   features, existing C++ test suites, and incremental compilation of
   increasingly large real-world codebases.
