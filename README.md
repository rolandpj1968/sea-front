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

**Active development.** Lexer, parser, sema, template instantiation, and C
codegen are all working end-to-end. Plumbed into the gcc 4.8 build via a
`sea-front-cc` wrapper script that intercepts CXX compilation
(preprocess → sea-front → cc).

### Bootstrap Targets

| Stage | Target | C++ Standard | Status |
|-------|--------|-------------|--------|
| **A** | gcc 4.8 (bootstrap bridge) | C++03 | ~98% of objects build via real Makefile + sea-front-cc |
| **B** | Modern gcc | C++14 | Grammar ready, features incremental |
| **C** | LLVM/Clang | C++17 | Grammar ready, features incremental |

Stage A is the immediate goal: transpile gcc 4.8's C++ source to C, producing
the first C++ compiler in the trusted bootstrap chain. Stages B and C extend
upward to modern compilers. See
[Trusted Bootstrap Design](docs/trusted-bootstrap-design.md).

| Component | Status |
|-----------|--------|
| Lexer | Complete — handles all C++17 tokens, string prefixes, raw strings, digraphs |
| Parser | Complete — full C++17 grammar, recursive descent, tentative parsing |
| Name Lookup | Complete — declarative regions, unqualified/qualified, using-directives |
| Sema | First slice — type propagation, member resolution, implicit `this`, operator overload return types |
| Template Instantiation | Working — class + function templates, member templates, deduction, dedup, transitive deps |
| C Codegen | Working — structs, vtables, ctors/dtors, scope cleanup, name mangling, temp materialization |
| Standard Library | 80/80 libstdc++ headers parse and emit through `--emit-c` |

### Test Suite

- 144 lexer unit tests
- 42 parser integration tests
- 187 emit-c end-to-end tests (C++ in → C out → compile → execute → verify)
- 28/28 gated + 52/52 stretch libstdc++ header smoke tests
- gcc 4.8 source: 384/391 source files (~98%) compile via real Makefile +
  `sea-front-cc`

## Building

```sh
make            # build sea-front + mcpp preprocessor
make test       # run full test suite
```

Requires: a C11 compiler (gcc or clang), and gcc 13 libstdc++ headers for the
header smoke tests.

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

# As a CXX wrapper for a real build system
make CXX=./scripts/sea-front-cc CXX_FOR_BUILD=g++ ...
```

## Documentation

### Bootstrap context

| Document | Description |
|----------|-------------|
| [Trusted Bootstrap Design](docs/trusted-bootstrap-design.md) | Architecture, design decisions, the concrete bootstrap chain |
| [C++ Feature Survey](docs/cxx-feature-survey.md) | What features GCC and Clang require, implementation stages |

### Design

| Document | Description |
|----------|-------------|
| [Grammar Evolution](docs/grammar-evolution.md) | C++17 → C++20 → C++23 grammar changes |
| [Disambiguation Rules](docs/disambiguation-rules.md) | Audit of C++ parsing ambiguities |
| [Mangling](docs/mangling.md) | Name mangling framework (human-readable + Itanium) |
| [Inline & Dedup](docs/inline_and_dedup.md) | Multi-TU deduplication strategy |
| [Coding Standards](docs/coding-standards.md) | Project coding conventions |

### Implementation pipeline (in execution order)

| Document | Description |
|----------|-------------|
| [Lexer](docs/lexer-design.md) | Lexer architecture and token representation |
| [AST](docs/ast.md) | AST + Type representation, slot-by-slot, including interim "ambiguous" forms |
| [Parser](docs/parser.md) | Recursive descent strategy, ambiguity resolution, two semantic oracles |
| [Template Instantiation](docs/template-instantiation.md) | Cloning + substitution, deduction, dedup, lookup phases |
| [Emit](docs/emit.md) | C++ → C translation: classes, methods, templates, references, cleanup |

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

- Partial template specialization — full specialization works
- SFINAE / `enable_if` — not yet supported
- Lambdas — parsed but not transpiled
- `auto` / `decltype` deduction — parsed but not resolved
- ADL in dependent contexts (template instantiation lookup is a partial
  Phase-1/Phase-2 implementation)
- Variadic templates (out of scope for the C++03 bootstrap target)
- A handful of `SHORTCUT`-tagged narrow lowerings (vNULL, arg-type mangling
  fallback for unresolved qualified calls) — greppable, each cited

## License

MIT
