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

#endif
