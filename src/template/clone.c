/*
 * clone.c — AST cloning with type substitution for template instantiation.
 *
 * SubstMap maps template parameter names to concrete types.
 * clone_node deep-copies an AST subtree, applying subst_type to every
 * Type* field so that TY_DEPENDENT placeholders become concrete types.
 *
 * Clone strategy: arena_alloc zero-inits the target node, then we copy
 * the active union variant struct (`c->func = n->func`, etc.) to get
 * all scalar fields, then override pointer fields that need deep cloning
 * or type substitution. This avoids copying bytes from inactive union
 * variants — the rest of the node stays zero.
 */

#include <assert.h>

#include "clone.h"

/* ------------------------------------------------------------------ */
/* SubstMap                                                            */
/* ------------------------------------------------------------------ */

SubstMap subst_map_new(Arena *arena, int capacity) {
    return subst_map_new_with_registry(arena, capacity, NULL);
}

SubstMap subst_map_new_with_registry(Arena *arena, int capacity,
                                     TmplRegistry *reg) {
    SubstMap m = {0};
    m.entries = arena_alloc(arena, capacity * sizeof(SubstEntry));
    m.nentries = 0;
    m.capacity = capacity;
    m.arena = arena;
    m.registry = reg;
    return m;
}

void subst_map_add(SubstMap *m, Token *param_name, Type *concrete_type) {
    if (m->nentries >= m->capacity) return;  /* silently drop — shouldn't happen */
    m->entries[m->nentries].param_name = param_name;
    m->entries[m->nentries].concrete_type = concrete_type;
    m->entries[m->nentries].tt_bound_name = NULL;
    m->entries[m->nentries].pack_types = NULL;
    m->entries[m->nentries].pack_ntypes = 0;
    m->entries[m->nentries].is_pack = false;
    m->nentries++;
}

void subst_map_add_pack(SubstMap *m, Token *param_name,
                        Type **pack_types, int pack_ntypes) {
    if (m->nentries >= m->capacity) return;
    m->entries[m->nentries].param_name = param_name;
    m->entries[m->nentries].concrete_type = NULL;
    m->entries[m->nentries].tt_bound_name = NULL;
    m->entries[m->nentries].pack_types = pack_types;
    m->entries[m->nentries].pack_ntypes = pack_ntypes;
    m->entries[m->nentries].is_pack = true;
    m->nentries++;
}

SubstEntry *subst_map_lookup_pack(SubstMap *m, Token *name) {
    if (!name) return NULL;
    for (int i = 0; i < m->nentries; i++) {
        if (!m->entries[i].is_pack) continue;
        Token *pn = m->entries[i].param_name;
        if (pn && tokens_equal(pn, name))
            return &m->entries[i];
    }
    return NULL;
}

void subst_map_bind_args(SubstMap *m, Node **params, int nparams,
                         Node **args, int nargs) {
    int n = nparams < nargs ? nparams : nargs;
    for (int i = 0; i < n; i++) {
        Node *p = params ? params[i] : NULL;
        Node *a = args   ? args[i]   : NULL;
        if (!p || !p->param.name) continue;
        Type *aty = (a && a->kind == ND_VAR_DECL) ? a->var_decl.ty : NULL;
        if (aty) subst_map_add(m, p->param.name, aty);
    }
}

void subst_map_add_tt(SubstMap *m, Token *param_name, Token *bound_name) {
    if (m->nentries >= m->capacity) return;
    m->entries[m->nentries].param_name = param_name;
    m->entries[m->nentries].concrete_type = NULL;
    m->entries[m->nentries].tt_bound_name = bound_name;
    m->entries[m->nentries].pack_types = NULL;
    m->entries[m->nentries].pack_ntypes = 0;
    m->entries[m->nentries].is_pack = false;
    m->nentries++;
}

static Type *subst_map_lookup(SubstMap *map, const char *name, int len) {
    for (int i = 0; i < map->nentries; i++) {
        Token *pn = map->entries[i].param_name;
        /* Skip TT-only entries — concrete_type is NULL by design. */
        if (!map->entries[i].concrete_type) continue;
        if (pn && token_equals_str(pn, name, len))
            return map->entries[i].concrete_type;
    }
    return NULL;
}

