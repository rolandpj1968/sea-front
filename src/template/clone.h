/*
 * clone.h — AST cloning with type substitution.
 *
 * Used by the template instantiation pass to produce concrete
 * class/function definitions from template definitions.
 */

#ifndef TEMPLATE_CLONE_H
#define TEMPLATE_CLONE_H

#include "../parse/parse.h"

/* Substitution map: maps template parameter names to concrete types. */
typedef struct {
    Token *param_name;
    Type  *concrete_type;
} SubstEntry;

typedef struct {
    SubstEntry *entries;
    int         nentries;
    int         capacity;
    Arena      *arena;
} SubstMap;

SubstMap subst_map_new(Arena *arena, int capacity);
void     subst_map_add(SubstMap *m, Token *param_name, Type *concrete_type);

/*
 * Deep-copy a Type, replacing TY_DEPENDENT nodes whose tag matches
 * a SubstMap entry with the concrete type. Returns the original
 * pointer unchanged if no substitution was needed (avoids
 * unnecessary allocation).
 */
Type *subst_type(Type *ty, SubstMap *map, Arena *arena);

/*
 * Deep-copy a Node tree, applying subst_type to every Type* field
 * and recursively cloning child nodes. Tokens are shared (not
 * cloned) — they point into the original source buffer.
 */
Node *clone_node(Node *n, SubstMap *map, Arena *arena);

/*
 * Deduce template arguments for a function-template call —
 * N4659 §17.8.2.1 [temp.deduct.call]/1.
 *
 *   tmpl_func  = the inner ND_FUNC_DEF / ND_FUNC_DECL of the template
 *   arg_types  = concrete types of the call-site arguments
 *   nargs      = number of call-site arguments
 *   out        = SubstMap to populate (must be pre-allocated with
 *                enough capacity)
 *
 * Returns true if deduction produced at least one binding (i.e. the
 * pattern had some dependent param that unified with an arg); false
 * on pattern/arg shape mismatch.
 */
bool deduce_template_args(Node *tmpl_func, Type **arg_types, int nargs,
                          SubstMap *out);

#endif /* TEMPLATE_CLONE_H */
