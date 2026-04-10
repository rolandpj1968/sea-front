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

/* region_add_base_raw, region_declare_raw, region_build_class,
 * region_build_prototype, region_lookup_own, hash_name are all
 * declared in parse.h and defined in lookup.c. */
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

/* hash_name is declared in parse.h and defined in lookup.c. */

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

/* Find the primary template (nparams > 0) for a given name. */
static Node *registry_find(TmplRegistry *reg, const char *name, int name_len) {
    uint32_t idx = hash_name(name, name_len) % TMPL_REGISTRY_SIZE;
    for (TmplEntry *e = reg->buckets[idx]; e; e = e->next) {
        if (e->name_len == name_len && memcmp(e->name, name, name_len) == 0 &&
            e->tmpl->template_decl.nparams > 0)
            return e->tmpl;
    }
    /* Fallback: return any match (covers cases where only
     * specializations exist, e.g. forward-declared primaries). */
    for (TmplEntry *e = reg->buckets[idx]; e; e = e->next) {
        if (e->name_len == name_len && memcmp(e->name, name, name_len) == 0)
            return e->tmpl;
    }
    return NULL;
}

/*
 * Find a full specialization matching the given template-id args.
 * Specializations are ND_TEMPLATE_DECL with nparams == 0 whose
 * inner class has a template_id_node with matching arg types.
 *
 * Returns the specialization's ND_TEMPLATE_DECL, or NULL.
 */
