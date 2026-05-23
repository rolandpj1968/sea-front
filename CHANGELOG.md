# Changelog

## 0.2.2 — C++03 grind continued (2026-05-23)

More targeted slices through g++.dg. Each one clears one
test and exposes a real underlying bug — not just an isolated
fix.

### Slices

- **Value-init memset for `new T()`** — when T has no user-
  provided default ctor (synth only), zero the storage before
  the synth ctor runs. N4659 §11.6/8 [dcl.init]: value-init of
  such a class first zero-initialises, then default-initialises.
  Threads a `new_value_init` flag through parse_new. Pattern:
  g++.dg/init/value3.C.
- **`__real__` / `__imag__` as scalar narrowers** — both
  keywords had been aliased to TK_PLUS at lex time, so
  `__imag__ z` lowered to `+z` and stayed complex. Introduce
  proper TK_KW_IMAG / TK_KW_REAL token kinds and pass through
  to C unchanged (gcc + clang both accept the keywords in C).
  Pattern: g++.dg/other/complex1.C.
- **Deferred-assign hoist on plain reads** — the deferred-
  assign mechanism (hoist-without-init inside short-circuit
  branches) used to materialise only at `&this` binding. Plain
  reads via `.member` access read uninitialised memory.
  Materialise `(name = call, name)` at every use site that
  emits the captured temp. Pattern: g++.dg/opt/expect1.C.
- **delete-pointee Type refresh + null-guard** — `~T() { delete
  prev; }` resolved `prev: T*` to a stale T copy from before
  T's has_dtor was stamped, dropping the recursive dtor call.
  Always re-resolve the pointee through the canonical class
  def. Plus null-guard the dtor invocation since `delete null`
  is a no-op (N4659 §8.3.5/2 [expr.delete]). Pattern:
  g++.dg/opt/pr42508.C.
- **Per-element ctor loop for array new** — `new T[N]` and
  `new (p) T[N]()` now run T's default ctor on EVERY element,
  not just element 0. The element count N is captured into
  `__sf_new_n` so its side effects fire exactly once across
  the malloc-call's size arg AND the loop bound. Tag the
  malloc-call's size-arg-lhs directly to handle template-
  clone divergence (g++.dg/template/new1.C's
  `new T[Blksize()]` case). Pattern: g++.dg/expr/anew4.C.
- **sema/common_arith_type preserves signedness** — `int *
  unsigned long` had been losing its is_unsigned flag in the
  fresh result Type, causing the Itanium placement-new
  aliasing path (which keys on size_t-ness) to skip
  `_Znam` / `_ZnamPv` and emit unresolved tag-suffix names.
  N4659 §7.6 [conv.prom] preserves the winner's signedness.

### Result

dg: 391 PASS / 6 FAIL (from 385/6 at v0.2.1). Same 6 out-of-
scope failures.

## 0.2.1 — C++03 grind start (2026-05-23)

Continuing from C++98 compliance into deeper g++.dg territory.
Each slice clears one or two specific tests + sets up
infrastructure for future grinding.

### Slices

- **using-decl method mangling** — `using Base::foo;` inside
  Derived now resolves at the call site: mangle as `Base::foo`,
  pass `&d.__sf_base` as `this`. Declaration grew
  `using_decl_source_class` to record the source class at the
  using-decl parse site. Pattern: g++.dg/inherit/using2.C.
- **Aggregate init copy elision** — `B b = { T() }` constructs
  T directly into the aggregate slot (calls T's default ctor on
  the member, not a copy ctor). Required for tests where T's
  ctor body records `this` for later identity checks. The
  emit_var_decl_inner ` = ` path also gates on the
  aggregate-per-member detector so the bitwise + per-member
  emit pair doesn't double-fire. Pattern: g++.dg/init/aggr2.C.
- **std::nothrow asm rename** — `extern const std::nothrow_t
  nothrow;` gets an `__asm__("_ZSt7nothrow")` rename at the
  canonical declaration so the emitted C symbol matches
  libstdc++'s exported form. Pattern: g++.dg/init/new5.C.
- **Singleton operator new/delete mangling** — a SINGLE user-
  defined `::operator new` / `::operator delete` (no visible
  overload set) now still routes through the mangler so the
  Itanium aliasing fires and the def overrides libstdc++'s
  default at link. Pattern: g++.dg/init/new41.C.

### Result

dg: 385 PASS / 6 FAIL (from 381/6 at v0.2.0). Same out-of-scope
6 (2 signal/SIMD, 3 C++11, 1 flag-only) as before.

## 0.2.0 — C++98 dg compliance (2026-05-22)

All C++98-relevant entries in gcc's g++.dg `dg-do run` corpus pass.
The remaining 6 FAILs are explicitly out-of-scope:

- `eh/sighandle.C`, `eh/simd-4.C` — signal handlers / SIMD intrinsics;
  by-design out of scope for a portable bootstrap C transpiler.
- `cpp0x/implicit2.C`, `cpp0x/initlist49.C`, `tls/thread_local6g.C` —
  C++11 features (the cpp0x/ folder is gcc's pre-standardisation working
  draft name for C++11). Future work.
- `init/copy3.C` — flag-only test (`-fno-elide-constructors`);
  sea-front already elides where the standard permits.

### Highlights

**C++98 grind, this release:**

- **Multi-inheritance virtual dispatch + thunks** — implicit-virtual
  overrides (a derived method matching a base virtual's name+arity is
  virtual without the keyword) propagate at class finalization. Secondary
  vtables for non-first bases emit this-adjusting thunks; covariant
  returns get the additional return-value adjustment so a `B*`-typed
  slot really receives the B subobject's address.