Token *subst_map_lookup_tt(SubstMap *map, const char *name, int len) {
    for (int i = 0; i < map->nentries; i++) {
        if (!map->entries[i].tt_bound_name) continue;
        Token *pn = map->entries[i].param_name;
        if (pn && token_equals_str(pn, name, len))
            return map->entries[i].tt_bound_name;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Type substitution                                                   */
/* ------------------------------------------------------------------ */

/* True if ty mentions a TY_DEPENDENT anywhere in its structure
 * (under PTR/REF/ARRAY/FUNC/template_id args). Used by subst_type to
 * decide whether to recurse on a class-template arg type, and by
 * deduce_from_pair to detect that a non-matching A means failure
 * (rather than silent success) when P is a dependent compound. */
bool type_has_dependent(Type *ty) {
    if (!ty) return false;
    if (ty->kind == TY_DEPENDENT) return true;
    switch (ty->kind) {
    case TY_PTR: case TY_REF: case TY_RVALREF: case TY_ARRAY:
        return type_has_dependent(ty->base);
    case TY_FUNC:
        if (type_has_dependent(ty->ret)) return true;
        for (int i = 0; i < ty->nparams; i++)
            if (type_has_dependent(ty->params[i])) return true;
        return false;
    case TY_STRUCT: case TY_UNION:
        if (ty->template_id_node &&
            ty->template_id_node->kind == ND_TEMPLATE_ID) {
            Node *tid = ty->template_id_node;
            for (int i = 0; i < tid->template_id.nargs; i++) {
                Node *a = tid->template_id.args[i];
                Type *at = (a && a->kind == ND_VAR_DECL) ? a->var_decl.ty : NULL;
                if (at && type_has_dependent(at)) return true;
            }
        }
        return false;
    default:
        return false;
    }
}

/* Resolve a dependent member-typedef 'concrete::member' once 'concrete'
 * is a substitution result. Two strategies tried in order:
 *  1. Fast path: concrete already has its class_region populated (its
 *     instantiation was emitted earlier) — look up member directly.
 *  2. Fallback: concrete is an un-emitted template-id — walk its
 *     source template's class body for the typedef, then recursively
 *     substitute its target type with concrete's template_args.
 * Returns NULL if neither path resolves. N4659 §17.7.1 [temp.inst]. */
static Type *resolve_dep_member(Type *concrete, Token *member,
                                 SubstMap *map, Arena *arena) {
    if (!concrete || !member) return NULL;
    if (concrete->class_region) {
        Declaration *md = lookup_in_scope(concrete->class_region,
            member->loc, member->len);
        if (md && md->type) return md->type;
    }
    if (!concrete->class_region && concrete->template_id_node &&
        concrete->template_id_node->kind == ND_TEMPLATE_ID) {
        Node *tid = concrete->template_id_node;
        Node *tmpl = tid->template_id.resolved_tmpl;
        if (!tmpl && tid->template_id.name && map->registry)
            tmpl = registry_lookup_class_template(map->registry,
                                            tid->template_id.name->loc,
                                            tid->template_id.name->len);
        if (tmpl && tmpl->kind == ND_TEMPLATE_DECL &&
            tmpl->template_decl.decl &&
            tmpl->template_decl.decl->kind == ND_CLASS_DEF) {
            Node *cls = tmpl->template_decl.decl;
            Node *typedef_node = NULL;
            for (int i = 0; i < cls->class_def.nmembers; i++) {
                Node *m = cls->class_def.members[i];
                if (!m || m->kind != ND_TYPEDEF) continue;
                if (!m->var_decl.name) continue;
                if (tokens_equal(m->var_decl.name, member)) {
                    typedef_node = m; break;
                }
            }
            if (typedef_node && typedef_node->var_decl.ty) {
                int np = tmpl->template_decl.nparams;
                int na = concrete->n_template_args;
                SubstMap inner = subst_map_new_with_registry(arena,
                    np > 0 ? np : 1, map->registry);
                for (int i = 0; i < np && i < na; i++) {
                    Node *param = tmpl->template_decl.params[i];
                    if (!param || !param->param.name) continue;
                    if (concrete->template_args[i])
                        subst_map_add(&inner, param->param.name,
                                      concrete->template_args[i]);
                }
                return subst_type(typedef_node->var_decl.ty,
                                  &inner, arena);
            }
        }
    }
    return NULL;
}

Type *subst_type(Type *ty, SubstMap *map, Arena *arena) {
    if (!ty) return NULL;

    /* TY_DEPENDENT with dep_base: the qualifier was a template-id with
     * dependent args (e.g. 'typename _Alloc_traits::difference_type'
     * where _Alloc_traits = AllocTraits<Alloc>). Recursively substitute
     * into the underlying type so its template args become concrete,
     * then resolve dep_member against the result. Distinct from the
     * tag-based path below — dep_base is the typedef'd underlying
     * type, not a directly-mapped template-param name. */
    if (ty->kind == TY_DEPENDENT && ty->dep_base && ty->dep_member) {
        Type *base_concrete = subst_type(ty->dep_base, map, arena);
        Type *r = resolve_dep_member(base_concrete, ty->dep_member, map, arena);
        if (r) return r;
        return ty;  /* couldn't resolve — leave dependent */
    }

    /* TY_DEPENDENT → substitute if the name matches a map entry */
    if (ty->kind == TY_DEPENDENT && ty->tag) {
        Type *concrete = subst_map_lookup(map, ty->tag->loc, ty->tag->len);
        if (!concrete) {
            /* No match — leave as-is (still dependent for outer template) */
            return ty;
        }
        /* Qualified dependent name 'typename T::member': resolve via
         * the shared helper (fast path through concrete's class_region,
         * fallback through the source template's class body). */
        if (ty->dep_member) {
            Type *r = resolve_dep_member(concrete, ty->dep_member,
                                          map, arena);
            if (r) return r;
        }
        return concrete;
    }

    /* TY_STRUCT / TY_UNION with a template_id_node: substitute args
     * in the template-id so transitive instantiation can pick them up.
     * E.g. inside Box<T>, a member 'Pair<T,T> p' has template_id_node
     * with T args — those need to become concrete. */
    if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) &&
        ty->template_id_node &&
        ty->template_id_node->kind == ND_TEMPLATE_ID) {
        Node *tid = ty->template_id_node;

        /* N4659 §13.8.3 [temp.dep.type]: check if any template-id arg
         * type contains a dependent name anywhere in its structure —
         * not just at the top level. 'vec<T*, A, vl_embed>' has args[0]
         * = TY_PTR(TY_DEPENDENT(T)), which needs substitution even
         * though the outermost kind isn't TY_DEPENDENT. */
        bool needs_subst = false;
        for (int i = 0; i < tid->template_id.nargs; i++) {
            Node *a = tid->template_id.args[i];
            Type *at = (a && a->kind == ND_VAR_DECL) ? a->var_decl.ty : NULL;
            if (at && type_has_dependent(at)) { needs_subst = true; break; }
        }

        if (needs_subst) {
            Type *copy = arena_alloc(arena, sizeof(Type));
            *copy = *ty;
            Node *tid_copy = arena_alloc(arena, sizeof(Node));
            *tid_copy = *tid;
            tid_copy->template_id.args = arena_alloc(arena,
                tid->template_id.nargs * sizeof(Node *));
            for (int i = 0; i < tid->template_id.nargs; i++) {
                Node *a = tid->template_id.args[i];
                Type *at = (a && a->kind == ND_VAR_DECL) ? a->var_decl.ty : NULL;
                if (at && type_has_dependent(at)) {
                    Type *sub = subst_type(at, map, arena);
                    Node *ac = arena_alloc(arena, sizeof(Node));
                    *ac = *a;
                    ac->var_decl.ty = sub;
                    tid_copy->template_id.args[i] = ac;
                } else {
                    tid_copy->template_id.args[i] = a;
                }
            }
            copy->template_id_node = tid_copy;
            /* Populate template_args from the substituted template_id
             * so mangling can access them directly. Without this, the
             * mangled name omits template args (producing just 'sf__vec'
             * instead of 'sf__vec_t_int_va_heap_vl_ptr_te_'). */
            int na = tid_copy->template_id.nargs;
            copy->template_args = arena_alloc(arena, na * sizeof(Type *));
            copy->n_template_args = na;
            for (int i = 0; i < na; i++) {
                Node *a = tid_copy->template_id.args[i];
                copy->template_args[i] = (a && a->kind == ND_VAR_DECL)
                    ? a->var_decl.ty : NULL;
            }
            return copy;
        }
    }

    /* Injected-class-name substitution: a TY_STRUCT whose tag matches
     * a SubstMap entry is the template class itself (e.g. sizeof(Box)
     * inside Box<T>'s body). Replace with the concrete type. */
    if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) &&
        ty->tag && !ty->template_id_node) {
        Type *concrete = subst_map_lookup(map, ty->tag->loc, ty->tag->len);
        if (concrete) return concrete;
    }

    /* TY_STRUCT/UNION with template_args but no template_id_node:
     * the substituted version of an instantiated template-id (e.g.
     * the return type of a method built up by sema rather than
     * carrying a synthesised template-id node). The tag-emit path
     * walks template_args directly, so dependent args here surface
     * as 'sf__vec_t_T_A_vl_embed_te_' in the emitted C. Substitute
     * each arg so the tag prints with concrete types. Pattern: gcc
     * 4.8 vec.h, where 'vec<T, A, vl_ptr>::copy()' returns a
     * 'vec<T, A, vl_ptr>' value whose template_args carry T/A. */
    if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) &&
        !ty->template_id_node && ty->n_template_args > 0) {
        bool needs_subst = false;
        for (int i = 0; i < ty->n_template_args; i++) {
            if (type_has_dependent(ty->template_args[i])) {
                needs_subst = true; break;
            }
        }
        if (needs_subst) {
            Type *copy = arena_alloc(arena, sizeof(Type));
            *copy = *ty;
            copy->template_args = arena_alloc(arena,
                ty->n_template_args * sizeof(Type *));
            for (int i = 0; i < ty->n_template_args; i++)
                copy->template_args[i] = subst_type(ty->template_args[i],
                                                     map, arena);
            return copy;
        }
    }

    /* Recurse into compound types */
    switch (ty->kind) {
    case TY_PTR: case TY_REF: case TY_RVALREF: {
        Type *sub = subst_type(ty->base, map, arena);
        if (sub == ty->base) return ty;  /* no change */
        Type *copy = arena_alloc(arena, sizeof(Type));
        *copy = *ty;
        copy->base = sub;
        return copy;
    }
    case TY_ARRAY: {
        Type *sub = subst_type(ty->base, map, arena);
        /* The array bound itself may be a dependent expression
         * (NTTP used as size: 'T arr[NUM]' where NUM is a non-type
         * template parameter). clone_node walks the expression tree
         * and substitutes ND_IDENT 'NUM' → its bound literal value
         * via the TT-binding machinery (see ND_IDENT case in
         * clone_node). N4659 §11.3.4/1 [dcl.array]. Without this,
         * the cloned array type still references the unsubstituted
         * NTTP name and the emitted C has 'T arr[NUM_EMBEDDED]'
         * with NUM_EMBEDDED undeclared at TU scope. */
        Node *sub_size = ty->array_size_expr
                         ? clone_node(ty->array_size_expr, map, arena)
                         : NULL;
        if (sub == ty->base && sub_size == ty->array_size_expr) return ty;
        Type *copy = arena_alloc(arena, sizeof(Type));
        *copy = *ty;
        copy->base = sub;
        copy->array_size_expr = sub_size;
        return copy;
    }
    case TY_FUNC: {
        Type *ret = subst_type(ty->ret, map, arena);
        bool changed = (ret != ty->ret);
        Type **params = ty->params;
        if (ty->nparams > 0) {
            params = arena_alloc(arena, ty->nparams * sizeof(Type *));
            for (int i = 0; i < ty->nparams; i++) {
                params[i] = subst_type(ty->params[i], map, arena);
                if (params[i] != ty->params[i]) changed = true;
            }
        }
        if (!changed) return ty;
        Type *copy = arena_alloc(arena, sizeof(Type));
        *copy = *ty;
        copy->ret = ret;
        copy->params = params;
        return copy;
    }
    default:
        return ty;
    }
}

