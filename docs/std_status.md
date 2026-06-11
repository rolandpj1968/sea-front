# C++ standard support

Sea-front is a forward-tracking C++ → C transpiler. The intended
input dialect is "modern C++ as commonly written today" — code that
gcc and clang accept under recent `-std=` settings. There is no
separate per-`-std=` parser mode; the parser accepts everything it
knows about, regardless of the `--std=` flag, and `--std=` is
forwarded to the preprocessor (`g++ -E`) so libstdc++ headers fork
correctly on `__cplusplus`.

This document is the honest map of which standard features sea-front
actually transpiles correctly. The empirical reality check is the
gcc dg pass-rate; the per-feature tables below explain *why* the
pass-rate is what it is.

## Why forward-only

C++ source-level evolution from C++11 onward is essentially
monotonic-adds. The standard's backwards-breaking changes are a
short, mostly-cosmetic list, and real code post-2015 trips none of
them in practice. So a forward-tracking transpiler doesn't need to
gate the parser by `-std=`: a single recent dialect covers nearly
all input.

### Backwards-breaking constructs (and our shim policy)

| Construct | When broken | Sea-front policy |
|---|---|---|
| `auto x;` as storage-class specifier | Removed C++11 (rebinding of `auto` keyword) | Accept (rare, harmless to keep) |
| `throw(T1, T2)` dynamic exception spec | Deprecated C++11, removed C++17 | Accept; treat as advisory / `noexcept(false)` |
| `register` as storage class | Removed C++17 | Accept; silently dropped |
| Trigraphs (`??=`, …) | Removed C++17 | Handled by preprocessor; pass through |
| `bool` increment (`++b;`) | Removed C++17 | Accept |
| New reserved words colliding with old variable names (`constexpr`, `nullptr`, `decltype`, `thread_local`, `alignas`, `static_assert`, `noexcept`, `char16_t`, `char32_t`, `concept`, `requires`, `consteval`, `constinit`, `co_await/yield/return`, …) | Reserved progressively from C++11+ | Treat as reserved at all times; legacy code using them as identifiers will not transpile (vanishingly rare) |
| Library removals (`auto_ptr`, `random_shuffle`, `bind1st/2nd`, dynamic-exception-spec traits, …) | Removed C++17 | n/a (library / header concern, inherits from the linked libstdc++) |

The whole break-list is short enough that sea-front can plausibly
accept "always-latest C++" plus the legacy shims simultaneously,
which is what it does. The honest claim is: *"sea-front accepts
modern C++ source; legacy constructs removed by newer standards are
still accepted as a convenience."*

## How to read the tables

- **yes** — implemented and exercised by tests
- **partial** — recognised; specific shapes or interactions known
  to fail; notes explain the gap
- **no** — not yet implemented; using the feature produces a
  parse error or unsupported-feature error
- **n/a** — preprocessor / library / semantic concern, not a
  parser/codegen one

C++98/03 features are the implicit baseline — anything not listed
is expected to work. The tables below cover only features
introduced in C++11 onward.

## C++11

### Language

