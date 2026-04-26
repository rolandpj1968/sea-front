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
 *     re-visit shortcut — close enough for the gcc-4.8 workload.
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

#endif
