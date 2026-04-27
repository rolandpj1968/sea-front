# C++ `inline` Semantics and Multi-TU Deduplication

Status: **implemented**, currently lowered via `static inline` after
weak symbols hit reality. The original design picked weak symbols
(option A below); a multi-week run against gcc 4.8 forced a switch
to option B (`static inline`) because weak's "keep every body in
every .o" behavior triggered link failures that had nothing to do
with anyone actually calling the inlines. See *What we implemented,
what broke, and what we did about it* below. The switch carries
real downsides — binary bloat and per-TU static-local state — and
is explicitly tagged as worth revisiting once we have either a
linker-flag plumbing pass or COMDAT support.

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

## The option space

### Option A: Weak symbols

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
- `__attribute__((weak))` is a non-standard extension.
- ODR violations become **silent corruption** rather than link errors.
- **The killer (discovered against gcc 4.8):** weak keeps the body
  in every .o regardless of whether anyone in *that* TU called the
  function. The linker therefore has to resolve every symbol the
  weak body references, even from .o files where the inline is
  unreachable. gcc 4.8's bitmap.h has

  ```cpp
  inline void dump_bitmap (FILE *f, const_bitmap m) {
      bitmap_print (f, m, "", "\n");
  }
  ```

  `bitmap_print` lives in `libbackend.a`, which the small gen-tools
  (`gencondmd`, `genpreds`, etc.) don't link. Nothing in those
  tools calls `dump_bitmap`, but with weak emission the body sat
  in every gen-tool .o anyway, dragging in a `bitmap_print`
  reference that resolved nowhere → link aborts. Same story for
  libstdc++'s `__terminate` calling `terminate` (only present in
  libsupc++/libstdc++).

### Option B: `static inline` (currently chosen)

Emit each inline function as `static inline`. Each TU has its own
private copy; the C compiler drops it via standard DCE if unused.

**Pros**:
- Standard C. Works on every toolchain, no extensions.
- **Unused inlines are dropped** before linking, sidestepping the
  Option A failure mode entirely.
- Cross-TU dedup is trivial because there's no cross-TU symbol —
  each TU has its own private copy, no possibility of multiple-
  definition.

**Cons**:
- **Binary bloat.** Every TU that includes the header carries its
  own copy of every used inline. The C compiler inlines and DCEs a
  lot, but anything not inlined at every call site (recursion,
  address-taken, large body) leaves a per-TU function. For a
  template-heavy build like gcc 4.8 (~100 vec<T> instantiations × ~20
  inline methods × ~300 TUs) the bloat is meaningful — eyeballed
  ballpark 20–40% over a COMDAT-merged build, not yet measured
  precisely.
- **Address-equality breaks.** Each TU has a different address for
  what should be "the same" function. C++ code that compares
  function pointers across TUs (`&Foo::method == ...`) gives wrong
  answers.
- **Function-local statics inside inline functions get per-TU
  state.** `inline int counter() { static int n = 0; return ++n; }`
  counts separately in each TU. C++ semantics demands one shared
  counter program-wide. This is the deal-breaker the original
  design called out for option B — accepted now because gcc 4.8's
  build doesn't appear to depend on the cross-TU sharing.

### Option C: `static inline` in shared headers (rejected)

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
  + one-file-per-TU, which the user explicitly does NOT want.

### Option D: Whole-program amalgamation (rejected)

Preprocess + transpile all .cpp files into a single .c "unity build".
ODR-violation-detection and dedup happen within one TU where C's
rules apply naturally.

**Cons**: Huge change to pipeline. Doesn't scale. Debugging is
harder. Doesn't match the preferred output model.

### Option E: Owner-TU per inline function (rejected)

Pick one TU per inline function to be the "owner" and emit the body
only there. All other TUs see only a forward declaration.

**Cons**: Requires global knowledge — sea-front would need a
multi-TU compilation phase rather than single-TU.

## What we implemented, what broke, and what we did about it

