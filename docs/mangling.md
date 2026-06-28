# Name Mangling Design

Status: **both schemes ship.** Itanium is the **default** (`g_mangle_kind
= MANGLE_ITANIUM` in `src/codegen/mangle.c`); the human-readable scheme
is opt-in via `--mangling=human` and still works as a review/audit aid.
The Itanium implementation covers nested-name encoding, NTTP literal
values, type-arg templates, substitution back-refs (`S_`, `S0_`, ...),
ctor C1/C2 / dtor D0/D1/D2 variants, ref- and cv-qualifiers, and a
chunk of gcc-parity fixtures (see `tests/test_mangle_itanium`).
Cross-ref: [`rtti.md`](rtti.md) for typeinfo symbol naming,
[`inline_and_dedup.md`](inline_and_dedup.md) for how mangled names
plug into the static-inline dedup story, [`exceptions.md`](exceptions.md)
for the throw-class typeinfo placeholder. The "System-class interop"
section near the end captures the open question of how to bridge
sea-front-mangled symbols to libstdc++.

## Goal

Generate a name for every C++ entity sea-front lowers to a free C
function or symbol such that:

1. **Two distinct entities never collide.** Different overloads,
   different template instantiations, different members of different
   classes, etc., all produce different names.
2. **The "same" entity always produces the same name across multiple
   TUs**, so multi-TU compilation paired with `__SF_INLINE`
   (`static inline`, see [`inline_and_dedup.md`](inline_and_dedup.md))
   collapses inline functions and template instantiations on a single
   producer per TU. Determinism on the same inputs is required.
3. **Generated `.c` files stay reviewable by humans.** Sea-front's
   reason for existing is "trusted bootstrap — read the lowered C
   yourself and verify it's correct." That makes review accessibility
   **load-bearing**, which is why the human scheme survived as an
   opt-in (`--mangling=human`) even after Itanium became the default —
   pipe a small reproducer through it when reading mangled output is
   the friction.
4. **Sea-front-emitted symbols never collide with C library or
   `extern "C"` symbols.** Every synthesized name lives behind a
   prefix that puts it in its own namespace (`_Z…` for Itanium,
   `sf__…` for human).

## Why Itanium ended up the default

Despite the early "human-by-default" instinct (preserved in earlier
drafts of this doc), Itanium ended up shipping as the default for
three reasons:

1. **Multi-TU consistency with stock toolchains.** Even though
   sea-front emits its own producer for every used entity, mixed
   builds (where some objects are sea-front-built and others are
   stock-built — see the bootstrap chain in
   [`trusted-bootstrap-design.md`](trusted-bootstrap-design.md))
   need a common naming contract at the link stage. Itanium is the
   obvious one.
2. **Tooling, debug, link diagnostics.** `c++filt`, `addr2line`, gdb,
   `nm -C` all demangle Itanium. Undefined-symbol reports from `ld`
   become readable directly.
3. **NTTP / template-arg encoding lands cleanly.** Once the framework
   was in for class templates and member templates, the cost of also
   doing Itanium was less than feared — the algorithm is bounded and
   well-specified.

The human scheme is retained because the audit story still wins under
it: a developer eyeballing the lowered C of a single reproducer can
read off "this is `get` on `vec` with template arg `int`, called with
one `double`" from `sf__vec_t_int_te___get_p_double` without piping
through anything. Itanium would produce `_ZN3vecIiE3getEd` — same
information, more friction.

## Plugin shape (as it actually exists today)

Mangling is fundamentally a **traversal** of the AST/type tree where
every visited node emits something. Itanium and the human-readable
scheme **agree on the traversal** (you visit a class type, then its
template args if any, then a member name, etc.) but **disagree only on
leaf tokens** (`i` vs `int`, `_Z` vs `sf__`, `N…E` vs `__`).

The actual implementation is plainer than the original "vtable struct"
sketch this doc described before Itanium shipped — every public mangle
entry (`mangle_class_ctor`, `mangle_class_method`,
`mangle_free_function_symbol`, …) dispatches on `g_mangle_kind` at
the top and routes to an `itan_…` or human implementation. Both
implementations are around ~700-900 lines each. The vtable struct
described below would still be a possible refactor; the dispatch shape
isn't worth changing absent a concrete need.

## The plugin shape (sketched / reference design)

Mangling is fundamentally a **traversal** of the AST/type tree where
every visited node emits something. Itanium and our human-readable
scheme **agree on the traversal** (you visit a class type, then its
template args if any, then a member name, etc.) but **disagree only on
leaf tokens** (`i` vs `int`, `_Z` vs `sf__`, `N…E` vs `__`).

