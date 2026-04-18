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

#include "clone.h"

/* ------------------------------------------------------------------ */
/* SubstMap                                                            */
/* ------------------------------------------------------------------ */

SubstMap subst_map_new(Arena *arena, int capacity) {
    SubstMap m = {0};
    m.entries = arena_alloc(arena, capacity * sizeof(SubstEntry));
    m.nentries = 0;
    m.capacity = capacity;
    return m;
}

void subst_map_add(SubstMap *m, Token *param_name, Type *concrete_type) {
    if (m->nentries >= m->capacity) return;  /* silently drop — shouldn't happen */
    m->entries[m->nentries].param_name = param_name;
    m->entries[m->nentries].concrete_type = concrete_type;
    m->nentries++;
}

static Type *subst_map_lookup(SubstMap *map, const char *name, int len) {
    for (int i = 0; i < map->nentries; i++) {
        Token *pn = map->entries[i].param_name;
        if (pn && pn->len == len && memcmp(pn->loc, name, len) == 0)
            return map->entries[i].concrete_type;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Type substitution                                                   */
/* ------------------------------------------------------------------ */

Type *subst_type(Type *ty, SubstMap *map, Arena *arena) {
    if (!ty) return NULL;

    /* TY_DEPENDENT → substitute if the name matches a map entry */
    if (ty->kind == TY_DEPENDENT && ty->tag) {
        Type *concrete = subst_map_lookup(map, ty->tag->loc, ty->tag->len);
        if (!concrete) {
            /* No match — leave as-is (still dependent for outer template) */
            return ty;
        }
        /* Qualified dependent name 'typename T::member': once T
         * resolves to a concrete class type, look up 'member' in its
         * class region. Handles dependent defaults like
         *   template<typename T, typename L = typename T::default_layout>
         * where the substitution must resolve T::default_layout.
         *
         * N4659 §13.8.3 [temp.dep.type] — 'typename' nested-name is a
         * dependent type until instantiation resolves the base. At
         * instantiation (§17.7 [temp.inst]), the member is looked up
         * in the (now concrete) enclosing type. */
        if (ty->dep_member && concrete->class_region) {
            Declaration *md = lookup_in_scope(concrete->class_region,
                ty->dep_member->loc, ty->dep_member->len);
            if (md && md->type) return md->type;
            /* Fall through — leave dependent if we can't resolve. */
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
         * is dependent. Template params parse as TY_DEPENDENT thanks
         * to the OOL scope fix (decl.c qscope wrapper). */
        /* N4659 §13.8.3 [temp.dep.type]: check if any template-id arg
         * is dependent. With the OOL scope fix (decl.c qscope→enclosing
         * chain), template params reliably parse as TY_DEPENDENT. */
        bool needs_subst = false;
        for (int i = 0; i < tid->template_id.nargs; i++) {
            Node *a = tid->template_id.args[i];
            Type *at = (a && a->kind == ND_VAR_DECL) ? a->var_decl.ty : NULL;
            if (at && at->kind == TY_DEPENDENT) { needs_subst = true; break; }
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
                if (at && at->kind == TY_DEPENDENT) {
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
        if (sub == ty->base) return ty;
        Type *copy = arena_alloc(arena, sizeof(Type));
        *copy = *ty;
        copy->base = sub;
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
    if (!arr || n <= 0) return NULL;
    Node **out = arena_alloc(arena, n * sizeof(Node *));
    for (int i = 0; i < n; i++)
        out[i] = clone_node(arr[i], map, arena);
    return out;
}

static MemInit *clone_mem_inits(MemInit *inits, int n,
                                 SubstMap *map, Arena *arena) {
    (void)map;
    if (!inits || n <= 0) return NULL;
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
                    /* Replace the leading token with the concrete type's tag */
                    if (sub->tag) c->qualified.parts[0] = sub->tag;
                    /* Phase 2: look up the member in the concrete class.
                     * N4659 §6.4.3 [basic.lookup.qual] — qualified name
                     * lookup in the named class. */
                    if (sub->class_region && c->qualified.nparts >= 2) {
                        Token *member = c->qualified.parts[c->qualified.nparts - 1];
                        if (member) {
                            Declaration *md = lookup_in_scope(
                                sub->class_region, member->loc, member->len);
                            if (md && md->type) {
                                c->resolved_type = md->type;
                            }
                        }
                    }
                }
            }
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

    case ND_CALL:
        c->call = n->call;
        c->call.callee = clone_node(n->call.callee, map, arena);
        c->call.args   = clone_node_array(n->call.args, n->call.nargs,
                                           map, arena);
        break;

    case ND_MEMBER:
        c->member = n->member;
        c->member.obj = clone_node(n->member.obj, map, arena);
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
        break;

    case ND_SIZEOF:
        c->sizeof_ = n->sizeof_;
        c->sizeof_.expr = clone_node(n->sizeof_.expr, map, arena);
        c->sizeof_.ty   = subst_type(n->sizeof_.ty, map, arena);
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
        c->var_decl.ctor_args = clone_node_array(
            n->var_decl.ctor_args, n->var_decl.ctor_nargs, map, arena);
        break;

    case ND_FUNC_DEF:
    case ND_FUNC_DECL:
        c->func = n->func;
        c->func.ret_ty = subst_type(n->func.ret_ty, map, arena);
        c->func.params = clone_node_array(n->func.params, n->func.nparams,
                                           map, arena);
        c->func.body   = clone_node(n->func.body, map, arena);
        c->func.mem_inits = clone_mem_inits(n->func.mem_inits,
                                             n->func.n_mem_inits,
                                             map, arena);
        c->func.param_scope = NULL;  /* sema re-creates */
        c->func.deferred_class_region = NULL;
        c->func.body_start_pos = -1;  /* not deferred — body is cloned */
        c->func.body_end_pos   = -1;
        break;

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

    case ND_TEMPLATE_ID:
        c->template_id = n->template_id;
        c->template_id.args = clone_node_array(
            n->template_id.args, n->template_id.nargs, map, arena);
        break;

    case ND_FRIEND:
        c->friend_decl = n->friend_decl;
        c->friend_decl.decl = clone_node(n->friend_decl.decl, map, arena);
        break;

    case ND_TRANSLATION_UNIT:
        c->tu = n->tu;
        /* Should not be cloned — top-level container */
        break;
    }

    /* Carry resolved_type through with substitution. The first sema
     * pass (before instantiation) populates resolved_type on the
     * original template's expression nodes; without this line the
     * cloned ident/expr would lose it. Substituting through map
     * means a 'b' of declared type Box<T> in the template becomes
     * Box<int> on the clone, so codegen can match the instantiated
     * class's methods for operator-overload rewrites. */
    if (n->resolved_type)
        c->resolved_type = subst_type(n->resolved_type, map, arena);

    return c;
}