| Feature | Status | Notes |
|---|---|---|
| `auto` type deduction | yes | |
| `decltype` | yes | |
| Trailing return type `auto f() -> T` | yes | |
| Range-based `for` | yes | Array and iterator forms |
| Lambda expressions | yes | Including default capture (`[=]`, `[&]`) |
| `nullptr` / `std::nullptr_t` | yes | |
| Rvalue references, move semantics | yes | |
| Variadic templates | partial | Recognised; pack expansion in initialiser-list / brace-init contexts has gaps |
| `static_assert` | yes | Constant folding handles literal arithmetic; calls to constexpr functions in the predicate are not yet folded |
| `constexpr` functions and variables | partial | Parsed and lowered; not *evaluated*. Variables emit as `static const`; functions emit as `static inline`. Calls to constexpr functions in constant-expression contexts (array bounds at namespace scope, template non-type args) fail unless the result is already a literal |
| `noexcept` operator and specifier | partial | Parsed; violation lowers to `__sf_terminate` (see `docs/exceptions.md`). Inference (phase 4) not done |
| Inheriting constructors (`using Base::Base`) | partial | Single-base case; multi-base / template-base cases drop |
| Defaulted / deleted member functions (`= default`, `= delete`) | yes | |
| Explicit conversion operators (`explicit operator T()`) | yes | |
| `override` / `final` | yes | |
| Strongly-typed scoped enums (`enum class`) | yes | |
| Forward-declared enums | yes | |
| `alignas` / `alignof` | yes | |
| `thread_local` | partial | Per-TU storage works; cross-TU dtor cleanup tracked in `docs/exceptions.md` |
| Brace-init / list-init | partial | Aggregate and array forms work; `std::initializer_list<T>` interactions are the biggest open cluster — see Empirical pass-rates below |
| `std::initializer_list<T>` | no | Slated as the next major slice |
| Delegating constructors | yes | |
| Non-static data member initialisers (NSDMI) | partial | Literal initialisers work; calls and braces inside NSDMI drop in some shapes |
| Attributes `[[noreturn]]`, `[[carries_dependency]]` | yes | gcc-style `__attribute__((...))` is also accepted |
| User-defined literals (`operator""`) | no | |
| Raw / Unicode string literals | partial | UTF-8 string literals tokenise; raw-string literals not yet |
| `char16_t` / `char32_t` | partial | Tokenised; treated as `uint16_t` / `uint32_t` underneath |
| Right-angle bracket `>>` in templates | yes | |
| Inline namespaces | yes | |
| Generalised constant expressions in template args | partial | Literal args fine; `f(N)` in template args needs the constexpr evaluator |
| Unrestricted unions | partial | |
| Ref-qualified member functions (`& / &&`) | partial | Parsed; overload resolution doesn't yet rank on ref-qualifier |

### Library

Library support comes from whatever libstdc++ (or other STL
implementation) the preprocessor pulls in. Sea-front handles most
of gcc 14's `<bit>`, `<charconv>`, `<csetjmp>`, `<csignal>`,
`<cstdarg>`, `<cstddef>`, `<cstdint>`, `<cstdio>`, `<cstdlib>`,
`<cstring>`, `<ctime>`, `<cwchar>`, `<cwctype>`, `<cassert>`,
`<cctype>`, `<cerrno>`, `<cfenv>`, `<cfloat>`, `<cinttypes>`,
`<climits>`, `<clocale>`, `<cmath>`, `<exception>`, `<filesystem>`,
and others — see `tests/test_libstdcxx_headers.sh` for the gated
set. Containers, algorithms, and `<initializer_list>` itself are
gated on the brace-init / initializer-list slice.

## C++14

| Feature | Status | Notes |
|---|---|---|
| Variable templates | partial | Type-arg form works; non-type-arg form has gaps |
| Generic lambdas (`auto` parameters) | partial | Recognised in the parse path; deduction has gaps |
| Lambda init-capture (`[x = expr]`) | no | |
| `auto` return-type deduction | partial | Single-return functions yes; multi-return-merge not done |
| `decltype(auto)` | partial | Recognised; treated as `auto` in most contexts |
| Relaxed `constexpr` (loops, locals, mutation) | no | Would need an extended constexpr evaluator; deferred |
| Member initialisers and aggregates | partial | Tied to the brace-init slice |
| Binary literals `0b...` | yes | |
| Digit separators `1'000'000` | yes | |
| `[[deprecated]]` attribute | yes | Accepted; not enforced |
| Sized deallocation | n/a | Library concern |

## C++17

