/*
 * src/codegen/mangle.c — name mangling framework + human vtable.
 *
 * See docs/mangling.md for the design rationale.
 *
 * The framework is the recursive walker over the type/class tree.
 * It calls into a Mangler vtable for the leaf tokens — every scheme
 * (human-readable, Itanium, etc.) provides its own vtable. Currently
 * only the human-readable scheme is implemented.
 *
 * Human-readable encoding (the strawman from docs/mangling.md):
 *   - All sea-front symbols start with 'sf__'.
 *   - Namespace separator: '__' (so std::ranges::vec → sf__std__ranges__vec).
 *   - Class name same form (recursive: nested classes will join with '__').
 *   - Member separator: '__' (so vec::push → sf__vec__push).
 *   - Ctor / dtor / dtor body: '__ctor', '__dtor', '__dtor_body'.
 */
#include "mangle.h"
#include "../parse/parse.h"

#include <stdio.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Human vtable                                                        */
/* ------------------------------------------------------------------ */

static void hum_start(Mangler *m) {
    (void)m;
    fputs("sf__", stdout);
}

static void hum_open_namespace(Mangler *m, Token *name) {
    (void)m;
    if (name)
        fprintf(stdout, "%.*s__", name->len, name->loc);
}

static void hum_close_namespace(Mangler *m) {
    (void)m;
}

static void hum_open_class(Mangler *m) {
    (void)m;
}

static void hum_append_class_name(Mangler *m, Token *name) {
    (void)m;
    if (name)
        fprintf(stdout, "%.*s", name->len, name->loc);
    else
        fputs("anon", stdout);
}

static void hum_close_class(Mangler *m) {
    (void)m;
}

static void hum_append_member(Mangler *m, Token *name) {
    (void)m;
    if (name)
        fprintf(stdout, "__%.*s", name->len, name->loc);
}

static void hum_append_ctor(Mangler *m) {
    (void)m;
    fputs("__ctor", stdout);
}

static void hum_append_dtor(Mangler *m) {
    (void)m;
    fputs("__dtor", stdout);
}

static void hum_append_dtor_body(Mangler *m) {
    (void)m;
    fputs("__dtor_body", stdout);
}

Mangler g_mangler_human = {
    .start              = hum_start,
    .open_namespace     = hum_open_namespace,
    .close_namespace    = hum_close_namespace,
    .open_class         = hum_open_class,
    .append_class_name  = hum_append_class_name,
    .close_class        = hum_close_class,
    .append_member      = hum_append_member,
    .append_ctor        = hum_append_ctor,
    .append_dtor        = hum_append_dtor,
    .append_dtor_body   = hum_append_dtor_body,
};

Mangler *g_mangler = &g_mangler_human;

/* ------------------------------------------------------------------ */
/* Framework — recursive walker that calls into the active vtable.    */
/* ------------------------------------------------------------------ */

/* Walk OUT from a class to collect enclosing namespace tokens, then
 * emit them outermost-first. Mirrors emit_ns_prefix's behavior but
 * routes through the vtable. */
static void emit_namespace_chain(Type *class_type) {
    if (!class_type || !class_type->class_region) return;
    enum { MAX_NS = 16 };
    Token *names[MAX_NS];
    int n = 0;
    DeclarativeRegion *r = class_type->class_region->enclosing;
    while (r && n < MAX_NS) {
        if (r->kind == REGION_NAMESPACE && r->name)
            names[n++] = r->name;
        r = r->enclosing;
    }
    /* Outermost first */
    for (int i = n - 1; i >= 0; i--)
        g_mangler->open_namespace(g_mangler, names[i]);
    /* No matching close_namespace calls — the chain is flat in our
     * encoding (separator-style), not nested (paren-style). The
     * Itanium vtable would balance these via N…E and that's fine. */
    (void)n;
}

/* Open the class scope and emit its name. The caller is responsible
 * for any subsequent member/ctor/dtor append + close. */
static void emit_class_open(Type *class_type) {
    g_mangler->start(g_mangler);
    emit_namespace_chain(class_type);
    g_mangler->open_class(g_mangler);
    g_mangler->append_class_name(g_mangler,
        class_type ? class_type->tag : NULL);
}

static void emit_class_close(void) {
    g_mangler->close_class(g_mangler);
}

/* ------------------------------------------------------------------ */
/* High-level helpers                                                  */
/* ------------------------------------------------------------------ */

void mangle_class_tag(Type *class_type) {
    emit_class_open(class_type);
    emit_class_close();
}

void mangle_class_method(Type *class_type, Token *method_name) {
    emit_class_open(class_type);
    g_mangler->append_member(g_mangler, method_name);
    emit_class_close();
}

void mangle_class_ctor(Type *class_type) {
    emit_class_open(class_type);
    g_mangler->append_ctor(g_mangler);
    emit_class_close();
}

void mangle_class_dtor(Type *class_type) {
    emit_class_open(class_type);
    g_mangler->append_dtor(g_mangler);
    emit_class_close();
}

void mangle_class_dtor_body(Type *class_type) {
    emit_class_open(class_type);
    g_mangler->append_dtor_body(g_mangler);
    emit_class_close();
}
