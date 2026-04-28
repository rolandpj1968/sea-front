# C++ RTTI Lowering to C

Status: **design only**, not yet implemented. Current behavior:
`typeid` and `dynamic_cast` parse but lower to placeholders.
This document describes the long-term implementation for full C++
RTTI support.

The bootstrap target (gcc 4.8) was built with `-fno-rtti` and
needs none of this. Most of the complexity below exists to support
arbitrary C++ — the catch-by-base mechanism that
[`exceptions.md`](exceptions.md) requires, modern libraries that
type-switch via `dynamic_cast`, and any code that uses
`typeid(x).name()` for diagnostics or registration.

RTTI is genuinely cheap to implement *if you control both ends* —
sea-front emits both producer and consumer. The cost explosion in
real-world C++ comes from interop with libstdc++'s `type_info` and
the cross-DSO uniqueness contract; sea-front declines both and pays
none of that tax.

## Goal

Lower N4659 §8.2.7 [expr.dynamic.cast] and §8.2.8 [expr.typeid]
semantics into portable C, with these properties:

1. **Per-class typeinfo emit is small** — one symbol per polymorphic
   class, one vtable slot, one runtime helper.
2. **Pointer-form `dynamic_cast<T*>(p)` works without exceptions**.
   This is the form modern code mostly uses; uncoupling it from the
   exception machinery means RTTI is independently useful.