So the codegen layer should call into a **vtable** of leaf hooks
inside a recursive framework that sea-front owns. Implementations of
the vtable are pluggable. Two ship today: the human one (default) and
optionally an Itanium one (for tooling interop).

### API sketch

```c
typedef struct Mangler Mangler;

struct Mangler {
    /* Output buffer the framework owns. */
    char  *buf;
    int    buf_len;
    int    buf_cap;

    /* Leaf vtable — implementations fill these in. */

    /* Scope structure */
    void (*open_namespace)(Mangler *m, Token *name);
    void (*close_namespace)(Mangler *m);
    void (*open_class)(Mangler *m, Token *name);
    void (*close_class)(Mangler *m);

    /* Member entry-points */
    void (*append_member)(Mangler *m, Token *name);   /* methods, fields */
    void (*append_ctor)(Mangler *m);
    void (*append_dtor)(Mangler *m);
    void (*append_dtor_body)(Mangler *m);              /* sea-front only */

    /* Template arg list */
    void (*open_template_args)(Mangler *m);
    void (*close_template_args)(Mangler *m);

    /* Function param list (used for overload disambiguation) */
    void (*open_param_list)(Mangler *m);
    void (*close_param_list)(Mangler *m);
    void (*param_separator)(Mangler *m);

    /* Type leaves — recursive framework dispatches into these. */
    void (*type_builtin)(Mangler *m, TypeKind k, bool is_unsigned);
    void (*qual_const)(Mangler *m);
    void (*qual_volatile)(Mangler *m);
    void (*ptr_marker)(Mangler *m);
    void (*ref_marker)(Mangler *m);
    void (*rref_marker)(Mangler *m);
};

/* Recursive framework — calls into the vtable. Defined once, used
 * by every mangler implementation. */
void mangle_type(Mangler *m, Type *t);
void mangle_qualified_name(Mangler *m, Type *class_type, Token *member);
void mangle_function(Mangler *m, Node *func);

/* Vtable instances — pluggable. */
extern Mangler *mangler_human(void);    /* sf__ readable */
extern Mangler *mangler_itanium(void);  /* _Z encoded */

/* Codegen accesses the active mangler via a global. Set once at
 * codegen entry, used throughout. */
extern Mangler *g_mangler;
```

The recursive framework example for `const vec<int>*`:

```c
void mangle_type(Mangler *m, Type *t) {
    if (t->is_const) m->qual_const(m);
    if (t->is_volatile) m->qual_volatile(m);
    switch (t->kind) {
    case TY_PTR:  m->ptr_marker(m); mangle_type(m, t->base); return;
    case TY_REF:  m->ref_marker(m); mangle_type(m, t->base); return;
    case TY_STRUCT:
        /* Walk class_region's enclosing chain (namespaces), open
         * class, append name, handle template args if any. */
        ...
        return;
    default:
        m->type_builtin(m, t->kind, t->is_unsigned);
        return;
    }
}
```

The walker is shared. Each vtable is around 50 lines.

### Concrete vtable contrast

For `vec<int>::get(double)` the same walker calls:
`open_class(vec)`, `open_template_args()`, `mangle_type(int)`,
`close_template_args()`, `append_member(get)`, `open_param_list()`,
`mangle_type(double)`, `close_param_list()`, `close_class()`.

| Hook | Itanium emits | Human emits |
|---|---|---|
| (start) | `_Z` | `sf__` |
| `open_class("vec")` | `N3vec` | `vec` |
| `open_template_args()` | `I` | `_t_` |
| `type_builtin(INT)` | `i` | `int` |
| `close_template_args()` | `E` | `_te_` |
| `append_member("get")` | `3get` | `__get` |
| `open_param_list()` | (nothing) | `_p_` |
| `type_builtin(DOUBLE)` | `d` | `double` |
| `close_param_list()` | (nothing) | (nothing) |
| `close_class()` | `E` | (nothing) |

Resulting names:
- Itanium: `_ZN3vecIiE3getEd`
- Human:   `sf__vec_t_int_te___get_p_double`

Both ugly to different degrees. The human one is at least *grep-able*
and you can read off "this is `get` on `vec` with template arg `int`,
called with one `double` parameter."

## Why having both schemes is genuinely good

Beyond "you can swap":

1. **Testability** — the encoding tests (`tests/test_mangle_itanium`)
   exercise the framework and the leaf tokens separately. Changes to
   leaf encoding don't break framework tests, and vice versa.
