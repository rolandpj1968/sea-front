/*
 * sema.h — semantic analysis pass.
 *
 * The first slice of sema does just enough to enable a built-ins-only
 * AST → C codegen path:
 *
 *   - Walks the AST after parsing.
 *   - Resolves identifiers to their declarations via the symbol table
 *     (the same DeclarativeRegion structures the parser builds).
 *   - Fills in node->resolved_type on every expression node it visits.
 *
 * Out of scope for the first slice:
 *   - Class members, virtual dispatch, base-class lookup beyond what
 *     the parser already gives us.
 *   - Templates / instantiation.
 *   - Implicit conversions, overload resolution.
 *   - Constant evaluation (constexpr).
 *
 * The pass is intentionally lenient — unresolved expressions get
 * resolved_type = NULL and the codegen will fall back to dumping the
 * source-form. This lets us iterate without blocking on every gap.
 */

#ifndef SEMA_H
#define SEMA_H

#include "../parse/parse.h"

/* Run sema on a translation unit (an ND_TRANSLATION_UNIT node).
 * Mutates the AST in place by filling in resolved_type fields.
 * Uses the parser's arena for any new types it constructs. */
void sema_run(Node *tu, Arena *arena);

/* Run sema on a single subtree (typically an ND_FUNC_DEF cloned by
 * the template instantiation pass). The subtree is visited with the
 * standard visitor — function bodies push their param_scope, blocks
 * push their scope, etc. — so per-node identifier resolution and
 * post-substitution overload resolution work the same as during the
 * initial TU pass.
 *
 * Standard-mapping: this is N4659 §17.7.2 [temp.dep] / §17.7.3
 * [temp.nondep] phase-2 lookup applied to the substituted body.
 * Without this, calls inside a cloned template body that became
 * non-dependent after substitution (e.g. 'vec_alloc(new_vec, len)'
 * where new_vec gained a concrete type) never get their bare-ident
 * → ND_TEMPLATE_ID rewrite, and the instantiation pass doesn't pick
 * up the now-required nested instantiation. */
void sema_visit_node(Node *n, Arena *arena);

#endif
