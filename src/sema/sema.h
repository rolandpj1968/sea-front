/*
 * sema.h — semantic analysis pass.
 *
 * Walks the AST after parsing and:
 *
 *   - Resolves identifiers to their declarations via the symbol table
 *     (the same DeclarativeRegion structures the parser builds) —
 *     N4659 §6.4 [basic.lookup].
 *   - Fills in node->resolved_type on every expression node it visits.
 *   - Picks free-function and operator overloads — N4659 §16.3
 *     [over.match].
 *   - For class bodies, visits in-class method bodies with the class
 *     scope active (so unqualified members resolve via this).
 *   - Provides a Phase-2 entry point (sema_visit_node) that the
 *     template instantiation pass calls on each cloned subtree —
 *     N4659 §17.7.2 [temp.dep] / §17.7.3 [temp.nondep].
 *
 * Still missing or partial:
 *   - Implicit-conversion ranking is rudimentary; the integer-rank
 *     subset works, but full ICS rules (§16.3.3) aren't modelled.
 *   - Constant evaluation (constexpr) is out of scope.
 *   - Two-phase name lookup is single-phase plus the cloned-subtree
 *     re-visit shortcut — close enough for the C++03/C++11 workloads
 *     in practice.
 *
 * The pass is intentionally lenient — unresolved expressions get
 * resolved_type = NULL and codegen falls back to dumping the source-
 * form. Lets us iterate without blocking on every gap.
 *
 * Implementation lives in sema.c; see the per-function spec citations
 * there for which N4659 rule each visit_* function implements.
 */

#ifndef SEMA_H
#define SEMA_H

#include "../parse/parse.h"
#include "../codegen/mangle.h"  /* OperatorKind */

/* Whole-TU semantic analysis — see sema.c sema_run for the
 * standard-mapping and detail. Mutates the AST in place. */
void sema_run(Node *tu, Arena *arena);

/* Phase-2 sema entry point used by the template instantiation pass —
 * see sema.c sema_visit_node. N4659 §17.7.2 [temp.dep]: re-visits a
 * single subtree (typically a freshly-cloned template instantiation
 * body) so names that became non-dependent after type substitution
 * get their resolved_type / overload-pick. Without this, calls inside
 * a cloned body that became non-dependent (e.g. 'vec_alloc(new_vec)'
 * where new_vec gained a concrete type) never get their bare-ident
 * → ND_TEMPLATE_ID rewrite, and the instantiation pass misses the
 * now-required nested instantiation. */
void sema_visit_node(Node *n, Arena *arena);

/* --------- Overload resolution (N4659 §16.3 [over.match]) ----------
 * These helpers live in sema.c but are still called from emit_c.c
 * during the resolve-everything-before-emit migration. Once every
 * call site is replaced by reads of a sema-stamped Node field, the
 * codegen-side calls disappear and these become sema-internal. */

/* Return the kth param Node of a function-shaped declaration,
 * or NULL if unavailable. ND_FUNC_DEF stores params on func.params;
 * ND_VAR_DECL with TY_FUNC stores them on var_decl.fn_params.
 * Used by overload-resolution viability and by call-site emit to
 * read out param.default_value for default-arg expansion (N4659
 * §11.3.6 [dcl.fct.default]). */
Node *func_param_node(Node *m, int k);

/* Walk a receiver's Type chain looking for const-ness. Handles
 * 'const X', 'X const', 'const X*', 'const X&' — any const on the
 * class itself (or on a ref/ptr's pointee) means the implicit
 * 'this' is 'const C*'. N4659 §16.3.1.4 [over.match.funcs]/4. */
bool receiver_type_is_const(Type *t);

/* Retrieve the const-ness of a candidate declaration. Used for
 * const-aware overload selection. */
bool candidate_is_const(Node *m);

/* N4659 §11.4.9 [class.static] — static member functions take no
 * implicit 'this'. The unqualified-call lowering at the implicit-this
 * site must skip 'this' when the resolved candidate is static. */
bool candidate_is_static(Node *m);

/* Score a single (param-type, arg-type) pair. Higher = better
 * conversion (N4659 §16.3.3.2.1 [over.ics.rank]). */
int score_type_pair(Type *pt, Type *at);

/* Score a candidate against the call's full arg list. Returns -1 if
 * the candidate is non-viable (nparams mismatch with no defaults
 * covering the gap). N4659 §16.3.1.4 [over.match.viable]. */
int overload_match_score(Node *m, Type **arg_types, int nargs);

/* Copy a candidate's param Types into the supplied 64-slot pool,
 * returning the param count. Used by the caller to drive
 * arg-by-arg emit with emit_arg_for_param. */
int copy_member_param_types(Node *m, Type **pool);

/* Walk class_type (and its bases, for non-ctor lookups) collecting
 * methods named `name` (or all ctors when is_ctor) into `found`.
 * Caps at `cap`. The _with_origin variant also records the class
 * type each candidate was found in (so derived-base disambiguation
 * can use the introducer). N4659 §13.5.2 [class.member.lookup]. */
void collect_overload_candidates(Type *class_type, Token *name,
                                  bool is_ctor,
                                  Node **found, int *nfound, int cap);
void collect_overload_candidates_with_origin(
        Type *class_type, Token *name, bool is_ctor,
        Node **found, Type **origin, int *nfound, int cap);

/* Resolve an overload — full entry point. Returns the winning decl's
 * param count and fills *out_param_types with its param types (for
 * mangling). NULL-able out_best receives the winning Node. -1 on no
 * viable candidate. N4659 §16.3 [over.match]. */
int resolve_overload(Type *class_type, Token *name, bool is_ctor,
                     Type **arg_types, int nargs,
                     bool receiver_is_const,
                     Type ***out_param_types,
                     Node **out_best);

/* Same as resolve_overload but also returns the origin class through
 * which the winner was found — used by the call-site mangle to route
 * a using-declaration-inherited method through its real owner. */
int resolve_overload_with_origin(
        Type *class_type, Token *name, bool is_ctor,
        Type **arg_types, int nargs,
        bool receiver_is_const,
        Type ***out_param_types,
        Node **out_best,
        Type **out_origin);

/* Collect candidates for an operator overload (matched by
 * operator-symbol suffix, not by name token). */
void collect_operator_candidates(Type *class_type, OperatorKind op,
                                  Node **found, int *nfound, int cap);

/* Operator-overload resolution — same selection rules as
 * resolve_overload, keyed by operator suffix. */
int resolve_operator_overload(Type *class_type,
                               OperatorKind op,
                               Type **arg_types, int nargs,
                               bool receiver_is_const,
                               Type ***out_param_types,
                               Node **out_best);

#endif
