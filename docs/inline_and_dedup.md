# C++ `inline` Semantics and Multi-TU Deduplication

Status: **design decided, implementation deferred**. The implementation
is task #84 (`__SF_INLINE`: weak symbols for multi-TU dedup) and is
small (~30 lines) but explicitly deferred until we have a real
multi-TU build to test against.

## What C++ `inline` actually means

`inline` is widely misread as "please inline this at the call site".
The optimization hint exists, but it's **secondary**. The core
semantics (N4659 §10.1.6 [dcl.fct.spec]) are:

1. **Multi-definition is allowed across TUs.** You can define the
   same inline function in every TU that includes the header, and
   it's not an ODR violation as long as the definitions are
   token-identical.
2. **The linker is required to merge them into one entity.** All
   TUs end up calling "the same" function, with the same address,
   the same static-local state, etc.
3. **In-class method definitions are implicitly inline** (§10.1.6/3).
4. **Template instantiations are implicitly inline.** Each TU that
   touches a template independently emits the instantiation; the
   linker merges them.

Point 2 is the killer for transpilation. C has nothing like it.
C99 `inline` is a much weaker beast — basically "you may not see this
function's body during this TU; expect to find it `extern`-defined
elsewhere". Link-time deduplication of identical function bodies is
**not** a C language feature.

## Why sea-front hits this immediately

Today sea-front transpiles one .cpp → one .c. Realistic header chain:

```cpp
// vec.h
template<class T> struct vec {
    int size;
    int get(int i) { return data[i]; }   // in-class def, implicitly inline
    T *data;
};

inline int peek_size(const vec<int> &v) { return v.size; }
```

```cpp
// a.cpp
#include "vec.h"
int use_a(vec<int> &v) { return peek_size(v) + v.get(0); }
```

```cpp
// b.cpp
#include "vec.h"
int use_b(vec<int> &v) { return peek_size(v) - v.get(0); }
```

When sea-front processes `a.cpp` and `b.cpp`, **each one** emits:

- `int peek_size(const struct vec_int *v) { return v->size; }`
- `int vec_int_get(struct vec_int *this, int i) { return this->data[i]; }`

Linking `a.o` + `b.o` → multiple-definition error on every shared
inline function.

The same problem affects:

- Every class method defined inline in a header
- Every synthesized helper sea-front emits for a class (`Class_ctor`,
  `Class_dtor`, `Class_dtor_body`) — these are effectively
  inline-equivalent because they appear in every TU that uses the
  class
- Every template instantiation, when we eventually lower templates

## The option space (and why we picked one)

### Option A: Weak symbols (chosen)

Emit each inline function with `__attribute__((weak))`. ELF, Mach-O,
and PE-COFF all handle weak via the linker — one copy wins, the rest
are dropped silently.

**Pros**:
- Minimal change: one attribute on each emitted function.
- The C compiler still sees the body, so it can still inline at the
  call site. Performance unchanged.
- Address-equality holds across TUs (linker resolves all references
  to the surviving symbol).
- Static locals inside an inline function exist exactly once
  program-wide (in the surviving copy), matching C++ semantics.
- Template instantiations get the same treatment for free — same
  linker mechanism.

**Cons**:
- `__attribute__((weak))` is a non-standard extension. It works on
  GCC, Clang, and MSVC (`__declspec(selectany)`) but not on every
  weird embedded toolchain. For sea-front's bootstrap goal we're
  targeting GCC/Clang, so we have the extension.
- ODR violations (different definitions in different TUs with the
  same mangled name) become **silent corruption** rather than
  link errors. Same risk as in C++ proper, where mangling alone
  doesn't catch all ODR breaks.

### Option B: Static-per-TU (rejected)

Emit every inline function as `static`. Each TU has its own private
copy.

**Pros**: Standard C, no linker tricks.

**Cons**:
- Each TU has its own private copy → binary bloat. The C compiler
  will inline and DCE most copies, but not always.
- **Address-equality breaks** — each TU has a different address for
  what should be "the same" function.
- **Function-local statics inside inline functions get per-TU
  state**, which is observably wrong. (`inline int counter() {
  static int n = 0; return ++n; }` would count separately in each
  TU.) This is the deal-breaker — bug class is invisible until it
  isn't, which is the worst kind.

### Option C: `static inline` in shared headers (rejected for now)

Extract every inline function into a generated header (one per
source header, mirroring the original include structure), have each
emitted .c `#include` it. This is what idiomatic C does for its
own inline functions.

**Pros**: Closest fit to C++'s mental model. The C compiler treats
it correctly. Build pipeline parallels the original.

