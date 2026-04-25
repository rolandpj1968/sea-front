# AST Representation

## Overview

The sea-front AST is a tagged union over a fixed enum of node kinds. It
represents both the program syntax (statements, declarations, expressions)
AND the type system (a separate `Type` struct), with a single shared shape
across all phases.

This document describes the AST as a *data structure*: what's in it, what
the slots mean, and what state each slot is in at each point in the
pipeline. How the AST is *built* is parser.md. How it is *consumed* is
emit.md.

## Design Choices

### Tagged union with embedded structs

Each node carries a `NodeKind kind` discriminator plus a single anonymous
union of per-kind structs. This is the "Option B" alternative to a flat
struct with overloaded fields:

```c
struct Node {
    NodeKind kind;
    Token *tok;             /* anchoring token (source loc + diagnostics) */
    Type  *resolved_type;   /* set by sema for expression nodes */
    bool   is_type_dependent;
    const char *codegen_temp_name;
    union {
        struct { uint64_t lo, hi; bool is_signed; } num;
        struct { Token *name; bool implicit_this; ... } ident;
        struct { TokenKind op; Node *lhs, *rhs; } binary;
        /* ... one struct per NodeKind ... */
    };
};
```

Each accessor is `n->binary.lhs`, `n->ident.name`, etc. — the kind tag
documents which arm is valid. There is no compiler-enforced safety; using
the wrong arm reads garbage from another arm's bytes. The convention is
that any code touching `n->X` must have first checked `n->kind`.

### Allocation

All nodes come from a single arena owned by the `Parser`. Nodes are never
freed individually — the arena is dropped at end-of-process. Sub-arrays
(`call.args`, `block.stmts`, `func.params`) are built incrementally with a
`Vec` helper and "frozen" by copying the Vec's data pointer onto the node.

### Tokens, not strings

Identifier names are `Token *` pointers into the lexer's token list, which
in turn point into the original source buffer. No name is ever copied as a
string. Two `Token *` values refer to the same identifier iff their
`(loc, len)` ranges contain the same bytes — comparison is `len ==` plus
`memcmp`. This is correct because the source buffer is immutable for the
lifetime of the AST.

## Node Categories

The `NodeKind` enum is grouped by category in `parse.h`:

| Category | Examples | Notes |
|----------|----------|-------|
| Literal | ND_NUM, ND_FNUM, ND_STR, ND_CHAR, ND_BOOL_LIT, ND_NULLPTR | Leaf nodes — value baked into the per-kind struct |
| Name | ND_IDENT, ND_QUALIFIED, ND_TEMPLATE_ID | Pre-sema: just tokens. Post-sema: `resolved_decl` filled |
| Operator | ND_BINARY, ND_UNARY, ND_POSTFIX, ND_ASSIGN, ND_TERNARY, ND_COMMA | `op` is the original `TokenKind` |
| Postfix | ND_CALL, ND_MEMBER, ND_SUBSCRIPT | |
| Type-bearing expr | ND_CAST, ND_SIZEOF, ND_ALIGNOF, ND_OFFSETOF, ND_VA_ARG | Carry a `Type *` slot |
| GCC extension | ND_STMT_EXPR, ND_OFFSETOF, ND_VA_ARG | Re-emitted verbatim for gcc |
| Statement | ND_BLOCK, ND_RETURN, ND_IF, ND_WHILE, ND_DO, ND_FOR, ND_SWITCH, ND_CASE, ND_DEFAULT, ND_BREAK, ND_CONTINUE, ND_GOTO, ND_LABEL, ND_EXPR_STMT, ND_NULL_STMT | |
| Declaration | ND_VAR_DECL, ND_FUNC_DEF, ND_FUNC_DECL, ND_PARAM, ND_TYPEDEF, ND_FRIEND | |
| Class | ND_CLASS_DEF, ND_ACCESS_SPEC | |
| Template | ND_TEMPLATE_DECL, ND_TEMPLATE_ID | The instantiation pass clones from these |
| Top-level | ND_TRANSLATION_UNIT | Holds `decls[]`, the full program |

## Core Slots Common to Every Node