2. **The implementations *are* the spec.** Each scheme is executable
   documentation of one mangling shape; the Itanium impl is the
   reference for what sea-front emits when interop matters.
3. **Bikeshed isolation.** Anyone unhappy with the human leaf tokens
   (`_t_`, `_p_`, …) argues about *those* under `--mangling=human`
   without touching the Itanium default.

## extern "C" handling

The `sf__` prefix protects most generated symbols from collisions with
C library functions, but **only partially**:

- **Safe with prefix**: class methods, ctors, dtors, synthesized
  wrappers (`Class_dtor_body`, `Class_dtor`, `Class_ctor`). All
  prefixed, all in their own namespace, zero collision risk.
- **Not safe by default**: free functions at namespace scope. Today
  they're emitted with their bare name. `void puts()` in C++ at
  namespace scope would collide with libc `puts`. C++ wouldn't allow
  this in the first place (`<stdio.h>` exposes `puts` with C linkage),
  so it's mostly a non-issue but relies on the user's compiler also
  refusing the bad code.
- **`extern "C"` functions** are unmangled by definition. If a user
  writes both an `extern "C" void foo()` and a `namespace ns { void
  foo(); }`, both lower to `foo` in C and collide. Real C++ catches
  this at link time via mangling; we won't.

**The full fix**: mangle *every* free function through the framework
(also gaining a prefix), *except* those declared `extern "C"`.
`extern "C"` functions keep their bare name and live in the global C
namespace (which is what they explicitly opted into). Then every
sea-front-emitted symbol is in its own namespace except for the one
C++ feature that explicitly opts out.

This requires the parser to tag `extern "C"` blocks so the contained
decls inherit the C-linkage flag. Today the parser eats `extern "C" {
... }` blocks but doesn't propagate the linkage to inner decls. Small
parser change.

## Known gaps in the Itanium implementation

The default Itanium scheme covers nearly everything sea-front emits
today, but two gaps are worth noting (and are deferred per
`project_template_ctor_pipeline.md`):

1. **Template ctor mangle omits the `I…E` template-args block.** When
   the template-ctor pipeline (see
   [`template-instantiation.md`](template-instantiation.md)) clones
   `A<T>(T&)` for `T=int`, sea-front emits `_ZN1AC1Ei` rather than
   the gcc-canonical `_ZN1AC1IiEEi`. Both sea-front sides agree on
   the shape so internal links resolve; mixing with gcc-built objects
   that expect the canonical form would not link.
2. **Variadic-template-ctor leak.** Re-enabling the parser fix that
   recognises template ctor names propagates `is_constructor=true`
   onto the inner FD and surfaces an instantiation-mangling bug —
   variadic packs leak `TY_DEPENDENT` into the symbol
   (`_ZN1SC1Eu9_DEPENDENT`). The parser fix stays gated until the
   variadic mangle gap is closed; see
   `project_template_ctor_pipeline.md` and
   `project_implicit2_resolved.md` for the workaround discovery story.

Neither blocks internal builds. Both are tracked.

## Rules of the human encoding (strawman, all bikeshed-able)

These are the leaf-token choices the `mangler_human` vtable would
make. They're not load-bearing — anyone implementing the vtable can
pick differently.

- Every sea-front-generated symbol begins with `sf__` to mark
  provenance and avoid collisions with user/library symbols.
- Namespace separator: `__` (double underscore). Joins enclosing
  namespaces and the class name.
- Template arg list: open `_t_`, close `_te_`. Args separated by `_`.
- Param list (overload disambiguation): open `_p_`, args separated
  by `_`.
- Type tokens for builtins: `int`, `long`, `uint`, `ulong`, `float`,
  `double`, `char`, `uchar`, `bool`, `void`. (Borrowed straight from
  the source language.)
- Type qualifiers: `K_` for const (Itanium-borrowed because it's
  short and we use it once per arg), `V_` for volatile.
- Type structure: `P_` for pointer, `R_` for reference, `RR_` for
  rvalue reference.
- Class types as args: emit the same prefixed form as the type's own
  mangled name (recursive).
- Ctor / dtor / body: `__ctor`, `__dtor`, `__dtor_body`.
- Operators: `op_<name>` — e.g. `op_assign` for `operator=`,
  `op_plus_eq` for `operator+=`, `op_lt` for `operator<`,
  `op_call` for `operator()`, `op_index` for `operator[]`.

## Rationale summary

The plugin shape lets us pick our primary scheme on **readability /
audit-friendliness** grounds while leaving the door open to a
secondary scheme on **tooling-compat** grounds. Neither has to be
chosen at the expense of the other.