static Node *registry_find_specialization(TmplRegistry *reg,
                                           const char *name, int name_len,
                                           Node *template_id) {
    uint32_t idx = hash_name(name, name_len) % TMPL_REGISTRY_SIZE;
    for (TmplEntry *e = reg->buckets[idx]; e; e = e->next) {
        if (e->name_len != name_len ||
            memcmp(e->name, name, name_len) != 0)
            continue;
        /* Must be a specialization (nparams == 0) */
        if (e->tmpl->template_decl.nparams != 0) continue;
        /* Get the specialization's template-id args from its inner
         * class type */
        Node *spec_decl = e->tmpl->template_decl.decl;
        if (!spec_decl) continue;
        Type *spec_ty = NULL;
        if (spec_decl->kind == ND_CLASS_DEF)
            spec_ty = spec_decl->class_def.ty;
        else if (spec_decl->kind == ND_VAR_DECL)
            spec_ty = spec_decl->var_decl.ty;
        if (!spec_ty || !spec_ty->template_id_node) continue;
        Node *spec_tid = spec_ty->template_id_node;
        if (spec_tid->kind != ND_TEMPLATE_ID) continue;
        /* Compare arg count */
        if (spec_tid->template_id.nargs != template_id->template_id.nargs)
            continue;
        /* Compare each arg type */
        bool match = true;
        for (int i = 0; i < spec_tid->template_id.nargs && match; i++) {
            Node *sa = spec_tid->template_id.args[i];
            Node *ua = template_id->template_id.args[i];
            Type *st = (sa && sa->kind == ND_VAR_DECL) ? sa->var_decl.ty : NULL;
            Type *ut = (ua && ua->kind == ND_VAR_DECL) ? ua->var_decl.ty : NULL;
            if (!st || !ut) { match = false; break; }
            /* Compare type kinds — simple structural comparison */
            if (st->kind != ut->kind) { match = false; break; }
            if (st->is_unsigned != ut->is_unsigned) { match = false; break; }
            /* For struct/union, compare tags */
            if ((st->kind == TY_STRUCT || st->kind == TY_UNION) &&
                st->tag && ut->tag) {
                if (st->tag->len != ut->tag->len ||
                    memcmp(st->tag->loc, ut->tag->loc, st->tag->len) != 0)
                    match = false;
            }
        }
        if (match) return e->tmpl;
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
        /* Do NOT walk into template bodies during collection — their
         * template-id usages reference dependent types (TY_DEPENDENT)
         * that haven't been substituted yet. The instantiated copy of
         * the template will be scanned for transitive dependencies
         * after cloning in Phase 3. */
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

    case ND_TEMPLATE_ID: {
        /* A template-id in expression position (e.g. max_of<int> as
         * a call callee). Record as a function template instantiation
         * request if the name resolves to a template definition. */
        Token *tname = n->template_id.name;
        if (tname) {
            Node *tmpl = registry_find(col->reg, tname->loc, tname->len);
            if (tmpl) {
                InstRequest *req = arena_alloc(col->arena, sizeof(InstRequest));
                req->name = tname;
                req->template_id = n;
                req->tmpl_def = tmpl;
                req->usage_type = NULL;  /* no usage-site type for functions */
                req->next = col->head;
                col->head = req;
                col->count++;
            }
        }
        break;
    }

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

/*
 * Build a dedup key from the template name + resolved substitution
 * map (which includes defaults). This ensures that e.g. vec<int>
 * and vec<int, int, int> produce the same key when defaults fill
 * in the same types.
 */
static int build_dedup_key_from_map(Token *name, SubstMap *map,
                                     char *buf) {
    int pos = 0;
    /* Template name */
    if (name && pos + name->len < MAX_DEDUP_KEY) {
        memcpy(buf, name->loc, name->len);
        pos = name->len;
    }
    buf[pos++] = '\0';
    /* Resolved args (from substitution map, includes defaults) */
    for (int i = 0; i < map->nentries; i++) {
        pos = type_to_key(map->entries[i].concrete_type,
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

/* Reuse hash_name for dedup key hashing — same FNV-1a algorithm. */
static uint32_t hash_key(const char *key, int len) {
    return hash_name(key, len);
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
 * Find out-of-class method templates for a given class template.
 * These are top-level ND_TEMPLATE_DECL nodes wrapping ND_FUNC_DEF
 * where func.class_type matches the class template's type.
 * Collects into 'out' array, returns count.
 */
static int find_ool_methods(Node *tu, Type *class_type,
                             Node **out, int max) {
    int count = 0;
    if (!tu || !class_type || !class_type->tag) return 0;
    for (int i = 0; i < tu->tu.ndecls; i++) {
        Node *n = tu->tu.decls[i];
        if (!n) continue;
        /* Recurse into namespace blocks */
        if (n->kind == ND_BLOCK) {
            for (int j = 0; j < n->block.nstmts; j++) {
                Node *m = n->block.stmts[j];
                if (!m || m->kind != ND_TEMPLATE_DECL) continue;
                Node *inner = m->template_decl.decl;
                if (!inner) continue;
                if ((inner->kind == ND_FUNC_DEF || inner->kind == ND_FUNC_DECL) &&
                    inner->func.class_type &&
                    inner->func.class_type->tag &&
                    inner->func.class_type->tag->len == class_type->tag->len &&
                    memcmp(inner->func.class_type->tag->loc,
                           class_type->tag->loc, class_type->tag->len) == 0) {
                    if (count < max) out[count++] = m;
                }
            }
            continue;
        }
        if (n->kind != ND_TEMPLATE_DECL) continue;
        Node *inner = n->template_decl.decl;
        if (!inner) continue;
        if ((inner->kind == ND_FUNC_DEF || inner->kind == ND_FUNC_DECL) &&
            inner->func.class_type &&
            inner->func.class_type->tag &&
            inner->func.class_type->tag->len == class_type->tag->len &&
            memcmp(inner->func.class_type->tag->loc,
                   class_type->tag->loc, class_type->tag->len) == 0) {
            if (count < max) out[count++] = n;
        }
    }
    return count;
}

/*
 * Instantiate one template for a given set of arguments.
 * Returns the cloned ND_CLASS_DEF / ND_FUNC_DEF, or NULL on failure.
 * 'tu' is passed for finding out-of-class method templates.
 */
static Node *instantiate_one(Node *tmpl, Node *template_id,
                              Arena *arena, Node *tu,
                              Node ***extra_out, int *nextra) {
    if (!tmpl || tmpl->kind != ND_TEMPLATE_DECL) return NULL;

    Node *inner = tmpl->template_decl.decl;
    if (!inner) return NULL;

    int nparams = tmpl->template_decl.nparams;
    int nargs   = template_id->template_id.nargs;

    /* Build the substitution map.
     * For each template parameter, use the corresponding argument if
     * provided; otherwise fall back to the parameter's default type
     * (§17.1/8 [temp.param]). */
    SubstMap map = subst_map_new(arena, nparams > 0 ? nparams : 1);
    for (int i = 0; i < nparams; i++) {
        Node *param = tmpl->template_decl.params[i];
        if (!param) continue;
        Token *pname = param->param.name;
        if (!pname) continue;

        Type *arg_ty = NULL;
        if (i < nargs)
            arg_ty = type_arg_from_node(template_id->template_id.args[i]);
        /* Fall back to default if no explicit argument */
        if (!arg_ty && param->param.default_type)
            arg_ty = subst_type(param->param.default_type, &map, arena);
        if (arg_ty)
            subst_map_add(&map, pname, arg_ty);
        /* else: non-type param or missing default — not yet handled */
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

        /* Store the concrete template args on the type for mangling.
         * Use the substitution map (which includes defaults) rather
         * than just the explicit args from the template-id. */
        int n = map.nentries;
        if (n > 0) {
            inst_ty->template_args = arena_alloc(arena, n * sizeof(Type *));
            inst_ty->n_template_args = n;
            for (int i = 0; i < n; i++)
                inst_ty->template_args[i] = map.entries[i].concrete_type;
        }

        /* Build a class_region for the instantiated class so sema
         * can resolve member references inside method bodies. */
        DeclarativeRegion *cr = region_build_class(cloned, inst_ty, arena);
        inst_ty->class_region = cr;
        inst_ty->class_def = cloned;
        inst_ty->has_dtor = false;
        inst_ty->has_default_ctor = false;
        inst_ty->has_virtual_methods = false;

        /* Process base classes: for each base type on the cloned
         * class_def, find or create its class_region and add it to
         * the instantiated class_region's bases list. For template
         * bases (e.g. Base<T> → Base<int>), the base type was
         * already substituted by clone.c's subst_type. If the base
         * has a template_id_node, it needs to be instantiated too
         * (this happens transitively in the fixpoint loop). For
         * concrete bases, just link the existing class_region. */
        for (int bi = 0; bi < cloned->class_def.nbase_types; bi++) {
            Type *base_ty = cloned->class_def.base_types[bi];
            if (!base_ty) continue;
            if (base_ty->class_region) {
                /* Concrete base with known class_region */
                region_add_base_raw(cr, base_ty->class_region, arena);
            }
            /* Template bases will be resolved after the fixpoint
             * loop instantiates them and patch_all_types runs. */
        }

        /* Wire up method param_scopes and class_type on each method. */
        for (int i = 0; i < cloned->class_def.nmembers; i++) {
            Node *m = cloned->class_def.members[i];
            if (!m || (m->kind != ND_FUNC_DEF && m->kind != ND_FUNC_DECL))
                continue;
            if (!m->func.body) continue;
            m->func.param_scope = region_build_prototype(m, cr, arena);
            m->func.class_type = inst_ty;
        }

        cloned->class_def.ty  = inst_ty;
        cloned->class_def.tag = template_id->template_id.name;

        /* Instantiate out-of-class method definitions for this class
         * template. These are separate ND_TEMPLATE_DECL nodes at top
         * level whose inner ND_FUNC_DEF has class_type matching us. */
        if (tu && extra_out && nextra) {
            Type *orig_class_type = inner->kind == ND_CLASS_DEF ?
                inner->class_def.ty : NULL;
            if (orig_class_type) {
                enum { MAX_OOL = 64 };
                Node *ool[MAX_OOL];
                int nool = find_ool_methods(tu, orig_class_type,
                                             ool, MAX_OOL);
                if (nool > 0) {
                    *extra_out = arena_alloc(arena, nool * sizeof(Node *));
                    *nextra = 0;
                    for (int k = 0; k < nool; k++) {
                        Node *method_tmpl = ool[k];
                        Node *method_inner = method_tmpl->template_decl.decl;
                        Node *method_cloned = clone_node(method_inner,
                                                          &map, arena);
                        if (!method_cloned) continue;
                        /* Set class_type to the instantiated type */
                        method_cloned->func.class_type = inst_ty;
                        /* Set up param scope for sema */
                        if (method_cloned->func.body) {
                            method_cloned->func.param_scope =
                                region_build_prototype(method_cloned, cr, arena);
                        }
                        (*extra_out)[(*nextra)++] = method_cloned;
                    }
                }
            }
        }
    }

    /* For function templates: set up a param scope and rewrite the
     * template-id call site to reference the mangled name.
     * The cloned function gets emitted as a top-level free function
     * with a mangled name encoding the template args. */
    if (cloned->kind == ND_FUNC_DEF || cloned->kind == ND_FUNC_DECL) {
        /* Build a synthetic mangled name for the function.
         * For now we build it by snprintf'ing into an arena buffer.
         * E.g. max_of<int> → max_of_t_int_te_ */
        Token *fname = cloned->func.name;
        int n = template_id->template_id.nargs;
        int bufsize = 256;
        char *buf = arena_alloc(arena, bufsize);
        int pos = 0;
        if (fname)
            pos += snprintf(buf + pos, bufsize - pos, "%.*s",
                            fname->len, fname->loc);
        pos += snprintf(buf + pos, bufsize - pos, "_t_");
        for (int i = 0; i < n; i++) {
            if (i > 0) buf[pos++] = '_';
            Type *at = type_arg_from_node(template_id->template_id.args[i]);
            pos = type_to_key(at, buf, pos, bufsize);
        }
        pos += snprintf(buf + pos, bufsize - pos, "_te_");

        /* Create a synthetic token pointing at the mangled name.
         * We reuse the original token but override loc/len. */
        Token *mangled = arena_alloc(arena, sizeof(Token));
        if (fname) *mangled = *fname;
        else memset(mangled, 0, sizeof(Token));
        mangled->loc = buf;
        mangled->len = pos;
        mangled->kind = TK_IDENT;
        cloned->func.name = mangled;

        /* Rewrite the template-id node itself so codegen emits
         * the mangled name at call sites. We do this by converting
         * the ND_TEMPLATE_ID into an ND_IDENT pointing at the
         * mangled name. */
        template_id->kind = ND_IDENT;
        template_id->ident.name = mangled;
        template_id->ident.implicit_this = false;
        template_id->ident.resolved_decl = NULL;

        /* Set up param scope for sema */
        if (cloned->func.body && cloned->func.nparams > 0)
            cloned->func.param_scope = region_build_prototype(
                cloned, /*enclosing=*/NULL, arena);
    }

    return cloned;
}

/* region_add_base_raw moved to lookup.c */

/* Forward declarations for post-instantiation type patching */
static void patch_all_types(Node *tu, DedupSet *ds, Arena *arena);

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

    /* Phases 2+3 loop: collect instantiation requests and instantiate.
     * Iterate until no new instantiations are produced — this handles
     * transitive dependencies (e.g. Outer<int> instantiates Box<int>
     * as a member, which itself needs instantiation).
     *
     * Deduplication: a hash set keyed by (template-name, arg-types)
     * ensures each unique instantiation is cloned exactly once. */
    DedupSet ds = {0};
    ds.arena = arena;

    /* Total instantiated across all iterations */
    int total_inst = 0;
    enum { MAX_INST = 4096 };
    Node **all_instantiated = arena_alloc(arena, MAX_INST * sizeof(Node *));

    for (int iteration = 0; iteration < 32; iteration++) {
    /* Phase 2: collect instantiation requests from current TU */
    InstCollector col = {0};
    col.arena = arena;
    col.reg = &reg;
    for (int i = 0; i < tu->tu.ndecls; i++)
        collect_from_node(&col, tu->tu.decls[i]);
    if (col.count == 0) break;

    /* Phase 3: instantiate and patch */
    int ninst_this_round = 0;
    for (InstRequest *req = col.head; req; req = req->next) {
        /* Build a temporary SubstMap to compute the dedup key
         * (includes defaults). This is rebuilt inside instantiate_one
         * if we proceed — minor redundancy for cleaner code. */
        Node *tmpl = req->tmpl_def;
        int np = tmpl->template_decl.nparams;
        int na = req->template_id->template_id.nargs;
        SubstMap tmp_map = subst_map_new(arena, np > 0 ? np : 1);
        for (int i = 0; i < np; i++) {
            Node *param = tmpl->template_decl.params[i];
            if (!param || !param->param.name) continue;
            Type *arg_ty = (i < na) ?
                type_arg_from_node(req->template_id->template_id.args[i]) :
                NULL;
            if (!arg_ty && param->param.default_type)
                arg_ty = subst_type(param->param.default_type, &tmp_map, arena);
            if (arg_ty)
                subst_map_add(&tmp_map, param->param.name, arg_ty);
        }

        /* Dedup check — uses resolved map (includes defaults) */
        char key[MAX_DEDUP_KEY];
        int key_len = build_dedup_key_from_map(req->name, &tmp_map, key);
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

        /* Check for a full specialization that matches the requested
         * args. If found, use the specialization's concrete definition
         * directly instead of cloning the primary template. */
        Node *spec = registry_find_specialization(
            &reg, req->name->loc, req->name->len, req->template_id);

        Node **extra_methods = NULL;
        int nextra = 0;
        Node *inst;
        if (spec) {
            /* Full specialization — use the concrete class directly.
             * No cloning or substitution needed. */
            inst = spec->template_decl.decl;
            if (inst && inst->kind == ND_CLASS_DEF) {
                /* Set template_args on the type for mangling */
                Type *sty = inst->class_def.ty;
                if (sty) {
                    int n = req->template_id->template_id.nargs;
                    if (n > 0 && sty->n_template_args == 0) {
                        sty->template_args = arena_alloc(arena,
                            n * sizeof(Type *));
                        sty->n_template_args = n;
                        for (int i = 0; i < n; i++)
                            sty->template_args[i] = type_arg_from_node(
                                req->template_id->template_id.args[i]);
                    }
                    /* Build class_region if not already present */
                    if (!sty->class_region) {
                        sty->class_region = region_build_class(
                            inst, sty, arena);
                        sty->class_def = inst;
                    }
                    /* Wire method param scopes */
                    for (int i = 0; i < inst->class_def.nmembers; i++) {
                        Node *m = inst->class_def.members[i];
                        if (!m || m->kind != ND_FUNC_DEF || !m->func.body)
                            continue;
                        if (m->func.param_scope) continue;
                        m->func.param_scope = region_build_prototype(
                            m, sty->class_region, arena);
                        m->func.class_type = sty;
                    }
                }
            }
        } else {
            inst = instantiate_one(req->tmpl_def, req->template_id,
                                      arena, tu, &extra_methods, &nextra);
        }
        if (inst) {
            if (total_inst < MAX_INST)
                all_instantiated[total_inst++] = inst;
            ninst_this_round++;
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
                /* Register in dedup set — both the resolved key (for
                 * dedup across explicit+defaulted args) and the raw
                 * template-id key (for post-instantiation patching
                 * which only has the raw args). */
                dedup_add(&ds, key, key_len, inst_ty);
                /* Also register by raw template-id args */
                {
                    char raw_key[MAX_DEDUP_KEY];
                    int rpos = 0;
                    if (req->name && rpos + req->name->len < MAX_DEDUP_KEY) {
                        memcpy(raw_key, req->name->loc, req->name->len);
                        rpos = req->name->len;
                    }
                    raw_key[rpos++] = '\0';
                    for (int ri = 0; ri < req->template_id->template_id.nargs; ri++) {
                        Node *rarg = req->template_id->template_id.args[ri];
                        Type *rty = (rarg && rarg->kind == ND_VAR_DECL) ?
                                    rarg->var_decl.ty : NULL;
                        rpos = type_to_key(rty, raw_key, rpos, MAX_DEDUP_KEY);
                        raw_key[rpos++] = '\0';
                    }
                    dedup_add(&ds, raw_key, rpos, inst_ty);
                }
            }
            /* Add out-of-class method instantiations */
            for (int e = 0; e < nextra; e++) {
                if (total_inst < MAX_INST)
                    all_instantiated[total_inst++] = extra_methods[e];
                ninst_this_round++;
            }
        }
    }
    if (ninst_this_round == 0) break;

    /* Insert this round's instantiations into the TU. They go after
     * all class definitions (so concrete base classes are defined
     * first) but before function definitions (so function bodies
     * can reference the instantiated types). Find the last
     * ND_CLASS_DEF / ND_BLOCK in the source and insert after it. */
    int insert_pos = 0;
    for (int i = 0; i < tu->tu.ndecls; i++) {
        Node *d = tu->tu.decls[i];
        if (d && (d->kind == ND_CLASS_DEF || d->kind == ND_BLOCK))
            insert_pos = i + 1;
    }
    /* If no classes found, insert at front (same as before) */
    int old_n = tu->tu.ndecls;
    int new_n = old_n + ninst_this_round;
    Node **new_decls = arena_alloc(arena, new_n * sizeof(Node *));
    int idx = 0;
    /* Source decls before insert point */
    for (int i = 0; i < insert_pos && i < old_n; i++)
        new_decls[idx++] = tu->tu.decls[i];
    /* Instantiations */
    for (int i = total_inst - ninst_this_round; i < total_inst; i++)
        new_decls[idx++] = all_instantiated[i];
    /* Source decls after insert point */
    for (int i = insert_pos; i < old_n; i++)
        new_decls[idx++] = tu->tu.decls[i];
    tu->tu.decls  = new_decls;
    tu->tu.ndecls = new_n;
    } /* end iteration loop */

    /* Reverse the instantiated array so that transitive dependencies
     * (discovered in later rounds) appear before the types that
     * reference them. This is a simple heuristic that works because
     * the fixpoint loop discovers leaf types (e.g. holder<int>)
     * AFTER the containing types (e.g. container<int>).
     *
     * Also do a topological sort: reorder all_instantiated so that template
     * instantiations that are used as by-value members of other
     * instantiations come first. Simple O(n^2) approach: for each
     * pair, check if A's struct definition contains a by-value
     * member whose mangled tag matches B. If so, B must come before A.
     *
     * We use a simple bubble-sort-like pass: move items forward if
     * they have no unresolved dependencies. Repeat until stable. */
    for (int pass = 0; pass < total_inst; pass++) {
        bool changed = false;
        for (int i = 0; i < total_inst; i++) {
            Node *a = all_instantiated[i];
            if (!a || a->kind != ND_CLASS_DEF || !a->class_def.ty) continue;
            /* Find any later class def that A depends on */
            for (int j = i + 1; j < total_inst; j++) {
                Node *b = all_instantiated[j];
                if (!b || b->kind != ND_CLASS_DEF || !b->class_def.ty) continue;
                Type *bty = b->class_def.ty;
            /* Check if A contains B as a by-value member.
             * Simple approach: check if any struct/union member of A
             * has the same base tag name as B and also has
             * template_id_node or template_args (i.e., it's a
             * template instantiation of the same template). */
            bool a_needs_b = false;
            for (int m = 0; m < a->class_def.nmembers && !a_needs_b; m++) {
                Node *mem = a->class_def.members[m];
                if (!mem || mem->kind != ND_VAR_DECL) continue;
                Type *mty = mem->var_decl.ty;
                if (!mty) continue;
                if (mty->kind != TY_STRUCT && mty->kind != TY_UNION)
                    continue;
                /* Match by tag name — if the member is an
                 * instantiation of the same template as B, A depends
                 * on B (or another instantiation with the same tag,
                 * which is close enough for ordering). */
                if (mty->tag && bty->tag &&
                    mty->tag->len == bty->tag->len &&
                    memcmp(mty->tag->loc, bty->tag->loc,
                           mty->tag->len) == 0)
                    a_needs_b = true;
            }
            if (a_needs_b) {
                /* Move B to just before A by shifting elements */
                Node *save = all_instantiated[j];
                for (int k = j; k > i; k--)
                    all_instantiated[k] = all_instantiated[k - 1];
                all_instantiated[i] = save;
                changed = true;
                break;  /* restart inner loop for A's new position */
            }
            }
        }
        if (!changed) break;
    }

    /* Rebuild the TU decl list: insert sorted instantiations after
     * the last class/block (so concrete base classes are defined
     * first) and before function definitions. */
    {
        int old_n = tu->tu.ndecls;
        /* Find insert position: after the last ND_CLASS_DEF or
         * ND_BLOCK among user (non-instantiated) decls. */
        int insert_pos = 0;
        for (int i = 0; i < old_n; i++) {
            Node *d = tu->tu.decls[i];
            /* Skip instantiated decls when finding position */
            bool is_inst = false;
            for (int j = 0; j < total_inst; j++) {
                if (d == all_instantiated[j]) { is_inst = true; break; }
            }
            if (is_inst) continue;
            if (d && (d->kind == ND_CLASS_DEF || d->kind == ND_BLOCK))
                insert_pos = i + 1;
        }
        /* Count non-instantiated decls */
        int user_n = 0;
        for (int i = 0; i < old_n; i++) {
            bool is_inst = false;
            for (int j = 0; j < total_inst; j++) {
                if (tu->tu.decls[i] == all_instantiated[j]) {
                    is_inst = true; break;
                }
            }
            if (!is_inst) user_n++;
        }
        int new_n = total_inst + user_n;
        Node **new_decls = arena_alloc(arena, new_n * sizeof(Node *));
        int idx = 0;
        /* User decls before insert point */
        for (int i = 0; i < old_n; i++) {
            if (idx == insert_pos) {
                /* Insert instantiations here */
                for (int j = 0; j < total_inst; j++)
                    new_decls[idx++] = all_instantiated[j];
            }
            bool is_inst = false;
            for (int j = 0; j < total_inst; j++) {
                if (tu->tu.decls[i] == all_instantiated[j]) {
                    is_inst = true; break;
                }
            }
            if (!is_inst) new_decls[idx++] = tu->tu.decls[i];
        }
        /* If insert_pos >= user_n, append at end */
        if (idx == new_n - total_inst) {
            for (int j = 0; j < total_inst; j++)
                new_decls[idx++] = all_instantiated[j];
        }
        tu->tu.decls = new_decls;
        tu->tu.ndecls = new_n;
    }

    /* Post-instantiation: walk the ENTIRE AST and patch every Type
     * with a template_id_node to point at the correct instantiated
     * class_region / template_args. This catches Types that weren't
     * the exact pointer collected in Phase 2 (e.g. Declarations
     * store a copy of the Type from parse_type_specifiers). */
    patch_all_types(tu, &ds, arena);
}

/*
 * Recursively walk all types in the AST and patch any TY_STRUCT with
 * a template_id_node whose key matches an entry in the dedup set.
 */
static void patch_type(Type *ty, DedupSet *ds, Arena *arena) {
    if (!ty) return;
    /* Recurse into compound types */
    switch (ty->kind) {
    case TY_PTR: case TY_REF: case TY_RVALREF: case TY_ARRAY:
        patch_type(ty->base, ds, arena);
        return;
    case TY_FUNC:
        patch_type(ty->ret, ds, arena);
        for (int i = 0; i < ty->nparams; i++)
            patch_type(ty->params[i], ds, arena);
        return;
    default: break;
    }
    if (ty->kind != TY_STRUCT && ty->kind != TY_UNION) return;
    if (!ty->template_id_node) return;
    if (ty->class_region) return;  /* already patched */
    /* already patched */
    /* Build key and look up */
    Node *tid = ty->template_id_node;
    if (tid->kind != ND_TEMPLATE_ID || !tid->template_id.name)
        return;
    /* We need to build the same key that the dedup set uses.
     * Reconstruct the SubstMap from the template definition. */
    Token *tname = tid->template_id.name;
    /* Simple key: just use template name + arg types */
    char key[MAX_DEDUP_KEY];
    int pos = 0;
    if (tname && pos + tname->len < MAX_DEDUP_KEY) {
        memcpy(key, tname->loc, tname->len);
        pos = tname->len;
    }
    key[pos++] = '\0';
    for (int i = 0; i < tid->template_id.nargs; i++) {
        Node *arg = tid->template_id.args[i];
        Type *arg_ty = (arg && arg->kind == ND_VAR_DECL) ?
                        arg->var_decl.ty : NULL;
        pos = type_to_key(arg_ty, key, pos, MAX_DEDUP_KEY);
        key[pos++] = '\0';
    }
    Type *existing = dedup_find(ds, key, pos);
    if (existing) {
        ty->template_args    = existing->template_args;
        ty->n_template_args  = existing->n_template_args;
        ty->class_region     = existing->class_region;
        ty->class_def        = existing->class_def;
        ty->has_dtor         = existing->has_dtor;
        ty->has_default_ctor = existing->has_default_ctor;
    }
}

static void patch_node_types(Node *n, DedupSet *ds, Arena *arena) {
    if (!n) return;
    switch (n->kind) {
    case ND_VAR_DECL: case ND_TYPEDEF:
        patch_type(n->var_decl.ty, ds, arena);
        patch_node_types(n->var_decl.init, ds, arena);
        break;
    case ND_PARAM:
        patch_type(n->param.ty, ds, arena);
        break;
    case ND_FUNC_DEF: case ND_FUNC_DECL:
        patch_type(n->func.ret_ty, ds, arena);
        for (int i = 0; i < n->func.nparams; i++)
            patch_node_types(n->func.params[i], ds, arena);
        patch_node_types(n->func.body, ds, arena);
        break;
    case ND_CLASS_DEF:
        for (int i = 0; i < n->class_def.nmembers; i++)
            patch_node_types(n->class_def.members[i], ds, arena);
        break;
    case ND_BLOCK:
        for (int i = 0; i < n->block.nstmts; i++)
            patch_node_types(n->block.stmts[i], ds, arena);
        break;
    case ND_CAST:
        patch_type(n->cast.ty, ds, arena);
        patch_node_types(n->cast.operand, ds, arena);
        break;
    case ND_BINARY: case ND_ASSIGN: case ND_COMMA:
        patch_node_types(n->binary.lhs, ds, arena);
        patch_node_types(n->binary.rhs, ds, arena);
        break;
    case ND_UNARY: case ND_POSTFIX:
        patch_node_types(n->unary.operand, ds, arena);
        break;
    case ND_TERNARY:
        patch_node_types(n->ternary.cond, ds, arena);
        patch_node_types(n->ternary.then_, ds, arena);
        patch_node_types(n->ternary.else_, ds, arena);
        break;
    case ND_CALL:
        patch_node_types(n->call.callee, ds, arena);
        for (int i = 0; i < n->call.nargs; i++)
            patch_node_types(n->call.args[i], ds, arena);
        break;
    case ND_MEMBER:
        patch_node_types(n->member.obj, ds, arena);
        break;
    case ND_SUBSCRIPT:
        patch_node_types(n->subscript.base, ds, arena);
        patch_node_types(n->subscript.index, ds, arena);
        break;
    case ND_IF:
        patch_node_types(n->if_.init, ds, arena);
        patch_node_types(n->if_.cond, ds, arena);
        patch_node_types(n->if_.then_, ds, arena);
        patch_node_types(n->if_.else_, ds, arena);
        break;
    case ND_WHILE:
        patch_node_types(n->while_.cond, ds, arena);
        patch_node_types(n->while_.body, ds, arena);
        break;
    case ND_FOR:
        patch_node_types(n->for_.init, ds, arena);
        patch_node_types(n->for_.cond, ds, arena);
        patch_node_types(n->for_.inc, ds, arena);
        patch_node_types(n->for_.body, ds, arena);
        break;
    case ND_RETURN:
        patch_node_types(n->ret.expr, ds, arena);
        break;
    case ND_EXPR_STMT:
        patch_node_types(n->expr_stmt.expr, ds, arena);
        break;
    case ND_SWITCH:
        patch_node_types(n->switch_.init, ds, arena);
        patch_node_types(n->switch_.expr, ds, arena);
        patch_node_types(n->switch_.body, ds, arena);
        break;
    case ND_CASE:
        patch_node_types(n->case_.expr, ds, arena);
        patch_node_types(n->case_.stmt, ds, arena);
        break;
    case ND_DEFAULT:
        patch_node_types(n->default_.stmt, ds, arena);
        break;
    case ND_TEMPLATE_DECL:
        /* Don't patch inside template bodies */
        break;
    case ND_FRIEND:
        patch_node_types(n->friend_decl.decl, ds, arena);
        break;
    case ND_TRANSLATION_UNIT:
        for (int i = 0; i < n->tu.ndecls; i++)
            patch_node_types(n->tu.decls[i], ds, arena);
        break;
    default:
        break;
    }
}

static void patch_all_types(Node *tu, DedupSet *ds, Arena *arena) {
    patch_node_types(tu, ds, arena);
}
