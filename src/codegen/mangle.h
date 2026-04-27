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

/* A regular method on a class. The parameter type list is encoded
 * as a suffix so overloads get distinct C names:
 *   vec::push(int)   → sf__vec__push_p_int_pe_
 *   vec::push(int,T) → sf__vec__push_p_int_T_pe_
 * N4659 §16.2 [over.load] — distinct overloads must have distinct
 * signatures. C has no overloading, so the signature is encoded
 * into the symbol name.
 *
 * param_types / nparams describe the method's own parameters
 * (excluding the implicit 'this'). Pass NULL/0 for signature-less
 * contexts (e.g. forward references from templates where the
 * signature isn't available); the caller accepts the ambiguity. */
void mangle_class_method(Type *class_type, Token *method_name,
                          Type **param_types, int nparams);

/* Like mangle_class_method but appends '_const' when is_const is true.
 * Distinguishes const from non-const method overloads (N4659 §16.3.1/4). */
void mangle_class_method_cv(Type *class_type, Token *method_name,
                             Type **param_types, int nparams,
                             bool is_const);

/* Constructor. Parameter-type suffix disambiguates overloads:
 *   vec()        → sf__vec__ctor_p_void_pe_
 *   vec(int)     → sf__vec__ctor_p_int_pe_
 *   vec(vec&)    → sf__vec__ctor_p_vec_ref_pe_ */
void mangle_class_ctor(Type *class_type,
                        Type **param_types, int nparams);

/* Destructor wrapper (chains user dtor body + member dtors). Dtors
 * cannot be overloaded so no parameter suffix:
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

/* Emit a single type's mangled encoding to stdout (used inside
 * template-arg lists, e.g. _t_<this>_<this>_te_). Same encoding
 * as mangle_param_suffix's per-param output and as
 * mangle_type_to_buf — keep all three in lockstep. */
void emit_type_for_mangle(Type *ty);

/* Emit just the parameter-type suffix (_p_<types>_pe_) to stdout.
 * Exposed so emit_c.c call-site rewrites can stay in sync with the
 * canonical mangling path. */
void mangle_param_suffix(Type **param_types, int nparams);
int  mangle_param_suffix_to_buf(Type **param_types, int nparams,
                                 char *buf, int pos, int max);

/* Encode a type into a buffer using the same scheme as
 * emit_type_for_mangle (the C-symbol-safe encoding used in mangled
 * names). Returns the new buffer position. Used by the function-
 * template instantiation pass to build a mangled name where each
 * arg's encoding must agree with what the codegen mangler emits
 * elsewhere. C-safe: no '<', '>', or other illegal-in-identifier
 * characters. */
int mangle_type_to_buf(Type *ty, char *buf, int pos, int max);

#endif /* SF_CODEGEN_MANGLE_H */
