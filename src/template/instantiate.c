/*
 * instantiate.c — Template instantiation pass.
 *
 * See instantiate.h for the public API and phase overview.
 *
 * This file implements:
 *   Phase 1: template registry — hash map from name to ND_TEMPLATE_DECL
 *   Phase 2: instantiation site collection — recursive AST walk
 *   Phase 3: clone + substitute + prepend to TU
 */

#include "instantiate.h"
#include "clone.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Template Registry — Phase 1                                        */
/* ------------------------------------------------------------------ */

#define TMPL_REGISTRY_SIZE 64

typedef struct TmplEntry TmplEntry;
struct TmplEntry {
    const char *name;
    int         name_len;
    Node       *tmpl;       /* ND_TEMPLATE_DECL */
    TmplEntry  *next;       /* hash chain */
};

typedef struct {
    TmplEntry *buckets[TMPL_REGISTRY_SIZE];
    Arena     *arena;
} TmplRegistry;

static uint32_t hash_name(const char *name, int len) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) {
        h ^= (uint8_t)name[i];
        h *= 16777619u;
    }
    return h;
}

static void registry_add(TmplRegistry *reg, const char *name, int name_len,
                          Node *tmpl) {
    uint32_t idx = hash_name(name, name_len) % TMPL_REGISTRY_SIZE;
    /* Don't duplicate — the last registration wins (specializations
     * shadow the primary, which is fine for now). */
    TmplEntry *e = arena_alloc(reg->arena, sizeof(TmplEntry));
    e->name = name;
    e->name_len = name_len;
    e->tmpl = tmpl;
    e->next = reg->buckets[idx];
    reg->buckets[idx] = e;
}

static Node *registry_find(TmplRegistry *reg, const char *name, int name_len) {
    uint32_t idx = hash_name(name, name_len) % TMPL_REGISTRY_SIZE;
    for (TmplEntry *e = reg->buckets[idx]; e; e = e->next) {
        if (e->name_len == name_len && memcmp(e->name, name, name_len) == 0)
            return e->tmpl;
    }
    return NULL;
}

/*
 * Extract the template name from an ND_TEMPLATE_DECL. The name lives
 * on the inner declaration — varies by kind:
 *   ND_CLASS_DEF   → class_def.tag
 *   ND_FUNC_DEF    → func.name
 *   ND_VAR_DECL    → var_decl.name (or var_decl.ty->tag for bare struct)
 *   ND_TYPEDEF     → var_decl.name
 *   ND_FRIEND      → recurse into friend_decl.decl
 */
static Token *template_name(Node *tmpl) {
    if (!tmpl || tmpl->kind != ND_TEMPLATE_DECL) return NULL;
    Node *decl = tmpl->template_decl.decl;
    if (!decl) return NULL;
    switch (decl->kind) {
    case ND_CLASS_DEF:   return decl->class_def.tag;
    case ND_FUNC_DEF:
    case ND_FUNC_DECL:   return decl->func.name;
    case ND_VAR_DECL:
    case ND_TYPEDEF:
        if (decl->var_decl.name) return decl->var_decl.name;
        if (decl->var_decl.ty && decl->var_decl.ty->tag)
            return decl->var_decl.ty->tag;
        return NULL;
    case ND_FRIEND:
        /* Unwrap one level for friend template declarations */
        if (decl->friend_decl.decl) {
            Node wrapper = *tmpl;
            wrapper.template_decl.decl = decl->friend_decl.decl;
            return template_name(&wrapper);
        }
        return NULL;
    default:
        return NULL;
    }
}

/*
 * Phase 1: walk top-level declarations and register every
 * ND_TEMPLATE_DECL in the registry. Recurses into ND_BLOCK
 * (namespaces, extern "C" blocks).
 */