3. **Reference-form `dynamic_cast<T&>(r)` either throws `bad_cast`
   (when exceptions are wired up) or aborts (when they aren't)**.
   The semantic divergence is small — code that uses `T&` form is
   asserting "I know it's a T", and rarely catches `bad_cast` in
   practice.
4. **`type_info` is sea-front-private**, not Itanium-ABI compatible.
   Code emitted by sea-front cannot exchange `std::type_info&` with
   stock-compiled libraries. Same deliberate cost as exceptions —
   the alternative is becoming a real backend.
5. **Multi-thread correct**: `type_info` symbols are read-only,
   shared by all threads. No state.

## What RTTI provides

Five operations, in order of implementation cost:

### 1. `typeid(T)` for a static type

A glvalue of type `std::type_info` describing `T`. Sea-front emits
one `__sf_typeinfo_<mangled>` symbol per polymorphic class and
returns its address. Cost: one symbol per class plus the codegen
to emit `&__sf_typeinfo_<T>` at each `typeid(T)` use.

For non-polymorphic `T`, `typeid(T)` is still legal and produces
the static type's `type_info`. Same emit, no vtable needed.

### 2. `typeid(*p)` for a polymorphic expression

If `p` points to a polymorphic class (one with virtual functions),
`typeid(*p)` returns the **dynamic** type of `*p`, fetched from the
vtable. Sea-front already emits vtables; one slot is reserved for
the type_info pointer. `typeid(*p)` lowers to `(*p).__vptr->typeinfo`.

If `p` is null, the standard requires throwing `bad_typeid`
(N4659 §8.2.8/2). Same exception-coupling shape as ref-form
dynamic_cast: lowered to abort when exceptions aren't available.

For non-polymorphic `*p`, the result is the **static** type at the
expression position; no vtable lookup, no runtime dispatch. The
distinction between static-type `typeid` and dynamic-type `typeid`
is a compile-time decision, not runtime.

### 3. `std::type_info` operations

The class itself, accessed via `<typeinfo>`. Sea-front-private
layout:

```c
struct __sf_type_info {
    const char *name;      /* mangled or pretty name */
    const struct __sf_type_info *parent;   /* single-inheritance chain */
    /* multi-inheritance: array of (parent, offset) pairs */
    int n_parents;
    const struct __sf_type_info_base *parents;  /* for MI */
};
```

User-visible operations (per N4659 §21.7 [type.info]):
- `.name()` → returns the `name` field.
- `operator==` / `operator!=` → pointer comparison. Works because
  every class has exactly one `__sf_typeinfo_<X>` symbol within a
  given binary. (Cross-DSO this breaks — see Cliffs below.)
- `.before()` → arbitrary total ordering, used by `std::type_index`.
  Pointer comparison is sufficient and consistent within a binary.
- `.hash_code()` (C++11) → return `(size_t)(uintptr_t)this`.

That's the entire `type_info` API. Implementation is ~20 lines of C.

### 4. Pointer-form `dynamic_cast<D*>(b)`

Walks `b`'s dynamic type's parent chain to see if `D` is reachable.
If yes, returns the (possibly-adjusted) pointer; if no, returns
null.

Lowering:
```c
D *result = (D *)__sf_dynamic_cast(
    b,                                   /* source object pointer */
    b ? b->__vptr->typeinfo : NULL,      /* dynamic source type */
    &__sf_typeinfo_<D>);                 /* target type */
```

`__sf_dynamic_cast` is the runtime helper. For single-inheritance
hierarchies it's a parent-chain walk:

```c
void *__sf_dynamic_cast(void *p, const struct __sf_type_info *src,
                                   const struct __sf_type_info *dst) {
    if (!p) return NULL;
    for (const struct __sf_type_info *t = src; t; t = t->parent)
        if (t == dst) return p;
    return NULL;
}
```

For multi-inheritance the helper walks `parents[]` recursively and
applies the recorded offsets when descending into a base. Still a
small helper — maybe 30-40 lines for the full version.

### 5. Reference-form `dynamic_cast<D&>(r)`

Same logic as the pointer form, but null-on-fail isn't representable
because references can't be null. Per N4659 §8.2.7/11, fail throws
`std::bad_cast`. This is the canonical RTTI-couples-to-exceptions
case.

Lowered as:
```c
D *__sf_tmp = (D *)__sf_dynamic_cast(&r, /*src*/, &__sf_typeinfo_<D>);
if (!__sf_tmp)
    __sf_throw_bad_cast();   /* throws or aborts depending on build */
D &result = *__sf_tmp;
```

When exceptions are wired up, `__sf_throw_bad_cast` builds a
`std::bad_cast` exception object and sets the unwind state per
[`exceptions.md`](exceptions.md). When they aren't, it just calls
`abort()`. The latter is a semantic divergence from ISO but rarely
observable: code using `dynamic_cast<T&>` typically assumes the
cast succeeds, and would have UB on a real-compiler implementation
that doesn't catch.

## Itanium ABI history (and why sea-front opts out)

The "Itanium C++ ABI" name is misleading — it's the dominant C++
ABI on Linux/macOS/BSDs/most non-Windows Unix-likes, despite the
namesake architecture flopping. Late 1990s C++ had per-compiler
ABIs (g++ 2.95, HP aCC, Sun CC, cfront-ARM, all incompatible).
Around 1999-2000 HP/Intel/SGI/GCC collaborated on a clean,
vendor-neutral C++ ABI specification for the Itanium architecture;
the spec deliberately separated platform-specific concerns
(calling convention, register usage) from C++-specific concerns
(vtable layout, RTTI, exception handling, name mangling). GCC 3.0
adopted it as its default ABI in 2001 — a deliberate break from
g++ 2.95. Clang and ICC followed. The platform-neutral C++ parts
became the de facto Linux/macOS/BSD standard on x86-64, ARM,
RISC-V, etc.

The Itanium type_info hierarchy specifies:
- `__class_type_info` for classes with no bases.
- `__si_class_type_info` for single-inheritance classes (one base).
- `__vmi_class_type_info` for multiple-/virtual-inheritance classes,
  with an array of `__base_class_type_info` records carrying offset
  + virtual-base-flag information.
- Cross-DSO `type_info` uniqueness via the name string: when comparing
  two `type_info*` from different shared libraries, fall back to
  `strcmp(a->name, b->name)` to identify "same type".

**Sea-front opts out of all of this.** The reasons:

- **Single-binary scope.** Sea-front emits both producer and consumer
  of `type_info` references; no third-party shared library is going
  to hand sea-front-emitted code a stock `std::type_info&`. Pointer
  comparison is sufficient.
- **Layout simplification.** The Itanium hierarchy with
  `__si_class_type_info` and `__vmi_class_type_info` exists to save
  space on classes with simple inheritance. Sea-front's flat layout
  (one record type with optional `parents[]` array) is slightly
  larger but trivially uniform — one struct, one helper, no runtime
  type-of-typeinfo dispatch.
- **No mangled-name interop requirement.** The Itanium name string
  format (`_ZTI...` plus the type) is required for cross-DSO
  identity comparison. Sea-front's name field can be the
  human-readable form for diagnostics; identity is by pointer.

The cost of opting out: sea-front-built `.so` files cannot share
exception types or `dynamic_cast` targets with stock-compiled `.so`
files. For sea-front's transpilation use case, this is the same
constraint exceptions impose anyway.

## The cliffs

Three places where "trivial RTTI" gets non-trivial:

### Multiple and virtual inheritance

Single-inheritance `dynamic_cast` is a parent-chain walk. Multiple
inheritance requires:
- Following each base in `parents[]` and applying the recorded
  offset when descending (since base subobjects sit at non-zero
  offsets within the derived layout).
- Detecting **virtual base** sharing — a class derived twice from
  the same virtual base must have one shared subobject, not two.
  `dynamic_cast` to the virtual base must produce that one address.
- **Sidecast** — `dynamic_cast<Sibling*>(p)` where `Sibling` is not
  on `p`'s ancestor chain but is reachable through a common derived
  type. The classic case is a diamond inheritance with a `dynamic_cast`
  from one branch to the other; ISO requires it works if the
  dynamic type is the diamond's most-derived type.

Implementing single-inheritance dynamic_cast: ~20 lines.
Implementing full ISO with multi/virtual/sidecast: ~150 lines plus
a more elaborate `type_info` record carrying offset + virtual-base
flags. Still tractable — gcc's libsupc++ does it in maybe 400 lines
of C++ — but a real engineering effort.

For sea-front's likely workload (modern C++ apps that use single or
shallow inheritance hierarchies), single-inheritance support
covers most usages. Multi-inheritance can ship as a phase 2 once
something demands it.