### Round 1: weak symbols (commit history)

`__SF_INLINE` macro emitted as `__attribute__((weak))` on:

- All class methods (in-class definitions are implicitly inline).
- Synthesized helpers per class: `Class_ctor`, `Class_dtor`,
  `Class_dtor_body`, the ctor wrapper, the dtor wrapper.
- Template instantiations.
- Free functions parsed with the `inline` keyword (via
  `emit_storage_flags_impl`, separate macro path).

This worked for the common case — multi-TU template instantiations
deduped, in-class method bodies deduped, `_const` mangling kept
overload distinctions intact. 145 emit-c tests passed; ~336 gcc 4.8
.o files compiled clean.

### Round 2: link failures from unused weak bodies

Three categories of failure surfaced as the gcc 4.8 build chain
got further:

1. `bitmap_print` undefined reference in `gencondmd`.
   `dump_bitmap` (inline in bitmap.h) calls `bitmap_print`. Weak
   kept the unused `dump_bitmap` body in every .o. `gencondmd`
   doesn't link `bitmap.o`. Linker fails resolving the reference.

2. `terminate` undefined reference in `genchecksum`. Same shape:
   libstdc++'s `__terminate` (extern "C++" inline in
   `bits/c++config.h`) calls `terminate` (mangled in
   libstdc++/libsupc++ as `_ZSt9terminatev`). Weak `__terminate`
   bodies live in every TU; the bare `terminate()` call inside
   them never matches the mangled name; link aborts.

3. Member-template multi-defs (`va_gc::reserve<rtx_def*>`). Weak +
   our overload-key collisions had silent-merging failure modes;
   the linker picked the wrong copy or the body was emitted as
   strong by accident.

### Round 3: switch to `static inline`

Two coordinated changes (commits e519c3d, bb74fd0, 4e06689):

- `__SF_INLINE` redefined as `static inline`.
- Forward decls reconciled with their definitions' linkage. C99
  §6.2.2/4 forbids mixing internal/external linkage between fwd
  decl and def; sea-front now walks the TU to match (in-class
  fwd decl pulls the OOL ND_FUNC_DEF's storage_flags via
  `find_ool_method_storage`).
- In-class methods (implicitly inline) → `static inline`.
- OOL methods → strong global unless DECL_INLINE explicitly set,
  with a special case: any cloned function (template instantiation,
  set in `clone.c` ND_FUNC_DEF) gets DECL_INLINE because template
  instantiations are conceptually inline (N4659 §17.5.1/4
  [temp.spec.general]).

This unblocked the build chain end to end: every link error is
gone, gen-tools build cleanly, and the failures that remain are
gen-tool runtime asserts — different category.

### What we gave up and accept for now

- **Binary bloat.** Not measured. Tolerable for sea-front's
  bootstrap target, problematic for production-grade builds.
- **Address-equality between TUs is gone.** No code in gcc 4.8
  appears to depend on it; first time we hit a real consumer this
  will need revisiting.
- **Per-TU static-local state in inline functions.** Likewise no
  hit yet. Audit needed before sea-front targets anything beyond
  gcc 4.8.

## Routes worth revisiting (when bloat or static-state bites)

These all preserve sea-front's "single .cpp → single .c" output
shape. None require the option C / D / E pipeline rewrites.

### Route 1: weak + linker GC of unused bodies

Re-emit as `__attribute__((weak))` AND require the gcc-driven link
step to use `-ffunction-sections -fdata-sections -Wl,--gc-sections`.
Each function lands in its own `.text.NAME` section; the linker
discards sections nothing reachable references. The weak body in
each .o is still present, but the linker drops it before symbol
resolution if no live code reaches it — which is exactly the
property we wanted from option A's first attempt.

**Status:** mechanically straightforward. Plumb the section flags
through `scripts/sea-front-cc` (and document them as a build-system
requirement). Restore weak emission. Keep the C99 §6.2.2/4 fwd-decl
machinery — weak symbols don't clash with the existing fwd-decl
logic. Largely undoes commit bb74fd0 with a flag-plumbing pass on
top.

