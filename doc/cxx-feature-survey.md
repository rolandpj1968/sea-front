# C++ Feature Survey: GCC and Clang/LLVM Build Requirements

## Purpose

This document surveys the C++ language features required to compile the GCC and
Clang/LLVM compiler source code. The goal is to define the subset of C++ that a
C-written transpiler (cfront-style, C++ to C) must support in order to
bootstrap these compilers from a trusted C toolchain.

---

## 1. C++ Standard Requirements

| Target     | Required Standard | Bootstrap Compiler          |
|------------|-------------------|-----------------------------|
| GCC 15+    | C++14             | GCC 5.4+ or equivalent      |
| LLVM/Clang | C++17             | Clang 5.0, GCC 7.4, or equivalent |

**LLVM is the harder target.** It requires C++17 and uses features like
`if constexpr`, fold expressions, structured bindings, and `std::optional`.

GCC is more conservative at C++14, but still uses templates heavily, lambdas,
constexpr, move semantics, and type traits.

---

## 2. Features NOT Needed (Explicitly Prohibited by Both)

Both GCC and Clang/LLVM compile with `-fno-exceptions -fno-rtti`. This is
critical — it eliminates two of the hardest C++ features to transpile to C:

| Feature              | GCC        | LLVM/Clang | Needed? |
|----------------------|------------|------------|---------|
| Exceptions           | Prohibited | Prohibited | **NO**  |
| RTTI (dynamic_cast)  | Prohibited | Prohibited | **NO**  |
| `<iostream>`         | Discouraged| Prohibited | **NO**  |

Both projects implement their own alternatives:
- **Casting**: GCC uses `is_a<>`, `as_a<>`, `dyn_cast<>` (in `is-a.h`);
  LLVM uses `isa<>`, `cast<>`, `dyn_cast<>` (in `Casting.h`).
  Both rely on static `classof()` methods and kind enums, not RTTI.
- **Error handling**: GCC aborts on errors; LLVM uses `llvm::Error` and
  `llvm::Expected<T>` (move-only value types).

---

## 3. Core Language Features Required

### 3.1 Templates (HEAVY — both codebases)

This is the single largest feature area. Both codebases are heavily
template-driven.

| Sub-feature                        | GCC | LLVM | Notes |
|------------------------------------|-----|------|-------|
| Class templates                    | Yes | Yes  | Containers, traits, casting infra |
| Function templates                 | Yes | Yes  | Utilities, algorithms |
| Template specialization (full)     | Yes | Yes  | Type-specific behavior |
| Template specialization (partial)  | Yes | Yes  | Trait dispatch, SFINAE patterns |
| Variadic templates                 | Yes | Yes  | LLVM's `isa<A, B>(val)` |
| SFINAE / `std::enable_if`         | Yes | Yes  | Compile-time dispatch |
| CRTP                               | Yes | Yes  | LLVM's `RTTIExtends<>` |
| Dependent names (`typename`, `template` keywords) | Yes | Yes | |
| Non-type template parameters       | Yes | Yes  | e.g., `SmallVector<T, N>` |
| Default template arguments         | Yes | Yes  | |
| Template argument deduction        | Yes | Yes  | |
| Fold expressions (C++17)           | No  | Yes  | LLVM casting infra |
| CTAD (C++17)                       | No  | Yes  | Where appropriate |

### 3.2 Classes and Inheritance

| Sub-feature                | GCC | LLVM | Notes |
|----------------------------|-----|------|-------|
| Single inheritance         | Yes | Yes  | Dominant pattern |
| Multiple inheritance       | Rare| Rare | Interface-style only |
| Virtual inheritance        | Rare| Rare | |
| Virtual functions          | Yes | Yes  | Polymorphic dispatch |
| Pure virtual functions     | Yes | Yes  | Abstract interfaces |
| `override`                 | Yes | Yes  | |
| `final`                    | Yes | Yes  | |
| Access control (public/protected/private) | Yes | Yes | |
| Constructors (default, copy, move) | Yes | Yes | |
| Destructors (including virtual) | Yes | Yes | |
| Delegating constructors    | Yes | Yes  | C++11 |
| `explicit` constructors   | Yes | Yes  | Mandatory for single-arg |
| Defaulted/deleted special members | Yes | Yes | `= default`, `= delete` |
| Member initializer lists   | Yes | Yes  | Required by GCC conventions |
| In-class member initializers | Yes | Yes | |

### 3.3 Move Semantics and Value Categories

| Sub-feature              | GCC | LLVM | Notes |
|--------------------------|-----|------|-------|
| Rvalue references (`&&`) | Yes | Yes  | |
| Move constructors        | Yes | Yes  | |
| Move assignment          | Yes | Yes  | |
| `std::move`              | Yes | Yes  | |
| `std::forward`           | Yes | Yes  | Perfect forwarding |
| Move-only types          | Yes | Yes  | LLVM's `Error`, `Expected<T>` |
| Return value optimization| Yes | Yes  | Relied upon implicitly |

