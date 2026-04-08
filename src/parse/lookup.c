/*
 * lookup.c — Name lookup and declarative region management.
 *
 * Implements the core name lookup mechanism of C++17 (N4659 §6.3-§6.4),
 * unchanged in C++20 (N4861 §6.4-§6.5) and C++23 (N4950 §6.4-§6.5).
 *
 * N4659 §6.4/1 [basic.lookup]:
 *   "The name lookup rules apply uniformly to all names (including
 *    typedef-names (10.1.3), namespace-names (10.3), and class-names
 *    (12.1)) wherever the grammar allows such names in the context
 *    discussed by a particular rule."
 *
 * Name lookup associates the use of a name with a Declaration that
 * was introduced into a DeclarativeRegion. The parser's disambiguation
 * rules then inspect the EntityKind of that Declaration.
 */

#include "parse.h"

/* ------------------------------------------------------------------ */
/* Hash function — FNV-1a                                             */
/* ------------------------------------------------------------------ */

static uint32_t hash_name(const char *name, int len) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) {
        h ^= (uint8_t)name[i];
        h *= 16777619u;
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* Declarative region management — N4659 §6.3 [basic.scope]           */
/* ------------------------------------------------------------------ */

/*
 * Push a new declarative region as a child of the current region.
 * Arena-allocated — no cleanup needed on pop.
 *
 * N4659 §6.3/1: "Declarative regions can nest."
 */
void region_push(Parser *p, RegionKind kind, Token *name) {
    DeclarativeRegion *r = arena_alloc(p->arena, sizeof(DeclarativeRegion));
    r->kind = kind;
    r->enclosing = p->region;
    r->name = name;
    /* buckets, using_regions are zero-initialized by arena_alloc */
    p->region = r;
}

/*
 * Pop the current declarative region. Restores the enclosing
 * pointer; the region struct itself remains in the arena.
 *
 * The "name is local to its region" property is general — N4659
 * §6.3 [basic.scope] frames it for declarative regions of all
 * kinds, with §6.3.3 [basic.scope.block] / §6.3.4
 * [basic.scope.proto] / §6.3.6 [basic.scope.namespace] etc.
 * specialising it per region.
 */
void region_pop(Parser *p) {
    p->region = p->region->enclosing;
}

/* ------------------------------------------------------------------ */
/* Point of declaration — N4659 §6.3.2 [basic.scope.pdecl]            */
/* ------------------------------------------------------------------ */

/*
 * Introduce a name into the current declarative region.
 *
 * N4659 §6.3.2/1: "The point of declaration for a name is immediately
 * after its complete declarator and before its initializer (if any)."
 *
 * No-op when p->tentative is true — speculative parsing must not
 * pollute the declarative regions. If the tentative parse succeeds,
 * the declaration is re-parsed in committed mode.
 *
 * No duplicate checking: if the same name is declared twice in the
 * same region, the newer entry shadows the older in the hash chain.
 * For a bootstrap tool processing valid C++ source, this is correct
 * — the source has already been compiled by GCC/Clang.
 */
Declaration *region_declare_in(Parser *p, DeclarativeRegion *r,
                               const char *name, int name_len,
                               EntityKind entity, Type *type) {
    if (p->tentative)
        return NULL;

    uint32_t idx = hash_name(name, name_len) % REGION_HASH_SIZE;

    Declaration *decl = arena_alloc(p->arena, sizeof(Declaration));
    decl->name = name;
    decl->name_len = name_len;
    decl->entity = entity;
    decl->type = type;
    decl->ns_region = NULL;
    decl->home = r;
    decl->next = r->buckets[idx];
    r->buckets[idx] = decl;
    return decl;
}

Declaration *region_declare(Parser *p, const char *name, int name_len,
                            EntityKind entity, Type *type) {
    return region_declare_in(p, p->region, name, name_len, entity, type);
}

/* ------------------------------------------------------------------ */
/* Unqualified name lookup — N4659 §6.4.1 [basic.lookup.unqual]       */
/*                           N4861 §6.5.1 (C++20)                     */
/*                           N4950 §6.5.1 (C++23)                     */
/* ------------------------------------------------------------------ */

/*
 * Look up a name in a SINGLE region (no outward walk). Searches the
 * region's hash buckets first, then — for class scopes — the base-
 * class regions per N4659 §6.4.2 [class.member.lookup].
 *
 * The outward walk through enclosing scopes is in
 * lookup_unqualified_from below.
 *
 * --- SHORTCUT (our implementation, not the standard's algorithm):
 * The base-class search is depth-first, first-match-wins. The
 * standard's §6.4.2/3-7 algorithm would diagnose certain
 * ambiguous-base lookups as ill-formed (when the same name is
 * found via two non-overlapping paths through distinct subobjects).
 * We silently take the first match. TODO(seafront#mi-ambig): when
 * sema needs accurate diagnostics, replace with the
 * §6.4.2 algorithm proper.
 */
static Declaration *lookup_in_region(DeclarativeRegion *r,
                                     const char *name, int name_len) {
    uint32_t idx = hash_name(name, name_len) % REGION_HASH_SIZE;
    for (Declaration *d = r->buckets[idx]; d; d = d->next) {
        if (d->name_len == name_len && memcmp(d->name, name, name_len) == 0)
            return d;
    }
    if (r->kind == REGION_CLASS) {
        for (int i = 0; i < r->nbases; i++) {
            Declaration *d = lookup_in_region(r->bases[i], name, name_len);
            if (d) return d;
        }
    }
    return NULL;
}

/*
 * lookup_unqualified_from — outward walk through the chain of
 * enclosing declarative regions starting at 'start'.
 *
 * N4659 §6.4.1/1 [basic.lookup.unqual]: "the scopes are searched
 * for a declaration in the order listed in each of the respective
 * categories; name lookup ends as soon as a declaration is found
 * for the name."
 *
 * Each step searches the region itself (lookup_in_region, which
 * also walks base classes for a class scope) and then any
 * using-directive regions attached to that step.
 *
 * Takes a starting region by value so callers without a Parser
 * handle (sema, future tooling) can use the same walker.
 */
Declaration *lookup_unqualified_from(DeclarativeRegion *start,
                                     const char *name, int name_len) {
    for (DeclarativeRegion *r = start; r; r = r->enclosing) {
        Declaration *d = lookup_in_region(r, name, name_len);
        if (d)
            return d;
        for (int i = 0; i < r->nusing; i++) {
            d = lookup_in_region(r->using_regions[i], name, name_len);
            if (d)
                return d;
        }
    }
    return NULL;
}

Declaration *lookup_unqualified(Parser *p, const char *name, int name_len) {
    return lookup_unqualified_from(p->region, name, name_len);
}

/*
 * Look up a name but only match a specific entity kind.
 *
 * Needed for elaborated-type-specifier (§10.1.7.3 [dcl.type.elab]):
 * 'struct Foo' must find the ENTITY_TAG declaration even if a variable
 * named 'Foo' hides the class name in the same scope.
 *
 * N4659 §6.3.10/2 [basic.scope.hiding]: "A class name or enumeration
 * name can be hidden by the name of a variable, data member, function,
 * or enumerator declared in the same scope."
 */
static Declaration *lookup_kind_in_region(DeclarativeRegion *r,
                                          const char *name, int name_len,
                                          EntityKind kind) {
    uint32_t idx = hash_name(name, name_len) % REGION_HASH_SIZE;
    for (Declaration *d = r->buckets[idx]; d; d = d->next) {
        if (d->name_len == name_len &&
            memcmp(d->name, name, name_len) == 0 &&
            d->entity == kind)
            return d;
    }
    /* Walk base classes — see lookup_in_region. */
    if (r->kind == REGION_CLASS) {
        for (int i = 0; i < r->nbases; i++) {
            Declaration *d = lookup_kind_in_region(r->bases[i], name, name_len, kind);
            if (d) return d;
        }
    }
    return NULL;
}

/*
 * lookup_unqualified_kind — like lookup_unqualified, but only
 * returns a Declaration whose entity kind matches 'kind'. Lets
 * callers ask "is this an ENTITY_TYPE in scope" without being
 * confused by a same-named ENTITY_VARIABLE that hides it (per the
 * §6.3.10 hiding rule, which is asymmetric: variables hide
 * type-names but not vice versa).
 *
 * Walks the same enclosing-scope chain + using-directive list as
 * lookup_unqualified_from, but uses lookup_kind_in_region at each
 * step to filter by kind.
 */
Declaration *lookup_unqualified_kind(Parser *p, const char *name,
                                     int name_len, EntityKind kind) {
    for (DeclarativeRegion *r = p->region; r; r = r->enclosing) {
        Declaration *d = lookup_kind_in_region(r, name, name_len, kind);
        if (d)
            return d;
        /* Also search using-directive regions */
        for (int i = 0; i < r->nusing; i++) {
            d = lookup_kind_in_region(r->using_regions[i], name, name_len, kind);
            if (d)
                return d;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Disambiguation oracles                                             */
/*                                                                    */
/* These are the "two semantic oracles" identified in                 */
/* doc/disambiguation-rules.md. They are convenience wrappers around  */
/* unqualified name lookup that inspect the EntityKind of the result. */
/* ------------------------------------------------------------------ */

/*
 * Is this identifier a type-name?
 *
 * N4659 §10.1.7.1 [dcl.type.simple]:
 *   type-name: class-name | enum-name | typedef-name
 *
 * Used by disambiguation rules:
 *   §9.8 [stmt.ambig] — statement vs declaration
 *   §11.2 [dcl.ambig.res] — declarator ambiguities
 */
bool lookup_is_type_name(Parser *p, Token *tok) {
    if (tok->kind != TK_IDENT)
        return false;
    /* Kind-specific lookup: the same name may be registered as both
     * ENTITY_TEMPLATE and ENTITY_TYPE (e.g. a class template — its
     * tag/injected name AND its template-ness are both recorded).
     * Plain lookup_unqualified would return whichever was inserted
     * last; we want to know if the name names a type at all. */
    return lookup_unqualified_kind(p, tok->loc, tok->len, ENTITY_TYPE) ||
           lookup_unqualified_kind(p, tok->loc, tok->len, ENTITY_TAG);
}

/*
 * Is this identifier a template-name?
 *
 * N4659 §17.1 [temp]: template-name is the name of a template.
 *
 * Used by the disambiguation rule in N4659 §17.2/2 [temp.names]:
 * "After name lookup finds that a name is a template-name ...
 * this name, when followed by <, is always taken as the [start of
 * a] template-id." Sea-front consults this oracle from
 * primary_expr (and a few other call sites) to decide whether to
 * route into parse_template_id.
 */
bool lookup_is_template_name(Parser *p, Token *tok) {
    if (tok->kind != TK_IDENT)
        return false;
    /* Kind-specific lookup: a template-name can be shadowed in the chain
     * by a same-named ENTITY_TYPE/ENTITY_TAG (e.g. when the primary template
     * is followed by an explicit specialization which re-registers the
     * tag/type). The template-ness must still be findable. */
    return lookup_unqualified_kind(p, tok->loc, tok->len,
                                   ENTITY_TEMPLATE) != NULL;
}

/* ------------------------------------------------------------------ */
/* Using directives — N4659 §10.3.4 [namespace.udir]                  */
/* ------------------------------------------------------------------ */

/*
 * Add a namespace's declarative region to the current region's
 * "also search" list for unqualified lookup.
 *
 * N4659 §6.4.1/2: "The declarations from the namespace nominated
 * by a using-directive become visible in a namespace enclosing the
 * using-directive."
 *
 * The using list is arena-allocated and grows semi-exponentially.
 * When the current region is popped, the list is abandoned — scoping
 * is automatic, no explicit clearing needed.
 */
void region_add_using(Parser *p, DeclarativeRegion *ns) {
    if (p->tentative || !ns)
        return;

    DeclarativeRegion *r = p->region;
    if (r->nusing >= r->using_cap) {
        int new_cap = r->using_cap < 4 ? 4 : r->using_cap * 2;
        DeclarativeRegion **new_arr = arena_alloc(p->arena,
            new_cap * sizeof(DeclarativeRegion *));
        if (r->using_regions)
            memcpy(new_arr, r->using_regions,
                   r->nusing * sizeof(DeclarativeRegion *));
        r->using_regions = new_arr;
        r->using_cap = new_cap;
    }
    r->using_regions[r->nusing++] = ns;
}

/*
 * Add a base-class region to the current class scope —
 * N4659 §13.1 [class.derived].
 *
 * 'base' is the class_region of a base class (the region holding its
 * member declarations). When unqualified lookup runs in this class
 * scope, base regions are searched after the class's own buckets.
 *
 * Skipped in tentative mode (no declarative-region pollution).
 */
void region_add_base(Parser *p, DeclarativeRegion *base) {
    if (p->tentative || !base)
        return;
    DeclarativeRegion *r = p->region;
    if (r->nbases >= r->bases_cap) {
        int new_cap = r->bases_cap < 4 ? 4 : r->bases_cap * 2;
        DeclarativeRegion **new_arr = arena_alloc(p->arena,
            new_cap * sizeof(DeclarativeRegion *));
        if (r->bases)
            memcpy(new_arr, r->bases,
                   r->nbases * sizeof(DeclarativeRegion *));
        r->bases = new_arr;
        r->bases_cap = new_cap;
    }
    r->bases[r->nbases++] = base;
}

/*
 * lookup_in_scope — qualified-name lookup: 'A::B'.
 *
 * 'scope' is the region of A; we look up B in it (walking bases
 * for a class scope, but NOT walking enclosing scopes — qualified
 * names are scope-restricted per N4659 §6.4.3
 * [basic.lookup.qual]). Returns NULL if scope is NULL or B isn't
 * found.
 */
Declaration *lookup_in_scope(DeclarativeRegion *scope,
                             const char *name, int name_len) {
    if (!scope) return NULL;
    return lookup_in_region(scope, name, name_len);
}

/*
 * region_find_namespace — find a named namespace's declarative
 * region via name lookup. The namespace name is declared as
 * ENTITY_NAMESPACE with the region pointer stored on the
 * Declaration (ns_region field).
 *
 * N4659 §6.3.6 [basic.scope.namespace] / §10.3 [basic.namespace].
 * Used by 'using namespace' to find the region the directive
 * names so it can be added to the current region's using-list.
 */
DeclarativeRegion *region_find_namespace(Parser *p, const char *name,
                                         int name_len) {
    Declaration *d = lookup_unqualified_kind(p, name, name_len,
                                             ENTITY_NAMESPACE);
    return d ? d->ns_region : NULL;
}
