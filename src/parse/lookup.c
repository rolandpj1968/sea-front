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
/* Hash function — FNV-1a                                              */
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
/* Declarative region management — N4659 §6.3 [basic.scope]            */
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
 * Pop the current declarative region.
 * The region struct remains in the arena — just restore the enclosing pointer.
 *
 * N4659 §6.3.3/1 [basic.scope.block]: "A name declared in a block
 * is local to that block."
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
Declaration *region_declare(Parser *p, const char *name, int name_len,
                            EntityKind entity, Type *type) {
    if (p->tentative)
        return NULL;

    DeclarativeRegion *r = p->region;
    uint32_t idx = hash_name(name, name_len) % REGION_HASH_SIZE;

    Declaration *decl = arena_alloc(p->arena, sizeof(Declaration));
    decl->name = name;
    decl->name_len = name_len;
    decl->entity = entity;
    decl->type = type;
    decl->next = r->buckets[idx];
    r->buckets[idx] = decl;
    return decl;
}

/* ------------------------------------------------------------------ */
/* Unqualified name lookup — N4659 §6.4.1 [basic.lookup.unqual]        */
/*                           N4861 §6.5.1 (C++20)                      */
/*                           N4950 §6.5.1 (C++23)                      */
/* ------------------------------------------------------------------ */

/*
 * Search outward through enclosing declarative regions.
 *
 * N4659 §6.4.1/1: "the scopes are searched for a declaration in
 * the order listed in each of the respective categories; name lookup
 * ends as soon as a declaration is found for the name."
 */
static Declaration *lookup_in_region(DeclarativeRegion *r,
                                     const char *name, int name_len) {
    uint32_t idx = hash_name(name, name_len) % REGION_HASH_SIZE;
    for (Declaration *d = r->buckets[idx]; d; d = d->next) {
        if (d->name_len == name_len && memcmp(d->name, name, name_len) == 0)
            return d;
    }
    return NULL;
}

Declaration *lookup_unqualified(Parser *p, const char *name, int name_len) {
    /* Terminates: walks enclosing chain toward NULL (global has no enclosing) */
    for (DeclarativeRegion *r = p->region; r; r = r->enclosing) {
        /* Search this region's own declarations first */
        Declaration *d = lookup_in_region(r, name, name_len);
        if (d)
            return d;

        /* N4659 §6.4.1/2 [basic.lookup.unqual]: "declarations from the
         * namespace nominated by a using-directive become visible in a
         * namespace enclosing the using-directive."
         *
         * Search regions imported by using-directives in this region. */
        for (int i = 0; i < r->nusing; i++) {
            d = lookup_in_region(r->using_regions[i], name, name_len);
            if (d)
                return d;
        }
    }
    return NULL;
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
    return NULL;
}

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
/* Disambiguation oracles                                              */
/*                                                                     */
/* These are the "two semantic oracles" identified in                   */
/* doc/disambiguation-rules.md. They are convenience wrappers around   */
/* unqualified name lookup that inspect the EntityKind of the result.  */
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
    Declaration *d = lookup_unqualified(p, tok->loc, tok->len);
    return d && (d->entity == ENTITY_TYPE || d->entity == ENTITY_TAG);
}

/*
 * Is this identifier a template-name?
 *
 * N4659 §17.1 [temp]: template-name is the name of a template.
 *
 * Used by disambiguation rule:
 *   §17.2/3 [temp.names] — '<' as template-argument-list vs less-than
 *
 * Deferred to Stage 3 (no templates yet), but the oracle is ready.
 */
bool lookup_is_template_name(Parser *p, Token *tok) {
    if (tok->kind != TK_IDENT)
        return false;
    Declaration *d = lookup_unqualified(p, tok->loc, tok->len);
    return d && d->entity == ENTITY_TEMPLATE;
}

/* ------------------------------------------------------------------ */
/* Using directives — N4659 §10.3.4 [namespace.udir]                   */
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
 * Find a named namespace region by walking outward from the current
 * region. Searches for a REGION_NAMESPACE whose name matches.
 *
 * N4659 §6.3.6 [basic.scope.namespace]: namespace names have
 * namespace scope. The region itself carries the name.
 */
DeclarativeRegion *region_find_namespace(Parser *p, const char *name,
                                         int name_len) {
    for (DeclarativeRegion *r = p->region; r; r = r->enclosing) {
        /* Check this region itself */
        if (r->kind == REGION_NAMESPACE && r->name &&
            r->name->len == name_len &&
            memcmp(r->name->loc, name, name_len) == 0)
            return r;

        /* Check declarations — namespace names are declared as ENTITY_NAMESPACE
         * and the Declaration doesn't carry the region pointer directly.
         * But namespaces push named regions, so we search the enclosing
         * chain for matching REGION_NAMESPACE entries. */
    }
    return NULL;
}
