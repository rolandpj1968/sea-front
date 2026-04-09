/*
 * clone.c — AST cloning with type substitution for template
 * instantiation.
 *
 * subst_type: deep-copy a Type, replacing TY_DEPENDENT nodes whose
 *   tag matches a SubstMap entry with the concrete type.
 * clone_node: deep-copy a Node tree, applying subst_type to every
 *   Type* field and recursively cloning child nodes.
 *
 * All allocations use the Arena passed in — no freeing needed.
 */

#include "clone.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* SubstMap                                                            */
/* ------------------------------------------------------------------ */

SubstMap subst_map_new(Arena *arena, int capacity) {
    SubstMap m;
    m.entries = arena_alloc(arena, capacity * sizeof(SubstEntry));
    m.nentries = 0;
    m.capacity = capacity;
    m.arena = arena;
    return m;
}

void subst_map_add(SubstMap *m, Token *param_name, Type *concrete_type) {
    if (m->nentries >= m->capacity) return; /* shouldn't happen */
    m->entries[m->nentries].param_name = param_name;
    m->entries[m->nentries].concrete_type = concrete_type;
    m->nentries++;
}

static Type *subst_map_lookup(SubstMap *m, Token *tag) {
    if (!tag) return NULL;
    for (int i = 0; i < m->nentries; i++) {
        Token *p = m->entries[i].param_name;
        if (p && p->len == tag->len &&
            memcmp(p->loc, tag->loc, tag->len) == 0)
            return m->entries[i].concrete_type;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Type substitution                                                   */
/* ------------------------------------------------------------------ */

Type *subst_type(Type *ty, SubstMap *map, Arena *arena) {
    if (!ty) return NULL;

    /* TY_DEPENDENT — the core substitution case */
    if (ty->kind == TY_DEPENDENT) {
        Type *concrete = subst_map_lookup(map, ty->tag);
        if (concrete) {
            /* Apply cv-qualifiers from the dependent usage onto
             * the concrete type (e.g. 'const T' → 'const int') */
            if (ty->is_const || ty->is_volatile) {
                Type *copy = arena_alloc(arena, sizeof(Type));
                *copy = *concrete;
                copy->is_const    = copy->is_const    || ty->is_const;
                copy->is_volatile = copy->is_volatile || ty->is_volatile;
                return copy;
            }
            return concrete;
        }
        /* Unresolved dependent type — return as-is (may be from an
         * outer template scope we're not instantiating yet) */
        return ty;
    }

    /* Compound types — recurse into children */
    switch (ty->kind) {
    case TY_PTR: case TY_REF: case TY_RVALREF: {
        Type *new_base = subst_type(ty->base, map, arena);
        if (new_base == ty->base) return ty; /* no change */
        Type *copy = arena_alloc(arena, sizeof(Type));
        *copy = *ty;
        copy->base = new_base;
        return copy;
    }
    case TY_ARRAY: {
        Type *new_base = subst_type(ty->base, map, arena);
        if (new_base == ty->base) return ty;
        Type *copy = arena_alloc(arena, sizeof(Type));
        *copy = *ty;
        copy->base = new_base;
        return copy;
    }
    case TY_FUNC: {
        Type *new_ret = subst_type(ty->ret, map, arena);
        bool changed = (new_ret != ty->ret);
        Type **new_params = NULL;
        if (ty->nparams > 0) {
            new_params = arena_alloc(arena, ty->nparams * sizeof(Type *));
            for (int i = 0; i < ty->nparams; i++) {
                new_params[i] = subst_type(ty->params[i], map, arena);
                if (new_params[i] != ty->params[i]) changed = true;
            }
        }
        if (!changed) return ty;
        Type *copy = arena_alloc(arena, sizeof(Type));
        *copy = *ty;
        copy->ret = new_ret;
        if (new_params) copy->params = new_params;
        return copy;
    }
    default:
        /* Fundamental types, TY_STRUCT, TY_UNION, TY_ENUM — no
         * dependent children to substitute. Return as-is. */
        return ty;
    }
}

/* ------------------------------------------------------------------ */
/* Node cloning                                                        */
/* ------------------------------------------------------------------ */

/* Forward declaration — clone_node is recursive */
Node *clone_node(Node *n, SubstMap *map, Arena *arena);

/* Clone an array of Node pointers */
static Node **clone_node_array(Node **arr, int count,
                                SubstMap *map, Arena *arena) {
    if (!arr || count <= 0) return NULL;
    Node **copy = arena_alloc(arena, count * sizeof(Node *));
    for (int i = 0; i < count; i++)
        copy[i] = clone_node(arr[i], map, arena);
    return copy;
}

/* Clone a MemInit array (ctor mem-initializer-list) */
static MemInit *clone_mem_inits(MemInit *inits, int count,
                                 SubstMap *map, Arena *arena) {
    if (!inits || count <= 0) return NULL;
    MemInit *copy = arena_alloc(arena, count * sizeof(MemInit));
    for (int i = 0; i < count; i++) {
        copy[i].name = inits[i].name;
        copy[i].args = clone_node_array(inits[i].args, inits[i].nargs,
                                         map, arena);
        copy[i].nargs = inits[i].nargs;
    }
    return copy;
}

Node *clone_node(Node *n, SubstMap *map, Arena *arena) {
    if (!n) return NULL;

    Node *c = arena_alloc(arena, sizeof(Node));
    *c = *n;  /* shallow copy — then fix up child pointers below */

    switch (n->kind) {
    /* -- Leaf expression nodes -- */
    case ND_NUM:
    case ND_FNUM:
    case ND_STR:
    case ND_CHAR:
    case ND_BOOL_LIT:
    case ND_NULLPTR:
    case ND_NULL_STMT:
    case ND_BREAK:
    case ND_CONTINUE:
    case ND_GOTO:
    case ND_ACCESS_SPEC:
        /* No child nodes or types to substitute */
        break;

    case ND_IDENT:
        /* Leaf — name token is shared, not cloned */
        break;

    case ND_QUALIFIED:
        /* Clone the parts array (tokens are shared) */
        if (n->qualified.parts && n->qualified.nparts > 0) {
            c->qualified.parts = arena_alloc(arena,
                n->qualified.nparts * sizeof(Token *));
            memcpy(c->qualified.parts, n->qualified.parts,
                   n->qualified.nparts * sizeof(Token *));
        }
        break;

    /* -- Binary expressions -- */
    case ND_BINARY:
    case ND_ASSIGN:
    case ND_COMMA:
        c->binary.lhs = clone_node(n->binary.lhs, map, arena);
        c->binary.rhs = clone_node(n->binary.rhs, map, arena);
        break;

    /* -- Unary expressions -- */
    case ND_UNARY:
    case ND_POSTFIX:
        c->unary.operand = clone_node(n->unary.operand, map, arena);
        break;

    case ND_TERNARY:
        c->ternary.cond  = clone_node(n->ternary.cond, map, arena);
        c->ternary.then_ = clone_node(n->ternary.then_, map, arena);
        c->ternary.else_ = clone_node(n->ternary.else_, map, arena);
        break;

    case ND_CALL:
        c->call.callee = clone_node(n->call.callee, map, arena);
        c->call.args   = clone_node_array(n->call.args, n->call.nargs,
                                           map, arena);
        break;

    case ND_MEMBER:
        c->member.obj = clone_node(n->member.obj, map, arena);
        break;

    case ND_SUBSCRIPT:
        c->subscript.base  = clone_node(n->subscript.base, map, arena);
        c->subscript.index = clone_node(n->subscript.index, map, arena);
        break;

    case ND_CAST:
        c->cast.ty      = subst_type(n->cast.ty, map, arena);
        c->cast.operand = clone_node(n->cast.operand, map, arena);
        break;

    case ND_SIZEOF:
        c->sizeof_.expr = clone_node(n->sizeof_.expr, map, arena);
        c->sizeof_.ty   = subst_type(n->sizeof_.ty, map, arena);
        break;

    case ND_ALIGNOF:
        c->alignof_.ty = subst_type(n->alignof_.ty, map, arena);
        break;

    /* -- Statements -- */
    case ND_BLOCK:
        c->block.stmts = clone_node_array(n->block.stmts, n->block.nstmts,
                                           map, arena);
        c->block.scope = NULL;  /* sema re-creates */
        break;

    case ND_RETURN:
        c->ret.expr = clone_node(n->ret.expr, map, arena);
        break;

    case ND_EXPR_STMT:
        c->expr_stmt.expr = clone_node(n->expr_stmt.expr, map, arena);
        break;

    case ND_IF:
        c->if_.init  = clone_node(n->if_.init, map, arena);
        c->if_.cond  = clone_node(n->if_.cond, map, arena);
        c->if_.then_ = clone_node(n->if_.then_, map, arena);
        c->if_.else_ = clone_node(n->if_.else_, map, arena);
        break;

    case ND_WHILE:
        c->while_.cond = clone_node(n->while_.cond, map, arena);
        c->while_.body = clone_node(n->while_.body, map, arena);
        break;

    case ND_DO:
        c->do_.cond = clone_node(n->do_.cond, map, arena);
        c->do_.body = clone_node(n->do_.body, map, arena);
        break;

    case ND_FOR:
        c->for_.init = clone_node(n->for_.init, map, arena);
        c->for_.cond = clone_node(n->for_.cond, map, arena);
        c->for_.inc  = clone_node(n->for_.inc, map, arena);
        c->for_.body = clone_node(n->for_.body, map, arena);
        break;

    case ND_SWITCH:
        c->switch_.init = clone_node(n->switch_.init, map, arena);
        c->switch_.expr = clone_node(n->switch_.expr, map, arena);
        c->switch_.body = clone_node(n->switch_.body, map, arena);
        break;

    case ND_CASE:
        c->case_.expr = clone_node(n->case_.expr, map, arena);
        c->case_.stmt = clone_node(n->case_.stmt, map, arena);
        break;

    case ND_DEFAULT:
        c->default_.stmt = clone_node(n->default_.stmt, map, arena);
        break;

    case ND_LABEL:
        c->label.stmt = clone_node(n->label.stmt, map, arena);
        break;

    /* -- Declarations -- */
    case ND_VAR_DECL:
    case ND_TYPEDEF:
        c->var_decl.ty   = subst_type(n->var_decl.ty, map, arena);
        c->var_decl.init = clone_node(n->var_decl.init, map, arena);
        c->var_decl.ctor_args = clone_node_array(
            n->var_decl.ctor_args, n->var_decl.ctor_nargs, map, arena);
        break;

    case ND_FUNC_DEF:
    case ND_FUNC_DECL:
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
        c->param.ty = subst_type(n->param.ty, map, arena);
        break;

    case ND_CLASS_DEF:
        c->class_def.members = clone_node_array(
            n->class_def.members, n->class_def.nmembers, map, arena);
        /* Type is created fresh by the instantiation driver */
        break;

    case ND_TEMPLATE_DECL:
        /* Don't recurse into nested templates — they'll be
         * instantiated separately if needed */
        break;

    case ND_TEMPLATE_ID:
        /* Clone args but keep name token shared */
        c->template_id.args = clone_node_array(
            n->template_id.args, n->template_id.nargs, map, arena);
        break;

    case ND_FRIEND:
        c->friend_decl.decl = clone_node(n->friend_decl.decl, map, arena);
        break;

    case ND_TRANSLATION_UNIT:
        /* Should not be cloned — top-level container */
        break;
    }

    return c;
}