/* ------------------------------------------------------------------ */
/* Node array cloning                                                  */
/* ------------------------------------------------------------------ */

static Node **clone_node_array(Node **arr, int n, SubstMap *map, Arena *arena) {
    assert(n >= 0);
    if (n == 0) return NULL;
    assert(arr != NULL);
    Node **out = arena_alloc(arena, n * sizeof(Node *));
    for (int i = 0; i < n; i++)
        out[i] = clone_node(arr[i], map, arena);
    return out;
}

/* Single-pack lookup: if the SubstMap has exactly one pack
 * binding, return it; else NULL. Used by the cloner's pack
 * expansion when an expression marked `is_pack_expand` doesn't
 * directly carry its pack name (e.g. bare `args...` in a call
 * arg list — sea-front doesn't track the pack name on the
 * marker). Works for the common single-pack template (`template
 * <class... Ts>`); multi-pack templates need a more precise
 * lookup but they're vanishingly rare. */
static SubstEntry *subst_map_single_pack(SubstMap *map) {
    SubstEntry *only = NULL;
    for (int i = 0; i < map->nentries; i++) {
        if (!map->entries[i].is_pack) continue;
        if (only) return NULL;  /* multiple packs — caller must disambiguate */
        only = &map->entries[i];
    }
    return only;
}

/* Synth a Token for `<base>_<idx>` (e.g. `args_0`, `args_1`).
 * The buffer is one-shot malloc'd so the token outlives the
 * clone phase. */
static Token *synth_pack_name(Token *base, int idx, Arena *arena) {
    char *buf = arena_alloc(arena, base->len + 16);
    int len = snprintf(buf, base->len + 16, "%.*s_%d",
                       base->len, base->loc, idx);
    Token *t = arena_alloc(arena, sizeof(Token));
    memset(t, 0, sizeof(*t));
    t->kind = TK_IDENT;
    t->loc  = buf;
    t->len  = len;
    t->line = base->line;
    t->col  = base->col;
    t->file = base->file;
    return t;
}

/* Pack-aware array clone: handles pack-expansion sites by
 * replicating an input node N times.
 *
 *   - ND_PARAM with `param.is_pack` AND `param.ty` is TY_DEPENDENT
 *     bound to a pack: emit N params, each with the i-th
 *     concrete type and name `<orig>_<i>`.
 *   - Any expression node with `is_pack_expand`: emit N clones,
 *     replacing the pack-named ident with `<orig>_<i>`.
 *   - Other nodes: clone once.
 *
 * Out-count is written through `*out_n`. */
static Node **clone_node_array_pack(Node **arr, int n, SubstMap *map,
                                     Arena *arena, int *out_n) {
    assert(n >= 0);
    if (n == 0) { *out_n = 0; return NULL; }
    assert(arr != NULL);
    /* First pass: compute output count. */
    int total = 0;
    for (int i = 0; i < n; i++) {
        Node *src = arr[i];
        if (!src) { total++; continue; }
        bool is_pack_param = (src->kind == ND_PARAM && src->param.is_pack);
        bool is_pack_expr  = src->is_pack_expand;
        if (is_pack_param || is_pack_expr) {
            SubstEntry *pe = NULL;
            if (is_pack_param && src->param.ty &&
                src->param.ty->kind == TY_DEPENDENT)
                pe = subst_map_lookup_pack(map, src->param.ty->tag);
            if (!pe) pe = subst_map_single_pack(map);
            if (pe) { total += pe->pack_ntypes; continue; }
        }
        total++;
    }
    Node **out = arena_alloc(arena, total * sizeof(Node *));
    int oi = 0;
    for (int i = 0; i < n; i++) {
        Node *src = arr[i];
        if (!src) { out[oi++] = NULL; continue; }
        bool is_pack_param = (src->kind == ND_PARAM && src->param.is_pack);
        bool is_pack_expr  = src->is_pack_expand;
        if (is_pack_param || is_pack_expr) {
            SubstEntry *pe = NULL;
            if (is_pack_param && src->param.ty &&
                src->param.ty->kind == TY_DEPENDENT)
                pe = subst_map_lookup_pack(map, src->param.ty->tag);
            if (!pe) pe = subst_map_single_pack(map);
            if (pe) {
                for (int j = 0; j < pe->pack_ntypes; j++) {
                    Node *c = arena_alloc(arena, sizeof(Node));
                    memcpy(c, src, sizeof(Node));
                    c->is_pack_expand = false;
                    if (is_pack_param) {
                        c->kind = ND_PARAM;
                        c->param.is_pack = false;
                        c->param.ty = pe->pack_types[j];
                        if (src->param.name)
                            c->param.name = synth_pack_name(src->param.name,
                                                             j, arena);
                    } else {
                        /* Clone the expression first, then if it's an
                         * ND_IDENT naming the pack, rewrite the name
                         * to <orig>_<i>. Other shapes (e.g. expr that
                         * contains the pack ident) are not yet handled
                         * — they'd need pack-aware deep rewrite. */
                        c = clone_node(src, map, arena);
                        if (c && c->kind == ND_IDENT && c->ident.name)
                            c->ident.name = synth_pack_name(c->ident.name,
                                                             j, arena);
                        c->is_pack_expand = false;
                    }
                    out[oi++] = c;
                }
                continue;
            }
        }
        out[oi++] = clone_node(src, map, arena);
    }
    *out_n = total;
    return out;
}

