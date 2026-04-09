/*
 * instantiate.h — Template instantiation pass.
 *
 * Called between sema and codegen. Walks the AST to find template-id
 * usage sites, clones template definitions with concrete type
 * substitutions, and prepends instantiated declarations to the
 * translation unit so codegen can emit them as ordinary classes
 * and functions.
 */

#ifndef TEMPLATE_INSTANTIATE_H
#define TEMPLATE_INSTANTIATE_H

#include "../parse/parse.h"

/*
 * Run the template instantiation pass over a translation unit.
 *
 * Phase 1: build a registry of template definitions (ND_TEMPLATE_DECL).
 * Phase 2: walk the AST collecting instantiation requests (Types with
 *          template_id_node set).
 * Phase 3: for each unique request, clone the template body with type
 *          substitution and prepend the result to tu->tu.decls[].
 *
 * After this pass, all template usages have concrete ND_CLASS_DEF /
 * ND_FUNC_DEF nodes in the TU that codegen can process normally.
 * The original ND_TEMPLATE_DECL nodes remain but are still skipped
 * by emit_top_level.
 */
void template_instantiate(Node *tu, Arena *arena);

#endif /* TEMPLATE_INSTANTIATE_H */
