# Template Instantiation

## Overview

The instantiation pass turns a *template* (an `ND_TEMPLATE_DECL`
wrapping a generic class or function body) into a concrete *instance*
(a fresh `ND_CLASS_DEF` or `ND_FUNC_DEF` with all template parameters
replaced by concrete types).

It runs as a pass between sema and codegen:

```
parse → sema → template_instantiate → emit_c
```

The approach is **AST-level cloning**: the original template body is
deep-copied with every `Type *` walked through a substitution map that
swaps `TY_DEPENDENT` placeholders for concrete types. This is more
principled than token-level macro replay — disambiguation, lookup, and
typing decisions made when the template was first parsed are preserved
in the clone.

The pass is implemented in `src/template/instantiate.c` (driver,
registry, dedup, deduction) and `src/template/clone.c` (the
deep-copy + substitute machinery).

## Why AST-Level Cloning

The alternative — re-parse the template's tokens with the parameters
substituted — has two problems:

1. C++ disambiguation depends on prior context. Re-parsing inside a
   different scope can flip a declaration into an expression and vice
   versa. Token replay would re-run those decisions and potentially
   diverge from what the original parse saw.
2. Templates can be *parsed once* and *instantiated many times*. Token
   replay does the parse work N times for N instantiations.

AST cloning runs the parser exactly once per template definition, then
just walks the tree at instantiation time. It also makes the
instantiation pass debuggable with the same `--dump-ast` machinery as
the rest of the pipeline.

## What sea-front Implements

| Feature | Status |
|---------|--------|
| Class templates | Yes |
| Function templates (namespace-scope) | Yes |
| Member function templates inside non-template classes | Yes |
| Multiple type parameters | Yes |
| Default template arguments (incl. `Alloc = allocator<T>`) | Yes |
| Full specialization | Yes |
| Out-of-class method definitions | Yes |
| Nested / transitive instantiation | Yes |
| Deduplication across reachable uses | Yes |
| Template argument deduction (basic) | Yes — for the gcc 4.8 patterns |
| Partial specialization | Not yet |
| SFINAE / `enable_if` | Not yet |
| Variadic templates | Not yet (out of scope for the C++03 bootstrap) |
| Two-phase lookup (full §17.7) | Limited — see "Lookup Phases" below |

## The Pass Pipeline

```
template_instantiate(tu, arena):
    1. build_registry(tu)           — index every ND_TEMPLATE_DECL
    2. collect_from_node(tu)        — find every template-id reachable
                                      from non-template code; queue
                                      InstRequests
    3. for each request:
         instantiate_one(...)       — clone body, substitute types,
                                      run nested collection, dedup,
                                      append to tu->decls[]
    4. patch_all_types(tu)          — walk every Type * in the AST,
                                      hook up class_def + template_args
                                      on Type copies that originated in
                                      a template context
```

### Registry

`build_registry` walks `tu->decls[]` and every nested namespace
(`ND_BLOCK` at top level) plus class member lists for member-templates.
Each entry maps a template name (or `Class\0member` for member templates)
to the `ND_TEMPLATE_DECL` node.

Two lookups: `registry_find(name)` for top-level templates and
`registry_find_member(class_name, member_name)` for class-scoped member
templates.

### Collection

`collect_from_node` is a full AST walk. For every `Type *` that has a
`template_id_node` (a parsed template-id like `vec<int>`), and for
every `ND_CALL` whose callee is a function template-id or a qualified
member-template call, the collector:

1. Resolves the template name in the registry.
2. Builds the concrete type-argument list (either explicit args from
   the source or deduced from the call-site argument types).
3. Forms a dedup key from `(template-name, arg-types)`.
4. Queues an `InstRequest` if not already instantiated.

The walk recurses into class member bodies, function bodies, statement
trees, expression trees — anywhere a template-id can appear.

### Argument Deduction

Implemented in `deduce_from_pair` and `deduce_template_args`
(`instantiate.c`). Mirrors `subst_type` in reverse: given a pattern
type P (containing `TY_DEPENDENT`) and a concrete argument type A,
recursively binds the dependent leaves of P to the matching parts of A.

Covers the cases needed for gcc 4.8 C++03 patterns:

- `T*&` vs concrete `int**` → strip ref, `T*` vs `int*` → `T = int`
- `vec<T>` vs `vec<int>` → walk template_args, `T = int`
- `T` vs anything → `T = anything`

Does NOT cover (TODO):

- Conversion sequences (deduction requires exact match modulo
  ref/cv-stripping)
- Pack expansion
- Non-type template parameter deduction
- SFINAE (failed deduction is just "no candidate")

### Specialization Lookup

Before instantiating a primary template, `registry_find_specialization`
checks for an explicit full specialization with matching argument
types. If found, the specialization's body is used directly (no
substitution needed — full specializations have `nparams == 0`).

### Cloning + Substitution

`clone_node(n, map, arena)` deep-copies a Node, recursing through every
sub-node. `subst_type(ty, map, arena)` deep-copies a Type, replacing
every `TY_DEPENDENT` whose `tag` matches a map entry with the concrete
type, and recursively substituting through `base`, `ret`, `params[]`,
and `template_args[]`.