- **Two-phase name lookup** — non-dependent names inside a template
  body bind at template-definition-point (phase 1). A namespace-scope
  phase-1 binding is not rebound at phase-2 by a class member that
  becomes visible only because a previously-dependent base became
  concrete. Class-member phase-1 bindings (incl. `using B<T>::foo;`
  injected via using-declarations) DO get re-resolved at phase-2 to
  the instantiated class's same member.
- **typename T::member in __PRETTY_FUNCTION__** — qualified
  dependent types render as `typename T::member` in the signature
  AND emit `; typename T::member = <resolved>` in the `[with ...]`
  substitution suffix.
- **User copy ctor invocation** — `T u = v;` / `T u(v);` calls the
  user-declared copy ctor when one exists; transitive cases (the
  immediate class has no user copy ctor but a base or member does)
  expand to an inline subobject-by-subobject copy chain. Flat-block
  comma-separated declarators (`T u, v(u);`) now push cleanups so
  the dtor fires at scope exit.
- **Defaulted / aggregate-init shapes** — `T::~T() = default;`
  out-of-class produces a linkable body symbol. Aggregate
  initialisation `T x = {e1, e2, ...};` with class members that have
  user copy ctors calls per-member copy ctors with partial-destruction
  unwind on throw.
- **Qualified template typedef** — `S<T>::type` (and any STL-shape
  `Container::iterator` / `Container::value_type`) resolves at parse
  by substituting the template-id's args into the typedef target's
  dependent type.

**Earlier in the session (still in this release):**

- **Operator new dispatch slice** — `new T[N]` lowers to `_Znam`,
  `new T` to `_Znwm`; user-defined `::operator new` / `::operator
  delete` mangled to the matching Itanium symbol overrides the
  libstdc++ default at link time. On-throw chain calls the matching
  `_ZdlPv` / `_ZdaPv` with the original pointer. Placement-new
  threads the placement args through. Prelude provides static-inline
  fallbacks over libc malloc/free for cc-without-`-lstdc++` linkage.
- **Array-mem-init partial-destruction unwind** — when an element
  ctor throws mid-array-mem-init, elements `[0..k-1]` get destroyed
  in reverse before the throw propagates. N4659 §15.2/2 [except.ctor].
- **Value-init zero-fill** — `: m()` mem-init for non-class scalar /
  array, polymorphic class arrays (zero-init then default-construct),
  `new T()` / `new T[N]()` for POD types.
- **Noexcept entry-state snapshot** — a throw()-spec function called
  during in-flight unwind doesn't trip its no-throw guard merely by
  inheriting the caller's exception state.

### Test counts

- 144 lexer unit tests
- 43 parser integration tests
- 451 emit-c end-to-end tests
- 4 multi-TU dedup tests
- 28 gated + 52 stretch libstdc++ header smoke tests
- 2 gcc-standalone tests
- **g++.dg dg-do run: 382 PASS / 6 FAIL** (all 6 out-of-scope by design
  / post-C++98 — see above)
- libcpp.a rebuild via `scripts/sea-front-cc` succeeds end-to-end

### Demos

`demo/*.c` regenerated against the current sea-front output. Each
demo's C output matches g++'s exit-code behavior on the same source.

## 0.1.0 — Bootstrap I — gcc 4.8 fixed point (2026-05-06)

First tagged release. Sea-front clears the canonical bootstrappable.org
rung: cc1plus-built-by-sea-front from gcc 4.8 source is a self-consistent
fixed point under the gcc bootstrap protocol.

### Highlights

- **stage2 == stage3 byte-equal.** Rebuilding cc1plus through itself a
  second time produces a bit-identical binary modulo BUILD-ID, DWARF
  debug info, and gcc's own embedded `executable_checksum` (a 16-byte
  MD5 over input .o files whose debug-info timestamps differ between
  stages — non-determinism in gcc 4.8 itself, not in sea-front).
  18,601,376 bytes of meaningful content match exactly.
- **Working stage-1 cc1plus binary.** Built end-to-end via sea-front
  from gcc 4.8's C++ source. Compiles and runs real C++ programs.
- **Era-correct glibc shim** (`scripts/glibc-shim/`) replaces the
  earlier compat-header hacks. Surgical overrides: glibc 2.17 cdefs.h
  with empty stubs for post-2.17 attribute decorators; bits/floatn.h
  + bits/floatn-common.h with `__HAVE_FLOAT*=0` so modern math.h
  skips _FloatN declarations.
- **`scripts/cc1plus-via-sf`** — drives a sea-front-built cc1plus as
  a g++ replacement, used for both stage-N rebuilds and g++.dg testing.

### Test counts

- 144 lexer unit tests
- 42 parser integration tests
- 301 emit-c end-to-end tests (compile → execute → verify)
- 4 multi-TU dedup tests
- 28 gated + 52 stretch libstdc++ header smoke tests

### Bootstrap chain

```
cc1plus-stage0  53.3 MB  (built by sea-front-cc — initial host bootstrap)
cc1plus-stage1  24.9 MB  (built by stage-0 via cc1plus-via-sf)
cc1plus-stage2  23.7 MB  (built by stage-1 — first fully self-built)
cc1plus-stage3  23.7 MB  (built by stage-2 — fixed point)
```

### Known gaps

- C++11+ features incrementally supported; current target is C++03 for
  the gcc 4.8 bootstrap.
- Lambdas, partial template specialization, SFINAE, variadic templates
  not yet implemented.
- One known cc1plus segfault on insn-recog.c::recog_63 affects stage-0
  only; stage-1 and beyond compile it correctly. Tracked separately.
