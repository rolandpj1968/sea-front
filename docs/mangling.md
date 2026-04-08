# Name Mangling Design

Status: **design only**, not yet implemented. To be tackled immediately
before whichever slice first requires it (multi-overload ctors,
template instantiation lowering, or operator overloading — whichever
comes first).

## Goal

Generate a name for every C++ entity sea-front lowers to a free C
function or symbol such that:

1. **Two distinct entities never collide.** Different overloads,
   different template instantiations, different members of different
   classes, etc., all produce different names.
2. **The "same" entity always produces the same name across multiple
   TUs**, so multi-TU compilation paired with `__SF_INLINE` (weak
   symbols) deduplicates inline functions and template instantiations
   correctly. Determinism on the same inputs is required.
3. **Generated `.c` files stay reviewable by humans.** Sea-front's
   reason for existing is "trusted bootstrap — read the lowered C
   yourself and verify it's correct." That makes name readability
   **load-bearing**, not cosmetic.
4. **Sea-front-emitted symbols never collide with C library or
   `extern "C"` symbols.** Every synthesized name lives behind a
   prefix that puts it in its own namespace.

## Why we don't follow Itanium directly

The "Itanium C++ ABI" is the de facto C++ ABI everywhere except MSVC.
GCC and Clang use it on Linux, macOS, BSD, x86/x86_64/ARM/ARM64/
RISC-V/PowerPC, etc. — its name is misleading historical baggage.
Following it gets us tooling for free (`c++filt`, `nm -C`, gdb).

The cost is that Itanium-mangled names are dense, e.g. `_ZN3vecIiE3getEv`.
Our generated `.c` files would be full of these. The reviewability
proposition gets dramatically weakened — auditing the bootstrap would
require piping every emitted file through `c++filt`.

For sea-front specifically, the audit case is the dominant audience.
Tooling-compat is a nice-to-have. So our **primary scheme is
human-readable**, with Itanium left as an opt-in alternative.

## The plugin shape

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

## Why the plugin shape is genuinely good

Beyond "you can swap implementations":

1. **Testability** — every mangling test exercises the framework, not
   the leaf tokens. Changes to leaf encoding don't break framework
   tests, and vice versa.
2. **Itanium becomes cheap-to-add-later**, not a redesign. Pick human
   for now; if anyone wants `c++filt` interop, they implement the
   Itanium vtable in one focused commit and add a `--mangling=itanium`
   flag.
3. **The vtable interface IS the spec.** Each implementation is
   essentially executable documentation of one scheme.
4. **Bikeshed deferral.** Ship with the human vtable; if anyone hates
   the chosen leaf tokens (`_t_`, `_p_`, etc.), they argue about
   *those* in isolation without touching codegen or the framework.

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

## Implementation plan

When we get to it (immediately before the first slice that needs
overload disambiguation, template instantiation, or operator
overloading), the rollout is three commits:

1. **First commit — framework + human vtable, refactor existing
   callers.** Introduce the `Mangler` struct, implement just the
   human vtable, switch every existing call site
   (`emit_method_signature`, `emit_class_def`, the cleanup chain
   emissions, the temp dtor calls in the hoist code) over to call
   through it. Outputs change from `Foo_ctor` etc. to the prefixed
   form `sf__Foo__ctor`. Goldens get bulk-updated. Pure refactor,
   no new functionality.
2. **Second commit — free functions + extern "C" tag.** Mangle free
   functions through the framework too, with `extern "C"` opt-out.
   Requires the small parser change to tag `extern "C"` blocks and
   propagate the linkage to contained decls.
3. **Third commit (optional, deferrable forever).** Implement
   `mangler_itanium` as a second vtable. Add `--mangling=itanium`
   flag. Sea-front continues to emit human by default.

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