static void build_registry(TmplRegistry *reg, Node *n) {
    if (!n) return;
    switch (n->kind) {
    case ND_TEMPLATE_DECL: {
        Token *name = template_name(n);
        if (name)
            registry_add(reg, name->loc, name->len, n);
        break;
    }
    case ND_BLOCK:
        for (int i = 0; i < n->block.nstmts; i++)
            build_registry(reg, n->block.stmts[i]);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Instantiation Request Collection — Phase 2                         */
/* ------------------------------------------------------------------ */

typedef struct InstRequest InstRequest;
struct InstRequest {
    Token *name;            /* template name */
    Node  *template_id;     /* ND_TEMPLATE_ID with args */
    Node  *tmpl_def;        /* resolved ND_TEMPLATE_DECL */
    Type  *usage_type;      /* the Type* at the usage site (to patch) */
    InstRequest *next;
};

typedef struct {
    InstRequest *head;
    int          count;
    Arena       *arena;
    TmplRegistry *reg;
} InstCollector;

/*
 * Check if a Type references a template instantiation (has a
 * template_id_node set) and, if so, record a request.
 */
static void collect_from_type(InstCollector *col, Type *ty) {
    if (!ty) return;

    /* Recurse into compound types */
    switch (ty->kind) {
    case TY_PTR: case TY_REF: case TY_RVALREF: case TY_ARRAY:
        collect_from_type(col, ty->base);
        return;
    case TY_FUNC:
        collect_from_type(col, ty->ret);
        for (int i = 0; i < ty->nparams; i++)
            collect_from_type(col, ty->params[i]);
        return;
    default:
        break;
    }

    if (!ty->template_id_node) return;

    Node *tid = ty->template_id_node;
    if (tid->kind != ND_TEMPLATE_ID || !tid->template_id.name)
        return;

    Token *name = tid->template_id.name;
    Node *tmpl = registry_find(col->reg, name->loc, name->len);
    if (!tmpl) return;  /* template definition not found — skip */

    /* TODO: dedup — check if we already have a request for this
     * (name, args) combination. For now, collect all and dedup later. */
    InstRequest *req = arena_alloc(col->arena, sizeof(InstRequest));
    req->name = name;
    req->template_id = tid;
    req->tmpl_def = tmpl;
    req->usage_type = ty;  /* patch this type after instantiation */
    req->next = col->head;
    col->head = req;
    col->count++;
}

/*
 * Recursively walk an AST node collecting template instantiation
 * requests from every Type* field.
 */
static void collect_from_node(InstCollector *col, Node *n) {
    if (!n) return;

    /* Check types on this node */
    switch (n->kind) {
    case ND_VAR_DECL:
    case ND_TYPEDEF:
        collect_from_type(col, n->var_decl.ty);
        if (n->var_decl.init)
            collect_from_node(col, n->var_decl.init);
        break;

    case ND_PARAM:
        collect_from_type(col, n->param.ty);
        break;

    case ND_FUNC_DEF:
    case ND_FUNC_DECL:
        collect_from_type(col, n->func.ret_ty);
        for (int i = 0; i < n->func.nparams; i++)
            collect_from_node(col, n->func.params[i]);
        if (n->func.body)
            collect_from_node(col, n->func.body);
        break;

    case ND_CLASS_DEF:
        for (int i = 0; i < n->class_def.nmembers; i++)
            collect_from_node(col, n->class_def.members[i]);
        break;

    case ND_TEMPLATE_DECL:
        /* Walk into the template body to find nested template-id
         * usages (e.g. a class template whose member is another
         * template instantiation). */
        collect_from_node(col, n->template_decl.decl);
        break;

    case ND_BLOCK:
        for (int i = 0; i < n->block.nstmts; i++)
            collect_from_node(col, n->block.stmts[i]);
        break;

    case ND_IF:
        collect_from_node(col, n->if_.cond);
        collect_from_node(col, n->if_.then_);
        collect_from_node(col, n->if_.else_);
        break;

    case ND_FOR:
        collect_from_node(col, n->for_.init);
        collect_from_node(col, n->for_.cond);
        collect_from_node(col, n->for_.inc);
        collect_from_node(col, n->for_.body);
        break;

    case ND_WHILE:
        collect_from_node(col, n->while_.cond);
        collect_from_node(col, n->while_.body);
        break;

    case ND_DO:
        collect_from_node(col, n->do_.cond);
        collect_from_node(col, n->do_.body);
        break;

    case ND_RETURN:
        collect_from_node(col, n->ret.expr);
        break;

    case ND_BINARY:
    case ND_ASSIGN:
        collect_from_node(col, n->binary.lhs);
        collect_from_node(col, n->binary.rhs);
        break;

    case ND_UNARY:
    case ND_POSTFIX:
        collect_from_node(col, n->unary.operand);
        break;

    case ND_TERNARY:
        collect_from_node(col, n->ternary.cond);
        collect_from_node(col, n->ternary.then_);
        collect_from_node(col, n->ternary.else_);
        break;

    case ND_CALL:
        collect_from_node(col, n->call.callee);
        for (int i = 0; i < n->call.nargs; i++)
            collect_from_node(col, n->call.args[i]);
        break;

    case ND_MEMBER:
        collect_from_node(col, n->member.obj);
        break;

    case ND_SUBSCRIPT:
        collect_from_node(col, n->subscript.base);
        collect_from_node(col, n->subscript.index);
        break;

    case ND_CAST:
        collect_from_type(col, n->cast.ty);
        collect_from_node(col, n->cast.operand);
        break;

    case ND_COMMA:
        collect_from_node(col, n->binary.lhs);
        collect_from_node(col, n->binary.rhs);
        break;

    case ND_EXPR_STMT:
        collect_from_node(col, n->expr_stmt.expr);
        break;

    case ND_SWITCH:
        collect_from_node(col, n->switch_.expr);
        collect_from_node(col, n->switch_.body);
        break;

    case ND_CASE:
        collect_from_node(col, n->case_.expr);
        collect_from_node(col, n->case_.stmt);
        break;

    case ND_DEFAULT:
        collect_from_node(col, n->default_.stmt);
        break;

    case ND_FRIEND:
        collect_from_node(col, n->friend_decl.decl);
        break;

    default:
        /* Leaf nodes (ND_NUM, ND_IDENT, ND_STR, etc.) — no types to check */
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Deduplication set                                                   */
/* ------------------------------------------------------------------ */

/*
 * Build a dedup key string for a template-id: "name\0arg1\0arg2\0...".
 * Two instantiations with the same key are identical and should be
 * emitted only once. The key encodes template name + each arg type's
 * mangled representation.
 *
 * Returns key length (including embedded NULs). The buffer must be
 * large enough (MAX_DEDUP_KEY).
 */
#define MAX_DEDUP_KEY 512

static int type_to_key(Type *ty, char *buf, int pos, int max) {
    if (!ty || pos >= max - 1) return pos;
    switch (ty->kind) {
    case TY_VOID:    pos += snprintf(buf+pos, max-pos, "v"); break;
    case TY_BOOL:    pos += snprintf(buf+pos, max-pos, "b"); break;
    case TY_CHAR:    pos += snprintf(buf+pos, max-pos, ty->is_unsigned ? "uc" : "c"); break;
    case TY_SHORT:   pos += snprintf(buf+pos, max-pos, ty->is_unsigned ? "us" : "s"); break;
    case TY_INT:     pos += snprintf(buf+pos, max-pos, ty->is_unsigned ? "ui" : "i"); break;
    case TY_LONG:    pos += snprintf(buf+pos, max-pos, ty->is_unsigned ? "ul" : "l"); break;
    case TY_LLONG:   pos += snprintf(buf+pos, max-pos, ty->is_unsigned ? "ull" : "ll"); break;
    case TY_FLOAT:   pos += snprintf(buf+pos, max-pos, "f"); break;
    case TY_DOUBLE:  pos += snprintf(buf+pos, max-pos, "d"); break;
    case TY_LDOUBLE: pos += snprintf(buf+pos, max-pos, "ld"); break;
    case TY_PTR:     buf[pos++] = 'P'; pos = type_to_key(ty->base, buf, pos, max); break;
    case TY_REF:     buf[pos++] = 'R'; pos = type_to_key(ty->base, buf, pos, max); break;
    case TY_RVALREF: buf[pos++] = 'O'; pos = type_to_key(ty->base, buf, pos, max); break;
    case TY_STRUCT: case TY_UNION:
        if (ty->tag) pos += snprintf(buf+pos, max-pos, "S%.*s", ty->tag->len, ty->tag->loc);
        /* Include template args of nested template types */
        if (ty->n_template_args > 0) {
            buf[pos++] = '<';
            for (int i = 0; i < ty->n_template_args; i++)
                pos = type_to_key(ty->template_args[i], buf, pos, max);
            buf[pos++] = '>';
        }
        break;
    default:
        pos += snprintf(buf+pos, max-pos, "?");
        break;
    }
    return pos;
}

static int build_dedup_key(Node *template_id, char *buf) {
    Token *name = template_id->template_id.name;
    int pos = 0;
    /* Template name */
    if (name && pos + name->len < MAX_DEDUP_KEY) {
        memcpy(buf, name->loc, name->len);
        pos = name->len;
    }
    buf[pos++] = '\0';
    /* Args */
    for (int i = 0; i < template_id->template_id.nargs; i++) {
        pos = type_to_key(
            (template_id->template_id.args[i] &&
             template_id->template_id.args[i]->kind == ND_VAR_DECL)
                ? template_id->template_id.args[i]->var_decl.ty : NULL,
            buf, pos, MAX_DEDUP_KEY);
        buf[pos++] = '\0';
    }
    return pos;
}

#define DEDUP_HASH_SIZE 64

typedef struct DedupEntry DedupEntry;
struct DedupEntry {
    char       key[MAX_DEDUP_KEY];
    int        key_len;
    Type      *inst_type;   /* the instantiated Type (for patching) */
    DedupEntry *next;
};

typedef struct {
    DedupEntry *buckets[DEDUP_HASH_SIZE];
    Arena      *arena;
} DedupSet;

static uint32_t hash_key(const char *key, int len) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) {
        h ^= (uint8_t)key[i];
        h *= 16777619u;
    }
    return h;
}

/* Returns the existing Type* if already instantiated, else NULL. */
static Type *dedup_find(DedupSet *ds, const char *key, int key_len) {
    uint32_t idx = hash_key(key, key_len) % DEDUP_HASH_SIZE;
    for (DedupEntry *e = ds->buckets[idx]; e; e = e->next) {
        if (e->key_len == key_len && memcmp(e->key, key, key_len) == 0)
            return e->inst_type;
    }
    return NULL;
}

static void dedup_add(DedupSet *ds, const char *key, int key_len, Type *ty) {
    uint32_t idx = hash_key(key, key_len) % DEDUP_HASH_SIZE;
    DedupEntry *e = arena_alloc(ds->arena, sizeof(DedupEntry));
    memcpy(e->key, key, key_len < MAX_DEDUP_KEY ? key_len : MAX_DEDUP_KEY);
    e->key_len = key_len;
    e->inst_type = ty;
    e->next = ds->buckets[idx];
    ds->buckets[idx] = e;
}

/* ------------------------------------------------------------------ */
/* Phase 3 — Instantiation                                            */
/* ------------------------------------------------------------------ */

/*
 * Extract a concrete Type from a template argument node.
 * Type arguments are stored as ND_VAR_DECL with ty set and name=NULL
 * by parse_template_id (type.c). Expression arguments (non-type
 * template params) return NULL — not yet supported.
 */
static Type *type_arg_from_node(Node *arg) {
    if (!arg) return NULL;
    if (arg->kind == ND_VAR_DECL && arg->var_decl.ty)
        return arg->var_decl.ty;
    return NULL;
}

/*
 * Instantiate one template for a given set of arguments.
 * Returns the cloned ND_CLASS_DEF / ND_FUNC_DEF, or NULL on failure.
 */
static Node *instantiate_one(Node *tmpl, Node *template_id, Arena *arena) {
    if (!tmpl || tmpl->kind != ND_TEMPLATE_DECL) return NULL;

    Node *inner = tmpl->template_decl.decl;
    if (!inner) return NULL;

    int nparams = tmpl->template_decl.nparams;
    int nargs   = template_id->template_id.nargs;

    /* Build the substitution map */
    SubstMap map = subst_map_new(arena, nparams > 0 ? nparams : 1);
    for (int i = 0; i < nparams && i < nargs; i++) {
        Node *param = tmpl->template_decl.params[i];
        if (!param) continue;
        Token *pname = param->param.name;
        if (!pname) continue;

        Type *arg_ty = type_arg_from_node(template_id->template_id.args[i]);
        if (arg_ty)
            subst_map_add(&map, pname, arg_ty);
        /* else: non-type param — not yet handled */
    }

    /* Clone the inner declaration with type substitution */
    Node *cloned = clone_node(inner, &map, arena);
    if (!cloned) return NULL;

    /* For class templates: create a fresh Type for the instantiation
     * so it has its own tag and template_args for mangling. */
    if (cloned->kind == ND_CLASS_DEF) {
        Type *inst_ty = arena_alloc(arena, sizeof(Type));
        if (inner->kind == ND_CLASS_DEF && inner->class_def.ty)
            *inst_ty = *inner->class_def.ty;
        else
            inst_ty->kind = TY_STRUCT;
        inst_ty->tag = template_id->template_id.name;

        /* Store the concrete template args on the type for mangling */
        int n = template_id->template_id.nargs;
        if (n > 0) {
            inst_ty->template_args = arena_alloc(arena, n * sizeof(Type *));
            inst_ty->n_template_args = n;
            for (int i = 0; i < n; i++) {
                inst_ty->template_args[i] =
                    type_arg_from_node(template_id->template_id.args[i]);
            }
        }

        /* Build a class_region for the instantiated class so sema
         * can resolve member references inside method bodies. Walk
         * the cloned members and declare data members + methods. */
        DeclarativeRegion *cr = arena_alloc(arena, sizeof(DeclarativeRegion));
        memset(cr, 0, sizeof(DeclarativeRegion));
        cr->kind = REGION_CLASS;
        cr->owner_type = inst_ty;
        for (int i = 0; i < cloned->class_def.nmembers; i++) {
            Node *m = cloned->class_def.members[i];
            if (!m) continue;
            Token *mname = NULL;
            Type  *mtype = NULL;
            if (m->kind == ND_VAR_DECL || m->kind == ND_TYPEDEF) {
                mname = m->var_decl.name;
                mtype = m->var_decl.ty;
            } else if (m->kind == ND_FUNC_DEF || m->kind == ND_FUNC_DECL) {
                mname = m->func.name;
                mtype = m->func.ret_ty;
            }
            if (mname && mname->kind == TK_IDENT) {
                /* Declare in the class region — use a minimal
                 * inline declaration (no Parser needed). */
                uint32_t idx = 2166136261u;
                for (int j = 0; j < mname->len; j++) {
                    idx ^= (uint8_t)mname->loc[j];
                    idx *= 16777619u;
                }
                idx %= REGION_HASH_SIZE;
                Declaration *d = arena_alloc(arena, sizeof(Declaration));
                d->name     = mname->loc;
                d->name_len = mname->len;
                d->entity   = ENTITY_VARIABLE;
                d->type     = mtype;
                d->home     = cr;
                d->next     = cr->buckets[idx];
                cr->buckets[idx] = d;
            }
        }
        inst_ty->class_region = cr;
        inst_ty->class_def = cloned;
        inst_ty->has_dtor = false;
        inst_ty->has_default_ctor = false;
        inst_ty->has_virtual_methods = false;

        /* Wire up method param_scopes so sema can resolve member
         * references inside method bodies. Each method gets a fresh
         * REGION_PROTOTYPE whose enclosing is the class region, with
         * its parameters declared so name lookup finds them. */
        for (int i = 0; i < cloned->class_def.nmembers; i++) {
            Node *m = cloned->class_def.members[i];
            if (!m || (m->kind != ND_FUNC_DEF && m->kind != ND_FUNC_DECL))
                continue;
            if (!m->func.body) continue;
            DeclarativeRegion *ps = arena_alloc(arena,
                sizeof(DeclarativeRegion));
            memset(ps, 0, sizeof(DeclarativeRegion));
            ps->kind = REGION_PROTOTYPE;
            ps->enclosing = cr;  /* class scope is the parent */
            /* Declare each parameter in the prototype scope */
            for (int j = 0; j < m->func.nparams; j++) {
                Node *p = m->func.params[j];
                if (!p || !p->param.name) continue;
                Token *pn = p->param.name;
                uint32_t pidx = 2166136261u;
                for (int k = 0; k < pn->len; k++) {
                    pidx ^= (uint8_t)pn->loc[k];
                    pidx *= 16777619u;
                }
                pidx %= REGION_HASH_SIZE;
                Declaration *pd = arena_alloc(arena, sizeof(Declaration));
                pd->name     = pn->loc;
                pd->name_len = pn->len;
                pd->entity   = ENTITY_VARIABLE;
                pd->type     = p->param.ty;
                pd->home     = ps;
                pd->next     = ps->buckets[pidx];
                ps->buckets[pidx] = pd;
            }
            m->func.param_scope = ps;
            /* Also set the class_type so codegen knows to mangle
             * this as a method, not a free function. */
            m->func.class_type = inst_ty;
        }

        cloned->class_def.ty  = inst_ty;
        cloned->class_def.tag = template_id->template_id.name;
    }

    return cloned;
}

/* ------------------------------------------------------------------ */
/* Main entry point                                                    */
/* ------------------------------------------------------------------ */

void template_instantiate(Node *tu, Arena *arena) {
    if (!tu || tu->kind != ND_TRANSLATION_UNIT) return;

    /* Phase 1: build template registry */
    TmplRegistry reg = {0};
    reg.arena = arena;
    for (int i = 0; i < tu->tu.ndecls; i++)
        build_registry(&reg, tu->tu.decls[i]);

    /* Phase 2: collect instantiation requests */
    InstCollector col = {0};
    col.arena = arena;
    col.reg = &reg;
    for (int i = 0; i < tu->tu.ndecls; i++)
        collect_from_node(&col, tu->tu.decls[i]);

    /* Phase 3: instantiate (Stage 3 — not yet implemented).
     * For now, diagnostic output in debug mode. */
    (void)col;
    /* Phase 3: instantiate each unique request and prepend to the TU.
     *
     * We build an array of instantiated declarations, then prepend
     * them all before the existing TU declarations so that
     * instantiated types are defined before user code that uses them.
     *
     * Deduplication: a hash set keyed by (template-name, arg-types)
     * ensures each unique instantiation is cloned exactly once. If a
     * duplicate is found, we just patch the usage-site type to point
     * at the existing instantiation. */
    if (col.count == 0) return;

    DedupSet ds = {0};
    ds.arena = arena;

    /* Collect instantiated nodes and patch usage-site types */
    int ninst = 0;
    Node **instantiated = arena_alloc(arena, col.count * sizeof(Node *));
    for (InstRequest *req = col.head; req; req = req->next) {
        /* Dedup check */
        char key[MAX_DEDUP_KEY];
        int key_len = build_dedup_key(req->template_id, key);
        Type *existing = dedup_find(&ds, key, key_len);

        if (existing) {
            /* Already instantiated — just patch the usage-site type */
            if (req->usage_type) {
                req->usage_type->template_args    = existing->template_args;
                req->usage_type->n_template_args   = existing->n_template_args;
                req->usage_type->class_region      = existing->class_region;
                req->usage_type->class_def         = existing->class_def;
                req->usage_type->has_dtor          = existing->has_dtor;
                req->usage_type->has_default_ctor  = existing->has_default_ctor;
            }
            continue;
        }

        Node *inst = instantiate_one(req->tmpl_def, req->template_id, arena);
        if (inst) {
            instantiated[ninst++] = inst;
            /* Patch the usage-site type so codegen mangles it
             * with template args (e.g. sf__Box_t_int_te_). */
            if (inst->kind == ND_CLASS_DEF && inst->class_def.ty &&
                req->usage_type) {
                Type *inst_ty = inst->class_def.ty;
                req->usage_type->template_args    = inst_ty->template_args;
                req->usage_type->n_template_args   = inst_ty->n_template_args;
                req->usage_type->class_region      = inst_ty->class_region;
                req->usage_type->class_def         = inst_ty->class_def;
                req->usage_type->has_dtor          = inst_ty->has_dtor;
                req->usage_type->has_default_ctor  = inst_ty->has_default_ctor;
                /* Register in dedup set */
                dedup_add(&ds, key, key_len, inst_ty);
            }
        }
    }
    if (ninst == 0) return;

    /* Prepend instantiated declarations before existing TU decls */
    int old_n = tu->tu.ndecls;
    int new_n = old_n + ninst;
    Node **new_decls = arena_alloc(arena, new_n * sizeof(Node *));
    /* Instantiated first (reverse the linked-list order so they
     * appear in source-encounter order) */
    for (int i = 0; i < ninst; i++)
        new_decls[i] = instantiated[ninst - 1 - i];
    /* Then original declarations */
    for (int i = 0; i < old_n; i++)
        new_decls[ninst + i] = tu->tu.decls[i];
    tu->tu.decls  = new_decls;
    tu->tu.ndecls = new_n;
}