| Slot | Set by | Meaning |
|------|--------|---------|
| `kind` | parser | which union arm is valid |
| `tok` | parser | anchoring token for diagnostics + source locations |
| `resolved_type` | sema | type of the value an expression evaluates to (`Type *`); NULL for non-expression nodes and pre-sema |
| `is_type_dependent` | parser/sema | true if this expr depends on a template parameter (Phase 1 marking; see template-instantiation.md) |
| `codegen_temp_name` | emit_c | name of a synthesized local hoisted from this expression (`__SF_temp_N`); when set, `emit_expr` substitutes the name verbatim |

## The Type System

`Type` is a parallel tagged union with its own kind enum:

| Kind | Meaning |
|------|---------|
| TY_VOID, TY_BOOL, TY_CHAR, TY_CHAR16, TY_CHAR32, TY_WCHAR, TY_SHORT, TY_INT, TY_LONG, TY_LLONG, TY_FLOAT, TY_DOUBLE, TY_LDOUBLE | Fundamental types |
| TY_PTR, TY_REF, TY_RVALREF, TY_ARRAY | Indirect — chain via `base` |
| TY_FUNC | `ret`, `params[]`, `nparams`, `is_variadic`, `param_defaults[]` |
| TY_STRUCT, TY_UNION | `tag`, `class_region`, `class_def`, `template_args[]`, plus a handful of has-* flags driving codegen |
| TY_ENUM | `tag` plus a raw token range for the enumerator body — re-emitted verbatim |
| TY_DEPENDENT | Template parameter placeholder; substituted during instantiation |

Cv-qualifiers (`is_const`, `is_volatile`, `is_unsigned`) live on the type
struct itself. For pointers, `is_const` means *the pointer is const* (`T *
const`); for everything else it means *the value is const* (`const T`).

A few cross-references to other docs:

- The `class_region` chain is the heart of name lookup — see parser.md.
- `template_id_node`, `template_args[]`, `n_template_args` drive the
  instantiation pass — see template-instantiation.md.
- `has_dtor` / `has_default_ctor` / `has_virtual_methods` drive
  ctor/dtor synthesis and vtable emission — see emit.md.

## States the AST Passes Through

The AST shape never changes between phases — only its slots get filled in.

### Phase 1: Just-parsed

After `parse_translation_unit` returns:

- Every node has its `kind`, `tok`, and per-kind struct populated.
- `is_type_dependent` is set on dependent expressions and propagated
  bottom-up.
- `Type *` slots on declarations are fully built from the
  decl-specifier-seq + declarator.
- **`resolved_type` is NULL** on every expression node.
- **`ident.resolved_decl` is NULL** on every name reference.
- Names are still raw tokens — no lookup has happened yet.
- Templates are intact — `ND_TEMPLATE_DECL` wraps the unsubstituted body.
- The class-scope side: `class_region` and `class_def` are wired up on
  every defined class type.

### Phase 2: Post-sema

After `sema_run` walks the AST:

- Every expression node has its `resolved_type` filled (or stays NULL
  with a fallback chain in emit_c).
- `ND_IDENT.resolved_decl` and `.overload_set` are populated.
- `ident.implicit_this` is set on names that resolved to a class member
  reached via the implicit `this` — codegen rewrites these to
  `this->name` or a method call.
- Member-access (`ND_MEMBER`) `resolved_type` is set from the member's
  declared type, walking the class's `class_region` (and base chain).
- For class operator overloads, `ND_BINARY.resolved_type` is set from
  the operator method's return type.

### Phase 3: Post-instantiation

After `template_instantiate` runs:

- For every template-id reachable from non-template code, the registry
  has been consulted, the template body cloned, and the clone appended
  to `tu->decls[]` (after any necessary nested instantiations).
- `Type` copies that pointed at `ND_TEMPLATE_ID` now have their
  `template_args[]` filled with concrete `Type *` and a `class_def`
  hooked up to the cloned class body.
- `TY_DEPENDENT` placeholders inside cloned bodies have been replaced
  with concrete types via `subst_type`.
- A second sema pass over the new clones fills their `resolved_type`s
  (the cloned tree starts un-resolved).

### Phase 4: Codegen

`emit_c` consumes the AST without mutating it, except for two slots
written purely for its own bookkeeping:

- `codegen_temp_name`: when a class-typed sub-expression must be hoisted
  to a local (because we'd otherwise take the address of an rvalue,
  pass it by reference, etc.), the original node is tagged with the
  local's name and `emit_expr` short-circuits to printing the name.