**Caveats:** users invoking the C compiler directly without those
flags get the round-1 link failures back. We'd need either:
- the wrapper script enforcing the flags, or
- a documented requirement in `docs/coding-standards.md`.

### Route 2: `__attribute__((gnu_inline))`

Emit `__attribute__((gnu_inline)) inline` on inline-eligible
functions. GNU inline semantics: the inline body in a header is
discarded if not inlined at the call site; the compiler expects
to find an extern OOL definition somewhere. Each TU's body is
inlined-or-discarded — no per-TU symbol, no bloat.

**Status:** fits sea-front's emission shape minimally. The catch
is the "extern OOL definition somewhere" requirement: sea-front
would need to designate one canonical TU per inline function as
the OOL owner. We don't track that today. Requires either
per-symbol "owner TU" tracking (rejected as option E) or a
separate one-off OOL-emission pass.

### Route 3: ELF COMDAT groups (most principled)

Emit each inline-eligible function with section directives that
declare it as part of a COMDAT group keyed by its mangled name:

```c
__attribute__((section(".text.sf__vec_t_int_te___get,\"axG\",@progbits,sf__vec_t_int_te___get,comdat#")))
void sf__vec_t_int_te___get(...) { ... }
```

The linker drops all-but-one copy of each COMDAT group. This is
exactly how C++ ABIs handle inline functions — sea-front would
just be reproducing the standard mechanism in raw section
directives.

**Status:** most work. Section attributes are toolchain-specific
(GCC syntax above; clang accepts but ICC/MSVC differ). Worth
prototyping if either of (1) the bloat becomes unacceptable or
(2) we need cross-TU address equality. The output is portable
across all ELF linkers without any wrapper-script contract.

### Recommendation

If we hit a real bloat ceiling first → route 1 (cheapest by far).

If we hit a real address-equality or static-state correctness
issue first → route 3 (semantically right; route 1 doesn't fix
either).

Route 2 is appealing as a middle-ground but the "designate an OOL
owner TU" prerequisite is the same global-knowledge prerequisite
that killed option E originally — track if it ever becomes
attractive enough to warrant the pipeline change.

## extern "C" interaction

The `__SF_INLINE` plan sits on top of the mangling decision: every
sea-front-emitted symbol that gets mangled into the `sf__` namespace
also gets `__SF_INLINE`. Symbols in the global C namespace
(`extern "C"` and bare `int main`) do not.

## Templates: same mechanism, harder mangling

Templates are an extension of the inline problem with one extra
prerequisite: **deterministic mangling across TUs**. Even with
`static inline`'s no-cross-TU-symbol approach, the names still
need to be byte-identical across TUs so the same instantiation
produces the same in-TU symbol (matters for forward decls and for
tooling that reads the .o symbol tables). See `docs/mangling.md`
for the full design.

**Edge cases that complicate the simple story** (none blocking, all
deferrable):

1. **Explicit specializations** — if TU A sees `template<> int
   f<int>() { /* different body */ }` and TU B sees the primary
   template instantiated with `int`, both produce the same mangled
   name with different bodies. Under `static inline` each TU runs
   its own copy, so the divergence is silent — same hazard as
   weak's silent merge.

2. **Explicit instantiation** — `template int f<int>();` requests
   that THIS TU own the symbol non-inline. With `static inline` we
   ignore the directive (every TU gets its own copy regardless).

3. **`extern template`** — the inverse: "this TU should NOT
   instantiate, expect to find it elsewhere." Same: we instantiate
   anyway. Just defeats the compile-time-savings purpose.

4. **Template static data members** — `template<class T> struct Foo
   { static int counter; };`. Each instantiation needs exactly one
   definition program-wide. `static inline` doesn't apply to
   variables; this stays on the weak-or-COMDAT path no matter
   what we do for functions.
