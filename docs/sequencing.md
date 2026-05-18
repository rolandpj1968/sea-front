# C++17 Sequencing in Lowered C

**Status: phased. Phase 1 (assignment RHS-before-LHS) implemented; later
phases gated on evidence of need.**

## Motivation

C++ has tightened evaluation-order rules over time. Several operators have
fully specified sequencing in C++17 that are completely unsequenced in C.
Naïve C++ → C lowering inherits C's permissive ordering and can therefore
silently mis-execute programs whose semantics rely on the C++ rules.

| Construct | C++17 (N4659) | C |
|---|---|---|
| `lhs = rhs`, `lhs op= rhs` | §8.18/1: RHS sequenced before LHS | unspecified |
| `a << b << c`, `a >> b >> c` | §8.7/4: left-to-right | unspecified |
| `f(a, b, c)` | §8.2.2/4: callee before args; args indeterminately sequenced | unspecified |
| `a.b`, `a->b`, `a[b]` | §8.2.2/4: postfix LHS before sub-expression | unspecified |

The risk is only realised when operands have side effects:

```cpp
int i = 0, a[3] = {0, 0, 0};
a[i] = (i = 2);   // C++17: a[2] = 2 (RHS sequenced first).
                  // C lowering: unspecified — may write a[0].
```

In practice most code we transpile (gcc 4.8 source, libcpp) was written
to be C-portable and doesn't depend on C++17-only orderings. gcc and
clang in C mode also tend to evaluate `=` operands left-to-right, which
masks the issue. So the gap is latent, not active — but it's a real
correctness hole worth closing as we tighten the rest of the transpiler.

## Discipline

> **Whenever sea-front lowers a C++ expression to C, if C++17 specifies
> an evaluation order that C does not, sea-front must make that order
> explicit in the emitted C when the operands have side effects.**

Corollaries:

- **Side-effect-aware hoisting, not blanket hoisting.** The lazy fix
  is to hoist *every* operand into a named temp. That works but bloats
  every assignment in the output with `__SF_seq_*` decls, hurting
  readability of a tool whose audience (cproc maintainer, gcc 4.8
  bootstrapping) is supposed to read the C. Use a side-effect predicate
  and hoist only when the hazard is real.
- **Hoist the operand that needs to run *first*.** Binding the
  earlier-sequenced operand to a temp gives an explicit "run, then
  proceed" boundary. The later-sequenced operand can stay inline.
- **Hoist to a fresh single-assignment local, never reuse names.** Each
  hazard site gets its own `__SF_seq_<id>` so two adjacent assignments
  can't accidentally share a temp.
- **Skip the rewrite when neither operand has side effects, or when
  only one does.** Pure value computations re-order freely; only one
  side effect means there's no *between* to mis-order.

## Side-effect predicate

`expr_has_side_effects(Node *n)` is conservative — it returns true only
for well-understood side-effecting forms:

- Calls (`ND_CALL`, `ND_NEW`, `ND_DELETE`).
- Pre/post increment/decrement.
- Nested assignment / compound assignment.
- Any subexpression of the above (recursive).

It returns false for plain lvalue references (`x`, `*p`, `a.b`,
`obj->field`, `arr[i]` with non-side-effecting `i`), arithmetic, casts,
and literal nodes — these are pure value computations that reorder
safely.

The asymmetric default is intentional: we'd rather miss a hoist
opportunity in an exotic node kind than blanket-hoist everything. If a
real bug exposes a missing case, extend the predicate.

## Phase 1 — Assignment RHS

For `ND_ASSIGN` with `op == TK_ASSIGN` (or any compound `+=` etc.),
during `hoist_temps_in_expr`:

```
if both sides have side effects, hoist RHS to a fresh local.
```

The emitted C becomes:

```c
T __SF_seq_N = <rhs>;
<lhs> = __SF_seq_N;
```

Sequenced left-to-right in C by virtue of statement order, matching
C++17 RHS-before-LHS semantics.

Restrictions:

- **Scalar RHS only** in phase 1. Class-typed RHS goes through the
  existing `hoist_emit_decl` / `is_class_temp_call` paths, which have
  their own dtor-scope considerations. If a class-rhs ordering bug
  surfaces, extend.
- **No effect on the ternary-as-lvalue rewrite.** That lowering
  produces `*(c ? &a : &b) = rhs`, which has the same RHS-vs-LHS
  hazard. The phase-1 hoist catches it through the same code path.

## Other C++17 sequencing rules

Beyond assignment, C++17 specifies ordering for several constructs
that C leaves unspecified:

- Function call: callee sequenced before args (§8.2.2/4). Args are
  indeterminately sequenced *with respect to each other* in both
  languages, so no rewrite needed there.
- Subscript: postfix sequenced before sub-expression (§8.2.1).
- Shift: left operand before right (§8.7/4).
- Member access `a.b`, `a->b`: trivially single-sided.

The phase-1 hoist already covers all of these *when they appear as
the LHS of an assignment* — the LHS gate (`!= ND_IDENT`) catches
anything more complex, and the hoist itself ensures RHS is
sequenced first.

Outside of an enclosing assignment, these rules would each require
hoisting the *first-sequenced operand* into a fresh local. That
operand typically has a compound declarator type (function pointer
for a callee, array/pointer for a subscript base), which the
seq-hoist's simple `T name = init;` form cannot render — it would
need declarator-aware decl emit. Combined with how rare the hazard
is in real code (`f()[g()]` with interlocking side effects between
`f` and `g`), the cost/benefit doesn't justify pre-implementing.
If a real test surfaces a miscompile from one of these rules, build
the declarator-aware path then.

## Anti-patterns

- **Always-hoist RHS.** Produces lowered C like
  `int __SF_seq_0 = 1; x = __SF_seq_0;` for trivial assignments.
  Unreadable and pointless. Use the predicate.
- **GNU statement-expressions.** `({ T t = rhs; lhs = t; t; })` is
  one shape that gives the right semantics, but stays in GNU C and
  blocks the cproc side-goal. Always use plain statement
  sequencing — emit the temp decl as a separate top-level statement.
- **Single shared seq-temp per statement.** Reusing one name across
  multiple hazard sites in a comma-chained expression creates a hidden
  data dependency between them. One temp per hoist.
