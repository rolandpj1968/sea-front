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

/* Walk OUT from a class to collect enclosing namespace tokens,
 * then emit them outermost-first via the active mangler's
 * open_namespace hook. Bounded to MAX_NS to keep the buffer
 * stack-allocated; namespace nesting in real C++ rarely exceeds 4. */
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
    /* TODO(seafront#mangle-itanium): we currently only call
     * open_namespace, never close_namespace, because the human
     * encoding is separator-style (no balanced markers). An
     * Itanium-style scheme balances namespaces via N…E and would
     * need close_namespace to be called here in reverse order.
     * The vtable HAS the close_namespace hook ready; the
     * framework needs the matching call when an Itanium vtable
     * lands. Same applies to close_class. */
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

void mangle_class_vtable_type(Type *class_type) {
    /* The vtable suffix is sea-front-specific (no Itanium analogue
     * uses this exact form), so we hardcode the leaf rather than
     * route through the vtable for now. When/if an Itanium scheme
     * lands, it'll add proper vtable hooks. */
    emit_class_open(class_type);
    fputs("__vtable", stdout);
    emit_class_close();
}

void mangle_class_vtable_instance(Type *class_type) {
    emit_class_open(class_type);
    fputs("__vtable_instance", stdout);
    emit_class_close();
}