| Feature | Status | Notes |
|---|---|---|
| `if constexpr` | no | Parser accepts; lowered as a regular `if` (wrong semantics — see below) |
| Structured bindings (`auto [a, b] = ...`) | no | |
| Fold expressions (`(args + ...)`) | no | |
| `inline` variables | partial | Keyword recognised; cross-TU dedup follows the existing scheme in `docs/inline_and_dedup.md` |
| Selection statements with init (`if (init; cond)`) | no | |
| Class template argument deduction (CTAD) | no | |
| `auto` in non-type template parameters | no | |
| Nested namespace definitions (`namespace A::B::C`) | yes | |
| `__has_include` | n/a | Preprocessor (`g++ -E`) |
| `[[fallthrough]]`, `[[nodiscard]]`, `[[maybe_unused]]` | yes | Accepted; not enforced |
| `u8` character literal | yes | |
| Guaranteed copy elision | partial | Some call sites elide; not yet systematic |
| Dynamic exception specs removed | yes (shim) | `throw(T)` still accepted for legacy input |
| `register` keyword removed | yes (shim) | Still accepted; silently dropped |
| Trigraphs removed | n/a | Preprocessor handles |
| `std::byte`, `std::optional`, `std::variant`, `std::string_view` | n/a | Library; depend on the brace-init / template slices |

The `if constexpr` gap is the most user-visible C++17 hole — gcc 14
libstdc++ uses it heavily in `<type_traits>` and friends, so the
preprocessor path through those headers may pull both branches into
sea-front's input even when only one is valid. This shows up as
spurious type errors on standard-library headers.

## C++20

| Feature | Status | Notes |
|---|---|---|
| Concepts / `requires` | no | |
| Modules (`import`, `module`) | no | Out of scope (sea-front emits portable C, not modular C++) |
| Coroutines (`co_await`, `co_yield`, `co_return`) | no | Large slice; deferred |
| Three-way comparison `<=>` (spaceship) | no | |
| Designated initialisers (`{.x = 1, .y = 2}`) | partial | Aggregate form transpiles to C99 designated init; order constraints not enforced |
| `consteval` / `constinit` | no | Requires a constexpr evaluator |
| Lambda templates, pack init-capture | no | |
| `using enum` | no | |
| `std::is_constant_evaluated()` | no | |
| `[[likely]]` / `[[unlikely]]` | yes | Accepted; passed through to emitted C if the target compiler supports it |
| `[[no_unique_address]]` | partial | Accepted; layout impact not yet honoured |
| Range-based `for` with init-statement | no | |

## C++23

| Feature | Status | Notes |
|---|---|---|
| `if consteval` | no | |
| Multidimensional subscript `a[i, j]` | no | |
| Deducing `this` (`auto this`) | no | |
| Static call operator (`static operator()`) | no | |
| `if`/`while` with optional init-statement | partial | |
| `[[assume(...)]]` | no | |
| `std::expected` | n/a | Library |
| `std::print` / `<print>` | n/a | Library |

## Tooling and preprocessor

| Concern | Status | Notes |
|---|---|---|
| `__cplusplus` value forwarded from `--std=` | yes | Via `g++ -E` in `scripts/sea-front-cc` |
| Feature-test macros (`__cpp_concepts`, `__cpp_lambdas`, …) | n/a | Whatever `g++ -E` defines under the chosen `--std=` |
| GNU `__attribute__((...))` | yes | Primary form for weak, packed, ctor/dtor, noreturn, aligned, … |
| `_Pragma` | partial | Most pragmas pass through unchanged |
| Inline assembly (`asm`, `__asm__`) | partial | Recognised and passed through; target-specific operands not validated |
| Predefined macros (`__FILE__`, `__LINE__`, `__func__`) | yes | |

## Empirical pass-rates

The aggregate gcc dg pass-rate is the empirical reality check.
Sea-front does not gate by `--std=`, so per-std bucketing is
intentionally not reported.

| Suite | Date | Pass / Total |
|---|---|---|
| gcc 4.8 g++.dg dg-do run | 2026-06-11 | 404 / 809 (50%) |
| gcc 14.2 g++.dg dg-do run | 2026-06-11 | 599 / 1671 (36%) |

The gcc-14 number is the primary target going forward. The gcc-4.8
number is retained as a regression marker — it should not move
downward as gcc-14 work lands.

Run reproducers:

```
make test           # gated suites
scripts/run_gpp_dg.sh > /tmp/dg_4.8.log
SEA_GCC_TESTSUITE=$HOME/src/sea-front-deps/gcc-14.2.0/gcc/testsuite \
    scripts/run_gpp_dg.sh > /tmp/dg_14.log
```
