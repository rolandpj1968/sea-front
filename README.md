# sea-front

**v0.2.4 — closing out gcc 4.8 dg** ([CHANGELOG](CHANGELOG.md))

A C++-to-C transpiler written in C — turns C++ source into portable C that
any standard C compiler can build. Used in two complementary ways:

  - **As a C++ frontend for a C-only backend.** Pair sea-front with a C
    compiler that lacks a C++ frontend (e.g. tcc, [cproc + QBE](https://c9x.me/compile/),
    a fresh-built gcc 4.7) and you have a working C++ toolchain end-to-end.
    No language-runtime fork: the C output is plain ISO C with sea-front's
    own small support shim (exception-state TLS struct, vtable-thunk
    helpers — see [Generated Output Examples](demo/)).
  - **As a trusted-bootstrap bridge.** Sea-front is the first link that
    lets [Bootstrappable Builds](https://bootstrappable.org) reach a real
    C++ compiler — see "Bootstrap context" below.

Sample C output is in [`demo/`](demo/) — five small C++ inputs paired with
the C sea-front produces.

## Why a C++ → C transpiler?

The portability story: C is the universal lowest-common-denominator IR.
Every interesting target (cproc/QBE, tcc, gcc, clang, mrustc-style trusted
chains, hex0-derived stacks) has a C path; few have a C++ path. A C++
program transpiled to C can be retargeted to any of them with no further
work in sea-front.

The trusted-bootstrap story:

Every major C++ compiler (GCC, Clang, MSVC) is self-hosting — it requires an
existing C++ compiler to build. The Bootstrappable Builds project has built
a trusted chain from auditable hex all the way to a C compiler (hex0 →
mescc → tcc → gcc 4.7.4), but **C++ is the unsolved gap**. gcc 4.8 was
the first to require C++ to build itself.

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

**Stage A bootstrap complete.** A cc1plus binary built end-to-end through
sea-front from gcc 4.8 source compiles and runs real C++ programs, and is a
self-consistent fixed point under the gcc bootstrap protocol
(stage2 == stage3 byte-equal). See [the bootstrap context below](#bootstrap-context-stage-a-gcc-48).

Lexer, parser, sema, template instantiation, and C codegen all work
end-to-end. Plumbed into the gcc 4.8 build via two wrapper scripts:
`scripts/sea-front-cc` (the transpiler driving the initial host bootstrap)
and `scripts/cc1plus-via-sf` (drives sea-front-built cc1plus as a g++
replacement for stage-N rebuilds).

### Bootstrap Targets

| Stage | Target | C++ Standard | Status |
|-------|--------|-------------|--------|
| **A** | gcc 4.8 (bootstrap bridge) | C++03 | ✅ stage2 == stage3 byte-equal (v0.1.0) |
| **B** | Modern gcc | C++14 | Grammar ready, features incremental |
| **C** | LLVM/Clang | C++17 | Grammar ready, features incremental |

Stage A — the canonical bootstrappable.org rung — is the immediate goal:
transpile gcc 4.8's C++ source to C, producing the first C++ compiler in the
trusted bootstrap chain. Stages B and C extend upward to modern compilers.
See [Trusted Bootstrap Design](docs/trusted-bootstrap-design.md).

| Component | Status |
|-----------|--------|
| Lexer | Complete — all C++17 tokens, string prefixes, raw strings, digraphs |
| Parser | Complete — full C++17 grammar, recursive descent, tentative parsing |
| Name Lookup | Complete — declarative regions, unqualified/qualified, using-directives |
| Sema | Working — type propagation, member resolution, implicit `this`, operator-overload return types, C++11 `auto` deduction, default-arg expansion, using-declaration type forwarding |
| Template Instantiation | Working — class + function templates, member templates, deduction, dedup, transitive deps, NTTP literal-value substitution + mangling, lambdas under template substitution, statement-expressions in template bodies |
| C Codegen | Working — structs, vtables, ctors/dtors, scope cleanup, name mangling, temp materialization, range-for, lambdas (capturing + non-capturing), inline variables, scoped enums, anonymous-enum array bounds, static class data members lowered to TU-scope |
| Exceptions (Phase 2) | Working — try / catch / throw of primitive types, cross-function propagation via TLS-polling per `docs/exceptions.md`. Class-type throws + dtor-on-unwind + RTTI-based catch-by-base land in later phases. Compile with `-fno-exceptions` to bypass entirely (the gcc 4.8 bootstrap target). |
| Standard Library | 80/80 libstdc++ headers parse and emit through `--emit-c` |

### Test Suite

- 144 lexer unit tests
- 43 parser integration tests
- 451 emit-c end-to-end tests (C++ in → C out → compile → execute → verify)
- 4 multi-TU deduplication tests
- 28/28 gated + 52/52 stretch libstdc++ header smoke tests
- 2 gcc-standalone tests
- **g++.dg `dg-do run` corpus: 385 PASS / 6 FAIL.** All 6 FAILs are
  out-of-scope for C++98: 2 are deliberately so (`eh/sighandle.C` signal
  handlers, `eh/simd-4.C` SIMD intrinsics), 3 are C++11 features
  (`cpp0x/implicit2.C` implicit-move via inherited ctor, `cpp0x/initlist49.C`
  std::initializer_list, `tls/thread_local6g.C` C++11 thread_local dtor),
  and 1 is flag-only (`init/copy3.C` needs `-fno-elide-constructors`).
- gcc 4.8 source: 403 of 404 .o files in cc1plus rebuild compile clean
  through cc1plus-built-by-sea-front
- gcc 14 libcpp/ source: 5 of 16 .cc files transpile + compile cleanly
  (informational; gcc 14 isn't a Stage A target)

## Bootstrap context (Stage A, gcc 4.8)

The C → C++ chasm via the trusted-bootstrap chain — `hex0 → … → tcc →
gcc 4.7.4`, then sea-front (built by that tcc / gcc 4.7.4) transpiles
gcc 4.8 source to C, which the trusted C compiler then builds into a
working gcc 4.8 binary. Beyond gcc 4.8, modern gcc / clang build
themselves. The milestone:

```
cc1plus-stage0  53.3 MB  (built by sea-front-cc — initial host bootstrap)
cc1plus-stage1  24.9 MB  (built by stage-0 via cc1plus-via-sf)
cc1plus-stage2  23.7 MB  (built by stage-1 — first fully self-built)
cc1plus-stage3  23.7 MB  (built by stage-2 — fixed point)
```

**stage2 vs stage3 after stripping debug + BUILD-ID and masking the 16-byte
embedded `executable_checksum`: 18,601,376 bytes, perfectly equal.** The 16
checksum bytes are gcc 4.8's own MD5 over input .o files whose debug-info
timestamps differ between stages — a non-determinism in gcc 4.8 itself, not
introduced by sea-front.

This is the bootstrappable.org rung. cc1plus-built-by-sea-front is a working,
deterministic, self-consistent C++ compiler. Reproduction recipe and the
strip+cmp comparison procedure are in [CHANGELOG.md](CHANGELOG.md) and the
session-context memory.

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

# Print version
./build/sea-front --version
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
| [Mangling](docs/mangling.md) | Name mangling framework (human-readable + Itanium); system-class interop discussion |
| [Inline & Dedup](docs/inline_and_dedup.md) | Multi-TU deduplication strategy |
| [Exceptions](docs/exceptions.md) | TLS-polling EH lowering, phased roadmap |
| [RTTI](docs/rtti.md) | `type_info` layout, `dynamic_cast` lowering (design only) |
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

The [`demo/`](demo/) directory contains five small C++ inputs paired with the C
sea-front produces: name mangling, ctors/dtors with scope cleanup, vtables,
templates, and exception handling. Each emitted C definition carries a
`/* C++: ... */` comment showing the original C++ declaration the C
implements.

## Approach

- **C++-to-C transpiler** (like cfront, but with whole-program analysis)
- **Hand-written recursive descent parser** (the only proven approach for C++)
- **Full C++17 grammar** with proper disambiguation and C++20/23 change annotations
- **AST-level template instantiation** — clone + substitute, not token replay
- **Pragmatic semantic subsetting** — enough to compile GCC (C++14) then Clang (C++17)
- **Goto-chain destructor cleanup** — O(N) code, zero runtime overhead, correct by construction
- **TLS-polling exceptions** — extends the goto-chain machinery with an
  `__SF_UNWIND_THROW` state; portable ISO C, no setjmp/longjmp, no DWARF
  unwind tables. See [docs/exceptions.md](docs/exceptions.md). The gcc 4.8
  bootstrap target is built with `-fno-exceptions -fno-rtti` and skips the
  EH machinery entirely.

## Known Gaps

- Partial template specialization — full specialization works
- SFINAE / `enable_if` — not yet supported
- `decltype` deduction — parsed but not resolved (`auto` works)
- ADL in dependent contexts (template instantiation lookup is a partial
  Phase-1/Phase-2 implementation)
- Variadic templates (out of scope for the C++03 bootstrap target)
- Exception handling — Phase 2 (polling lowering) shipped; phases 3+
  (`extern "C"` elision, full `noexcept` inference, RTTI for
  catch-by-base, `std::exception_ptr`) per
  [docs/exceptions.md](docs/exceptions.md) are deferred.
- System-class interop — code that includes libstdc++/libc++ headers
  sees polymorphic system classes (`std::exception` family) whose
  vtable bodies live in the runtime library. Sea-front skips emitting
  vtables for these; calls into their virtual methods don't link
  against `libstdc++.so` (sea-front's mangling is private). See
  [docs/mangling.md](docs/mangling.md) "System-class interop" — the
  long-term answer is to compile our own libstdc++ through sea-front.
- A handful of `SHORTCUT`-tagged narrow lowerings (vNULL, arg-type
  mangling fallback for unresolved qualified calls) — greppable, each
  cited.

## License

MIT
