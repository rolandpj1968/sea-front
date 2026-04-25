# C Code Emission

## Overview

`emit_c` turns the post-instantiation AST into C source. The output is
designed to compile with any conforming C99 compiler — gcc, clang, tcc.
This document describes how each C++ construct is translated.

The emitter lives in `src/codegen/emit_c.c` (the bulk) and
`src/codegen/mangle.c` (name mangling). Mangling itself is described in
detail in [`mangling.md`](mangling.md).

## Output Conventions

- All names in the global namespace are prefixed `sf__` to avoid
  colliding with C library / system headers.
- Each emitted definition is preceded by a `/* C++: <original> */`
  comment showing the original C++ declaration form. Useful for grep,
  debugging, and source-mapping.
- Class types emit as `struct sf__ClassName`. Unions as
  `union sf__UnionName`.
- Method calls emit as free-function calls passing `this` explicitly
  as the first argument.
- References (`T&`) lower to pointers (`T *`). Callers pass `&x`;
  callees see `*p` semantics.
- Output uses no nonstandard C extensions except `__attribute__((weak))`
  for inline / template-instantiation deduplication.

## C++ to C Translation Table

### Classes

```cpp
class Foo {
    int x;
    void set(int v);
};

void Foo::set(int v) { x = v; }
```

→

```c
struct sf__Foo {
    int x;
};

void sf__Foo__set(struct sf__Foo *this, int v) {
    (this->x = v);
}
```

The implicit `this` parameter is made explicit. The body's unqualified
`x` is rewritten to `this->x` because sema set `ident.implicit_this`.

### Inheritance — `__sf_base` chaining

```cpp
class B : public A { ... };
```

→

```c
struct sf__B {
    struct sf__A __sf_base;   /* always at offset 0 */
    /* B's own members */
};
```

A `B*` is layout-compatible with an `A*` because `__sf_base` is at
offset zero. Member access through the base walks the chain:
`b.a_member` in C++ becomes `b.__sf_base.a_member` in C.

### Constructors and destructors

If the class has a non-trivial constructor or any class-typed members
that need construction, codegen synthesises (or emits user-written):

```c
void sf__Foo__ctor(struct sf__Foo *this);
void sf__Foo__dtor(struct sf__Foo *this);
```

Member subobjects are constructed/destroyed in declaration order
(reverse for dtors), per N4659 §15.6.2/13 [class.base.init]. The
ordering uses `Type.class_def->class_def.members[]` which preserves
parse order.

`Foo a;` at block scope emits as:

```c
struct sf__Foo a;
sf__Foo__ctor(&a);
/* ... block body ... */
__SF_cleanup_N: ;       /* cleanup label */
sf__Foo__dtor(&a);
```

### Goto-chain destructor cleanup

When a block has multiple class-typed locals with destructors, exit
paths (return, break, continue, fallthrough) jump through a chain of
labels that destroy the live locals in reverse order. This is the
"slice D" cleanup described in `project_dtor_strategy.md`.

```cpp
void f() {
    Foo a;
    if (cond) return;
    Foo b;
    return;
}
```

→

```c
void f(void) {
    struct sf__Foo a;
    sf__Foo__ctor(&a);
    if (cond) goto __SF_cleanup_0;
    struct sf__Foo b;
    sf__Foo__ctor(&b);
    goto __SF_cleanup_1;
__SF_cleanup_1: ;
    sf__Foo__dtor(&b);
__SF_cleanup_0: ;
    sf__Foo__dtor(&a);
__SF_epilogue: ;
}
```

The exact chain logic — labels per declared class temp, three-way
branching for return/break/continue, etc. — lives in `emit_block` and
the helpers near `emit_cleanup_chain_for_added`.

### Virtual methods (vtables)

Classes with `has_virtual_methods` true get an extra synthesised member
at offset 0:

```c
struct sf__Foo {
    struct sf__Foo__vtable_t *__sf_vptr;   /* offset 0 */
    /* ... rest of layout ... */
};
struct sf__Foo__vtable_t {
    void (*virt_method_1)(struct sf__Foo *this, ...);
    /* ... */
};
static struct sf__Foo__vtable_t sf__Foo__vtable = {
    sf__Foo__virt_method_1,
    /* ... */
};
```

Constructors install the vptr first thing in the body. Virtual calls
emit as `(obj)->__sf_vptr->method(obj, ...)` instead of a direct call.

### References (`T&` and `T&&`)

References lower to pointers, but the AST keeps them as `TY_REF` /
`TY_RVALREF` so emit_c can preserve the call-site semantics:

```cpp
void take(int& r);
int x; take(x);          // pass by reference
```

→

```c
void take(int *r);
int x; take(&x);
```

For initialization:

```cpp
T& r = expr;             // bind reference
const T& cr = make();    // bind to rvalue
```

→

```c
T *r = &expr;            // address-of an lvalue
const T cr_tmp = make(); // materialise rvalue into temp
const T *cr = &cr_tmp;
```

### Method calls — free-function form

```cpp
obj.method(arg);         // value object
ptr->method(arg);        // pointer object
```

→

```c
sf__Cls__method(&obj, arg);
sf__Cls__method(ptr, arg);
```

For static methods, no receiver is passed: `Cls::sm(x)` →
`sf__Cls__sm(x)`.

### Operator overloads

`a + b` where `a` is class-typed becomes `sf__Cls__plus(&a, b)`. The
mangling appends `_const` for const-qualified methods, plus a parameter
suffix to disambiguate overloads (see `mangling.md`).

Both binary and unary operators are dispatched. The mapping from
TokenKind to mangled suffix is in `binop_to_operator_suffix` /
`unop_to_operator_suffix` in `emit_c.c`.

