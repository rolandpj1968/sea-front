# Template Instantiation Policy

Status: **design** for the recursive (use-driven) replacement.
Current implementation is round-loop-based; that's documented in
the *Current shape* section below for context, with the
specifically-rejected pieces called out.

## Motivation

The C++ standard mandates **lazy, use-driven** instantiation
(N4659 §17.7.1 [temp.inst]). A class-template specialization is
implicitly instantiated only when it is referenced "in a context
that requires a completely-defined object type or when the
completeness of the class type affects the semantics of the
program." The negative form (§17.7.1/9) makes this normative:

> "An implementation **shall not** implicitly instantiate a
> function template, a variable template, a member template, a
> non-virtual member function, a member class, or a static data
> member of a class template **that does not require
> instantiation**."

The "shall not" matters. Eager instantiation isn't merely
inefficient — it's non-conforming. A pointer-to-class-template
parameter doesn't require the class to be complete; instantiating
it eagerly is wrong.

The use-driven design also has a practical property: it's bounded
by what the program actually uses, not by what's syntactically
nested. Real C++ code mentions far more types-as-names than it
performs complete-type uses on. Eager (syntactic) instantiation
explodes combinatorially on these mentions; use-driven traversal
visits only what's needed.

## Current shape (rejected design)

`template_instantiate(tu)` runs a **round loop**, up to 32
iterations:

```
for round in 0..32:
    collect_all_uses(tu->tu.decls ∪ all_instantiated)
    process_each_request_in_collected_list()
    merge_new_instantiations_into_tu_decls()
    break if no new requests
```

Each round re-walks the entire TU plus everything previously
instantiated, looking for `ND_TEMPLATE_ID` and qualified-call
uses. The processing phase clones template bodies and pushes them
onto an `all_instantiated[]` array. End-of-round merges new
entries into `tu->tu.decls` so the next round sees them. Loop
terminates when a round adds nothing.

What's wrong with this:

- **The 32-iteration cap is arbitrary.** Anything that needs more
  silently truncates and produces undefined-symbol link errors.
- **Each round re-walks already-processed decls.** O(N) work per
  round across N rounds → quadratic in the number of templates.
- **Two-phase collect/process** is an artifact of the loop, not a
  semantic requirement. A use found mid-processing has to wait
  for the *next* round to be processed.
- **The end-of-round merge** is the source of bug 2a7fca4: when
  the `MAX_INST` cap fired mid-round, `ninst_this_round` (which
  drives the merge index) drifted past `total_inst`'s actual
  growth, producing a negative start index and reading
  pre-buffer garbage into `tu->tu.decls`. The merge is structurally
  fragile.
- **`MAX_INST` is needed because the loop's data structure is
  fixed-size.** A cap-on-instantiations only exists because the
  array has bounded capacity.
- **No topological ordering** — emit order is "round by round,
  insertion order within round," not "dependencies before
  dependents." Required additional logic to insert at the right
  position in `tu->tu.decls`.

The shape is "fixed-point iteration over a global work-set." It's
a smell — fixed-point iteration is the right move when the
dependency graph is genuinely cyclic and unknowable. C++
instantiation isn't either: the dependency graph is known
(use-graph) and acyclic (after memoization on the (template,
args) tuple).

## Replacement: recursive use-driven instantiation

```c
/* Memo: (template-name, concrete-args) → cloned-instantiation. */
static DedupSet memo;

/* Topo-order list — each instantiation is appended AFTER its
 * dependencies are processed, so dependents follow in the list. */
static Node **emit_order;
static int    n_emit_order;

void instantiate_use(InstantiationKey key, Context *ctx) {
    /* Memoization on the canonical (template, args) tuple. Both
     * (a) breaks cycles in mutually-recursive templates and (b)
     * deduplicates repeated uses naturally. */
    Node *existing = memo_lookup(memo, key);
    if (existing) return;

    /* Clone the template's body with the args substitution. */
    Node *cloned = clone_template_body(key.template_def, key.args);

    /* Register BEFORE recursing — if the cloned body references
     * the same template (recursive template) or a template that
     * eventually references this one (mutual recursion), the
     * lookup above short-circuits. */
    memo_add(memo, key, cloned);

    /* Re-sema the cloned body so name lookups and type
     * resolutions reflect the substituted arguments. */
    sema_visit(cloned);

    /* Walk the cloned body for COMPLETE-TYPE USES that require
     * further instantiations. The use-set is what the standard
     * enumerates in §17.7.1/2:
     *
     *   - Member access (Class::member, p->member) where Class
     *     is a class-template instantiation.
     *   - Variable / parameter / return / member declarations of
     *     class type (not pointer/reference — those are
     *     incomplete-type uses and DO NOT require instantiation).
     *   - sizeof, typeid, dynamic_cast operands of class type.
     *   - Base-class clauses.
     *   - Implicit conversion through derived-to-base.
     *   - Calls to function templates (deduce + instantiate the
     *     specific specialization the call resolves to).
     */
    for (Use *u = walk_complete_type_uses(cloned); u; u = u->next) {
        instantiate_use(u->key, ctx);
    }

    /* Append AFTER recursion bottoms out, so emit_order is
     * topologically sorted (dependencies before dependents). */
    emit_order[n_emit_order++] = cloned;
}

/* Driver: walk top-level user code for uses, then run the merge
 * once at the end. */
void template_instantiate(TU *tu) {
    /* Build registry, dedup table, etc. — same as today. */
    ...

    /* Walk top-level decls for complete-type uses. Each found use
     * recursively instantiates everything reachable. */
    for (int i = 0; i < tu->ndecls; i++) {
        for (Use *u = walk_complete_type_uses(tu->decls[i]); u; u = u->next)
            instantiate_use(u->key, ctx);
    }

    /* Single merge at the end. emit_order is topologically
     * ordered, so the merge is just an append (or insert at a
     * single chosen point). */
    merge_emit_order_into_tu_decls(tu, emit_order, n_emit_order);
}
```

