# C++ Exception Lowering to C

Status: **design only**, not yet implemented. Current behavior:
`try` / `catch` / `throw` parse but lower to `abort()` (trap-and-die).
This document describes the long-term implementation for full C++
exception support.

The bootstrap target (gcc 4.8) was built with `-fno-exceptions` and
needs none of this. Most of the complexity below exists to support
arbitrary C++ — modern libstdc++, user code that catches
`std::bad_alloc`, `std::out_of_range`, or domain-specific exception
hierarchies. Treating exceptions as a real feature, not a
placeholder.

## Goal

Lower N4659 §18 [except] semantics into portable C, with these
properties:

1. **No-throw path is fast**: code that doesn't actually throw pays
   at most a single predictable branch per call site, ideally none.
2. **No-throw path is small**: the per-call-site shadow code is the
   dominant cost concern, not the runtime cycles. Bloat directly
   drives icache pressure, which in turn drives the actual perf
   cost. Sections below treat bloat as a load-bearing constraint.
3. **Standard semantics**: stack unwinding runs destructors in
   reverse construction order; `catch` matches by type with
   inheritance + cv-qualifier rules per §15.3 [except.handle];
   `noexcept` violations call `std::terminate`.
4. **Composes with C**: sea-front-emitted code can call ordinary C
   functions and vice versa without ABI surgery. Calls that cross
   into C frames simply don't unwind through them — same constraint
   the Itanium ABI imposes via "personality routine missing → call
   `std::terminate`", just enforced more cheaply.
5. **Multi-thread correct**: each thread has independent exception
   state. TLS gives this for free.

## Implementation strategy: TLS-polling

Three real implementation strategies exist for C++ exceptions:

**1. setjmp/longjmp ("SJLJ").** Every cleanup scope pushes a frame
onto a thread-local chain; `throw` calls `longjmp` to walk it back.
Simple to implement, ~few hundred lines of runtime, works on any C
target. **Killed by always-on cost**: every function with cleanup
pays `setjmp` on every entry into a cleanup scope, even when no
exception is ever thrown. Modern C++ is written assuming `throw` is
rare and the no-throw path should be free. SJLJ violates this hard
enough that GCC moved away from it on most platforms; mingw kept it
for years and its programs ran measurably slower than DWARF-EH ones.

**2. DWARF table-driven ("Itanium ABI / zero-cost").** Compiler
emits unwind tables (`.eh_frame` + `.gcc_except_table` LSDA)
describing, per PC range, what cleanups and catches apply. `throw`
calls `_Unwind_RaiseException`, the personality routine consults
the tables, the unwinder walks the stack frame by frame using
register save info from CFI. **Truly free in the no-throw path** —
all the metadata is non-loaded sections. **Wrong for sea-front**:
the tables are emitted by the compiler that knows about the
machine code it's producing, with CFI tied to register allocation
and prologue layout. Sea-front emits portable C; the C compiler
generates the actual code, and the C compiler has no way to know
"this call site needs a cleanup landing pad here" from sea-front's
perspective. Generating LSDA from C source is fundamentally not a
thing the C compiler will do for you. Implementing DWARF EH would
require sea-front to be a real backend — losing the entire
"transpile to portable C" value proposition.

**3. TLS polling.** A thread-local "exception state" variable;
`throw` sets it; every potentially-throwing call site checks it
after return; scope-exit destructor chains naturally propagate.
**Right for sea-front** because (a) it's portable C — no compiler
magic required, (b) it composes with sea-front's existing
destructor goto-chain (see below), (c) the no-throw path cost is
one branch per call site, predictable-not-taken, plus the
destructor chain runs that's already running for `return` anyway.

Polling is the chosen strategy. The remainder of this document
specifies it.

## Architectural fit: the destructor goto-chain

Sea-front already lowers C++ scope exit using a thread-local-ish
unwind state machine and a goto-chain through scope-level cleanup
labels:

```c
#define __SF_RETURN_VOID(lbl) do { __SF_unwind = __SF_UNWIND_RETURN; goto lbl; } while (0)
#define __SF_CHAIN_RETURN(lbl) do { if (__SF_unwind == __SF_UNWIND_RETURN) goto lbl; } while (0)
```

A `return` statement inside a nested scope sets
`__SF_unwind = __SF_UNWIND_RETURN` and gotos the innermost scope's
cleanup label. That label runs the scope's destructors in reverse
construction order, then chains to the outer scope's label, and so
on until reaching `__SF_epilogue` where the actual `return` happens.

Exceptions reuse this machinery essentially unchanged. The only
additions:

1. **A new state value `__SF_UNWIND_THROW`** alongside `RETURN`,
   `BREAK`, `CONT`.
2. **A check macro `__SF_CHAIN_THROW(lbl)`** identical to
   `__SF_CHAIN_RETURN` but matching `THROW`.
3. **A try-block landing pad** that intercepts `__SF_UNWIND_THROW`
   in the chain and either runs a matching catch or rethrows.

The destructor lowering is the architectural hard part of
exceptions. Sea-front already shipped it.

## The core lowering

### Exception state

