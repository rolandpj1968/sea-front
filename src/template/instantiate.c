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

struct TmplRegistry {
    TmplEntry *buckets[TMPL_REGISTRY_SIZE];
    Arena     *arena;
};

/* hash_name is declared in parse.h and defined in lookup.c. */

/* Push a freshly-instantiated node into all_instantiated and bump
 * ninst_this_round in lockstep.
 *
 * The end-of-round merge computes
 *   all_instantiated.data[arr->len - ninst_this_round .. arr->len]
 * If the bumps drift (e.g. ninst_this_round++ outside this helper),
 * the start index goes negative and the merge reads pre-buffer
 * garbage into tu->tu.decls. Bug fixed in 2a7fca4 — keep them
 * coupled here. The 'what' arg is preserved for grep-ability of
 * historical "instantiation cap exceeded" diagnostics. */
static void inst_push(Vec *arr, int *ninst_this_round,
                      Node *node, const char *what) {
    (void)what;
    vec_push(arr, node);
    (*ninst_this_round)++;
}

/* Walk an instantiated decl's subtree for ND_LAMBDA nodes and push
 * each cloned closure ND_CLASS_DEF + lambda func_def into
 * all_instantiated[] BEFORE the surrounding decl. The clone pass
 * (clone.c ND_LAMBDA case) leaves the lambda fn + closure attached
 * to the cloned ND_LAMBDA when the lambda is inside a template body
 * (parser also defers TU-top hoisting in the same case); this walker
 * is what actually puts them into the emit list per instantiation.
 *
 * Order: closure first (struct visible before fn that takes a
 * pointer to it), then the lambda fn, then by caller convention the
 * surrounding instantiated decl. */
static void collect_inner_lambdas_in_node(Node *n, Vec *arr,
                                           int *ninst_this_round);
static void collect_inner_lambdas(Node *n, Vec *arr,
                                   int *ninst_this_round) {
    if (!n) return;
    if (n->kind == ND_LAMBDA) {
        if (n->lambda.closure_type && n->lambda.closure_type->class_def)
            inst_push(arr, ninst_this_round,
                      n->lambda.closure_type->class_def,
                      "instantiated lambda closure");
        if (n->lambda.func_def)
            inst_push(arr, ninst_this_round,
                      n->lambda.func_def,
                      "instantiated lambda fn");
        /* Lambda body may itself contain nested lambdas — recurse. */
        if (n->lambda.func_def && n->lambda.func_def->func.body)
            collect_inner_lambdas(n->lambda.func_def->func.body, arr,
                                   ninst_this_round);
        return;
    }
    collect_inner_lambdas_in_node(n, arr, ninst_this_round);
}
static void collect_inner_lambdas_in_node(Node *n, Vec *arr,
                                           int *ninst_this_round) {
    if (!n) return;
    switch (n->kind) {
    case ND_FUNC_DEF: case ND_FUNC_DECL:
        for (int i = 0; i < n->func.nparams; i++)
            collect_inner_lambdas(n->func.params[i], arr, ninst_this_round);
        collect_inner_lambdas(n->func.body, arr, ninst_this_round);
        break;
    case ND_CLASS_DEF:
        for (int i = 0; i < n->class_def.nmembers; i++)
            collect_inner_lambdas(n->class_def.members[i], arr,
                                   ninst_this_round);
        break;
    case ND_BLOCK:
        for (int i = 0; i < n->block.nstmts; i++)
            collect_inner_lambdas(n->block.stmts[i], arr,
                                   ninst_this_round);
        break;
    case ND_VAR_DECL: case ND_PARAM:
        collect_inner_lambdas(n->var_decl.init, arr, ninst_this_round);
        break;
    case ND_RETURN:
        collect_inner_lambdas(n->ret.expr, arr, ninst_this_round);
        break;
    case ND_EXPR_STMT:
        collect_inner_lambdas(n->expr_stmt.expr, arr, ninst_this_round);
        break;
    case ND_IF:
        collect_inner_lambdas(n->if_.cond,  arr, ninst_this_round);
        collect_inner_lambdas(n->if_.then_, arr, ninst_this_round);
        collect_inner_lambdas(n->if_.else_, arr, ninst_this_round);
        break;
    case ND_WHILE: case ND_DO:
        collect_inner_lambdas(n->while_.cond, arr, ninst_this_round);
        collect_inner_lambdas(n->while_.body, arr, ninst_this_round);
        break;
    case ND_FOR:
        collect_inner_lambdas(n->for_.init, arr, ninst_this_round);
        collect_inner_lambdas(n->for_.cond, arr, ninst_this_round);
        collect_inner_lambdas(n->for_.inc,  arr, ninst_this_round);
        collect_inner_lambdas(n->for_.body, arr, ninst_this_round);
        break;
    case ND_RANGE_FOR:
        collect_inner_lambdas(n->range_for.decl,  arr, ninst_this_round);
        collect_inner_lambdas(n->range_for.range, arr, ninst_this_round);
        collect_inner_lambdas(n->range_for.body,  arr, ninst_this_round);
        break;
    case ND_SWITCH:
        collect_inner_lambdas(n->switch_.init, arr, ninst_this_round);
        collect_inner_lambdas(n->switch_.expr, arr, ninst_this_round);
        collect_inner_lambdas(n->switch_.body, arr, ninst_this_round);
        break;
    case ND_CASE:
        collect_inner_lambdas(n->case_.expr, arr, ninst_this_round);
        collect_inner_lambdas(n->case_.stmt, arr, ninst_this_round);
        break;
    case ND_DEFAULT:
        collect_inner_lambdas(n->default_.stmt, arr, ninst_this_round);
        break;
    case ND_LABEL:
        collect_inner_lambdas(n->label.stmt, arr, ninst_this_round);
        break;
    case ND_CALL:
        collect_inner_lambdas(n->call.callee, arr, ninst_this_round);
        for (int i = 0; i < n->call.nargs; i++)
            collect_inner_lambdas(n->call.args[i], arr, ninst_this_round);
        break;
    case ND_BINARY: case ND_ASSIGN: case ND_COMMA:
        collect_inner_lambdas(n->binary.lhs, arr, ninst_this_round);
        collect_inner_lambdas(n->binary.rhs, arr, ninst_this_round);
        break;
    case ND_UNARY: case ND_POSTFIX:
        collect_inner_lambdas(n->unary.operand, arr, ninst_this_round);
        break;
    case ND_TERNARY:
        collect_inner_lambdas(n->ternary.cond,  arr, ninst_this_round);
        collect_inner_lambdas(n->ternary.then_, arr, ninst_this_round);
        collect_inner_lambdas(n->ternary.else_, arr, ninst_this_round);
        break;
    case ND_MEMBER:
        collect_inner_lambdas(n->member.obj, arr, ninst_this_round);
        break;
    case ND_SUBSCRIPT:
        collect_inner_lambdas(n->subscript.base, arr, ninst_this_round);
        collect_inner_lambdas(n->subscript.index, arr, ninst_this_round);
        break;
    case ND_CAST:
        collect_inner_lambdas(n->cast.operand, arr, ninst_this_round);
        break;
    case ND_INIT_LIST:
        for (int i = 0; i < n->init_list.nelems; i++)
            collect_inner_lambdas(n->init_list.elems[i], arr, ninst_this_round);
        break;
    default:
        /* Leaf or non-recursive — no children to walk. */
        break;
    }
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
        Node *inner = n->template_decl.decl;
        /* OOL definition of a class-template member method:
         *   template<typename T, typename A>
         *   void vec<T, A, vl_ptr>::splice(vec<T, A, vl_ptr> &src) { ... }
         * The leading template head re-states the OUTER class
         * template's params; the method itself is NOT a free
         * function template. Detected by inner being a function with
         * class_type set (or qual_tid carrying the class qualifier
         * template-id). Skip top-level registration — the call-site
         * resolution path uses the class membership, not a free
         * lookup keyed on the bare method name.
         *
         * Without this filter, registry_add records the OOL func
         * under its plain method name; subsequent lookups treat it
         * as a free function template, the bare-template rewrite
         * synthesises a template-id with the OOL's params bound to
         * the deduced types, and the cloned function's class_type
         * keeps its TY_DEPENDENT outer params (T leaks into the
         * mangled symbol). N4659 §17.5.2/2 [temp.mem]: a member of
         * a class template defined outside its class definition is
         * a member, not a separate template. */
        bool is_ool_method = inner &&
            (inner->kind == ND_FUNC_DEF || inner->kind == ND_FUNC_DECL) &&
            (inner->func.class_type != NULL ||
             inner->func.qual_tid != NULL);
        if (name && !is_ool_method)
            registry_add(reg, name->loc, name->len, n);
        /* If the template wraps a class def, descend so any member
         * templates inside the class template are also registered.
         * N4659 §17.5.2 [temp.mem] permits 'template<class T> struct
         * Box { template<class U> static T *cast(U *); };' — both
         * heads need separate registration so a qualified call like
         * Box<int>::cast<float>(p) can find the member template. */
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
    /* For member class templates of a class template (e.g.
     * 'Outer<int>::Inner<int>' used inside Outer<int>'s methods —
     * N4659 §17.5.2 [temp.mem]/2): the fully-instantiated enclosing
     * class type. The cloned struct's tag is rewritten to a
     * synthesized 'OuterTag_t_outerargs_te___InnerTag' so the
     * mangled symbol scopes the inner specialization under the
     * outer one (Itanium C++ ABI §5.1.5 — substitution encodes
     * nested class-template specializations). NULL for top-level
     * class/function templates. */
    Type  *member_owner;
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
    Type      *enclosing_class; /* fully-instantiated enclosing class type
                              * for unqualified calls inside a cloned
                              * class-template member body. The sibling
                              * resolves into the same specialization
                              * (N4659 §6.4.1/13 [basic.lookup.unqual]:
                              * unqualified lookup in a class template
                              * member function uses the class scope of
                              * the specialization), and its definition
                              * is implicitly instantiated on use
                              * (§17.7.1/9 [temp.inst]). The cloned
                              * sibling's def must mangle with the SAME
                              * outer args as the calling method (Itanium
                              * C++ ABI §5.1.5), and its body needs the
                              * fully-defined struct (not a forward decl)
                              * for member access. NULL for qualified
                              * calls (use class_tid). */
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
    /* Stack of ND_CLASS_DEF nodes currently being walked. Used by the
     * ND_TYPEDEF branch to skip descents that would re-enter a class
     * body already in flight ('class X { typedef X Self; ... };' — STL
     * containers like std::vector use this idiom). Tag comparison via
     * Type* fails when an instantiation produces fresh Type pointers
     * for the same logical type; comparing class_def Node* is robust. */
    Node              *walking_class_defs[32];
    int                walking_class_def_depth;
} InstCollector;

/* Build a MemberTmplRequest from a call site and push it onto the
 * collector's member-request list. Centralises the build + arg-type
 * capture + linked-list insertion shared by the qualified-call,
 * sibling-call, and member-access-call paths. N4659 §17.5.2 [temp.mem]. */
