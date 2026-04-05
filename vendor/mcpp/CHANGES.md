# sea-front mcpp modifications

Vendored from [h8liu/mcpp](https://github.com/h8liu/mcpp) (mcpp 2.7.2, BSD).
Upstream: Kiyoshi Matsui, 2008. No further upstream releases.

## Changes from upstream

### Already applied

1. **`noconfig.H`**: changed `SYSTEM` from `SYS_FREEBSD` to `SYS_LINUX`.

### Needed for GCC/Clang source preprocessing

The following features are missing from mcpp and required to preprocess
GCC and/or Clang source code. These are listed in priority order based
on frequency of occurrence in the Clang test suite and GCC/Clang sources.

All changes should be marked with `/* sea-front: ... */` comments.

#### Priority 1: Clang/GCC built-in feature-test macros

mcpp errors with "Operator '(' in incorrect context" on these because
it doesn't recognize them as function-like built-in macros.

| Macro | Used in | Proposed default |
|-------|---------|------------------|
| `__has_feature(x)` | Clang test suite, LLVM source | `0` (no features) |
| `__has_extension(x)` | Clang test suite, LLVM source | `0` |
| `__has_builtin(x)` | GCC/Clang source | `0` |
| `__has_include(x)` | C++17 §19.1, widely used | `0` |
| `__has_include_next(x)` | LLVM source | `0` |
| `__has_attribute(x)` | GCC/Clang source | `0` |
| `__has_declspec_attribute(x)` | MSVC compat in Clang | `0` |
| `__has_cpp_attribute(x)` | C++17 §19.1 | `0` |
| `__has_c_attribute(x)` | C23 | `0` |
| `__has_warning(x)` | Clang source | `0` |
| `__is_identifier(x)` | Clang test suite | `1` (everything is an identifier) |

Implementation: define these as built-in macros in mcpp's `system.c`
or `init_sys_macro()`. They take one argument and expand to a constant.
For the bootstrap use case, expanding to `0` is correct — we're not
claiming to support any features; we just need `#if __has_feature(x)`
to evaluate to `#if 0` and take the `#else` branch.

**`__is_identifier(x)`** is special: it returns 1 if `x` is NOT a
keyword. Default of `1` is conservative (treats everything as an
identifier).

#### Priority 2: GCC/Clang predefined version macros

These aren't built-in functions but predefined object-like macros
that GCC/Clang sources test in `#if` directives.

| Macro | Proposed value | Notes |
|-------|---------------|-------|
| `__GNUC__` | `4` | GCC version major |
| `__GNUC_MINOR__` | `8` | GCC version minor |
| `__GNUC_PATCHLEVEL__` | `0` | |
| `__cplusplus` | `201703L` | C++17 |
| `__cpp_*` feature macros | per-feature | N4659 §19.1 Table 36 |
| `__STDC_VERSION__` | `201710L` | For C mode |

#### Priority 3: GCC variadic macro extensions

mcpp errors on `#define FOO(fmt, args...)` — the GCC extension for
named variadic macros. GCC/Clang source uses this pattern.

Also: `##__VA_ARGS__` (the comma-deletion extension) and
`__VA_OPT__(,)` (C++20).

#### Priority 4: C++23/C23 preprocessor features

| Feature | Standard | Notes |
|---------|----------|-------|
| `#elifdef` / `#elifndef` | C23/C++23 | mcpp doesn't recognize |
| `#embed` | C23/C++26 | mcpp doesn't recognize |
| `#warning` | C23/C++23 | mcpp may not support |

#### Priority 5: Miscellaneous

- Allow redefining `__FILE__`, `__TIME__` etc. (currently an error)
- `_Pragma("...")` operator form (C99/C++11) — may need improvement
- `__COUNTER__` macro (GCC/Clang extension)
- Control character tolerance (0x1a = Ctrl-Z / DOS EOF)
