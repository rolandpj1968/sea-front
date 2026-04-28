/*
 * clone.h — AST cloning with type substitution.
 *
 * Used by the template instantiation pass to produce concrete
 * class/function definitions from template definitions.
 */

#ifndef TEMPLATE_CLONE_H
#define TEMPLATE_CLONE_H

#include "../parse/parse.h"

/* Substitution map: maps template parameter names to concrete types,
 * and (for template-template parameters) to a class-template name token.
 *
 * A regular entry has concrete_type set; a TT-param entry has
 * concrete_type=NULL and tt_bound_name set to the token of the
 * bound class-template name. clone.c uses this to rewrite ND_QUALIFIED
 * parts[0] from the TT-param name to the bound name (e.g.
 * Allocator<X>::data_alloc → xcallocator<X>::data_alloc when
 * hash_table<D, xcallocator> is instantiated).
 *
 * N4659 §17.2/3 [temp.param] + §17.7.1 [temp.inst]. */
typedef struct {
    Token *param_name;
    Type  *concrete_type;
    Token *tt_bound_name;   /* non-NULL iff this is a TT-param binding */
} SubstEntry;

typedef struct {
    SubstEntry *entries;
    int         nentries;
    int         capacity;
    Arena      *arena;
} SubstMap;

SubstMap subst_map_new(Arena *arena, int capacity);
void     subst_map_add(SubstMap *m, Token *param_name, Type *concrete_type);
/* Bind a template-template parameter name to a concrete class-template
 * name. Used at class-instantiation time when the template's i-th param
 * is a TT-param and the i-th usage arg names a class template. */
void     subst_map_add_tt(SubstMap *m, Token *param_name, Token *bound_name);
/* Look up a TT-binding by name; returns the bound name token or NULL. */
Token   *subst_map_lookup_tt(SubstMap *m, const char *name, int len);

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