### 3.4 Type Deduction and Compile-Time Features

| Sub-feature              | GCC | LLVM | Notes |
|--------------------------|-----|------|-------|
| `auto` variables         | Yes | Yes  | |
| `auto` return types      | Yes | Yes  | C++14 |
| `decltype`               | Yes | Yes  | |
| `decltype(auto)`         | Yes | Yes  | LLVM casting infra |
| `constexpr` variables    | Yes | Yes  | |
| `constexpr` functions    | Yes | Yes  | |
| `if constexpr` (C++17)   | No  | Yes  | Compile-time branching |
| `static_assert`          | Yes | Yes  | |
| `sizeof...` (parameter packs) | Yes | Yes | |

### 3.5 Lambdas

| Sub-feature              | GCC | LLVM | Notes |
|--------------------------|-----|------|-------|
| Basic lambdas            | Yes | Yes  | Callbacks, algorithms |
| Capture by reference     | Yes | Yes  | GCC prefers `[&]` for local lambdas |
| Capture by value         | Yes | Yes  | |
| Init captures (`[x = expr]`) | Yes | Yes | C++14 |
| Generic lambdas (`auto` params) | Yes | Yes | C++14 |
| Mutable lambdas          | Yes | Yes  | |

### 3.6 Other Language Features

| Sub-feature               | GCC | LLVM | Notes |
|---------------------------|-----|------|-------|
| Namespaces                | Yes | Yes  | |
| `using` declarations      | Yes | Yes  | |
| `using` type aliases       | Yes | Yes  | Preferred over `typedef` |
| Alias templates           | Yes | Yes  | `template<typename T> using X = ...` |
| `enum class`              | Some| Yes  | GCC still has many old-style enums |
| `nullptr`                 | Yes | Yes  | GCC still has legacy `NULL` usage |
| Range-based for loops     | Yes | Yes  | Mandatory in new LLVM code |
| Uniform/brace initialization | Yes | Yes | With restrictions |
| `[[nodiscard]]`           | Yes | Yes  | C++17 attribute |
| `alignas`                 | Yes | Yes  | Low-level data structures |
| Structured bindings (C++17)| No | Yes  | Map/pair decomposition |
| `inline` variables (C++17) | No | Yes  | |
| Operator overloading      | Yes | Yes  | Streams, containers, iterators |
| User-defined conversions  | GCC avoids | Yes | |
| Placement `new`           | Yes | Yes  | |

---

## 4. Standard Library Features Required

### 4.1 Headers Used by Both

| Header              | Key Features Used |
|---------------------|-------------------|
| `<type_traits>`     | `is_base_of`, `enable_if`, `conditional`, `is_trivially_copyable/destructible`, `remove_cv`, `is_same`, etc. |
| `<utility>`         | `std::move`, `std::forward`, `std::pair`, `std::swap`, `std::declval` |
| `<memory>`          | `std::unique_ptr`, `std::shared_ptr` (rare), placement `new` |
| `<algorithm>`       | `std::sort`, `std::find`, `std::find_if`, `std::lower_bound`, `std::move` (range version) |
| `<functional>`      | `std::function`, `std::less` |
| `<initializer_list>`| `std::initializer_list<T>` |
| `<iterator>`        | `std::reverse_iterator`, iterator traits |
| `<string>`          | `std::string` |
| `<cstdint>`         | Fixed-width integer types |
| `<cstddef>`         | `size_t`, `ptrdiff_t`, `nullptr_t` |
| `<cstring>`         | `memcpy`, `memset`, `strlen`, etc. |
| `<cassert>`         | `assert()` |
| `<new>`             | Placement new, `std::align_val_t` |
| `<tuple>`           | `std::tuple`, `std::tie`, `std::get` |

### 4.2 Additional Headers — GCC

| Header        | Notes |
|---------------|-------|
| `<vector>`    | `std::vector` (not GC-managed data) |
| `<map>`       | `std::map` |
| `<set>`       | `std::set` |
| `<list>`      | `std::list` |
| `<deque>`     | `std::deque` |
| `<array>`     | `std::array` |
| `<sstream>`   | `std::stringstream` |
| `<mutex>`     | Threading (conditional) |

### 4.3 Additional Headers — LLVM

LLVM prefers its own containers over STL equivalents:

| STL                | LLVM Replacement | Notes |
|--------------------|------------------|-------|
| `std::vector`      | `SmallVector<T,N>` | Stack-allocated small buffer |
| `std::map`         | `DenseMap<K,V>`  | Open-addressing hash map |
| `std::set`         | `DenseSet`, `SmallPtrSet` | |
| `std::string` (ref)| `StringRef`     | Non-owning string reference |
| `span`-like        | `ArrayRef<T>`    | Non-owning contiguous view |

