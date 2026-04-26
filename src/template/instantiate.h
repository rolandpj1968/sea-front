/*
 * instantiate.h — Template instantiation pass.
 *
 * Called between sema and codegen. AST-level cloning + type
 * substitution: per N4659 §17.7.1 [temp.inst], finds template-id
 * usage sites and produces concrete ND_CLASS_DEF / ND_FUNC_DEF
 * nodes that downstream codegen can emit as ordinary classes and
 * functions.
 *
 * Covers (today):
 *   - Class templates, function templates, member templates
 *     (N4659 §17.5.2 [temp.mem]).
 *   - Partial and full specializations (§17.7.3 [temp.expl.spec],
 *     §17.7.4 [temp.spec.partial]).
 *   - Default template arguments (§17.1.2 [temp.param]).
 *   - Implicit argument deduction for function templates (§17.8.2
 *     [temp.deduct]) — limited to the patterns in clone.c's
 *     deduce_template_args.
 *   - Phase-2 re-sema of cloned bodies via sema_visit_node so
 *     names that became non-dependent after substitution get
 *     re-resolved (§17.7.2 [temp.dep] / §17.7.3 [temp.nondep]).
 *
 * Still missing or partial:
 *   - Variadic templates / parameter packs.
 *   - SFINAE.
 *   - Two-phase name lookup proper (we use Phase-2 re-sema as a
 *     pragmatic substitute).
 *
 * Per-iteration mechanics:
 *   Phase 1 — build a registry of every ND_TEMPLATE_DECL plus
 *             member-template entries (registry_add_member).
 *   Phase 2 — collect instantiation requests by walking tu->tu.decls
 *             (Type with template_id_node set, ND_QUALIFIED calls
 *             that resolve to a member template, etc.).
 *   Phase 3 — for each unique request, clone the inner declaration
 *             (or its OOL definition) with a SubstMap, run
 *             sema_visit_node on the cloned body, and INSERT the
 *             result into tu->tu.decls[] at a position that keeps
 *             struct-defs before function-bodies.
 *   The whole loop iterates until no new requests are produced —
 *   handles transitive dependencies (Outer<int> instantiates
 *   Box<int> instantiates Pair<int,int>...).
 */

#ifndef TEMPLATE_INSTANTIATE_H
#define TEMPLATE_INSTANTIATE_H

#include "../parse/parse.h"

/*
 * Run the template instantiation pass over a translation unit. See
 * the file header above for the per-iteration mechanics. After this
 * pass, all template usages have concrete ND_CLASS_DEF /
 * ND_FUNC_DEF nodes in the TU that codegen can process normally.
 * The original ND_TEMPLATE_DECL nodes remain but are skipped by
 * emit_top_level.
 */
void template_instantiate(Node *tu, Arena *arena);

#endif /* TEMPLATE_INSTANTIATE_H */
