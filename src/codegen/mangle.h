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

/* Active scheme. Selected at codegen entry by the --mangling=… flag.
 * MANGLE_HUMAN is the default sea-front-native readable scheme that
 * has been the only choice to date; MANGLE_ITANIUM produces names
 * matching the Itanium C++ ABI <https://itanium-cxx-abi.github.io/
 * cxx-abi/abi.html#mangling> for interop with gcc/clang/libstdc++. */
typedef enum {
    MANGLE_HUMAN = 0,
    MANGLE_ITANIUM = 1,
} MangleKind;

extern MangleKind g_mangle_kind;

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
 * signature isn't available); the caller accepts the ambiguity.
 *
 * is_const distinguishes const from non-const method overloads
 * (N4659 §16.3.1/4). */
void mangle_class_method_tid(Type *class_type, Token *method_name,
                              Type **method_targs, int n_method_targs,
                              Type **param_types, int nparams,
                              bool is_const);
void mangle_class_method(Type *class_type, Token *method_name,
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

/* Static data member — N4659 §11.4.9 [class.static.data]. The OOL
 * definition 'int Foo::bar;' emits as 'sf__Foo__bar' / Itanium
 * '_ZN3Foo3barE'. Without this an OOL static data member would emit
 * its bare source name and clash with any same-named C symbol
 * (libc 'abort', 'errno', etc.). */
void mangle_class_static_data_member(Type *class_type, Token *member_name);

/* Operator method mangling. Sea-front emits the method symbol for
 * an overloaded operator at multiple sites: the class's def, every
 * call (binary expr → operator+, subscript → operator[], etc.), and
 * the class's forward decls. All sites must produce the same symbol;
 * this API is the single entry point so emit_c.c never picks per-
 * scheme strings itself.
 *
 * OP_CONVERSION is handled separately because Itanium encodes it
 * with the target type (`cv<T>`) rather than a fixed operator id —
 * callers use mangle_class_conversion below. */
typedef enum {
    OP_PLUS,           /* + (binary or unary; nparams disambiguates) */
    OP_MINUS,          /* - (binary or unary) */
    OP_STAR,           /* * (binary mul or unary deref) */
    OP_SLASH,          /* / */
    OP_MOD,            /* % */
    OP_AMP,            /* & (binary bitand or unary addr-of) */
    OP_PIPE,           /* | */
    OP_CARET,          /* ^ */
    OP_TILDE,          /* ~ */
    OP_BANG,           /* ! */
    OP_EQ,             /* == */
    OP_NE,             /* != */
    OP_LT,             /* < */
    OP_GT,             /* > */
    OP_LE,             /* <= */
    OP_GE,             /* >= */
    OP_LSHIFT,         /* << */
    OP_RSHIFT,         /* >> */
    OP_LAND,           /* && */
    OP_LOR,            /* || */
    OP_ASSIGN,         /* = */
    OP_PLUS_ASSIGN,    /* += */
    OP_MINUS_ASSIGN,   /* -= */
    OP_MUL_ASSIGN,     /* *= */
    OP_DIV_ASSIGN,     /* /= */
    OP_MOD_ASSIGN,     /* %= */
    OP_BITAND_ASSIGN,  /* &= */
    OP_BITOR_ASSIGN,   /* |= */
    OP_XOR_ASSIGN,     /* ^= */
    OP_LSHIFT_ASSIGN,  /* <<= */
    OP_RSHIFT_ASSIGN,  /* >>= */
    OP_INCR,           /* ++ (pre or post) */
    OP_DECR,           /* -- (pre or post) */
    OP_SUBSCRIPT,      /* [] */
    OP_CALL,           /* () */
    OP_ARROW,          /* -> */
    OP_NEW,            /* operator new       — N4659 §16.5 [over.oper] */
    OP_NEW_ARRAY,      /* operator new[]     — same § */
    OP_DELETE,         /* operator delete    — same § */
    OP_DELETE_ARRAY,   /* operator delete[]  — same § */
    OP_UNKNOWN,        /* fallback for unrecognised operator names */
} OperatorKind;

/* Source-text-driven detection of an operator method's kind from
 * its name token. The token's `loc` points into the source buffer;
 * the operator symbol follows the "operator" keyword in source.
 * Returns OP_UNKNOWN when nothing matches (e.g. conversion
 * operator `operator T()`, where the caller should use
 * mangle_class_conversion instead). */
OperatorKind operator_kind_from_method_name(Token *name);

void mangle_class_operator(Type *class_type, OperatorKind op,
                            Type **param_types, int nparams,
                            bool is_const);

/* Conversion operator `operator T()` — N4659 §16.3.1.5
 * [over.match.conv]. Itanium encodes as `cv<T>`. Human encodes as
 * a per-target-type suffix so distinct conversion operators on the
 * same class get distinct symbols. */
void mangle_class_conversion(Type *class_type, Type *target_type,
                              bool is_const);

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
 * canonical mangling path. `is_variadic` adds the Itanium ABI `z`
 * suffix (§5.1.6 [mangle.fn-type]/ellipsis) — a trailing `_var_`
 * marker in the human scheme — so `f(int)` and `f(int, ...)` mangle
 * to distinct symbols and the dedup key distinguishes them. */
void mangle_param_suffix(Type **param_types, int nparams, bool is_variadic);
int mangle_param_suffix_to_buf(Type **param_types, int nparams, bool is_variadic,
                               char *buf, int pos, int max);

/* Encode a type into a buffer using the same scheme as
 * emit_type_for_mangle (the C-symbol-safe encoding used in mangled
 * names). Returns the new buffer position. Used by the function-
 * template instantiation pass to build a mangled name where each
 * arg's encoding must agree with what the codegen mangler emits
 * elsewhere. C-safe: no '<', '>', or other illegal-in-identifier
 * characters. */
int mangle_type_to_buf(Type *ty, char *buf, int pos, int max);

/* ---------------------------------------------------------------- */
/* Itanium C++ ABI implementations (src/codegen/mangle_itanium.c).  */
/*                                                                  */
/* One entry point per public mangle_* helper above. The dispatch   */
/* in mangle.c selects between human and itanium based on           */
/* g_mangle_kind. Each Itanium entry resets its per-symbol          */
/* substitution table at the start.                                 */
/* ---------------------------------------------------------------- */
void itan_mangle_class_tag(Type *class_type);
void itan_mangle_class_method(Type *class_type, Token *method_name,
                               Type **param_types, int nparams,
                               bool is_const);
void itan_mangle_class_method_tid(Type *class_type, Token *method_name,
                                   Type **method_targs, int n_method_targs,
                                   Type **param_types, int nparams,
                                   bool is_const);
void itan_mangle_class_ctor(Type *class_type,
                             Type **param_types, int nparams);
void itan_mangle_class_dtor(Type *class_type);
void itan_mangle_class_dtor_body(Type *class_type);
void itan_mangle_class_static_data_member(Type *class_type,
                                           Token *member_name);
void itan_mangle_class_operator(Type *class_type, OperatorKind op,
                                 Type **param_types, int nparams,
                                 bool is_const);
void itan_mangle_class_conversion(Type *class_type, Type *target_type,
                                   bool is_const);
void itan_mangle_class_vtable_type(Type *class_type);
void itan_mangle_class_vtable_instance(Type *class_type);
void itan_emit_type_for_mangle(Type *ty);
void itan_mangle_param_suffix(Type **param_types, int nparams, bool is_variadic);

#endif /* SF_CODEGEN_MANGLE_H */