A small TLS struct, accessed once per check:

```c
struct __sf_exception_state {
    __sf_unwind_t  state;      /* NONE / RETURN / BREAK / CONT / THROW */
    void          *exc_obj;    /* heap-allocated thrown object */
    const struct __sf_type_info *exc_type;
    void         (*exc_dtor)(void *);  /* destructor for exc_obj */
};
__thread struct __sf_exception_state __sf_exc_state;
```

`__SF_unwind` (existing single-byte flag) is replaced or augmented
by `__sf_exc_state.state`. The rest is exception-specific.

### `throw expr`

```c
{
    typeof(expr) *__sf_thrown = __sf_alloc_exception(sizeof(typeof(expr)));
    __sf_construct_in_place(__sf_thrown, expr);  /* copy-construct */
    __sf_exc_state.exc_obj = __sf_thrown;
    __sf_exc_state.exc_type = &__sf_typeinfo_<mangled-T>;
    __sf_exc_state.exc_dtor = (void(*)(void*))&sf__T__dtor_p_void_pe_;
    __sf_exc_state.state = __SF_UNWIND_THROW;
    goto __sf_innermost_cleanup_label;
}
```

`__sf_alloc_exception` is a small runtime helper. To match Itanium
ABI semantics (unbounded exception size, exception-safe allocation
that can't itself throw), it allocates from the heap with a
fallback to a small thread-local pre-reserved buffer for
`bad_alloc` propagation.

### `try { body } catch (T name) { handler }`

Lowered as:

```c
{   /* try-block scope */
    body
    goto __sf_after_try_<N>;
__sf_catch_<N>:
    if (__sf_exc_state.state == __SF_UNWIND_THROW &&
        __sf_type_matches(__sf_exc_state.exc_type, &__sf_typeinfo_<T>)) {
        T name = *(T*)__sf_type_adjust(__sf_exc_state.exc_obj,
                                        __sf_exc_state.exc_type,
                                        &__sf_typeinfo_<T>);
        __sf_exception_caught();   /* clears state, frees exc_obj */
        handler
    }
    /* no match — re-propagate */
    if (__sf_exc_state.state == __SF_UNWIND_THROW)
        goto __sf_outer_cleanup_label;
__sf_after_try_<N>:
    ;
}
```

The cleanup chain for the try-block's scope chains to
`__sf_catch_<N>` instead of straight to the outer cleanup label
when in `THROW` state. After the catch matches and the handler
runs, the state is cleared and control falls through. After
mismatched catch, state remains set and we chain outward.

### `throw;` (re-throw inside catch)

```c
__sf_exception_uncaught();   /* re-marks state THROW without re-allocating */
goto __sf_outer_cleanup_label;
```

The exception object is preserved across the catch handler. Just
the "caught" flag flips back.

### Catch matching across inheritance

`catch (Base &)` against `throw Derived` requires base-pointer
adjustment. Same machinery as `dynamic_cast`: walk the type_info
parent chain. See [`rtti.md`](rtti.md) (TODO).

## RTTI dependency

`catch` matches by type, which requires comparing `type_info` and
walking inheritance for derived-to-base. This pulls in a subset of
RTTI machinery whether or not user code uses `typeid` / `dynamic_cast`
directly:

- Per-class `__sf_typeinfo_<mangled>` symbol with name + parent chain.
- `__sf_type_matches(src, dst)` runtime helper for catch lookup.
- `__sf_type_adjust(ptr, src, dst)` for base-pointer offsetting (only
  matters under multiple inheritance).

Sea-front's RTTI implementation can be its own minimal layout, not
Itanium-ABI compatible. There's no need to interop with libstdc++'s
`type_info` because exception objects flow through sea-front-emitted
code on both sides. (See `rtti.md` for the RTTI design once written.)

## Bloat as the driving concern

The naive lowering — emit `if (__sf_exc_state.state == __SF_UNWIND_THROW)
goto __sf_cleanup;` after every call — is correct but wastes
**bytes of executable code, not just cycles**. The branch itself is
predicted-not-taken at near-zero runtime cost, but the byte sequence
sits in the icache footprint of every function and pushes real code
out.

Per call site, the check is roughly:
```
mov   %fs:__sf_exc_state.state, %al    ; ~6-9 bytes
test  %al, %al                          ; ~2 bytes
jne   .L_cleanup                        ; ~2-6 bytes
```

10-15 bytes per call site. In typical C++ a function has 5-10
calls; dense code with method dispatch can have far more. Across a
large binary that's megabytes of pure-overhead instruction stream
that the icache has to fight. The L1 icache is 32KB on
contemporary x86, often less on ARM cores. Every wasted byte
displaces real instructions.

So **icache pressure dominates** the cost model, not branch cycles.
Cycle-counting analysis (which makes polling look "almost free")
misses the real expense.

This makes `noexcept` inference table-stakes for keeping binaries
sane, not a perf-polish afterthought.

## `noexcept` inference

A function is provably noexcept iff its body contains no `throw`
**and** every callee is provably noexcept. Standard call-graph
fixed-point analysis.

### Initial labeling

- **`extern "C"` → noexcept** (N4659 §15.5.2/3 + §18.4 — a function
  with C language linkage cannot have an exception specification
  and is treated as noexcept). This single rule wipes out 30-50%
  of checks immediately, because the leaf-call graph in real C++
  code is dominated by libc.
- **Destructors → noexcept** (N4659 §15.4/3, since C++11/14 unless
  explicitly `noexcept(false)`).
- **Functions marked `noexcept` / `noexcept(true)` → noexcept.**
- **Functions containing a `throw` expression → throws.**
- **Indirect calls** (function pointer, virtual dispatch) →
  conservatively **throws** unless devirtualization or vtable
  analysis proves otherwise.
- **Cross-TU externals without source** → conservatively **throws**.
  LTO-style whole-program input to sea-front would lift many of
  these. Today, per-TU operation forces conservative treatment.

### Iteration

Repeatedly: any "unknown" function whose entire callee set is
proven noexcept gets promoted to noexcept; any function with a
throwing callee becomes throws. Iterate until no changes. Standard
fixed-point, terminates in O(call-graph-depth) passes.

### Per-call-site application

Even with conservative function-level results, the elision is
**per-call-site**. For each `f();` in the body, look at the
specific callee `f`'s status. If `f` is proven noexcept, emit no
post-call check. If unknown or throwing, emit the check.

This means a single throwing callee inside a function doesn't
poison every call site in that function — only call sites to
provably-throwing or unknown callees pay the bloat cost.

### Empirical expectation

In `-fno-exceptions` code: zero throw points, fixed-point promotes
everything to noexcept, all checks elided. Same code size as if
exceptions weren't supported.

In modern C++ with exceptions: typically 1–5% of functions
genuinely throw (allocators, container bounds checks, locale,
regex, file I/O — most paths terminate in `operator new` or library
boundaries). Fixed-point should leave 80–95% of call sites
elision-eligible.

The remaining 5-20% of call sites is the inherent cost of
"transpile arbitrary modern C++" — there's no path back to zero
without DWARF table-driven unwind, and that's only "zero in the
.text section": Itanium ABI moves the bloat into `.eh_frame`,
which is just as large but lives in non-loaded memory. The total
bytes are similar; the difference is icache locality.

## What sea-front explicitly does not implement

- **Itanium ABI compatibility.** Sea-front's exception object,
  type_info layout, and unwind protocol are private. Code emitted
  by sea-front cannot interop at the exception level with code
  built by stock GCC/Clang. This is the deliberate cost of
  transpilation; the alternative (DWARF generation) requires
  sea-front to become a real backend.
- **Stack-trace symbolication.** No frame-walking helper, no
  `std::stacktrace` (C++23) support. Throwing produces a typed
  object; what called what is not preserved.
- **`std::exception_ptr` / `std::nested_exception` / `std::current_exception`.**
  These need exception-pointer copy-on-rethrow semantics, which
  sea-front's single-thread-state model doesn't naturally give.
  Could be added with a refcounted exception-object wrapper if
  needed.
- **Cross-DSO `type_info` uniqueness.** Itanium ABI guarantees
  `&typeid(X) == &typeid(X)` across shared libraries via name-string
  fallback; sea-front's per-binary type_info doesn't. Only matters
  for code that compares `type_info*` directly across DSO
  boundaries — rare in practice.
- **Function-try-block.** §15.3/14 — `try { } catch { }` wrapping a
  function body. Parses fine, lowering is the same as a normal try
  but with member-init lifetime to think about. Probably trivial,
  just hasn't been needed.

## Phased implementation roadmap

1. **Trap-and-abort (current).** Parser accepts try/catch/throw,
   sema validates, codegen emits `abort()` for `throw` and ignores
   the catch. Lets libstdc++ headers parse without committing to
   runtime semantics. Sufficient for any code that doesn't actually
   take an exception path.

2. **Polling lowering, no elision.** Wire up `__SF_UNWIND_THROW`,
   the throw expression, the try/catch landing pads, the runtime
   helpers. Bloat is bad (every call site checked), runtime is
   correct. Functions throw and are caught; modern C++ test cases
   pass.

3. **`extern "C"` elision.** First and biggest bloat win — a
   one-rule check at codegen-emit time. Probably 30-50% of checks
   gone immediately. Trivial to implement.

4. **Fixed-point `noexcept` inference.** The full call-graph
   analysis. 80-95% of remaining checks elided. This is where the
   binary size approaches what `-fno-exceptions` produces minus
   the unavoidable check at the small fraction of genuinely
   throwing call sites.

5. **RTTI / type_info dependency.** Implementing 1-3 above mostly
   doesn't need RTTI (catch-all `catch (...)` works without type
   matching, and catch-by-exact-type only needs pointer comparison).
   Catch-by-base or catch-with-conversion needs the `type_info`
   parent chain — covered in `rtti.md`.

6. **`std::exception_ptr` and friends.** Optional, only when user
   code demands it. Most code doesn't.

Steps 1+2 are correctness; 3+4 are bloat reduction; 5+6 are scope
expansion. Each phase ships independently and the binary keeps
working between phases.