LLVM also uses from C++17:
- `std::optional<T>`
- `std::string_view` (though `StringRef` is still preferred)

---

## 5. Transpilation Complexity Assessment

### Relatively Straightforward to Transpile to C

- Classes with single inheritance → C structs + function tables
- Virtual functions → vtable structs, manual dispatch
- Namespaces → name mangling / prefixing
- `enum class` → C enums with prefixed names
- Range-based for → iterator expansion
- `nullptr` → `NULL` / `(void*)0`
- `auto` → resolve to concrete types at compile time
- `static_assert` → `_Static_assert` (C11) or compile-time check
- Operator overloading → named function calls
- References → pointers (mostly)
- `override` / `final` → semantic checks only, no codegen impact

### Moderate Difficulty

- Templates → monomorphization (generate specialized C code per instantiation)
- Move semantics → careful pointer/ownership management in generated C
- Lambdas → structs with captured variables + function pointers
- `constexpr` → compile-time evaluation in the transpiler
- SFINAE / `enable_if` → template resolution logic in the transpiler
- `std::unique_ptr` → generated cleanup code (destructors at scope exit)
- Destructors / RAII → scope-based cleanup (goto chains or similar)

### Hard

- Partial template specialization → pattern matching on types
- Variadic templates → recursive instantiation
- Fold expressions → expand at compile time
- `if constexpr` → dead branch elimination during template instantiation
- CRTP → mutual recursion between template and derived class
- `decltype(auto)` → full expression type deduction
- `std::function` → type-erased callable (vtable-like wrapper)
- Standard library reimplementation → substantial effort for `<type_traits>`,
  `<algorithm>`, containers, `<memory>`
- Multiple/virtual inheritance → complex object layouts, pointer adjustments

---

## 6. Recommended Implementation Stages

### Stage 1: C with Classes
- Structs with methods, constructors, destructors
- Single inheritance, virtual functions (vtables)
- Access control, `explicit`, member initializer lists
- References, `nullptr`
- Namespaces (as name prefixes)
- Operator overloading
- `enum class`
- Basic RAII / destructor scope cleanup

### Stage 2: Templates (Basic)
- Class templates, function templates
- Full and partial specialization
- Non-type template parameters
- Default template arguments
- Template argument deduction
- Basic `std::` type traits

### Stage 3: Modern C++11/14 Core
- Move semantics (rvalue refs, move ctors/assignment)
- `auto`, `decltype`
- Lambdas (including generic lambdas, init captures)
- `constexpr` functions and variables
- `static_assert`
- `= default`, `= delete`
- Variadic templates
- SFINAE / `enable_if`
- `using` type aliases and alias templates
- Initializer lists

### Stage 4: C++17 Features (for LLVM)
- `if constexpr`
- Structured bindings
- Fold expressions
- `inline` variables
- `std::optional`
- `[[nodiscard]]`
- Class template argument deduction (CTAD)

### Stage 5: Standard Library
- `<type_traits>` (substantial subset)
- `<memory>` (`unique_ptr`, allocator support)
- `<algorithm>` (sort, find, etc.)
- `<functional>` (`std::function`)
- `<string>`, `<tuple>`, `<utility>`
- Enough container support for both codebases' needs
- LLVM's own ADT headers may need special handling

### Stage 6: Full Bootstrap Target
- Can compile GCC's C++ source → produces working g++
- Can compile LLVM/Clang's C++ source → produces working clang++
- Validated: resulting compilers pass their own test suites

---

## 7. Key Observations

1. **No exceptions, no RTTI.** This is the single biggest simplification.
   These are the hardest C++ features to transpile to C, and neither
   target needs them.

2. **Templates are the dominant challenge.** Both codebases are heavily
   template-driven. A correct template instantiation engine is the
   core of this project.

3. **LLVM is harder than GCC.** LLVM requires C++17 and uses more modern
   features. GCC at C++14 is a more tractable first target.

4. **The standard library is a separate problem.** Both codebases need
   substantial `<type_traits>` support and various containers/algorithms.
   This is a large but well-defined body of code to implement.

5. **ABI compatibility is not required.** The transpiled code only needs to
   produce correct binaries, not link against external C++ libraries.

6. **Optimization is not required.** The transpiler's output just needs to
   be correct. The bootstrapped GCC/Clang will optimize themselves.

7. **Suggested order: GCC first, then LLVM.** GCC's C++14 requirement is
   a strict subset of LLVM's C++17 requirement. Successfully compiling
   GCC gives you a working C++ compiler to validate against before
   tackling LLVM.