- `Type.codegen_emitted`: a struct's full definition has been printed,
  used to suppress duplicate emission.

## Interim "Ambiguous" Representations

Several places in the AST hold *deliberately under-resolved* shapes
because C++ disambiguation requires information the parser doesn't yet
have. The job of later phases is to refine these.

### ND_QUALIFIED — sema-resolved, not parser-resolved

A qualified-id like `std::vector<int>::size_type` is parsed into:

```c
n->kind == ND_QUALIFIED;
n->qualified.parts    = [Token("std"), Token("vector"), Token("size_type")];
n->qualified.nparts   = 3;
n->qualified.lead_tid = ND_TEMPLATE_ID(<int>);   /* if any segment had <args> */
```

The parser does not walk the namespace/class chain. Sema does, and writes
the result into `resolved_type` / `resolved_decl`. Codegen has a fallback
path that emits a best-effort mangled name when sema couldn't resolve.

### ND_IDENT — overload set, not single resolution

For an unqualified name that names a function, sema fills BOTH:

- `ident.resolved_decl` — one historical "best guess"
- `ident.overload_set[] / n_overloads` — the full candidate list

Per-call overload resolution (N4659 §16.3 [over.match]) picks the winner
from `overload_set[]` at the call site, in `visit_call`. The original
`resolved_decl` is then rewritten to point at the winner.

### TY_DEPENDENT — placeholder for substitution

A use of a template parameter `T` in type position parses as:

```c
Type { kind = TY_DEPENDENT; tag = Token("T"); base = NULL; ... }
```

`is_type_dependent` then propagates upward through the AST. The
instantiation pass walks every `Type *` reachable from a clone, replacing
TY_DEPENDENT nodes whose `tag` matches a substitution-map entry with the
concrete type. A `tag` plus optional `dep_member` covers the
`typename T::member` shape.

### TY_INT with a tag — opaque user types

When the parser meets an unknown identifier in type position with a
declarator-like follower, it produces `TY_INT` with the identifier as
the `tag`. emit_c emits the tag verbatim for tags it recognises
(currently `__builtin_va_list`); otherwise the type effectively
disappears into `int`. This is a deliberate "lossy fallback so we keep
parsing" rather than a hard error — it papers over header constructs we
don't fully model. See the SHORTCUT comment block in `type.c` near the
fallback for the exact gating.

### ND_CALL with type-as-arg — special builtins

`__builtin_offsetof(T, m)` and `__builtin_va_arg(ap, T)` take a type as
an argument, which the generic call parser cannot handle. They are
detected by name in `expr.c` and parsed into dedicated `ND_OFFSETOF` /
`ND_VA_ARG` nodes that carry a `Type *` slot directly. emit_c re-emits
them verbatim for gcc.

## Cross-cutting Slots Worth Knowing

### `Type.template_args` — set late, read everywhere

A class template instantiation looks like an ordinary `TY_STRUCT` with
its `template_args[]` filled. The mangler reads these to produce
distinct names for each instantiation
(`vec<int>` → `sf__vec_t_int_te_`). Without `template_args`, all
instantiations of the same template would mangle to the same name and
collide at link time.

The instantiation pass is responsible for ensuring that every Type
copy reachable from non-template code which originated in a template
context has its `template_args[]` populated. This involves a `patch_type`
sweep at the end of `template_instantiate` because Type copies are
shared between the original template body and individual function
parameter lists.

### `Type.class_def` — back-pointer for ordered iteration

Class members live in a `DeclarativeRegion` (hash-bucketed for fast
lookup), but emission needs them in *declaration order* (so member ctors
chain in the right sequence, vtables are laid out correctly, etc.).
`class_def` is the back-pointer to the original `ND_CLASS_DEF` node,
whose `members[]` array preserves order.

### `is_type_dependent` — Phase 1 marking, Phase 2 oracle

Set during parsing — the only base case is an `ND_IDENT` that resolves
to a `TY_DEPENDENT` declaration. Any compound expression inherits
dependence from its children. The instantiation pass uses it as the
oracle for "does this name need Phase-2 lookup against the concrete
class scope, or did Phase 1 already resolve it". See
template-instantiation.md for the lookup phases.