Important: clones share `Token *` pointers with the original (tokens
are immutable, deduped by source location), but every `Node *` and
`Type *` is freshly allocated in the instantiation arena.

### Dedup

`DedupSet` is a hash set keyed by `(template-name, concrete-arg-types)`.
The key is built by `type_to_key` which produces a stable string
encoding of the arg list. Hits return the previously-instantiated
`Type *`; misses trigger a fresh `instantiate_one`.

Dedup is critical: gcc 4.8 instantiates `vec<tree>` from dozens of
files. Without dedup, every file would emit its own copy and the link
would fail with multiple definitions.

### `patch_all_types` — the post-pass

A subtle issue: `Type *` is deep-shared across the AST. A single class
template instantiation creates one `Type *`, but parameter lists,
return types, and member field types might hold *copies* of that Type
(made for cv-qualifier purposes). Those copies don't get `class_def`
or `template_args[]` filled in by the instantiation itself.

The post-pass `patch_all_types` walks every `Type *` reachable from the
TU and, for any TY_STRUCT/TY_UNION whose `tag` matches a known
instantiation, hooks up the missing `class_def` + `template_args[]`.
This is what makes downstream `class_region`-walks and member-lookups
work consistently even on Type copies.

## Lookup Phases (Two-Phase Lookup)

N4659 §17.7 [temp.res] mandates two-phase lookup:

- **Phase 1**: at template definition, names that don't depend on
  template parameters are looked up immediately.
- **Phase 2**: at instantiation, dependent names are looked up against
  the substituted scope.

sea-front implements a partial version:

- Phase 1 happens during the parse + sema of the template definition.
  Non-dependent names get their `resolved_decl` set normally.
- The `is_type_dependent` flag is the oracle for "this needed Phase 2".
  It's set during parsing (base case: `ND_IDENT` resolves to a
  `TY_DEPENDENT` declaration; propagation: any compound expression
  inherits dependence from its children).
- During cloning, `clone.c` performs Phase-2 qualified lookup for
  `ND_QUALIFIED` nodes against the substituted class scope.
- For unqualified dependent names, the cloned tree gets a fresh sema
  pass after the substitution is complete, which fills in the
  `resolved_decl` against the now-concrete scope.

### Limitations vs. the standard

- ADL (argument-dependent lookup) in dependent contexts is not modeled
  separately from regular Phase-2 lookup.
- Value-dependent expressions (non-type template parameters in
  expression context) get the same coarse "type-dependent" treatment.
- A handful of remaining narrow shortcuts in `emit_c.c` paper over
  cases where Phase-2 lookup didn't run (e.g. arg-type mangling
  fallback for unresolved `ND_QUALIFIED` calls). Each is gated and
  comment-cited; greppable as `SHORTCUT`.

## What Goes Wrong, and How

A few classes of bug recur. Knowing them speeds up debugging:

### Symptom: same template instantiated twice → "redefinition" link error

The dedup key didn't match. Common causes:

- Two `Type *` copies of the same instantiation reached the collector
  separately and produced different keys. Check `type_to_key`.
- The collection walk reached the same template-id from two contexts
  with different `usage_type` — make sure `dedup_add` is hoisted
  outside any per-usage guard.

### Symptom: cloned function calls undefined function `sf__X__Y_te_`

The mangled name reflects an unsubstituted dependent type. Either:

- `subst_type` didn't reach this Type * (look for a missing recursion
  arm in clone.c)
- A Type copy was made AFTER substitution and isn't in the
  substitution map. `patch_all_types` covers most of this; add a
  patch for the missing case.

### Symptom: `class_region` is NULL on a Type that should be a known class

Typically a Type copy that didn't get patched up. Two paths:

- Use the helper `find_class_def_node_by_tag_args(tu, ty)` to look up
  the canonical Type with `class_region` populated. sema's
  `visit_member` already does this fallback.
- Add the case to `patch_all_types`.

### Symptom: `T x = T()` in a template body emits as `T x = T()` instead of `{0}`

Sema didn't rewrite the functional-cast as a default-init. Check
`emit_var_decl_inner`'s functional-cast detection — there's gating on
`resolved_decl->entity == ENTITY_TYPE` plus a name-match fallback for
typedef names.

## Mangling

Each instantiation gets a distinct C name via the mangler
(`src/codegen/mangle.c`). The naming scheme is described in
[`mangling.md`](mangling.md). The instantiation pass is responsible for
ensuring `Type.template_args[]` is set on every instantiated Type
before codegen sees it; the mangler reads from there.

## Cross-references

- AST-level details (what `template_id_node`, `template_args[]`, and
  `TY_DEPENDENT` mean): [`ast.md`](ast.md)
- Mangling: [`mangling.md`](mangling.md)
- Multi-TU dedup (the link-time side of the deduplication story):
  [`inline_and_dedup.md`](inline_and_dedup.md)
- Codegen consumption of instantiated AST: [`emit.md`](emit.md)