static MemInit *clone_mem_inits(MemInit *inits, int n,
                                 SubstMap *map, Arena *arena) {
    assert(n >= 0);
    if (n == 0) return NULL;
    assert(inits != NULL);
    MemInit *copy = arena_alloc(arena, n * sizeof(MemInit));
    for (int i = 0; i < n; i++) {
        copy[i].name = inits[i].name;
        copy[i].args = clone_node_array(inits[i].args, inits[i].nargs,
                                         map, arena);
        copy[i].nargs = inits[i].nargs;
    }
    return copy;
}

/* ------------------------------------------------------------------ */
/* Node cloning                                                        */
/* ------------------------------------------------------------------ */

/*
 * Deep-copy an AST node, applying type substitution via SubstMap.
 *
 * The target node is arena-allocated (zero-init). We copy only the
 * common fields (kind, tok) and the active union variant's struct,
 * then override pointer fields that need deep cloning or type
 * substitution. Inactive union variants stay zero — no stale bytes.
 */
Node *clone_node(Node *n, SubstMap *map, Arena *arena) {
    if (!n) return NULL;

    Node *c = arena_alloc(arena, sizeof(Node));
    c->kind = n->kind;
    c->tok = n->tok;

    switch (n->kind) {
    /* -- Leaf expression nodes (no child pointers or types) -- */
    case ND_NUM:
        c->num = n->num;
        break;
    case ND_FNUM:
        c->fnum = n->fnum;
        break;
    case ND_STR:
        c->str = n->str;
        break;
    case ND_CHAR:
        c->chr = n->chr;
        break;
    case ND_BOOL_LIT:
    case ND_NULLPTR:
    case ND_NULL_STMT:
    case ND_BREAK:
    case ND_CONTINUE:
        /* No variant data — just kind + tok */
        break;
    case ND_GOTO:
        c->goto_ = n->goto_;
        break;
    case ND_ACCESS_SPEC:
        c->access_spec = n->access_spec;
        break;

    case ND_IDENT:
        c->ident = n->ident;
        /* Substitute the sema-recorded resolved_type so the cloned
         * ident carries the instantiation-specific type instead of
         * the source template's TY_DEPENDENT shape. Sema's visit_
         * ident treats a non-NULL resolved_type as "already typed"
         * and preserves it instead of re-binding from the source
         * Declaration — without the substitution, the cloned ident
         * would inherit the source param's un-substituted type and
         * downstream member-access type resolution on it (vec_ of
         * 'vec<T,va_stack,vl_ptr>' rather than
         * 'vec<loop*,va_stack,vl_ptr>') would leak TY_DEPENDENT
         * into the receiver-class mangle. Real-world shape: gcc
         * 4.8 vec.h's 'va_stack::alloc<T>' body referencing
         * 'v.vec_->embedded_init(...)'. N4659 §17.7.1 [temp.inst].
         *
         * Restricted to types that ACTUALLY contained a template
         * param. Non-dependent source types (e.g. a lambda's
         * concrete closure tag) get re-resolved by sema against the
         * fresh per-instantiation Declaration registered for the
         * cloned local — pinning the source resolved_type would
         * mask the cloned lambda's suffixed name and emit the
         * wrong call symbol. */
        if (n->resolved_type && type_has_dependent(n->resolved_type))
            c->resolved_type = subst_type(n->resolved_type, map, arena);
        /* Non-type template parameter substitution: when this ident
         * names an NTTP whose binding was recorded via the TT-param
         * slot (the slot is reused for any "name → Token" binding;
         * TT-param uses appear in ND_QUALIFIED parts[0], so they
         * never collide with bare-ident NTTP uses here). N4659
         * §17.1/4 [temp.param] + §17.7.1 [temp.inst]. */
        if (c->ident.name) {
            Token *bound = subst_map_lookup_tt(map,
                c->ident.name->loc, c->ident.name->len);
            if (bound) {
                /* Literal-valued NTTPs ('integral_constant<bool,true>'):
                 * the bound token's kind reveals what kind of value
                 * was passed. Morph the cloned node into the matching
                 * literal NodeKind so downstream sema/emit treats it
                 * as a constant, not as an ident lookup that would
                 * fail (e.g. resolving 'V' → 'true' as ND_IDENT would
                 * try to look up 'true' as a name). */
                NodeKind lit_kind = ND_IDENT;
                switch (bound->kind) {
                case TK_NUM:        lit_kind = ND_NUM;      break;
                case TK_FNUM:       lit_kind = ND_FNUM;     break;
                case TK_KW_TRUE:
                case TK_KW_FALSE:   lit_kind = ND_BOOL_LIT; break;
                case TK_KW_NULLPTR: lit_kind = ND_NULLPTR;  break;
                case TK_CHAR:       lit_kind = ND_CHAR;     break;
                case TK_STR:        lit_kind = ND_STR;      break;
                default: break;
                }
                if (lit_kind != ND_IDENT) {
                    c->kind = lit_kind;
                    c->tok = bound;
                    /* Variant-data shapes for literals: most read
                     * c->tok directly (NUM/BOOL/NULLPTR). CHAR/STR
                     * keep the token in their own variant struct,
                     * so populate that too. FNUM stores a parsed
                     * double we don't compute here — emit_c falls
                     * back to '%g' on fnum.fval=0 which is wrong,
                     * but float NTTPs are vanishingly rare so the
                     * correct path is gated until we hit one. */
                    if (lit_kind == ND_CHAR)
                        c->chr.tok = bound;
                    else if (lit_kind == ND_STR) {
                        c->str.tok = bound;
                        c->str.ntoks = 1;
                    }
                } else {
                    c->ident.name = bound;
                    /* Clear cached resolution — the rewritten name maps
                     * to a different decl which sema will re-resolve. */
                    c->ident.resolved_decl = NULL;
                    c->resolved_type = NULL;
                }
            }
        }
        break;

    case ND_QUALIFIED:
        c->qualified = n->qualified;
        if (n->qualified.parts && n->qualified.nparts > 0) {
            c->qualified.parts = arena_alloc(arena,
                n->qualified.nparts * sizeof(Token *));
            memcpy(c->qualified.parts, n->qualified.parts,
                   n->qualified.nparts * sizeof(Token *));
            /* N4659 §17.7.1 [temp.inst]/1 — Phase 2 qualified lookup:
             * substitute the leading qualifier via SubstMap, then
             * attempt qualified lookup in the concrete class_region
             * to resolve the member and set resolved_type. If the
             * class_region doesn't have the member (e.g. it's a
             * member template), fall back to token-level swap. */
            Token *lead = c->qualified.parts[0];
            if (lead) {
                Type *sub = subst_map_lookup(map, lead->loc, lead->len);
                if (sub) {
                    /* Replace the leading token with the concrete type's
                     * tag. ALSO carry the substituted Type as
                     * resolved_class_type so codegen mangles through
                     * mangle_class_tag — preserving the template args
                     * (e.g. Descriptor → pointer_hash<gimple_statement_d>
                     * keeps the <gimple_statement_d> in the symbol).
                     * Without this, the call mangles as bare
                     * 'sf__pointer_hash__hash_*' and the def lives at
                     * 'sf__pointer_hash_t_gimple_statement_d_te___hash_*'.
                     * N4659 §17.7.1 [temp.inst]. */
                    if (sub->tag) c->qualified.parts[0] = sub->tag;
                    if (sub->n_template_args > 0 && !c->qualified.resolved_class_type)
                        c->qualified.resolved_class_type = sub;
                    /* Phase 2: look up the member in the concrete class.
                     * N4659 §6.4.3 [basic.lookup.qual] — qualified name
                     * lookup in the named class. */
                    if (sub->class_region && c->qualified.nparts >= 2) {
                        Token *member = c->qualified.parts[c->qualified.nparts - 1];
                        if (member) {
                            Declaration *md = lookup_in_scope(
                                sub->class_region, member->loc, member->len);
                            if (md && md->type) {
                                /* Substitute through the lookup result —
                                 * the Declaration's type may carry
                                 * TY_DEPENDENT params (the un-instantiated
                                 * template body's signature). Without
                                 * subst_type, codegen mangles the call
                                 * with literal 'Type' tags instead of
                                 * the deduced concrete arg. N4659
                                 * §17.7.1 [temp.inst]. */
                                c->resolved_type = subst_type(md->type, map, arena);
                            }
                        }
                    }
                } else {
                    /* Template-template parameter binding: when the
                     * cloned class template's body has a call like
                     * Allocator<value_type>::data_alloc(...), and
                     * Allocator was bound to xcallocator at instantiation
                     * time, rewrite parts[0] to the bound name so the
                     * call mangles as sf__xcallocator_t_..._te___data_alloc_*
                     * matching the actual definition. Also rewrite the
                     * lead_tid's template-id name so the collection pass
                     * picks up the bound class template (xcallocator<X>)
                     * for instantiation. N4659 §17.2/3 [temp.param] +
                     * §17.7.1 [temp.inst]. */
                    Token *bound = subst_map_lookup_tt(map, lead->loc, lead->len);
                    if (bound) {
                        c->qualified.parts[0] = bound;
                        if (n->qualified.lead_tid &&
                            n->qualified.lead_tid->kind == ND_TEMPLATE_ID &&
                            n->qualified.lead_tid->template_id.name &&
                            n->qualified.lead_tid->template_id.name->len ==
                                lead->len &&
                            memcmp(n->qualified.lead_tid->template_id.name->loc,
                                   lead->loc, lead->len) == 0) {
                            /* lead_tid's name matches the bound TT-param;
                             * the cloned lead_tid below will pick this up
                             * via the rewrite hook in ND_TEMPLATE_ID. */
                        }
                    }
                }
            }
        }
        /* Clone lead_tid with type substitution so template args
         * like is_a_helper<T>::test get T→concrete_type. */
        if (n->qualified.lead_tid) {
            c->qualified.lead_tid = clone_node(n->qualified.lead_tid,
                                                map, arena);
        }
        break;

    /* -- Binary expressions -- */
    case ND_BINARY:
    case ND_ASSIGN:
    case ND_COMMA:
        c->binary = n->binary;
        c->binary.lhs = clone_node(n->binary.lhs, map, arena);
        c->binary.rhs = clone_node(n->binary.rhs, map, arena);
        break;

    /* -- Unary expressions -- */
    case ND_UNARY:
    case ND_POSTFIX:
        c->unary = n->unary;
        c->unary.operand = clone_node(n->unary.operand, map, arena);
        break;

    case ND_TERNARY:
        c->ternary = n->ternary;
        c->ternary.cond  = clone_node(n->ternary.cond, map, arena);
        c->ternary.then_ = clone_node(n->ternary.then_, map, arena);
        c->ternary.else_ = clone_node(n->ternary.else_, map, arena);
        break;

    case ND_CALL: {
        c->call = n->call;
        c->call.callee = clone_node(n->call.callee, map, arena);
        int new_nargs = 0;
        c->call.args = clone_node_array_pack(n->call.args, n->call.nargs,
                                              map, arena, &new_nargs);
        c->call.nargs = new_nargs;
        break;
    }

    case ND_MEMBER:
        c->member = n->member;
        c->member.obj = clone_node(n->member.obj, map, arena);
        /* Clone any explicit template-id on the member name so its
         * type/value args get substituted alongside the rest of the
         * cloned expression. N4659 §17.2 [temp.names] +
         * §17.7.1 [temp.inst]. */
        if (n->member.template_id)
            c->member.template_id = clone_node(n->member.template_id,
                                               map, arena);
        break;

    case ND_SUBSCRIPT:
        c->subscript = n->subscript;
        c->subscript.base  = clone_node(n->subscript.base, map, arena);
        c->subscript.index = clone_node(n->subscript.index, map, arena);
        break;

    case ND_CAST:
        c->cast = n->cast;
        c->cast.ty      = subst_type(n->cast.ty, map, arena);
        c->cast.operand = clone_node(n->cast.operand, map, arena);
        /* New-expression initializer / placement args may contain
         * pack-expansion sites (`new T(args...)`); route through
         * clone_node_array_pack so `args...` expands to the bound
         * pack at instantiation. Without this the args list keeps
         * the template-body pack-ident verbatim and the cloned cast
         * still has new_ctor_nargs==1 after empty-pack binding.
         * N4659 §17.5.3.4 [temp.variadic.expand]. */
        if (n->cast.new_ctor_args && n->cast.new_ctor_nargs > 0) {
            int new_n = 0;
            c->cast.new_ctor_args = clone_node_array_pack(
                n->cast.new_ctor_args, n->cast.new_ctor_nargs,
                map, arena, &new_n);
            c->cast.new_ctor_nargs = new_n;
        }
        if (n->cast.new_placement_args && n->cast.new_placement_nargs > 0) {
            int new_n = 0;
            c->cast.new_placement_args = clone_node_array_pack(
                n->cast.new_placement_args, n->cast.new_placement_nargs,
                map, arena, &new_n);
            c->cast.new_placement_nargs = new_n;
        }
        break;

    case ND_SIZEOF:
        c->sizeof_ = n->sizeof_;
        c->sizeof_.expr = clone_node(n->sizeof_.expr, map, arena);
        c->sizeof_.ty   = subst_type(n->sizeof_.ty, map, arena);
        /* `sizeof...(pack)` collapses to the integer literal N
         * (count of types the pack binds to). N4659 §8.3.3/5
         * [expr.sizeof]. */
        if (n->sizeof_.is_pack && n->sizeof_.pack_name) {
            SubstEntry *pe = subst_map_lookup_pack(map, n->sizeof_.pack_name);
            if (!pe) pe = subst_map_single_pack(map);
            if (pe) {
                c->kind = ND_NUM;
                c->num.lo = (uint64_t)pe->pack_ntypes;
                c->num.hi = 0;
                c->num.is_signed = true;
                /* Drop the source `sizeof` token so ND_NUM's emit
                 * uses the synth integer format (otherwise it
                 * writes the verbatim source text). */
                c->tok = NULL;
            }
        }
        break;

    case ND_ALIGNOF:
        c->alignof_ = n->alignof_;
        c->alignof_.ty = subst_type(n->alignof_.ty, map, arena);
        break;

    case ND_OFFSETOF:
        c->offsetof_ = n->offsetof_;
        c->offsetof_.ty = subst_type(n->offsetof_.ty, map, arena);
        break;

    case ND_INIT_LIST:
        c->init_list = n->init_list;
        c->init_list.elems = clone_node_array(n->init_list.elems,
                                              n->init_list.nelems,
                                              map, arena);
        break;

    /* -- Statements -- */
    case ND_BLOCK:
        c->block = n->block;
        c->block.stmts = clone_node_array(n->block.stmts, n->block.nstmts,
                                           map, arena);
        c->block.scope = NULL;  /* sema re-creates */
        break;

    case ND_RETURN:
        c->ret = n->ret;
        c->ret.expr = clone_node(n->ret.expr, map, arena);
        break;

    case ND_EXPR_STMT:
        c->expr_stmt = n->expr_stmt;
        c->expr_stmt.expr = clone_node(n->expr_stmt.expr, map, arena);
        break;

    case ND_IF:
        c->if_ = n->if_;
        c->if_.init  = clone_node(n->if_.init, map, arena);
        c->if_.cond  = clone_node(n->if_.cond, map, arena);
        c->if_.then_ = clone_node(n->if_.then_, map, arena);
        c->if_.else_ = clone_node(n->if_.else_, map, arena);
        break;

    case ND_WHILE:
        c->while_ = n->while_;
        c->while_.cond = clone_node(n->while_.cond, map, arena);
        c->while_.body = clone_node(n->while_.body, map, arena);
        break;

    case ND_DO:
        c->do_ = n->do_;
        c->do_.cond = clone_node(n->do_.cond, map, arena);
        c->do_.body = clone_node(n->do_.body, map, arena);
        break;

    case ND_FOR:
        c->for_ = n->for_;
        c->for_.init = clone_node(n->for_.init, map, arena);
        c->for_.cond = clone_node(n->for_.cond, map, arena);
        c->for_.inc  = clone_node(n->for_.inc, map, arena);
        c->for_.body = clone_node(n->for_.body, map, arena);
        break;

    case ND_SWITCH:
        c->switch_ = n->switch_;
        c->switch_.init = clone_node(n->switch_.init, map, arena);
        c->switch_.expr = clone_node(n->switch_.expr, map, arena);
        c->switch_.body = clone_node(n->switch_.body, map, arena);
        break;

    case ND_CASE:
        c->case_ = n->case_;
        c->case_.expr = clone_node(n->case_.expr, map, arena);
        c->case_.stmt = clone_node(n->case_.stmt, map, arena);
        break;

    case ND_DEFAULT:
        c->default_ = n->default_;
        c->default_.stmt = clone_node(n->default_.stmt, map, arena);
        break;

    case ND_LABEL:
        c->label = n->label;
        c->label.stmt = clone_node(n->label.stmt, map, arena);
        break;

    /* -- Declarations -- */
    case ND_VAR_DECL:
    case ND_TYPEDEF:
        c->var_decl = n->var_decl;
        c->var_decl.ty   = subst_type(n->var_decl.ty, map, arena);
        c->var_decl.init = clone_node(n->var_decl.init, map, arena);
        /* Direct-init declarator `T name(args...)` can contain a
         * pack-expansion site that must expand to the bound pack at
         * instantiation. Pattern: g++.dg/cpp0x/variadic-new2.C
         * `int y(args...);` with empty pack instantiates to
         * `int y();` (value-init → 0). N4659 §17.5.3.4
         * [temp.variadic.expand]. */
        if (n->var_decl.ctor_args && n->var_decl.ctor_nargs > 0) {
            int new_n = 0;
            c->var_decl.ctor_args = clone_node_array_pack(
                n->var_decl.ctor_args, n->var_decl.ctor_nargs,
                map, arena, &new_n);
            c->var_decl.ctor_nargs = new_n;
        } else {
            c->var_decl.ctor_args = NULL;
        }
        /* If the init was an ND_LAMBDA (capturing), the var's type
         * was set at parse time to the original (template-context)
         * closure TY_STRUCT. Cloning the lambda produced a fresh
         * closure type with substituted member types and a unique
         * tag — subst_type can't see that relationship since it
         * walks a Type tree, not the Node graph. Re-bind the var
         * to the cloned closure so its declared type matches the
         * compound-literal init. */
        if (c->var_decl.init && c->var_decl.init->kind == ND_LAMBDA &&
            c->var_decl.init->lambda.closure_type)
            c->var_decl.ty = c->var_decl.init->lambda.closure_type;
        break;

    case ND_FUNC_DEF:
    case ND_FUNC_DECL: {
        c->func = n->func;
        c->func.ret_ty = subst_type(n->func.ret_ty, map, arena);
        int new_nparams = 0;
        c->func.params = clone_node_array_pack(n->func.params, n->func.nparams,
                                                map, arena, &new_nparams);
        c->func.nparams = new_nparams;
        c->func.body   = clone_node(n->func.body, map, arena);
        c->func.mem_inits = clone_mem_inits(n->func.mem_inits,
                                             n->func.n_mem_inits,
                                             map, arena);
        c->func.param_scope = NULL;  /* sema re-creates */
        c->func.deferred_class_region = NULL;
        c->func.body_start_pos = -1;  /* not deferred — body is cloned */
        c->func.body_end_pos   = -1;
        /* Template instantiations are conceptually inline (N4659
         * §17.5.1/4 [temp.spec.general] — implicit instantiations
         * have the same linkage as the template they instantiate;
         * function templates have weak/inline-style multi-TU
         * semantics). Mark DECL_INLINE so codegen emits as
         * static-inline (per-TU body), avoiding multi-def link
         * errors when the same instantiation appears in multiple
         * TUs (e.g. va_gc::reserve<rtx_def*> instantiated in
         * every gen-tool TU that #includes vec.h). */
        c->func.storage_flags |= DECL_INLINE;
        break;
    }

    case ND_PARAM:
        c->param = n->param;
        c->param.ty = subst_type(n->param.ty, map, arena);
        break;

    case ND_CLASS_DEF:
        c->class_def = n->class_def;
        c->class_def.members = clone_node_array(
            n->class_def.members, n->class_def.nmembers, map, arena);
        /* Substitute base types (for template inheritance) */
        if (n->class_def.nbase_types > 0) {
            c->class_def.base_types = arena_alloc(arena,
                n->class_def.nbase_types * sizeof(Type *));
            c->class_def.nbase_types = n->class_def.nbase_types;
            for (int i = 0; i < n->class_def.nbase_types; i++)
                c->class_def.base_types[i] =
                    subst_type(n->class_def.base_types[i], map, arena);
        }
        /* Type is created fresh by the instantiation driver */
        break;

    case ND_TEMPLATE_DECL:
        c->template_decl = n->template_decl;
        /* Don't recurse into nested templates — they'll be
         * instantiated separately if needed */
        break;

    case ND_TYPE_TRAIT:
        /* Deferred GCC/Clang type-trait intrinsic. The clone walks
         * each argument Type through subst_type so any TY_DEPENDENT
         * gets replaced by the bound concrete type; emit later
         * re-evaluates with the new args (parse.h:eval_type_trait). */
        c->type_trait.name = n->type_trait.name;
        c->type_trait.arg0 = subst_type(n->type_trait.arg0, map, arena);
        c->type_trait.arg1 = subst_type(n->type_trait.arg1, map, arena);
        break;

    case ND_TYPEID:
        /* typeid — N4659 §8.2.7 [expr.typeid]. Substitute the static
         * type through the SubstMap (it may reference a dependent
         * template param: 'typeid(T)' becomes 'typeid(int)' once T is
         * bound). The expression-form operand is cloned recursively
         * via the standard handler. */
        c->typeid_.static_type = subst_type(n->typeid_.static_type, map, arena);
        c->typeid_.operand     = clone_node(n->typeid_.operand, map, arena);
        break;

    case ND_TEMPLATE_ID:
        c->template_id = n->template_id;
        c->template_id.args = clone_node_array(
            n->template_id.args, n->template_id.nargs, map, arena);
        /* Rewrite the template-id's name when it matches a TT-param
         * binding. This makes the cloned ND_TEMPLATE_ID refer to the
         * bound class template (e.g. Allocator<int> → xcallocator<int>)
         * so the collection pass picks up xcallocator<int> for class-
         * template instantiation. N4659 §17.2/3 [temp.param] +
         * §17.7.1 [temp.inst]. */
        if (c->template_id.name) {
            Token *bound = subst_map_lookup_tt(map,
                c->template_id.name->loc, c->template_id.name->len);
            if (bound) c->template_id.name = bound;
        }
        break;

    case ND_FRIEND:
        c->friend_decl = n->friend_decl;
        c->friend_decl.decl = clone_node(n->friend_decl.decl, map, arena);
        break;

    case ND_LAMBDA: {
        /* Lambda inside a template body — N4659 §17.6.4 [temp.point].
         * The parser leaves the synthesised closure ND_CLASS_DEF and
         * lambda func_def attached (no TU-top hoist) for templates;
         * each instantiation produces a fresh pair with substituted
         * types. Naming uses a per-process counter so concurrent
         * instantiations of distinct templates get distinct symbols.
         *
         * After this clone returns, the post-clone walker in
         * instantiate.c collects the cloned closure ND_CLASS_DEF and
         * lambda func_def from the cloned lambda nodes inside the
         * cloned body and pushes them into all_instantiated[]. */
        static int s_lambda_inst_counter = 0;
        int        idx = ++s_lambda_inst_counter;

        /* 1. Clone captures[] with resolved_type substituted; expand
         * pack-bound captures to N entries (one per pack-bound type).
         * A capture is pack-bound when its resolved_type's leaf is a
         * TY_DEPENDENT tag that the SubstMap has as a pack entry.
         * Synth names follow the same `<base>_<i>` convention as the
         * function-param-pack expansion in clone_node_array_pack so
         * the lambda body's references resolve uniformly after sema
         * re-runs on the clone. N4659 §8.1.5.2 [expr.prim.lambda.closure]
         * + §17.5.3 [temp.variadic]. */
        Capture *src_caps = n->lambda.captures;
        int      ncap     = n->lambda.ncaptures;
        Capture *new_caps = NULL;
        int new_ncap = 0;
        if (ncap > 0) {
            /* First pass: compute expanded count. */
            int total = 0;
            for (int i = 0; i < ncap; i++) {
                SubstEntry *pe = NULL;
                Type *rt = src_caps[i].resolved_type;
                Type *leaf = rt;
                while (leaf && (leaf->kind == TY_REF ||
                                leaf->kind == TY_RVALREF ||
                                leaf->kind == TY_PTR ||
                                leaf->kind == TY_ARRAY) && leaf->base)
                    leaf = leaf->base;
                if (leaf && leaf->kind == TY_DEPENDENT)
                    pe = subst_map_lookup_pack(map, leaf->tag);
                if (pe) total += pe->pack_ntypes;
                else    total += 1;
            }
            new_caps = arena_alloc(arena, total * sizeof(Capture));
            int oi = 0;
            for (int i = 0; i < ncap; i++) {
                Type *rt = src_caps[i].resolved_type;
                Type *leaf = rt;
                while (leaf && (leaf->kind == TY_REF ||
                                leaf->kind == TY_RVALREF ||
                                leaf->kind == TY_PTR ||
                                leaf->kind == TY_ARRAY) && leaf->base)
                    leaf = leaf->base;
                SubstEntry *pe = NULL;
                if (leaf && leaf->kind == TY_DEPENDENT)
                    pe = subst_map_lookup_pack(map, leaf->tag);
                if (pe && src_caps[i].name) {
                    for (int j = 0; j < pe->pack_ntypes; j++) {
                        new_caps[oi] = src_caps[i];
                        new_caps[oi].name = synth_pack_name(
                            src_caps[i].name, j, arena);
                        /* Wrap pack type with the original ref shape
                         * (by_ref => TY_PTR/REF wrapper preserved via
                         * subst_type on a freshly-bound non-pack entry
                         * is awkward — emit the leaf type directly;
                         * sea-front's lambda emit treats by_ref via
                         * pointer storage regardless). */
                        new_caps[oi].resolved_type = pe->pack_types[j];
                        new_caps[oi].resolved_decl = NULL;
                        oi++;
                    }
                } else {
                    new_caps[oi] = src_caps[i];
                    if (rt)
                        new_caps[oi].resolved_type =
                            subst_type(rt, map, arena);
                    oi++;
                }
            }
            new_ncap = total;
        }
        ncap = new_ncap;  /* downstream uses ncap */

        /* 2. Build a fresh closure TY_STRUCT with a unique tag. */
        Token *src_tag = n->lambda.closure_tag;
        char   tagbuf[80];
        int    src_tlen = src_tag ? src_tag->len : 0;
        const char *src_tloc = src_tag ? src_tag->loc : "__sf_closure";
        int tlen = snprintf(tagbuf, sizeof(tagbuf), "%.*s_inst%d",
                            src_tlen, src_tloc, idx);
        char *tstr = arena_alloc(arena, tlen + 1);
        memcpy(tstr, tagbuf, tlen);
        tstr[tlen] = '\0';
        Token *new_tag = arena_alloc(arena, sizeof(Token));
        if (src_tag) *new_tag = *src_tag; else memset(new_tag, 0, sizeof(*new_tag));
        new_tag->kind = TK_IDENT;
        new_tag->loc  = tstr;
        new_tag->len  = tlen;
        Type *new_closure = arena_alloc(arena, sizeof(Type));
        if (n->lambda.closure_type) *new_closure = *n->lambda.closure_type;
        else memset(new_closure, 0, sizeof(*new_closure));
        new_closure->kind = TY_STRUCT;
        new_closure->tag  = new_tag;

        /* 3. Clone the closure ND_CLASS_DEF (members get substituted).
         * If we expanded any pack captures (new_ncap != source ncap),
         * the cloned source cdef would still have the source's single
         * pack-member; rebuild the members from new_caps so the
         * closure layout matches the expanded captures one-to-one. */
        Node *src_cdef = n->lambda.closure_type
                       ? n->lambda.closure_type->class_def : NULL;
        Node *new_cdef = NULL;
        if (src_cdef) {
            new_cdef = clone_node(src_cdef, map, arena);
            new_cdef->class_def.tag = new_tag;
            new_cdef->class_def.ty  = new_closure;
            new_closure->class_def  = new_cdef;
            /* Detect whether ANY capture was pack-renamed (even when
             * the pack bound to a single type, the name changed from
             * `args` to `args_0`). Rebuild members from new_caps so
             * the closure field names match the body's expanded
             * references. */
            bool any_renamed = false;
            for (int i = 0; i < n->lambda.ncaptures && i < new_ncap; i++) {
                if (n->lambda.captures[i].name != new_caps[i].name) {
                    any_renamed = true;
                    break;
                }
            }
            if (new_ncap != n->lambda.ncaptures || any_renamed) {
                /* Rebuild members from new_caps: one ND_VAR_DECL per
                 * capture. For by-ref captures, the C-side field type
                 * is a pointer (sea-front lowers refs to pointers in
                 * closure storage); for by-value, it's the resolved
                 * type as-is. Mirrors what parse_lambda's closure
                 * synthesis would have produced for the expanded
                 * capture list directly. */
                Node **mems = arena_alloc(arena, new_ncap * sizeof(Node *));
                for (int i = 0; i < new_ncap; i++) {
                    Node *m = arena_alloc(arena, sizeof(Node));
                    memset(m, 0, sizeof(*m));
                    m->kind = ND_VAR_DECL;
                    m->var_decl.name = new_caps[i].name;
                    Type *ft = new_caps[i].resolved_type;
                    if (new_caps[i].by_ref && ft) {
                        Type *pt = arena_alloc(arena, sizeof(Type));
                        memset(pt, 0, sizeof(*pt));
                        pt->kind = TY_PTR;
                        pt->base = ft;
                        ft = pt;
                    }
                    m->var_decl.ty = ft;
                    mems[i] = m;
                }
                new_cdef->class_def.members = mems;
                new_cdef->class_def.nmembers = new_ncap;
            }
        }

        /* 4. Clone the lambda func_def. Reusing clone_node for
         *    ND_FUNC_DEF substitutes the body, params, and ret_ty. */
        Node *src_fd = n->lambda.func_def;
        Node *new_fd = src_fd ? clone_node(src_fd, map, arena) : NULL;
        if (new_fd) {
            /* Rename so distinct instantiations don't collide.
             * Suffix preserves the original counter for traceability. */
            char fbuf[80];
            int srcnl = src_fd->func.name ? src_fd->func.name->len : 0;
            const char *srcnloc = src_fd->func.name
                               ? src_fd->func.name->loc : "__sf_lambda";
            int fnlen = snprintf(fbuf, sizeof(fbuf), "%.*s_inst%d",
                                 srcnl, srcnloc, idx);
            char *fstr = arena_alloc(arena, fnlen + 1);
            memcpy(fstr, fbuf, fnlen);
            fstr[fnlen] = '\0';
            Token *new_fname = arena_alloc(arena, sizeof(Token));
            if (src_fd->func.name) *new_fname = *src_fd->func.name;
            else memset(new_fname, 0, sizeof(*new_fname));
            new_fname->kind = TK_IDENT;
            new_fname->loc  = fstr;
            new_fname->len  = fnlen;
            new_fd->func.name              = new_fname;
            new_fd->func.is_lambda_fn      = true;
            new_fd->func.closure_struct_type = new_closure;
            new_fd->func.captures          = new_caps;
            new_fd->func.ncaptures         = ncap;
            /* The first param (__self) was cloned with subst_type,
             * which sees it as TY_PTR(TY_STRUCT) where the struct may
             * not be the substituted closure (subst_type doesn't know
             * about lambdas). Override it to point at new_closure. */
            if (new_fd->func.nparams > 0 && new_fd->func.params[0]) {
                Type *pt = arena_alloc(arena, sizeof(Type));
                memset(pt, 0, sizeof(*pt));
                pt->kind = TY_PTR;
                pt->base = new_closure;
                new_fd->func.params[0]->param.ty = pt;
            }
            new_closure->lambda_fn = new_fd;
        }

        c->lambda.func_def     = new_fd;
        c->lambda.captures     = new_caps;
        c->lambda.ncaptures    = ncap;
        c->lambda.default_kind = n->lambda.default_kind;
        c->lambda.closure_type = new_closure;
        c->lambda.closure_tag  = new_tag;
        /* Note: the post-clone walker in instantiate.c will reach
         * new_cdef and new_fd via this ND_LAMBDA and prepend them
         * to all_instantiated[] before the surrounding decl. */
        break;
    }

    case ND_TRANSLATION_UNIT:
        c->tu = n->tu;
        /* Should not be cloned — top-level container */
        break;

    case ND_STMT_EXPR:
        /* GCC statement-expression — '({ stmts; expr; })'. The body
         * is a structured ND_BLOCK that may reference template
         * parameters via identifiers, types, or nested template-ids
         * — all need substitution. Without recursing here, the
         * cloned ND_STMT_EXPR has block=NULL and emit_c renders it
         * as empty parens '()' inside the surrounding cast or
         * assignment. Real-world hit: gcc 14 libcpp/identifiers.cc
         * 'template<typename Node> alloc_node' whose body is the
         * XOBNEW macro — a deeply-nested __extension__ ({...}) with
         * stmt-exprs at multiple levels. The instantiated template
         * dropped the entire body. */
        c->stmt_expr.block = clone_node(n->stmt_expr.block, map, arena);
        break;

    default:
        /* Hygiene: NodeKinds above cover everything parse/expr.c,
         * parse/stmt.c, parse/decl.c produce. A future NodeKind
         * added without updating clone_node ends up here; it passes
         * through with just kind/tok/resolved_type copied. */
        break;
    }

    /* Carry resolved_type through with substitution. The first sema
     * pass (before instantiation) populates resolved_type on the
     * original template's expression nodes; without this line the
     * cloned ident/expr would lose it. Substituting through map
     * means a 'b' of declared type Box<T> in the template becomes
     * Box<int> on the clone, so codegen can match the instantiated
     * class's methods for operator-overload rewrites.
     *
     * EXCEPTION: closure types (TY_STRUCT.lambda_fn != NULL) have
     * a fresh per-instantiation tag/class_def/lambda_fn produced
     * by the ND_LAMBDA clone case above. subst_type can't follow
     * that Node-level relationship — it walks Type trees only —
     * so substituting yields the OLD closure. Leave resolved_type
     * NULL for these so sema's re-walk over the cloned body re-
     * resolves the ident from its (also-cloned) declaration whose
     * ty was correctly rebound. */
    if (n->resolved_type) {
        Type *rt = n->resolved_type;
        bool is_closure = rt->kind == TY_STRUCT && rt->lambda_fn != NULL;
        if (!is_closure)
            c->resolved_type = subst_type(rt, map, arena);
    }

    return c;
}