### Subscripts and operator[]

`v[i]` where `v` is class-typed dispatches to `sf__Cls__subscript(&v, i)`.
If the operator returns a reference (`T& operator[]`), the call is
wrapped in `(*...)` so the result is a value, mirroring C++ semantics.

### Templates

Per [template-instantiation.md](template-instantiation.md), template
*definitions* don't emit at all (`emit_top_level` returns early on
`ND_TEMPLATE_DECL`). Template *instantiations* — synthesised by the
instantiation pass and appended to the TU's decl list — emit as
ordinary `ND_CLASS_DEF` / `ND_FUNC_DEF` nodes.

The class-tag mangling embeds the concrete template arguments:
`vec<int>` emits as `struct sf__vec_t_int_te_`, where `_t_` and `_te_`
are the human-vtable's "template open / template close" markers. Full
scheme in [`mangling.md`](mangling.md).

### Temp materialization (Slice D)

The C++ source can take the address of an rvalue (`&(a + b)`,
`&func_returning_struct()`, ...). C cannot. Whenever emit_c would
produce a `&` of a non-lvalue expression — most commonly because a
class-typed value needs to be passed as `this` to a method — it hoists
the value to a synthesized local:

```cpp
(a + b).method();
```

→

```c
struct sf__Cls __SF_temp_3 = sf__Cls__plus(&a, b);
sf__Cls__method(&__SF_temp_3);
```

The hoisting machinery is `hoist_temps_in_expr` and friends in
emit_c.c. The original AST node carries a `codegen_temp_name` field
that `emit_expr` substitutes verbatim instead of re-emitting the
expression. The synthesized local also goes through the cleanup chain
if it has a destructor.

### Mini-block hoisting for if-conditions

When an if-condition contains an expression that needs hoisting, but
the if-statement body is a single statement (no `{}`), emit_c wraps
the whole thing in a fresh block to give the hoisted temps a scope:

```cpp
if ((a + b).method()) doit();
```

→

```c
int __SF_cond_5;
{
    struct sf__Cls __SF_temp_3 = sf__Cls__plus(&a, b);
    __SF_cond_5 = sf__Cls__method(&__SF_temp_3);
}
if (__SF_cond_5) doit();
```

### Default arguments

Captured per-parameter at parse time, stored in `Type.param_defaults[]`
on the function type. At each call site, if the call passes fewer
arguments than the function has parameters, the missing args are
emitted from the parameter defaults. Free-function and method calls
both supported. Defaults from a forward declaration are merged into
the definition's Type by `region_declare_in`.

### `vNULL` and other gcc idioms

gcc's `vec.h` defines `vNULL` as a sentinel object with a templated
conversion operator (`operator vec<T>() const { return vec<T>(); }`).
sea-front doesn't model conversion operators, so it emits the literal
identifier `vNULL` as `{0}` (a zero-initializer compound literal),
which produces the right value for the cases gcc 4.8 uses.

### `__builtin_offsetof`, `__builtin_va_arg`, `__builtin_va_list`

These are gcc/clang extensions whose syntax (a type as an argument)
breaks the regular call grammar. They get dedicated AST nodes
(`ND_OFFSETOF`, `ND_VA_ARG`) and emit verbatim for gcc to handle.
`__builtin_va_list` is a tagged opaque `TY_INT` that emits as the
identifier rather than `int`.

## Two-Phase Emission

emit_c walks the TU twice:

1. **PHASE_STRUCTS**: emit forward declarations for all classes, then
   full struct definitions in dependency order. Member-by-value
   dependencies are emitted first; pointer members are satisfied by the
   forward declaration. Structural dedup ensures each tag-with-args
   pair is emitted exactly once.
2. **PHASE_METHODS**: emit method bodies and free functions.

This split decouples struct layout ordering from function-body
ordering and is essential for templates: a template instantiation often
contains forward references that only resolve once all instantiations
are visible at struct level.

## Multi-TU Deduplication

Emitted functions and instantiations get `__attribute__((weak))` so
the linker can pick one copy when multiple TUs emit the same
template instantiation or inline function. The full scheme is in
[`inline_and_dedup.md`](inline_and_dedup.md).

## Tracking Emit Order

Top-level decls are emitted roughly in source order, with a topological
sort ensuring that:

- Struct definitions precede uses-by-value
- Forward decls precede uses-by-pointer
- Instantiated templates land before their use sites

The instantiation pass appends synthesized decls to `tu->decls[]` at
collected positions. Subsequent insertions handle nested dependencies
(e.g. `vec<vec<int>>` requires `vec<int>` first).

## SHORTCUTs and TODOs

emit_c has a number of `SHORTCUT` comments documenting cases where
the emit doesn't implement the full standard:

- `vNULL → {0}` lowering (no real conversion-operator support)
- arg-type mangling fallback for unresolved `ND_QUALIFIED` calls
- template-method dispatch heuristic when `class_def` is absent on a
  Type copy (Phase-2 lookup didn't reach this Type)

Each is greppable and gated. Most are scheduled to disappear when the
relevant sema-side feature is filled in.

## Integration

emit_c output is consumed by:

- A C compiler (gcc 4.7.4, tcc, modern gcc) to produce object files.
- The `sea-front-cc` wrapper script for build-system integration —
  intercepts CXX compilation, runs the pp → sea-front → cc pipeline,
  produces a normal `.o`.

When emit_c writes a definition, it's the only place in the pipeline
that writes user-visible source. The transpilation is reversible in
spirit — the `/* C++: ... */` comments preserve the original
construct for source-level debugging.
