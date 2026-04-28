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

#include <stdio.h>
#include <string.h>

#include "instantiate.h"
#include "clone.h"
#include "../codegen/mangle.h"
#include "../sema/sema.h"

/* region_add_base_raw, region_declare_raw, region_build_class,
 * region_build_prototype, region_lookup_own, hash_name are all
 * declared in parse.h and defined in lookup.c. */

/* ------------------------------------------------------------------ */
/* Template Registry — Phase 1                                        */
/* ------------------------------------------------------------------ */

#define TMPL_REGISTRY_SIZE 64

typedef struct TmplEntry TmplEntry;
struct TmplEntry {
    const char *name;
    int         name_len;
    Node       *tmpl;         /* ND_TEMPLATE_DECL */
    Type       *owner_class;  /* non-NULL for member templates (N4659 §17.5.2) */
    TmplEntry  *next;         /* hash chain */
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

/* Build a (class, member) compound key as a single byte run
 * "ClassName\0memberName". The embedded NUL is a separator, not a
 * terminator — the caller tracks the full length. This keeps the
 * key short (no mangling) while guaranteeing (classA::f, classB::g)
 * can't collide via concatenation.
 *
 * Writes into dst[0..MEMBER_KEY_CAP]; aborts if the encoding doesn't
 * fit rather than silently truncating (a truncated key would produce
 * a cross-class collision and a very confusing debugging session).
 * Returns the length written. */
#define MEMBER_KEY_CAP 256
static int build_member_key(char *dst,
                             const char *class_name, int class_len,
                             const char *member_name, int member_len) {
    int key_len = class_len + 1 + member_len;
    if (key_len > MEMBER_KEY_CAP) {
        fprintf(stderr, "sea-front: member-template key overflow "
                "(class=%.*s member=%.*s len=%d cap=%d)\n",
                class_len, class_name, member_len, member_name,
                key_len, MEMBER_KEY_CAP);
        abort();
    }
    memcpy(dst, class_name, class_len);
    dst[class_len] = '\0';
    memcpy(dst + class_len + 1, member_name, member_len);
    return key_len;
}

/* Register a member template with a compound (class, member) key.
 * N4659 §17.5.2 [temp.mem] — a template can be declared within a class. */
static void registry_add_member(TmplRegistry *reg,
                                 const char *class_name, int class_len,
                                 const char *member_name, int member_len,
                                 Node *tmpl, Type *owner_class) {
    char tmp[MEMBER_KEY_CAP];
    int key_len = build_member_key(tmp, class_name, class_len,
                                    member_name, member_len);
    /* Copy into arena so the entry outlives this stack frame. */
    char *key = arena_alloc(reg->arena, key_len);
    memcpy(key, tmp, key_len);

    uint32_t idx = hash_name(key, key_len) % TMPL_REGISTRY_SIZE;
    TmplEntry *e = arena_alloc(reg->arena, sizeof(TmplEntry));
    e->name = key;
    e->name_len = key_len;
    e->tmpl = tmpl;
    e->owner_class = owner_class;
    e->next = reg->buckets[idx];
    reg->buckets[idx] = e;
}

/* Find a member template by class name + member name.
 * N4659 §6.4.3 [basic.lookup.qual] — qualified name lookup in
 * a class scope. Returns the TmplEntry (not just the Node) so the
 * caller can access owner_class. */
static TmplEntry *registry_find_member(TmplRegistry *reg,
                                        const char *class_name, int class_len,
                                        const char *member_name, int member_len) {
    char key[MEMBER_KEY_CAP];
    int key_len = build_member_key(key, class_name, class_len,
                                    member_name, member_len);

    uint32_t idx = hash_name(key, key_len) % TMPL_REGISTRY_SIZE;
    for (TmplEntry *e = reg->buckets[idx]; e; e = e->next) {
        if (e->name_len == key_len &&
            memcmp(e->name, key, key_len) == 0)
            return e;
    }
    return NULL;
}

/* Find the primary template for a given name. The primary is the
 * ND_TEMPLATE_DECL whose inner class/func has NO template_id_node —
 * i.e., the declarator-id wasn't a template-id (partial specs and
 * full specs ARE template-ids). Falls back to any match when only
 * specializations exist.
 *
 * N4659 §17.8.3 [temp.class.spec] — a class template partial
 * specialization names a different template from the primary; the
 * primary template is the one without a template argument list.
 * N4659 §17.8.3/1: "... a partial specialization of the template."
 * We use 'has template_id_node' as the syntactic proxy for 'is a
 * specialization'. */
static Node *registry_find(TmplRegistry *reg, const char *name, int name_len) {
    uint32_t idx = hash_name(name, name_len) % TMPL_REGISTRY_SIZE;
    for (TmplEntry *e = reg->buckets[idx]; e; e = e->next) {
        if (e->name_len != name_len ||
            memcmp(e->name, name, name_len) != 0) continue;
        Node *decl = e->tmpl->template_decl.decl;
        if (!decl) continue;
        Type *dty = NULL;
        if (decl->kind == ND_CLASS_DEF)       dty = decl->class_def.ty;
        else if (decl->kind == ND_VAR_DECL)   dty = decl->var_decl.ty;
        /* Primary: no template_id_node on the inner declaration's type */
        if (dty && !dty->template_id_node &&
            e->tmpl->template_decl.nparams > 0)
            return e->tmpl;
        /* Function templates have no class Type with a template_id_node,
         * so accept any func-def/decl with nparams > 0 as primary. */
        if ((decl->kind == ND_FUNC_DEF || decl->kind == ND_FUNC_DECL) &&
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
 * Match a specialization pattern type against a concrete usage type.
 * Returns true if they match. TY_DEPENDENT in the pattern matches
 * any concrete type (wildcard for partial specializations).
 */
static bool types_match(Type *pattern, Type *concrete) {
    if (!pattern || !concrete) return (!pattern && !concrete);
    /* TY_DEPENDENT in pattern = wildcard (partial spec variable) */
    if (pattern->kind == TY_DEPENDENT) return true;
    /* Kind must match */
    if (pattern->kind != concrete->kind) return false;
    if (pattern->is_unsigned != concrete->is_unsigned) return false;
    /* For struct/union, compare tags */
    if ((pattern->kind == TY_STRUCT || pattern->kind == TY_UNION) &&
        pattern->tag && concrete->tag) {
        if (pattern->tag->len != concrete->tag->len ||
            memcmp(pattern->tag->loc, concrete->tag->loc,
                   pattern->tag->len) != 0)
            return false;
    }
    /* For pointers/refs, compare base types recursively */
    if (pattern->kind == TY_PTR || pattern->kind == TY_REF ||
        pattern->kind == TY_RVALREF)
        return types_match(pattern->base, concrete->base);
    return true;
}

/*
 * Find the best specialization (full or partial) matching the given
 * template-id args. Checks all ND_TEMPLATE_DECL entries with the
 * same name whose inner class has a template_id_node.
 *
 * Full specializations (nparams == 0) are preferred over partial
 * specializations (nparams > 0). Among partial specs, the most
 * specialized (fewer remaining params) wins.
 *
 * Returns the specialization's ND_TEMPLATE_DECL, or NULL.
 */
static Node *registry_find_specialization(TmplRegistry *reg,
                                           const char *name, int name_len,
                                           Node *template_id) {
    Node *best = NULL;
    int best_nparams = -1;  /* -1 = no match yet */

    uint32_t idx = hash_name(name, name_len) % TMPL_REGISTRY_SIZE;
    for (TmplEntry *e = reg->buckets[idx]; e; e = e->next) {
        if (e->name_len != name_len ||
            memcmp(e->name, name, name_len) != 0)
            continue;
        Node *tmpl = e->tmpl;
        /* Skip the primary template (no template_id_node on inner class) */
        Node *spec_decl = tmpl->template_decl.decl;
        if (!spec_decl) continue;
        Type *spec_ty = NULL;
        if (spec_decl->kind == ND_CLASS_DEF)
            spec_ty = spec_decl->class_def.ty;
        else if (spec_decl->kind == ND_VAR_DECL)
            spec_ty = spec_decl->var_decl.ty;
        if (!spec_ty || !spec_ty->template_id_node) continue;
        Node *spec_tid = spec_ty->template_id_node;
        if (spec_tid->kind != ND_TEMPLATE_ID) continue;
        /* Arg count must match */
        if (spec_tid->template_id.nargs != template_id->template_id.nargs)
            continue;
        /* Match each arg: concrete positions must match exactly,
         * TY_DEPENDENT positions are wildcards. */
        bool match = true;
        for (int i = 0; i < spec_tid->template_id.nargs && match; i++) {
            Node *sa = spec_tid->template_id.args[i];
            Node *ua = template_id->template_id.args[i];
            Type *st = (sa && sa->kind == ND_VAR_DECL) ? sa->var_decl.ty : NULL;
            Type *ut = (ua && ua->kind == ND_VAR_DECL) ? ua->var_decl.ty : NULL;
            if (!types_match(st, ut)) match = false;
        }
        if (!match) continue;
        /* Prefer full specializations (nparams == 0) over partial.
         * Among partials, prefer more specialized (fewer params) as
         * a SHORTCUT for "most specialized". N4659 §17.8.3.2
         * [temp.class.order] specifies the actual partial-order rule
         * via §16.5.6.2 [temp.func.order]: one spec P1 is more
         * specialized than P2 iff deducing P1's args from P2's
         * pattern succeeds AND the reverse fails. Our nparam-count
         * proxy is right for the simple cases the bootstrap throws
         * at us (e.g. vec<T,A,vl_embed> more specialized than
         * vec<T,A,L>) but would mispick when two partial specs have
         * the same nparams but different specificity.
         * TODO(seafront#partial-order): real §16.5.6.2 ordering. */
        int np = tmpl->template_decl.nparams;
        if (best == NULL || np < best_nparams) {
            best = tmpl;
            best_nparams = np;
        }
    }
    return best;
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
        /* If the template wraps a class def, descend so any member
         * templates inside the class template are also registered.
         * N4659 §17.5.2 [temp.mem] permits 'template<class T> struct
         * Box { template<class U> static T *cast(U *); };' — both
         * heads need separate registration so a qualified call like
         * Box<int>::cast<float>(p) can find the member template. */
        Node *inner = n->template_decl.decl;
        if (inner && inner->kind == ND_CLASS_DEF)
            build_registry(reg, inner);
        break;
    }
    case ND_BLOCK:
        for (int i = 0; i < n->block.nstmts; i++)
            build_registry(reg, n->block.stmts[i]);
        break;
    case ND_CLASS_DEF:
        /* N4659 §17.5.2 [temp.mem]: a template can be declared within
         * a class or class template. Walk class members and register
         * any ND_TEMPLATE_DECL as a member template with a compound
         * key so they're findable via registry_find_member. */
        if (n->class_def.ty && n->class_def.tag) {
            for (int i = 0; i < n->class_def.nmembers; i++) {
                Node *m = n->class_def.members[i];
                if (m && m->kind == ND_TEMPLATE_DECL) {
                    Token *mname = template_name(m);
                    if (mname)
                        registry_add_member(reg,
                            n->class_def.tag->loc, n->class_def.tag->len,
                            mname->loc, mname->len,
                            m, n->class_def.ty);
                }
            }
        }
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
    /* Call-site argument types for function-template deduction
     * (N4659 §17.8.2.1 [temp.deduct.call]). NULL when this request
     * comes from a type-position use where no deduction is needed.
     * Used when the explicit template args don't cover all template
     * parameters; the remaining ones are deduced from call args. */
    Type **arg_types;
    int    nargs;
    InstRequest *next;
};

/* N4659 §17.5.2 [temp.mem]: member template instantiation request.
 * Created when a qualified call like Alloc::release(data) resolves
 * to a member template. */
typedef struct MemberTmplRequest MemberTmplRequest;
struct MemberTmplRequest {
    TmplEntry *entry;       /* registry entry (has tmpl + owner_class) */
    Type     **arg_types;   /* call-site argument types for deduction */
    int        nargs;
    Node      *call_node;   /* ND_CALL node (to patch resolved_type) */
    Node      *class_tid;   /* leading template-id for the class qualifier
                              * (Box<int> in 'Box<int>::convert(...)') —
                              * NULL when the qualifier is a non-template
                              * class. Drives the cloned func's class_type
                              * so the def mangles with the same template
                              * args as the call site. N4659 §17.5.2 +
                              * Itanium C++ ABI §5.1. */
    MemberTmplRequest *next;
};

typedef struct {
    InstRequest       *head;
    int                count;
    MemberTmplRequest *member_head;
    int                member_count;
    Arena             *arena;
    TmplRegistry      *reg;
    /* Class context for the func body currently being walked. When
     * an unqualified call inside a (cloned or original) class
     * member function names a sibling member template, this is the
     * class to look up against. N4659 §6.4.1 [basic.lookup.unqual] +
     * §17.5.2 [temp.mem]. NULL when not inside a class member. */
    Type              *cur_class;
    /* Class-template instantiation args used to mangle the enclosing
     * func. Reused so a sibling call's instantiation lands on the
     * SAME class instantiation (e.g. va_heap::release inside
     * va_heap::reserve<T> stays on va_heap, not 'va_heap<T>'). For
     * non-template classes this is NULL — we just use cur_class. */
    Node              *cur_class_tid;
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

    /* Recurse into nested template-args so e.g. 'user<hasher<int>>'
     * also triggers instantiation of hasher<int>. Without this
     * recursion, a class-template instantiation that's only ever
     * used as a template-argument to another class template never
     * gets emitted — its methods are then undefined when the outer
     * class's body calls them via the bound name. N4659 §17.7.1
     * [temp.inst]. */
    for (int i = 0; i < tid->template_id.nargs; i++) {
        Node *arg = tid->template_id.args[i];
        Type *aty = (arg && arg->kind == ND_VAR_DECL) ? arg->var_decl.ty : NULL;
        if (aty) collect_from_type(col, aty);
    }

    /* Skip template-ids that still have dependent (unresolved) args.
     * These appear inside cloned template bodies where an outer
     * template parameter hasn't been substituted yet. They'll be
     * collected once the outer template is instantiated and the
     * clone produces concrete args.
     *
     * N4659 §17.7.2 [temp.dep]: a template-id with dependent args
     * can't be instantiated yet. */
    for (int i = 0; i < tid->template_id.nargs; i++) {
        Node *arg = tid->template_id.args[i];
        Type *aty = (arg && arg->kind == ND_VAR_DECL) ? arg->var_decl.ty : NULL;
        if (aty && aty->kind == TY_DEPENDENT) return;
    }

    Token *name = tid->template_id.name;
    Node *tmpl = registry_find(col->reg, name->loc, name->len);
    if (!tmpl) return;  /* template definition not found — skip */

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
        collect_from_type(col, n->var_decl.ty);
        /* Inline anonymous struct in a var-decl:
         *   static struct { vec<T> m; } foo;
         * The parser produces ONE ND_VAR_DECL and hangs the struct
         * body off var_decl.ty->class_def — no separate top-level
         * ND_CLASS_DEF. Same shape as the ND_TYPEDEF case below;
         * without descent, template-ids inside the anon body are
         * missed. Pattern from gcc 4.8 calls.c internal_arg_pointer_
         * exp_state.
         *
         * Limit to *anonymous* structs: named types already have a
         * top-level ND_CLASS_DEF (walked elsewhere) and descending
         * into them causes infinite recursion when their methods
         * contain var-decls of their own type ('T r;' inside a
         * T::operator-()). N4659 §9.5.3 [dcl.type.elab] — anonymous
         * types can't name themselves, so they can't recurse. */
        if (n->var_decl.ty &&
            (n->var_decl.ty->kind == TY_STRUCT || n->var_decl.ty->kind == TY_UNION) &&
            n->var_decl.ty->class_def &&
            !n->var_decl.ty->tag)
            collect_from_node(col, n->var_decl.ty->class_def);
        if (n->var_decl.init)
            collect_from_node(col, n->var_decl.init);
        break;

    case ND_TYPEDEF:
        collect_from_type(col, n->var_decl.ty);
        /* For typedef'd structs ('typedef struct S { vec<T> m; } S2;'
         * or the 'typedef struct _X {...} *X;' pointer form), the
         * struct body is only accessible through the Type's class_def
         * — the TU has no separate ND_CLASS_DEF node. Walk the class_def
         * members so template-id types inside get collected. Peel
         * through TY_PTR/TY_ARRAY to find the struct. Pattern: gcc 4.8
         * tree-outof-ssa.c 'typedef struct _elim_graph { vec<int>
         * nodes; ... } *elim_graph;'. */
        {
            Type *tyw = n->var_decl.ty;
            while (tyw && (tyw->kind == TY_PTR || tyw->kind == TY_ARRAY) && tyw->base)
                tyw = tyw->base;
            if (tyw && (tyw->kind == TY_STRUCT || tyw->kind == TY_UNION) &&
                tyw->class_def)
                collect_from_node(col, tyw->class_def);
        }
        break;

    case ND_PARAM:
        collect_from_type(col, n->param.ty);
        break;

    case ND_FUNC_DEF:
    case ND_FUNC_DECL: {
        /* Push class context so unqualified sibling calls inside the
         * body can resolve against this class's member templates.
         * N4659 §6.4.1 [basic.lookup.unqual]: an unqualified name
         * inside a member function looks up the class scope. */
        Type *saved_class = col->cur_class;
        Node *saved_class_tid = col->cur_class_tid;
        if (n->func.class_type) {
            col->cur_class = n->func.class_type;
            col->cur_class_tid = NULL;  /* OOL clones carry their
                                          * own concrete class type;
                                          * no separate tid needed. */
        }
        collect_from_type(col, n->func.ret_ty);
        for (int i = 0; i < n->func.nparams; i++)
            collect_from_node(col, n->func.params[i]);
        if (n->func.body)
            collect_from_node(col, n->func.body);
        col->cur_class = saved_class;
        col->cur_class_tid = saved_class_tid;
        break;
    }

    case ND_CLASS_DEF:
        for (int i = 0; i < n->class_def.nmembers; i++)
            collect_from_node(col, n->class_def.members[i]);
        /* Collect from base types — a template base like Base<T>
         * (substituted to Base<int>) needs to be instantiated too. */
        for (int i = 0; i < n->class_def.nbase_types; i++)
            collect_from_type(col, n->class_def.base_types[i]);
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
        /* Function-template call with deducible U: 'is_a<Cat>(p)' in
         * 'template<T, U> bool is_a(U*)' — the ND_TEMPLATE_ID callee
         * only has T explicit; U must be deduced from the call args.
         * When the callee is ND_TEMPLATE_ID, create the request HERE
         * with arg_types from the call (so the instantiation phase
         * can run deduce_template_args). Skip the recursive
         * collect_from_node(callee) so we don't ALSO add a
         * no-arg-types duplicate via the ND_TEMPLATE_ID case.
         * N4659 §17.8.2.1 [temp.deduct.call]. */
        if (n->call.callee && n->call.callee->kind == ND_TEMPLATE_ID) {
            Node *tid = n->call.callee;
            Token *tname = tid->template_id.name;
            bool has_dep = false;
            for (int i = 0; i < tid->template_id.nargs && !has_dep; i++) {
                Node *arg = tid->template_id.args[i];
                Type *aty = (arg && arg->kind == ND_VAR_DECL) ? arg->var_decl.ty : NULL;
                if (aty && aty->kind == TY_DEPENDENT) has_dep = true;
            }
            if (!has_dep && tname) {
                /* Use sema's resolved template if it set one — for
                 * overloaded function templates the name alone isn't
                 * enough to pick the right entry. Falls back to
                 * registry_find for class-template ids and other
                 * paths that don't tag the resolved template. */
                Node *tmpl = tid->template_id.resolved_tmpl;
                if (!tmpl)
                    tmpl = registry_find(col->reg, tname->loc, tname->len);
                if (tmpl) {
                    InstRequest *req = arena_alloc(col->arena, sizeof(InstRequest));
                    req->name = tname;
                    req->template_id = tid;
                    req->tmpl_def = tmpl;
                    req->usage_type = NULL;
                    req->nargs = n->call.nargs;
                    if (n->call.nargs > 0) {
                        req->arg_types = arena_alloc(col->arena,
                            n->call.nargs * sizeof(Type *));
                        for (int i = 0; i < n->call.nargs; i++)
                            req->arg_types[i] = n->call.args[i]
                                ? n->call.args[i]->resolved_type : NULL;
                    } else {
                        req->arg_types = NULL;
                    }
                    req->next = col->head;
                    col->head = req;
                    col->count++;
                }
            }
        } else {
            collect_from_node(col, n->call.callee);
        }
        for (int i = 0; i < n->call.nargs; i++)
            collect_from_node(col, n->call.args[i]);
        /* N4659 §17.5.2 [temp.mem] / §17.8.2.1 [temp.deduct.call]:
         * detect qualified calls to member templates.
         * Pattern: Class::method(args) where method is a member template. */
        if (n->call.callee && n->call.callee->kind == ND_QUALIFIED &&
            n->call.callee->qualified.nparts >= 2) {
            Token *class_tok = n->call.callee->qualified.parts[0];
            Token *method_tok = n->call.callee->qualified.parts[
                n->call.callee->qualified.nparts - 1];
            if (class_tok && method_tok) {
                TmplEntry *me = registry_find_member(col->reg,
                    class_tok->loc, class_tok->len,
                    method_tok->loc, method_tok->len);
                if (me) {
                    /* Collect call-site arg types for deduction */
                    MemberTmplRequest *mr = arena_alloc(col->arena,
                        sizeof(MemberTmplRequest));
                    mr->entry = me;
                    mr->nargs = n->call.nargs;
                    mr->call_node = n;
                    mr->class_tid = n->call.callee->qualified.lead_tid;
                    if (n->call.nargs > 0) {
                        mr->arg_types = arena_alloc(col->arena,
                            n->call.nargs * sizeof(Type *));
                        for (int i = 0; i < n->call.nargs; i++)
                            mr->arg_types[i] = n->call.args[i]
                                ? n->call.args[i]->resolved_type : NULL;
                    } else {
                        mr->arg_types = NULL;
                    }
                    mr->next = col->member_head;
                    col->member_head = mr;
                    col->member_count++;
                }
            }
        }
        /* Unqualified call inside a class member function — may name a
         * sibling member template. gcc 4.8 va_heap::reserve<T>'s body
         * calls release(v); per N4659 §6.4.1 [basic.lookup.unqual]
         * unqualified lookup finds the sibling member release. The
         * static-method-this fix (#136) already mangles the call to
         * 'sf__A__release_*', but without this collection path the
         * member template was never instantiated → undefined symbol.
         * Match by current class context + member name. */
        if (col->cur_class && col->cur_class->tag &&
            n->call.callee && n->call.callee->kind == ND_IDENT &&
            n->call.callee->ident.name) {
            Token *cls = col->cur_class->tag;
            Token *name = n->call.callee->ident.name;
            TmplEntry *me = registry_find_member(col->reg,
                cls->loc, cls->len, name->loc, name->len);
            if (me) {
                MemberTmplRequest *mr = arena_alloc(col->arena,
                    sizeof(MemberTmplRequest));
                mr->entry = me;
                mr->nargs = n->call.nargs;
                mr->call_node = n;
                /* Reuse the enclosing call's class instantiation —
                 * a sibling call inherits the same class<args>. */
                mr->class_tid = col->cur_class_tid;
                if (n->call.nargs > 0) {
                    mr->arg_types = arena_alloc(col->arena,
                        n->call.nargs * sizeof(Type *));
                    for (int i = 0; i < n->call.nargs; i++)
                        mr->arg_types[i] = n->call.args[i]
                            ? n->call.args[i]->resolved_type : NULL;
                } else {
                    mr->arg_types = NULL;
                }
                mr->next = col->member_head;
                col->member_head = mr;
                col->member_count++;
            }
        }
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

    case ND_QUALIFIED:
        /* N4659 §17.2 [temp.names] / §8.1.4.3 [expr.prim.id.qual]:
         * a qualified-id like Embed<int>::embedded_size carries the
         * template-id on lead_tid. Build a synthetic Type with
         * template_id_node so it goes through the class-template
         * collection path (with proper dedup) rather than the
         * function-template ND_TEMPLATE_ID path. */
        if (n->qualified.lead_tid &&
            n->qualified.lead_tid->kind == ND_TEMPLATE_ID) {
            Node *tid = n->qualified.lead_tid;
            Type *syn = arena_alloc(col->arena, sizeof(Type));
            syn->kind = TY_STRUCT;
            syn->tag = tid->template_id.name;
            syn->template_id_node = tid;
            /* Copy template args onto the synthetic type so the
             * instantiation pass can match it. */
            syn->n_template_args = tid->template_id.nargs;
            if (tid->template_id.nargs > 0) {
                syn->template_args = arena_alloc(col->arena,
                    tid->template_id.nargs * sizeof(Type *));
                for (int i = 0; i < tid->template_id.nargs; i++) {
                    Node *arg = tid->template_id.args[i];
                    syn->template_args[i] = (arg && arg->kind == ND_VAR_DECL)
                        ? arg->var_decl.ty : NULL;
                }
            }
            collect_from_type(col, syn);
        }
        break;

    case ND_TEMPLATE_ID: {
        /* A template-id in expression position (e.g. max_of<int> as
         * a call callee). Record as a function template instantiation
         * request if the name resolves to a template definition. */
        Token *tname = n->template_id.name;
        /* Skip if any arg is still dependent (unsubstituted). */
        bool has_dep = false;
        for (int i = 0; i < n->template_id.nargs && !has_dep; i++) {
            Node *arg = n->template_id.args[i];
            Type *aty = (arg && arg->kind == ND_VAR_DECL) ? arg->var_decl.ty : NULL;
            if (aty && aty->kind == TY_DEPENDENT) has_dep = true;
        }
        if (has_dep) break;
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

/* ------------------------------------------------------------------ */
/* Template Argument Deduction — N4659 §17.8.2.1 [temp.deduct.call]   */
/*                                                                     */
/* Inverse of subst_type: given a pattern type P (may contain          */
/* TY_DEPENDENT) and a concrete argument type A, deduce the bindings.  */
/* ------------------------------------------------------------------ */

/* Deduce from a single (parameter-type, argument-type) pair.
 * Returns true on success, false on deduction failure.
 *
 * N4659 §17.8.2.1/2: "If P is a reference type, the type referred
 * to by P is used for type deduction."
 * N4659 §17.8.2.5/8: for pointer types, recurse on pointee. */
static bool deduce_from_pair(Type *P, Type *A, SubstMap *map) {
    if (!P || !A) return true;  /* nothing to deduce */

    /* §17.8.2.1/2: strip references from P. Also strip from A — sea-
     * front represents `T&` parameter idents with TY_REF on their
     * resolved_type, but the argument-type for deduction is the
     * non-reference type. Without stripping A, deduction fails when
     * a cloned template body passes its own `T&` parameter to another
     * function template (gcc 4.8 vec.h `vec_alloc` body calling
     * `vec_safe_reserve(v, n, false)` with v of type `vec<T,A>*&`). */
    if (P->kind == TY_REF || P->kind == TY_RVALREF)
        P = P->base;
    if (A && (A->kind == TY_REF || A->kind == TY_RVALREF))
        A = A->base;
    if (!P || !A) return true;

    /* TY_DEPENDENT: this IS the template parameter — bind it */
    if (P->kind == TY_DEPENDENT && P->tag) {
        /* Check if already bound (consistency) */
        for (int i = 0; i < map->nentries; i++) {
            Token *pn = map->entries[i].param_name;
            if (pn && pn->len == P->tag->len &&
                memcmp(pn->loc, P->tag->loc, pn->len) == 0) {
                /* Already bound — must be consistent */
                return true;  /* trust the first binding */
            }
        }
        subst_map_add(map, P->tag, A);
        return true;
    }

    /* Compound types: recurse structurally */
    if (P->kind == TY_PTR && A->kind == TY_PTR)
        return deduce_from_pair(P->base, A->base, map);
    if (P->kind == TY_REF && A->kind == TY_REF)
        return deduce_from_pair(P->base, A->base, map);
    if (P->kind == TY_ARRAY && A->kind == TY_ARRAY)
        return deduce_from_pair(P->base, A->base, map);

    /* Class-template specialization on both sides: recurse through
     * template arguments. N4659 §17.8.2.5/9 [temp.deduct.type]: for
     * a TT<T1, T2, ...> vs TT<A1, A2, ...> pattern, deduce each Ti
     * from the corresponding Ai.
     *
     * The template-id info can sit in two places on a Type:
     *   - template_id_node: the ND_TEMPLATE_ID produced by parsing
     *     'vec<int,va_gc,vl_embed>', with args as Node** (each ND_
     *     VAR_DECL carrying a Type).
     *   - template_args / n_template_args: Type** flattened, set by
     *     the instantiation pass when a concrete class is produced.
     *
     * Pre-instantiation (when this runs in sema pass 1), A has only
     * its template_id_node populated — n_template_args is 0. Post-
     * instantiation, template_args is set too. Handle both shapes so
     * deduction works at both stages.
     *
     * Handles the gcc 4.8 pattern 'template<T,A> gt_pch_nx(
     * vec<T,A,vl_embed>*)' called with vec<ipa_set, va_gc, vl_embed>*
     * — PTR strips to struct, then we unify template args here to
     * bind T=ipa_set, A=va_gc. */
    if ((P->kind == TY_STRUCT || P->kind == TY_UNION) &&
        (A->kind == TY_STRUCT || A->kind == TY_UNION)) {
        /* Tag-spelling match — if the template is 'vec' we only
         * deduce when the arg is also 'vec'. */
        if (!P->tag || !A->tag) return true;
        if (P->tag->len != A->tag->len) return true;
        if (memcmp(P->tag->loc, A->tag->loc, P->tag->len) != 0)
            return true;
        /* Pattern args: prefer template_id_node (has dependent
         * bindings pre-instantiation). */
        Node  *ptid = P->template_id_node;
        int    np_args = 0;
        if (ptid && ptid->kind == ND_TEMPLATE_ID)
            np_args = ptid->template_id.nargs;
        else
            np_args = P->n_template_args;
        /* Arg args: either template_id_node (unresolved, pre-inst)
         * or template_args (resolved, post-inst). */
        Node  *atid = A->template_id_node;
        int    na_args = (atid && atid->kind == ND_TEMPLATE_ID)
                            ? atid->template_id.nargs
                            : A->n_template_args;
        if (np_args == 0 || na_args == 0) return true;
        if (np_args != na_args) return true;
        for (int i = 0; i < np_args; i++) {
            Type *pt = NULL;
            if (ptid && ptid->kind == ND_TEMPLATE_ID) {
                Node *pa = ptid->template_id.args[i];
                pt = (pa && pa->kind == ND_VAR_DECL) ? pa->var_decl.ty : NULL;
            } else {
                pt = P->template_args[i];
            }
            Type *at = NULL;
            if (atid && atid->kind == ND_TEMPLATE_ID) {
                Node *aa = atid->template_id.args[i];
                at = (aa && aa->kind == ND_VAR_DECL) ? aa->var_decl.ty : NULL;
            } else {
                at = A->template_args[i];
            }
            if (!deduce_from_pair(pt, at, map)) return false;
        }
        return true;
    }

    /* Non-dependent, non-compound: no deduction needed */
    return true;
}

/* Deduce template arguments for a member template call.
 * tmpl_func: the inner ND_FUNC_DEF/ND_FUNC_DECL of the member template
 * arg_types: concrete types of the call-site arguments
 * nargs: number of call-site arguments
 * out: SubstMap to populate (must be pre-allocated)
 *
 * N4659 §17.8.2.1 [temp.deduct.call]/1: "Template arguments can be
 * deduced from each function call argument by comparing the type of
 * the function parameter with the corresponding function argument." */
bool deduce_template_args(Node *tmpl_func, Type **arg_types, int nargs,
                          SubstMap *out) {
    if (!tmpl_func) return false;
    int nparams = 0;
    if (tmpl_func->kind == ND_FUNC_DEF || tmpl_func->kind == ND_FUNC_DECL)
        nparams = tmpl_func->func.nparams;
    else if (tmpl_func->kind == ND_VAR_DECL && tmpl_func->var_decl.ty &&
             tmpl_func->var_decl.ty->kind == TY_FUNC)
        nparams = tmpl_func->var_decl.ty->nparams;
    else
        return false;

    int pairs = nparams < nargs ? nparams : nargs;
    for (int i = 0; i < pairs; i++) {
        Type *P = NULL;
        if (tmpl_func->kind == ND_FUNC_DEF || tmpl_func->kind == ND_FUNC_DECL)
            P = tmpl_func->func.params[i]->param.ty;
        else
            P = tmpl_func->var_decl.ty->params[i];
        if (!deduce_from_pair(P, arg_types[i], out))
            return false;
    }
    return out->nentries > 0;
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
 * Unify two template-id argument lists. Each arg is an ND_VAR_DECL
 * whose var_decl.ty is the type. TY_DEPENDENT positions on either
 * side are wildcards. Arg counts must match. Used to decide whether
 * an OOL method's qualifier ('vec<T,A,vl_embed>::f') matches the
 * template's own pattern ('vec<T,A,vl_embed>').
 *
 * N4659 §17.8.3.2 [temp.class.spec.match] — matching a class template
 * specialization against a template-id. SHORTCUT (ours, not the
 * standard): we do position-wise type equality with TY_DEPENDENT as a
 * wildcard rather than running full unification (no occurs check, no
 * binding capture across positions). Sufficient for the OOL-method
 * binding we need; not equivalent to the standard's algorithm for
 * pathological patterns. TODO(seafront#tmpl-unify-full): replace with
 * real unification when we need it.
 */
static bool template_ids_unify(Node *a, Node *b) {
    if (!a || !b) return a == b;
    if (a->kind != ND_TEMPLATE_ID || b->kind != ND_TEMPLATE_ID) return false;
    if (a->template_id.nargs != b->template_id.nargs) return false;
    for (int i = 0; i < a->template_id.nargs; i++) {
        Node *aa = a->template_id.args[i];
        Node *bb = b->template_id.args[i];
        Type *at = (aa && aa->kind == ND_VAR_DECL) ? aa->var_decl.ty : NULL;
        Type *bt = (bb && bb->kind == ND_VAR_DECL) ? bb->var_decl.ty : NULL;
        /* NULL on either side = wildcard. Template-template parameters
         * (gcc 4.8 hash_table's 'template<typename T> class Allocator')
         * carry no Type — sea-front parses them but doesn't model the
         * inner template-parameter-list. Treating NULL as a wildcard
         * lets the OOL of 'hash_table<D,A>::create' bind to the
         * instantiated 'hash_table<asan_mem_ref_hasher>' (where the
         * default Allocator was filled in at usage). N4659 §17.2/3
         * [temp.param]: a template-template parameter accepts any
         * argument that itself names a class template. */
        if (!at || !bt) continue;
        if (!types_match(at, bt)) return false;
    }
    return true;
}

/*
 * Decide whether an OOL method template belongs to the class template
 * being instantiated.
 *
 * N4659 §17.8.2 [temp.mem]/5 — a member of a class template (or of a
 * member template of a class template) defined outside its template
 * definition must be specified using the template-id of the class or
 * specialization it belongs to. That template-id determines the
 * method's owning template.
 *
 * Two-stage match:
 *   (1) Tag names must agree (necessary for any match).
 *   (2) If the OOL method's qualifier is a template-id (e.g.
 *       'vec<T,A,vl_embed>::last'), and the target class template is
 *       a specialization (has a template_id_node pattern), the args
 *       must unify via template_ids_unify. A non-specialization
 *       target (primary) only accepts methods whose qualifier has
 *       NO template-id or whose template-id args are all dependent.
 *
 * SHORTCUT: "all dependent args → primary-compatible" is a sea-front
 * proxy, not a standard rule. The standard selects the template
 * whose template-id syntactically matches the qualifier; our proxy
 * covers the common case (primary's 'Box<T>::get()' has args {T_dep})
 * without running full template-id-to-template resolution.
 * TODO(seafront#ool-primary-match): replace with real matching
 * against the primary's own template_id pattern when one exists. */
static bool ool_method_matches(Node *method, Type *target_class) {
    Node *inner = method->template_decl.decl;
    if (!inner) return false;
    if (!(inner->kind == ND_FUNC_DEF || inner->kind == ND_FUNC_DECL))
        return false;
    Type *mct = inner->func.class_type;
    if (!mct || !mct->tag || !target_class || !target_class->tag) return false;
    if (mct->tag->len != target_class->tag->len ||
        memcmp(mct->tag->loc, target_class->tag->loc, mct->tag->len) != 0)
        return false;
    /* Refinement: when both sides carry template-id patterns, require
     * them to unify. This keeps 'vec<T,A,vl_embed>::f' from binding
     * to 'vec<T,A,vl_ptr>' or to the primary. */
    Node *m_tid = inner->func.qual_tid;
    Node *t_tid = target_class->template_id_node;
    if (m_tid && t_tid)
        return template_ids_unify(m_tid, t_tid);
    /* OOL qualifier has template-id args but target has no pattern
     * (primary instantiation). This is legitimate when the OOL's
     * args are all dependent (template params of the method itself,
     * like 'Box<T>::get'). It's a mismatch when any arg is concrete
     * ('vec<T,A,vl_embed>::last' shouldn't bind to the primary). */
    if (m_tid && !t_tid) {
        /* Build a set of the OOL's enclosing template-parameter
         * names so we can recognise template-template-parameter
         * args (which sea-front parses as opaque TY_STRUCT with
         * tag = the param name, not TY_DEPENDENT). N4659 §17.2/3
         * [temp.param] — a template-template parameter accepts any
         * argument that names a class template; from the OOL's
         * perspective it's a bindable variable, just like a regular
         * type parameter. gcc 4.8 hash_table:
         *   template<typename Descriptor,
         *            template<typename T> class Allocator = xcallocator>
         *   class hash_table { ... };
         *   template<typename Descriptor,
         *            template<typename T> class Allocator>
         *   void hash_table<Descriptor, Allocator>::create(size_t) { ... }
         */
        for (int i = 0; i < m_tid->template_id.nargs; i++) {
            Node *a = m_tid->template_id.args[i];
            Type *t = (a && a->kind == ND_VAR_DECL) ? a->var_decl.ty : NULL;
            if (!t || t->kind == TY_DEPENDENT) continue;
            /* A non-TY_DEPENDENT type whose tag matches one of the
             * method's enclosing template parameters is also a
             * dependent-style wildcard. Sea-front parses
             * template-template parameter names in template-arg
             * position via the ENTITY_TEMPLATE lookup, which has
             * type=NULL → falls through to the opaque-type path
             * (TY_INT or TY_STRUCT with tag = the param name). The
             * tag comparison recovers the template-template-arg
             * intent. N4659 §17.2/3 [temp.param] — a template-
             * template parameter accepts any class-template arg. */
            bool matches_tparam = false;
            if (t->tag) {
                for (int j = 0; j < method->template_decl.nparams; j++) {
                    Node *tp = method->template_decl.params[j];
                    Token *tn = tp ? tp->param.name : NULL;
                    if (tn && tn->len == t->tag->len &&
                        memcmp(tn->loc, t->tag->loc, tn->len) == 0) {
                        matches_tparam = true;
                        break;
                    }
                }
            }
            if (!matches_tparam) return false;
        }
        return true;  /* all args dependent or template-template params */
    }
    /* OOL qualifier had no template-id args: legacy tag-only match.
     * (This is the common case for non-templated classes.) */
    return true;
}

/*
 * Find out-of-class method templates for a given class template.
 * These are top-level ND_TEMPLATE_DECL nodes wrapping ND_FUNC_DEF
 * where func.class_type matches the class template's type AND (when
 * applicable) the qualifier template-id unifies with the target.
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
                if (ool_method_matches(m, class_type) && count < max)
                    out[count++] = m;
            }
            continue;
        }
        if (n->kind != ND_TEMPLATE_DECL) continue;
        if (ool_method_matches(n, class_type) && count < max)
            out[count++] = n;
    }
    return count;
}

/* Build a TY_FUNC from an ND_FUNC_DEF/ND_FUNC_DECL's params + ret_ty.
 * Used to back-fill the call-site callee's resolved_type so emit_call
 * can see the param types (for ref-arg adaptation) and default-arg
 * injection (vec_safe_reserve(v, n) — third 'exact' param defaults
 * to false in vec.h). N4659 §11.3.6 [dcl.fct.default]. */
static Type *build_func_type_from_node(Node *func, Arena *arena) {
    if (!func || (func->kind != ND_FUNC_DEF && func->kind != ND_FUNC_DECL))
        return NULL;
    Type *ft = arena_alloc(arena, sizeof(Type));
    memset(ft, 0, sizeof(Type));
    ft->kind = TY_FUNC;
    ft->ret = func->func.ret_ty;
    ft->nparams = func->func.nparams;
    ft->is_variadic = func->func.is_variadic;
    if (ft->nparams > 0) {
        ft->params = arena_alloc(arena, ft->nparams * sizeof(Type *));
        bool any_default = false;
        for (int i = 0; i < ft->nparams; i++) {
            Node *p = func->func.params[i];
            ft->params[i] = (p && p->kind == ND_PARAM) ? p->param.ty : NULL;
            if (p && p->kind == ND_PARAM && p->param.default_value)
                any_default = true;
        }
        if (any_default) {
            ft->param_defaults = arena_alloc(arena,
                ft->nparams * sizeof(Node *));
            for (int i = 0; i < ft->nparams; i++) {
                Node *p = func->func.params[i];
                ft->param_defaults[i] = (p && p->kind == ND_PARAM)
                    ? p->param.default_value : NULL;
            }
        }
    }
    return ft;
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
     *
     * For primary templates: match params by position against the
     * usage args, with fallback to default types.
     *
     * For partial specializations: the spec's inner class has a
     * template_id_node with a PATTERN (e.g. vec<T, A, vl_embed>).
     * Unify each pattern position against the usage arg. TY_DEPENDENT
     * positions bind the spec's param to the usage arg; concrete
     * positions must match exactly (already checked by the
     * specialization finder). */
    /* +1 capacity for the injected-class-name entry */
    SubstMap map = subst_map_new(arena, (nparams > 0 ? nparams : 1) + 1);

    /* Check if this is a partial specialization */
    Type *inner_ty = NULL;
    if (inner->kind == ND_CLASS_DEF) inner_ty = inner->class_def.ty;
    else if (inner->kind == ND_VAR_DECL) inner_ty = inner->var_decl.ty;
    Node *spec_pattern = (inner_ty && inner_ty->template_id_node &&
                          inner_ty->template_id_node->kind == ND_TEMPLATE_ID)
                         ? inner_ty->template_id_node : NULL;

    if (spec_pattern && spec_pattern->template_id.nargs == nargs) {
        /* Partial specialization: unify pattern against usage args */
        for (int i = 0; i < nargs; i++) {
            Node *pa = spec_pattern->template_id.args[i];
            Type *pt = (pa && pa->kind == ND_VAR_DECL) ? pa->var_decl.ty : NULL;
            if (!pt || pt->kind != TY_DEPENDENT) continue;
            /* This pattern position is a param variable — bind it */
            Type *arg_ty = type_arg_from_node(template_id->template_id.args[i]);
            if (arg_ty && pt->tag)
                subst_map_add(&map, pt->tag, arg_ty);
        }
    } else {
        /* Primary template: match by position */
        for (int i = 0; i < nparams; i++) {
            Node *param = tmpl->template_decl.params[i];
            if (!param) continue;
            Token *pname = param->param.name;
            if (!pname) continue;

            /* Template-template parameter detection: parse_template_
             * parameter passes the leading 'template' keyword as the
             * param node's tok, which distinguishes TT-params from
             * regular type-params (whose tok is TK_KW_TYPENAME or
             * TK_KW_CLASS). Both shape have param.ty == NULL, so
             * we MUST check tok->kind, not ty.
             *
             * Bind the TT-param's name to the actual class-template
             * name token from the usage arg, or the default if the
             * user omitted it. gcc 4.8 hash_table<D, A=xcallocator>
             * with usage hash_table<asan_mem_ref_hasher>: A defaults
             * to xcallocator. The cloned body's
             * Allocator<value_type>::data_alloc(...) will be
             * rewritten to xcallocator<value_type>::data_alloc by
             * clone.c's ND_QUALIFIED handler, so the call mangles
             * to sf__xcallocator_t_..._te___data_alloc_* matching
             * the actual definition. N4659 §17.2/3 [temp.param] +
             * §17.7.1 [temp.inst]. */
            if (param->tok && param->tok->kind == TK_KW_TEMPLATE) {
                Token *bound_name = NULL;
                if (i < nargs) {
                    Node *a = template_id->template_id.args[i];
                    Type *at = (a && a->kind == ND_VAR_DECL) ? a->var_decl.ty : NULL;
                    if (at && at->tag) bound_name = at->tag;
                }
                if (!bound_name && param->param.default_type &&
                    param->param.default_type->tag) {
                    bound_name = param->param.default_type->tag;
                }
                if (bound_name) subst_map_add_tt(&map, pname, bound_name);
                continue;
            }

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
    }

    /* For class templates: pre-create the instantiated Type so the
     * injected-class-name (bare 'ClassName' inside the class body)
     * can be substituted during cloning via the SubstMap. */
    Type *inst_ty = NULL;
    if (inner->kind == ND_CLASS_DEF) {
        inst_ty = arena_alloc(arena, sizeof(Type));
        if (inner->class_def.ty)
            *inst_ty = *inner->class_def.ty;
        else
            inst_ty->kind = TY_STRUCT;
        inst_ty->tag = template_id->template_id.name;
        /* Add template args early for mangling */
        int n = template_id->template_id.nargs;
        if (n > 0) {
            inst_ty->template_args = arena_alloc(arena, n * sizeof(Type *));
            inst_ty->n_template_args = n;
            for (int i = 0; i < n; i++)
                inst_ty->template_args[i] =
                    type_arg_from_node(template_id->template_id.args[i]);
        }
        if (n < map.nentries) {
            n = map.nentries;
            inst_ty->template_args = arena_alloc(arena, n * sizeof(Type *));
            inst_ty->n_template_args = n;
            for (int i = 0; i < n; i++)
                inst_ty->template_args[i] = map.entries[i].concrete_type;
        }
        /* Add the class name to the SubstMap so the injected-class-name
         * (bare 'Box' inside 'Box<T>' body) gets substituted to the
         * instantiated type during cloning. */
        Token *class_tag = inner->class_def.tag;
        if (class_tag)
            subst_map_add(&map, class_tag, inst_ty);
    }

    /* Clone the inner declaration with type substitution */
    Node *cloned = clone_node(inner, &map, arena);
    if (!cloned) return NULL;

    /* For class templates: finish setting up the instantiated type.
     * (template_args already set pre-clone for injected-class-name.) */
    if (cloned->kind == ND_CLASS_DEF && inst_ty) {

        /* Build a class_region for the instantiated class so sema
         * can resolve member references inside method bodies. */
        DeclarativeRegion *cr = region_build_class(cloned, inst_ty, arena);
        inst_ty->class_region = cr;
        inst_ty->class_def = cloned;
        /* Scan the cloned members for ctor/dtor/virtual flags.
         * The original class template has these set during parsing;
         * instantiated copies need them re-derived from the cloned
         * member list. N4659 §15.1 [class.ctor], §15.4 [class.dtor],
         * §13.3 [class.virtual]. */
        inst_ty->has_dtor = false;
        inst_ty->has_default_ctor = false;
        inst_ty->has_virtual_methods = false;
        for (int i = 0; i < cloned->class_def.nmembers; i++) {
            Node *m = cloned->class_def.members[i];
            if (!m) continue;
            if (m->kind == ND_FUNC_DEF) {
                if (m->func.is_destructor) {
                    bool empty = m->func.body &&
                        m->func.body->kind == ND_BLOCK &&
                        m->func.body->block.nstmts == 0;
                    if (!empty) inst_ty->has_dtor = true;
                }
                if (m->func.is_constructor && m->func.nparams == 0)
                    inst_ty->has_default_ctor = true;
                if (m->func.is_virtual)
                    inst_ty->has_virtual_methods = true;
            } else if (m->kind == ND_VAR_DECL) {
                if (m->var_decl.is_destructor)
                    inst_ty->has_dtor = true;
                if (m->var_decl.is_constructor &&
                    m->var_decl.ty && m->var_decl.ty->kind == TY_FUNC &&
                    m->var_decl.ty->nparams == 0)
                    inst_ty->has_default_ctor = true;
                if (m->var_decl.is_virtual)
                    inst_ty->has_virtual_methods = true;
            }
        }

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
         * Uses mangle_type_to_buf — the C-symbol-safe encoding —
         * NOT type_to_key (which embeds '<', '>', 'P', 'S' for
         * dedup hashing and would produce invalid C identifiers).
         * E.g. max_of<int> → max_of_t_int_te_ */
        Token *fname = cloned->func.name;
        int n = template_id->template_id.nargs;
        int bufsize = 512;
        char *buf = arena_alloc(arena, bufsize);
        int pos = 0;
        if (fname)
            pos += snprintf(buf + pos, bufsize - pos, "%.*s",
                            fname->len, fname->loc);
        pos += snprintf(buf + pos, bufsize - pos, "_t_");
        for (int i = 0; i < n; i++) {
            if (i > 0 && pos < bufsize - 1) buf[pos++] = '_';
            Type *at = type_arg_from_node(template_id->template_id.args[i]);
            pos = mangle_type_to_buf(at, buf, pos, bufsize);
        }
        pos += snprintf(buf + pos, bufsize - pos, "_te_");
        /* Param suffix — distinguishes overloaded function templates
         * whose template-arg substitution alone produces the same
         * key. Pattern: gcc 4.8 vec.h has two `vec_alloc` templates,
         *   template<T,A> vec_alloc(vec<T,A,vl_embed>*&, unsigned)
         *   template<T>   vec_alloc(vec<T>*&, unsigned)
         * which both mangle to `vec_alloc_t_<T>_te_` without a param
         * suffix → C-symbol collision. */
        pos += snprintf(buf + pos, bufsize - pos, "_p_");
        for (int i = 0; i < cloned->func.nparams; i++) {
            if (i > 0 && pos < bufsize - 1) buf[pos++] = '_';
            Node *p = cloned->func.params[i];
            Type *pt = (p && p->kind == ND_PARAM) ? p->param.ty : NULL;
            pos = mangle_type_to_buf(pt, buf, pos, bufsize);
        }
        pos += snprintf(buf + pos, bufsize - pos, "_pe_");

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
        template_id->ident.overload_set = NULL;
        template_id->ident.n_overloads = 0;

        /* Synthesize a TY_FUNC carrying the substituted param types
         * and return type onto the call-site callee's resolved_type.
         * Without this, the ND_CALL emit path can't see the param
         * types and skips ref-param adaptation — passing a `T*` arg
         * to a `T*&` (now `T**` in C) param without taking address.
         * Pattern: gcc 4.8 vec.h `vec_safe_grow_cleared(vec, n)` —
         * the `vec<...> *&v` param needs `&vec` at the call site. */
        template_id->resolved_type = build_func_type_from_node(cloned, arena);

        /* Set up param scope for sema. The enclosing must be the
         * TU's global scope so phase-2 sema can resolve free-function
         * names referenced from the cloned body (e.g.
         * vec_safe_reserve called from vec_alloc's body). Without it,
         * lookup stops at the param scope and the bare-call rewrite
         * never fires for nested template instantiations. */
        if (cloned->func.body && cloned->func.nparams > 0)
            cloned->func.param_scope = region_build_prototype(
                cloned, tu ? tu->tu.global_scope : NULL, arena);
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
    if (col.count == 0 && col.member_count == 0) break;

    /* Phase 3a: member template instantiations FIRST — they're
     * standalone functions that class instantiation methods may call.
     * Processing them first ensures they're earlier in all_instantiated[]
     * and thus emitted before their callers. */
    int ninst_this_round = 0;
    for (MemberTmplRequest *mr = col.member_head; mr; mr = mr->next) {
        Node *tmpl = mr->entry->tmpl;
        int np = tmpl->template_decl.nparams;
        Node *inner = tmpl->template_decl.decl;
        if (!inner) continue;

        /* N4659 §17.8.2.1 [temp.deduct.call]: deduce template args.
         * Capacity must accommodate both the inner head's params AND
         * the outer class-template's params (deduce_from_pair binds
         * those too via 'Holder<A>*' vs 'Holder<int>*' matching).
         * Without enough capacity, subst_map_add silently drops the
         * second binding and clone_node leaves T as TY_DEPENDENT. */
        int outer_np = (mr->class_tid &&
                        mr->class_tid->kind == ND_TEMPLATE_ID)
                       ? mr->class_tid->template_id.nargs : 0;
        int cap = np + outer_np;
        if (cap < 1) cap = 1;
        SubstMap deduced = subst_map_new(arena, cap);
        if (!deduce_template_args(inner, mr->arg_types, mr->nargs, &deduced))
            continue;

        /* Dedup key: class + NUL + class-template-args (if any) +
         * NUL + member + NUL + member-template deduced types.
         *
         * Including the class-template args is required for the
         * gcc 4.8 is_a_helper<T>::cast<U> pattern: with T=cgraph_node
         * vs T=varpool_node and the same U=symtab_node_def, the
         * deduced map only carries U (T is the class template's
         * parameter, not deduced from the in-class member's params).
         * Without the class args in the key, the second class
         * instantiation collides with the first → only one def
         * emitted, the other call goes unresolved at link.
         * N4659 §17.7.1 [temp.inst]: each distinct argument set
         * (across BOTH heads) produces a distinct specialization. */
        char key[MAX_DEDUP_KEY];
        Token *class_tag = mr->entry->owner_class ? mr->entry->owner_class->tag : NULL;
        Token *member_name = template_name(tmpl);
        if (!class_tag || !member_name) continue;
        int pos = 0;
        if (pos + class_tag->len < MAX_DEDUP_KEY) {
            memcpy(key, class_tag->loc, class_tag->len);
            pos = class_tag->len;
        }
        key[pos++] = '\0';
        /* Class-template args from the call-site lead_tid. */
        if (mr->class_tid && mr->class_tid->kind == ND_TEMPLATE_ID) {
            int ctna = mr->class_tid->template_id.nargs;
            for (int i = 0; i < ctna; i++) {
                Type *cta = type_arg_from_node(
                    mr->class_tid->template_id.args[i]);
                pos = type_to_key(cta, key, pos, MAX_DEDUP_KEY);
                key[pos++] = '\0';
            }
        }
        key[pos++] = '\0';  /* separator after class args */
        if (pos + member_name->len < MAX_DEDUP_KEY) {
            memcpy(key + pos, member_name->loc, member_name->len);
            pos += member_name->len;
        }
        key[pos++] = '\0';
        for (int i = 0; i < deduced.nentries; i++) {
            pos = type_to_key(deduced.entries[i].concrete_type,
                              key, pos, MAX_DEDUP_KEY);
            key[pos++] = '\0';
        }
        if (dedup_find(&ds, key, pos)) continue;

        /* N4659 §17.5.2/5 [temp.mem]: if in-class member is a
         * declaration without body, find the OOL definition.
         * The parser represents ALL in-class function declarations as
         * ND_VAR_DECL with TY_FUNC type (whether template or not),
         * so that's the dominant declaration shape — must be matched
         * alongside the rare ND_FUNC_DECL / body-less ND_FUNC_DEF. */
        Node *func_src = inner;
        bool is_decl_only =
            inner->kind == ND_FUNC_DECL ||
            (inner->kind == ND_FUNC_DEF && !inner->func.body) ||
            (inner->kind == ND_VAR_DECL && inner->var_decl.ty &&
             inner->var_decl.ty->kind == TY_FUNC);
        /* Set when the source already provides an explicit
         * specialization for this (class, member, args) tuple. The
         * source-level def emits as a regular function from
         * tu->tu.decls; we must NOT also clone it from the primary
         * or we get two defs of the same mangled symbol. N4659
         * §17.8.4 [temp.expl.spec]: an explicit specialization is a
         * distinct entity — the primary template is not used to
         * generate it. gcc 4.8 cgraph.h has
         *   template<> template<>
         *   inline bool is_a_helper<cgraph_node>::test(symtab_node_def *p);
         */
        bool source_has_explicit_spec = false;
        if (is_decl_only && tu) {
            for (int i = 0; i < tu->tu.ndecls; i++) {
                Node *d = tu->tu.decls[i];
                if (!d || d->kind != ND_TEMPLATE_DECL) continue;
                /* Peel any nested template heads. The OOL definition
                 * of a member template inside a class template carries
                 * TWO heads —
                 *   template<typename A>
                 *   template<typename T>
                 *   int Holder<A>::combine(...) { ... }
                 * — and parses as ND_TEMPLATE_DECL wrapping another
                 * ND_TEMPLATE_DECL wrapping the FUNC_DEF. N4659
                 * §17.5.2/3 [temp.mem]. An explicit specialization
                 * has the same shape but EVERY head has nparams == 0
                 * ('template<> template<>'). */
                bool all_heads_empty = (d->template_decl.nparams == 0);
                Node *di = d->template_decl.decl;
                while (di && di->kind == ND_TEMPLATE_DECL) {
                    if (di->template_decl.nparams != 0)
                        all_heads_empty = false;
                    di = di->template_decl.decl;
                }
                if (!di || di->kind != ND_FUNC_DEF || !di->func.body) continue;
                if (!di->func.class_type || !di->func.name) continue;
                Token *ct = di->func.class_type->tag;
                if (!ct || ct->len != class_tag->len ||
                    memcmp(ct->loc, class_tag->loc, ct->len) != 0) continue;
                if (di->func.name->len != member_name->len ||
                    memcmp(di->func.name->loc, member_name->loc,
                           member_name->len) != 0) continue;
                if (all_heads_empty) {
                    /* Explicit specialization — let the source-level
                     * def stand on its own; don't clone. */
                    source_has_explicit_spec = true;
                    break;
                }
                func_src = di;
                break;
            }
        }
        if (source_has_explicit_spec) {
            /* Record in dedup so subsequent identical requests also
             * skip; the source's def covers them. */
            Type *dummy = arena_alloc(arena, sizeof(Type));
            dummy->kind = TY_FUNC;
            dedup_add(&ds, key, pos, dummy);
            continue;
        }
        /* If we couldn't find an OOL definition, the body lives in a
         * different TU. Skip — the call-site mangle still needs to match
         * the (other-TU) definition, but we have no body to clone here. */
        if (is_decl_only && func_src == inner) continue;

        /* Clone with deduced substitutions */
        Node *cloned = clone_node(func_src, &deduced, arena);
        if (!cloned) continue;
        /* If the qualifier is a class-template instantiation
         * (Box<int>::convert), build a class_type carrying its
         * template_args so the def mangles as
         * 'sf__Box_t_int_te___convert_*' — matching the call site
         * which emits via mangle_class_tag(). Without this the def
         * would mangle as the bare class name 'sf__Box__convert_*'
         * and link would fail. N4659 §17.5.2 [temp.mem]. */
        cloned->func.class_type = mr->entry->owner_class;
        if (mr->class_tid && mr->class_tid->kind == ND_TEMPLATE_ID &&
            mr->entry->owner_class) {
            int tna = mr->class_tid->template_id.nargs;
            if (tna > 0) {
                Type *ct = arena_alloc(arena, sizeof(Type));
                *ct = *mr->entry->owner_class;
                ct->n_template_args = tna;
                ct->template_args = arena_alloc(arena, tna * sizeof(Type *));
                for (int i = 0; i < tna; i++)
                    ct->template_args[i] = type_arg_from_node(
                        mr->class_tid->template_id.args[i]);
                cloned->func.class_type = ct;
            }
        }

        /* N4659 §16.3 [over.match]: build TY_FUNC from cloned params
         * and set as resolved_type on the call-site callee so the
         * param suffix matches between definition and call. */
        if (mr->call_node && mr->call_node->call.callee) {
            int cnp = cloned->func.nparams;
            Type **cparams = NULL;
            if (cnp > 0) {
                cparams = arena_alloc(arena, cnp * sizeof(Type *));
                for (int i = 0; i < cnp; i++)
                    cparams[i] = cloned->func.params[i]->param.ty;
            }
            Type *ft = arena_alloc(arena, sizeof(Type));
            ft->kind = TY_FUNC;
            ft->ret = cloned->func.ret_ty;
            ft->params = cparams;
            ft->nparams = cnp;
            mr->call_node->call.callee->resolved_type = ft;
        }

        /* Register in dedup set */
        Type *dummy = arena_alloc(arena, sizeof(Type));
        dummy->kind = TY_FUNC;
        dedup_add(&ds, key, pos, dummy);

        if (total_inst < MAX_INST) {
            all_instantiated[total_inst++] = cloned;
            ninst_this_round++;
        }
    }

    /* Phase 3b: class + function template instantiation */
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
        /* Deduce remaining function-template params from call args.
         * Pattern: 'template<T, U> bool is_a(U*)' invoked as
         * 'is_a<Cat>(&thing)' — only T is explicit; U deduces from
         * the arg. Adds bindings to tmp_map for any deducible
         * parameter not already bound by explicit args or defaults.
         * N4659 §17.8.2.1 [temp.deduct.call]. */
        if (req->arg_types && tmp_map.nentries < np) {
            Node *inner = tmpl->template_decl.decl;
            if (inner)
                deduce_template_args(inner, req->arg_types, req->nargs,
                                      &tmp_map);
        }
        /* If deduction added bindings beyond the explicit usage args,
         * extend the template_id's args in place so it reflects the
         * full instantiation (explicit + deduced). instantiate_one
         * builds its own SubstMap from template_id->args; without
         * this the downstream pass wouldn't see the deduced bindings
         * and the dedup key would collide across same-T-different-U
         * requests.
         *
         * PREVIOUSLY we created a synthetic Node and swapped
         * req->template_id to it — but the rewrite at the end of the
         * dedup branch (req->template_id->kind = ND_IDENT) then fired
         * on the synthetic, leaving the real in-tree node unchanged.
         * Calls in cloned bodies then reached emit_expr with an
         * ND_TEMPLATE_ID kind and fell through to the placeholder.
         * Extend in place so the same Node is the rewrite target. */
        if (tmp_map.nentries > na) {
            Node **new_args = arena_alloc(arena,
                tmp_map.nentries * sizeof(Node *));
            for (int i = 0; i < tmp_map.nentries; i++) {
                if (i < na) {
                    new_args[i] = req->template_id->template_id.args[i];
                } else {
                    Node *arg = arena_alloc(arena, sizeof(Node));
                    memset(arg, 0, sizeof(Node));
                    arg->kind = ND_VAR_DECL;
                    arg->var_decl.ty = tmp_map.entries[i].concrete_type;
                    new_args[i] = arg;
                }
            }
            req->template_id->template_id.args = new_args;
            req->template_id->template_id.nargs = tmp_map.nentries;
            na = tmp_map.nentries;
        }

        /* Dedup check — use ALL usage args (not just the map, which
         * may exclude fixed args from partial specializations). */
        char key[MAX_DEDUP_KEY];
        int key_len = 0;
        if (req->name && key_len + req->name->len < MAX_DEDUP_KEY) {
            memcpy(key, req->name->loc, req->name->len);
            key_len = req->name->len;
        }
        key[key_len++] = '\0';
        /* Include all usage args (explicit + defaults from map) */
        int total_args = na > tmp_map.nentries ? na : tmp_map.nentries;
        for (int i = 0; i < total_args; i++) {
            Type *arg_ty = (i < na) ?
                type_arg_from_node(req->template_id->template_id.args[i]) :
                (i < tmp_map.nentries ? tmp_map.entries[i].concrete_type : NULL);
            key_len = type_to_key(arg_ty, key, key_len, MAX_DEDUP_KEY);
            key[key_len++] = '\0';
        }
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
            /* For function template duplicates: rewrite the call-site
             * ND_TEMPLATE_ID to ND_IDENT with the mangled name, so
             * codegen emits the correct function name. */
            /* Only rewrite FUNCTION template calls to mangled idents.
             * For a class template's constructor-call form 'vec<T>()',
             * the collect path at line ~590 creates a request with
             * usage_type=NULL, but the rewrite below would mangle
             * the callee as if it were a function template — a name
             * that doesn't match the class's actual ctor mangling.
             * Skip the rewrite when the template's inner decl is a
             * class. Pattern: gcc 4.8 ipa-cp.c 'return vec<T,A,L>();'. */
            Node *tmpl_inner = req->tmpl_def ? req->tmpl_def->template_decl.decl : NULL;
            bool is_class_tmpl = tmpl_inner && tmpl_inner->kind == ND_CLASS_DEF;
            if (req->template_id->kind == ND_TEMPLATE_ID &&
                !req->usage_type && !is_class_tmpl) {
                Token *fname = req->template_id->template_id.name;
                int na = req->template_id->template_id.nargs;
                int bufsize = 512;
                char *buf = arena_alloc(arena, bufsize);
                int pos = 0;
                if (fname)
                    pos += snprintf(buf + pos, bufsize - pos, "%.*s",
                                    fname->len, fname->loc);
                pos += snprintf(buf + pos, bufsize - pos, "_t_");
                for (int i = 0; i < na; i++) {
                    if (i > 0 && pos < bufsize - 1) buf[pos++] = '_';
                    Type *at = type_arg_from_node(
                        req->template_id->template_id.args[i]);
                    pos = mangle_type_to_buf(at, buf, pos, bufsize);
                }
                pos += snprintf(buf + pos, bufsize - pos, "_te_");
                /* Param suffix — must match the first-instantiation
                 * site's mangling (see comment there). The dedup
                 * `existing` Type is the cloned function's TY_FUNC
                 * with proper params. */
                pos += snprintf(buf + pos, bufsize - pos, "_p_");
                if (existing && existing->kind == TY_FUNC && existing->params) {
                    for (int i = 0; i < existing->nparams; i++) {
                        if (i > 0 && pos < bufsize - 1) buf[pos++] = '_';
                        pos = mangle_type_to_buf(existing->params[i],
                                                  buf, pos, bufsize);
                    }
                }
                pos += snprintf(buf + pos, bufsize - pos, "_pe_");
                Token *mangled = arena_alloc(arena, sizeof(Token));
                if (fname) *mangled = *fname;
                else memset(mangled, 0, sizeof(Token));
                mangled->loc = buf;
                mangled->len = pos;
                mangled->kind = TK_IDENT;
                req->template_id->kind = ND_IDENT;
                req->template_id->ident.name = mangled;
                req->template_id->ident.implicit_this = false;
                req->template_id->ident.resolved_decl = NULL;
                req->template_id->ident.overload_set = NULL;
                req->template_id->ident.n_overloads = 0;
                /* Carry the previously-built TY_FUNC across the dedup
                 * hit — see comment at the first instantiation site. */
                if (existing && existing->kind == TY_FUNC && existing->params)
                    req->template_id->resolved_type = existing;
            }
            continue;
        }

        /* Check for a full specialization that matches the requested
         * args. If found, use the specialization's concrete definition
         * directly instead of cloning the primary template.
         *
         * If the usage args are shorter than the primary's param list
         * (defaults expansion applies), try matching against a SYNTHETIC
         * template-id whose args are the expanded usage. This lets
         *   template<typename T, typename A = X, typename L = Y>
         *   struct vec<T, A, vl_ptr> { ... };  // partial spec
         * match a usage 'vec<int>' that expands to 'vec<int, X, vl_ptr>'.
         *
         * N4659 §17.6.2.3 [temp.arg.type]/2 — default template
         * arguments are considered when matching a template-id
         * against a specialization; the specialization is selected
         * using the fully-substituted arguments. Our retry with the
         * expanded arg list implements this for the common case. */
        Node *spec = registry_find_specialization(
            &reg, req->name->loc, req->name->len, req->template_id);
        if (!spec && np > na && tmp_map.nentries == np) {
            /* Build synthetic template-id from the expanded tmp_map. */
            Node *syn = arena_alloc(arena, sizeof(Node));
            *syn = *req->template_id;
            syn->template_id.nargs = np;
            syn->template_id.args = arena_alloc(arena, np * sizeof(Node *));
            for (int i = 0; i < np; i++) {
                Node *arg = arena_alloc(arena, sizeof(Node));
                memset(arg, 0, sizeof(Node));
                arg->kind = ND_VAR_DECL;
                arg->var_decl.ty = tmp_map.entries[i].concrete_type;
                syn->template_id.args[i] = arg;
            }
            spec = registry_find_specialization(
                &reg, req->name->loc, req->name->len, syn);
            if (spec) req->template_id = syn;  /* use expanded for instantiation */
        }

        Node **extra_methods = NULL;
        int nextra = 0;
        Node *inst;
        if (spec && spec->template_decl.nparams > 0) {
            /* Partial specialization — clone with substitution.
             * Build a SubstMap by matching the specialization's
             * pattern args against the usage args. TY_DEPENDENT
             * positions in the pattern become bindings. */
            inst = instantiate_one(spec, req->template_id,
                                    arena, tu, &extra_methods, &nextra);
        } else if (spec) {
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
            /* Phase-2 sema on the freshly-cloned subtree.
             * N4659 §17.7 [temp.res] — names that became non-dependent
             * after substitution need re-resolution. The visitor does
             * the same work as the initial TU pass; in particular,
             * visit_call's bare-ident-template rewrite (sema/sema.c
             * ~1167) fires on calls inside the cloned body that now
             * see concrete arg types, producing ND_TEMPLATE_ID
             * callees that the next collect_from_node round will
             * pick up as new instantiation requests.
             *
             * For class-template instantiations, walk the class's
             * methods and visit each func body. For function-template
             * instantiations, visit the func directly. For OOL methods
             * (extra_methods below), visit them too. */
            if (inst->kind == ND_FUNC_DEF || inst->kind == ND_FUNC_DECL) {
                sema_visit_node(inst, arena);
            } else if (inst->kind == ND_CLASS_DEF) {
                for (int mi = 0; mi < inst->class_def.nmembers; mi++) {
                    Node *m = inst->class_def.members[mi];
                    if (m && (m->kind == ND_FUNC_DEF || m->kind == ND_FUNC_DECL))
                        sema_visit_node(m, arena);
                }
            }
            if (total_inst < MAX_INST) {
                all_instantiated[total_inst++] = inst;
                ninst_this_round++;
            }
            /* (trace removed) */
            /* For class instantiations: dedup_add unconditionally so
             * a subsequent request for the same (name, args) finds
             * the existing entry and short-circuits. Previously the
             * dedup_add was nested inside the usage_type != NULL
             * guard, so class-template instantiations requested via
             * a constructor call (ND_CALL callee=ND_TEMPLATE_ID, which
             * sets usage_type=NULL) didn't register — and a later
             * type-position request for the same class instantiated
             * AGAIN, producing duplicate ND_CLASS_DEFs and ultimately
             * duplicate method definitions at link time. Pattern:
             * gcc 4.8 ipa-cp.c with vec<ipa_agg_jf_item> constructed
             * functionally AND used in type position. */
            if (inst->kind == ND_CLASS_DEF && inst->class_def.ty) {
                Type *inst_ty = inst->class_def.ty;
                if (req->usage_type) {
                    req->usage_type->template_args    = inst_ty->template_args;
                    req->usage_type->n_template_args   = inst_ty->n_template_args;
                    req->usage_type->class_region      = inst_ty->class_region;
                    req->usage_type->class_def         = inst_ty->class_def;
                    req->usage_type->has_dtor          = inst_ty->has_dtor;
                    req->usage_type->has_default_ctor  = inst_ty->has_default_ctor;
                }
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
            /* Add out-of-class method instantiations + phase-2 sema. */
            for (int e = 0; e < nextra; e++) {
                Node *em = extra_methods[e];
                if (em && (em->kind == ND_FUNC_DEF || em->kind == ND_FUNC_DECL))
                    sema_visit_node(em, arena);
                if (total_inst < MAX_INST) {
                    all_instantiated[total_inst++] = em;
                    ninst_this_round++;
                }
            }
        }
        /* Register function template instantiations in the dedup set
         * so the same function isn't instantiated multiple times from
         * different call sites. Store the instantiation's TY_FUNC as
         * the dedup value so the dedup-hit path below can wire it
         * onto the call-site callee's resolved_type for ref-arg
         * adaptation. */
        if (inst && (inst->kind == ND_FUNC_DEF || inst->kind == ND_FUNC_DECL)) {
            Type *fty = build_func_type_from_node(inst, arena);
            if (!fty) {
                fty = arena_alloc(arena, sizeof(Type));
                memset(fty, 0, sizeof(Type));
                fty->kind = TY_FUNC;
            }
            dedup_add(&ds, key, key_len, fty);
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
        /* Full specializations (template<> struct X<int> { ... }) are
         * ND_TEMPLATE_DECL wrapping ND_CLASS_DEF — count them too so
         * instantiations that reference the specialization come after. */
        if (d && d->kind == ND_TEMPLATE_DECL &&
            d->template_decl.nparams == 0 && d->template_decl.decl &&
            d->template_decl.decl->kind == ND_CLASS_DEF)
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

    /* Post-loop: resolve template base classes. During instantiation,
     * a derived template's base type (e.g. base_t<int>) may not have
     * been instantiated yet when the derived class was cloned. Now
     * that all instantiations are done, walk each instantiated class's
     * base_types and link any that now have class_region set. Also
     * check the dedup set for bases that were instantiated.
     *
     * ALSO: plain (non-template) classes that inherit from a template
     * instantiation (e.g. 'struct asan_hasher : typed_noop_remove<int>'
     * in gcc 4.8 hash_table users) have the same problem — at parse
     * time the base's class_region was NULL because the template
     * hadn't been instantiated. Walk top-level plain ND_CLASS_DEFs
     * too. N4659 §13.1 [class.derived]. */
    Node *all_class_defs[1024];
    int n_all_class_defs = 0;
    for (int i = 0; i < total_inst; i++) {
        Node *inst = all_instantiated[i];
        if (inst && inst->kind == ND_CLASS_DEF &&
            n_all_class_defs < (int)(sizeof(all_class_defs)/sizeof(all_class_defs[0])))
            all_class_defs[n_all_class_defs++] = inst;
    }
    for (int i = 0; i < tu->tu.ndecls; i++) {
        Node *d = tu->tu.decls[i];
        if (d && d->kind == ND_CLASS_DEF &&
            n_all_class_defs < (int)(sizeof(all_class_defs)/sizeof(all_class_defs[0])))
            all_class_defs[n_all_class_defs++] = d;
    }
    for (int i = 0; i < n_all_class_defs; i++) {
        Node *inst = all_class_defs[i];
        if (!inst || inst->kind != ND_CLASS_DEF) continue;
        Type *ity = inst->class_def.ty;
        if (!ity || !ity->class_region) continue;
        for (int bi = 0; bi < inst->class_def.nbase_types; bi++) {
            Type *base_ty = inst->class_def.base_types[bi];
            if (!base_ty) continue;
            /* Already linked? */
            if (base_ty->class_region) {
                /* Check if already in bases list */
                bool found = false;
                for (int k = 0; k < ity->class_region->nbases; k++) {
                    if (ity->class_region->bases[k] == base_ty->class_region) {
                        found = true; break;
                    }
                }
                if (!found)
                    region_add_base_raw(ity->class_region,
                                         base_ty->class_region, arena);
                continue;
            }
            /* Try the dedup set — base may have been instantiated */
            if (base_ty->template_id_node &&
                base_ty->template_id_node->kind == ND_TEMPLATE_ID) {
                Node *tid = base_ty->template_id_node;
                char key[512];
                int pos = 0;
                if (tid->template_id.name) {
                    Token *tn = tid->template_id.name;
                    memcpy(key, tn->loc, tn->len);
                    pos = tn->len;
                }
                key[pos++] = '\0';
                for (int k = 0; k < tid->template_id.nargs; k++) {
                    Node *arg = tid->template_id.args[k];
                    Type *aty = (arg && arg->kind == ND_VAR_DECL) ?
                                arg->var_decl.ty : NULL;
                    pos = type_to_key(aty, key, pos, 512);
                    key[pos++] = '\0';
                }
                Type *resolved = dedup_find(&ds, key, pos);
                if (resolved && resolved->class_region) {
                    base_ty->class_region = resolved->class_region;
                    base_ty->class_def = resolved->class_def;
                    region_add_base_raw(ity->class_region,
                                         resolved->class_region, arena);
                }
            }
        }
    }

    /* Post-instantiation member-type patching: walk all instantiated
     * classes and patch any member/param/return type that references
     * another template (has template_id_node) against the dedup set.
     * This is the bridge between transitive instantiation rounds
     * and codegen: without it, a cloned method body's expression
     * types (e.g. 'vec<int, vl_embed>' inside vec<int, vl_ptr>)
     * have no class_region, so method dispatch falls through to
     * plain C emission.
     *
     * The base-type patching above handles inheritance. This pass
     * handles composition (by-pointer and by-value members) and
     * method bodies. */
    for (int i = 0; i < total_inst; i++) {
        Node *inst = all_instantiated[i];
        if (!inst || inst->kind != ND_CLASS_DEF) continue;
        for (int mi = 0; mi < inst->class_def.nmembers; mi++) {
            Node *m = inst->class_def.members[mi];
            if (!m) continue;
            Type *mty = NULL;
            if (m->kind == ND_VAR_DECL) mty = m->var_decl.ty;
            else if (m->kind == ND_FUNC_DEF) mty = m->func.ret_ty;
            if (!mty) continue;
            /* Peel pointer/ref/array to find the struct underneath */
            Type *inner = mty;
            while (inner && (inner->kind == TY_PTR || inner->kind == TY_REF ||
                             inner->kind == TY_RVALREF || inner->kind == TY_ARRAY))
                inner = inner->base;
            if (!inner || !(inner->kind == TY_STRUCT || inner->kind == TY_UNION))
                continue;
            if (inner->class_region) continue;  /* already resolved */
            if (!inner->template_id_node ||
                inner->template_id_node->kind != ND_TEMPLATE_ID)
                continue;
            Node *tid = inner->template_id_node;
            char key[512];
            int pos = 0;
            if (tid->template_id.name) {
                Token *tn = tid->template_id.name;
                if (pos + tn->len < 512) {
                    memcpy(key, tn->loc, tn->len);
                    pos = tn->len;
                }
            }
            key[pos++] = '\0';
            for (int k = 0; k < tid->template_id.nargs; k++) {
                Node *arg = tid->template_id.args[k];
                Type *aty = (arg && arg->kind == ND_VAR_DECL) ?
                            arg->var_decl.ty : NULL;
                pos = type_to_key(aty, key, pos, 512);
                key[pos++] = '\0';
            }
            Type *resolved = dedup_find(&ds, key, pos);
            if (resolved && resolved->class_region) {
                inner->template_args   = resolved->template_args;
                inner->n_template_args = resolved->n_template_args;
                inner->class_region    = resolved->class_region;
                inner->class_def       = resolved->class_def;
                inner->has_dtor        = resolved->has_dtor;
                inner->has_default_ctor = resolved->has_default_ctor;
            }
        }
    }

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
            /* Check by-value members */
            for (int m = 0; m < a->class_def.nmembers && !a_needs_b; m++) {
                Node *mem = a->class_def.members[m];
                if (!mem || mem->kind != ND_VAR_DECL) continue;
                Type *mty = mem->var_decl.ty;
                if (!mty) continue;
                if (mty->kind != TY_STRUCT && mty->kind != TY_UNION)
                    continue;
                if (mty->tag && bty->tag &&
                    mty->tag->len == bty->tag->len &&
                    memcmp(mty->tag->loc, bty->tag->loc,
                           mty->tag->len) == 0)
                    a_needs_b = true;
            }
            /* Also check base types — base classes are embedded
             * as __sf_base members, so they must be defined first. */
            for (int bt = 0; bt < a->class_def.nbase_types && !a_needs_b; bt++) {
                Type *base = a->class_def.base_types[bt];
                if (!base || !base->tag) continue;
                if (bty->tag &&
                    base->tag->len == bty->tag->len &&
                    memcmp(base->tag->loc, bty->tag->loc,
                           base->tag->len) == 0)
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
            /* Full specializations: ND_TEMPLATE_DECL wrapping ND_CLASS_DEF
             * with nparams == 0. Count them for insert ordering so
             * instantiations that depend on specializations come after. */
            if (d && d->kind == ND_TEMPLATE_DECL &&
                d->template_decl.nparams == 0 && d->template_decl.decl &&
                d->template_decl.decl->kind == ND_CLASS_DEF)
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
    case ND_SIZEOF:
        patch_type(n->sizeof_.ty, ds, arena);
        patch_node_types(n->sizeof_.expr, ds, arena);
        break;
    case ND_ALIGNOF:
        patch_type(n->alignof_.ty, ds, arena);
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