The encoding rules are *negotiable*. The plugin shape is the
load-bearing decision.

## System-class interop (deferred design)

When sea-front compiles source that includes libstdc++ / libc++
headers — gcc 14 libcpp/, libstdc++'s `<cstdlib>` chain, etc. — it
sees polymorphic system classes (`std::exception`, `std::bad_alloc`,
`std::runtime_error`, …). Their virtual method bodies live in the
system shared library, **not** in the TU we're compiling.

This raises a question the mangling-plugin shape was always meant to
defer: *do we mangle calls into those classes' virtuals using our own
scheme (which the linker can't resolve against `libstdc++.so`) or
using Itanium (which it can)?*

### Why not "obvious answer = Itanium"

The Itanium C++ ABI is the de-facto cross-compiler ABI on every
platform sea-front targets, and our mangler vtable is exactly the
hook for plugging in an Itanium scheme. The hooks are in place; the
algorithm is real work but bounded — substitutions (`S_`, `S0_`...),
std-namespace abbreviations (`Sa`, `Ss`, `Sd`, `Si`, `So`),
nested-name encoding (`N...E`), special-function tags (`C1`/`C2`/`C3`
for ctors, `D0`/`D1`/`D2` for dtors), CV/ref-qualifiers, virtual
thunks. Several hundred lines of careful code; doable.

The **real** cost is the inconsistency. Itanium-compat for vtables
alone is a one-way bridge:

- We can call into libstdc++ — good.
- We **cannot catch what libstdc++ throws** (sea-front EH stays
  TLS-polling private per `docs/exceptions.md`; libstdc++ throws
  Itanium-shaped objects via `_Unwind_RaiseException`).
- We **cannot query libstdc++'s `type_info`** (different layout).
- We **cannot let libstdc++ catch what we throw**.

Picking up Itanium for vtables only opens the slippery slope: "OK,
type_info next? exception_ptr next? full unwind ABI?" Each step is
small but the cumulative direction conflicts with the deliberate
"sea-front exceptions / type_info / unwind protocol are private"
stance.

### Why not trampoline stubs

The other mechanically-feasible path: per-system-class C++ shims
that bridge sea-front mangling to Itanium symbols. e.g. ship an
`__sf_libstdcxx_shim.cc` that hand-writes:

```cpp
extern "C" const char *
sf__std__exception__what_p_void_pe_(const std::exception *self)
{ return self->what(); }
```

This works at link time without any new mangler. But it scales
poorly — every system class user code touches needs a hand-written
shim, and "user code that includes libstdc++" is most C++ code.
Doesn't generalise.

### The right long-term answer

Compile our own libstdc++ (or libc++) **through sea-front itself**,
producing a sea-front-mangled stdlib that user code links against.
gcc 14's `libstdc++-v3/` and LLVM's `libcxx/` are both pure-C++
source distributions designed to be re-built — they're conceptually
within sea-front's reach. The output is a `libsf_stdc++.so` whose
symbols use `sf__std__...` names, and a sea-front-compiled binary
links against that.

This aligns with the **bootstrappable** instinct: sea-front sits in
a chain of "build everything from source," and its standard library
should be no exception. It's a major slice — not a today thing — but
it's the answer that doesn't require taking on Itanium ABI
compatibility at all.

### Current pragmatic stance: skip-the-vtable

Until we either pick "build our own stdlib" or change our minds on
Itanium, the working compromise is: **emit no vtable for polymorphic
classes whose virtual methods are all extern-only** (no in-TU body
for any virtual). Loses dispatch on system types, which is fine
because the source we compile rarely dispatches through them — they
typically just appear in catch clauses or as opaque return types.

The alternative — emit vtables with extern declarations for the
missing methods — would compile (cc-only pipelines pass) but
link-fail. Skip-the-vtable is the cleaner move: nothing references
a symbol that doesn't exist anywhere.

### When to revisit

Two triggers should re-open this discussion:

1. **A real link-against-libstdc++ build** is wanted (someone wants
   to run a sea-front-compiled binary). Then we either pick
   trampoline shims for whatever subset is needed, or commit to the
   Itanium plugin, or to the "compile our own stdlib" track.
2. **User source actually dispatches through a system class** (e.g.
   calls `e.what()` on a caught `std::exception &`). Today's
   skip-the-vtable means that call wouldn't compile; we'd need at
   least the Itanium plugin to make it work.

When either lands, this section gets the design pivot it deserves —
not a rushed half-step.