### Cross-DSO `type_info` uniqueness

Code that does:
```cpp
extern std::type_info const &get_type_from_other_lib();
bool same = (typeid(MyType) == get_type_from_other_lib());
```

assumes `==` works across shared libraries even when each library
has its own `type_info` record for `MyType`. The Itanium ABI
makes this work via name-string fallback. Sea-front's pointer-only
identity does not.

In practice, this matters mostly for plugin architectures and
serialization libraries that compare types across DSO boundaries.
For monolithic applications it's a non-issue. **Sea-front declines
this feature**; users who need it should compile with stock GCC/Clang.

### Ref-form dynamic_cast couples to exceptions

Already covered above and in [`exceptions.md`](exceptions.md). The
ref form is the *only* RTTI feature that hard-requires exceptions;
the pointer form is fully usable in `-fno-exceptions` builds.

GCC and Clang both support `-fno-exceptions -frtti` as a valid
build mode where pointer-form `dynamic_cast` works fine and ref-form
calls `std::terminate` on mismatch. Sea-front matches this behavior:
the ref-form lowers to "abort on fail" when the exception runtime
isn't linked in, "throw bad_cast" when it is.

## What sea-front explicitly does not implement

- **Itanium-ABI-compatible `type_info`.** Layout is private, names
  may differ from `_ZTI...`, no cross-DSO uniqueness.
- **`std::type_index`.** Trivially implementable on top of
  `type_info`; library-level concern, not codegen.
- **C++26 `std::reflexpr` / static reflection.** Different feature
  entirely; orthogonal.
- **`std::any` runtime type erasure** is library-level, doesn't
  depend on RTTI specifically (uses type-erasure idioms internally),
  works regardless.
- **Runtime introspection of class members.** ISO C++ doesn't expose
  this; nor does sea-front.
- **`__cxxabiv1::__dynamic_cast` interop.** Stock libstdc++ exposes
  `abi::__dynamic_cast` as a hook for non-conforming usage. Sea-front
  doesn't.

## Phased implementation roadmap

1. **Per-class type_info emission.** One `__sf_typeinfo_<mangled>`
   symbol per polymorphic class, with name + single-parent link.
   Vtable layout extended with one slot for `typeinfo` pointer.
   `typeid(T)` for static T lowers to `&__sf_typeinfo_<T>`. Maybe
   100 lines of codegen.

2. **Polymorphic `typeid(*p)`.** Vtable lookup at the call site.
   Trivial once vtable slot is reserved.

3. **Pointer-form `dynamic_cast<T*>` for single inheritance.** The
   parent-chain-walk runtime helper plus codegen at each
   `dynamic_cast` site. Independently useful — works without
   exceptions, covers most real-world casts. ~50 lines runtime, ~20
   lines codegen.

4. **`type_info` API surface.** `.name()`, `==`, `.before()`,
   `.hash_code()`. Trivial library-level implementation.

5. **Ref-form `dynamic_cast<T&>`.** Same machinery as pointer form,
   plus `__sf_throw_bad_cast`. Decoupled from full exceptions: until
   exceptions are wired up, ref-form aborts on fail. After exceptions,
   it throws `std::bad_cast` per ISO.

6. **Multiple inheritance.** `parents[]` array on `type_info`,
   offset adjustments in the helper, virtual-base sharing logic.
   The "real engineering effort" tier — order of 150 runtime lines.

7. **Sidecast (full ISO conformance).** Diamond-inheritance edge
   cases. Probably never needed; ship if a use case forces it.

Steps 1-5 cover what most real code uses and ship in phase order.
6+7 are scope expansion when demanded.

## Connections to other docs

- [`exceptions.md`](exceptions.md) — catch-by-base needs the
  `type_info` parent chain walk. Sharing the runtime helper across
  catch matching and `dynamic_cast` is natural; both are "is this
  source type reachable from this destination type via inheritance?"
  questions.
- [`mangling.md`](mangling.md) — `__sf_typeinfo_<mangled>` reuses
  the existing class-tag mangling. No new naming scheme.