**Cons**:
- Same `static`-bloat and address-equality issues as B.
- Requires a **pipeline rethink**: sea-front would need to track
  which functions originated in which source header, emit a
  corresponding generated `.h`, and have each emitted `.c`
  `#include` the right ones.
- Output model changes from one-file-per-TU to one-file-per-header
  + one-file-per-TU, which the user explicitly does NOT want
  (single TU → TU is preferred for review reasons; see
  [Mangling design](mangling.md)).

### Option D: Whole-program amalgamation (rejected)

Preprocess + transpile all .cpp files into a single .c "unity build".
ODR-violation-detection and dedup happen within one TU where C's
rules apply naturally.

**Pros**: Sidesteps the multi-TU question entirely.

**Cons**: Huge change to pipeline. Doesn't scale to large codebases
the way separate compilation does. Debugging is harder. Doesn't
match the user's preferred output model.

### Option E: Owner-TU per inline function (rejected)

Pick one TU per inline function to be the "owner" and emit the body
only there. All other TUs see only a forward declaration.

**Pros**: Standard C, no weak symbols, no header tricks.

**Cons**: Requires global knowledge — sea-front would need a
multi-TU compilation phase rather than single-TU. Violates the
preferred pipeline shape.

## The chosen plan: weak symbols (`__SF_INLINE`)

Add a prelude macro:

```c
#if defined(__GNUC__) || defined(__clang__)
#  define __SF_INLINE __attribute__((weak))
#elif defined(_MSC_VER)
#  define __SF_INLINE __declspec(selectany)
#else
#  define __SF_INLINE   /* fall back: each TU has its own copy */
#endif
```

Tag every emitted function whose source was inline-eligible with
`__SF_INLINE`:

- **All class methods** (in-class definitions are implicitly inline)
- **Synthesized helpers** sea-front emits per class:
  - `Class_ctor`
  - `Class_dtor`
  - `Class_dtor_body`
  - The constructor wrapper that runs member-init lists then user
    body
  - The destructor wrapper that calls `_dtor_body` then chains
    member dtors
- **Free functions explicitly marked `inline`** in the source
  (requires the parser to tag them; today the `inline` keyword is
  parsed but the flag isn't propagated to codegen)
- **Template instantiations**, when we lower them — every
  instantiated function gets the marker. The deterministic mangling
  ensures two TUs producing the same instantiation hit the same
  mangled name and the linker dedupes.

**Don't tag**:

- `int main` (must be unique program-wide)
- Free functions defined at namespace scope WITHOUT `inline`
  (these are unique-symbol by C++ semantics)
- Anything with `extern "C"` linkage (the user explicitly opted
  into the C global namespace; we shouldn't be emitting weak
  symbols for them)

## extern "C" interaction

This is the same gap discussed in [mangling.md](mangling.md). The
`__SF_INLINE` plan sits on top of the mangling decision: every
sea-front-emitted symbol that gets mangled into the `sf__` namespace
also gets `__SF_INLINE`. Symbols in the global C namespace
(`extern "C"` and bare `int main`) do not.

## Templates: same mechanism, harder mangling

Templates are an extension of the inline problem with one extra
prerequisite: **deterministic mangling across TUs**.

For weak-symbol dedup to work, two TUs that instantiate the SAME
template with the SAME args must produce byte-identical mangled
names. Different instantiations (`vec<int>` vs `vec<float>`) must
produce different names. This is a hard requirement on the mangling
scheme — see `docs/mangling.md` for the full design.

Once mangling is deterministic, template instantiations get the
same `__SF_INLINE` treatment as inline functions. Each TU that
touches `vec<int>::get()` emits its own copy with the same mangled
name; the linker picks one survivor. Static locals, address-equality,
and run-time semantics all match C++.

**Edge cases that complicate the simple story** (none blocking, all
deferrable until the test surface forces them):

1. **Explicit specializations** — if TU A sees `template<> int
   f<int>() { /* different body */ }` and TU B sees the primary
   template instantiated with `int`, both produce the same mangled
   name with different bodies. Weak dedup picks one arbitrarily —
   silent miscompile. Fix: mangle specializations distinguishably,
   or rely on the C++ standard requiring all TUs see the same
   specializations.

2. **Explicit instantiation** — `template int f<int>();` requests
   that THIS TU own the symbol non-inline. Marking it weak undermines
   the intent. Fix: track explicitly-instantiated templates and
   emit them non-weak.

3. **`extern template`** — the inverse: "this TU should NOT
   instantiate, expect to find it elsewhere." If we ignore the
   directive and instantiate locally, weak-merge handles it
   correctly — just defeats the compile-time-savings purpose.

4. **Template static data members** — `template<class T> struct Foo
   { static int counter; };`. Each instantiation needs exactly one
   definition. Same weak-merge story, but at variable level rather
   than function level.

