/*
 * src/codegen/mangle.h — name mangling framework.
 *
 * Mangling is fundamentally a TRAVERSAL of the AST/type tree where
 * every visited node emits something. Different schemes (our human-
 * readable default, Itanium for tooling-compat) AGREE on the
 * traversal but DISAGREE only on leaf tokens. So we factor the
 * walker out of the leaf encoding via a vtable.
 *
 * For now there's just one implementation (the human-readable
 * default). Itanium can be added later as a second vtable without
 * touching call sites — see docs/mangling.md.
 *
 * The framework writes directly to stdout (matching the rest of
 * codegen). When we need to compose mangled names into formatted
 * output, the API can grow a "write to buffer" variant.
 */
#ifndef SF_CODEGEN_MANGLE_H
#define SF_CODEGEN_MANGLE_H

#include "../sea-front.h"

typedef struct Mangler Mangler;

struct Mangler {
    /* Marker prefix at the very start of every mangled symbol.
     * For human: "sf__". For Itanium: "_Z". */
    void (*start)(Mangler *m);

    /* Open / close a namespace name (used while walking outward). */
    void (*open_namespace)(Mangler *m, Token *name);
    void (*close_namespace)(Mangler *m);

    /* Open / close a class scope. The class name is emitted by
     * append_class_name immediately after open_class so that future
     * extensions (template-args between class name and member name)
     * can be inserted naturally. */
    void (*open_class)(Mangler *m);
    void (*append_class_name)(Mangler *m, Token *name);
    void (*close_class)(Mangler *m);

    /* Append a member name (method, field). For ctors / dtors,
     * the dedicated hooks below are used instead so each scheme
     * can pick its own marker. */
    void (*append_member)(Mangler *m, Token *name);
    void (*append_ctor)(Mangler *m);
    void (*append_dtor)(Mangler *m);
    void (*append_dtor_body)(Mangler *m);
};

/* The default human-readable mangler. */
extern Mangler g_mangler_human;

/* Active mangler. Set once at codegen entry; used by all helpers. */
extern Mangler *g_mangler;

/* ---------------------------------------------------------------- */
/* High-level helpers — these are what codegen calls.               */
/*                                                                  */
/* Each writes the full mangled name for the entity directly to     */
/* stdout. They're built on the framework + active vtable so adding */
/* a new scheme means writing one new vtable, not changing callers. */
/* ---------------------------------------------------------------- */

/* Just the class tag (with namespace prefix), e.g. for the C struct
 * declaration: 'struct sf__std__vector { ... };' */
void mangle_class_tag(Type *class_type);

/* A regular method on a class:
 *   class vec, member 'push' → sf__vec__push */
void mangle_class_method(Type *class_type, Token *method_name);

/* Constructor:
 *   class vec → sf__vec__ctor */
void mangle_class_ctor(Type *class_type);

/* Destructor wrapper (chains user dtor body + member dtors):
 *   class vec → sf__vec__dtor */
void mangle_class_dtor(Type *class_type);

/* User dtor body (just the body the user wrote, no member chain):
 *   class vec → sf__vec__dtor_body */
void mangle_class_dtor_body(Type *class_type);

/* The vtable struct type for a polymorphic class:
 *   class vec → sf__vec__vtable */
void mangle_class_vtable_type(Type *class_type);

/* The static vtable instance (one per class with virtual methods):
 *   class vec → sf__vec__vtable_instance */
void mangle_class_vtable_instance(Type *class_type);

#endif /* SF_CODEGEN_MANGLE_H */
