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
/* Human vtable                                                       */
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

/* Emit the mangled encoding for a single type argument in a
 * template argument list. For the human scheme, this produces
 * the type's C-like name (int, double, struct tag, etc.). */
static void emit_type_for_mangle(Type *ty) {
    if (!ty) { fputs("unknown", stdout); return; }
    /* CV-qualifiers — N4659 §10.1.7.1 [dcl.type.cv].
     * Itanium encodes const as 'K', volatile as 'V'; we use
     * 'const_' / 'volatile_' prefixes for readability.
     * E.g. 'const int&' → 'const_int_ref'. */
    if (ty->is_const)    fputs("const_", stdout);
    if (ty->is_volatile) fputs("volatile_", stdout);
    switch (ty->kind) {
    case TY_VOID:    fputs("void", stdout); return;
    case TY_BOOL:    fputs("bool", stdout); return;
    case TY_CHAR:    fputs(ty->is_unsigned ? "uchar" : "char", stdout); return;
    case TY_CHAR16:  fputs("char16", stdout); return;
    case TY_CHAR32:  fputs("char32", stdout); return;
    case TY_WCHAR:   fputs("wchar", stdout); return;
    case TY_SHORT:   fputs(ty->is_unsigned ? "ushort" : "short", stdout); return;
    case TY_INT:     fputs(ty->is_unsigned ? "uint" : "int", stdout); return;
    case TY_LONG:    fputs(ty->is_unsigned ? "ulong" : "long", stdout); return;
    case TY_LLONG:   fputs(ty->is_unsigned ? "ullong" : "llong", stdout); return;
    case TY_FLOAT:   fputs("float", stdout); return;
    case TY_DOUBLE:  fputs("double", stdout); return;
    case TY_LDOUBLE: fputs("ldouble", stdout); return;
    case TY_PTR:     emit_type_for_mangle(ty->base); fputs("_ptr", stdout); return;
    case TY_REF:     emit_type_for_mangle(ty->base); fputs("_ref", stdout); return;
    case TY_RVALREF: emit_type_for_mangle(ty->base); fputs("_rref", stdout); return;
    /* Array-as-parameter decays to pointer — N4659 §11.3.4/5
     * [dcl.array]. Mangling must use the decayed form so a
     * forward-decl 'f(T*)' and a definition 'f(T arr[N])' produce
     * the same symbol. Without this, gengtype.c's set_gc_used_type
     * had distinct mangled names for fwd-decl vs def. */
    case TY_ARRAY:   emit_type_for_mangle(ty->base); fputs("_ptr", stdout); return;
    case TY_STRUCT: case TY_UNION:
        if (ty->tag) fprintf(stdout, "%.*s", ty->tag->len, ty->tag->loc);
        else fputs("anon", stdout);
        /* If this struct itself is a template instantiation, emit
         * its template args recursively. */
        if (ty->n_template_args > 0) {
            fputs("_t_", stdout);
            for (int i = 0; i < ty->n_template_args; i++) {
                if (i > 0) fputc('_', stdout);
                emit_type_for_mangle(ty->template_args[i]);
            }
            fputs("_te_", stdout);
        }
        return;
    case TY_ENUM:
        if (ty->tag) fprintf(stdout, "%.*s", ty->tag->len, ty->tag->loc);
        else fputs("anon_enum", stdout);
        return;
    default:
        fputs("unknown", stdout);
        return;
    }
}

/* Open the class scope and emit its name. If the class type has
 * template arguments, emit them after the class name using the
 * _t_..._te_ encoding. The caller is responsible for any
 * subsequent member/ctor/dtor append + close. */
static void emit_class_open(Type *class_type) {
    g_mangler->start(g_mangler);
    emit_namespace_chain(class_type);
    g_mangler->open_class(g_mangler);
    g_mangler->append_class_name(g_mangler,
        class_type ? class_type->tag : NULL);
    /* Template argument suffix */
    if (class_type && class_type->n_template_args > 0) {
        fputs("_t_", stdout);
        for (int i = 0; i < class_type->n_template_args; i++) {
            if (i > 0) fputc('_', stdout);
            emit_type_for_mangle(class_type->template_args[i]);
        }
        fputs("_te_", stdout);
    }
}

static void emit_class_close(void) {
    g_mangler->close_class(g_mangler);
}

/* ------------------------------------------------------------------ */
/* High-level helpers                                                 */
/* ------------------------------------------------------------------ */

void mangle_class_tag(Type *class_type) {
    emit_class_open(class_type);
    emit_class_close();
}

/* Append the parameter-type list as a mangled suffix —
 *   _p_<t0>_<t1>_..._pe_
 * with 'void' for the empty list. Mirrors the _t_..._te_ shape used
 * for template arguments. N4659 §16.2 [over.load]. */
void mangle_param_suffix(Type **param_types, int nparams) {
    fputs("_p_", stdout);
    if (nparams == 0) {
        fputs("void", stdout);
    } else {
        for (int i = 0; i < nparams; i++) {
            if (i > 0) fputc('_', stdout);
            emit_type_for_mangle(param_types[i]);
        }
    }
    fputs("_pe_", stdout);
}

void mangle_class_method(Type *class_type, Token *method_name,
                          Type **param_types, int nparams) {
    emit_class_open(class_type);
    g_mangler->append_member(g_mangler, method_name);
    mangle_param_suffix(param_types, nparams);
    emit_class_close();
}

void mangle_class_method_cv(Type *class_type, Token *method_name,
                             Type **param_types, int nparams,
                             bool is_const) {
    emit_class_open(class_type);
    g_mangler->append_member(g_mangler, method_name);
    mangle_param_suffix(param_types, nparams);
    /* N4659 §10.1.7.1 [dcl.type.cv] / §16.3.1/4: const qualifier
     * on the implicit object parameter distinguishes overloads
     * like operator[](int) vs operator[](int) const. */
    if (is_const) fputs("_const", stdout);
    emit_class_close();
}

void mangle_class_ctor(Type *class_type,
                        Type **param_types, int nparams) {
    emit_class_open(class_type);
    g_mangler->append_ctor(g_mangler);
    mangle_param_suffix(param_types, nparams);
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