## Termination

- **Finite use-graph.** The set of distinct
  (template-name, concrete-args) tuples is bounded by the source
  TU. Concrete-args after substitution don't manufacture types
  the source didn't mention; they only combine them. The memo's
  domain is finite.
- **Memoize-before-recurse** breaks cycles. `A<int>` triggering
  `B<int>` triggering `A<int>` finds `A<int>` already in the memo
  on the third call and short-circuits.
- **Self-recursive templates** (factorial-style) terminate when
  they hit a base specialization the source provides. If the
  source has no base case, the program is ill-formed C++ and the
  recursion will overflow the C call stack — which is acceptable
  behavior for ill-formed input. Real-world templates have base
  cases by construction.

## What's eliminated

| Current artifact | Eliminated because |
|---|---|
| Round loop | Recursion subsumes the iterate-to-fixpoint |
| 32-iteration cap | No iteration |
| `MAX_INST = 32768` cap | Memoization bounds the work; arena-grown emit list has no fixed size |
| `ninst_this_round` tracking | No rounds |
| End-of-round merge with `tu->tu.decls` | Single merge at end, against a topo-ordered list |
| Insert-position scan in `tu->tu.decls` | Topo order falls out of post-recursion append |
| `MemberTmplRequest` / `InstRequest` queues | Direct call to `instantiate_use` at the discovery site |
| Two-phase collect/process | One phase: discover and process inline |
| Bug class 2a7fca4 (cap drift) | No counters to drift |

## What stays

- `clone_node` / `subst_type` — the substitution machinery is
  correct; only the orchestration around it changes.
- The `DedupSet` data structure — repurposed as the memo.
- Sema's `sema_visit_node` — still called on each cloned body to
  resolve names and types post-substitution.
- The `find_ool_methods` / qualified-call resolution — same
  matching logic, just invoked from the recursive walker rather
  than from the round loop.
- The dedup-key encoding (class + nul + member + nul + arg-types)
  — used unchanged as the memo key.

## Use-set: what triggers instantiation

This is the heart of the use-driven model. Each entry below is a
context where the standard requires the class to be complete.

| Construct | Standard ref | Requires instantiation of |
|---|---|---|
| `T x;` (variable of class type) | §6.7/4 [basic.def] | T |
| `T f(...)` (return type, class) | §11.3.5 [dcl.fct] | T (only if used; declarations don't force) |
| Member field of class type | §12.2 [class.mem]/2 | the class type |
| Base-class clause | §13.1 [class.derived] | the base |
| `Class::member` access | §6.4.5 [class.qual] | Class |
| `p->member` / `o.member` | §8.2.5 [expr.ref] | the class of `*p` / `o` |
| `sizeof(T)` / `alignof(T)` | §8.5.2.6 [expr.sizeof] | T |
| `typeid(T)` (polymorphic) | §8.2.8 [expr.typeid] | T |
| `dynamic_cast<T*>(...)` | §8.2.7 [expr.dynamic.cast] | T |
| Implicit derived-to-base | §11.6.3 [conv.ptr] / §6.6 [conv] | base |
| Call to function template | §17.8 [temp.deduct] | the specific specialization |

Things that **do not** trigger instantiation:

- `T *p;` — pointer to class-template type. Incomplete-type use.
- `T &r;` — reference. Same.
- `template<typename U> struct X<T*> { ... };` — partial-spec arg
  is a name reference, not a use.
- `typedef T MyT;` — name binding, not a use.
- Friend declarations of class templates.
- Mention as another template's argument: `vec<T>` is a name, not
  a use. Only when the using template's body actually accesses
  `T::*` or `T x;` does T require instantiation.

The walker enforces this distinction. Eager (syntactic) walks
treat every type-name mention as a use; the use-driven walk does
not.

## Implementation phases

1. **Add the recursive entry point alongside the existing round
   loop.** New function `instantiate_use_recursive(key)`.
   Memo-based, no round counter. Driven from a small new
   top-level walker that finds uses in `tu->tu.decls`.
2. **Verify equivalence** by running both old and new in
   parallel during a transition window, comparing the resulting
   `emit_order` (modulo permutation; the topo-ordered new should
   be a valid topo-sort that the old happened to produce too).
3. **Switch the dispatcher** to call the new entry point.
4. **Delete the old round loop and its support cast** (`InstRequest`,
   `ninst_this_round`, the merge logic). `MemberTmplRequest`
   stays as a convenience struct but is no longer queued; it's
   invoked synchronously.

## What tests should cover

The existing emit-c suite covers the substitution and codegen
correctness; that doesn't need to change. New tests specifically
for the use-driven model:

- **Lazy instantiation** — `vec<X>* p; p = nullptr;` should
  compile *without* instantiating `vec<X>`. (Verifiable by
  grepping the emitted C for the absence of `struct sf__vec_t_X_te_`.)
- **Cycle termination** — mutually-recursive templates A<int>↔B<int>
  produce both, exactly once.
- **Self-recursion termination** — factorial<5>::value
  instantiates factorial<5> through factorial<0>, exactly once
  each, in topo order.
- **No fixed-point iteration leaks** — a template that uses
  another template that uses another, etc., produces all the
  needed instantiations in a single recursive descent (verifiable
  by the absence of round-iteration counters in any logging).
- **Topo emit order** — the dependency `Box<int>` references
  `vec<int>` in a member field; emitted C has `struct
  sf__vec_t_int_te_` defined before `struct sf__Box_t_int_te_`.
