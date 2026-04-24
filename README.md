# sea-front

A C++-to-C transpiler written in C, for trusted bootstrapping of GCC and Clang.

## The Problem

Every major C++ compiler (GCC, Clang, MSVC) is self-hosting — it requires an
existing C++ compiler to build. The [Bootstrappable Builds](https://bootstrappable.org)
project has built a trusted chain from auditable hex all the way to a C compiler
(hex0 → mescc → tcc → gcc 4.7.4), but **C++ is the unsolved gap**. gcc 4.8 was
the first to require C++ to build itself.

## The Solution

sea-front bridges the C → C++ chasm:

```
hex0 → ... → mescc → tcc → gcc 4.7.4
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

~23K lines of auditable C. No dependencies beyond a C compiler.

## Status

**Active development.** The lexer, parser, semantic analysis, template
instantiation, and C code generator are all working.

### Bootstrap Targets

| Stage | Target | C++ Standard | Status |
|-------|--------|-------------|--------|
| **A** | gcc 4.8 (bootstrap bridge) | C++03 | In progress — core templates working |
| **B** | Modern gcc | C++14 | Grammar ready, features incremental |
| **C** | LLVM/Clang | C++17 | Grammar ready, features incremental |

Stage A is the immediate goal: transpile gcc 4.8's C++ source to C,
producing the first C++ compiler in the trusted bootstrap chain.
Stages B and C extend upward to modern compilers. See
[Trusted Bootstrap Design](doc/trusted-bootstrap-design.md) for details.

| Component | Status |
|-----------|--------|
| Lexer | Complete — handles all C++17 tokens, string prefixes, raw strings, digraphs |
| Parser | Complete — full C++17 grammar, recursive descent, tentative parsing |
| Name Lookup | Complete — declarative regions, unqualified/qualified lookup, using-directives |
| Sema | First slice — type propagation, member resolution, implicit `this` |
| **Template Instantiation** | **Working** — class templates, function templates, defaults, dedup, transitive deps |
| C Codegen | Working — structs, vtables, ctors/dtors, scope cleanup, name mangling |
| Standard Library | 80/80 libstdc++ headers parse and emit through `--emit-c` |

### Test Suite

- 144 unit tests (lexer)
- 42 parser integration tests
- 186 emit-c end-to-end tests (C++ in → C out → compile → execute → verify)
- 28/28 gated + 52/52 stretch libstdc++ header smoke tests
- 1 multi-TU test
- 70+ of ~50 gcc 4.8 source files tested end-to-end via sea-front-cc
  (transpile → compile) compile cleanly

### Template Instantiation

The template engine uses AST-level cloning with type substitution:

```
template<typename T>               /* C++: struct holder */
struct holder {          ──────>   struct sf__holder_t_int_te_ {
    T value;                           int value;
    T get() { return value; }      };
};                                 __SF_INLINE int sf__holder_t_int_te___get(
                                       struct sf__holder_t_int_te_ *this) {
holder<int> h;                         return this->value;
h.set(42);                         }
```

Supports: multiple type parameters, default arguments (including template-id
defaults like `Alloc = allocator<T>`), namespace-scoped templates, nested/transitive
instantiation, out-of-class method definitions, deduplication, topological ordering.

## Building

```sh
make            # build sea-front + mcpp preprocessor
make test       # run full test suite
```

Requires: a C11 compiler (gcc or clang), and gcc 13 libstdc++ headers for
the header smoke tests.

### Bootstrap Build

```sh
bash bootstrap.sh   # single-command build with no make dependency
```

### Usage

```sh
# Parse and dump AST
./build/sea-front input.cpp
./build/sea-front --dump-ast input.cpp

# Transpile C++ to C
./build/sea-front --emit-c input.cpp > output.c

# With preprocessor (for real headers)
./build/mcpp-bin -+ -W0 -V201103L -I/usr/include/c++/13 ... input.cpp > input.i
./build/sea-front --emit-c input.i > output.c
```

## Documentation

| Document | Description |
|----------|-------------|
| [Trusted Bootstrap Design](doc/trusted-bootstrap-design.md) | Architecture, design decisions, the concrete bootstrap chain |
| [C++ Feature Survey](doc/cxx-feature-survey.md) | What C++ features GCC and Clang require, implementation stages |
| [Grammar Evolution](doc/grammar-evolution.md) | C++17 → C++20 → C++23 grammar changes and impact assessment |
| [Disambiguation Rules](doc/disambiguation-rules.md) | Complete audit of C++ parsing ambiguities |
| [Lexer Design](doc/lexer-design.md) | Lexer architecture and token representation |
| [Coding Standards](doc/coding-standards.md) | Project coding conventions |
| [Mangling Design](docs/mangling.md) | Name mangling framework (human-readable + Itanium) |
| [Inline & Dedup](docs/inline_and_dedup.md) | Multi-TU deduplication strategy |
| [Two-Phase Lookup](docs/two-phase-lookup.md) | Template name lookup — current state and remaining shortcuts |

### Generated Output Examples

The `gen/` directory contains source + generated C pairs for eyeballing
the transpiler's output quality. Each emitted C definition includes a
`/* C++: ... */` comment showing the original C++ declaration.

## Approach

- **C++-to-C transpiler** (like cfront, but with whole-program analysis)
- **Hand-written recursive descent parser** (the only proven approach for C++)
- **Full C++17 grammar** with proper disambiguation and C++20/23 change annotations
- **AST-level template instantiation** — clone + substitute, not token replay
- **Pragmatic semantic subsetting** — enough to compile GCC (C++14) then Clang (C++17)
- **No exceptions, no RTTI needed** — both targets compile with `-fno-exceptions -fno-rtti`
- **Goto-chain destructor cleanup** — O(N) code, zero runtime overhead, correct by construction

## Known Gaps

- Partial template specialization — not yet implemented (full specialization works)
- SFINAE / `enable_if` — not yet supported
- Lambda lowering — parsed but not transpiled
- `auto` / `decltype` type deduction — parsed but not resolved
- Standard library instantiation — headers parse and emit, but some
  library internals use advanced patterns (partial specialization,
  SFINAE) not yet supported

## License

MIT