static void record_member_request(InstCollector *col, TmplEntry *entry,
                                   Node *call_node,
                                   Node *class_tid,
                                   Type *enclosing_class) {
    MemberTmplRequest *mr = arena_alloc(col->arena, sizeof(MemberTmplRequest));
    mr->entry = entry;
    mr->call_node = call_node;
    mr->class_tid = class_tid;
    mr->enclosing_class = enclosing_class;
    int na = call_node ? call_node->call.nargs : 0;
    mr->nargs = na;
    if (na > 0) {
        mr->arg_types = arena_alloc(col->arena, na * sizeof(Type *));
        for (int i = 0; i < na; i++)
            mr->arg_types[i] = call_node->call.args[i]
                ? call_node->call.args[i]->resolved_type : NULL;
    } else {
        mr->arg_types = NULL;
    }
    mr->next = col->member_head;
    col->member_head = mr;
    col->member_count++;
}

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

    /* Resolve and cache the template definition on the template-id
     * node, BEFORE the dep-args bail-out. subst_type relies on
     * resolved_tmpl to walk a class template's body for dependent
     * member-typedef lookup ('typename T::value_type') when the
     * substituted type doesn't yet have its class_region populated.
     * Without caching here, template-ids with dependent args (e.g.
     * 'vec<T>' inside a class template body) never get resolved_tmpl
     * set, and subst_type can't recover the typedef. */
    Token *name = tid->template_id.name;
    Type  *member_owner = NULL;
    if (!tid->template_id.resolved_tmpl) {
        Node *t = registry_find(col->reg, name->loc, name->len);
        /* N4659 §17.5.2 [temp.mem]/2 + §6.4.1/13 [basic.lookup.unqual]:
         * an unqualified template-id inside a class-template member
         * function looks up the class scope; a member class template
         * is found there. Fall back to the member-template registry
         * keyed by enclosing class tag. Pattern: Outer<T>::call_inner
         * body references Inner<int> where Inner is a member class
         * template of Outer. */
        if (!t && col->cur_class && col->cur_class->tag) {
            TmplEntry *me = registry_find_member(col->reg,
                col->cur_class->tag->loc, col->cur_class->tag->len,
                name->loc, name->len);
            if (me) {
                t = me->tmpl;
                member_owner = col->cur_class;
            }
        }
        if (t) tid->template_id.resolved_tmpl = t;
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

    Node *tmpl = tid->template_id.resolved_tmpl;
    if (!tmpl) return;  /* template definition not found — skip */

    InstRequest *req = arena_alloc(col->arena, sizeof(InstRequest));
    req->name = name;
    req->template_id = tid;
    req->tmpl_def = tmpl;
    req->usage_type = ty;  /* patch this type after instantiation */
    req->member_owner = member_owner;
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
         * missed. Real-world shape: a file-scope anonymous struct
         * holding template-container members.
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
         * through TY_PTR/TY_ARRAY to find the struct. Real-world
         * shape: 'typedef struct _elim_graph { vec<int> nodes; ... }
         * *elim_graph;'. */
        {
            Type *tyw = n->var_decl.ty;
            while (tyw && (tyw->kind == TY_PTR || tyw->kind == TY_ARRAY) && tyw->base)
                tyw = tyw->base;
            /* Skip the descent when the typedef target's class_def is
             * already on the walk stack ('class X { typedef X Self; };',
             * common in STL containers like std::vector). Otherwise the
             * walk bounces infinitely between ND_TYPEDEF and ND_CLASS_DEF. */
            if (tyw && (tyw->kind == TY_STRUCT || tyw->kind == TY_UNION) &&
                tyw->class_def) {
                bool already_walking = false;
                for (int i = 0; i < col->walking_class_def_depth; i++) {
                    if (col->walking_class_defs[i] == tyw->class_def) {
                        already_walking = true;
                        break;
                    }
                }
                if (!already_walking)
                    collect_from_node(col, tyw->class_def);
            }
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

    case ND_CLASS_DEF: {
        /* Push class context so member declarations whose types use
         * an unqualified member template (e.g. 'vec<const char *>
         * targets;' inside class mkdeps where vec is a nested class
         * template) resolve via the member-template registry, same
         * as ND_FUNC_DEF below. N4659 §17.5.2 [temp.mem]/2 +
         * §6.4.1/13 [basic.lookup.unqual]: an unqualified template-id
         * inside a class body looks up the class scope.
         *
         * Without this, member-field types reference an instantiation
         * sea-front never emits — cc errors with 'field has incomplete
         * type' on the field decl. Real-world hit: gcc 14 libcpp's
         * mkdeps with five member fields of type 'vec<...>'. */
        Type *saved_class = col->cur_class;
        Node *saved_class_tid = col->cur_class_tid;
        if (n->class_def.ty) {
            col->cur_class = n->class_def.ty;
            col->cur_class_tid = NULL;
        }
        bool pushed_walking = false;
        if (col->walking_class_def_depth <
            (int)(sizeof(col->walking_class_defs) / sizeof(col->walking_class_defs[0]))) {
            col->walking_class_defs[col->walking_class_def_depth++] = n;
            pushed_walking = true;
        }
        for (int i = 0; i < n->class_def.nmembers; i++)
            collect_from_node(col, n->class_def.members[i]);
        /* Collect from base types — a template base like Base<T>
         * (substituted to Base<int>) needs to be instantiated too. */
        for (int i = 0; i < n->class_def.nbase_types; i++)
            collect_from_type(col, n->class_def.base_types[i]);
        if (pushed_walking)
            col->walking_class_def_depth--;
        col->cur_class = saved_class;
        col->cur_class_tid = saved_class_tid;
        break;
    }

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

    case ND_LABEL:
        /* labeled-statement (N4659 §9.1 [stmt.label]) wraps the next
         * statement after the label. Without this case the labeled
         * statement is silently skipped during instantiation
         * collection — any free-fn-template call inside (e.g.
         * `done: return vec_safe_length(...)`) leaves its
         * ND_TEMPLATE_ID callee uninstantiated and codegen emits
         * the bare unmangled name. */
        collect_from_node(col, n->label.stmt);
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
                    /* qualified path carries class_tid */
                    record_member_request(col, me, n,
                        n->call.callee->qualified.lead_tid, NULL);
                }
            }
        }
        /* Unqualified call inside a class member function — may name a
         * sibling member template. Per N4659 §6.4.1 [basic.lookup.unqual]
         * unqualified lookup from inside a class method finds sibling
         * members. Without this collection path the called template
         * was never instantiated → undefined symbol at link.
         * Match by current class context + member name.
         *
         * Also handles explicit-template-args calls: ND_TEMPLATE_ID
         * callees take this same path. Dispatch on the bare name and
         * let the member-template instantiation use whatever args
         * deduction can bind. */
        if (col->cur_class && col->cur_class->tag &&
            n->call.callee &&
            (n->call.callee->kind == ND_IDENT ||
             n->call.callee->kind == ND_TEMPLATE_ID)) {
            Token *cls = col->cur_class->tag;
            Token *name = (n->call.callee->kind == ND_IDENT)
                ? n->call.callee->ident.name
                : n->call.callee->template_id.name;
            if (!name) goto skip_sibling_member;
            TmplEntry *me = registry_find_member(col->reg,
                cls->loc, cls->len, name->loc, name->len);
            if (me) {
                /* Sibling call inherits the enclosing class<args>. */
                record_member_request(col, me, n,
                    col->cur_class_tid, col->cur_class);
            }
        }
    skip_sibling_member: ;
        /* Member-access call 'obj.method(args)' / 'p->method(args)'
         * where method is a member template. N4659 §17.5.2 [temp.mem] +
         * §16.3.1.1 [over.match.call.general]. The receiver carries
         * the enclosing class instantiation; we look up the method
         * name in that class's member-template registry. Real-world
         * shape: 'v.splice<T2,A2>(src)' on a templated container. */
        if (n->call.callee && n->call.callee->kind == ND_MEMBER &&
            n->call.callee->member.member &&
            n->call.callee->member.obj) {
            Type *ot = n->call.callee->member.obj->resolved_type;
            /* Peel ref/ptr layers — same shape as codegen does for
             * 'p->m()'. */
            while (ot && (ot->kind == TY_PTR || ot->kind == TY_REF ||
                          ot->kind == TY_RVALREF))
                ot = ot->base;
            if (ot && (ot->kind == TY_STRUCT || ot->kind == TY_UNION) &&
                ot->tag) {
                Token *name = n->call.callee->member.member;
                TmplEntry *me = registry_find_member(col->reg,
                    ot->tag->loc, ot->tag->len, name->loc, name->len);
                if (me)
                    record_member_request(col, me, n, NULL, ot);
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
                for (int i = 0; i < tid->template_id.nargs; i++)
                    syn->template_args[i] = template_arg_to_arg_type(
                        tid->template_id.args[i], col->arena);
            }
            collect_from_type(col, syn);
        } else if (n->qualified.resolved_class_type &&
                   n->qualified.resolved_class_type->template_id_node) {
            /* Cloned ND_QUALIFIED whose substituted leading qualifier
             * is a class-template instantiation. The original was
             * 'T::method' (parts[0] = plain ident T, lead_tid = NULL);
             * clone.c rewrites parts[0] to the substituted class's
             * tag and sets resolved_class_type = sub. Without this
             * arm, the call's owning class template (e.g.
             * pointer_hash<gimple_d>) isn't collected for
             * instantiation, so its OOL static methods never get
             * cloned and the call links to nowhere.
             * N4659 §17.7.1 [temp.inst] — implicit instantiation of
             * a class template covers any specialization referenced
             * by the program. */
            collect_from_type(col, n->qualified.resolved_class_type);
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
    /* Literal-valued NTTP placeholder — encode the literal text into
     * the dedup key so 'integral_constant<int,42>' and
     * '<int,99>' produce distinct keys. Without this, distinct NTTP
     * values dedup to a single instantiation and the surviving one
     * wins in the C output (correct symbol but wrong VALUE for the
     * other use-sites). */
    case TY_NTTP_VALUE:
        if (ty->tag)
            pos += snprintf(buf+pos, max-pos, "N%.*s",
                            ty->tag->len, ty->tag->loc);
        else
            pos += snprintf(buf+pos, max-pos, "N?");
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
     * function template (real-world shape: one function template
     * forwarding a reference param to a sibling template, e.g.
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
            if (pn && tokens_equal(pn, P->tag)) {
                /* Already bound — must be consistent */
                return true;  /* trust the first binding */
            }
        }
        subst_map_add(map, P->tag, A);
        return true;
    }

    /* Compound types: recurse structurally. If P is a compound type
     * containing a dependent name (e.g. T*) and A's outer kind doesn't
     * match, deduction FAILS — N4659 §17.8.2.5 [temp.deduct.type]/8:
     * unification of a dependent compound P with a non-matching A is
     * a deduction failure (per the per-pattern rules), which under
     * §17.8.2/3 [temp.deduct]/4 removes the candidate. Returning true
     * here would silently bind nothing for T and let an
     * incompatible candidate stay viable. */
    if (P->kind == TY_PTR && A->kind == TY_PTR)
        return deduce_from_pair(P->base, A->base, map);
    if (P->kind == TY_PTR) return !type_has_dependent(P);
    if (P->kind == TY_REF && A->kind == TY_REF)
        return deduce_from_pair(P->base, A->base, map);
    if (P->kind == TY_REF) return !type_has_dependent(P);
    if (P->kind == TY_ARRAY && A->kind == TY_ARRAY)
        return deduce_from_pair(P->base, A->base, map);
    if (P->kind == TY_ARRAY) return !type_has_dependent(P);

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
     * Real-world shape: 'template<T,A> f(vec<T,A,vl_embed>*)' called
     * with 'vec<X, Y, vl_embed>*' — PTR strips to struct, then we
     * unify template args here to bind T=X, A=Y. */
    if ((P->kind == TY_STRUCT || P->kind == TY_UNION) &&
        (A->kind == TY_STRUCT || A->kind == TY_UNION)) {
        /* Tag mismatch is unification failure. Deduction is checking
         * whether P's pattern can match A; different concrete classes
         * cannot unify regardless of whether either side carries a
         * dependent. (Template deduction has its own type rules — ICS
         * doesn't run inside template_args recursion, so we can't
         * defer the rejection to ICS the way we do for top-level
         * arg/param mismatch.) N4659 §17.8.2.5 [temp.deduct.type]. */
        if (!P->tag || !A->tag) return false;
        if (P->tag->len != A->tag->len) return false;
        if (memcmp(P->tag->loc, A->tag->loc, P->tag->len) != 0)
            return false;
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

    /* Detect trailing function-parameter pack (N4659 §17.5.3
     * [temp.variadic]). The pack consumes the trailing run of
     * call args; the non-pack leading params are paired 1:1 with
     * the leading args as before. Only the FUNC_DEF/FUNC_DECL
     * shape carries `param->is_pack` reliably (parser sets it
     * via the side channel); VAR_DECL function types don't. */
    bool has_func_def = (tmpl_func->kind == ND_FUNC_DEF ||
                          tmpl_func->kind == ND_FUNC_DECL);
    Node *pack_param = NULL;
    if (has_func_def && nparams > 0 &&
        tmpl_func->func.params[nparams - 1] &&
        tmpl_func->func.params[nparams - 1]->param.is_pack)
        pack_param = tmpl_func->func.params[nparams - 1];

    int n_non_pack = pack_param ? (nparams - 1) : nparams;
    int pairs = n_non_pack < nargs ? n_non_pack : nargs;
    for (int i = 0; i < pairs; i++) {
        Type *P = NULL;
        if (has_func_def)
            P = tmpl_func->func.params[i]->param.ty;
        else
            P = tmpl_func->var_decl.ty->params[i];
        if (!deduce_from_pair(P, arg_types[i], out))
            return false;
    }
    if (pack_param) {
        /* Walk through ref/ptr/array wrappers to find the dependent
         * leaf so forwarding-ref pack `Args&&... args` binds Args
         * correctly. Storage borrowed from the SubstMap's arena.
         * Includes the EMPTY-PACK case (`f()` with no trailing args)
         * — still bind as a 0-element pack so build_template_id
         * recognises the deduction succeeded and instantiation
         * picks up f<>(). */
        Type *leaf = pack_param->param.ty;
        while (leaf && (leaf->kind == TY_REF || leaf->kind == TY_RVALREF ||
                        leaf->kind == TY_PTR || leaf->kind == TY_ARRAY) &&
               leaf->base)
            leaf = leaf->base;
        if (leaf && leaf->kind == TY_DEPENDENT && leaf->tag) {
            int npack = (nargs > n_non_pack) ? (nargs - n_non_pack) : 0;
            if (npack == 1) {
                /* Single-arg case: bind as a regular non-pack entry
                 * so the cloner doesn't rename `args`→`args_0`. */
                subst_map_add(out, leaf->tag, arg_types[n_non_pack]);
            } else {
                /* 0 or 2+ args → genuine pack binding. */
                Type **pack_list = npack > 0
                    ? arena_alloc(out->arena, sizeof(Type *) * npack)
                    : NULL;
                for (int i = 0; i < npack; i++)
                    pack_list[i] = arg_types[n_non_pack + i];
                subst_map_add_pack(out, leaf->tag, pack_list, npack);
            }
        }
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
 * template params) return NULL — not yet supported here. The
 * mangling-aware variant (template_arg_to_arg_type, parse.h) also
 * handles literal NTTPs; use that for sites that build the
 * Type::template_args[] slot consumed by the mangler.
 */
static Type *type_arg_from_node(Node *arg) {
    if (!arg) return NULL;
    if (arg->kind == ND_VAR_DECL && arg->var_decl.ty)
        return arg->var_decl.ty;
    return NULL;
}

/* Walk an arbitrary AST subtree (TU, namespace ND_BLOCK, or
 * ND_CLASS_DEF body) looking for an ND_CLASS_DEF whose tag matches
 * `class_name`. Returns the class def node, or NULL. Symmetric with
 * find_enum_tag_in_tu_walk in emit_c — kept local to instantiate.c
 * since it's only consumed by the NTTP-from-static-const resolver
 * below. */
static Node *find_class_def_walk(Node *n, Token *class_name) {
    if (!n || !class_name) return NULL;
    switch (n->kind) {
    case ND_CLASS_DEF: {
        Token *tag = n->class_def.tag;
        if (tag && tag->len == class_name->len &&
            memcmp(tag->loc, class_name->loc, class_name->len) == 0)
            return n;
        for (int i = 0; i < n->class_def.nmembers; i++) {
            Node *r = find_class_def_walk(n->class_def.members[i], class_name);
            if (r) return r;
        }
        return NULL;
    }
    case ND_TRANSLATION_UNIT:
        for (int i = 0; i < n->tu.ndecls; i++) {
            Node *r = find_class_def_walk(n->tu.decls[i], class_name);
            if (r) return r;
        }
        return NULL;
    case ND_BLOCK:
        for (int i = 0; i < n->block.nstmts; i++) {
            Node *r = find_class_def_walk(n->block.stmts[i], class_name);
            if (r) return r;
        }
        return NULL;
    case ND_TEMPLATE_DECL:
        return find_class_def_walk(n->template_decl.decl, class_name);
    default:
        return NULL;
    }
}

/* Wrapper around template_arg_to_arg_type that ALSO resolves
 * ident/qualified-ref NTTPs to their initializer literal — see
 * nttp_ident_to_literal_tok below. Used at the inst_ty
 * template_args build sites so the mangled tag carries the literal
 * value rather than 'unknown'.
 *
 * Note: forward-declares nttp_ident_to_literal_tok which is the
 * sibling helper defined below. C ordering: the two helpers
 * cross-reference, so one needs a forward decl. */
static Token *nttp_ident_to_literal_tok(Node *tu, Node *arg);
static Type *template_arg_to_arg_type_resolved(Node *arg, Arena *arena, Node *tu) {
    Type *t = template_arg_to_arg_type(arg, arena);
    if (t) return t;
    Token *lit = nttp_ident_to_literal_tok(tu, arg);
    if (!lit) return NULL;
    Type *out = arena_alloc(arena, sizeof(Type));
    memset(out, 0, sizeof(Type));
    out->kind = TY_NTTP_VALUE;
    out->tag  = lit;
    return out;
}

/* Resolve an NTTP arg that's an identifier reference (ND_IDENT or
 * ND_QUALIFIED) to the literal token of its initializer, when the
 * referent is a class static-const integer member. Returns NULL
 * when the arg isn't an ident/qualified or the referent isn't
 * a known constant.
 *
 * Real-world hit: gcc 14 libcpp/include/rich-location.h
 *   class rich_location {
 *     static const int STATICALLY_ALLOCATED_RANGES = 3;
 *     semi_embedded_vec<location_range, STATICALLY_ALLOCATED_RANGES> ...;
 *   };
 * Without this resolution the NTTP is treated as opaque, the
 * mangled tag carries 'unknown' instead of '3', and the cloned
 * template body's array bound stays as the unsubstituted ident
 * which is undeclared at TU scope.
 *
 * Resolution paths:
 *   - ND_IDENT: use ident.resolved_decl (set by sema_run before
 *     instantiation) to find the home class, then walk its
 *     class_def for the matching member.
 *   - ND_QUALIFIED: parts = [ClassName, MemberName] — look up the
 *     class via the TU walker and walk its members.
 *
 * The integer constant must literally appear as the initializer
 * — sea-front has no constexpr evaluator, so '= N+1' won't fold.
 * Standard alignment (TODO seafront#nttp-constexpr-fold): hook in
 * a constexpr evaluator when one exists. */
/* Look in `cdef`'s member list for a static-const data member named
 * `member_name`. Returns its initializer's literal token if it's a
 * recognised constant kind; NULL otherwise. */
static Token *find_const_init_in_class(Node *cdef, Token *member_name) {
    if (!cdef || cdef->kind != ND_CLASS_DEF || !member_name) return NULL;
    for (int i = 0; i < cdef->class_def.nmembers; i++) {
        Node *m = cdef->class_def.members[i];
        if (!m || m->kind != ND_VAR_DECL) continue;
        Token *nm = m->var_decl.name;
        if (!nm) continue;
        if (nm->len != member_name->len ||
            memcmp(nm->loc, member_name->loc, member_name->len) != 0)
            continue;
        Node *init = m->var_decl.init;
        if (!init) return NULL;
        switch (init->kind) {
        case ND_NUM:
        case ND_FNUM:
        case ND_BOOL_LIT:
        case ND_NULLPTR:
            return init->tok;
        case ND_CHAR:
            return init->chr.tok;
        default:
            return NULL;
        }
    }
    return NULL;
}

/* Walk the TU recursively looking for any class with a static-const
 * member of `member_name`. Used as a fallback when an ND_IDENT NTTP
 * arg has no resolved_decl (sema doesn't currently walk template-id
 * arg expressions). Returns the first match — relies on the member
 * name being distinctive (the gcc 14 STATICALLY_ALLOCATED_RANGES
 * pattern). Standard alignment (TODO seafront#nttp-scope-aware):
 * threading the enclosing class context through the instantiation
 * pipeline lets us avoid the TU walk and disambiguate correctly. */
static Token *find_const_init_in_tu_walk(Node *n, Token *member_name) {
    if (!n) return NULL;
    switch (n->kind) {
    case ND_CLASS_DEF: {
        Token *t = find_const_init_in_class(n, member_name);
        if (t) return t;
        for (int i = 0; i < n->class_def.nmembers; i++) {
            t = find_const_init_in_tu_walk(n->class_def.members[i], member_name);
            if (t) return t;
        }
        return NULL;
    }
    case ND_TRANSLATION_UNIT:
        for (int i = 0; i < n->tu.ndecls; i++) {
            Token *t = find_const_init_in_tu_walk(n->tu.decls[i], member_name);
            if (t) return t;
        }
        return NULL;
    case ND_BLOCK:
        for (int i = 0; i < n->block.nstmts; i++) {
            Token *t = find_const_init_in_tu_walk(n->block.stmts[i], member_name);
            if (t) return t;
        }
        return NULL;
    case ND_TEMPLATE_DECL:
        return find_const_init_in_tu_walk(n->template_decl.decl, member_name);
    default:
        return NULL;
    }
}

static Token *nttp_ident_to_literal_tok(Node *tu, Node *arg) {
    if (!tu || !arg) return NULL;
    if (arg->kind == ND_IDENT && arg->ident.name) {
        /* Resolved-decl path (preferred when sema set it) */
        Declaration *d = arg->ident.resolved_decl;
        if (d && d->is_static_member && d->home && d->home->owner_type) {
            Node *cdef = find_class_def_walk(tu, d->home->owner_type->tag);
            Token *t = find_const_init_in_class(cdef, arg->ident.name);
            if (t) return t;
        }
        /* Fallback TU-walk by member name only — for ND_IDENT inside
         * template-id args where sema hasn't populated resolved_decl
         * (the visit() path doesn't enter template-id arg
         * expressions). Best-effort. */
        return find_const_init_in_tu_walk(tu, arg->ident.name);
    }
    if (arg->kind == ND_QUALIFIED &&
        arg->qualified.nparts >= 2 && arg->qualified.parts) {
        Token *class_name  = arg->qualified.parts[0];
        Token *member_name = arg->qualified.parts[arg->qualified.nparts - 1];
        if (!class_name || !member_name) return NULL;
        Node *cdef = find_class_def_walk(tu, class_name);
        return find_const_init_in_class(cdef, member_name);
    }
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
         * (e.g. a class template carrying 'template<typename T> class
         * Allocator' as one of its parameters) carry no Type —
         * sea-front parses them but doesn't model the inner template-
         * parameter-list. Treating NULL as a wildcard lets the OOL of
         * 'C<D,A>::create' bind to the instantiated 'C<X>' (where the
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
     * them to unify AND have the same pattern shape — same positions
     * dependent on each side. C++ partial-spec ordering (N4659
     * §17.6.5.2 [temp.func.order]/2 + §17.6.5 [temp.class.spec.match])
     * makes each partial spec a distinct entity; an OOL definition
     * belongs to exactly one. Without the shape check, a less-
     * specialized OOL like 'vec<T,A,vl_ptr>::splice' would bind to
     * the more-specialized empty 'vec<T,va_gc,vl_ptr>' partial spec
     * (T,A both dependent in OOL; T dependent + va_gc concrete in
     * target — template_ids_unify accepts because TY_DEPENDENT in
     * pattern matches any concrete, but the specs are different
     * entities and the OOL doesn't apply). */
    Node *m_tid = inner->func.qual_tid;
    Node *t_tid = target_class->template_id_node;
    if (m_tid && t_tid) {
        if (!template_ids_unify(m_tid, t_tid)) return false;
        if (m_tid->template_id.nargs != t_tid->template_id.nargs)
            return false;
        for (int i = 0; i < m_tid->template_id.nargs; i++) {
            Node *aa = m_tid->template_id.args[i];
            Node *bb = t_tid->template_id.args[i];
            Type *at = (aa && aa->kind == ND_VAR_DECL) ? aa->var_decl.ty : NULL;
            Type *bt = (bb && bb->kind == ND_VAR_DECL) ? bb->var_decl.ty : NULL;
            bool a_dep = !at || at->kind == TY_DEPENDENT;
            bool b_dep = !bt || bt->kind == TY_DEPENDENT;
            if (a_dep != b_dep) return false;
        }
        return true;
    }
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
         * type parameter. Real-world shape:
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
                    if (tn && tokens_equal(tn, t->tag)) {
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
static void find_ool_methods(Node *tu, Type *class_type, Vec *out) {
    if (!tu || !class_type || !class_type->tag) return;
    for (int i = 0; i < tu->tu.ndecls; i++) {
        Node *n = tu->tu.decls[i];
        if (!n) continue;
        /* Recurse into namespace blocks */
        if (n->kind == ND_BLOCK) {
            for (int j = 0; j < n->block.nstmts; j++) {
                Node *m = n->block.stmts[j];
                if (!m || m->kind != ND_TEMPLATE_DECL) continue;
                if (ool_method_matches(m, class_type))
                    vec_push(out, m);
            }
            continue;
        }
        if (n->kind != ND_TEMPLATE_DECL) continue;
        if (ool_method_matches(n, class_type))
            vec_push(out, n);
    }
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
 *
 * 'member_owner' is non-NULL for member class/function templates of
 * a class template (e.g. cloning Outer<int>::Inner<U>): the inner
 * template's body references both Outer's params (T) and Inner's (U).
 * The SubstMap is pre-seeded with Outer's param→arg bindings before
 * the inner head's bindings are added. N4659 §17.5.2 [temp.mem]/2.
 */
/* Build the inst_ty's template_args array.
 *
 * Single source of truth for "what types fill an instantiated template
 * type's template_args slot, and what does each slot know about
 * itself." Two responsibilities, deliberately fused so neither can
 * silently drift:
 *
 *   1. Per-position synthesis. For each `i` in [0, max(nargs, nparams)),
 *      either take the usage arg (i < nargs) or the SubstMap default
 *      (i in [nargs, nparams)).
 *
 *   2. NTTP type attribution. When a synthesized arg is TY_NTTP_VALUE,
 *      stash the corresponding template parameter's declared type
 *      (from `tmpl->template_decl.params[i]->param.ty`) onto the
 *      arg's `nttp_decl_type` field. Mangling reads this directly so
 *      it doesn't need to text-match the literal.
 *
 * Replaces the duplicate per-arg loops at the inst_ty build site;
 * adding new attribution here keeps both the explicit-arg and
 * default-arg paths in sync.
 */
static void build_inst_template_args(Type *inst_ty, Node *tmpl,
                                      Node *template_id,
                                      SubstMap *map, int outer_nparams,
                                      Arena *arena, Node *tu) {
    int nargs   = template_id->template_id.nargs;
    int nparams = tmpl ? tmpl->template_decl.nparams : 0;
    int n = nargs > nparams ? nargs : nparams;
    if (n <= 0) return;

    inst_ty->template_args = arena_alloc(arena, n * sizeof(Type *));
    inst_ty->n_template_args = n;

    for (int i = 0; i < n; i++) {
        Type *t = NULL;
        if (i < nargs) {
            t = template_arg_to_arg_type_resolved(
                template_id->template_id.args[i], arena, tu);
        } else {
            int mi = outer_nparams + i;
            t = (mi < map->nentries) ? map->entries[mi].concrete_type
                                      : NULL;
        }
        /* NTTP attribution — propagate the parameter's declared type
         * onto the arg Type so mangling reads it from structured data.
         * The declared type may itself be a template parameter (e.g.
         * 'template<typename T, T V> ...' — V's declared type is `T`,
         * which is TY_DEPENDENT until we substitute. Resolve through
         * the SubstMap so `integral_constant<bool, false>`'s V has
         * nttp_decl_type = bool, not the pre-substitution T placeholder.
         * Without this, builtin_code() falls back and every bool/int/long
         * NTTP collapses to the same Itanium `Li0E` literal encoding. */
        if (t && t->kind == TY_NTTP_VALUE && tmpl &&
            i < tmpl->template_decl.nparams) {
            Node *param = tmpl->template_decl.params[i];
            if (param && param->kind == ND_PARAM && param->param.ty) {
                Type *resolved = subst_type(param->param.ty, map, arena);
                t->nttp_decl_type = resolved ? resolved : param->param.ty;
            }
        }
        inst_ty->template_args[i] = t;
    }
}

static Node *instantiate_one(Node *tmpl, Node *template_id,
                              Arena *arena, Node *tu, TmplRegistry *reg,
                              Type *member_owner,
                              Node ***extra_out, int *nextra) {
    if (!tmpl || tmpl->kind != ND_TEMPLATE_DECL) return NULL;

    Node *inner = tmpl->template_decl.decl;
    if (!inner) return NULL;

    int nparams = tmpl->template_decl.nparams;
    int nargs   = template_id->template_id.nargs;

    /* Outer template params count, for SubstMap capacity. */
    Node *outer_tmpl = NULL;
    int outer_nparams = 0;
    if (member_owner && member_owner->tag) {
        outer_tmpl = registry_find(reg,
            member_owner->tag->loc, member_owner->tag->len);
        if (outer_tmpl && outer_tmpl->kind == ND_TEMPLATE_DECL)
            outer_nparams = outer_tmpl->template_decl.nparams;
    }

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
    /* +1 capacity for the injected-class-name entry, +outer for the
     * enclosing class-template's params when this is a member of a
     * class template (N4659 §17.5.2 [temp.mem]/2). */
    SubstMap map = subst_map_new_with_registry(arena,
        (nparams > 0 ? nparams : 1) + 1 + outer_nparams, reg);

    /* Seed with outer-template param→arg bindings BEFORE the inner
     * head's bindings, so a body that references both T (outer) and
     * U (inner) gets both substituted. */
    if (outer_tmpl && member_owner->n_template_args > 0) {
        int n = outer_nparams;
        if (n > member_owner->n_template_args)
            n = member_owner->n_template_args;
        for (int i = 0; i < n; i++) {
            Node *p = outer_tmpl->template_decl.params[i];
            if (!p || !p->param.name) continue;
            subst_map_add(&map, p->param.name,
                          member_owner->template_args[i]);
        }
    }

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
             * user omitted it. Real-world shape:
             * hash_table<D, A=xcallocator> with usage
             * hash_table<X>: A defaults to xcallocator. The cloned
             * body's Allocator<value_type>::data_alloc(...) will be
             * rewritten to xcallocator<value_type>::data_alloc by
             * clone.c's ND_QUALIFIED handler, so the call mangles to
             * a symbol matching the actual definition. N4659 §17.2/3
             * [temp.param] + §17.7.1 [temp.inst]. */
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

            /* Variadic-template parameter pack — N4659 §17.5.3
             * [temp.variadic]. The pack consumes the trailing run of
             * template args; bind it as a type list so the cloner
             * can expand pack-expansion sites in the body. */
            if (param->param.is_pack) {
                int npack = (nargs > i) ? (nargs - i) : 0;
                if (npack == 1) {
                    /* Single-arg fallback: bind as non-pack so the
                     * cloner leaves the param name un-renamed. See
                     * matching note in deduce_template_args. */
                    Type *t = type_arg_from_node(
                        template_id->template_id.args[i]);
                    if (t) subst_map_add(&map, pname, t);
                    continue;
                }
                /* 0 or 2+ args → genuine pack binding. Bind even
                 * the empty case so the cloner expands `args...`
                 * sites to zero elements and the function param
                 * disappears from the signature. */
                Type **pack_list = npack > 0
                    ? arena_alloc(arena, sizeof(Type *) * npack) : NULL;
                int pi = 0;
                for (int j = i; j < nargs; j++) {
                    Type *t = type_arg_from_node(template_id->template_id.args[j]);
                    if (t) pack_list[pi++] = t;
                }
                subst_map_add_pack(&map, pname, pack_list, pi);
                continue;
            }

            Type *arg_ty = NULL;
            if (i < nargs)
                arg_ty = type_arg_from_node(template_id->template_id.args[i]);
            /* Fall back to type-param default if no explicit argument
             * — N4659 §17.1/8 [temp.param]. NTTP defaults live in
             * param.default_value and are handled in the else-if
             * branch below. */
            if (!arg_ty && param->param.default_type)
                arg_ty = subst_type(param->param.default_type, &map, arena);
            if (arg_ty) {
                subst_map_add(&map, pname, arg_ty);
                /* For NTTP literal args (TY_NTTP_VALUE), also bind the
                 * param→token mapping so clone.c's ND_IDENT handler
                 * can morph body references to the param into the
                 * corresponding literal node. Without this, the body's
                 * 'v' (in 'static const int trait = v;') stays as
                 * ND_IDENT after clone and emits as an unresolved
                 * identifier. */
                if (arg_ty->kind == TY_NTTP_VALUE && arg_ty->tag)
                    subst_map_add_tt(&map, pname, arg_ty->tag);
                continue;
            }
            /* Non-type template parameter — N4659 §17.1/4 [temp.param].
             * The arg is an expression. Two flavors are common:
             *   - ND_IDENT naming a function/object (function-pointer
             *     NTTP, e.g. an allocator-function defaulted on a
             *     container template).
             *   - Literal: ND_NUM, ND_BOOL_LIT, ND_CHAR, ND_NULLPTR
             *     (libstdc++ <type_traits> integral_constant pattern —
             *     'integral_constant<bool, true>' has V=true).
             * Both bind the param name to a single Token so clone.c's
             * ND_IDENT handler can substitute references in the cloned
             * body. For literals, clone.c morphs the cloned ND_IDENT
             * into the corresponding literal NodeKind. */
            if (i < nargs) {
                Node *a = template_id->template_id.args[i];
                Token *bound_tok = NULL;
                /* Static-const-int member (ND_IDENT or ND_QUALIFIED
                 * referring to one) — resolve to its initializer
                 * literal so the cloned body sees the value, not the
                 * unsubstituted ident. Tried first because for
                 * ND_IDENT the legacy fallback would bind to the
                 * ident's own name, which works only for the
                 * function-pointer NTTP shape, not for value NTTPs. */
                bound_tok = nttp_ident_to_literal_tok(tu, a);
                if (!bound_tok && a) {
                    switch (a->kind) {
                    case ND_IDENT:
                        bound_tok = a->ident.name;
                        break;
                    case ND_NUM:
                    case ND_FNUM:
                    case ND_BOOL_LIT:
                    case ND_NULLPTR:
                        bound_tok = a->tok;
                        break;
                    case ND_CHAR:
                        bound_tok = a->chr.tok;
                        break;
                    case ND_STR:
                        bound_tok = a->str.tok;
                        break;
                    default:
                        break;
                    }
                }
                if (bound_tok)
                    subst_map_add_tt(&map, pname, bound_tok);
            } else if (param->param.default_value) {
                /* NTTP default substitution — N4659 §17.7.1/8
                 * [temp.inst]: a template parameter default is
                 * substituted with the SubstMap built from the
                 * explicit args specified so far and evaluated at
                 * the template-id's instantiation point.
                 *
                 * Sea-front handles the type-trait shape directly:
                 * substitute each arg type via the current SubstMap,
                 * call eval_type_trait, synthesize a Token carrying
                 * the literal text "0" or "1" for the NTTP binding.
                 * Other expression shapes (literal, arithmetic, ...)
                 * fall back to binding the original default expr's
                 * token text — works for literal defaults like
                 * 'bool b = true' but not for more complex shapes.
                 * TODO(seafront#nttp-default-eval): full constant-
                 * evaluator for arbitrary NTTP defaults. */
                Node *def = param->param.default_value;
                Token *bound_tok = NULL;
                if (def->kind == ND_TYPE_TRAIT) {
                    Type *a0 = subst_type(def->type_trait.arg0, &map, arena);
                    Type *a1 = subst_type(def->type_trait.arg1, &map, arena);
                    int result = eval_type_trait(def->type_trait.name, a0, a1);
                    Token *t = arena_alloc(arena, sizeof(*t));
                    memset(t, 0, sizeof(*t));
                    t->kind = TK_NUM;
                    t->loc  = result ? "1" : "0";
                    t->len  = 1;
                    bound_tok = t;
                } else if (def->kind == ND_NUM || def->kind == ND_FNUM ||
                           def->kind == ND_BOOL_LIT || def->kind == ND_NULLPTR) {
                    bound_tok = def->tok;
                } else if (def->kind == ND_IDENT) {
                    bound_tok = def->ident.name;
                }
                if (bound_tok)
                    subst_map_add_tt(&map, pname, bound_tok);
            }
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
        /* Build template_args. Filled to max(nargs, nparams) so default
         * positions get their values from `map`. For each NTTP arg we
         * also stash the parameter's declared type onto the resulting
         * Type so the mangler emits Itanium L<type><value>E (or the
         * human equivalent) from structured data, not text-matching. */
        build_inst_template_args(inst_ty, tmpl, template_id,
                                  &map, outer_nparams, arena, tu);
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
         * can resolve member references inside method bodies. The
         * enclosing scope is inherited from the source template's
         * own class_region (the namespace it was declared in) — so
         * unqualified lookups in cloned method bodies reach the
         * same global helpers as the original. */
        DeclarativeRegion *src_enclosing = NULL;
        if (inner->class_def.ty && inner->class_def.ty->class_region)
            src_enclosing = inner->class_def.ty->class_region->enclosing;
        DeclarativeRegion *cr = region_build_class(cloned, inst_ty,
                                                   src_enclosing, arena);
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
        bool any_user_ctor = false;
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
                if (m->func.is_constructor) {
                    any_user_ctor = true;
                    if (m->func.nparams == 0)
                        inst_ty->has_default_ctor = true;
                }
                if (m->func.is_virtual)
                    inst_ty->has_virtual_methods = true;
            } else if (m->kind == ND_VAR_DECL) {
                if (m->var_decl.is_destructor)
                    inst_ty->has_dtor = true;
                if (m->var_decl.is_constructor) {
                    any_user_ctor = true;
                    if (m->var_decl.ty && m->var_decl.ty->kind == TY_FUNC &&
                        m->var_decl.ty->nparams == 0)
                        inst_ty->has_default_ctor = true;
                }
                if (m->var_decl.is_virtual)
                    inst_ty->has_virtual_methods = true;
                /* NSDMI — N4659 §12.6.2/9 [class.base.init]/9: a
                 * default-member-initializer on a non-static, non-
                 * function member triggers default-ctor synthesis.
                 * Mirrors the same condition in parse/type.c so a
                 * template instantiation of 'struct C { T m = t; };'
                 * gets its synth default ctor (which assigns m=t).
                 * Both NSDMI forms count: `T m = expr;` sets init,
                 * `T m{args};` / `T m(args);` populate ctor_args via
                 * has_ctor_init. Pattern: g++.dg/cpp0x/nsdmi1.C
                 * `C<int,3>`. */
                if (!any_user_ctor &&
                    m->var_decl.ty &&
                    m->var_decl.ty->kind != TY_FUNC &&
                    !(m->var_decl.storage_flags & DECL_STATIC) &&
                    (m->var_decl.init || m->var_decl.has_ctor_init)) {
                    inst_ty->has_default_ctor = true;
                }
            }
        }
        /* Polymorphic-no-user-ctor: synthesise a default ctor so the
         * vptr gets installed at construction. N4659 §15.1/4
         * [class.ctor] — the implicit default ctor is defined as
         * defaulted when no user ctor is declared. Sea-front's vptr
         * install lives in the ctor wrapper; without a synthesised
         * default ctor the vtable instance stays dormant and
         * `TPL<int> i;` followed by `i.virtual_method()` jumps
         * through an uninitialised vptr. Pattern:
         * g++.dg/template/qual2.C — TPL<int> inherits B's virtuals,
         * overrides `activate`, and the test calls `i.activate()`. */
        if (!any_user_ctor && inst_ty->has_virtual_methods)
            inst_ty->has_default_ctor = true;
        /* A class with non-trivial members (transitive has_dtor /
         * has_default_ctor through bases) also needs synthesised
         * ones — mirror type.c's same rule for templates that
         * inherit from such bases. */
        if (cr) {
            for (int bi = 0; bi < cr->nbases; bi++) {
                Type *bt = cr->bases[bi] ? cr->bases[bi]->owner_type : NULL;
                if (!bt) continue;
                if (bt->has_dtor) inst_ty->has_dtor = true;
                if (bt->has_default_ctor && !any_user_ctor)
                    inst_ty->has_default_ctor = true;
                if (bt->has_virtual_methods)
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
                Vec ool = vec_new(arena);
                find_ool_methods(tu, orig_class_type, &ool);
                if (ool.len > 0) {
                    *extra_out = arena_alloc(arena, ool.len * sizeof(Node *));
                    *nextra = 0;
                    for (int k = 0; k < ool.len; k++) {
                        Node *method_tmpl = (Node *)ool.data[k];
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
         * key. Real-world shape: two `vec_alloc` templates,
         *   template<T,A> vec_alloc(vec<T,A,vl_embed>*&, unsigned)
         *   template<T>   vec_alloc(vec<T>*&, unsigned)
         * both mangle to `vec_alloc_t_<T>_te_` without a param
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

        /* Stash a back-pointer to the source template_decl and the
         * deduced template args on the cloned func. Used by
         * codegen's __PRETTY_FUNCTION__ emit to reconstruct the
         * source-level signature (`foo(T, typename T::type)`) and
         * the `[with T = x; ...]` substitution list. */
        cloned->func.source_template = tmpl;
        cloned->func.n_template_args = n;
        if (n > 0) {
            cloned->func.template_args =
                arena_alloc(arena, n * sizeof(Type *));
            for (int i = 0; i < n; i++)
                cloned->func.template_args[i] =
                    type_arg_from_node(template_id->template_id.args[i]);
        }

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
         * Real-world shape: `grow_helper(vec, n)` where the
         * `vec<...> *&v` param needs `&vec` at the call site. */
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

/* Rewrite a member class template's cloned tag to scope it under the
 * enclosing specialization. N4659 §17.5.2 [temp.mem]/2 — a member
 * class template specialization is a member of a particular outer
 * specialization. Itanium C++ ABI §5.1.5 encodes this nesting in the
 * mangled name. Our human encoding produces:
 *   OuterTag_t_<outer_args>_te___InnerTag
 * which mangle_class_tag then suffixes with _t_<inner_args>_te_ for
 * the inner template args carried on inst_ty.
 *
 * Both inst_ty (the cloned struct's own Type) and usage_type (the
 * call-site var's TY_STRUCT) must share the same rewritten tag so
 * the struct definition and method-call mangling agree. */
static Token *make_scoped_member_tag(Type *owner, Token *inner_tag,
                                      Arena *arena) {
    if (!owner || !owner->tag || !inner_tag) return inner_tag;
    int bufsize = 256;
    char *buf = arena_alloc(arena, bufsize);
    int pos = 0;
    int n = owner->tag->len;
    if (pos + n < bufsize) {
        memcpy(buf + pos, owner->tag->loc, n);
        pos += n;
    }
    if (owner->n_template_args > 0 && pos + 3 < bufsize) {
        memcpy(buf + pos, "_t_", 3);
        pos += 3;
        for (int i = 0; i < owner->n_template_args; i++) {
            if (i > 0 && pos < bufsize - 1) buf[pos++] = '_';
            pos = mangle_type_to_buf(owner->template_args[i],
                                      buf, pos, bufsize);
        }
        if (pos + 4 < bufsize) {
            memcpy(buf + pos, "_te_", 4);
            pos += 4;
        }
    }
    if (pos + 2 < bufsize) {
        memcpy(buf + pos, "__", 2);
        pos += 2;
    }
    int in = inner_tag->len;
    if (pos + in < bufsize) {
        memcpy(buf + pos, inner_tag->loc, in);
        pos += in;
    }
    Token *t = arena_alloc(arena, sizeof(Token));
    *t = *inner_tag;
    t->loc = buf;
    t->len = pos;
    t->kind = TK_IDENT;
    return t;
}

/* Forward declarations for post-instantiation type patching */
static void patch_all_types(Node *tu, DedupSet *ds, Arena *arena);

/* ------------------------------------------------------------------ */
/* Main entry point                                                    */
/* ------------------------------------------------------------------ */

/* Public lookup: clone.c calls this from subst_type via the registry
 * field carried on SubstMap. Walks the registry to find a class
 * template by name for dependent member-typedef resolution
 * ('typename T::value_type'). */
Node *registry_lookup_class_template(TmplRegistry *reg,
                                     const char *name, int name_len) {
    if (!reg) return NULL;
    return registry_find(reg, name, name_len);
}

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

    /* All instantiated decls across all iterations — arena-backed
     * Vec so there's no fixed cap (the previous MAX_INST=32K array
     * had to be bumped each time a new instantiation pattern leaked
     * extra nodes per template, e.g. the closure+fn pair from
     * lambda-in-template). inst_push pushes here. */
    Vec all_instantiated = vec_new(arena);

    /* Worklist iteration: each pass walks ONLY entries new since
     * the previous pass. The previous design re-walked all of
     * tu->tu.decls every iteration (quadratic in the number of
     * templates) and merged new instantiations into tu->tu.decls
     * mid-loop (the merge that 2a7fca4 fixed for cap-overflow
     * corruption). New shape:
     *
     *   - Walk only tu->tu.decls[n_walked_decls..ndecls)
     *   - AND walk all_instantiated[n_walked_inst..total_inst)
     *   - Bump indices to current sizes
     *   - If no new requests: terminate
     *   - Single merge with tu->tu.decls happens once after the
     *     worklist closes (post-loop, below)
     *
     * Each cloned body is walked exactly once. Termination is
     * natural: the (template, args) memoization in dedup ensures
     * a finite number of distinct instantiations; the worklist
     * shrinks to empty.
     *
     * This isn't full recursion (the user wanted "concrete work,
     * not loops"), but it IS the linear worklist algorithm —
     * equivalent in space/time to recursion, smaller scope of
     * structural change. See docs/instantiation-policy.md for
     * the deferred fully-recursive design. */
    int n_walked_decls = 0;
    int n_walked_inst = 0;
    while (1) {
    /* Phase 2: collect instantiation requests from current TU */
    InstCollector col = {0};
    col.arena = arena;
    col.reg = &reg;
    for (int i = n_walked_decls; i < tu->tu.ndecls; i++)
        collect_from_node(&col, tu->tu.decls[i]);
    for (int i = n_walked_inst; i < all_instantiated.len; i++)
        collect_from_node(&col, ((Node*)all_instantiated.data[i]));
    n_walked_decls = tu->tu.ndecls;
    n_walked_inst  = all_instantiated.len;
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
                       ? mr->class_tid->template_id.nargs
                       : (mr->enclosing_class
                          ? mr->enclosing_class->n_template_args : 0);
        int cap = np + outer_np;
        if (cap < 1) cap = 1;
        SubstMap deduced = subst_map_new_with_registry(arena, cap, &reg);

        /* Seed outer class-template param→arg bindings BEFORE
         * function-arg deduction. N4659 §17.5.2/2 [temp.mem]: when a
         * class template's member is itself a template, its body can
         * reference both the outer class's parameters and the inner
         * member-template's parameters. Without this seed, an outer
         * param appearing in the member's body (but not in its
         * function-parameter types) leaks through cloning as
         * TY_DEPENDENT.
         *
         * The bindings come from either the qualified-call shape
         * (mr->class_tid carries the explicit class-template-id) or
         * the unqualified-sibling shape (mr->enclosing_class carries
         * the substituted class type with template_args). */
        Type *owner = mr->entry->owner_class;
        Node *outer_tmpl = NULL;
        if (owner && owner->tag)
            outer_tmpl = registry_find(&reg, owner->tag->loc, owner->tag->len);
        if (outer_tmpl && outer_tmpl->kind == ND_TEMPLATE_DECL) {
            int onp = outer_tmpl->template_decl.nparams;
            if (mr->class_tid && mr->class_tid->kind == ND_TEMPLATE_ID) {
                int xna = mr->class_tid->template_id.nargs;
                int n = onp < xna ? onp : xna;
                for (int i = 0; i < n; i++) {
                    Node *p = outer_tmpl->template_decl.params[i];
                    if (!p || !p->param.name) continue;
                    Type *cta = type_arg_from_node(
                        mr->class_tid->template_id.args[i]);
                    if (cta) subst_map_add(&deduced, p->param.name, cta);
                }
            } else if (mr->enclosing_class &&
                       mr->enclosing_class->n_template_args > 0) {
                int xna = mr->enclosing_class->n_template_args;
                int n = onp < xna ? onp : xna;
                for (int i = 0; i < n; i++) {
                    Node *p = outer_tmpl->template_decl.params[i];
                    if (!p || !p->param.name) continue;
                    subst_map_add(&deduced, p->param.name,
                                  mr->enclosing_class->template_args[i]);
                }
            }
        }

        if (!deduce_template_args(inner, mr->arg_types, mr->nargs, &deduced))
            continue;

        /* N4659 §17.1/4 [temp.param] + §17.8.2/1 [temp.deduct]: when
         * a member template is called with explicit args
         * '...m<X, &foo>(arg)' the function-arg deduction above only
         * binds those template params that appear in the function's
         * parameter types. Non-type template parameters (NTTPs)
         * passed explicitly are NOT covered by deduction — bind them
         * here from the call-site's explicit template-id args. The
         * binding piggybacks on the TT-param slot (subst_map_add_tt):
         * both shapes are "param-name → Token" rewrites that
         * clone.c's ND_IDENT case applies during cloning.
         *
         * NTTP params are recognised by param.ty != NULL (the type
         * of the constant — int, function pointer, etc.). Type-params
         * have ty == NULL, so they're skipped. */
        {
            Node *cb = mr->call_node ? mr->call_node->call.callee : NULL;
            /* The explicit template-id may sit at the call's callee
             * (unqualified call shape: 'name<args>(...)' parses as
             * ND_CALL(callee=ND_TEMPLATE_ID, ...)) or, for a member
             * call ('obj.name<args>(...)') the callee is ND_MEMBER
             * carrying member.template_id with the parsed args. */
            Node *xtid = NULL;
            if (cb) {
                if (cb->kind == ND_TEMPLATE_ID)
                    xtid = cb;
                else if (cb->kind == ND_MEMBER && cb->member.template_id)
                    xtid = cb->member.template_id;
            }
            if (xtid) {
                int xna = xtid->template_id.nargs;
                for (int i = 0; i < np && i < xna; i++) {
                    Node *param = tmpl->template_decl.params[i];
                    if (!param || param->kind != ND_PARAM) continue;
                    if (!param->param.ty) continue;  /* type-param, not NTTP */
                    if (!param->param.name) continue;
                    Node *a = xtid->template_id.args[i];
                    if (a && a->kind == ND_IDENT && a->ident.name)
                        subst_map_add_tt(&deduced, param->param.name, a->ident.name);
                }
            }
        }

        /* Dedup key: class + NUL + class-template-args (if any) +
         * NUL + member + NUL + member-template deduced types.
         *
         * Including the class-template args is required when a
         * sibling member template inside a class template depends
         * on the class's parameters but those don't appear in the
         * member-template's own deduced args (e.g. helper<T>'s
         * member m<U>() — distinct T values must produce distinct
         * symbols even when called with the same U).
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
        /* Class-template args from the call-site lead_tid OR (for
         * unqualified sibling calls) from the enclosing instantiated
         * class. N4659 §17.7.1/2 [temp.inst]: each distinct
         * specialization is a distinct entity. Both sources must
         * enter the dedup key so requests across different outer
         * instantiations (e.g. Outer<int>::m vs Outer<float>::m via
         * sibling calls in their respective bodies) don't collide. */
        if (mr->class_tid && mr->class_tid->kind == ND_TEMPLATE_ID) {
            int ctna = mr->class_tid->template_id.nargs;
            for (int i = 0; i < ctna; i++) {
                Type *cta = type_arg_from_node(
                    mr->class_tid->template_id.args[i]);
                pos = type_to_key(cta, key, pos, MAX_DEDUP_KEY);
                key[pos++] = '\0';
            }
        } else if (mr->enclosing_class &&
                   mr->enclosing_class->n_template_args > 0) {
            int ctna = mr->enclosing_class->n_template_args;
            for (int i = 0; i < ctna; i++) {
                pos = type_to_key(mr->enclosing_class->template_args[i],
                                  key, pos, MAX_DEDUP_KEY);
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
        {
            /* Member-template dedup hit: the (class, member, args)
             * already produced a clone earlier this round. The call
             * site still needs callee->resolved_type set to the
             * substituted TY_FUNC so codegen mangles its param
             * suffix with concrete types — without this propagation,
             * the second-and-later call site falls back to the
             * source template-decl's TY_DEPENDENT params and emits
             * a bogus 'name_p_Argument_pe_' symbol that doesn't
             * match the unique definition's
             * 'name_p_<concrete>_pe_'. Real-world shape: multiple
             * identical 'container.traverse<T*, ...>(&info)' calls
             * sharing one instantiated specialization.
             * N4659 §17.7.1 [temp.inst]: each unique
             * specialization is one entity; identical (template,
             * args) calls all reach it. */
            Type *existing = dedup_find(&ds, key, pos);
            if (existing) {
                if (mr->call_node && mr->call_node->call.callee &&
                    existing->kind == TY_FUNC && existing->params)
                    mr->call_node->call.callee->resolved_type = existing;
                continue;
            }
        }

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
         * generate it. Real-world shape:
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
        } else if (mr->enclosing_class &&
                   mr->enclosing_class->n_template_args > 0) {
            /* Unqualified sibling call inside a cloned class-template
             * member body — mangle with the SAME outer args as the
             * calling method. N4659 §6.4.1/13 [basic.lookup.unqual]
             * + §17.7.1/2,9 [temp.inst] resolve the sibling into the
             * enclosing specialization; Itanium C++ ABI §5.1.5
             * requires those outer args in the mangled name.
             * Without this, the def mangles with the bare source
             * class tag, leaves the function with 'struct sf__Outer
             * *this' (forward-decl only), and any member access
             * reads garbage. */
            cloned->func.class_type = mr->enclosing_class;
        }

        /* N4659 §16.3 [over.match]: build TY_FUNC from cloned params
         * and set as resolved_type on the call-site callee so the
         * param suffix matches between definition and call. Store
         * the same TY_FUNC as the dedup value so subsequent same-
         * (class, member, args) calls can wire it onto their
         * callees too (see dedup-hit branch above). */
        Type *ft = func_type_from_func_def(arena, cloned);
        if (mr->call_node && mr->call_node->call.callee) {
            Node *cb = mr->call_node->call.callee;
            cb->resolved_type = ft;
            /* For unqualified explicit-template-args sibling calls,
             * reduce the callee from ND_TEMPLATE_ID to
             * ND_IDENT(implicit_this) so codegen lowers via the
             * standard class-method dispatch (mangling as
             * sf__<class>_..._te___<member>_*). Without this, the
             * default ND_TEMPLATE_ID emit prints the bare name and
             * the link breaks. The ND_IDENT path is what the
             * unqualified-no-template-args sibling case already
             * takes. */
            if (cb->kind == ND_TEMPLATE_ID &&
                cb->template_id.name &&
                mr->enclosing_class) {
                Token *bare = cb->template_id.name;
                cb->kind = ND_IDENT;
                cb->ident.name = bare;
                cb->ident.implicit_this = true;
                cb->ident.resolved_decl = NULL;
                cb->ident.overload_set = NULL;
                cb->ident.n_overloads = 0;
            }
        }

        /* Register in dedup set carrying the substituted TY_FUNC. */
        dedup_add(&ds, key, pos, ft);

        /* Wire a prototype scope so phase-2 sema can resolve names
         * (parameters, sibling class members, free helpers in the
         * declaring namespace) inside the cloned body.
         *
         * The enclosing must reach through the owning class region
         * first, then up to the namespace — anything else makes
         * unqualified sibling-member calls miss. With the namespace
         * as direct enclosing, an unqualified call to a sibling
         * static member skips past the class and emits as a bare
         * unmangled name. */
        if (cloned->func.body && !cloned->func.param_scope) {
            DeclarativeRegion *enc = NULL;
            if (mr->entry->owner_class &&
                mr->entry->owner_class->class_region)
                enc = mr->entry->owner_class->class_region;
            else if (tu)
                enc = tu->tu.global_scope;
            cloned->func.param_scope = region_build_prototype(
                cloned, enc, arena);
        }

        collect_inner_lambdas(cloned, &all_instantiated, &ninst_this_round);
        inst_push(&all_instantiated, &ninst_this_round,
                  cloned, "member-template instantiation");
    }

    /* Phase 3b: class + function template instantiation */
    for (InstRequest *req = col.head; req; req = req->next) {
        /* Build a temporary SubstMap to compute the dedup key
         * (includes defaults). This is rebuilt inside instantiate_one
         * if we proceed — minor redundancy for cleaner code. */
        Node *tmpl = req->tmpl_def;
        int np = tmpl->template_decl.nparams;
        int na = req->template_id->template_id.nargs;
        SubstMap tmp_map = subst_map_new_with_registry(arena,
            np > 0 ? np : 1, &reg);
        /* Track NTTP-default arg extensions: for each param i in
         * [na, np), nttp_extra_args[i - na] is the synthesized arg
         * node (ND_NUM) when the param is an NTTP with an evaluable
         * default — N4659 §17.7.1/8 [temp.inst]. NULL otherwise.
         * After the loop we splice these into req->template_id.args
         * so build_inst_template_args, the mangler, and dedup all
         * see the full arg list. */
        Node **nttp_extra_args = NULL;
        int nttp_extra_upto = na;  /* last index where extension is contiguous */
        if (np > na) {
            nttp_extra_args = arena_alloc(arena, (np - na) * sizeof(Node *));
            memset(nttp_extra_args, 0, (np - na) * sizeof(Node *));
        }
        for (int i = 0; i < np; i++) {
            Node *param = tmpl->template_decl.params[i];
            if (!param || !param->param.name) continue;
            Type *arg_ty = (i < na) ?
                type_arg_from_node(req->template_id->template_id.args[i]) :
                NULL;
            /* Type-param default — N4659 §17.1/8 [temp.param]. NTTP
             * defaults live in param.default_value (handled below). */
            if (!arg_ty && param->param.default_type)
                arg_ty = subst_type(param->param.default_type, &tmp_map, arena);
            if (arg_ty) {
                subst_map_add(&tmp_map, param->param.name, arg_ty);
                continue;
            }
            /* NTTP default evaluation (only when no explicit arg).
             * Currently handles the type-trait shape; other shapes
             * fall through and remain unbound (mangled as "unknown"
             * — TODO(seafront#nttp-default-eval)). */
            if (i >= na && param->param.default_value) {
                Node *def = param->param.default_value;
                if (def->kind == ND_TYPE_TRAIT) {
                    Type *a0 = subst_type(def->type_trait.arg0, &tmp_map, arena);
                    Type *a1 = subst_type(def->type_trait.arg1, &tmp_map, arena);
                    int result = eval_type_trait(def->type_trait.name, a0, a1);
                    Token *t = arena_alloc(arena, sizeof(*t));
                    memset(t, 0, sizeof(*t));
                    t->kind = TK_NUM;
                    t->loc  = result ? "1" : "0";
                    t->len  = 1;
                    /* Wrap in ND_VAR_DECL carrying a TY_NTTP_VALUE so
                     * every site that extracts the Type via the
                     * 'a->kind == ND_VAR_DECL ? a->var_decl.ty : NULL'
                     * shape (stub builders, mangler, dedup) sees it.
                     * The TY_NTTP_VALUE.tag points at the literal token
                     * so the mangler emits 'Li1E' / 'Li0E'. */
                    Type *nttp_ty = arena_alloc(arena, sizeof(Type));
                    memset(nttp_ty, 0, sizeof(Type));
                    nttp_ty->kind = TY_NTTP_VALUE;
                    nttp_ty->tag  = t;
                    if (param->param.ty)
                        nttp_ty->nttp_decl_type = param->param.ty;
                    Node *vd = arena_alloc(arena, sizeof(Node));
                    memset(vd, 0, sizeof(Node));
                    vd->kind = ND_VAR_DECL;
                    vd->var_decl.ty = nttp_ty;
                    if (i == nttp_extra_upto) {
                        nttp_extra_args[i - na] = vd;
                        nttp_extra_upto = i + 1;
                    }
                }
            }
        }
        /* Splice NTTP-default extensions into the template-id's args
         * (in place; same Node as the in-tree reference so the
         * dedup-mangled-name rewrite at the end of this branch reaches
         * every call site). */
        if (nttp_extra_upto > na) {
            int new_n = nttp_extra_upto;
            Node **new_args = arena_alloc(arena, new_n * sizeof(Node *));
            for (int i = 0; i < na; i++)
                new_args[i] = req->template_id->template_id.args[i];
            for (int i = na; i < new_n; i++)
                new_args[i] = nttp_extra_args[i - na];
            req->template_id->template_id.args = new_args;
            req->template_id->template_id.nargs = new_n;
            na = new_n;
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
         * may exclude fixed args from partial specializations).
         *
         * For FUNCTION templates we also include the ND_TEMPLATE_DECL
         * pointer in the key, so two same-named function templates
         * with the same template args but different function
         * signatures (e.g. `gt_pch_nx<T,A>(vec*)` and
         * `gt_pch_nx<T,A>(vec*, op, cookie)`) don't dedup-collide.
         * Class-template requests skip this — only one ND_TEMPLATE_
         * DECL exists per class-template name in scope, and including
         * the pointer would prevent later same-(name,args) class
         * requests from finding the existing instantiation. */
        char key[MAX_DEDUP_KEY];
        int key_len = 0;
        /* For member class templates, prefix the key with the owner's
         * tag and template args so two distinct enclosing
         * specializations (e.g. Outer<int>::Inner<int> vs
         * Outer<float>::Inner<int>) get separate clones. N4659
         * §17.5.2/2 [temp.mem]. */
        if (req->member_owner && req->member_owner->tag &&
            key_len + req->member_owner->tag->len < MAX_DEDUP_KEY) {
            memcpy(key + key_len, req->member_owner->tag->loc,
                   req->member_owner->tag->len);
            key_len += req->member_owner->tag->len;
            key[key_len++] = '\0';
            for (int i = 0; i < req->member_owner->n_template_args; i++) {
                key_len = type_to_key(req->member_owner->template_args[i],
                                       key, key_len, MAX_DEDUP_KEY);
                key[key_len++] = '\0';
            }
        }
        if (req->name && key_len + req->name->len < MAX_DEDUP_KEY) {
            memcpy(key + key_len, req->name->loc, req->name->len);
            key_len += req->name->len;
        }
        key[key_len++] = '\0';
        {
            Node *tmpl_inner_dk = req->tmpl_def
                ? req->tmpl_def->template_decl.decl : NULL;
            bool is_fn_tmpl = tmpl_inner_dk &&
                (tmpl_inner_dk->kind == ND_FUNC_DEF ||
                 tmpl_inner_dk->kind == ND_FUNC_DECL);
            if (is_fn_tmpl &&
                key_len + (int)sizeof(Node *) < MAX_DEDUP_KEY) {
                memcpy(key + key_len, &req->tmpl_def, sizeof(Node *));
                key_len += sizeof(Node *);
                key[key_len++] = '\0';
            }
        }
        /* Include all usage args (explicit + defaults from map). Use
         * the mangling-aware arg-to-type helper so literal NTTPs
         * differentiate the dedup key — without this, two
         * 'integral_constant<int,42>' and '<int,99>' instantiations
         * key identically and the second collapses onto the first. */
        int total_args = na > tmp_map.nentries ? na : tmp_map.nentries;
        for (int i = 0; i < total_args; i++) {
            Type *arg_ty = (i < na) ?
                template_arg_to_arg_type_resolved(
                    req->template_id->template_id.args[i], arena, tu) :
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
             * the collect path creates a request with usage_type=NULL,
             * but the rewrite below would mangle the callee as if it
             * were a function template — a name that doesn't match
             * the class's actual ctor mangling. Skip the rewrite when
             * the template's inner decl is a class. Pattern: a
             * template-id at expression position constructing a
             * temporary, e.g. 'return vec<T,A,L>();'. */
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
                                    arena, tu, &reg, req->member_owner,
                                    &extra_methods, &nextra);
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
                    /* Build class_region if not already present.
                     * Inherit the source template's enclosing scope so
                     * unqualified lookups in method bodies reach the
                     * declaring namespace's free helpers. */
                    if (!sty->class_region) {
                        DeclarativeRegion *src_enclosing = NULL;
                        Node *src = req->tmpl_def
                            ? req->tmpl_def->template_decl.decl : NULL;
                        if (src && src->class_def.ty &&
                            src->class_def.ty->class_region)
                            src_enclosing =
                                src->class_def.ty->class_region->enclosing;
                        sty->class_region = region_build_class(
                            inst, sty, src_enclosing, arena);
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
                                      arena, tu, &reg, req->member_owner,
                                      &extra_methods, &nextra);
        }
        /* Member class template: rewrite the cloned struct's tag to a
         * scoped name so the emitted symbol distinguishes
         * Outer<X>::Inner<int> from a top-level Inner<int>. Done before
         * sema_visit so methods inside the cloned struct mangle with
         * the scoped tag. N4659 §17.5.2 [temp.mem]/2. */
        if (inst && inst->kind == ND_CLASS_DEF && inst->class_def.ty &&
            req->member_owner) {
            Type *inst_ty = inst->class_def.ty;
            Token *new_tag = make_scoped_member_tag(req->member_owner,
                                                     inst_ty->tag, arena);
            inst_ty->tag = new_tag;
            inst->class_def.tag = new_tag;
            if (req->usage_type) req->usage_type->tag = new_tag;
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
            collect_inner_lambdas(inst, &all_instantiated, &ninst_this_round);
            inst_push(&all_instantiated, &ninst_this_round,
                      inst, "class/function template instantiation");
            /* For class instantiations: dedup_add unconditionally so
             * a subsequent request for the same (name, args) finds
             * the existing entry and short-circuits. The check must
             * not be gated on usage_type != NULL — a class-template
             * instantiation requested via a constructor call (ND_CALL
             * callee=ND_TEMPLATE_ID, which sets usage_type=NULL) still
             * needs a dedup entry so a later type-position request
             * for the same class hits the existing instantiation
             * rather than producing a duplicate ND_CLASS_DEF.
             * N4659 §17.4 [temp.type]: same template-id names one
             * type. */
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
                collect_inner_lambdas(em, &all_instantiated, &ninst_this_round);
                inst_push(&all_instantiated, &ninst_this_round,
                          em, "OOL extra method");
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

    /* Per-iteration merge with tu->tu.decls is GONE (was the source
     * of the cap-overflow corruption fixed in 2a7fca4). With the
     * worklist-style walk, we only walk new entries each pass —
     * the cloned bodies sit in all_instantiated[] and the next
     * iteration's collect_from_node walks them directly via
     * n_walked_inst, no merge needed during the loop. The single
     * merge happens once after the loop, below. */
    } /* end iteration loop */

    /* Single end-of-loop merge: insert ALL instantiations (across
     * every iteration) into tu->tu.decls. Same insert-position
     * logic as before — after the last source class def, before
     * function defs — but applied once with total_inst entries. */
    {
        int insert_pos = 0;
        for (int i = 0; i < tu->tu.ndecls; i++) {
            Node *d = tu->tu.decls[i];
            if (d && (d->kind == ND_CLASS_DEF || d->kind == ND_BLOCK))
                insert_pos = i + 1;
            if (d && d->kind == ND_TEMPLATE_DECL &&
                d->template_decl.nparams == 0 && d->template_decl.decl &&
                d->template_decl.decl->kind == ND_CLASS_DEF)
                insert_pos = i + 1;
        }
        int old_n = tu->tu.ndecls;
        int new_n = old_n + all_instantiated.len;
        if (all_instantiated.len > 0) {
            Node **new_decls = arena_alloc(arena, new_n * sizeof(Node *));
            int idx = 0;
            for (int i = 0; i < insert_pos && i < old_n; i++)
                new_decls[idx++] = tu->tu.decls[i];
            for (int i = 0; i < all_instantiated.len; i++)
                new_decls[idx++] = ((Node*)all_instantiated.data[i]);
            for (int i = insert_pos; i < old_n; i++)
                new_decls[idx++] = tu->tu.decls[i];
            tu->tu.decls  = new_decls;
            tu->tu.ndecls = new_n;
        }
    }

    /* Post-loop: resolve template base classes. During instantiation,
     * a derived template's base type (e.g. base_t<int>) may not have
     * been instantiated yet when the derived class was cloned. Now
     * that all instantiations are done, walk each instantiated class's
     * base_types and link any that now have class_region set. Also
     * check the dedup set for bases that were instantiated.
     *
     * ALSO: plain (non-template) classes that inherit from a template
     * instantiation (e.g. 'struct D : typed_helper<int>') have the
     * same problem — at parse time the base's class_region was NULL
     * because the template hadn't been instantiated. Walk top-level
     * plain ND_CLASS_DEFs too. N4659 §13.1 [class.derived]. */
    Node *all_class_defs[1024];
    int n_all_class_defs = 0;
    for (int i = 0; i < all_instantiated.len; i++) {
        Node *inst = ((Node*)all_instantiated.data[i]);
        if (inst && inst->kind == ND_CLASS_DEF &&
            n_all_class_defs < (int)(sizeof(all_class_defs)/sizeof(all_class_defs[0])))
            all_class_defs[n_all_class_defs++] = inst;
    }
    for (int i = 0; i < tu->tu.ndecls; i++) {
        Node *d = tu->tu.decls[i];
        if (!d) continue;
        if (d->kind == ND_CLASS_DEF &&
            n_all_class_defs < (int)(sizeof(all_class_defs)/sizeof(all_class_defs[0]))) {
            all_class_defs[n_all_class_defs++] = d;
        } else if (d->kind == ND_TYPEDEF) {
            /* Pick up class defs hidden inside typedef wrappers:
             *   typedef struct pre_expr_d : typed_noop_remove<pre_expr_d>
             *   { ... } *pre_expr;
             * The class is on var_decl.ty (peeling ptr/array layers).
             * Without this, the post-instantiation base-region
             * patching skips the class and its inheritance link
             * from the template base never gets set — qualified
             * static calls end up mangled with the derived prefix
             * and fail to link. N4659 §13.1 [class.derived]. */
            Type *ty = d->var_decl.ty;
            while (ty && (ty->kind == TY_PTR || ty->kind == TY_ARRAY))
                ty = ty->base;
            if (ty && (ty->kind == TY_STRUCT || ty->kind == TY_UNION) &&
                ty->class_def &&
                n_all_class_defs < (int)(sizeof(all_class_defs)/sizeof(all_class_defs[0])))
                all_class_defs[n_all_class_defs++] = ty->class_def;
        }
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
    for (int i = 0; i < all_instantiated.len; i++) {
        Node *inst = ((Node*)all_instantiated.data[i]);
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
    for (int pass = 0; pass < all_instantiated.len; pass++) {
        bool changed = false;
        for (int i = 0; i < all_instantiated.len; i++) {
            Node *a = ((Node*)all_instantiated.data[i]);
            if (!a || a->kind != ND_CLASS_DEF || !a->class_def.ty) continue;
            /* Find any later class def that A depends on */
            for (int j = i + 1; j < all_instantiated.len; j++) {
                Node *b = ((Node*)all_instantiated.data[j]);
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
                if (mty->tag && bty->tag && tokens_equal(mty->tag, bty->tag))
                    a_needs_b = true;
            }
            /* Also check base types — base classes are embedded
             * as __sf_base members, so they must be defined first. */
            for (int bt = 0; bt < a->class_def.nbase_types && !a_needs_b; bt++) {
                Type *base = a->class_def.base_types[bt];
                if (!base || !base->tag) continue;
                if (bty->tag && tokens_equal(base->tag, bty->tag))
                    a_needs_b = true;
            }
            if (a_needs_b) {
                /* Move B to just before A by shifting elements */
                void *save = all_instantiated.data[j];
                for (int k = j; k > i; k--)
                    all_instantiated.data[k] = all_instantiated.data[k - 1];
                all_instantiated.data[i] = save;
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
            for (int j = 0; j < all_instantiated.len; j++) {
                if (d == ((Node*)all_instantiated.data[j])) { is_inst = true; break; }
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
            for (int j = 0; j < all_instantiated.len; j++) {
                if (tu->tu.decls[i] == ((Node*)all_instantiated.data[j])) {
                    is_inst = true; break;
                }
            }
            if (!is_inst) user_n++;
        }
        int new_n = all_instantiated.len + user_n;
        Node **new_decls = arena_alloc(arena, new_n * sizeof(Node *));
        int idx = 0;
        /* User decls before insert point */
        for (int i = 0; i < old_n; i++) {
            if (idx == insert_pos) {
                /* Insert instantiations here */
                for (int j = 0; j < all_instantiated.len; j++)
                    new_decls[idx++] = ((Node*)all_instantiated.data[j]);
            }
            bool is_inst = false;
            for (int j = 0; j < all_instantiated.len; j++) {
                if (tu->tu.decls[i] == ((Node*)all_instantiated.data[j])) {
                    is_inst = true; break;
                }
            }
            if (!is_inst) new_decls[idx++] = tu->tu.decls[i];
        }
        /* If insert_pos >= user_n, append at end */
        if (idx == new_n - all_instantiated.len) {
            for (int j = 0; j < all_instantiated.len; j++)
                new_decls[idx++] = ((Node*)all_instantiated.data[j]);
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
 * Return the canonical Type* for `ty` per N4659 §17.4 [temp.type]/1
 * (template-id type equivalence: two template-ids name the same type
 * iff template name + argument list are equivalent).
 *
 * For TY_STRUCT/TY_UNION with a template_id_node, look up the
 * instantiated Type in the dedup set and return that pointer. Every
 * use-site of vec<int> resolves to the same Type object.
 *
 * For compound types (TY_PTR, TY_FUNC, ...), recurse into bases and
 * replace them in place; the compound wrapper itself stays per-site
 * but its inner template-id Type is canonicalised. Two separately-
 * parsed `vec<int>*` end up as two TY_PTR Types whose base is the
 * SAME canonical TY_STRUCT, which is enough for type identity at the
 * template-id level.
 *
 * If no canonical entry exists (e.g. the template-id failed to
 * instantiate) returns the original Type unchanged.
 */
static Type *canonicalize_type(Type *ty, DedupSet *ds, Arena *arena) {
    (void)arena;
    if (!ty) return NULL;
    switch (ty->kind) {
    case TY_PTR: case TY_REF: case TY_RVALREF: case TY_ARRAY:
        ty->base = canonicalize_type(ty->base, ds, arena);
        return ty;
    case TY_FUNC:
        ty->ret = canonicalize_type(ty->ret, ds, arena);
        for (int i = 0; i < ty->nparams; i++)
            ty->params[i] = canonicalize_type(ty->params[i], ds, arena);
        return ty;
    default: break;
    }
    if (ty->kind != TY_STRUCT && ty->kind != TY_UNION) return ty;
    if (!ty->template_id_node) return ty;
    Node *tid = ty->template_id_node;
    if (tid->kind != ND_TEMPLATE_ID || !tid->template_id.name) return ty;
    Token *tname = tid->template_id.name;
    char key[MAX_DEDUP_KEY];
    int pos = 0;
    if (tname && pos + tname->len < MAX_DEDUP_KEY) {
        memcpy(key, tname->loc, tname->len);
        pos = tname->len;
    }
    key[pos++] = '\0';
    for (int i = 0; i < tid->template_id.nargs; i++) {
        Node *arg = tid->template_id.args[i];
        /* For type args the parser wraps in ND_VAR_DECL; for NTTP
         * literal args it leaves the literal Node directly. Both shapes
         * must contribute to the dedup key — without the NTTP branch,
         * '<bool,true>' and '<bool,false>' produced the same key and
         * downstream pointer-replacement collapsed them, breaking
         * call-site mangling at every distinct NTTP value site. */
        Type *arg_ty = template_arg_to_arg_type(arg, arena);
        pos = type_to_key(arg_ty, key, pos, MAX_DEDUP_KEY);
        key[pos++] = '\0';
    }
    Type *canonical = dedup_find(ds, key, pos);
    return canonical ? canonical : ty;
}

static void canonicalize_region_decls(DeclarativeRegion *r,
                                       DedupSet *ds, Arena *arena) {
    if (!r) return;
    for (int i = 0; i < REGION_HASH_SIZE; i++)
        for (Declaration *d = r->buckets[i]; d; d = d->next)
            d->type = canonicalize_type(d->type, ds, arena);
}

static void patch_node_types(Node *n, DedupSet *ds, Arena *arena) {
    if (!n) return;
    /* Every Node has a sema-filled resolved_type; canonicalise it
     * so the type identity of N4659 §17.4 [temp.type]/1 holds across
     * every reference to a given template-id. */
    n->resolved_type = canonicalize_type(n->resolved_type, ds, arena);
    switch (n->kind) {
    case ND_VAR_DECL: case ND_TYPEDEF:
        n->var_decl.ty = canonicalize_type(n->var_decl.ty, ds, arena);
        patch_node_types(n->var_decl.init, ds, arena);
        break;
    case ND_PARAM:
        n->param.ty = canonicalize_type(n->param.ty, ds, arena);
        break;
    case ND_FUNC_DEF: case ND_FUNC_DECL:
        n->func.ret_ty = canonicalize_type(n->func.ret_ty, ds, arena);
        canonicalize_region_decls(n->func.param_scope, ds, arena);
        for (int i = 0; i < n->func.nparams; i++)
            patch_node_types(n->func.params[i], ds, arena);
        patch_node_types(n->func.body, ds, arena);
        break;
    case ND_CLASS_DEF:
        if (n->class_def.ty)
            canonicalize_region_decls(n->class_def.ty->class_region, ds, arena);
        for (int i = 0; i < n->class_def.nmembers; i++)
            patch_node_types(n->class_def.members[i], ds, arena);
        break;
    case ND_BLOCK:
        canonicalize_region_decls(n->block.scope, ds, arena);
        for (int i = 0; i < n->block.nstmts; i++)
            patch_node_types(n->block.stmts[i], ds, arena);
        break;
    case ND_CAST:
        n->cast.ty = canonicalize_type(n->cast.ty, ds, arena);
        patch_node_types(n->cast.operand, ds, arena);
        break;
    case ND_SIZEOF:
        n->sizeof_.ty = canonicalize_type(n->sizeof_.ty, ds, arena);
        patch_node_types(n->sizeof_.expr, ds, arena);
        break;
    case ND_ALIGNOF:
        n->alignof_.ty = canonicalize_type(n->alignof_.ty, ds, arena);
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
