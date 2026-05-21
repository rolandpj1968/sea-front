/*
 * emit_c.c — AST → C codegen.
 *
 * Walks a parsed (and sema'd) translation unit and emits C source
 * to stdout. The lowering targets a fixed C subset that any C99
 * compiler can build (notably tcc-from-mescc — sea-front's whole
 * point is to be the C/C++ bridge in the bootstrappable.org chain;
 * see memory project_bootstrap_north_star).
 *
 * Currently emits:
 *   - Free functions, parameters with all built-in and pointer types
 *   - References (TY_REF / TY_RVALREF) lowered to plain C pointers
 *   - Variable declarations with init or direct-init (T x(args))
 *   - All flow control: return, if/else, while, for, do-while,
 *     break/continue, switch/case/default, goto/label
 *   - Compound statements with cleanup-aware destructor unwinding
 *     (the Slice A-D dtor lowering — see below)
 *   - Full binary/unary/ternary/assignment expressions
 *   - All literals (int, float, char, bool, string, nullptr)
 *   - Identifiers (with this-> rewriting for class members and
 *     __sf_base.<...> walking for inherited members)
 *   - Function calls including method calls (direct dispatch via
 *     mangled name) and virtual calls (indirect dispatch via vptr)
 *   - Class definitions: struct layout, ctors, dtors, ctor/dtor
 *     synthesis, member-initializer lists, vptr install + vtable
 *     emission, base-subobject embedding for non-virtual inheritance
 *
 * Mangling: every emitted symbol routes through src/codegen/mangle.c
 * and the Mangler vtable (default scheme: human-readable 'sf__' form;
 * see docs/mangling.md). Direct fputs of unmangled forms is
 * deliberately avoided so a future Itanium adapter is a one-vtable
 * change.
 *
 * NOT YET EMITTED (gaps that need their own slice):
 *   - Operator overloading (parsed, but not lowered to op_* calls)
 *   - Templates beyond opaque parse (no instantiation)
 *   - Exceptions (try/throw/catch — the cleanup chain is the
 *     substrate but the THROW unwind variant isn't wired up)
 *   - Virtual inheritance, virtual destructors in vtable slots
 *   - RTTI / dynamic_cast / typeid
 *   - Coroutines / modules / concepts (C++20+)
 *
 * The output is intentionally minimal — no formatting cleverness,
 * no register allocation, no optimisation. The trust story is
 * "read the lowered C and verify it matches the source", so
 * readability beats brevity.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emit_c.h"
#include "mangle.h"
#include "../sea-front.h"

bool g_emit_line_directives = true;

/* Forward declarations for the typeid sentinel helpers — used by
 * emit_expr's ND_TYPEID case which lives ahead of the definitions. */
static const char *typeid_sym_for_type(Type *ty);
static Type       *typeid_resolved_type(Node *n);

/* Two-phase emit: structs first, then method bodies. */
static int g_emit_phase = 0;
enum { PHASE_STRUCTS = 1, PHASE_METHODS = 2 };

static int g_indent = 0;
/* Currently-emitting class definition (for ctor member-init walking
 * and inherited-member access rewriting). Set by emit_class_def
 * before each method body, restored after. NULL outside class
 * member emission. */
static Node *g_current_class_def = NULL;
/* Return type of the function currently being emitted. Set by
 * emit_func_def / emit_method_as_free_fn so ND_RETURN can adapt to
 * reference returns (T& lowered to T*: 'return *x;' → 'return x;'). */
static Type *g_current_func_ret_ty = NULL;
/* Source-name token of the function currently being emitted. Used to
 * rewrite `__func__` / `__FUNCTION__` references inside the body to a
 * string literal of the user-written name; the C compiler's native
 * __func__ would expand to the mangled symbol instead (e.g.
 * '_ZN3y8a4zqjxEic' instead of 'zqjx'). N4659 §8.4.1/8 [dcl.fct.def]
 * says __func__ is "the unqualified name of the enclosing function".
 * Patterns: g++.dg/ext/fnname{1,2,3}.C. */
static Token *g_current_func_src_name = NULL;
static bool   g_current_func_is_dtor   = false;
/* Class type of the OOL method currently being emitted. Used by
 * ND_OFFSETOF to substitute unresolvable local typedefs. */
static Type *g_current_method_class = NULL;
/* Const-qualification of the current method. Used to pick the right
 * overload when an unqualified method call inside a const method
 * invokes another method — N4659 §16.3.1.4 [over.match.funcs]/4
 * says the implicit 'this' is 'const C*' when the enclosing method
 * is const, which biases overload resolution toward const overloads. */
static bool g_current_method_is_const = false;
/* True while emitting the body of a static member function. Static
 * methods have no 'this' (N4659 §11.4.9 [class.static]), so the
 * ND_IDENT emit's 'this->' prefix for class-member refs must be
 * suppressed when this flag is set. The class-method mangle path
 * still fires (we want sf__C__release_p_..._pe_ for sibling calls),
 * just without the leading 'this->'. */
static bool g_current_method_is_static = false;

/* Translation unit root — set by emit_c at entry, consulted by
 * helpers that need to find ND_CLASS_DEF nodes by tag+template_args
 * (template-instantiated method bodies sometimes carry Type copies
 * with class_def=NULL and we need the real body for overload
 * resolution / const-qualification). */
static Node *g_tu = NULL;

/* Active lambda function — N4659 §8.1.5 [expr.prim.lambda]. Set by
 * emit_func_def for capturing-lambda function bodies (func.is_lambda_fn).
 * emit_ident consults g_current_lambda_fn->func.captures and rewrites
 * matching identifiers to '__self->name' (by-value) or '*__self->name'
 * (by-ref). NULL outside lambda bodies. */
static Node *g_current_lambda_fn = NULL;

/* Reference-parameter tracking — N4659 §11.3.2 [dcl.ref].
 * C has no references; we lower T& to T* in the C signature.
 * When the body uses a ref-param as a value, codegen must deref it.
 * cf_begin_function populates this table from the func's params;
 * is_ref_param checks it at ident-emission time. */
/* Table of the current function's reference-parameter names.
 * Sized to the function's actual param count at cf_begin_function;
 * any number of ref params is supported. */
static Token **g_ref_params = NULL;
static int     g_nref_params = 0;
static int     g_ref_params_cap = 0;

static bool is_ref_param(Token *name) {
    if (!name) return false;
    for (int i = 0; i < g_nref_params; i++) {
        Token *rp = g_ref_params[i];
        if (rp && tokens_equal(rp, name))
            return true;
    }
    return false;
}

/* Suppresses ref-param deref for one ident emission. Set by
 * ND_MEMBER before emitting its object (the -> already derefs),
 * and by ND_UNARY TK_AMP (address-of a ref is the pointer itself). */
static bool g_suppress_ref_deref = false;

static void emit_indent(void) {
    for (int i = 0; i < g_indent; i++) fputs("    ", stdout);
}

/* Symmetric block-brace helpers — paired with each other.
 *
 *   emit_open_brace()  emits "{\n" and increments indent.
 *   emit_close_brace() decrements indent, indents, emits "}\n".
 *
 * The asymmetry between the call-site shapes (open is one line, close
 * is three) is purely K&R style: openers are typically glued to the
 * line above ('if (x) {', 'do {', 'fn() {'). The operations them-
 * selves — enter scope / exit scope — are symmetric.
 *
 * These helpers cover the bare-{ case. K&R-glued opens still emit the
 * opener text in one fputs("do {\n", ...) etc. and their matching
 * close still uses emit_close_brace() — open helper doesn't fit, but
 * the close helper does, and we accept the visible asymmetry rather
 * than wrap the K&R-glued openers in a printf-shaped helper. */
static void emit_open_brace(void) {
    fputs("{\n", stdout);
    g_indent++;
}

static void emit_close_brace(void) {
    g_indent--;
    emit_indent();
    fputs("}\n", stdout);
}

/* ------------------------------------------------------------------ */
/* Per-function codegen state for destructor lowering (Slice B)       */
/*                                                                    */
/* When the function body contains any local with a non-trivial dtor  */
/* we lower 'return expr' as:                                         */
/*     __retval = expr; __unwind = 1; goto __cleanup_<innermost>;     */
/* Each block carrying cleanups emits a label, runs its dtors, and    */
/* conditionally chains outward via 'if (__unwind) goto <parent>'.    */
/* The function epilogue runs 'return __retval;'.                     */
/*                                                                    */
/* This is per-function state, not nested blocks, so a flat module    */
/* global is fine — codegen is single-threaded and not reentrant.     */
/* ------------------------------------------------------------------ */

/* Per-var cleanup tracking — Slice C extension.
 *
 * The 'live' stack now holds two kinds of entries:
 *   CL_VAR  — a constructed dtor-bearing local. Has a label_id and
 *             a var_decl; its label is both a dtor call site and a
 *             chain target for any unwind type.
 *   CL_LOOP — a loop boundary marker. Pushed by emit_while/do/for
 *             when a loop has cleanups in its body. Carries the
 *             loop's break_label and cont_label. Markers do not run
 *             dtors; they exist so break/continue chains know where
 *             to terminate.
 *
 * Walking the stack outward:
 *   RETURN: skip CL_LOOP markers, find the next CL_VAR; if none,
 *           target __SF_epilogue.
 *   BREAK:  use the FIRST entry encountered (CL_VAR or CL_LOOP).
 *           A var chains to the next thing; a marker is the loop
 *           boundary and resolves to its break_label.
 *   CONT:   same as BREAK, but a marker resolves to its cont_label.
 *
 * For chain-out emission at the bottom of a block, the three target
 * walks usually collapse to the same answer (no enclosing loop, or
 * no marker between us and the next var). We compare and emit a
 * single 'if (__SF_unwind) goto X' when they coincide, otherwise a
 * three-way conditional. */

typedef enum CleanupKind {
    CL_VAR,
    CL_LOOP,
    /* Try-block landing pad — N4659 §18.1 [except.pre]. Pushed
     * before emitting the try-body so a throw inside lowers to a
     * goto into the handler-dispatch sequence (label_id) instead
     * of falling through to the function epilogue. The dispatch
     * sequence is emitted right after the body and pops the entry
     * before its own handlers run. See docs/exceptions.md. */
    CL_TRY,
} CleanupKind;

typedef struct CleanupEntry {
    CleanupKind kind;
    int         label_id;       /* CL_VAR: cleanup label / CL_LOOP: break_label */
    int         cont_label_id;  /* CL_LOOP only */
    Node       *var_decl;       /* CL_VAR only */
} CleanupEntry;

static struct {
    bool          func_has_cleanups;  /* function-wide flag, set by pre-scan */
    int           next_label_id;      /* fresh-id counter */
    CleanupEntry *live;               /* grows to fit deepest cleanup nesting */
    int           nlive;              /* number of live entries currently */
    int           cap;                /* allocated capacity of live[] */
} g_cf;

/* Grow g_cf.live so g_cf.nlive can advance by 'need' more entries.
 * Geometric growth keeps amortized push cost constant. The buffer
 * stays alive across function emissions; pushes reset nlive to 0
 * but don't shrink cap, so subsequent functions reuse the allocation. */
static void cleanup_live_reserve(int need) {
    if (g_cf.nlive + need <= g_cf.cap) return;
    int ncap = g_cf.cap ? g_cf.cap * 2 : 16;
    while (ncap < g_cf.nlive + need) ncap *= 2;
    CleanupEntry *nlive = realloc(g_cf.live, ncap * sizeof(CleanupEntry));
    if (!nlive) abort();
    g_cf.live = nlive;
    g_cf.cap  = ncap;
}

/* For RETURN: walk outward, skip CL_LOOP markers, find first CL_VAR.
 * Returns label_id, or -1 meaning "jump straight to __SF_epilogue". */
static int find_return_target_from(int top) {
    for (int i = top - 1; i >= 0; i--)
        if (g_cf.live[i].kind == CL_VAR)
            return g_cf.live[i].label_id;
    return -1;
}

/* For BREAK: walk outward looking for a CL_LOOP — only loops and
 * switches are valid break destinations (N4659 §9.6 [stmt.break]).
 * CL_VARs above the loop contribute their cleanup labels as chain
 * stops, but the topmost entry being a CL_VAR or CL_TRY without
 * any enclosing CL_LOOP means there's no break destination at all
 * (sema would have flagged a user-written break; the cleanup
 * machinery should likewise emit no break-chain).
 * Returns the topmost CL_VAR label_id when a CL_LOOP exists above
 * it, else -1. */
static int find_break_target_from(int top) {
    bool has_loop = false;
    for (int i = top - 1; i >= 0; i--) {
        if (g_cf.live[i].kind == CL_LOOP) { has_loop = true; break; }
    }
    if (!has_loop) return -1;
    if (top <= 0) return -1;
    return g_cf.live[top - 1].label_id;
}

/* For CONT: same rule as break — only meaningful with an enclosing
 * CL_LOOP (N4659 §9.6 [stmt.cont]). Returns the topmost entry's
 * appropriate label when a loop is present, else -1. */
static int find_cont_target_from(int top) {
    bool has_loop = false;
    for (int i = top - 1; i >= 0; i--) {
        if (g_cf.live[i].kind == CL_LOOP) { has_loop = true; break; }
    }
    if (!has_loop) return -1;
    if (top <= 0) return -1;
    CleanupEntry *e = &g_cf.live[top - 1];
    return (e->kind == CL_VAR) ? e->label_id : e->cont_label_id;
}

/* Top-of-stack views for ND_RETURN/BREAK/CONTINUE rewrite sites. */
static int return_target(void) { return find_return_target_from(g_cf.nlive); }
static int break_target(void)  { return find_break_target_from(g_cf.nlive); }
static int cont_target(void)   { return find_cont_target_from(g_cf.nlive); }

/* For ND_THROW: walk outward until the first CL_VAR (its dtor must
 * fire) or CL_TRY (its handler dispatches). Skip CL_LOOP markers —
 * a loop boundary is irrelevant to throw propagation. The
 * difference from return_target() is that a CL_TRY counts as a
 * throw target. Returns label_id, or -1 to mean "jump straight to
 * __SF_epilogue" (uncaught throw exits via the function epilogue,
 * propagating __SF_unwind = THROW to the caller for slice 5).
 *
 * The 'in_try' out-parameter (when non-NULL) indicates whether the
 * matched entry is a CL_TRY — useful for emit-side decisions like
 * "do not run dtors, jump straight to dispatch". For slice 4 the
 * caller currently ignores this distinction since dtor integration
 * with try-blocks is deferred. */
static int find_throw_target_from(int top, bool *in_try) {
    if (in_try) *in_try = false;
    for (int i = top - 1; i >= 0; i--) {
        if (g_cf.live[i].kind == CL_VAR) return g_cf.live[i].label_id;
        if (g_cf.live[i].kind == CL_TRY) {
            if (in_try) *in_try = true;
            return g_cf.live[i].label_id;
        }
    }
    return -1;
}
static int throw_target(void) {
    return find_throw_target_from(g_cf.nlive, NULL);
}

/* Track emitted enum bodies by their enum_tokens pointer.
 *
 * Why pointer-not-flag: type.c shallow-copies Type structs when a
 * typedef-name is used as a type-specifier (so cv-qualifiers don't
 * leak), which means the boolean codegen_emitted on the original
 * Type is invisible to copies. The enum_tokens ARRAY pointer survives
 * the shallow copy unchanged, so it's a stable identity across all
 * copies of the same logical enum. Marking by token-array address
 * makes one emission cover all copies. */
enum { ENUM_EMIT_CAP = 256 };
static Token *g_emitted_enum_bodies[ENUM_EMIT_CAP];
static int    g_emitted_enum_count = 0;

static bool enum_body_already_emitted(Token *toks) {
    if (!toks) return false;
    for (int i = 0; i < g_emitted_enum_count; i++)
        if (g_emitted_enum_bodies[i] == toks) return true;
    return false;
}

static void mark_enum_body_emitted(Token *toks) {
    if (!toks) return;
    if (g_emitted_enum_count < ENUM_EMIT_CAP)
        g_emitted_enum_bodies[g_emitted_enum_count++] = toks;
}

/* Forward decls for the hoist helpers below — both call into the
 * regular emitters, which appear later in the file. */
static void emit_expr(Node *n);
static void emit_type(Type *ty);
static bool emit_anon_member_rewrite(Token *name);
static void emit_array_dims(Type *array_chain);
/* Walks a TY_PTR/TY_ARRAY chain and emits a C declarator interleaving
 * '*' and '[N]' brackets around the optional name; returns true when
 * a pointer-to-array shape was detected and emitted, false otherwise.
 * 'name' may be NULL (used in casts / function returns / sizeof). */
static bool emit_pointer_to_array_declarator(Type *ty, Token *name);
static void emit_mangled_class_tag(Type *class_type);
static void emit_stmt(Node *n);
static int collect_call_arg_types(Node **args, int nargs, Type ***out_types);
static Node *func_param_node(Node *m, int k);
/* After a call site has emitted nargs user-supplied arguments,
 * fill any trailing param slots [nargs, np) from the resolved
 * callee's param.default_value expressions. N4659 §11.3.6
 * [dcl.fct.default]. Each emit prepends ", " — caller has
 * already emitted at least one arg or the bare callee. The pty
 * array (when non-NULL) provides param Types for ref-adaptation;
 * pass NULL when types aren't readily available — defaults are
 * usually simple values (NULL, 0, false) that need no adaptation. */
static void emit_default_args_tail(Node *resolved_callee, int nargs, int np,
                                    Type **pty);
static int resolve_overload(Type *class_type, Token *name, bool is_ctor,
                             Type **arg_types, int nargs,
                             bool receiver_is_const,
                             Type ***out_param_types,
                             Node **out_best);
static int resolve_operator_overload(Type *class_type,
                                      OperatorKind op,
                                      Type **arg_types, int nargs,
                                      bool receiver_is_const,
                                      Type ***out_param_types,
                                      Node **out_best);

/* Emit a C function-prototype parameter-type list (the contents
 * between the parens — caller emits the '(' and ')'). Handles the
 * three N4659 §11.3.5 [dcl.fct] cases:
 *   - empty list:      'void'         (C requires explicit void)
 *   - non-variadic:    'T1, T2, T3'
 *   - variadic:        'T1, T2, ...'  (or just '...' when nparams==0)
 *
 * Used by emit_var_decl_inner's function-pointer typedef case, the
 * top-level free-function declarator emit, and the var-decl-as-fn
 * top-level path. Five sites collapsed; doesn't fit two more sites
 * that omit the 'void' for empty (function-pointer-return shape). */
static void emit_func_param_types(Type *fty) {
    for (int i = 0; i < fty->nparams; i++) {
        if (i > 0) fputs(", ", stdout);
        emit_type(fty->params[i]);
    }
    if (fty->is_variadic) {
        if (fty->nparams > 0) fputs(", ", stdout);
        fputs("...", stdout);
    } else if (fty->nparams == 0) {
        fputs("void", stdout);
    }
}

/* Type-peeling helpers — consolidate the many
 * 'if (ty->kind == TY_REF || ty->kind == TY_RVALREF)' and similar
 * open-coded patterns. References lower to pointers in C, so these
 * three kinds are treated as a single 'indirection' concept.
 * N4659 §11.3.1-§11.3.2 [dcl.ptr][dcl.ref]. */
static inline bool ty_is_ref(Type *t) {
    return t && (t->kind == TY_REF || t->kind == TY_RVALREF);
}
static inline bool ty_is_indirect(Type *t) {
    return t && (t->kind == TY_PTR || t->kind == TY_REF || t->kind == TY_RVALREF);
}

/* Does any type in `params[0..n)` mention TY_DEPENDENT — directly,
 * via TY_REF/TY_RVALREF/TY_PTR/TY_ARRAY chains, OR nested inside
 * a TY_STRUCT/TY_UNION's template_args? Used to detect call sites
 * inside cloned member-template bodies where the candidate's
 * declared params still carry the template's TY_DEPENDENT
 * placeholders that need substitution from the call's arg types. */
static bool ty_has_dependent(Type *t) {
    if (!t) return false;
    if (t->kind == TY_DEPENDENT) return true;
    if (t->kind == TY_PTR || t->kind == TY_REF || t->kind == TY_RVALREF ||
        t->kind == TY_ARRAY)
        return ty_has_dependent(t->base);
    if (t->kind == TY_STRUCT || t->kind == TY_UNION) {
        for (int i = 0; i < t->n_template_args; i++)
            if (ty_has_dependent(t->template_args[i])) return true;
    }
    return false;
}
static bool ty_contains_dependent(Type **params, int n) {
    for (int i = 0; i < n; i++)
        if (ty_has_dependent(params[i])) return true;
    return false;
}

/* Emit a call-site argument with reference-parameter adaptation.
 *
 * Mirrors emit_return_expr in the opposite direction: when the
 * formal parameter is T& / T&& (lowered to T*), a value-typed
 * argument like '*this' or 'foo' must become '&(arg)' so the C
 * call passes the address. If the argument is already a pointer in
 * our lowering (resolved_type is TY_PTR / TY_REF / TY_RVALREF) we
 * pass it through. The '*X' → 'X' simplification keeps generated
 * C readable in the common '*this' case.
 *
 * If we don't know the param type (param_ty == NULL), fall back to
 * plain emit_expr — the caller is signalling 'no signature info,
 * trust the source'. */
/* Emit a call-site argument adapted for a ref-typed parameter.
 * N4659 §11.3.2 [dcl.ref]: references are lowered to pointers in C.
 * When the param is T& (lowered to T*), the arg must be passed as &arg
 * UNLESS the arg is already a ref/rvalref (i.e. already a pointer in
 * our lowering). Plain pointers (TY_PTR) are NOT the same — a T*
 * arg passed to a T*& param needs &(arg) to become T**. */
/* Is the node a C lvalue — can we safely take its address with '&'?
 * Covers the common shapes: identifier, member access, array subscript,
 * and pointer dereference. Conservative: returns false for anything
 * we're not sure about, which routes through the compound-literal path
 * in emit_arg_for_param. N4659 §7.2.1 [basic.lval]. */
static bool is_addressable_lvalue(Node *n) {
    if (!n) return false;
    switch (n->kind) {
    case ND_IDENT:
    case ND_MEMBER:
    case ND_SUBSCRIPT:
        return true;
    case ND_UNARY:
        return n->unary.op == TK_STAR;
    default:
        return false;
    }
}

static int find_base_path(Type *current, Type *target,
                          int *path, int max_depth);
static void emit_base_chain(int *path, int len);
static Node *find_class_def_by_tag_only(Type *class_type);
static Node *find_class_def_by_tag_args(Type *class_type);
static int class_nbases(Type *class_type);
static Type *class_base(Type *class_type, int i);

static void emit_arg_for_param(Node *arg, Type *param_ty) {
    if (!arg) return;
    /* 'vNULL' passed as a function argument to a struct-typed param:
     * same issue as the ND_ASSIGN / return paths — ND_IDENT vNULL
     * lowers to '{0}' which C only accepts in init-declarators.
     * Emit a compound literal using the param type. Pattern:
     * gcc 4.8 function.c 'convert_jumps_to_returns(bb, false, vNULL)'. */
    if (arg->kind == ND_IDENT && arg->ident.name &&
        arg->ident.name->len == 5 &&
        memcmp(arg->ident.name->loc, "vNULL", 5) == 0 &&
        param_ty && (param_ty->kind == TY_STRUCT ||
                     param_ty->kind == TY_UNION) &&
        param_ty->tag) {
        fputc('(', stdout);
        emit_type(param_ty);
        fputs("){0}", stdout);
        return;
    }
    /* Pass-through fast path: the arg is an ND_IDENT naming a
     * ref-param and we either know the callee takes a ref-param
     * here, or we don't know the param type at all. In both cases
     * the right C emission is the bare ident — its C-level value
     * is already the address (ref-params are lowered to T**).
     *
     * Why this comes BEFORE the param_ty == NULL early return:
     * for member-template qualified calls (e.g. gcc 4.8 vec.h's
     * 'A::reserve(v, ...)' inside a cloned vec_safe_reserve body),
     * sema can't resolve the member-template name so callee_ft is
     * NULL and pty falls through as NULL — emit_arg_for_param
     * previously fell to emit_expr(arg) which emits '(*v)' (the
     * usual ref-param deref), passing a dereferenced NULL into a
     * function expecting vec** → segfault inside va_heap::reserve.
     * Suppressing the deref keeps the address-of-pointer through.
     *
     * The only case this misroutes is a non-ref param taking a
     * value where we'd want '(*v)'; a -Wincompatible-pointer-types
     * warning surfaces it. Member-template ref-pass-through is the
     * far more common gcc/libstdc++ pattern, and keeping the
     * default safe for known-ref params remains exactly correct. */
    bool ident_is_ref = arg && arg->kind == ND_IDENT && arg->ident.name &&
                        is_ref_param(arg->ident.name);
    if (ident_is_ref && (param_ty == NULL || ty_is_ref(param_ty))) {
        bool saved = g_suppress_ref_deref;
        g_suppress_ref_deref = true;
        emit_expr(arg);
        g_suppress_ref_deref = saved;
        return;
    }
    /* Derived-to-base POINTER conversion — N4659 §7.11 [conv.ptr]/3.
     * Mirror of the reference-binding case below, but for plain
     * pointer params. When 'D *p' is passed to a 'B *' param and B
     * is a base of D, C++ adjusts the pointer to the B-subobject.
     * In sea-front's layout the Nth base lives at __sf_baseN, so a
     * D-pointer pp must be lowered to '&(pp)->__sf_baseN' (or '&p'
     * for the first base, where offset is zero — but we emit
     * '__sf_base' uniformly for clarity and let the compiler fold). */
    if (param_ty && param_ty->kind == TY_PTR && param_ty->base &&
        (param_ty->base->kind == TY_STRUCT || param_ty->base->kind == TY_UNION) &&
        arg->resolved_type && arg->resolved_type->kind == TY_PTR &&
        arg->resolved_type->base &&
        (arg->resolved_type->base->kind == TY_STRUCT ||
         arg->resolved_type->base->kind == TY_UNION) &&
        arg->resolved_type->base != param_ty->base) {
        Type *deriv = arg->resolved_type->base;
        if (!deriv->class_region && deriv->tag) {
            Node *d = find_class_def_by_tag_args(deriv);
            if (!d) d = find_class_def_by_tag_only(deriv);
            if (d && d->class_def.ty && d->class_def.ty->class_region)
                deriv = d->class_def.ty;
        }
        int base_path[8];
        int base_len = find_base_path(deriv, param_ty->base, base_path, 8);
        if (base_len == 0 && param_ty->base->tag) {
            Type *tgt = param_ty->base;
            int nb = class_nbases(deriv);
            for (int b = 0; b < nb; b++) {
                Type *base = class_base(deriv, b);
                if (!base || !base->tag) continue;
                if (base->tag->len == tgt->tag->len &&
                    memcmp(base->tag->loc, tgt->tag->loc,
                           tgt->tag->len) == 0) {
                    base_path[0] = b;
                    base_len = 1;
                    break;
                }
            }
        }
        if (base_len > 0) {
            fputs("&(", stdout);
            emit_expr(arg);
            fputs(")->", stdout);
            if (base_len > 1)
                emit_base_chain(base_path, base_len - 1);
            int last = base_path[base_len - 1];
            if (last == 0) fputs("__sf_base", stdout);
            else           fprintf(stdout, "__sf_base%d", last);
            return;
        }
    }
    if (!ty_is_ref(param_ty)) {
        /* Pass-by-value of a class-typed arg: C++ requires the copy
         * ctor to run. C's struct assignment is bitwise — fine for
         * trivially-copyable classes, but wrong when the user
         * defined a copy ctor with observable side effects.
         * N4659 §11.4.5 [class.copy.ctor], §16.3.1.4
         * [over.match.copy].
         *
         * Detect: param is a class, arg's resolved_type names the
         * same class (looking through cv / refs), arg is addressable,
         * and the class declares a 1-arg ctor accepting (const T&) or
         * (T&). When all hold, emit
         *   ({ T __sf_cpy; T_copy_ctor(&__sf_cpy, &arg); __sf_cpy; })
         * GNU stmt-expr — the rest of the EH path uses these already.
         * Patterns: g++.dg/init/elide2.C, copy3.C, cpp0x/implicit2.C. */
        /* Gated on !has_dtor: when the class has a destructor, a
         * copy-constructed param would need its dtor to fire after
         * the call. Sea-front doesn't yet manage stmt-expr-scoped
         * temps that way — so for dtor-bearing classes the pre-fix
         * bitwise path is kept (matches the dtor-count expectation
         * in tests like opt/dtor1.C which check ctor/dtor balance).
         * For classes without a dtor, the copy ctor side effects
         * still apply via this stmt-expr and no dtor counterbalance
         * is needed. TODO: hoist the copy temp out of the arg and
         * register its dtor after the call, then this gate can lift. */
        if (param_ty &&
            (param_ty->kind == TY_STRUCT || param_ty->kind == TY_UNION) &&
            !param_ty->has_dtor &&
            arg->resolved_type && is_addressable_lvalue(arg)) {
            Type *src = arg->resolved_type;
            if (src->kind == TY_REF || src->kind == TY_RVALREF)
                src = src->base;
            if (src && src->tag && param_ty->tag &&
                tokens_equal(src->tag, param_ty->tag)) {
                Type *at[1] = { src };
                Type **pty = NULL;
                Node *best = NULL;
                int np = resolve_overload(param_ty, /*name=*/NULL,
                                           /*is_ctor=*/true, at, 1,
                                           /*receiver_is_const=*/false,
                                           &pty, &best);
                if (np >= 0 && pty && pty[0] &&
                    (pty[0]->kind == TY_REF || pty[0]->kind == TY_RVALREF)) {
                    fputs("({ ", stdout);
                    emit_type(param_ty);
                    fputs(" __sf_cpy; ", stdout);
                    mangle_class_ctor(param_ty, pty, np);
                    fputs("(&__sf_cpy, &(", stdout);
                    emit_expr(arg);
                    fputs(")); __sf_cpy; })", stdout);
                    return;
                }
            }
        }
        emit_expr(arg);
        return;
    }
    Type *at = arg->resolved_type;
    /* Already a ref in the AST (lowered to pointer) — pass as-is. */
    if (ty_is_ref(at)) {
        bool saved = g_suppress_ref_deref;
        g_suppress_ref_deref = true;
        emit_expr(arg);
        g_suppress_ref_deref = saved;
        return;
    }
    /* Dereference cancellation: *X passed to T& → pass X directly */
    if (arg->kind == ND_UNARY && arg->unary.op == TK_STAR) {
        emit_expr(arg->unary.operand); return;
    }
    /* Non-lvalue rvalue passed to a reference param: '&(a + b)' is
     * illegal C. For scalar-type args (int, long, ptr, etc.), wrap
     * in a C99 compound literal '(T){expr}' which IS an lvalue and
     * has the block-scoped lifetime we need. Struct rvalues are
     * handled earlier via hoist_temps_in_expr's force-hoist; if we
     * get one here it means the hoist missed it — still wrap in
     * '&(...)' below which will error, giving a clear diagnostic.
     * Pattern: gcc 4.8 cfgexpand.c
     *   data->asan_vec.safe_push(offset + stack_vars[i].size);
     * N4659 §7.2.1 [basic.lval] / C11 §6.5.2.5 compound literals. */
    if (!is_addressable_lvalue(arg)) {
        /* Prefer the ref param's base type as the compound-literal
         * type. C++ binds 'const T &' to a converted-to-T temporary
         * (N4659 §11.6.3 [dcl.init.ref]/5.2), so the temp's type IS
         * T regardless of the arg's source type. Falling back to
         * arg's resolved_type emits e.g. '(int){'a'+i}' for an int
         * rvalue passed to 'const char &' — the resulting int* is
         * incompatible with the expected char*. Use param_ty->base
         * (== T) when the param is a ref; only fall back to arg's
         * type for non-ref params (struct-by-value via &(temp), etc.). */
        Type *lit_ty = ty_is_ref(param_ty) && param_ty->base
                         ? param_ty->base : at;
        if (lit_ty &&
            (lit_ty->kind == TY_INT || lit_ty->kind == TY_BOOL ||
             lit_ty->kind == TY_CHAR || lit_ty->kind == TY_SHORT ||
             lit_ty->kind == TY_LONG || lit_ty->kind == TY_LLONG ||
             lit_ty->kind == TY_PTR)) {
            fputs("&((", stdout);
            emit_type(lit_ty);
            fputs("){", stdout);
            emit_expr(arg);
            fputs("})", stdout);
            return;
        }
    }
    /* Type-mismatched lvalue passed to T& — C++ binds a const T& to a
     * temporary of T initialized from the converted arg (N4659
     * §11.6.3 [dcl.init.ref]/5.2). Without materializing the temp
     * here, we'd just emit &(arg) — yielding S* where the callee
     * expects T*, which the C compiler warns about and accepts as an
     * implicit-cast pointer. The runtime read inside the callee then
     * picks up sizeof(T) bytes from a sizeof(S) storage region:
     * silent partial-uninitialized read.
     *
     * Concrete: gcc 4.8 genautomata's
     *   static int undefined_vect_el_value;
     *   ...
     *   vla_hwint_t vect;       // vec<long>
     *   vect.safe_push(undefined_vect_el_value);
     * sea-front emitted &undefined_vect_el_value (int*) into a
     * safe_push expecting long*. quick_push read 8 bytes from the
     * 4-byte int — top half garbage. add_vect's gcc_assert(x >= 0)
     * fired on the garbage half; gen-tool aborted at runtime.
     *
     * Materialize a temp of the param's base type when arg's type
     * kind differs and both are scalar. C99 compound literal is an
     * lvalue with block-scoped lifetime — exactly what const T&
     * binding to a converted rvalue calls for. */
    if (at && param_ty->base && at->kind != param_ty->base->kind &&
        (param_ty->base->kind == TY_INT || param_ty->base->kind == TY_BOOL ||
         param_ty->base->kind == TY_CHAR || param_ty->base->kind == TY_SHORT ||
         param_ty->base->kind == TY_LONG || param_ty->base->kind == TY_LLONG) &&
        (at->kind == TY_INT || at->kind == TY_BOOL ||
         at->kind == TY_CHAR || at->kind == TY_SHORT ||
         at->kind == TY_LONG || at->kind == TY_LLONG)) {
        fputs("&((", stdout);
        emit_type(param_ty->base);
        fputs("){", stdout);
        emit_expr(arg);
        fputs("})", stdout);
        return;
    }
    /* Derived-to-base reference binding — N4659 §11.6.3 [dcl.init.ref]/4.1
     * + §7.11 [conv.ptr]. When 'd' is of derived type D and the param is
     * 'B&' where B is a base of D, C++ implicit conversion binds B& to
     * the B-subobject of d. In our sea-front layout the B-subobject lives
     * at d.__sf_base[N] for the Nth base, so we need '&(d).__sf_base[N]'
     * not the bare '&(d)' a generic lvalue→ref binding would emit.
     * Without this, the callee dereferences an offset of zero into D when
     * it expects the B layout — silent corruption / segv. */
    if (at && param_ty->base && at != param_ty->base &&
        (at->kind == TY_STRUCT || at->kind == TY_UNION) &&
        (param_ty->base->kind == TY_STRUCT || param_ty->base->kind == TY_UNION)) {
        /* Recover class_region if the Type copy lost it (typedef path,
         * subst_type result, etc.) — same fallback ND_MEMBER uses. */
        Type *deriv = at;
        if (!deriv->class_region && deriv->tag) {
            Node *d = find_class_def_by_tag_args(deriv);
            if (!d) d = find_class_def_by_tag_only(deriv);
            if (d && d->class_def.ty && d->class_def.ty->class_region)
                deriv = d->class_def.ty;
        }
        int base_path[8];
        int base_len = find_base_path(deriv, param_ty->base, base_path, 8);
        /* Type identity isn't canonical across all paths — D's stored
         * base-Type for B and the param's B-Type may be different
         * instances. Fall back to tag-based search when the pointer-
         * equality walk fails. */
        if (base_len == 0 && param_ty->base->tag) {
            Type *tgt = param_ty->base;
            int nb = class_nbases(deriv);
            for (int b = 0; b < nb; b++) {
                Type *base = class_base(deriv, b);
                if (!base || !base->tag) continue;
                if (base->tag->len == tgt->tag->len &&
                    memcmp(base->tag->loc, tgt->tag->loc,
                           tgt->tag->len) == 0) {
                    base_path[0] = b;
                    base_len = 1;
                    break;
                }
            }
        }
        if (base_len > 0) {
            fputs("&(", stdout);
            emit_expr(arg);
            fputs(").", stdout);
            if (base_len > 1)
                emit_base_chain(base_path, base_len - 1);
            int last = base_path[base_len - 1];
            if (last == 0) fputs("__sf_base", stdout);
            else           fprintf(stdout, "__sf_base%d", last);
            return;
        }
    }
    fputs("&(", stdout);
    emit_expr(arg);
    fputc(')', stdout);
}

/* Emit a return-expression with reference-return adaptation.
 *
 * In sea-front, T& and T&& parameters/returns are lowered to T* in C.
 * A 'return E;' from such a function must produce a T*, but C++ source
 * commonly returns a T-lvalue (e.g. 'return *this;' or 'return foo;'
 * where foo is a class object). The expression's resolved_type tells us
 * whether it's already a reference (so already a pointer in our C):
 *
 *   - resolved_type is TY_REF / TY_RVALREF → already lowered to a
 *     pointer; emit unchanged.
 *   - expression is '*X' for some pointer X → cancel the deref, emit X.
 *   - otherwise → take the address: '&(E)'. Works for any lvalue. */
static void emit_return_expr(Node *e) {
    Type *rt = g_current_func_ret_ty;
    /* 'return vNULL;' where the function returns a struct (vec<T,A,L>).
     * vNULL lowers to '{0}', valid in init-declarators only. For a
     * return expression we need a compound literal '(struct T){0}'.
     * Mirrors the ND_ASSIGN branch that does the same for 'x = vNULL'.
     * Pattern: gcc 4.8 dominance.c get_dominated_by '  return vNULL;' */
    if (e && e->kind == ND_IDENT && e->ident.name &&
        e->ident.name->len == 5 &&
        memcmp(e->ident.name->loc, "vNULL", 5) == 0 &&
        rt && (rt->kind == TY_STRUCT || rt->kind == TY_UNION) &&
        rt->tag) {
        fputc('(', stdout);
        emit_type(rt);
        fputs("){0}", stdout);
        return;
    }
    if (!ty_is_ref(rt)) { emit_expr(e); return; }
    Type *et = e ? e->resolved_type : NULL;
    if (ty_is_ref(et)) {
        emit_expr(e); return;
    }
    if (e->kind == ND_UNARY && e->unary.op == TK_STAR) {
        emit_expr(e->unary.operand); return;
    }
    /* ND_SUBSCRIPT on a class type gets rewritten to a method call
     * that returns T* (ref-return lowered). The result is already a
     * pointer — wrapping in '&' would be taking the address of a
     * temporary pointer, which is invalid. */
    if (e->kind == ND_SUBSCRIPT) {
        Type *base_ty = e->subscript.base ? e->subscript.base->resolved_type : NULL;
        if (base_ty && (base_ty->kind == TY_STRUCT || base_ty->kind == TY_UNION)) {
            emit_expr(e); return;
        }
    }
    /* ND_CALL whose callee is a method that returns a reference —
     * the codegen has already lowered the return type to T* in the
     * method's C signature. Pass through without '&'. */
    if (e->kind == ND_CALL) {
        Type *crt = e->resolved_type;
        if (crt && (crt->kind == TY_REF || crt->kind == TY_RVALREF ||
                    crt->kind == TY_PTR)) {
            emit_expr(e); return;
        }
    }
    fputs("&(", stdout);
    emit_expr(e);
    fputc(')', stdout);
}

/* Shared diagnostic for the "no matching overload" case. We fail
 * loudly rather than fall through to arg-type mangling: a fallback
 * that emits a best-guess symbol turns a sema-stage gap into a
 * link-time mystery, where the missing-symbol error tells you
 * nothing about which call site couldn't be resolved. Aborting
 * here puts the diagnostic at the right layer. */
static void die_no_overload(Type *class_type, Token *name, int nargs,
                             const char *where) {
    fprintf(stderr,
        "sea-front: no matching overload for %s on class %.*s "
        "(%d arg%s); called from %s\n",
        name ? "method" : "ctor",
        class_type && class_type->tag ? class_type->tag->len : 7,
        class_type && class_type->tag ? class_type->tag->loc : "unknown",
        nargs, nargs == 1 ? "" : "s", where);
    abort();
}

/*
 * Emit a "C++: ..." comment showing the original C++ declaration.
 * Extracts the source line from the Token's loc pointer (which points
 * into the source buffer). Prints from the start of the line up to
 * the first '{', ';', or end of line — just the declaration part.
 */
static void emit_source_comment(Token *tok) {
    if (!tok || !tok->loc) return;
    /* Walk backward to the start of the line */
    const char *line_start = tok->loc;
    while (line_start > tok->loc - 200 && line_start[-1] != '\n')
        line_start--;
    /* Skip leading whitespace */
    const char *p = line_start;
    while (*p == ' ' || *p == '\t') p++;
    int len = 0;
    while (p[len] && p[len] != '\n' && p[len] != '{' && len < 200)
        len++;
    /* Trim trailing whitespace */
    while (len > 0 && (p[len-1] == ' ' || p[len-1] == '\t'))
        len--;
    if (len <= 0) return;
    fputs("/* C++: ", stdout);
    /* Write the source line, escaping any embedded comment-close
     * sequences that would break the C comment. */
    for (int i = 0; i < len; i++) {
        if (p[i] == '*' && i + 1 < len && p[i + 1] == '/') {
            fputs("*\\/", stdout);
            i++;  /* skip the '/' */
        } else {
            fputc(p[i], stdout);
        }
    }
    fputs(" */\n", stdout);
    if (g_emit_line_directives && tok->line > 0) {
        /* Standard C #line directive: tells the C compiler that the
         * NEXT line came from the given source position. The file
         * name comes from the Token's file field if available,
         * otherwise we omit it (line-only form). */
        if (tok->file && tok->file->name)
            fprintf(stdout, "#line %d \"%s\"\n", tok->line,
                    tok->file->name);
        else
            fprintf(stdout, "#line %d\n", tok->line);
    }
}

/* Slice D-Hoist temp materialization.
 *
 * Walks an expression tree post-order, looking for ND_CALL nodes
 * whose return type is a class with a non-trivial dtor. For each
 * such call, emits a synthesized local declaration of the form
 *     struct T __SF_temp_<n> = <call>;
 * before the enclosing statement, tags the call node with its
 * temp name, and pushes the temp onto g_cf.live[] so its dtor
 * fires through the cleanup chain at end of block.
 *
 * The post-order walk means inner temps are hoisted first; by
 * the time we emit the outer temp's initializer the inner
 * sub-expressions already have codegen_temp_name set, so emit_expr
 * substitutes the local name verbatim.
 *
 * Lifetime divergence from C++: temps are scoped to the enclosing
 * BLOCK rather than to the full-expression. Their dtors fire in
 * the right relative order (most-recently-constructed first), but
 * later than the C++ standard requires. Observable only when a
 * temp's dtor has a side effect that the next user statement
 * reads back. Documented; mini-block isolation per full-expression
 * is a possible future refinement. */

/* True if 'n' is safe as a file-scope (static-storage) initializer
 * under C's constant-expression rules (C11 §6.7.9/4). Conservative:
 * accepts only literal shapes and address-of-globals; rejects calls,
 * non-literal idents (which are runtime loads in C), and other
 * computed expressions. Used to detect whether a file-scope ND_VAR_DECL
 * should keep its initializer or defer it to __sf_global_init. */
static bool is_const_initializer_for_static(Node *n) {
    if (!n) return true;
    switch (n->kind) {
    case ND_NUM:
    case ND_FNUM:
    case ND_STR:
    case ND_CHAR:
    case ND_BOOL_LIT:
    case ND_NULLPTR:
    case ND_SIZEOF:
    case ND_ALIGNOF:
    case ND_OFFSETOF:
        return true;
    case ND_INIT_LIST:
        for (int i = 0; i < n->init_list.nelems; i++)
            if (!is_const_initializer_for_static(n->init_list.elems[i]))
                return false;
        return true;
    case ND_UNARY:
        /* Address-of-global is a constant address. Treat '&ident'
         * conservatively as constant — most callers reference a
         * file-scope global, the cast-through-pointer case is what
         * we're trying to support. */
        if (n->unary.op == TK_AMP && n->unary.operand &&
            n->unary.operand->kind == ND_IDENT)
            return true;
        /* Unary plus / minus on a literal is also constant. */
        if ((n->unary.op == TK_PLUS || n->unary.op == TK_MINUS) &&
            n->unary.operand)
            return is_const_initializer_for_static(n->unary.operand);
        return false;
    case ND_BINARY:
        /* Pure arithmetic on constants. */
        return is_const_initializer_for_static(n->binary.lhs) &&
               is_const_initializer_for_static(n->binary.rhs);
    case ND_CAST:
        /* A cast is constant when its operand is. The (foo*)objPtr
         * case fails here because objPtr is ND_IDENT — not handled.
         * That's what routes the cast to __sf_global_init. */
        return is_const_initializer_for_static(n->cast.operand);
    default:
        return false;
    }
}

/* Conservative side-effect predicate for the C++17 sequencing
 * discipline (docs/sequencing.md). Returns true only for forms with
 * well-understood side effects: calls, pre/post inc/dec, nested
 * assignment, and any subexpression of those. Returns false for pure
 * value computations (idents, literals, arithmetic, member access,
 * subscript with pure index, etc.) so the seq-hoist path doesn't
 * fire on trivial assignments. */
static bool expr_has_side_effects(Node *n) {
    if (!n) return false;
    switch (n->kind) {
    case ND_CALL:
    case ND_ASSIGN:
    case ND_THROW:
        return true;
    case ND_UNARY:
    case ND_POSTFIX:
        if (n->unary.op == TK_INC || n->unary.op == TK_DEC) return true;
        return expr_has_side_effects(n->unary.operand);
    case ND_BINARY:
        return expr_has_side_effects(n->binary.lhs) ||
               expr_has_side_effects(n->binary.rhs);
    case ND_TERNARY:
        return expr_has_side_effects(n->ternary.cond) ||
               expr_has_side_effects(n->ternary.then_) ||
               expr_has_side_effects(n->ternary.else_);
    case ND_COMMA:
        return expr_has_side_effects(n->comma.lhs) ||
               expr_has_side_effects(n->comma.rhs);
    case ND_MEMBER:
        return expr_has_side_effects(n->member.obj);
    case ND_SUBSCRIPT:
        return expr_has_side_effects(n->subscript.base) ||
               expr_has_side_effects(n->subscript.index);
    case ND_CAST:
        return expr_has_side_effects(n->cast.operand);
    default:
        return false;
    }
}

/* Returns true if 'ty' is safely emittable as a C declaration prefix:
 * 'T name = init;'. False for compound declarator types that need
 * the name interleaved (function pointers, arrays). Used to gate the
 * seq-hoist away from shapes its simple prefix-form emit can't render. */
static bool type_simple_for_decl_prefix(Type *ty) {
    if (!ty) return false;
    if (ty->kind == TY_FUNC) return false;
    if (ty->kind == TY_ARRAY) return false;
    if (ty->kind == TY_PTR && ty->base &&
        (ty->base->kind == TY_FUNC || ty->base->kind == TY_ARRAY))
        return false;
    return true;
}

/* Hoist a scalar RHS to a fresh local so its evaluation completes
 * before the surrounding statement's LHS is computed. C++17 §8.18/1
 * (RHS-before-LHS for '='); C leaves the order unspecified.
 * Skips when the node is already hoisted, has a non-scalar type, or
 * lacks a resolved type. See docs/sequencing.md. */
/* Allocate '__SF_<tag>_<id>' in malloc'd storage. Used for names
 * borrowed by Node->codegen_temp_name; the borrow's lifetime is the
 * rest of the emit, so a per-call malloc (one-shot process, never
 * freed) is the right shape. Names must remain unique within the
 * TU: reusing a slot would let one temp's name substitute for
 * another's emit. */
static char *make_codegen_temp_name(const char *tag, int id) {
    enum { CAP = 32 };
    char *name = malloc(CAP);
    if (!name) abort();
    snprintf(name, CAP, "__SF_%s_%d", tag, id);
    return name;
}

static void hoist_seq_scalar(Node *rhs) {
    if (!rhs || rhs->codegen_temp_name) return;
    Type *ty = rhs->resolved_type;
    if (!ty) return;
    if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) return;
    if (ty->kind == TY_VOID) return;
    if (!type_simple_for_decl_prefix(ty)) return;

    int id = g_cf.next_label_id++;
    char *name = make_codegen_temp_name("seq", id);

    emit_indent();
    emit_type(ty);
    fprintf(stdout, " %s = ", name);
    emit_expr(rhs);
    fputs(";\n", stdout);
    rhs->codegen_temp_name = name;
}

static bool is_class_temp_call(Node *n) {
    if (!n || n->kind != ND_CALL || n->codegen_temp_name) return false;
    /* Self-class value-construction inside the same class body —
     * 'static T factory() { return T(args); }'. Sema's lookup of T
     * inside T's class scope can resolve to one of the ctors
     * (entity=VARIABLE, return-type void) instead of the class tag,
     * giving a TY_VOID resolved_type that the regular
     * resolved_type->kind==TY_STRUCT check below rejects. The call
     * IS a class-temp construction; force-recognise it via the
     * callee-name == enclosing-class-tag match. Real-world hit:
     * gcc 14 libcpp/include/rich-location.h's label_text class with
     * static borrow() / take() factories returning 'label_text(...)'.
     * N4659 §6.4.1 [basic.lookup.unqual] really wants type-names to
     * win this lookup, but our lookup doesn't do that ordering yet
     * — patch up at codegen instead.
     * TODO(seafront#class-name-lookup): fix sema's lookup ordering
     * so 'T(args)' inside T's body resolves to T-the-tag rather than
     * one of T's ctors. Then this codegen patch becomes redundant. */
    if (g_current_method_class && g_current_method_class->tag &&
        n->call.callee && n->call.callee->kind == ND_IDENT &&
        n->call.callee->ident.name &&
        tokens_equal(n->call.callee->ident.name, g_current_method_class->tag))
        return true;
    if (!n->resolved_type || n->resolved_type->kind != TY_STRUCT)
        return false;
    /* Standard case: class-typed call with non-trivial dtor needs
     * hoisting so the dtor fires at the right scope. */
    if (n->resolved_type->has_dtor) return true;
    /* Ctor-call shape: 'Foo(args)' where the callee is a type-name
     * needs hoisting even when Foo has no dtor — there's no Foo()
     * function in the lowered C, only the mangled
     * sf__Foo__ctor(struct sf__Foo *, ...). The temp gets a slot
     * but no cleanup-chain registration. */
    if (n->call.callee && n->call.callee->kind == ND_IDENT) {
        Declaration *d = n->call.callee->ident.resolved_decl;
        if (d && (d->entity == ENTITY_TYPE || d->entity == ENTITY_TAG))
            return true;
    }
    return false;
}

/* Emit '&<expr>' for implicit-this binding, with one piece of cleverness:
 * if <expr> is a deferred-assign hoist (codegen_temp_pending_assign),
 * inline the assignment into the comma operator —
 *   (name = call(), &name)
 * — so the call evaluates only when the surrounding short-circuit /
 * conditional context actually reaches the use site, AND the result is
 * a true address (not '&(comma)' which is invalid in ISO C). */
static void emit_addrof_for_this(Node *n) {
    if (n && n->codegen_temp_name && n->codegen_temp_pending_assign) {
        const char *name = n->codegen_temp_name;
        n->codegen_temp_pending_assign = false;
        n->codegen_temp_name = NULL;
        fprintf(stdout, "(%s = ", name);
        emit_expr(n);
        fprintf(stdout, ", &%s)", name);
        n->codegen_temp_name = name;
        return;
    }
    fputc('&', stdout);
    emit_expr(n);
}

static void hoist_emit_decl(Node *call, bool in_shortcircuit) {
    int id = g_cf.next_label_id++;
    char *name = make_codegen_temp_name("temp", id);

    /* ND_BINARY / ND_UNARY as the hoist target: operator overloads
     * on struct values. Their resolved_type is often NULL (sema's
     * common_arith_type returns NULL for struct operands) so derive
     * the temp's type from an operand. Emit as direct-copy form —
     * the underlying operator becomes a mangled free-fn call that
     * returns the struct by value, and the temp catches the result. */
    if (call->kind == ND_BINARY || call->kind == ND_UNARY ||
        call->kind == ND_POSTFIX || call->kind == ND_TERNARY) {
        Type *ty = call->resolved_type;
        if (!ty) {
            Node *operand = NULL;
            if (call->kind == ND_BINARY) operand = call->binary.lhs;
            else if (call->kind == ND_TERNARY) operand = call->ternary.then_;
            else operand = call->unary.operand;
            if (operand) ty = operand->resolved_type;
            if (!ty && call->kind == ND_TERNARY && call->ternary.else_)
                ty = call->ternary.else_->resolved_type;
        }
        /* Bail WITHOUT setting codegen_temp_name if we can't
         * determine the temp's type — otherwise we'd leave a
         * dangling tag that emit_expr substitutes with an
         * undeclared name. */
        if (!ty || (ty->kind != TY_STRUCT && ty->kind != TY_UNION))
            return;
        emit_indent();
        emit_type(ty);
        fprintf(stdout, " %s = ", name);
        emit_expr(call);
        fputs(";\n", stdout);
        call->codegen_temp_name = name;
        return;
    }

    /* Detect ctor-call shape: callee is an ND_IDENT whose
     * resolved_decl is a type-name (ENTITY_TYPE). For these we
     * emit the two-line construction form
     *     struct sf__T __SF_temp_<n>;
     *     sf__T__ctor(&__SF_temp_<n>, args);
     * instead of the single-line assignment-init form
     *     struct sf__T __SF_temp_<n> = some_call();
     * because Foo(args) isn't a function call in C — there's no
     * symbol named 'Foo' (only the mangled sf__Foo__ctor). */
    bool is_ctor_call = false;
    Type *ctor_class_type = call->resolved_type;
    if (call->call.callee && call->call.callee->kind == ND_IDENT) {
        Declaration *d = call->call.callee->ident.resolved_decl;
        if (d && (d->entity == ENTITY_TYPE || d->entity == ENTITY_TAG))
            is_ctor_call = true;
        /* Self-class value-construction inside the same class —
         * sema's lookup may have resolved the callee to a ctor
         * (entity=VARIABLE, return-type void), so the entity check
         * above misses it. The is_class_temp_call gate already
         * promoted this to a hoist; mirror the recognition here so
         * we emit as ctor call (not as 'void temp;' followed by a
         * void function call). Type comes from the enclosing class.
         * See is_class_temp_call's comment for the libcpp pattern. */
        if (!is_ctor_call && g_current_method_class &&
            g_current_method_class->tag &&
            call->call.callee->ident.name &&
            tokens_equal(call->call.callee->ident.name,
                         g_current_method_class->tag)) {
            is_ctor_call = true;
            ctor_class_type = g_current_method_class;
        }
    }

    if (is_ctor_call) {
        Node *resolved_ctor = NULL;
        int np = 0;
        Type **pty = NULL;
        Type **at = NULL;
        int na = collect_call_arg_types(call->call.args,
                                         call->call.nargs, &at);
        np = resolve_overload(ctor_class_type, /*name=*/NULL,
                               /*is_ctor=*/true,
                               at, na,
                               /*receiver_is_const=*/false,
                               &pty, /*out_best=*/&resolved_ctor);
        emit_indent();
        emit_type(ctor_class_type);
        if (np < 0) {
            /* No user-declared matching ctor — trivial default
             * construction per N4659 §11.6/8 [dcl.init]. If the class
             * has a synthesised default ctor (has_default_ctor —
             * needed e.g. when a member has its own ctor that must
             * run), call it; the synth is emitted with the same
             * mangled name as a user-defined zero-arg ctor. Pattern:
             * g++.dg/init/elide2.C — `struct B { A a; };` where A
             * has a user ctor that ++c's; bare `{0}` would leave c=0.
             *
             * If no synth either, zero-init via `{0}` so the bytes
             * are well-defined; bare `struct T temp;` left ptmf
             * members and padding indeterminate
             * (g++.dg/init/ptrmem4.C bug). */
            if (call->call.nargs == 0 && ctor_class_type &&
                ctor_class_type->has_default_ctor) {
                fprintf(stdout, " %s;\n", name);
                emit_indent();
                mangle_class_ctor(ctor_class_type, NULL, 0);
                fprintf(stdout, "(&%s);\n", name);
            } else {
                fprintf(stdout, " %s = {0};\n", name);
            }
            goto hoist_done;
        }
        fprintf(stdout, " %s;\n", name);
        emit_indent();
        mangle_class_ctor(ctor_class_type, pty, np);
        fprintf(stdout, "(&%s", name);
        for (int i = 0; i < call->call.nargs; i++) {
            fputs(", ", stdout);
            emit_expr(call->call.args[i]);
        }
        emit_default_args_tail(resolved_ctor, call->call.nargs, np, pty);
        fputs(");\n", stdout);
    } else {
        /* Function call returning a class — direct copy form.
         * If we're hoisting INSIDE a short-circuit / conditional
         * operand (in_shortcircuit), defer the assignment to the use
         * site so the call only runs when the gate actually reaches
         * it. ISO C comma at the use; no GNU stmt-expr dependency.
         * Pattern: gcc 4.8 cgraph.c
         *   gcc_checking_assert(!virtual_offset
         *                       || tree_to_double_int(virtual_offset)...)
         * — eager hoist crashed cc1plus on NULL virtual_offset. */
        if (in_shortcircuit) {
            emit_indent();
            emit_type(call->resolved_type);
            fprintf(stdout, " %s;\n", name);
            call->codegen_temp_pending_assign = true;
        } else {
            emit_indent();
            emit_type(call->resolved_type);
            fprintf(stdout, " %s = ", name);
            emit_expr(call);
            fputs(";\n", stdout);
        }
    }

hoist_done:
    /* Tag the call so emit_expr now substitutes the temp name. */
    call->codegen_temp_name = name;

    /* Push as a CL_VAR onto the live stack so the cleanup chain
     * runs the dtor at end of block. Skip the push for trivially-
     * destructible types (no dtor to call) — the temp is still a
     * named local that the rest of the expression uses, but it
     * doesn't need cleanup tracking. */
    if (call->resolved_type && call->resolved_type->has_dtor) {
        cleanup_live_reserve(1);
        g_cf.live[g_cf.nlive].kind = CL_VAR;
        g_cf.live[g_cf.nlive].label_id = id;
        g_cf.live[g_cf.nlive].cont_label_id = -1;
        g_cf.live[g_cf.nlive].var_decl = call;  /* not an ND_VAR_DECL — flagged via codegen_temp_name */
        g_cf.nlive++;
    }
}

static void hoist_temps_in_expr(Node *n, bool in_shortcircuit) {
    if (!n) return;
    switch (n->kind) {
    case ND_CALL:
        if (n->call.callee) hoist_temps_in_expr(n->call.callee, in_shortcircuit);
        for (int i = 0; i < n->call.nargs; i++)
            hoist_temps_in_expr(n->call.args[i], in_shortcircuit);
        /* 'obj(args)' where obj is a class value with operator() →
         * dispatch will take '&obj'. If obj is an rvalue (call,
         * operator overload result), hoist it first. Pattern: gcc
         * 4.8 expr.c '(*genfun)(to1, from1)' — '*genfun' is operator*
         * returning insn_gen_fn; then (...)(args) is operator(). */
        if (n->call.callee && !n->call.callee->codegen_temp_name) {
            Node *callee_ = n->call.callee;
            Type *cty = callee_->resolved_type;
            if (ty_is_ref(cty)) cty = cty->base;
            if (cty && (cty->kind == TY_STRUCT || cty->kind == TY_UNION) &&
                cty->tag) {
                NodeKind ck = callee_->kind;
                if (ck == ND_CALL ||
                    (ck == ND_UNARY && callee_->unary.op != TK_STAR &&
                     callee_->unary.operand &&
                     callee_->unary.operand->resolved_type &&
                     (callee_->unary.operand->resolved_type->kind == TY_STRUCT ||
                      callee_->unary.operand->resolved_type->kind == TY_UNION)) ||
                    (ck == ND_UNARY && callee_->unary.op == TK_STAR &&
                     callee_->unary.operand &&
                     callee_->unary.operand->resolved_type &&
                     (callee_->unary.operand->resolved_type->kind == TY_STRUCT ||
                      callee_->unary.operand->resolved_type->kind == TY_UNION)))
                    hoist_emit_decl(callee_, in_shortcircuit);
            }
        }
        /* Arg passed to a reference parameter is lowered to '&(arg)'.
         * If the arg is itself an rvalue call, '&f()' is illegal C.
         * Force-hoist the arg call so it lands in a named local.
         * Works for any element type — hoist_emit_decl prints
         * 'T __tmp = call();' and tags the call for emit_expr to
         * substitute the temp name. Pattern: gcc 4.8 cgraphunit.c
         *   vargs.quick_push(thunk_adjust(&bsi, a, 1, ...));
         * — thunk_adjust returns tree (pointer); quick_push's 2nd
         * param is T& (→ tree*&). N4659 §11.3.2 [dcl.ref]. */
        {
            Type *callee_ty = n->call.callee ? n->call.callee->resolved_type : NULL;
            /* Function pointers have TY_PTR(TY_FUNC); peel the pointer. */
            if (callee_ty && callee_ty->kind == TY_PTR && callee_ty->base)
                callee_ty = callee_ty->base;
            bool is_method_call = n->call.callee &&
                                   n->call.callee->kind == ND_MEMBER;
            bool have_free_fn_types = callee_ty && callee_ty->kind == TY_FUNC &&
                                       callee_ty->nparams == n->call.nargs;
            for (int i = 0; i < n->call.nargs; i++) {
                Node *arg = n->call.args[i];
                if (!arg || arg->kind != ND_CALL ||
                    arg->codegen_temp_name || !arg->resolved_type ||
                    ty_is_ref(arg->resolved_type))
                    continue;
                bool pass_by_ref;
                if (have_free_fn_types) {
                    /* Free function with arity match: only hoist when
                     * the specific param is TY_REF. */
                    pass_by_ref = ty_is_ref(callee_ty->params[i]);
                } else if (is_method_call) {
                    /* Method call: we don't have callee_ty's params
                     * here (resolution lives at emit time), so we
                     * can't tell which params are ref. Force-hoist
                     * any call-valued arg — possible extra temp is
                     * harmless; missing one yields invalid '&f()'. */
                    pass_by_ref = true;
                } else {
                    pass_by_ref = false;
                }
                if (pass_by_ref) hoist_emit_decl(arg, in_shortcircuit);
            }
        }
        if (is_class_temp_call(n)) hoist_emit_decl(n, in_shortcircuit);
        return;
    case ND_BINARY:
    case ND_ASSIGN: {
        /* Both share the 'binary' member layout.
         * For '&&' and '||' the RHS is conditionally evaluated — any
         * hoist inside it must defer the assignment to the use site
         * (else the call runs unconditionally and breaks the gate).
         * The LHS always evaluates, so it inherits the outer context. */
        bool rhs_short = in_shortcircuit ||
            (n->kind == ND_BINARY &&
             (n->binary.op == TK_LAND || n->binary.op == TK_LOR));
        hoist_temps_in_expr(n->binary.lhs, in_shortcircuit);
        hoist_temps_in_expr(n->binary.rhs, rhs_short);
        /* Struct-typed lhs of a binary/compound-assign op dispatches
         * through an overloaded operator: 'a == b' → 'sf__T__eq(&a, b)'.
         * If lhs is an rvalue call 'f() == b', the emitted '&f()' is
         * illegal C. Force-hoist same as the ND_MEMBER case below.
         * Pattern: gcc 4.8 cgraph.c cgraph_add_thunk
         *   tree_to_double_int(virtual_offset) == double_int::from_shwi(...) */
        /* C++17 sequencing: ND_ASSIGN is RHS-before-LHS (N4659 §8.18/1),
         * but C leaves the order unspecified. Hoist RHS into a fresh
         * local so RHS is sequenced first by statement order, whenever:
         *   - RHS has side effects, AND
         *   - LHS is not a bare-ident write ('x = ...') —
         *     i.e. LHS performs any non-trivial value computation
         *     (subscript, member, deref, ternary, etc.) that could
         *     read memory the RHS's side effects might disturb.
         * Scalar RHS only — class RHS goes through the existing struct-
         * temp / by-ref-arg paths. See docs/sequencing.md. */
        if (n->kind == ND_ASSIGN && !in_shortcircuit &&
            n->binary.lhs && n->binary.rhs &&
            !n->binary.rhs->codegen_temp_name &&
            n->binary.lhs->kind != ND_IDENT &&
            expr_has_side_effects(n->binary.rhs)) {
            hoist_seq_scalar(n->binary.rhs);
        }
        if (n->binary.lhs && !n->binary.lhs->codegen_temp_name) {
            Node *lhs = n->binary.lhs;
            NodeKind lk = lhs->kind;
            /* Struct-returning call LHS — hoist for the '&lhs' emit. */
            if (lk == ND_CALL && lhs->resolved_type &&
                (lhs->resolved_type->kind == TY_STRUCT ||
                 lhs->resolved_type->kind == TY_UNION))
                hoist_emit_decl(lhs, in_shortcircuit);
            /* Nested operator-overload LHS '(a op1 b) op2 c' where op1
             * returns struct — same issue. Skip plain pointer deref
             * '*p' (lvalue, safe to address without hoist). */
            else if (lk == ND_BINARY) {
                Type *op_ty = lhs->binary.lhs ?
                    lhs->binary.lhs->resolved_type : NULL;
                if (op_ty && (op_ty->kind == TY_STRUCT ||
                               op_ty->kind == TY_UNION))
                    hoist_emit_decl(lhs, in_shortcircuit);
            } else if ((lk == ND_UNARY || lk == ND_POSTFIX) &&
                        lhs->unary.op != TK_STAR) {
                Type *op_ty = lhs->unary.operand ?
                    lhs->unary.operand->resolved_type : NULL;
                if (op_ty && (op_ty->kind == TY_STRUCT ||
                               op_ty->kind == TY_UNION))
                    hoist_emit_decl(lhs, in_shortcircuit);
            }
        }
        return;
    }
    case ND_UNARY:
    case ND_POSTFIX:
        /* Both share the 'unary' member layout. */
        hoist_temps_in_expr(n->unary.operand, in_shortcircuit);
        /* Unary overload on a struct operand: '-x' → 'sf__T__minus(&x)'.
         * Force-hoist rvalue struct-returning operand so the emitted
         * '&f()' stays valid. N4659 §16.5 [over.oper]. */
        if (n->unary.operand && !n->unary.operand->codegen_temp_name) {
            Node *op = n->unary.operand;
            NodeKind ok = op->kind;
            if (ok == ND_CALL && op->resolved_type &&
                (op->resolved_type->kind == TY_STRUCT ||
                 op->resolved_type->kind == TY_UNION))
                hoist_emit_decl(op, in_shortcircuit);
            /* Nested struct-operator '~(a op b)': infer from operand's
             * own lhs since sema leaves the binary's resolved_type NULL
             * for struct operands. Pattern: gcc 4.8 tree-vrp.c
             * 'complement = ~(bound - double_int_one);'. */
            else if (ok == ND_BINARY) {
                Type *sub_ty = op->binary.lhs ?
                    op->binary.lhs->resolved_type : NULL;
                if (sub_ty && (sub_ty->kind == TY_STRUCT ||
                                sub_ty->kind == TY_UNION))
                    hoist_emit_decl(op, in_shortcircuit);
            }
        }
        return;
    case ND_TERNARY:
        /* The two arms are conditionally evaluated — propagate the
         * defer-assignment flag so any hoist inside them defers. */
        hoist_temps_in_expr(n->ternary.cond, in_shortcircuit);
        hoist_temps_in_expr(n->ternary.then_, /*in_shortcircuit=*/true);
        hoist_temps_in_expr(n->ternary.else_, /*in_shortcircuit=*/true);
        return;
    case ND_MEMBER:
        hoist_temps_in_expr(n->member.obj, in_shortcircuit);
        /* Method dispatch needs an lvalue for '&obj'. If the obj is
         * an rvalue struct-returning call that wasn't already hoisted
         * (e.g. no dtor), force-hoist so '&__SF_temp_N' is valid C.
         * Pattern: 'make_value().method()' or
         * 'double_int::from_shwi(1).lshift(...)'.
         * N4659 §16.3.1.4 [over.match.funcs] — implicit this must
         * bind to an lvalue that the callee can take the address of. */
        if (n->member.obj && !n->member.obj->codegen_temp_name) {
            Node *obj = n->member.obj;
            NodeKind k = obj->kind;
            /* Struct-returning call result — hoist to a named temp so
             * '&obj' works for the implicit-this arg. */
            if (k == ND_CALL && obj->resolved_type &&
                (obj->resolved_type->kind == TY_STRUCT ||
                 obj->resolved_type->kind == TY_UNION))
                hoist_emit_decl(obj, in_shortcircuit);
            /* Operator-overload rvalue: '(a op b).method()' where 'op'
             * is an overloaded binary/unary on struct operands. sema's
             * resolved_type is often NULL for this; infer from an
             * operand. Pattern: gcc 4.8 gimple-fold.c '(a - b).sext(p)'.
             * Exclude pointer dereference: '(*p).method()' has an
             * lvalue receiver — '&(*p) == p' — and force-hoisting copies
             * the entire struct via a stack temp. For vec<T,A,vl_embed>
             * (which is a flexible-array-style container) the copy
             * truncates the trailing data, so iterate(&copy, i, ...)
             * reads garbage past the first element. N4659 §8.2.5
             * [expr.ref] / §8.3.1 [expr.unary.op]. Pattern: gcc 4.8
             * 'FOR_EACH_VEC_ELT (*level->class_shadowed, i, cb)'. */
            else if (k == ND_BINARY || k == ND_UNARY || k == ND_POSTFIX) {
                bool is_deref = (k == ND_UNARY && obj->unary.op == TK_STAR);
                if (!is_deref) {
                    Node *op = (k == ND_BINARY) ? obj->binary.lhs
                                                : obj->unary.operand;
                    Type *ty = obj->resolved_type;
                    if (!ty && op) ty = op->resolved_type;
                    if (ty && (ty->kind == TY_STRUCT || ty->kind == TY_UNION))
                        hoist_emit_decl(obj, in_shortcircuit);
                }
            }
            /* Ternary rvalue: '(c ? a : b).method()' where both
             * branches return struct. Pattern: gcc 4.8 tree-ssa-ccp.c
             * '(cond ? mask(prec) : from_shwi(-1)).and_not(...)'. */
            else if (k == ND_TERNARY) {
                Type *ty = obj->resolved_type;
                if (!ty && obj->ternary.then_)
                    ty = obj->ternary.then_->resolved_type;
                if (!ty && obj->ternary.else_)
                    ty = obj->ternary.else_->resolved_type;
                if (ty && (ty->kind == TY_STRUCT || ty->kind == TY_UNION))
                    hoist_emit_decl(obj, in_shortcircuit);
            }
        }
        return;
    case ND_SUBSCRIPT:
        hoist_temps_in_expr(n->subscript.base, in_shortcircuit);
        hoist_temps_in_expr(n->subscript.index, in_shortcircuit);
        /* Class-type subscript lowers to 'sf__T__subscript(&base, idx)'.
         * Force-hoist an rvalue struct-returning call base so '&f()'
         * stays valid. N4659 §16.5.5 [over.sub]. */
        if (n->subscript.base && n->subscript.base->kind == ND_CALL &&
            !n->subscript.base->codegen_temp_name &&
            n->subscript.base->resolved_type &&
            (n->subscript.base->resolved_type->kind == TY_STRUCT ||
             n->subscript.base->resolved_type->kind == TY_UNION))
            hoist_emit_decl(n->subscript.base, in_shortcircuit);
        return;
    case ND_CAST:
        hoist_temps_in_expr(n->cast.operand, in_shortcircuit);
        return;
    case ND_COMMA:
        hoist_temps_in_expr(n->comma.lhs, in_shortcircuit);
        hoist_temps_in_expr(n->comma.rhs, in_shortcircuit);
        return;
    default:
        return;
    }
}

/* Hoist any class temps from the expressions belonging to a single
 * statement. Called by emit_block before emit_indent for each
 * statement, so synthesized declarations land at the same indent
 * level as the user statement.
 *
 * Special case: 'T x = make();' is direct-init from a class call
 * — the call's return value IS x via struct copy in C, no separate
 * temp needed. We skip hoisting the top-level call but still
 * recurse into its arguments (which may carry their own temps). */
static void hoist_stmt_temps(Node *s) {
    if (!s) return;
    /* Note: we deliberately do NOT gate on func_has_cleanups here.
     * Even functions without any non-trivial dtors may contain
     * ctor-call expressions ('Foo(7)') that need to be materialized
     * to a synthesized local — the lowered C has no symbol named
     * 'Foo', only the mangled sf__Foo__ctor. The hoist fires; the
     * cleanup-chain registration in hoist_emit_decl is what's
     * gated on has_dtor, so a trivially-destructible temp gets a
     * local but no CL_VAR entry. */
    switch (s->kind) {
    case ND_VAR_DECL: {
        Node *init = s->var_decl.init;
        if (!init) return;
        bool direct_init =
            init->kind == ND_CALL && init->resolved_type &&
            init->resolved_type->kind == TY_STRUCT &&
            s->var_decl.ty && s->var_decl.ty->kind == TY_STRUCT;
        if (direct_init) {
            /* Recurse into the call's children only — don't hoist
             * the call itself. */
            if (init->call.callee) hoist_temps_in_expr(init->call.callee, /*in_shortcircuit=*/false);
            for (int i = 0; i < init->call.nargs; i++)
                hoist_temps_in_expr(init->call.args[i], /*in_shortcircuit=*/false);
        } else {
            hoist_temps_in_expr(init, /*in_shortcircuit=*/false);
        }
        return;
    }
    case ND_EXPR_STMT:
        hoist_temps_in_expr(s->expr_stmt.expr, /*in_shortcircuit=*/false);
        return;
    case ND_RETURN:
        hoist_temps_in_expr(s->ret.expr, /*in_shortcircuit=*/false);
        return;
    case ND_CASE:
    case ND_DEFAULT:
    case ND_LABEL:
        /* Don't hoist temps from inside a labeled statement to before
         * its label. C99 §6.8.6 [stmt.jump] / C11 §6.8.6.4 — control
         * jumping to a label skips over any intervening statements,
         * so a temp declaration emitted *before* the label has its
         * initializer never executed when the switch jumps directly
         * to that case (the temp's storage exists for the whole
         * enclosing block, but its value is indeterminate). emit_stmt
         * for ND_CASE/ND_DEFAULT wraps the case body in a brace-
         * block instead, and hoist runs *inside* that block — every
         * jump to the case label flows into the brace block, so the
         * temp's initializer always executes before any use.
         *
         * Was: 'hoist_stmt_temps(s->case_.stmt)' here, which emitted
         * the temp decl at the OUTER (switch-body) scope, past the
         * case label. gcc 4.8 genextract.c walk_rtx then read
         * uninitialised stack memory through the temp — the path
         * pushed into oplocs/duplocs ended up pointing into the
         * .text section, gen-tool aborted in print_path on the
         * machine-code bytes that came back. */
        return;
    /* ND_IF/WHILE/DO/FOR conditions are NOT hoisted at the outer
     * block level — that would put the temp in extended-lifetime
     * scope, observing the wrong dtor timing. Instead, the
     * per-statement handler in emit_stmt does a structural rewrite:
     * synth int outside, mini-block around the cond evaluation,
     * temp dtors fire before the branch runs. See ND_IF case in
     * emit_stmt. */
    default:
        return;
    }
}

/* Are we currently inside any enclosing loop? Used to decide
 * whether ND_BREAK/CONTINUE need the cleanup-aware rewrite or
 * can be lowered as a plain C break/continue. */
static bool break_needs_cleanup(void) {
    /* If the topmost live entry is a CL_LOOP marker, no cleanup
     * vars are in flight inside the current loop and a plain C
     * 'break' will exit the loop correctly. Otherwise we have at
     * least one var to destroy first. */
    if (!g_cf.func_has_cleanups) return false;
    if (g_cf.nlive == 0) return false;
    return g_cf.live[g_cf.nlive - 1].kind == CL_VAR;
}

/* Pre-scan: does any block in this subtree declare a class instance
 * with a non-trivial dtor, OR contain an expression that creates a
 * class temporary (call returning class with has_dtor)? Either case
 * needs the cleanup machinery (locals __retval/__unwind, __epilogue
 * label, return rewrite, per-block cleanup chain). */
static bool subtree_has_cleanups(Node *n) {
    if (!n) return false;
    /* First: a call returning a non-trivially destructible class
     * is a temporary site that Slice D-Hoist will materialize. */
    if (n->kind == ND_CALL && n->resolved_type &&
        n->resolved_type->kind == TY_STRUCT && n->resolved_type->has_dtor)
        return true;
    switch (n->kind) {
    case ND_VAR_DECL:
        if (n->var_decl.ty && n->var_decl.ty->kind == TY_STRUCT &&
            n->var_decl.ty->has_dtor) return true;
        /* Array of class-with-dtor — element dtors must fire at
         * scope exit. push_user_var_cleanup emits a destruct-each-
         * element loop. Pattern: g++.dg/init/array14.C. */
        if (n->var_decl.ty && n->var_decl.ty->kind == TY_ARRAY &&
            n->var_decl.ty->base &&
            n->var_decl.ty->base->kind == TY_STRUCT &&
            n->var_decl.ty->base->has_dtor) return true;
        return subtree_has_cleanups(n->var_decl.init);
    case ND_BLOCK:
        for (int i = 0; i < n->block.nstmts; i++)
            if (subtree_has_cleanups(n->block.stmts[i])) return true;
        return false;
    case ND_IF:
        return subtree_has_cleanups(n->if_.cond) ||
               subtree_has_cleanups(n->if_.then_) ||
               subtree_has_cleanups(n->if_.else_);
    case ND_WHILE:
        return subtree_has_cleanups(n->while_.cond) ||
               subtree_has_cleanups(n->while_.body);
    case ND_DO:
        return subtree_has_cleanups(n->do_.cond) ||
               subtree_has_cleanups(n->do_.body);
    case ND_FOR:
        return subtree_has_cleanups(n->for_.init) ||
               subtree_has_cleanups(n->for_.cond) ||
               subtree_has_cleanups(n->for_.inc) ||
               subtree_has_cleanups(n->for_.body);
    case ND_RANGE_FOR:
        return subtree_has_cleanups(n->range_for.range) ||
               subtree_has_cleanups(n->range_for.body);
    case ND_RETURN:
        return subtree_has_cleanups(n->ret.expr);
    case ND_EXPR_STMT:
        return subtree_has_cleanups(n->expr_stmt.expr);
    /* Expression nodes — recurse into children. */
    case ND_BINARY:
    case ND_ASSIGN:
        return subtree_has_cleanups(n->binary.lhs) ||
               subtree_has_cleanups(n->binary.rhs);
    case ND_UNARY:
    case ND_POSTFIX:
        return subtree_has_cleanups(n->unary.operand);
    case ND_TERNARY:
        return subtree_has_cleanups(n->ternary.cond) ||
               subtree_has_cleanups(n->ternary.then_) ||
               subtree_has_cleanups(n->ternary.else_);
    case ND_CALL:
        if (subtree_has_cleanups(n->call.callee)) return true;
        for (int i = 0; i < n->call.nargs; i++)
            if (subtree_has_cleanups(n->call.args[i])) return true;
        return false;
    case ND_MEMBER:
        return subtree_has_cleanups(n->member.obj);
    case ND_SUBSCRIPT:
        return subtree_has_cleanups(n->subscript.base) ||
               subtree_has_cleanups(n->subscript.index);
    case ND_CAST:
        return subtree_has_cleanups(n->cast.operand);
    case ND_COMMA:
        return subtree_has_cleanups(n->comma.lhs) ||
               subtree_has_cleanups(n->comma.rhs);
    /* Exception machinery — a function containing a 'throw' or a
     * 'try' needs the __SF_unwind / __SF_retval / __SF_epilogue
     * prologue scaffolding because the throw sets __SF_unwind and
     * jumps to the epilogue (which may chain through dtor labels)
     * — N4659 §18 [except] + docs/exceptions.md. Without this, the
     * function emits no prologue and the goto target does not exist. */
    case ND_THROW:
        return true;
    case ND_TRY:
        return true;
    /* Labelled statement: walk into the wrapped stmt. Without this a
     * labelled class-typed var-decl ('again: C v;') doesn't get its
     * containing function flagged as func_has_cleanups, so v's
     * cleanup never fires when a goto crosses it. Pattern:
     * g++.dg/init/goto1.C. */
    case ND_LABEL:
        return subtree_has_cleanups(n->label.stmt);
    case ND_CASE:
        return subtree_has_cleanups(n->case_.stmt);
    case ND_DEFAULT:
        return subtree_has_cleanups(n->default_.stmt);
    case ND_SWITCH:
        return subtree_has_cleanups(n->switch_.expr) ||
               subtree_has_cleanups(n->switch_.body);
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* Name mangling                                                      */
/* ------------------------------------------------------------------ */

/* Emit the mangled struct/class tag for a class type via the
 * Mangler framework (see docs/mangling.md and mangle.c). With the
 * default human scheme this produces 'sf__<NS>__<Class>' where
 * the namespace prefix walks the class's enclosing chain. The
 * '?' fallback is for class types whose tag wasn't recorded
 * (anonymous structs, currently). */
static int g_anon_counter = 0;

/* Dedup for free-function declarations and definitions.
 * C doesn't support overloading (§6.2.1 identifier spaces): multiple
 * same-named declarations with different signatures (const/non-const
 * strchr, abs(int)/abs(long), div variants, etc.) redeclare the same
 * linker symbol. Likewise header-inline definitions may be included
 * multiple times after preprocessing.
 *
 * Policy:
 *   - FUNC_DECL: skip if the name has already been seen (as decl or
 *     def). Otherwise emit and record as decl.
 *   - FUNC_DEF: skip if the name has already been seen as DEF.
 *     Otherwise emit and mark as def — a decl upgrading to a def is
 *     the common pattern
 *         static void do_define(cpp_reader *);      // decl
 *         ... more code ...
 *         static void do_define(cpp_reader *pfile) { ... }  // def
 *     C accepts both when their signatures match, so the def must
 *     be allowed through even though the decl "already seen" the
 *     name. Mismatched signatures (abs(int) decl vs abs(long) def)
 *     are a C conflicting-types error — we can't detect that without
 *     signature tracking, so rely on the header-include order. */
/* Canonical free-function signature key.
 *
 * Replaces a previous patchwork of partial-signature predicates
 * (nparams + first_param_kind + first_param_tag + first_param_is_
 * unsigned + ...) that had to grow every time a new C++ distinction
 * surfaced (signed vs unsigned, enum tags, ref vs ptr). The mangler
 * already encodes every distinction the language considers
 * meaningful; using its output as the equality key turns "are these
 * two decls the same function?" into a string compare and removes
 * the need for per-feature predicates entirely.
 *
 * Key form:  <name>_p_<param1>_<param2>..._pe_  — name plus the
 * standard '_p_..._pe_' param suffix (mangle_param_suffix encoding).
 * Used both for dedup (same key → same function) and overload
 * detection (multiple keys for one name → overloaded). */
#define FUNC_SIG_KEY_MAX 512
typedef struct {
    char  key[FUNC_SIG_KEY_MAX];
    int   key_len;
    Token *name;       /* original source token, for emit + lookup */
    bool  is_def;
    bool  is_gnu_inline_def;  /* the recorded def is gnu_inline (a hint,
                                  not a strong symbol) — a later strong
                                  def for the same key should still emit. */
} FuncSig;
static FuncSig g_func_sigs[8192];
static int g_n_func_sigs = 0;

/* Build the canonical key for (name, params). Returns key length
 * written into buf (which must be at least FUNC_SIG_KEY_MAX). */
static int func_sig_key(Token *name, Type **params, int nparams,
                          char *buf) {
    int pos = 0;
    if (name && name->len > 0 && pos + name->len < FUNC_SIG_KEY_MAX) {
        memcpy(buf + pos, name->loc, name->len);
        pos += name->len;
    }
    pos = mangle_param_suffix_to_buf(params, nparams, buf, pos,
                                       FUNC_SIG_KEY_MAX);
    return pos;
}

/* Find a previously-registered sig matching this key. */
static FuncSig *func_sig_find_key(const char *key, int key_len) {
    for (int i = 0; i < g_n_func_sigs; i++) {
        if (g_func_sigs[i].key_len == key_len &&
            memcmp(g_func_sigs[i].key, key, key_len) == 0)
            return &g_func_sigs[i];
    }
    return NULL;
}

/* Add a new sig entry; returns it. Caller pre-checked it's new. */
static FuncSig *func_sig_add(Token *name, const char *key, int key_len,
                              bool is_def) {
    if (g_n_func_sigs >= 8192) return NULL;
    FuncSig *e = &g_func_sigs[g_n_func_sigs++];
    memcpy(e->key, key, key_len);
    e->key_len = key_len;
    e->name = name;
    e->is_def = is_def;
    e->is_gnu_inline_def = false;
    return e;
}

/* DECL dedup: a forward declaration we've seen before (any kind —
 * decl or def) gets skipped. Returns true if dup. */
static bool func_decl_dedup_check_sig(Token *name, Type **params, int nparams) {
    if (!name) return false;
    char key[FUNC_SIG_KEY_MAX];
    int key_len = func_sig_key(name, params, nparams, key);
    if (func_sig_find_key(key, key_len)) return true;
    func_sig_add(name, key, key_len, /*is_def=*/false);
    return false;
}

/* DEF dedup: seeing a def for a sig that was already a def → skip
 * (header-include duplicate). A def for a sig previously seen as a
 * decl → upgrade to def, emit. New sig → register, emit. */
static bool func_def_dedup_check_sig(Token *name, Type **params, int nparams,
                                       int storage_flags) {
    if (!name) return false;
    char key[FUNC_SIG_KEY_MAX];
    int key_len = func_sig_key(name, params, nparams, key);
    FuncSig *fs = func_sig_find_key(key, key_len);
    bool is_gnu_inline =
        (storage_flags & DECL_INLINE) && (storage_flags & DECL_EXTERN);
    if (fs) {
        /* Already saw a def for this sig. Skip the duplicate UNLESS
         * the prior def was gnu_inline (a hint, no symbol emitted)
         * and the current is a strong def — the strong def must be
         * emitted to provide the actual library symbol. C99 §6.2.2/4
         * + GNU gnu_inline semantics: 'extern inline gnu_inline'
         * declares a hint; a later non-extern non-inline definition
         * is the strong OOL.
         *
         * Concrete: gcc 4.8 tree.h declares
         *   extern inline __attribute__((__gnu_inline__)) HOST_WIDE_INT
         *   tree_low_cst (const_tree, int) { ... }
         * tree.c then provides the strong OOL
         *   HOST_WIDE_INT tree_low_cst (const_tree, int) { ... }
         * Without distinguishing, the strong def was deduped away
         * and tree.o had only the gnu_inline hint (no exported
         * symbol) — 211 cc1plus link errors. */
        if (fs->is_def && !fs->is_gnu_inline_def) return true;
        if (fs->is_def && fs->is_gnu_inline_def && is_gnu_inline)
            return true;  /* both gnu_inline: still a duplicate */
        fs->is_def = true;
        if (is_gnu_inline) fs->is_gnu_inline_def = true;
        else               fs->is_gnu_inline_def = false; /* strong def */
        return false;
    }
    FuncSig *added = func_sig_add(name, key, key_len, /*is_def=*/true);
    if (added) added->is_gnu_inline_def = is_gnu_inline;
    return false;
}


static void emit_mangled_class_tag(Type *class_type) {
    if (!class_type || !class_type->tag) {
        /* Anonymous struct/union — generate a unique name.
         * C11 allows anonymous struct/union members inside structs,
         * but we need a name for type references and forward decls.
         * Cache the id on the Type so every reference (definition
         * and uses) resolves to the same name. */
        if (class_type) {
            if (class_type->anon_id == 0)
                class_type->anon_id = ++g_anon_counter;
            fprintf(stdout, "__sf_anon_%d", class_type->anon_id);
        } else {
            fprintf(stdout, "__sf_anon_%d", ++g_anon_counter);
        }
        return;
    }
    mangle_class_tag(class_type);
}

/* Number of direct (non-virtual) base classes — N4659 §13 [class.derived].
 * Walks the class's REGION_CLASS base list. The bases are recorded
 * during class-body parsing (parse_type_specifiers calls
 * region_add_base). For inheritance LAYOUT, codegen embeds each
 * base as a struct field and chains ctors/dtors through them. */
static int class_nbases(Type *class_type) {
    if (!class_type || !class_type->class_region) return 0;
    return class_type->class_region->nbases;
}

/* Get the i-th direct base's TYPE (not just its region). The base's
 * region carries owner_type back to the Type — that's what codegen
 * needs to mangle the base's tag, walk the base's class_def, etc. */
static Type *class_base(Type *class_type, int i) {
    if (!class_type || !class_type->class_region) return NULL;
    if (i < 0 || i >= class_type->class_region->nbases) return NULL;
    return class_type->class_region->bases[i]->owner_type;
}

/* Find the class that owns the __sf_vptr field for `t`. Sea-front
 * places the vptr in the first polymorphic ancestor that itself has
 * no polymorphic base, so a derived class reuses its base's vptr
 * (offset-0 invariant — see emit_class_def's struct-emit comment).
 * Walk up through index-0 polymorphic bases until we find that owner.
 * Returns NULL if t isn't polymorphic. */
static Type *vptr_owner_class(Type *t) {
    if (!t || !t->has_virtual_methods) return NULL;
    Type *cur = t;
    while (1) {
        Type *first_poly_base = NULL;
        int nb = class_nbases(cur);
        for (int b = 0; b < nb; b++) {
            Type *base = class_base(cur, b);
            if (base && base->has_virtual_methods) {
                first_poly_base = base;
                break;
            }
        }
        if (!first_poly_base) return cur;
        cur = first_poly_base;
    }
}

/* True iff class_type has a virtual destructor (user-declared or
 * inherited from any polymorphic base). N4659 §15.4 [class.dtor]/12.
 * The vtable layout needs to reserve a __dtor slot iff this holds. */
static bool class_has_virtual_dtor(Type *class_type) {
    if (!class_type) return false;
    Node *cdef = class_type->class_def;
    if (cdef) {
        for (int i = 0; i < cdef->class_def.nmembers; i++) {
            Node *m = cdef->class_def.members[i];
            if (!m) continue;
            if (m->kind == ND_FUNC_DEF && m->func.is_destructor &&
                m->func.is_virtual) return true;
            if (m->kind == ND_VAR_DECL && m->var_decl.is_destructor &&
                m->var_decl.is_virtual) return true;
        }
    }
    /* Inherited from any direct base — N4659 §15.4 [class.dtor]/12
     * makes the derived's dtor implicitly virtual. */
    int nb = class_nbases(class_type);
    for (int b = 0; b < nb; b++) {
        Type *base = class_base(class_type, b);
        if (class_has_virtual_dtor(base)) return true;
    }
    return false;
}

/* Emit `this->__sf_vptr` (when class_type owns the vptr) or
 * `((struct <owner-tag> *)this)->__sf_vptr` (when an ancestor owns it).
 * The cast is sound because sea-front lays the polymorphic base at
 * offset 0 of the derived struct, matching the Itanium-style ABI's
 * is-a-compatibility convention. */
static void emit_vptr_lhs(Type *class_type) {
    Type *owner = vptr_owner_class(class_type);
    if (!owner || owner == class_type) {
        fputs("this->__sf_vptr", stdout);
        return;
    }
    fputs("((struct ", stdout);
    mangle_class_tag(owner);
    fputs(" *)this)->__sf_vptr", stdout);
}

/* Find a path from 'current' to a base 'target' through the
 * non-virtual inheritance graph. Depth-first; first match wins,
 * matching the lookup convention in lookup.c. Path indices are
 * written to path[] and the length (number of inheritance hops)
 * is returned. Zero means 'not found' (or current == target — no
 * walk needed). Bounded by max_depth to avoid runaway in malformed
 * graphs. */
static int find_base_path(Type *current, Type *target,
                          int *path, int max_depth) {
    if (!current || !target || current == target || max_depth <= 0)
        return 0;
    int nb = class_nbases(current);
    for (int b = 0; b < nb; b++) {
        Type *base = class_base(current, b);
        if (!base) continue;
        if (base == target) {
            path[0] = b;
            return 1;
        }
        int sub = find_base_path(base, target, path + 1, max_depth - 1);
        if (sub > 0) {
            path[0] = b;
            return sub + 1;
        }
    }
    return 0;
}

/* Emit '__sf_base.' (or '__sf_base<N>.') for each step in the path.
 * Used when chaining through inherited members:
 *     this->__sf_base.__sf_base.x
 * The trailing '.' is included so the caller can append the final
 * member name directly. Pass len == 0 for a no-op. */
static void emit_base_chain(int *path, int len) {
    for (int i = 0; i < len; i++) {
        int b = path[i];
        if (b == 0) fputs("__sf_base.", stdout);
        else        fprintf(stdout, "__sf_base%d.", b);
    }
}

/* Walk a class's member list to find a method by name and report
 * whether it's virtual. Used by call-site emission to decide
 * between direct dispatch (sf__Class__m(&obj, args)) and indirect
 * dispatch (obj.__sf_vptr->m(&obj, args)). */
static bool method_is_virtual(Type *class_type, Token *method_name) {
    if (!class_type || !class_type->class_def || !method_name) return false;
    Node *cdef = class_type->class_def;
    for (int i = 0; i < cdef->class_def.nmembers; i++) {
        Node *m = cdef->class_def.members[i];
        if (!m) continue;
        if (m->kind == ND_FUNC_DEF && m->func.name &&
            tokens_equal(m->func.name, method_name))
            return m->func.is_virtual;
        if (m->kind == ND_VAR_DECL && m->var_decl.name &&
            m->var_decl.ty && m->var_decl.ty->kind == TY_FUNC &&
            tokens_equal(m->var_decl.name, method_name))
            return m->var_decl.is_virtual;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Type emission                                                      */
/* ------------------------------------------------------------------ */

/* Walk a chain of TY_ARRAY layers and emit literal '[N]' brackets in
 * source (outermost-first) order. Stops on the first non-array node.
 * For unsized arrays emits '[]'; for arbitrary-expression sizes emits
 * '[expr]' verbatim. N4659 §11.3.4 [dcl.array]. */
static void emit_array_dims(Type *array_chain) {
    for (Type *d = array_chain; d && d->kind == TY_ARRAY; d = d->base) {
        if (d->array_len >= 0) {
            fprintf(stdout, "[%d]", d->array_len);
        } else if (d->array_size_expr) {
            fputc('[', stdout);
            emit_expr(d->array_size_expr);
            fputc(']', stdout);
        } else {
            fputs("[]", stdout);
        }
    }
}

/* Emit a generalized C declarator for the pointer-to-array shape
 *   ELEM (***NAME[outer_dims])[inner_dims]
 * (any number of outer-array layers, any number of pointer layers,
 * any number of inner-array layers, then a non-array/non-ptr element).
 * Returns true on a successful match + emit; false if the type doesn't
 * fit the shape and the caller should fall back. N4659 §11.3
 * [dcl.meaning]. */
static bool emit_pointer_to_array_declarator(Type *ty, Token *name) {
    if (!ty) return false;
    Type *t = ty;
    Type *outer_dims_head = NULL;
    while (t && t->kind == TY_ARRAY) {
        if (!outer_dims_head) outer_dims_head = t;
        t = t->base;
    }
    int n_ptrs = 0;
    while (t && t->kind == TY_PTR) { n_ptrs++; t = t->base; }
    if (n_ptrs < 1 || !t || t->kind != TY_ARRAY) return false;
    Type *inner_dims_head = t;
    Type *elem = inner_dims_head;
    while (elem && elem->kind == TY_ARRAY) elem = elem->base;
    if (!elem || elem->kind == TY_ARRAY || elem->kind == TY_PTR) return false;
    emit_type(elem);
    fputs(" (", stdout);
    for (int i = 0; i < n_ptrs; i++) fputc('*', stdout);
    if (name) fprintf(stdout, "%.*s", name->len, name->loc);
    emit_array_dims(outer_dims_head);
    fputc(')', stdout);
    emit_array_dims(inner_dims_head);
    return true;
}


static void emit_type(Type *ty) {
    if (!ty) { fputs("/*?*/ int", stdout); return; }

    /* Pointer-to-array shape — emit using the C declarator-interleave
     * form 'ELEM (***)[N]' instead of decaying everything to '**...'.
     * Without this, casts and return types lose the inner array bound
     * and 'sizeof(T(*)[N])' computes sizeof(T*) instead of N*sizeof(T).
     * N4659 §8.3.3 [expr.sizeof] / §11.3 [dcl.meaning]. */
    if (emit_pointer_to_array_declarator(ty, /*name=*/NULL)) return;

    /* cv-qualifier placement:
     *   TY_PTR with is_const: 'T * const' (const pointer to T) —
     *     the qualifier follows the '*' in the declarator
     *     (N4659 §11.3.1 [dcl.ptr]).
     *   Other kinds with is_const: 'const T' (const-qualified T) —
     *     west-const placement.
     * Emitting 'const' first for TY_PTR yields 'const T *' (const
     * pointee), which mis-types 'struct S *const ss' as a pointer
     * to const S and breaks 'ss->field = x' with an lvalue error. */
    bool is_ptr = ty->kind == TY_PTR;
    if (!is_ptr) {
        if (ty->is_const)    fputs("const ", stdout);
        if (ty->is_volatile) fputs("volatile ", stdout);
        /* _Complex prefix — C99/C11 §6.2.5/11. Only the scalar
         * arithmetic types are valid carriers; the parser preserves
         * the underlying real type (int/float/double/...) on the
         * base Type, and we re-emit _Complex here for round-trip. */
        if (ty->is_complex)  fputs("_Complex ", stdout);
    }

    switch (ty->kind) {
    case TY_VOID:    fputs("void", stdout); return;
    case TY_BOOL:    fputs("_Bool", stdout); return;  /* C spelling */
    case TY_CHAR:    fputs(ty->is_unsigned ? "unsigned char" : "char", stdout); return;
    case TY_CHAR16:  fputs("char16_t", stdout); return;
    case TY_CHAR32:  fputs("char32_t", stdout); return;
    case TY_WCHAR:   fputs("wchar_t", stdout); return;
    case TY_SHORT:   fputs(ty->is_unsigned ? "unsigned short" : "short", stdout); return;
    case TY_INT:
        /* GCC __builtin_va_list — opaque builtin type. The parser
         * resolves it to TY_INT (unknown ident fallback) but tags
         * it; emit the tag verbatim so gcc handles va_arg natively.
         * Pattern: gcc 4.8 tree-data-ref.c conflict_fn. */
        if (ty->tag && ty->tag->len == 17 &&
            memcmp(ty->tag->loc, "__builtin_va_list", 17) == 0) {
            fputs("__builtin_va_list", stdout);
            return;
        }
        fputs(ty->is_unsigned ? "unsigned int" : "int", stdout); return;
    case TY_LONG:    fputs(ty->is_unsigned ? "unsigned long" : "long", stdout); return;
    case TY_LLONG:   fputs(ty->is_unsigned ? "unsigned long long" : "long long", stdout); return;
    case TY_FLOAT:   fputs("float", stdout); return;
    case TY_DOUBLE:  fputs("double", stdout); return;
    case TY_LDOUBLE: fputs("long double", stdout); return;
    case TY_PTR:
        /* Function-pointer type in type-expression position.
         * Handles arbitrary pointer depth: T(*)(args) for single
         * pointer, T(**)(args) for pointer-to-function-pointer, etc.
         * N4659 §11.3.1 [dcl.ptr] / §11.3.5 [dcl.fct]. */
        {
            Type *fty = NULL;
            int pdepth = 0;
            Type *t = ty;
            while (t && t->kind == TY_PTR && t->base) {
                pdepth++;
                if (t->base->kind == TY_FUNC) { fty = t->base; break; }
                t = t->base;
            }
            if (fty) {
                emit_type(fty->ret);
                fputs(" (", stdout);
                for (int d = 0; d < pdepth; d++) fputc('*', stdout);
                fputs(")(", stdout);
                emit_func_param_types(fty);
                fputc(')', stdout);
                return;
            }
        }
        emit_type(ty->base);
        fputs("*", stdout);
        if (ty->is_const)    fputs(" const", stdout);
        if (ty->is_volatile) fputs(" volatile", stdout);
        return;
    /* References — emit as pointer in C (caller passes &x).
     * A reference to a function type (void(&)()) is also a single
     * pointer level in C, so route through the function-pointer
     * declarator form like TY_PTR(TY_FUNC) above. Otherwise the
     * recursive emit_type call hits the TY_FUNC default branch and
     * emits a comment-marker int, which the C compiler rejects. */
    case TY_REF:
    case TY_RVALREF:
        if (ty->base && ty->base->kind == TY_FUNC) {
            Type *fty = ty->base;
            emit_type(fty->ret);
            fputs(" (*)(", stdout);
            emit_func_param_types(fty);
            fputc(')', stdout);
            return;
        }
        emit_type(ty->base); fputs("*", stdout); return;
    case TY_ARRAY:   emit_type(ty->base); fputs("*", stdout); return;
    case TY_STRUCT:
        fputs("struct ", stdout);
        emit_mangled_class_tag(ty);
        return;
    case TY_UNION:
        fputs("union ", stdout);
        emit_mangled_class_tag(ty);
        return;
    case TY_ENUM:
        fputs("enum ", stdout);
        if (ty->tag) fprintf(stdout, "%.*s", ty->tag->len, ty->tag->loc);
        return;
    case TY_DEPENDENT:
        /* Template type parameter — should have been substituted by the
         * instantiation pass before codegen. If we reach here, the type
         * is in an uninstantiated template body (skipped by codegen) or
         * a dependent context we haven't resolved. Emit as a comment
         * for debugging. */
        fputs("/*dep:", stdout);
        if (ty->tag) fprintf(stdout, "%.*s", ty->tag->len, ty->tag->loc);
        fputs("*/ int", stdout);
        return;
    case TY_PMEM:
        /* Pointer-to-data-member lowers to __PTRDIFF_TYPE__ — a
         * signed integer holding the member's byte offset within the
         * owning class. Null is represented as -1 (Itanium ABI). The
         * pointee type and owner class are tracked on the Type for
         * sema/codegen but don't appear in the C type name. */
        fputs("__PTRDIFF_TYPE__", stdout);
        return;
    default:
        fputs("/*?*/ int", stdout);
        return;
    }
}

/* Re-emit storage-class / function-specifier keywords from the
 * DeclSpec flags captured at parse time. N4659 §10.1.1 [dcl.stc] /
 * §10.1.6 [dcl.inline].
 *
 * C-vs-C++ inline divergence is handled here (function DEFINITIONS
 * only, signalled by for_definition=true): C++ inline functions
 * have external linkage with "merge across TUs" semantics handled
 * by the linker (COMDAT / weak). A plain C 'inline' definition
 * also has external linkage but requires exactly ONE non-inline
 * provider across the program. Preprocessed C++ headers emit the
 * inline in every TU that includes them — plain C link produces
 * duplicate-definition errors.
 *
 * Lowering: an 'inline' DEFINITION without static/extern becomes
 * '__attribute__((weak))' WITHOUT the 'inline' keyword. gcc warns
 * and drops the weak attribute when combined with 'inline'
 * (-Wattributes), so we emit weak-without-inline to actually get
 * weak linkage. The compiler can still inline based on contents.
 * Static inlines need no shim (internal linkage per TU). */
static void emit_storage_flags_impl(int flags, bool for_definition) {
    bool is_inline = (flags & DECL_INLINE) != 0;
    bool is_static = (flags & DECL_STATIC) != 0;
    bool is_extern = (flags & DECL_EXTERN) != 0;
    /* Any inline DEFINITION (with or without extern) gets emitted
     * as 'static inline'. Each TU that includes the header gets its
     * own private copy; the C compiler drops the body if it's not
     * actually called from this TU (standard dead-code elimination),
     * which matters when a header-defined inline references a
     * function that lives in a library not linked into THIS .o
     * (gcc 4.8's bitmap.h inline 'dump_bitmap' calls bitmap_print —
     * not linked into the small gen-tools; libstdc++ c++config.h
     * inline '__terminate' calls 'terminate' — same story).
     *
     * Cross-TU dedup is trivial under static inline: each TU has
     * its own private copy, no external symbol → no possibility of
     * multi-definition. N4659 §10.1.6 [dcl.inline] / GNU 'extern
     * inline' / C99 inline all permit this lowering for inline
     * functions. */
    /* 'extern inline' definition: GNU 'gnu_inline' semantics — the
     * inline body is a hint; an extern OOL strong definition lives
     * elsewhere (usually in a separate .c file / library). gmp.h's
     * '__GMP_EXTERN_INLINE' is exactly this pattern; the OOL bodies
     * live in libgmp.a. Emitting the body as 'static inline' (per
     * the inline-DEFINITION branch below) would clash with prior
     * 'extern int __gmpz_abs(...);' declarations — C99 §6.2.2/4
     * doesn't allow internal-linkage to follow external. Preserve
     * GNU extern-inline semantics with the explicit attribute so
     * the C compiler doesn't emit a body symbol from this TU and
     * the linker resolves to the library's strong definition. */
    if (for_definition && is_inline && is_extern && !is_static) {
        fputs("__attribute__((gnu_inline)) extern inline ", stdout);
        return;
    }
    if (for_definition && is_inline && !is_static) {
        fputs("static inline ", stdout);
        return;  /* skip 'extern' — internal linkage by design */
    }
    if (is_extern)  fputs("extern ", stdout);
    if (is_static)  fputs("static ", stdout);
    if (is_inline)  fputs("inline ", stdout);
}

static void emit_storage_flags(int flags) {
    emit_storage_flags_impl(flags, /*for_definition=*/false);
}

static void emit_storage_flags_for_def(int flags) {
    emit_storage_flags_impl(flags, /*for_definition=*/true);
}

/* Variable storage flags + constexpr/inline lowering — §10.1.5
 * [dcl.constexpr] + §10.1.6 [dcl.inline]. C has no inline / constexpr
 * on variables, so:
 *   - constexpr (§10.1.5/9 implies const) → 'static const'.
 *   - inline variables (C++17 §10.1.6/7 — same multi-TU dedup
 *     guarantees as inline functions) → 'static'. The C output
 *     gets a per-TU private copy, no external symbol; multiple
 *     TUs including the same header don't link-collide.
 * Both implications also subsume 'static' if it wasn't already
 * spelled. The block-scope cases (locals) get 'static' as well —
 * harmless for true constants (init-once, same value) and the
 * common idiom for inline-vars-as-locals doesn't actually exist. */
static void emit_var_storage_flags(int flags) {
    bool implies_static = (flags & (DECL_CONSTEXPR | DECL_INLINE)) != 0;
    if (implies_static && !(flags & DECL_STATIC))
        fputs("static ", stdout);
    if (flags & DECL_CONSTEXPR)
        fputs("const ", stdout);
    /* Strip the C++-only spellings — emit_storage_flags would
     * otherwise print 'inline'/etc. literally, which cc warns
     * about on a variable and would multi-define across TUs. */
    flags &= ~(DECL_CONSTEXPR | DECL_INLINE);
    emit_storage_flags(flags);
}

/* Collect the parameter types of a function AST node (ND_FUNC_DEF
 * or ND_VAR_DECL with TY_FUNC) into a fresh Type* array so the
 * mangling helpers can encode them as a signature suffix.
 *
 * Returns the type array via *out_types (NULL when nparams == 0)
 * and the count via the return value. The array lives in a small
 * static pool since mangling emits immediately — no long-lived
 * ownership. */
static int collect_func_param_types(Node *func_node, Type ***out_types) {
    static Type *pool[64];
    *out_types = NULL;
    if (!func_node) return 0;
    int n = 0;
    if (func_node->kind == ND_FUNC_DEF || func_node->kind == ND_FUNC_DECL) {
        n = func_node->func.nparams;
        if (n > 64) n = 64;
        for (int i = 0; i < n; i++)
            pool[i] = func_node->func.params[i]->param.ty;
        *out_types = n > 0 ? pool : NULL;
        return n;
    }
    if (func_node->kind == ND_VAR_DECL && func_node->var_decl.ty &&
        func_node->var_decl.ty->kind == TY_FUNC) {
        Type *fty = func_node->var_decl.ty;
        n = fty->nparams;
        if (n > 64) n = 64;
        for (int i = 0; i < n; i++) pool[i] = fty->params[i];
        *out_types = n > 0 ? pool : NULL;
        return n;
    }
    return 0;
}

/* Collect the resolved types of call arguments. Used ONLY to pick
 * the matching overload at a call site (see resolve_overload) —
 * NOT for mangling directly. The mangled symbol is defined by the
 * DECLARATION's signature, so call-site mangling must round-trip
 * through resolve_overload to find the decl and emit ITS param
 * types. Call-arg types may differ (implicit conversions); only
 * the decl's types are part of the symbol name.
 *
 * Uses a static pool since mangling emits immediately — callers
 * must emit-and-consume before reinvoking. */
static int collect_call_arg_types(Node **args, int nargs, Type ***out_types) {
    static Type *pool[64];
    *out_types = NULL;
    if (nargs <= 0) return 0;
    if (nargs > 64) nargs = 64;
    for (int i = 0; i < nargs; i++)
        pool[i] = args[i] ? args[i]->resolved_type : NULL;
    *out_types = pool;
    return nargs;
}

/* Score how well a single param/arg type pair matches.
 * Returns 0 for "no match", positive scores for various levels of
 * compatibility. Recurses through indirection chains (TY_PTR / TY_REF /
 * TY_RVALREF / TY_ARRAY) so 'int *' vs 'int *' beats 'int *' vs
 * 'int **' — the previous kind-only check rated both as equal-score
 * because both arg+param were TY_PTR at the top, picking the first
 * candidate by table order rather than the better match. Pattern:
 * gcc 4.8 vec.h has overloaded 'iterate(unsigned, T*)' and
 * 'iterate(unsigned, T**)'; sea-front was always picking the first
 * regardless of the actual third-argument's pointer depth. */
static int score_type_pair(Type *pt, Type *at) {
    if (!pt || !at) return 0;
    if (pt->kind == at->kind) {
        int s = 2;
        /* Recurse into indirection bases so deeper structural agreement
         * outranks shallow kind agreement. */
        if ((pt->kind == TY_PTR || pt->kind == TY_REF ||
             pt->kind == TY_RVALREF || pt->kind == TY_ARRAY) &&
            pt->base && at->base) {
            s += score_type_pair(pt->base, at->base);
        }
        if ((pt->kind == TY_STRUCT || pt->kind == TY_UNION) &&
            pt->tag && at->tag && tokens_equal(pt->tag, at->tag))
            s++;
        return s;
    }
    /* Reference parameter (lowered to T* in C) accepting an arg of
     * the underlying type: copy/move-ctor pattern.
     * N4659 §16.3.3.2.1 [over.ics.rank]. */
    if (ty_is_ref(pt) && pt->base && pt->base->kind == at->kind) {
        int s = 2;
        if ((at->kind == TY_STRUCT || at->kind == TY_UNION) &&
            pt->base->tag && at->tag &&
            tokens_equal(pt->base->tag, at->tag))
            s++;
        return s;
    }
    return 0;
}

static void emit_default_args_tail(Node *resolved_callee, int nargs, int np,
                                    Type **pty) {
    if (!resolved_callee) return;
    for (int i = nargs; i < np; i++) {
        Node *p = func_param_node(resolved_callee, i);
        if (!p || !p->param.default_value) {
            /* Hard-fail: by the time we're here, overload_match_score
             * has already verified that every param past nargs has a
             * default — that's part of the candidate's viability under
             * N4659 §16.3.1.4 [over.match.viable]/2. Reaching this
             * branch means an internal inconsistency between
             * resolution and emit. Silently emitting fewer args than
             * the callee declares is the silent-miscompile pattern
             * we're explicitly avoiding (cf. die_no_overload). */
            fprintf(stderr,
                "sea-front: internal error in default-arg expansion: "
                "param %d/%d has no default but candidate was selected "
                "as viable\n",
                i, np);
            abort();
        }
        fputs(", ", stdout);
        emit_arg_for_param(p->param.default_value,
                            pty && i < np ? pty[i] : NULL);
    }
}

/* Return the kth param Node of a function-shaped declaration,
 * or NULL if unavailable. ND_FUNC_DEF stores params on func.params;
 * ND_VAR_DECL with TY_FUNC stores them on var_decl.fn_params (with
 * the typedef-decay form var_decl.ty->params being Type-only).
 * Used by overload-resolution viability and by call-site emit to
 * read out param.default_value for default-arg expansion (N4659
 * §11.3.6 [dcl.fct.default]). */
static Node *func_param_node(Node *m, int k) {
    if (!m) return NULL;
    if (m->kind == ND_FUNC_DEF) {
        if (k < 0 || k >= m->func.nparams) return NULL;
        return m->func.params[k];
    }
    if (m->kind == ND_VAR_DECL) {
        if (k < 0 || k >= m->var_decl.fn_nparams) return NULL;
        return m->var_decl.fn_params ? m->var_decl.fn_params[k] : NULL;
    }
    return NULL;
}

/* Match a single candidate member against a call's arg types.
 * Returns -1 if the candidate is non-viable, else a positive score
 * where higher = better fit. Viability per N4659 §16.3.1.4
 * [over.match.viable]/2: nparams equals nargs, OR nparams > nargs
 * and every excess param has a default. Score is over the user-
 * supplied args only — defaults contribute nothing, so an exact
 * match wins over a defaults-padded match all else equal. */
static int overload_match_score(Node *m, Type **arg_types, int nargs) {
    bool is_def = m->kind == ND_FUNC_DEF;
    int nparams = is_def ? m->func.nparams : m->var_decl.ty->nparams;
    /* N4659 §16.3.1.4 [over.match.viable]/2: a candidate is viable
     * when the number of arguments equals the number of params, OR
     * when nargs < nparams and every excess param has a default
     * argument. Excess args (nargs > nparams) are non-viable. */
    if (nparams < nargs) return -1;
    if (nparams > nargs) {
        for (int k = nargs; k < nparams; k++) {
            Node *p = func_param_node(m, k);
            if (!p || !p->param.default_value) return -1;
        }
    }
    /* Score over the user-supplied args only — extras are matched
     * by "default exists" above and contribute nothing to the score
     * (so a function exact-matched on N args wins over one matched
     * on N args + M defaulted, all else equal). */
    int score = 0;
    for (int k = 0; k < nargs && k < 64; k++) {
        Type *pt = is_def ? m->func.params[k]->param.ty
                          : m->var_decl.ty->params[k];
        Type *at = arg_types ? arg_types[k] : NULL;
        score += score_type_pair(pt, at);
    }
    return score;
}

static int copy_member_param_types(Node *m, Type **pool) {
    if (m->kind == ND_FUNC_DEF) {
        int n = m->func.nparams;
        if (n > 64) n = 64;
        for (int i = 0; i < n; i++)
            pool[i] = m->func.params[i]->param.ty;
        return n;
    }
    int n = m->var_decl.ty->nparams;
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++)
        pool[i] = m->var_decl.ty->params[i];
    return n;
}

static Node *find_class_def_by_tag_args(Type *class_type);

/* Two template-instantiated class types name the SAME instantiation
 * when they're both class types (struct or union — catches both
 * 'template<> struct S<int>' and 'template<> union U<int>'), both
 * have non-NULL matching tags, and their template_args match
 * structurally. Narrower-than-types_equivalent by design: we want to
 * collapse 'vec<int,va_heap,vl_embed>' reached via two discovery
 * paths, NOT two unrelated anonymous unions that happen to share a
 * tag-less Type. Used by the struct-phase, method-phase, and TU-
 * lookup dedup sites.
 *
 * N4659 §17.8.1 [temp.inst] — one instantiation per distinct
 * template-id. Cross-TU merging is handled separately by the weak-
 * linkage emission. */
static bool same_template_instantiation(Type *a, Type *b) {
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    if (a->kind != TY_STRUCT && a->kind != TY_UNION) return false;
    if (!a->tag || !b->tag) return false;
    if (a->tag->len != b->tag->len) return false;
    if (memcmp(a->tag->loc, b->tag->loc, a->tag->len) != 0) return false;
    if (a->n_template_args <= 0) return false;
    if (a->n_template_args != b->n_template_args) return false;
    for (int i = 0; i < a->n_template_args; i++)
        if (!types_equivalent(a->template_args[i], b->template_args[i]))
            return false;
    return true;
}

/* Collect same-named candidate methods from a class AND all its
 * base classes (recursive). Inherited methods need to be reachable
 * through the same-name lookup. Caller's 'found' vector accumulates. */
static void collect_overload_candidates(Type *class_type, Token *name,
                                         bool is_ctor,
                                         Node **found, int *nfound, int cap) {
    if (!class_type) return;
    /* class_def may be unset on a Type obtained via a method return
     * type or function param, even when class_region IS set. Fall
     * back through class_region->owner_type which is the canonical
     * Type (the one used when parsing the class body). */
    Node *cd = class_type->class_def;
    if (!cd && class_type->class_region &&
        class_type->class_region->owner_type &&
        class_type->class_region->owner_type->class_def)
        cd = class_type->class_region->owner_type->class_def;
    /* Template-instantiated Type copies carry neither class_def nor
     * class_region — look the instantiated class up in the TU by
     * (tag, template_args). Without this, calls inside instantiated
     * method bodies miss the _const mangling suffix because overload
     * resolution can't see the candidates. */
    if (!cd) {
        Node *d = find_class_def_by_tag_args(class_type);
        if (d) cd = d;
    }
    if (!cd) return;
    for (int i = 0; i < cd->class_def.nmembers; i++) {
        Node *m = cd->class_def.members[i];
        if (!m) continue;
        /* Member templates: peel ND_TEMPLATE_DECL to its inner func/
         * var-decl. The inner candidate is what we score against arg
         * types — its params have TY_DEPENDENT for the template-
         * parameter positions, which the existing match scorer treats
         * as wildcards. Pattern: gcc 4.8 vec.h va_heap::release<T>
         * called as bare release(v) inside va_heap::reserve<T>. */
        if (m->kind == ND_TEMPLATE_DECL && m->template_decl.decl)
            m = m->template_decl.decl;
        bool is_def   = m->kind == ND_FUNC_DEF;
        bool is_decl  = m->kind == ND_VAR_DECL && m->var_decl.ty &&
                        m->var_decl.ty->kind == TY_FUNC;
        if (!is_def && !is_decl) continue;
        bool m_is_ctor = is_def ? m->func.is_constructor
                                : m->var_decl.is_constructor;
        if (is_ctor != m_is_ctor) continue;
        Token *mn = is_def ? m->func.name : m->var_decl.name;
        if (!is_ctor) {
            if (!name || !mn) continue;
            if (mn->len != name->len) continue;
            if (memcmp(mn->loc, name->loc, name->len) != 0) continue;
        }
        if (*nfound < cap) found[(*nfound)++] = m;
    }
    /* Inherited methods — N4659 §13.5.2 [class.member.lookup].
     * Ctors are not inherited (§15.1 [class.ctor]). */
    if (!is_ctor && class_type->class_region) {
        for (int i = 0; i < class_type->class_region->nbases; i++) {
            DeclarativeRegion *br = class_type->class_region->bases[i];
            if (br && br->owner_type)
                collect_overload_candidates(br->owner_type, name, is_ctor,
                                             found, nfound, cap);
        }
    }
}

/* Find the ctor/method declaration in a class (or its bases) that
 * best matches the given call-argument types, and return ITS param
 * types for mangling. The decl's signature IS the mangled symbol —
 * callers must not substitute the arg types directly.
 *
 * 'name' is NULL for ctors. Selection (N4659 §16.3 [over.match]):
 *   - If exactly one candidate with the name exists, use it
 *     regardless of arg kinds (implicit-conversion catchall).
 *   - Otherwise require nparams == nargs and pick the highest
 *     kind-match score; break ties by source order.
 *   - If no candidate has the right nparams, aborts — an unresolved
 *     method call is either a bug in the input or a sea-front
 *     instantiation miss. Silent fallback was the wrong move last
 *     time; surfacing it loudly is the fix. */
/* Walk a receiver's Type chain looking for const-ness. Handles
 * 'const X', 'X const', 'const X*', 'const X&' — any const on the
 * class itself (or on a ref/ptr's pointee) means the implicit
 * 'this' is 'const C*'. N4659 §16.3.1.4 [over.match.funcs]/4. */
static bool receiver_type_is_const(Type *t) {
    while (t) {
        if (t->is_const) return true;
        if (!ty_is_indirect(t)) break;
        t = t->base;
    }
    return false;
}

/* Retrieve the const-ness of a candidate declaration (ND_FUNC_DEF or
 * ND_VAR_DECL with TY_FUNC). Used for const-aware overload selection. */
static bool candidate_is_const(Node *m) {
    if (!m) return false;
    if (m->kind == ND_FUNC_DEF) return m->func.is_const_method;
    if (m->kind == ND_VAR_DECL && m->var_decl.ty &&
        m->var_decl.ty->kind == TY_FUNC)
        return m->var_decl.ty->is_const;
    return false;
}

/* N4659 §11.4.9 [class.static] — static member functions take no
 * implicit 'this'. The unqualified-call lowering at the implicit-this
 * site must skip 'this' when the resolved candidate is static. */
static bool candidate_is_static(Node *m) {
    if (!m) return false;
    /* Member templates: the candidate may be the wrapping
     * ND_TEMPLATE_DECL (returned by resolve_overload's class-region
     * lookup). Static-ness lives on the inner var-decl/func-def, so
     * peel one layer. */
    if (m->kind == ND_TEMPLATE_DECL && m->template_decl.decl)
        m = m->template_decl.decl;
    if (m->kind == ND_FUNC_DEF) return (m->func.storage_flags & DECL_STATIC) != 0;
    if (m->kind == ND_VAR_DECL)  return (m->var_decl.storage_flags & DECL_STATIC) != 0;
    return false;
}

static int resolve_overload(Type *class_type, Token *name, bool is_ctor,
                             Type **arg_types, int nargs,
                             bool receiver_is_const,
                             Type ***out_param_types,
                             Node **out_best) {
    static Type *pool[64];
    *out_param_types = NULL;
    if (out_best) *out_best = NULL;
    enum { MAX_CAND = 32 };
    Node *cands[MAX_CAND];
    int ncands = 0;
    collect_overload_candidates(class_type, name, is_ctor,
                                 cands, &ncands, MAX_CAND);
    if (ncands == 0) return -1;  /* caller may still error — see above */
    if (ncands == 1) {
        *out_param_types = pool;
        if (out_best) *out_best = cands[0];
        return copy_member_param_types(cands[0], pool);
    }
    /* Pick by kind-match score, then break ties by const-qualification
     * of the implicit 'this' parameter. N4659 §16.3.1.4
     * [over.match.funcs]/4: the implicit object parameter is 'cv C&'
     * whose cv-qualifiers match the function's. A const method is
     * viable for both const and non-const receivers; a non-const
     * method is viable only for non-const receivers. */
    Node *best = NULL;
    int best_score = -1;
    for (int i = 0; i < ncands; i++) {
        Node *c = cands[i];
        /* Viability filter: non-const method is not viable for const
         * receiver. For ctors, no implicit-this constraint. */
        if (!is_ctor && receiver_is_const && !candidate_is_const(c))
            continue;
        int s = overload_match_score(c, arg_types, nargs);
        /* Tie-break: prefer the candidate whose const matches the
         * receiver exactly (const method for const receiver, non-const
         * for non-const receiver). */
        if (s > best_score ||
            (s == best_score && best &&
             candidate_is_const(c) == receiver_is_const &&
             candidate_is_const(best) != receiver_is_const)) {
            best = c;
            best_score = s;
        }
    }
    if (!best) return -1;
    *out_param_types = pool;
    if (out_best) *out_best = best;
    return copy_member_param_types(best, pool);
}

/* Check if a method on class_type returns a reference (TY_REF / TY_RVALREF).
 * Used to decide whether to wrap method calls in (*...) for ref-return deref. */
/* Look up a 0-arg method's return type on a class. Used by the
 * range-for desugar to determine the iterator type from begin()/end().
 * Returns NULL if no matching method is found. */
static Type *class_method_return_type(Type *class_type, const char *name,
                                       int name_len) {
    if (!class_type || !class_type->class_def) return NULL;
    Node *cd = class_type->class_def;
    for (int i = 0; i < cd->class_def.nmembers; i++) {
        Node *m = cd->class_def.members[i];
        if (!m) continue;
        Token *mn = NULL;
        Type *ret = NULL;
        if (m->kind == ND_FUNC_DEF) {
            mn = m->func.name;
            ret = m->func.ret_ty;
        } else if (m->kind == ND_VAR_DECL && m->var_decl.ty &&
                   m->var_decl.ty->kind == TY_FUNC) {
            mn = m->var_decl.name;
            ret = m->var_decl.ty->ret;
        }
        if (!mn || mn->len != name_len) continue;
        if (memcmp(mn->loc, name, name_len) != 0) continue;
        return ret;
    }
    return NULL;
}

static bool method_returns_ref(Type *class_type, Token *name) {
    if (!class_type || !class_type->class_def || !name) return false;
    Node *cd = class_type->class_def;
    for (int i = 0; i < cd->class_def.nmembers; i++) {
        Node *m = cd->class_def.members[i];
        if (!m) continue;
        Token *mn = NULL;
        Type *ret = NULL;
        if (m->kind == ND_FUNC_DEF) {
            mn = m->func.name;
            ret = m->func.ret_ty;
        } else if (m->kind == ND_VAR_DECL && m->var_decl.ty &&
                   m->var_decl.ty->kind == TY_FUNC) {
            mn = m->var_decl.name;
            ret = m->var_decl.ty->ret;
        }
        if (!mn || mn->len != name->len) continue;
        if (memcmp(mn->loc, name->loc, name->len) != 0) continue;
        if (ty_is_ref(ret))
            return true;
    }
    return false;
}

/* Find a class_def in the TU matching tag + template_args. Template-
 * instantiated method bodies can carry Type copies whose class_def
 * pointer wasn't patched during cloning/substitution; the real
 * instantiated ND_CLASS_DEF sits in the TU and is identifiable by
 * (tag, template_args). Returns NULL if no match.
 *
 * Templates only — bails when class_type has no template_args so that
 * accidental misuse with a plain class falls through to the caller's
 * non-template fallback (find_class_def_by_tag_only). Shared lookup
 * machinery lives on the parse-side find_class_def_in_tu. */
static Node *find_class_def_by_tag_args(Type *class_type) {
    if (!class_type || class_type->n_template_args <= 0) return NULL;
    return find_class_def_in_tu(g_tu, class_type);
}

/* True if any TY_ENUM in the TU has a tag whose spelling matches
 * `name`. Walks namespaces (ND_BLOCK) and class bodies (ND_CLASS_DEF
 * members) so an enum nested inside 'namespace bidi { enum class
 * kind { ... }; }' is found. Used by the ND_QUALIFIED emit path
 * to recognise scoped-enum member access ('kind::NONE'). */
static bool find_enum_tag_in_tu_walk(Node *n, Token *name) {
    if (!n || !name) return false;
    switch (n->kind) {
    case ND_VAR_DECL: {
        Type *ty = n->var_decl.ty;
        if (ty && ty->kind == TY_ENUM && ty->tag &&
            ty->tag->len == name->len &&
            memcmp(ty->tag->loc, name->loc, name->len) == 0)
            return true;
        return false;
    }
    case ND_TRANSLATION_UNIT:
        for (int i = 0; i < n->tu.ndecls; i++)
            if (find_enum_tag_in_tu_walk(n->tu.decls[i], name)) return true;
        return false;
    case ND_BLOCK:
        for (int i = 0; i < n->block.nstmts; i++)
            if (find_enum_tag_in_tu_walk(n->block.stmts[i], name)) return true;
        return false;
    case ND_CLASS_DEF:
        for (int i = 0; i < n->class_def.nmembers; i++)
            if (find_enum_tag_in_tu_walk(n->class_def.members[i], name)) return true;
        return false;
    default:
        return false;
    }
}
static bool find_enum_tag_in_tu(Node *tu, Token *name) {
    return find_enum_tag_in_tu_walk(tu, name);
}

/* For plain (non-template) classes: walk the TU looking for the
 * ND_CLASS_DEF whose tag matches. Used as a fallback when a Type
 * copy lacks class_region/class_def (field-decl / typedef paths).
 * Returns NULL if there's no match or multiple ambiguous matches. */
static Node *find_class_def_by_tag_only(Type *class_type) {
    if (!g_tu || !class_type || !class_type->tag) return NULL;
    Node *found = NULL;
    for (int i = 0; i < g_tu->tu.ndecls; i++) {
        Node *d = g_tu->tu.decls[i];
        if (!d) continue;
        Type *t = NULL;
        if (d->kind == ND_CLASS_DEF) {
            t = d->class_def.ty;
        } else if (d->kind == ND_TYPEDEF) {
            /* A struct definition inside a typedef wrapper:
             *   typedef struct pre_expr_d : Base { ... } *pre_expr;
             * The struct lives on var_decl.ty (peeling ptr/array
             * layers). Sea-front parses these as ND_TYPEDEF whose
             * declared type contains the class def's class_region.
             * Without picking up this shape, qualified-call
             * mangling can't find the struct's bases (gcc 4.8
             * tree-ssa-pre.c pre_expr_d / expr_pred_trans_d). */
            Type *ty = d->var_decl.ty;
            while (ty && (ty->kind == TY_PTR || ty->kind == TY_ARRAY))
                ty = ty->base;
            if (ty && (ty->kind == TY_STRUCT || ty->kind == TY_UNION) &&
                ty->class_def)
                t = ty;
        }
        if (!t || !t->tag || t->kind != class_type->kind) continue;
        if (t->tag->len != class_type->tag->len) continue;
        if (memcmp(t->tag->loc, class_type->tag->loc, t->tag->len) != 0) continue;
        /* Skip template-instantiated variants: prefer a plain-class
         * match. Callers with templates should use
         * find_class_def_by_tag_args instead. */
        if (t->n_template_args > 0) continue;
        if (found) return NULL;  /* ambiguous */
        found = (d->kind == ND_TYPEDEF) ? t->class_def : d;
    }
    return found;
}

/* Walk the TU for an OOL function definition matching (class_type,
 * name) and return its storage_flags, or 0 if none found. The
 * in-class forward decl in PHASE_STRUCTS doesn't see the OOL
 * definition's storage class (e.g. 'inline double_int from_shwi(...)'
 * sets DECL_INLINE on the OOL ND_FUNC_DEF but not on the in-class
 * ND_VAR_DECL declaration), so we look it up here to make the
 * forward decl's linkage match the body's. C99 §6.2.2/4. */
static int find_ool_method_storage(Type *class_type, Token *name,
                                     Type **wanted_params, int wanted_nparams,
                                     bool wanted_is_const,
                                     Token *wanted_op_after) {
    if (!g_tu || !class_type || !class_type->tag || !name) return 0;
    for (int i = 0; i < g_tu->tu.ndecls; i++) {
        Node *d = g_tu->tu.decls[i];
        if (!d) continue;
        if (d->kind == ND_TEMPLATE_DECL && d->template_decl.decl)
            d = d->template_decl.decl;
        if (d->kind != ND_FUNC_DEF) continue;
        if (d->func.class_type != class_type) {
            /* Tag-based fallback: instantiation pass can produce
             * distinct Type* for the same concrete class. */
            if (!d->func.class_type || !d->func.class_type->tag) continue;
            if (d->func.class_type->tag->len != class_type->tag->len) continue;
            if (memcmp(d->func.class_type->tag->loc, class_type->tag->loc,
                       class_type->tag->len) != 0) continue;
        }
        if (!d->func.name) continue;
        if (d->func.name->len != name->len) continue;
        if (memcmp(d->func.name->loc, name->loc, name->len) != 0) continue;
        /* Disambiguate by full signature (arity + param kinds + const)
         * AND, for operators, by the operator-symbol token text. The
         * name is just "operator" for ALL of class C's operator
         * methods; without the disambiguation, the FIRST OOL with
         * matching arity wins — e.g. inline 'double_int& operator *=
         * (double_int)' in the header propagates its DECL_INLINE to
         * the queries for 'operator +', 'operator -', etc. */
        if (d->func.nparams != wanted_nparams) continue;
        bool params_match = true;
        for (int k = 0; k < wanted_nparams && params_match; k++) {
            Type *dp = (d->func.params[k] &&
                        d->func.params[k]->kind == ND_PARAM)
                          ? d->func.params[k]->param.ty : NULL;
            Type *wp = wanted_params ? wanted_params[k] : NULL;
            if (!dp || !wp) { params_match = (dp == wp); continue; }
            if (dp->kind != wp->kind) params_match = false;
        }
        if (!params_match) continue;
        if (d->func.is_const_method != wanted_is_const) continue;
        /* For operators, compare the operator-symbol text that
         * follows the 'operator' keyword. E.g. for 'operator + ',
         * the chars after the keyword are " + (...)". Matching the
         * non-whitespace prefix is enough to distinguish + / - / *=
         * / >>=  / etc. */
        if (wanted_op_after &&
            name->len == 8 && memcmp(name->loc, "operator", 8) == 0) {
            const char *wa = wanted_op_after->loc + wanted_op_after->len;
            const char *da = d->func.name->loc + d->func.name->len;
            while (*wa == ' ' || *wa == '\t') wa++;
            while (*da == ' ' || *da == '\t') da++;
            int wlen = 0, dlen = 0;
            while (wa[wlen] && wa[wlen] != ' ' && wa[wlen] != '\t' &&
                   wa[wlen] != '(' && wa[wlen] != '[' && wa[wlen] != '\n')
                wlen++;
            while (da[dlen] && da[dlen] != ' ' && da[dlen] != '\t' &&
                   da[dlen] != '(' && da[dlen] != '[' && da[dlen] != '\n')
                dlen++;
            if (wlen != dlen || memcmp(wa, da, wlen) != 0) continue;
        }
        return d->func.storage_flags;
    }
    return 0;
}

/* Check if a method on class_type is const-qualified. Scans class_def
 * members for the first match. */
static bool method_is_const(Type *class_type, Token *name) {
    if (!class_type || !name) return false;
    Node *cd = class_type->class_def;
    /* Fall back to TU lookup when the Type carries no class_def —
     * happens inside template-instantiated method bodies. */
    if (!cd) {
        Node *d = find_class_def_by_tag_args(class_type);
        if (d) cd = d;
    }
    if (!cd) return false;
    for (int i = 0; i < cd->class_def.nmembers; i++) {
        Node *m = cd->class_def.members[i];
        if (!m) continue;
        Token *mn = NULL;
        bool mc = false;
        if (m->kind == ND_FUNC_DEF) {
            mn = m->func.name;
            mc = m->func.is_const_method;
        } else if (m->kind == ND_VAR_DECL && m->var_decl.ty &&
                   m->var_decl.ty->kind == TY_FUNC) {
            mn = m->var_decl.name;
            mc = m->var_decl.ty->is_const;
        }
        if (!mn || mn->len != name->len) continue;
        if (memcmp(mn->loc, name->loc, name->len) != 0) continue;
        return mc;
    }
    return false;
}

/* Walk class (+bases) collecting operator methods whose computed
 * suffix matches op_suffix ("__plus", "__subscript", etc.). Separate
 * from collect_overload_candidates because operator methods are all
 * named 'operator' — the suffix is what distinguishes them. */
static void collect_operator_candidates(Type *class_type,
                                         OperatorKind op,
                                         Node **found, int *nfound, int cap) {
    if (!class_type) return;
    Node *cd = class_type->class_def;
    /* class_def may be unset on a Type obtained via a method return
     * type. Fall back through class_region->owner_type (the canonical
     * Type used when parsing the class body). Same fallback chain as
     * collect_overload_candidates — without it, the lhs of a binary
     * op that came from a hoisted struct-returning call can miss its
     * class def and resolve_operator_overload returns -1, emitting an
     * unmangled 'sf__T__bitor(...)' that doesn't match any definition.
     * Pattern: gcc 4.8 combine.c 'o = o.and_not(m) | i' — the hoist
     * materializes o.and_not(m) as __SF_temp_0, whose Type's class_def
     * is unhooked. */
    if (!cd && class_type->class_region &&
        class_type->class_region->owner_type &&
        class_type->class_region->owner_type->class_def)
        cd = class_type->class_region->owner_type->class_def;
    if (!cd) {
        Node *d = find_class_def_by_tag_args(class_type);
        if (d) cd = d;
    }
    if (!cd) return;
    for (int i = 0; i < cd->class_def.nmembers; i++) {
        Node *m = cd->class_def.members[i];
        if (!m) continue;
        bool is_def  = m->kind == ND_FUNC_DEF;
        bool is_decl = m->kind == ND_VAR_DECL && m->var_decl.ty &&
                        m->var_decl.ty->kind == TY_FUNC;
        if (!is_def && !is_decl) continue;
        Token *mn = is_def ? m->func.name : m->var_decl.name;
        if (!mn || mn->kind != TK_KW_OPERATOR) continue;
        if (operator_kind_from_method_name(mn) != op) continue;
        if (*nfound < cap) found[(*nfound)++] = m;
    }
    if (class_type->class_region) {
        for (int i = 0; i < class_type->class_region->nbases; i++) {
            DeclarativeRegion *br = class_type->class_region->bases[i];
            if (br && br->owner_type)
                collect_operator_candidates(br->owner_type, op,
                                             found, nfound, cap);
        }
    }
}

/* Same selection rules as resolve_overload, keyed by operator suffix
 * rather than by name token. Returns the winning decl's param count,
 * and sets *out_param_types to the decl's param types (for mangling).
 * Also returns the winning Node* via *out_best (nullable) so callers
 * can read per-candidate flags like is_const_method for _const
 * suffix mangling. Returns -1 if no candidate exists. */
static int resolve_operator_overload(Type *class_type,
                                      OperatorKind op,
                                      Type **arg_types, int nargs,
                                      bool receiver_is_const,
                                      Type ***out_param_types,
                                      Node **out_best) {
    static Type *pool[64];
    *out_param_types = NULL;
    if (out_best) *out_best = NULL;
    enum { MAX_CAND = 32 };
    Node *cands[MAX_CAND];
    int ncands = 0;
    collect_operator_candidates(class_type, op,
                                 cands, &ncands, MAX_CAND);
    if (ncands == 0) return -1;
    if (ncands == 1) {
        *out_param_types = pool;
        if (out_best) *out_best = cands[0];
        return copy_member_param_types(cands[0], pool);
    }
    /* Same const-aware selection as resolve_overload; see comment there.
     * N4659 §16.3.1.4 [over.match.funcs]/4. */
    Node *best = NULL;
    int best_score = -1;
    for (int i = 0; i < ncands; i++) {
        Node *c = cands[i];
        if (receiver_is_const && !candidate_is_const(c))
            continue;
        int s = overload_match_score(c, arg_types, nargs);
        if (s > best_score ||
            (s == best_score && best &&
             candidate_is_const(c) == receiver_is_const &&
             candidate_is_const(best) != receiver_is_const)) {
            best = c;
            best_score = s;
        }
    }
    if (!best) return -1;
    *out_param_types = pool;
    if (out_best) *out_best = best;
    return copy_member_param_types(best, pool);
}

/* Resolve a class operator overload and emit its mangled name to
 * stdout. Returns the winning decl's param count and sets *out_pty
 * to its param types (for arg-by-arg emit with emit_arg_for_param).
 * Optionally returns the winning Node* via *out_winner for callers
 * that need per-candidate flags (e.g. subscript's ref-return check).
 * Returns -1 if no candidate exists; in that case the mangled name
 * is still emitted WITHOUT a param-suffix (best-effort; will likely
 * fail to link — see the binary-op comment at the caller).
 *
 * Consolidates the scaffolding used by ND_BINARY, ND_ASSIGN (compound),
 * and ND_SUBSCRIPT — each of which previously duplicated the resolve
 * + mangle_class_tag + suffix + param-suffix + _const emission. The
 * ND_UNARY site has a different shape (only emits on np == 0) and
 * keeps its open-coded form. */
static int emit_class_op_mangled_name(Type *class_ty, OperatorKind op,
                                       Type **args, int nargs,
                                       bool receiver_is_const,
                                       Type ***out_pty,
                                       Node **out_winner) {
    Node *winner = NULL;
    int np = resolve_operator_overload(class_ty, op, args, nargs,
                                        receiver_is_const, out_pty, &winner);
    bool mc = candidate_is_const(winner);
    mangle_class_operator(class_ty, op,
                           np >= 0 ? *out_pty : args,
                           np >= 0 ? np : nargs,
                           mc);
    if (out_winner) *out_winner = winner;
    return np;
}

/* Emit a C function declarator — the 'ret name(params)' shape —
 * handling the case where ret itself is a function pointer, which
 * requires declarator-interleaving (N4659 §11.3 [dcl.meaning]):
 *   void (*getsig(void))(int);      // getsig returns a fn pointer
 * cannot be written as
 *   void (*)(int) getsig(void);     // abstract ret in decl pos — invalid
 * The inner-return + '(*' + name + '(...this-fn-params...)' + ')' +
 * '(' + fn-ptr-params + ')' shape is the only legal C form.
 *
 * For simple (non-fptr) return types, falls back to
 *   emit_type(ret); ' '; name; '('
 * with the caller emitting the function-params-and-close afterward.
 *
 * Returns true if the caller still needs to emit params and ')'
 * (the common case); false if this helper already emitted everything
 * including the closing ')' (fptr-return case).
 *
 * Actually, to keep call sites simple, this helper emits the params
 * inline. Callers just pass the full param set. */
static void emit_param_declarator(Type *ty, Token *name, int idx);
static void emit_param_list(Node **params, int nparams, bool variadic);
static void emit_func_header(Type *ret_ty, Token *name,
                              Node **params, int nparams, bool variadic) {
    /* Pointer-to-array return type — N4659 §11.3 [dcl.meaning]: the
     * function name MUST sit inside the parens with the '*':
     *   int (*f(args))[5]
     * Without this, emit_type would print 'int (*)[5] f(args)' which
     * isn't valid C. */
    {
        Type *t = ret_ty;
        int n_ptrs = 0;
        while (t && t->kind == TY_PTR) { n_ptrs++; t = t->base; }
        if (n_ptrs >= 1 && t && t->kind == TY_ARRAY) {
            Type *inner_dims_head = t;
            Type *elem = inner_dims_head;
            while (elem && elem->kind == TY_ARRAY) elem = elem->base;
            if (elem && elem->kind != TY_ARRAY && elem->kind != TY_PTR &&
                elem->kind != TY_FUNC) {
                emit_type(elem);
                fputs(" (", stdout);
                for (int i = 0; i < n_ptrs; i++) fputc('*', stdout);
                if (name)
                    fprintf(stdout, "%.*s", name->len, name->loc);
                emit_param_list(params, nparams, variadic);
                fputc(')', stdout);
                emit_array_dims(inner_dims_head);
                return;
            }
        }
    }
    bool ret_is_fptr = ret_ty && ret_ty->kind == TY_PTR &&
                       ret_ty->base && ret_ty->base->kind == TY_FUNC;
    if (ret_is_fptr) {
        Type *fty = ret_ty->base;
        emit_type(fty->ret);
        fputs(" (*", stdout);
        if (name)
            fprintf(stdout, "%.*s", name->len, name->loc);
        emit_param_list(params, nparams, variadic);
        fputs(")(", stdout);
        emit_func_param_types(fty);
        fputc(')', stdout);
        return;
    }
    emit_type(ret_ty);
    fputc(' ', stdout);
    if (name)
        fprintf(stdout, "%.*s", name->len, name->loc);
    emit_param_list(params, nparams, variadic);
}

/* Emit a parameter's 'type name' pair. C interleaves the name with
 * the declarator for function-pointer parameters ('int (*p)(int)'),
 * so we can't just emit_type then name. Arrays also decay to pointer
 * in parameter position (N4659 §11.3.4/5 [dcl.array]); emit_type
 * already does that decay. Unnamed parameters get __sf_unused_N
 * because C requires named params in definitions. */
/* Emit '(p1, p2, ..., pN)' or '(void)' or '(p1, ...)' — the parameter
 * list of a function declarator with surrounding parentheses. Centralises
 * the void-on-empty + variadic-trailing-... handling shared by every
 * function-emit site. N4659 §11.3.5 [dcl.fct]. */
static void emit_param_list(Node **params, int nparams, bool variadic) {
    fputc('(', stdout);
    if (nparams == 0 && !variadic) {
        fputs("void", stdout);
    } else {
        for (int i = 0; i < nparams; i++) {
            if (i > 0) fputs(", ", stdout);
            Node *p = params[i];
            emit_param_declarator(p->param.ty, p->param.name, i);
        }
        if (variadic) {
            if (nparams > 0) fputs(", ", stdout);
            fputs("...", stdout);
        }
    }
    fputc(')', stdout);
}

static void emit_param_declarator(Type *ty, Token *name, int idx) {
    /* Pointer-to-array parameters keep their array shape — C's
     * function-arg adjustment (N4659 §11.3.5/5 [dcl.fct]) already
     * decays a top-level array to a pointer; for already-pointer-to-
     * array we want the interleaved 'T (*p)[N]' so the bound stays
     * visible to sizeof inside the callee. */
    if (emit_pointer_to_array_declarator(ty,
            name ? name : NULL)) {
        if (!name) fprintf(stdout, " __sf_unused_%d", idx);
        return;
    }
    /* N4659 §11.3 [dcl.meaning]: function pointer parameters
     * require declarator-interleaving in C:
     *   void (*name)(int)     — pointer to function
     *   void (**name)(int)    — pointer to pointer to function
     * Count the indirection levels to emit the right number of *s. */
    Type *fty = NULL;
    int ptr_depth = 0;
    {
        Type *t = ty;
        /* Reference-to-function lowers to a single function-pointer
         * level in C (N4659 §11.3.2 [dcl.ref]). Peel once so the loop
         * below can land on the underlying TY_FUNC and emit the
         * '(*name)(args)' declarator. Pattern: template instantiation
         * substitutes 'void()' for F in 'const F& f', producing
         * TY_REF(TY_FUNC) at the parameter site. */
        if (t && (t->kind == TY_REF || t->kind == TY_RVALREF) &&
            t->base && t->base->kind == TY_FUNC) {
            fty = t->base;
            ptr_depth = 1;
        }
        while (!fty && t && t->kind == TY_PTR && t->base) {
            ptr_depth++;
            if (t->base->kind == TY_FUNC) { fty = t->base; break; }
            t = t->base;
        }
        /* Bare function-type parameter: in C these decay to function
         * pointers at the ABI, but the declarator still needs the
         * '(*name)(params)' syntax — plain 'ret name(params)' in a
         * param list would be a nested function. Pattern: gcc 4.8
         * sel-sched-ir.h _succ_iter_cond's 'bool check(edge, succ*)'
         * parameter. N4659 §11.3.5/5 [dcl.fct]. */
        if (!fty && t && t->kind == TY_FUNC) {
            fty = t;
            ptr_depth = 1;  /* emit as '(*name)(...)'; C decays to this. */
        }
    }
    if (fty) {
        emit_type(fty->ret);
        fputs(" (", stdout);
        for (int i = 0; i < ptr_depth; i++) fputc('*', stdout);
        if (name)
            fprintf(stdout, "%.*s", name->len, name->loc);
        else
            fprintf(stdout, "__sf_unused_%d", idx);
        fputs(")(", stdout);
        emit_func_param_types(fty);
        fputc(')', stdout);
        return;
    }
    emit_type(ty);
    if (name)
        fprintf(stdout, " %.*s", name->len, name->loc);
    else
        fprintf(stdout, " __sf_unused_%d", idx);
}

/* ------------------------------------------------------------------ */
/* Expression emission                                                */
/* ------------------------------------------------------------------ */

static void emit_expr(Node *n);
static bool free_func_name_is_overloaded(Token *name);
static bool emit_free_func_ident_via_ffsig(Token *name);
static bool ffsig_is_first_c_linkage(Token *name, Type **params, int nparams);
static void emit_free_func_symbol(Token *name, Token *asm_name,
                                   bool c_linkage,
                                   Type **params, int nparams);
static void emit_free_func_mangled_name(Token *name, Type **param_types,
                                         int nparams);

/* The asm-label token's payload includes the surrounding quotes
 * (e.g. "strchr" with len=8). Strip them when emitting. */
static void emit_asm_name(Token *asm_tok) {
    if (!asm_tok || asm_tok->len < 2) return;
    fwrite(asm_tok->loc + 1, 1, asm_tok->len - 2, stdout);
}

/* Emit the C symbol for a free-function reference / declaration —
 * the single source of truth for "what name does this become?".
 * Resolution order (highest-precedence first):
 *
 *   1. asm_name set       → emit asm_name string verbatim. The user
 *                           explicitly named the C symbol via the GNU
 *                           __asm("name") declarator-suffix.
 *
 *   2. c_linkage          → emit bare '<name>'. N4659 §10.5/6
 *                           [dcl.link]: a name with C linkage refers
 *                           to a single C ABI function; we MUST emit
 *                           the bare name so the linker resolves
 *                           against libc / other C TUs.
 *
 *   3. name is overloaded → emit '<name>_p_<params>_pe_'. There are
 *                           2+ DISTINCT signatures registered for
 *                           this name (checked via free_func_name_
 *                           is_overloaded over the pre-pass table),
 *                           so the C-level identifier needs the
 *                           param-suffix to be unique.
 *
 *   4. otherwise          → emit '<name>' bare.
 *
 * The c_linkage gate is per-DECL, NOT per-name. Real headers mix
 * linkages — e.g. <cmath> has 'extern "C" double acos(double)' AND
 * 'extern "C++" inline float acos(float)' for the same name. The C
 * decl emits as bare 'acos' (libm symbol); the C++ overloads emit
 * mangled. Decl-emit dedup (ffsig_is_first_c_linkage) prevents the
 * bare emissions from clashing on the C ABI name when there are
 * multiple c_linkage decls (e.g. <cstring>'s dual strchr).
 *
 * Each caller still computes `params` (often with default-argument
 * injection or resolved-decl substitution) — this helper only owns
 * the asm/c-linkage/mangle/bare decision. */
static void emit_free_func_symbol(Token *name, Token *asm_name,
                                   bool c_linkage,
                                   Type **params, int nparams) {
    if (asm_name) {
        emit_asm_name(asm_name);
    } else if (c_linkage) {
        fprintf(stdout, "%.*s", name->len, name->loc);
    } else if (free_func_name_is_overloaded(name)) {
        emit_free_func_mangled_name(name, params, nparams);
    } else {
        fprintf(stdout, "%.*s", name->len, name->loc);
    }
}

static const char *binop_str(TokenKind k) {
    switch (k) {
    case TK_PLUS:    return "+";
    case TK_MINUS:   return "-";
    case TK_STAR:    return "*";
    case TK_SLASH:   return "/";
    case TK_PERCENT: return "%";
    case TK_LT:      return "<";
    case TK_LE:      return "<=";
    case TK_GT:      return ">";
    case TK_GE:      return ">=";
    case TK_EQ:      return "==";
    case TK_NE:      return "!=";
    case TK_LAND:    return "&&";
    case TK_LOR:     return "||";
    case TK_AMP:     return "&";
    case TK_PIPE:    return "|";
    case TK_CARET:   return "^";
    case TK_SHL:     return "<<";
    case TK_SHR:     return ">>";
    case TK_ASSIGN:  return "=";
    case TK_PLUS_ASSIGN:    return "+=";
    case TK_MINUS_ASSIGN:   return "-=";
    case TK_STAR_ASSIGN:    return "*=";
    case TK_SLASH_ASSIGN:   return "/=";
    case TK_PERCENT_ASSIGN: return "%=";
    case TK_AMP_ASSIGN:     return "&=";
    case TK_PIPE_ASSIGN:    return "|=";
    case TK_CARET_ASSIGN:   return "^=";
    case TK_SHL_ASSIGN:     return "<<=";
    case TK_SHR_ASSIGN:     return ">>=";
    default: return "?";
    }
}

/* Map binary operator TokenKind to mangled method suffix.
 * Returns NULL if the operator is not overloadable or we don't
 * support rewriting it yet. Must match the suffixes in
 * emit_operator_method_name. */
/* Map a binary-operator TokenKind to OperatorKind. Returns
 * OP_UNKNOWN for tokens that aren't class-overloadable binops. */
static OperatorKind binop_to_operator_kind(TokenKind op) {
    switch (op) {
    case TK_PLUS:           return OP_PLUS;
    case TK_MINUS:          return OP_MINUS;
    case TK_STAR:           return OP_STAR;
    case TK_SLASH:          return OP_SLASH;
    case TK_PERCENT:        return OP_MOD;
    case TK_AMP:            return OP_AMP;
    case TK_PIPE:           return OP_PIPE;
    case TK_CARET:          return OP_CARET;
    case TK_SHL:            return OP_LSHIFT;
    case TK_SHR:            return OP_RSHIFT;
    case TK_EQ:             return OP_EQ;
    case TK_NE:             return OP_NE;
    case TK_LT:             return OP_LT;
    case TK_GT:             return OP_GT;
    case TK_LE:             return OP_LE;
    case TK_GE:             return OP_GE;
    case TK_LAND:           return OP_LAND;
    case TK_LOR:            return OP_LOR;
    case TK_PLUS_ASSIGN:    return OP_PLUS_ASSIGN;
    case TK_MINUS_ASSIGN:   return OP_MINUS_ASSIGN;
    case TK_STAR_ASSIGN:    return OP_MUL_ASSIGN;
    case TK_SLASH_ASSIGN:   return OP_DIV_ASSIGN;
    case TK_AMP_ASSIGN:     return OP_BITAND_ASSIGN;
    case TK_PIPE_ASSIGN:    return OP_BITOR_ASSIGN;
    case TK_CARET_ASSIGN:   return OP_XOR_ASSIGN;
    case TK_SHL_ASSIGN:     return OP_LSHIFT_ASSIGN;
    case TK_SHR_ASSIGN:     return OP_RSHIFT_ASSIGN;
    case TK_PERCENT_ASSIGN: return OP_MOD_ASSIGN;
    default:                return OP_UNKNOWN;
    }
}

static const char *unop_str(TokenKind k) {
    switch (k) {
    case TK_PLUS:  return "+";
    case TK_MINUS: return "-";
    case TK_STAR:  return "*";
    case TK_AMP:   return "&";
    case TK_EXCL:  return "!";
    case TK_TILDE: return "~";
    case TK_INC:   return "++";
    case TK_DEC:   return "--";
    default: return "?";
    }
}

static void emit_token_text(Token *t) {
    if (!t) { fputs("?", stdout); return; }
    fprintf(stdout, "%.*s", t->len, t->loc);
}

/* Emit an enum-body token with C-friendly literal substitution.
 * Enum bodies are captured as raw token ranges and re-emitted
 * verbatim, so C++ keywords 'true' / 'false' would land in the
 * C output unresolved. Substitute them with their numeric values.
 * Pattern: gcc 4.8 cp/semantics.c
 *   enum { any = false, rval = true }; */
static void emit_enum_body_token(Token *t) {
    if (!t) { fputs("?", stdout); return; }
    if (t->len == 4 && memcmp(t->loc, "true", 4) == 0) {
        fputc('1', stdout);
        return;
    }
    if (t->len == 5 && memcmp(t->loc, "false", 5) == 0) {
        fputc('0', stdout);
        return;
    }
    fprintf(stdout, "%.*s", t->len, t->loc);
}

/* Emit the enumerator list of an enum type as C source — the
 * sequence between '{' and '}'. Used by every site that re-emits
 * a captured enum body (top-level, inline, member, typedef, etc.).
 * Caller is responsible for the surrounding 'enum tag { ... };'
 * shape. */
static void emit_enum_body(Type *ety) {
    for (int i = 0; i < ety->enum_ntokens; i++) {
        Token *t = &ety->enum_tokens[i];
        if (t->has_space && i > 0) fputc(' ', stdout);
        emit_enum_body_token(t);
    }
}

static void emit_expr(Node *n) {
    if (!n) return;
    /* Universal temp substitution: any hoisted node — ND_CALL, ND_BINARY
     * (operator overload result), etc. — carries the synthesized temp
     * name. Substitute at the top of emit_expr so every kind benefits
     * without per-kind branches. The ND_CALL kind also checks further
     * down (historical, before this universal check), which is a
     * redundant but harmless shortcut. */
    if (n->codegen_temp_name) {
        fputs(n->codegen_temp_name, stdout);
        return;
    }
    switch (n->kind) {
    case ND_NUM:
        /* N4659 §5.13.2 [lex.icon]: an integer-literal's type depends
         * on suffix (u/U, l/L, ll/LL, z/Z) and base. Dropping the
         * suffix changes the type and silently introduces UB — e.g.
         * '1L << 32' becomes '1 << 32', which on a 32-bit int target
         * is undefined and on a 64-bit-int target evaluates to 0L
         * instead of 1L<<32. The original token text already carries
         * the suffix, so prefer emitting it verbatim. Synthesized
         * ND_NUM nodes (no source token) fall back to a width-safe
         * suffix-bearing format. */
        if (n->tok && n->tok->loc && n->tok->len > 0)
            fwrite(n->tok->loc, 1, (size_t)n->tok->len, stdout);
        else if (n->num.is_signed)
            fprintf(stdout, "%lldLL", (long long)n->num.lo);
        else
            fprintf(stdout, "%lluULL", (unsigned long long)n->num.lo);
        return;
    case ND_FNUM:
        fprintf(stdout, "%g", n->fnum.fval);
        return;
    case ND_CHAR:
        if (n->chr.tok) fprintf(stdout, "%.*s", n->chr.tok->len, n->chr.tok->loc);
        return;
    case ND_BOOL_LIT:
        /* C doesn't have true/false without stdbool.h. Emit 1/0. */
        if (n->tok && n->tok->len == 4 &&
            memcmp(n->tok->loc, "true", 4) == 0)
            fputc('1', stdout);
        else
            fputc('0', stdout);
        return;
    case ND_NULLPTR:
        fputs("((void*)0)", stdout);
        return;
    case ND_LAMBDA: {
        /* Capturing lambda — N4659 §8.1.5 [expr.prim.lambda]. The
         * lambda-expression evaluates to a value of the closure type
         * (§8.1.5/2 — unique, unnamed non-union class), built here
         * as a C99 compound literal (§6.5.2.5) with one initializer
         * per capture per §8.1.5.2 [expr.prim.lambda.capture]: the
         * source identifier for by-value, '&ident' for by-ref since
         * the closure member is T*.
         *
         * Captureless lambdas don't reach this case — the parser
         * returns ND_IDENT naming the synthesised fn directly so
         * the §8.1.5/6 conversion-to-fn-pointer falls out of normal
         * fn-to-ptr decay (no closure type emitted). */
        Type *cty = n->lambda.closure_type;
        fputs("(struct ", stdout);
        if (cty) mangle_class_tag(cty);
        fputs("){", stdout);
        for (int i = 0; i < n->lambda.ncaptures; i++) {
            Capture *c = &n->lambda.captures[i];
            if (i > 0) fputs(", ", stdout);
            /* If the captured entity is itself a reference (T&), its
             * C lowering is a pointer T*. The closure member's type
             * has the reference stripped (§8.1.5.2/15 + the parser's
             * ref-strip when allocating the closure member), so at
             * the init site we must dereference the C-level pointer
             * to get the underlying T value. Both by-value and by-ref
             * captures need this: by-value because the closure stores
             * T, by-ref because '&(*p)' on a ref-param-pointer 'p'
             * yields the same 'p' but with the right pointer level. */
            bool source_is_ref = c->resolved_type &&
                (c->resolved_type->kind == TY_REF ||
                 c->resolved_type->kind == TY_RVALREF);
            if (c->is_this) {
                /* [this] capture — closure stores Class*. Emit 'this'. */
                fputs("this", stdout);
            } else if (c->by_ref) {
                if (source_is_ref) {
                    /* Source is already a ref → C-level T*; use it
                     * directly as the closure's T* member. */
                    fprintf(stdout, "%.*s", c->name->len, c->name->loc);
                } else {
                    fputc('&', stdout);
                    fprintf(stdout, "%.*s", c->name->len, c->name->loc);
                }
            } else {
                if (source_is_ref) {
                    /* Deref the C-level T* to get T for the closure's
                     * T member. */
                    fputs("(*", stdout);
                    fprintf(stdout, "%.*s", c->name->len, c->name->loc);
                    fputc(')', stdout);
                } else {
                    fprintf(stdout, "%.*s", c->name->len, c->name->loc);
                }
            }
        }
        fputc('}', stdout);
        return;
    }
    case ND_STR:
        /* Emit the whole adjacent-string run. C also concatenates
         * adjacent string literals (C99 §6.4.5/4), so emitting them
         * with whitespace between is fine — and preserves source
         * shape better than pre-joining. */
        for (int i = 0; i < n->str.ntoks; i++) {
            Token *t = n->str.tok + i;
            if (i > 0) fputc(' ', stdout);
            fprintf(stdout, "%.*s", t->len, t->loc);
        }
        return;
    case ND_IDENT:
        /* __func__ / __FUNCTION__ rewrite — N4659 §8.4.1/8 says the
         * value is the unqualified function name. If we just emitted
         * the bare identifier, the host C compiler would expand to the
         * mangled name (e.g. '_ZN3y8a4zqjxEic'). Substitute a string
         * literal of the original source name; destructors get a '~'
         * prefix (the func node carries the class-name token without
         * the tilde). __PRETTY_FUNCTION__ remains a pass-through —
         * the verbose-with-types form would need full signature
         * reconstruction. Patterns: g++.dg/ext/fnname{1,2,3}.C. */
        if (n->ident.name && g_current_func_src_name &&
            ((n->ident.name->len == 8 &&
              memcmp(n->ident.name->loc, "__func__", 8) == 0) ||
             (n->ident.name->len == 12 &&
              memcmp(n->ident.name->loc, "__FUNCTION__", 12) == 0))) {
            fputc('"', stdout);
            if (g_current_func_is_dtor) fputc('~', stdout);
            fprintf(stdout, "%.*s",
                    g_current_func_src_name->len,
                    g_current_func_src_name->loc);
            fputc('"', stdout);
            return;
        }
        /* Lambda capture rewrite — N4659 §8.1.5.2 [expr.prim.lambda
         * .capture]. Inside a capturing-lambda body
         * (g_current_lambda_fn set), an identifier whose name matches
         * a capture is emitted as access through the closure pointer:
         * '__self->x' for a by-value capture (closure member is T —
         * §8.1.5.2/15), '(*__self->x)' for by-ref (closure member is
         * T*; the rebind models §8.1.5.2 reference-capture semantics
         * in C, which has no native references). The closure's __self
         * parameter itself is emitted as-is.
         *
         * '[this]' (§8.1.5.2/8): the captured implicit object pointer
         * is stored as '__self->__this' (closure member type Class*).
         * A bare 'this' inside the lambda body emits as that access;
         * implicit-this member references (e.g. 'm' resolving to the
         * enclosing class's member) handled below in the implicit_this
         * branch. */
        if (g_current_lambda_fn && n->ident.name) {
            Token *nm = n->ident.name;
            bool has_this_cap = false;
            for (int i = 0; i < g_current_lambda_fn->func.ncaptures; i++) {
                Capture *c = &g_current_lambda_fn->func.captures[i];
                if (c->is_this) { has_this_cap = true; continue; }
                if (!c->name) continue;
                if (c->name->len == nm->len &&
                    memcmp(c->name->loc, nm->loc, nm->len) == 0) {
                    if (c->by_ref) {
                        fputs("(*__self->", stdout);
                        fprintf(stdout, "%.*s", nm->len, nm->loc);
                        fputs(")", stdout);
                    } else {
                        fputs("__self->", stdout);
                        fprintf(stdout, "%.*s", nm->len, nm->loc);
                    }
                    return;
                }
            }
            if (has_this_cap && nm->kind == TK_KW_THIS) {
                fputs("__self->__this", stdout);
                return;
            }
        }
        /* SHORTCUT (ours, not the standard): gcc vec.h defines
         * 'extern vnull vNULL;' with a template conversion operator
         * (N4659 §16.3.2 [class.conv.fct]). In C there are no
         * conversion operators; emit '{0}' (C99 §6.7.8/21) which
         * zero-initializes the struct (null pointer member). */
        if (n->ident.name && n->ident.name->len == 5 &&
            memcmp(n->ident.name->loc, "vNULL", 5) == 0) {
            fputs("{0}", stdout);
            return;
        }
        /* Anonymous-union member rewrite — see emit_anon_member_rewrite
         * and the block-scope ND_CLASS_DEF anon branch. Bare references
         * to a member of an active anonymous block-scope union/struct
         * become '__sf_anon_inst_<id>.member'. */
        if (n->ident.name && emit_anon_member_rewrite(n->ident.name))
            return;
        if (n->ident.implicit_this && !g_current_method_is_static) {
            /* Static data members (§10.1.1/4 [dcl.stc]) lower to a
             * TU-scope variable 'sf__<class>__<name>', not a struct
             * field — emit the mangled name directly, no 'this->'
             * prefix. */
            Declaration *sd = n->ident.resolved_decl;
            if (sd && sd->is_static_member && sd->home &&
                sd->home->owner_type) {
                mangle_class_tag(sd->home->owner_type);
                fputs("__", stdout);
                emit_token_text(n->ident.name);
                return;
            }
            /* Inside a capturing lambda with '[this]' (§8.1.5.2/8),
             * implicit-this references go through the closure's
             * '__this' member instead of the lambda's non-existent
             * 'this' parameter. */
            bool lam_has_this = false;
            if (g_current_lambda_fn) {
                for (int i = 0; i < g_current_lambda_fn->func.ncaptures; i++)
                    if (g_current_lambda_fn->func.captures[i].is_this) {
                        lam_has_this = true; break;
                    }
            }
            if (lam_has_this) fputs("__self->__this->", stdout);
            else              fputs("this->",            stdout);
            /* If the resolved declaration lives in a BASE class of
             * the current class, walk through the __sf_base chain
             * — N4659 §11 [class.derived]. */
            Declaration *d = n->ident.resolved_decl;
            if (d && d->home && d->home->owner_type && g_current_class_def) {
                Type *cur = g_current_class_def->class_def.ty;
                Type *tgt = d->home->owner_type;
                int path[8];
                int len = find_base_path(cur, tgt, path, 8);
                emit_base_chain(path, len);
            }
        }
        /* N4659 §11.3.2 [dcl.ref]: reference params are lowered to
         * pointers in C. When used as a value (rvalue), deref them.
         * Skip deref when the ident is the LHS of an assignment
         * (handled by the caller) or when used with & (address-of). */
        if (!n->ident.implicit_this && !g_suppress_ref_deref &&
            is_ref_param(n->ident.name)) {
            fputs("(*", stdout);
            emit_token_text(n->ident.name);
            fputc(')', stdout);
        } else {
            g_suppress_ref_deref = false;
            /* Implicit-this references emit the bare name; the receiver
             * is supplied by the surrounding member-access. Locals,
             * struct fields, and any non-function ident also emit
             * bare — only resolved-to-a-function references go through
             * the asm/mangle/bare resolver, since only functions can
             * be in the free-func overload table.
             *
             * Without this gate, a local 'int index;' (shadowing
             * libc's index() in genautomata.c) gets mangled to
             * 'index_p_void_pe_' because the libc index is in the
             * overload table. */
            Declaration *rd = n->ident.resolved_decl;
            bool rd_is_fn = rd && rd->type && rd->type->kind == TY_FUNC;
            if (n->ident.implicit_this) {
                emit_token_text(n->ident.name);
            } else if (rd_is_fn) {
                emit_free_func_symbol(n->ident.name,
                                       rd->asm_name,
                                       rd->c_linkage,
                                       rd->type->params,
                                       rd->type->nparams);
            } else if (!rd && emit_free_func_ident_via_ffsig(n->ident.name)) {
                /* sema didn't resolve this ident (file-scope aggregate
                 * initializer fall-through, etc.) — the g_ffsig_seen
                 * table still knows the function and its params; route
                 * through emit_free_func_symbol so the call matches
                 * the def's mangled emit. Critically, this branch
                 * only fires when rd is NULL: a local variable
                 * shadowing a same-named function (legal C; gcc 4.8
                 * genrecog.c's 'static struct decision *new_decision
                 * (...) { struct decision *new_decision = ...; ... }')
                 * must keep the local-var bare emit path. */
            } else {
                /* Bare emit for locals, parameters, file-scope vars,
                 * and any function ident the ffsig lookup didn't
                 * recognise. */
                emit_token_text(n->ident.name);
            }
        }
        return;
    case ND_BINARY: {
        /* Pointer-to-data-member access — N4659 §8.5 [expr.mptr.oper].
         * 'obj .* pmem'   → *(T *)((char *)&(obj) + (pmem))
         * 'obj ->* pmem'  → *(T *)((char *)(obj) + (pmem))
         * pmem is __PTRDIFF_TYPE__ holding the member's byte offset
         * in the owning class. The cast type T comes from the pmem's
         * Type::base. */
        if (n->binary.op == TK_DOTSTAR || n->binary.op == TK_ARROWSTAR) {
            Type *pty = n->binary.rhs ? n->binary.rhs->resolved_type : NULL;
            Type *field_ty = (pty && pty->kind == TY_PMEM) ? pty->base : NULL;
            fputs("(*(", stdout);
            if (field_ty) emit_type(field_ty);
            else          fputs("int", stdout);
            fputs(" *)((char *)", stdout);
            if (n->binary.op == TK_DOTSTAR) fputc('&', stdout);
            fputc('(', stdout);
            emit_expr(n->binary.lhs);
            fputs(") + (", stdout);
            emit_expr(n->binary.rhs);
            fputs(")))", stdout);
            return;
        }
        /* Overloaded-operator dispatch — N4659 §16.5 [over.oper]:
         * 'a op b' where a has class type calls the class's operator.
         *
         * Value lhs (TY_STRUCT / TY_UNION) → dispatch.
         * Reference lhs (TY_REF — becomes T* in emitted C but still
         *   has value semantics) → dereference and dispatch.
         * Pointer lhs (TY_PTR) → native C pointer arithmetic /
         *   comparison. Do NOT dispatch — 'pchdir != NULL' on a
         *   DIR* must emit as a plain pointer comparison, not
         *   sf__DIR__ne(&pchdir, NULL). */
        Type *lhs_ty = n->binary.lhs ? n->binary.lhs->resolved_type : NULL;
        if (ty_is_ref(lhs_ty)) lhs_ty = lhs_ty->base;
        /* Operator-overload rvalue LHS like '(a + b) == c' where the
         * + is an overloaded operator on structs: sema leaves the
         * ND_BINARY+'s resolved_type NULL (common_arith_type bails).
         * Infer from an operand. Pattern: gcc 4.8 fold-const.c
         * 'c1.bitand(c2) == c1' — lhs is ND_CALL already (its
         * resolved_type is set); but '(a op b) == c' where the op
         * is struct-operator gets caught here. */
        if (!lhs_ty && n->binary.lhs &&
            (n->binary.lhs->kind == ND_BINARY ||
             n->binary.lhs->kind == ND_UNARY)) {
            Node *op = (n->binary.lhs->kind == ND_BINARY)
                ? n->binary.lhs->binary.lhs
                : n->binary.lhs->unary.operand;
            if (op) lhs_ty = op->resolved_type;
            if (ty_is_ref(lhs_ty)) lhs_ty = lhs_ty->base;
        }
        if (lhs_ty && (lhs_ty->kind == TY_STRUCT || lhs_ty->kind == TY_UNION) &&
            lhs_ty->tag) {
            OperatorKind opk = binop_to_operator_kind(n->binary.op);
            if (opk != OP_UNKNOWN) {
                /* Resolve the specific operator overload by matching
                 * the rhs type against each candidate's sole param
                 * (ignoring the implicit 'this'). Fall back to
                 * unsuffixed mangle if the class has no such operator
                 * — this happens when the operator is inherited from
                 * a base we don't fully model (e.g. std::ios_base
                 * bitmask ops synthesised by the preprocessor) — the
                 * resulting symbol will likely fail to link, which
                 * is still a better diagnostic than a silent miscompile. */
                Type *rhs_ty = n->binary.rhs ? n->binary.rhs->resolved_type : NULL;
                Type *args[1] = { rhs_ty };
                Type **pty = NULL;
                bool lhs_const = receiver_type_is_const(
                    n->binary.lhs ? n->binary.lhs->resolved_type : NULL);
                int np = emit_class_op_mangled_name(lhs_ty, opk, args, 1,
                                                     lhs_const, &pty, NULL);
                fputc('(', stdout);
                emit_addrof_for_this(n->binary.lhs);
                fputs(", ", stdout);
                emit_arg_for_param(n->binary.rhs,
                                    (pty && np > 0) ? pty[0] : NULL);
                fputc(')', stdout);
                return;
            }
        }
        fputc('(', stdout);
        emit_expr(n->binary.lhs);
        fprintf(stdout, " %s ", binop_str(n->binary.op));
        emit_expr(n->binary.rhs);
        fputc(')', stdout);
        return;
    }
    case ND_ASSIGN: {
        /* Ternary as l-value — N4659 §8.16 [expr.cond]/5. In C++,
         * 'c ? a : b' is an lvalue when a and b are lvalues of the
         * same type. In C the ternary is always an rvalue, so we
         * rewrite '(c ? a : b) = rhs' as '*(c ? &a : &b) = rhs'.
         * Works for scalars and structs alike (struct assignment in
         * C is legal). Class-typed LHS with overloaded operator= is
         * handled below by the existing class-operator dispatch — it
         * runs against the deref'd ternary, which is an lvalue. */
        if (n->binary.op == TK_ASSIGN && n->binary.lhs &&
            n->binary.lhs->kind == ND_TERNARY) {
            Node *tern = n->binary.lhs;
            /* Class-operator overload dispatch for struct LHS — we
             * can't dispatch through a pointer, so fall through to
             * the regular path if the LHS is a class type with an
             * operator= overload. Detection mirrors below. */
            Type *t_lhs = tern->resolved_type;
            if (ty_is_ref(t_lhs)) t_lhs = t_lhs->base;
            bool is_struct = t_lhs &&
                (t_lhs->kind == TY_STRUCT || t_lhs->kind == TY_UNION);
            if (!is_struct) {
                fputc('(', stdout);
                fputs("*(", stdout);
                emit_expr(tern->ternary.cond);
                fputs(" ? &(", stdout);
                emit_expr(tern->ternary.then_);
                fputs(") : &(", stdout);
                emit_expr(tern->ternary.else_);
                fputs(")) = ", stdout);
                emit_expr(n->binary.rhs);
                fputc(')', stdout);
                return;
            }
            /* Struct LHS: route '*(c ? &a : &b)' as the new LHS and
             * re-enter the assignment path so class operator= dispatch
             * fires. Since we can't easily re-call ourselves with a
             * modified node, just emit the deref-of-ptr-ternary form
             * directly — works for plain struct assignment; user-
             * defined operator= will be missed here (limitation
             * deferred). */
            fputc('(', stdout);
            fputs("*(", stdout);
            emit_expr(tern->ternary.cond);
            fputs(" ? &(", stdout);
            emit_expr(tern->ternary.then_);
            fputs(") : &(", stdout);
            emit_expr(tern->ternary.else_);
            fputs(")) = ", stdout);
            emit_expr(n->binary.rhs);
            fputc(')', stdout);
            return;
        }
        /* Compound assignment on struct: a += b → Class__plus_assign(&a, b)
         * Same value-vs-pointer distinction as ND_BINARY — TY_PTR lhs
         * is native C pointer arithmetic, not a class operator. */
        Type *lhs_ty = n->binary.lhs ? n->binary.lhs->resolved_type : NULL;
        if (ty_is_ref(lhs_ty)) lhs_ty = lhs_ty->base;
        if (lhs_ty && (lhs_ty->kind == TY_STRUCT || lhs_ty->kind == TY_UNION) &&
            lhs_ty->tag && n->binary.op != TK_ASSIGN) {
            OperatorKind opk = binop_to_operator_kind(n->binary.op);
            if (opk != OP_UNKNOWN) {
                Type *rhs_ty = n->binary.rhs ? n->binary.rhs->resolved_type : NULL;
                Type *args[1] = { rhs_ty };
                Type **pty = NULL;
                bool lhs_const = receiver_type_is_const(
                    n->binary.lhs ? n->binary.lhs->resolved_type : NULL);
                int np = emit_class_op_mangled_name(lhs_ty, opk, args, 1,
                                                     lhs_const, &pty, NULL);
                fputc('(', stdout);
                emit_addrof_for_this(n->binary.lhs);
                fputs(", ", stdout);
                emit_arg_for_param(n->binary.rhs,
                                    (pty && np > 0) ? pty[0] : NULL);
                fputc(')', stdout);
                return;
            }
        }
        /* mutable class data member — N4659 §10.1.1/8 [dcl.stc].
         * C has no mutable, so cast away constness at the write site
         * by emitting `*(T*)&<lhs> = rhs`. Fires for plain '=' when:
         *   - LHS is `this->mut_field` (ident with implicit_this), or
         *   - LHS is `obj.mut_field` / `obj->mut_field`, or
         *   - LHS is a chain `obj.mutable_field.regular_field` —
         *     mutable anywhere in the receiver chain authorises the
         *     cast at the leaf. */
        if (n->binary.op == TK_ASSIGN && n->binary.lhs) {
            Type *leaf_ty = NULL;
            bool need_cast = false;
            Node *lhs = n->binary.lhs;
            /* A leaf field declared 'const T' (top-level const, e.g.
             * `T *const m_field;`) cannot be assigned in C even from
             * a constructor body — N4659 §15.6.2 [class.base.init]
             * permits the in-class initializer in C++, but our C
             * lowering of mem-initialisers becomes plain assignment.
             * Emit the same cast trick used for mutable so cc accepts
             * the ctor's writes to const fields. */
            if (lhs->kind == ND_IDENT && lhs->ident.implicit_this &&
                lhs->ident.resolved_decl) {
                Declaration *fd = lhs->ident.resolved_decl;
                if (fd->is_mutable ||
                    (fd->type && fd->type->is_const)) {
                    need_cast = true;
                    leaf_ty = fd->type;
                }
            } else if (lhs->kind == ND_MEMBER) {
                /* Walk the chain; mutable anywhere or top-level const
                 * on the leaf authorise the cast at the leaf. */
                Node *cur = lhs;
                while (cur && cur->kind == ND_MEMBER) {
                    Type *ot = cur->member.obj ? cur->member.obj->resolved_type : NULL;
                    if (ot && ot->kind == TY_PTR) ot = ot->base;
                    if (ot && (ot->kind == TY_REF || ot->kind == TY_RVALREF))
                        ot = ot->base;
                    Declaration *fd = (ot && ot->class_region && cur->member.member) ?
                        lookup_in_scope(ot->class_region,
                                        cur->member.member->loc,
                                        cur->member.member->len) : NULL;
                    if (fd && (fd->is_mutable ||
                               (cur == lhs && fd->type && fd->type->is_const))) {
                        need_cast = true;
                        if (cur == lhs && fd->type) leaf_ty = fd->type;
                        break;
                    }
                    cur = cur->member.obj;
                }
                if (!leaf_ty) leaf_ty = lhs->resolved_type;
                if (!need_cast && cur && cur->kind == ND_IDENT &&
                    cur->ident.implicit_this && cur->ident.resolved_decl &&
                    cur->ident.resolved_decl->is_mutable)
                    need_cast = true;
            }
            if (need_cast && leaf_ty) {
                /* Strip top-level const so the cast target is
                 * assignable after deref. */
                Type stripped = *leaf_ty;
                stripped.is_const = false;
                fputs("(*(", stdout);
                emit_type(&stripped);
                fputs("*)&(", stdout);
                emit_expr(n->binary.lhs);
                fputs(") = ", stdout);
                emit_expr(n->binary.rhs);
                fputs(")", stdout);
                return;
            }
        }
        /* Reference-returning function call used as lvalue —
         * N4659 §6.10/4 [basic.lval]: a function call returning T& is
         * an lvalue. Our C lowering emits the call as T*; the result
         * site needs '*' to recover the lvalue for assignment. Without
         * the wrap, `f() = x` and `f() += x` reach cc as `T* = ...`
         * and fail "lvalue required as left operand of assignment".
         * Already handled for method calls via mangle/emit-side ref
         * tracking; this covers free-function ref-returns. */
        {
            Node *lhs = n->binary.lhs;
            if (lhs && lhs->kind == ND_CALL && ty_is_ref(lhs->resolved_type) &&
                !ty_is_ref(g_current_func_ret_ty)) {
                fputs("((*", stdout);
                emit_expr(lhs);
                fprintf(stdout, ") %s ", binop_str(n->binary.op));
                emit_expr(n->binary.rhs);
                fputc(')', stdout);
                return;
            }
        }
        /* Brace-init-list as RHS of assignment to a struct: lower to a
         * C99 compound literal so the construct is a real expression
         * the assignment can consume. N4659 §11.6.4 [dcl.init.list]
         * permits `obj = {a, b, c}`; C requires `obj = (T){a, b, c}`
         * (C11 §6.5.2.5). */
        if (n->binary.op == TK_ASSIGN && n->binary.rhs &&
            n->binary.rhs->kind == ND_INIT_LIST && n->binary.lhs &&
            n->binary.lhs->resolved_type) {
            Type *lt = n->binary.lhs->resolved_type;
            if (ty_is_ref(lt)) lt = lt->base;
            if (lt && (lt->kind == TY_STRUCT || lt->kind == TY_UNION)) {
                fputc('(', stdout);
                emit_expr(n->binary.lhs);
                fputs(" = (", stdout);
                emit_type(lt);
                fputc(')', stdout);
                emit_expr(n->binary.rhs);
                fputc(')', stdout);
                return;
            }
        }
        fputc('(', stdout);
        emit_expr(n->binary.lhs);
        fprintf(stdout, " %s ", binop_str(n->binary.op));
        /* For plain assignment (=): if the RHS is a reference type
         * (TY_REF lowered to T*) and the LHS target is a struct value
         * (not a pointer), dereference the RHS. Pattern: '*slot = obj'
         * where obj is 'const T&' → '*slot = (*obj)'.
         * N4659 §8.18/2 [expr.ass] — the value of the RHS is assigned
         * to the LHS; references bind transparently. */
        {
            Type *rhs_rt = n->binary.rhs ? n->binary.rhs->resolved_type : NULL;
            bool rhs_is_ref = ty_is_ref(rhs_rt);
            Type *lhs_t = n->binary.lhs ? n->binary.lhs->resolved_type : NULL;
            bool lhs_wants_value = lhs_t &&
                (lhs_t->kind == TY_STRUCT || lhs_t->kind == TY_UNION);
            /* Also deref when LHS is a deref (*ptr = ref) — the target
             * is a value and the RHS is a pointer in our lowering. */
            if (!lhs_wants_value && n->binary.lhs &&
                n->binary.lhs->kind == ND_UNARY &&
                n->binary.lhs->unary.op == TK_STAR) {
                Type *inner = n->binary.lhs->unary.operand ?
                    n->binary.lhs->unary.operand->resolved_type : NULL;
                if (inner && inner->kind == TY_PTR && inner->base &&
                    (inner->base->kind == TY_STRUCT || inner->base->kind == TY_UNION))
                    lhs_wants_value = true;
            }
            if (rhs_is_ref && lhs_wants_value && n->binary.op == TK_ASSIGN) {
                /* Method call returning ref: the call-emit path already
                 * wraps with '(*...)' to convert T* → T (unless the
                 * current function also returns a ref). Don't double-
                 * wrap here. Pattern: gcc 4.8 lto-cgraph.c
                 *   last_node = encoder->nodes.pop();
                 * where vec::pop() returns T&. */
                bool is_ref_method_call = false;
                if (n->binary.rhs && n->binary.rhs->kind == ND_CALL &&
                    n->binary.rhs->call.callee &&
                    n->binary.rhs->call.callee->kind == ND_MEMBER) {
                    bool cur_returns_ref = ty_is_ref(g_current_func_ret_ty);
                    if (!cur_returns_ref) is_ref_method_call = true;
                }
                if (is_ref_method_call) {
                    emit_expr(n->binary.rhs);
                } else {
                    /* Suppress the ident-ref-param deref: we add our own
                     * '(*' here, so emit_expr should not additionally
                     * wrap an ND_IDENT ref-param. Otherwise we get
                     * '(*(*obj))' on a '*slot = obj' pattern. */
                    fputs("(*", stdout);
                    bool saved = g_suppress_ref_deref;
                    g_suppress_ref_deref = true;
                    emit_expr(n->binary.rhs);
                    g_suppress_ref_deref = saved;
                    fputc(')', stdout);
                }
            } else if (lhs_wants_value && n->binary.op == TK_ASSIGN &&
                       n->binary.rhs && n->binary.rhs->kind == ND_NUM &&
                       n->binary.rhs->num.lo == 0) {
                /* C++ allows 'struct_val = 0' (zero-init) for POD
                 * types in template bodies. C doesn't. Emit a compound
                 * literal zero-init: '(Type){0}'. */
                fputc('(', stdout);
                emit_type(lhs_t);
                fputs("){0}", stdout);
            } else if (lhs_wants_value && n->binary.op == TK_ASSIGN &&
                       n->binary.rhs && n->binary.rhs->kind == ND_IDENT &&
                       n->binary.rhs->ident.name &&
                       n->binary.rhs->ident.name->len == 5 &&
                       memcmp(n->binary.rhs->ident.name->loc, "vNULL", 5) == 0) {
                /* gcc vec.h defines 'extern vnull vNULL;' with a
                 * template conversion operator to vec<T,A,L>. The
                 * ident emit path lowers vNULL to '{0}', valid in
                 * an init-declarator but NOT in an assignment RHS
                 * (C: initializer lists appear only in declarators).
                 * Emit the equivalent compound literal here, mirroring
                 * the 'struct = 0' branch above. Pattern: gcc 4.8
                 * cfgexpand.c 'data.asan_vec = vNULL;'. */
                fputc('(', stdout);
                emit_type(lhs_t);
                fputs("){0}", stdout);
            } else {
                emit_expr(n->binary.rhs);
            }
        }
        fputc(')', stdout);
        return;
    }
    case ND_UNARY: {
        /* Unary class-operator dispatch — N4659 §16.5 [over.oper].
         * 'op x' where x has class type with matching unary operator
         * method becomes 'Class__suffix(&x)'. The suffix IS the same
         * as the binary case (operator '+', '-', '*', etc. all share
         * suffixes; unary vs binary is disambiguated by the 0-arg
         * param suffix '_p_void_pe_' vs '_p_T_pe_'). */
        Type *ot = n->unary.operand ? n->unary.operand->resolved_type : NULL;
        if (ty_is_ref(ot)) ot = ot->base;
        /* Operator-overload rvalue operand ('~(a - b)' where '-' is
         * struct operator-, returns struct, then '~' dispatches to
         * operator~). sema's common_arith_type returns NULL for
         * struct ND_BINARY, so the operand's resolved_type is missing
         * — infer from the operand's own lhs. Pattern: gcc 4.8
         * tree-vrp.c 'complement = ~(bound - double_int_one);'. */
        if (!ot && n->unary.operand &&
            (n->unary.operand->kind == ND_BINARY ||
             n->unary.operand->kind == ND_UNARY ||
             n->unary.operand->kind == ND_POSTFIX)) {
            Node *sub = n->unary.operand;
            Node *sub_op = (sub->kind == ND_BINARY)
                ? sub->binary.lhs : sub->unary.operand;
            if (sub_op) ot = sub_op->resolved_type;
        }
        OperatorKind uop = OP_UNKNOWN;
        if (ot && (ot->kind == TY_STRUCT || ot->kind == TY_UNION) &&
            ot->tag) {
            switch (n->unary.op) {
            case TK_MINUS: uop = OP_MINUS; break;
            case TK_PLUS:  uop = OP_PLUS;  break;
            case TK_EXCL:  uop = OP_BANG;  break;
            case TK_TILDE: uop = OP_TILDE; break;
            case TK_STAR:  uop = OP_STAR;  break;
            case TK_INC:   uop = OP_INCR;  break;
            case TK_DEC:   uop = OP_DECR;  break;
            default: break;
            }
        }
        if (uop != OP_UNKNOWN) {
            Type **pty = NULL;
            Node *winner = NULL;
            bool op_const = receiver_type_is_const(
                n->unary.operand ? n->unary.operand->resolved_type : NULL);
            int np = resolve_operator_overload(ot, uop, NULL, 0,
                                                op_const, &pty, &winner);
            if (np == 0) {
                mangle_class_operator(ot, uop, NULL, 0,
                                       candidate_is_const(winner));
                fputs("(&", stdout);
                emit_expr(n->unary.operand);
                fputc(')', stdout);
                return;
            }
        }
        /* '&ref_param' — N4659 §8.3.2 [dcl.ref]: a reference IS the
         * referent at the language level, so &ref yields the address
         * of the referent. In our C lowering the ref is already a
         * pointer holding that address, so the whole expression is
         * just the bare ident — emit no '&' and no deref.
         *
         * Pattern that surfaced this: gcc 4.8 gengtype-generated
         *   void gt_ggc_mx (struct foo_s& x_r) {
         *     struct foo_s *x = &x_r;       // C++: address of referent
         *     gt_ggc_m_9tree_node ((*x).field);
         *   }
         * Old emit produced 'struct foo_s* x = (&x_r);' (Foo**), then
         * '(*x).field' read the parameter slot as if it were Foo and
         * passed garbage to the GC mark walker. cc1plus segfaulted
         * during ggc_collect on the bad pointer.
         *
         * Gate is bare ND_IDENT because compound operands (&v->x,
         * &v[i]) still need '&' applied to the lowered expression. */
        if (n->unary.op == TK_AMP &&
            n->unary.operand && n->unary.operand->kind == ND_IDENT &&
            is_ref_param(n->unary.operand->ident.name)) {
            bool saved = g_suppress_ref_deref;
            g_suppress_ref_deref = true;
            emit_expr(n->unary.operand);
            g_suppress_ref_deref = saved;
            return;
        }
        /* '&Class::member' — N4659 §8.3.1/3 [expr.unary.op]: takes a
         * pointer-to-data-member. Lower to the member's offsetof in
         * its owning class (cast to ptrdiff_t). Null pmem is -1 by
         * convention so any valid member offset (including 0) is
         * distinguishable. */
        if (n->unary.op == TK_AMP &&
            n->unary.operand && n->unary.operand->kind == ND_QUALIFIED &&
            n->unary.operand->qualified.nparts == 2) {
            Token *cls = n->unary.operand->qualified.parts[0];
            Token *mem = n->unary.operand->qualified.parts[1];
            if (cls && mem) {
                fputs("(__PTRDIFF_TYPE__)__builtin_offsetof(struct ", stdout);
                /* Owner class tag — use the mangled-class-tag form so
                 * nested-namespace names route correctly. Build a
                 * probe Type that resolves to the class def's Type so
                 * mangle_class_tag picks up template args, etc. */
                Type probe = {0};
                probe.kind = TY_STRUCT;
                probe.tag = cls;
                Node *cdef = find_class_def_by_tag_only(&probe);
                Type *ct = (cdef && cdef->class_def.ty) ? cdef->class_def.ty
                                                         : &probe;
                mangle_class_tag(ct);
                fprintf(stdout, ", %.*s)", mem->len, mem->loc);
                return;
            }
        }
        /* `delete v` — N4659 §8.3.5 [expr.delete]. Lower to:
         *   (dtor-dispatch(v), __builtin_free(v))
         * where dtor-dispatch picks the vptr path for polymorphic
         * classes with a virtual dtor and the static wrapper otherwise.
         * The comma expression preserves the value-less context
         * (delete is a void expression). For trivially-destructible
         * pointees, the dtor call is elided. */
        if (n->unary.op == TK_KW_DELETE) {
            Node *opnd = n->unary.operand;
            Type *pt = opnd ? opnd->resolved_type : NULL;
            Type *pointee = NULL;
            if (pt && pt->kind == TY_PTR) pointee = pt->base;
            /* Resolve pointee through tag if class_region is missing. */
            if (pointee && (pointee->kind == TY_STRUCT || pointee->kind == TY_UNION) &&
                !pointee->class_region && pointee->tag) {
                Node *d = find_class_def_by_tag_args(pointee);
                if (!d) d = find_class_def_by_tag_only(pointee);
                if (d && d->class_def.ty) pointee = d->class_def.ty;
            }
            bool has_virt_dtor = pointee && class_has_virtual_dtor(pointee);
            bool has_dtor      = pointee && pointee->has_dtor;
            fputc('(', stdout);
            if (has_virt_dtor) {
                /* Vptr dispatch: read the vptr from the polymorphic
                 * root, then invoke its __dtor slot with the original
                 * pointer (slot signature takes the static pointer
                 * type, so the cast for vtable-type access is sound). */
                Type *root = vptr_owner_class(pointee);
                if (!root) root = pointee;
                fputs("((struct ", stdout);
                mangle_class_tag(root);
                fputs(" *)(", stdout);
                emit_expr(opnd);
                fputs("))->__sf_vptr->__dtor(", stdout);
                emit_expr(opnd);
                fputs("), ", stdout);
            } else if (has_dtor) {
                mangle_class_dtor(pointee);
                fputc('(', stdout);
                emit_expr(opnd);
                fputs("), ", stdout);
            }
            fputs("__builtin_free(", stdout);
            emit_expr(opnd);
            fputs("))", stdout);
            return;
        }
        /* For *(ref_param): deref the pointer, then deref again —
         * the user explicitly asked for indirection on a ref. */
        fputc('(', stdout);
        fputs(unop_str(n->unary.op), stdout);
        emit_expr(n->unary.operand);
        fputc(')', stdout);
        return;
    }
    case ND_POSTFIX:
        fputc('(', stdout);
        emit_expr(n->unary.operand);
        fputs(unop_str(n->unary.op), stdout);
        fputc(')', stdout);
        return;
    case ND_TERNARY:
        fputc('(', stdout);
        emit_expr(n->ternary.cond);
        fputs(" ? ", stdout);
        emit_expr(n->ternary.then_);
        fputs(" : ", stdout);
        emit_expr(n->ternary.else_);
        fputc(')', stdout);
        return;
    case ND_CALL: {
        /* Slice D-Hoist: if this call has been hoisted to a synthesized
         * temp local, just emit the local's name and skip the rest of
         * call emission. The temp's initializer (which IS this call)
         * was already emitted ahead of the current statement. */
        if (n->codegen_temp_name) {
            fputs(n->codegen_temp_name, stdout);
            return;
        }
        /* Zero-arg functional cast 'T()' to a non-template class name —
         * N4659 §8.2.3 [expr.type.conv]. The callee is ND_IDENT whose
         * resolved_decl is ENTITY_TYPE/ENTITY_TAG and there are no
         * args; lower to a value-init compound literal '(struct T){0}'.
         * Without this, 'Derived()' in expression position emitted as
         * a literal call to a function named 'Derived' — cc rejected. */
        if (n->call.nargs == 0 && n->call.callee &&
            n->call.callee->kind == ND_IDENT) {
            Declaration *rd = n->call.callee->ident.resolved_decl;
            Token *cname = n->call.callee->ident.name;
            Type *probe_ty = NULL;
            if (rd && (rd->entity == ENTITY_TYPE ||
                       rd->entity == ENTITY_TAG) && rd->type) {
                probe_ty = rd->type;
            } else if (cname) {
                /* Sema didn't tag the resolved_decl (e.g. typedef
                 * names at file scope). Probe the TU by tag to see
                 * if 'cname' names a class. */
                Type probe = {0};
                probe.kind = TY_STRUCT;
                probe.tag = cname;
                Node *cdef = find_class_def_by_tag_only(&probe);
                if (cdef && cdef->class_def.ty)
                    probe_ty = cdef->class_def.ty;
            }
            if (probe_ty &&
                (probe_ty->kind == TY_STRUCT ||
                 probe_ty->kind == TY_UNION)) {
                /* Classes with virtual methods or a default ctor need
                 * the ctor body to run (vptr setup happens there). A
                 * bare '(struct T){0}' would leave the vptr NULL and
                 * any virtual call would segfault. Emit a stmt-expr
                 * that constructs into a temp and yields the value.
                 * GNU extension (cproc-side goal accepts the cost for
                 * this corner). N4659 §11.6.1/8 [dcl.init]
                 * (value-initialization). Pattern: g++.dg/init/value1.C
                 * 'A a = A();' with 'virtual void f();' on A. */
                /* Restricted to polymorphic classes: a default ctor
                 * with no virtual-table setup is captured well enough
                 * by '{0}' (which zero-inits the same fields), and
                 * the stmt-expr form would otherwise break static-
                 * storage initializers that the C compiler folds (the
                 * stmt-expr isn't a constant expression). For a
                 * polymorphic class the bare '{0}' leaves the vptr
                 * NULL and any virtual dispatch through the value
                 * segfaults. */
                if (probe_ty->has_virtual_methods) {
                    fputs("({ ", stdout);
                    emit_type(probe_ty);
                    fputs(" __sf_val_t = {0}; ", stdout);
                    mangle_class_ctor(probe_ty, NULL, 0);
                    fputs("(&__sf_val_t); __sf_val_t; })", stdout);
                    return;
                }
                fputc('(', stdout);
                emit_type(probe_ty);
                fputs("){0}", stdout);
                return;
            }
        }
        /* Capturing-lambda call — N4659 §8.1.5/3 [expr.prim.lambda].
         * Invoking the closure invokes its function call operator;
         * sea-front lowers that to a free fn taking the closure ptr
         * as its first argument. The callee's resolved_type is a
         * closure TY_STRUCT (§8.1.5/2) carrying a back-pointer to
         * its synthesised __sf_lambda_<N> function
         * (Type.lambda_fn). Dispatch as 'lambda_fn(&closure, args)'.
         * Checked before the generic operator() dispatch so closure
         * types short-circuit out without overload resolution. */
        {
            Node *cc = n->call.callee;
            Type *ct = cc ? cc->resolved_type : NULL;
            if (ty_is_ref(ct)) ct = ct->base;
            if (ct && ct->kind == TY_STRUCT && ct->lambda_fn) {
                Node *lf = ct->lambda_fn;
                /* Match the def-site mangled name. The lambda fn has a
                 * struct-pointer __self param so the free-fn overload
                 * detection adds a '_p_..._pe_' suffix at the def site;
                 * emit_free_func_symbol with the same params produces
                 * the same bytes here. Lambda fns are bounded to
                 * FFSIG_MAX_PARAMS in practice — overflow falls back
                 * to bare-name emit (same shape as captureless). */
                Type *pty[16];
                int   np = lf->func.nparams;
                if (np > 16) np = 0;
                for (int i = 0; i < np; i++)
                    pty[i] = lf->func.params[i]->param.ty;
                emit_free_func_symbol(lf->func.name, NULL,
                                       /*c_linkage=*/false, pty, np);
                fputc('(', stdout);
                if (is_addressable_lvalue(cc)) {
                    emit_addrof_for_this(cc);
                } else {
                    fputs("&(", stdout);
                    emit_expr(cc);
                    fputc(')', stdout);
                }
                for (int i = 0; i < n->call.nargs; i++) {
                    fputs(", ", stdout);
                    emit_expr(n->call.args[i]);
                }
                fputc(')', stdout);
                return;
            }
        }
        /* 'obj(args)' where obj is a class value with operator() →
         * dispatch to the class's operator() method. N4659 §16.5
         * [over.oper]. Pattern: gcc 4.8 expr.h insn_gen_fn and
         * expr.c '(*genfun)(to1, from1)'. */
        {
            Node *callee_ = n->call.callee;
            Type *cty = callee_ ? callee_->resolved_type : NULL;
            if (ty_is_ref(cty)) cty = cty->base;
            if (cty && (cty->kind == TY_STRUCT || cty->kind == TY_UNION) &&
                cty->tag) {
                /* Collect call-site arg types for param-suffix mangling. */
                Type **at = NULL;
                int na = collect_call_arg_types(n->call.args,
                                                 n->call.nargs, &at);
                Type **pty = NULL;
                Node *winner = NULL;
                bool callee_const = receiver_type_is_const(cty);
                int np = resolve_operator_overload(cty, OP_CALL,
                                                    at, na, callee_const,
                                                    &pty, &winner);
                if (np >= 0 || (winner && winner->kind == ND_FUNC_DEF)) {
                    mangle_class_operator(cty, OP_CALL,
                                           np >= 0 ? pty : at,
                                           np >= 0 ? np : na,
                                           candidate_is_const(winner));
                    fputc('(', stdout);
                    /* Implicit this — lvalue callee address. */
                    if (is_addressable_lvalue(callee_)) {
                        emit_addrof_for_this(callee_);
                    } else {
                        /* Rvalue callee (e.g. result of operator*): use
                         * compound-literal trick or rely on hoist. Fall
                         * back to a simple '&(expr)' which works for
                         * trivial cases and fails cleanly otherwise. */
                        fputs("&(", stdout);
                        emit_expr(callee_);
                        fputc(')', stdout);
                    }
                    for (int i = 0; i < n->call.nargs; i++) {
                        fputs(", ", stdout);
                        emit_arg_for_param(n->call.args[i],
                                            (pty && i < np) ? pty[i] : NULL);
                    }
                    fputc(')', stdout);
                    return;
                }
            }
        }
        /* Class-template functional cast 'vec<T,A,L>()' (value-init).
         * The callee is ND_TEMPLATE_ID naming a class template, with
         * no args. Emit a compound literal of the instantiated struct:
         *   (struct sf__vec_t_..._te_){0}
         * Only handle the 0-arg form — ctor-with-args would need
         * hoisting to a temp + ctor call. Pattern: gcc 4.8 ipa-cp.c
         *   return vec<ipa_agg_jf_item, va_heap, vl_ptr>();
         * N4659 §8.2.3/2 [expr.type.conv]. */
        if (n->call.callee && n->call.callee->kind == ND_TEMPLATE_ID &&
            n->call.nargs == 0) {
            Node *tid = n->call.callee;
            /* Build a Type from (tag=tid.name, template_args=tid.args)
             * and look up the instantiated class in the TU. */
            Token *tname = tid->template_id.name;
            if (tname) {
                Type probe = {0};
                probe.kind = TY_STRUCT;
                probe.tag = tname;
                int n_args = tid->template_id.nargs;
                probe.n_template_args = n_args;
                static Type *probe_args[16];
                if (n_args > 0 && n_args <= 16) {
                    for (int i = 0; i < n_args; i++) {
                        Node *a = tid->template_id.args[i];
                        probe_args[i] = (a && a->kind == ND_VAR_DECL)
                            ? a->var_decl.ty : NULL;
                    }
                    probe.template_args = probe_args;
                }
                Node *d = find_class_def_by_tag_args(&probe);
                if (!d) d = find_class_def_by_tag_only(&probe);
                if (d && d->class_def.ty) {
                    fputc('(', stdout);
                    emit_type(d->class_def.ty);
                    fputs("){0}", stdout);
                    return;
                }
            }
        }
        /* Function-style cast / value-init: 'T()' or 'T(v)' where T
         * names a non-class type. This arises after template
         * instantiation: 'val(T())' inside a template becomes
         * 'val(int())' — which in C++ is int{0}. The ND_IDENT callee
         * still names the template param ('T') but its resolved_type
         * was substituted to a concrete type (int). Emit as a C99
         * compound literal or cast so the concrete type comes out:
         *   T()  → (int){0}          (0-arg value-init)
         *   T(v) → ((int)(v))        (1-arg conversion)
         * Struct/union types keep flowing through the ctor path
         * (hoist_emit_decl) which has its own handling.
         * N4659 §8.2.3/2 [expr.type.conv]. */
        if (n->call.callee && n->call.callee->kind == ND_IDENT &&
            n->call.callee->ident.resolved_decl &&
            n->call.callee->ident.resolved_decl->entity == ENTITY_TYPE) {
            /* Callee's resolved_type was post-subst'd to the concrete
             * type; the CALL node's resolved_type may be NULL because
             * sema's ND_CALL handler only copies ret-type from TY_FUNC
             * callees. */
            Type *conc = n->call.callee->resolved_type;
            if (conc && conc->kind != TY_STRUCT && conc->kind != TY_UNION &&
                conc->kind != TY_DEPENDENT) {
                if (n->call.nargs == 0) {
                    fputc('(', stdout);
                    emit_type(conc);
                    fputs("){0}", stdout);
                } else {
                    fputs("((", stdout);
                    emit_type(conc);
                    fputc(')', stdout);
                    fputc('(', stdout);
                    emit_expr(n->call.args[0]);
                    fputs("))", stdout);
                }
                return;
            }
            /* Struct/union T() → '(struct sf__T){0}' compound literal.
             * Happens when T is a TYPEDEF to a class template instance
             * (e.g. 'typedef vec<T> name;' in gcc 4.8 value-prof.h),
             * called as 'name()' in a function-call expression.
             * Pattern: gcc 4.8 profile.c 'histogram_values values =
             * histogram_values();'. */
            if (!conc) conc = n->call.callee->ident.resolved_decl->type;
            if (conc && (conc->kind == TY_STRUCT || conc->kind == TY_UNION) &&
                n->call.nargs == 0 && conc->tag) {
                fputc('(', stdout);
                emit_type(conc);
                fputs("){0}", stdout);
                return;
            }
        }
        /* Qualified static method call: 'Class::method(args)' where
         * the callee is ND_QUALIFIED with 2 parts [ClassName, method].
         * Emit as a mangled free-function call without a 'this' arg.
         * Common pattern in gcc: va_heap::release(ptr), double_int::from_shwi(v). */
        /* Qualified static method call: 'Class::method(args)'.
         * Callee is ND_QUALIFIED with parts [ClassName, method].
         * Look up the class type to get proper template-arg mangling.
         * If lookup fails, emit a best-effort human-readable name.
         * No 'this' argument (static method). */
        {
            Node *callee_q = n->call.callee;
            if (callee_q && callee_q->kind == ND_QUALIFIED &&
                callee_q->qualified.nparts >= 2) {
                Token *class_tok = callee_q->qualified.parts[0];
                Token *method_tok = callee_q->qualified.parts[callee_q->qualified.nparts - 1];
                /* Qualified instance-method call from inside the
                 * enclosing class — 'A::f(args)' inside B which
                 * inherits from A. Pass '&this->__sf_baseN' as the
                 * receiver instead of treating it as a static call.
                 * Without this, sea-front's static-call path emits
                 * the method without 'this' and gcc rejects with
                 * 'too few arguments'. Pattern: g++.dg/cpp0x/
                 * nsdmi-virtual1.C 'int k = A::i + A::f();'. */
                if (g_current_class_def && class_tok && method_tok) {
                    Type stub = {0};
                    stub.kind = TY_STRUCT;
                    stub.tag  = class_tok;
                    Node *qdef = find_class_def_by_tag_only(&stub);
                    if (qdef && qdef->class_def.ty &&
                        qdef->class_def.ty->class_region) {
                        Type *qcls = qdef->class_def.ty;
                        Type *cur = g_current_class_def->class_def.ty;
                        int path[8];
                        int len = find_base_path(cur, qcls, path, 8);
                        if (len > 0) {
                            Declaration *md = region_lookup_own(
                                qcls->class_region,
                                method_tok->loc, method_tok->len);
                            if (md && !md->is_static_member &&
                                md->type && md->type->kind == TY_FUNC) {
                                Type **pty = md->type->params;
                                int np = md->type->nparams;
                                bool mc = md->type->is_const;
                                mangle_class_method(qcls, method_tok,
                                                     pty, np, mc);
                                fputs("(&this", stdout);
                                /* Dot-less base chain: emit '->__sf_baseN' or
                                 * '.__sf_baseN' depending on whether the
                                 * preceding token is 'this' (pointer) or
                                 * a base subobject value. After 'this' the
                                 * first link is '->'; subsequent are '.'. */
                                for (int i = 0; i < len; i++) {
                                    fputs(i == 0 ? "->" : ".", stdout);
                                    if (path[i] == 0) fputs("__sf_base", stdout);
                                    else fprintf(stdout, "__sf_base%d", path[i]);
                                }
                                for (int i = 0; i < n->call.nargs; i++) {
                                    fputs(", ", stdout);
                                    emit_arg_for_param(n->call.args[i],
                                        (i < np) ? pty[i] : NULL);
                                }
                                fputc(')', stdout);
                                return;
                            }
                        }
                    }
                }
                /* Namespace-qualified extern "C" function: sema
                 * stashed the Declaration; if it carries c_linkage
                 * (or an explicit asm_name), emit the bare/asm form
                 * just like the unqualified path. Covers 'std::exit'
                 * via 'using ::exit;' in <cstdlib> — the symbol
                 * actually linked is 'exit', not the Itanium-mangled
                 * '_ZN3std4exitEi'. N4659 §10.5/6 [dcl.link]. */
                Declaration *qd = callee_q->qualified.resolved_decl;
                if (qd && qd->type && qd->type->kind == TY_FUNC &&
                    (qd->c_linkage || qd->asm_name)) {
                    emit_free_func_symbol(method_tok, qd->asm_name,
                                           qd->c_linkage,
                                           qd->type->params,
                                           qd->type->nparams);
                    fputc('(', stdout);
                    for (int i = 0; i < n->call.nargs; i++) {
                        if (i > 0) fputs(", ", stdout);
                        emit_arg_for_param(n->call.args[i],
                            (i < qd->type->nparams) ? qd->type->params[i] : NULL);
                    }
                    fputc(')', stdout);
                    return;
                }
                /* Free function in the std namespace under Itanium
                 * mangling — emit '_ZSt<len><name><param-encoding>'
                 * to match libstdc++'s standard encoding. The fully-
                 * scoped 'N3std<len><name>E' form sea-front otherwise
                 * produces is a valid Itanium <name>, but it's NOT
                 * the form libstdc++.so exports — link fails on
                 * std::set_terminate, std::set_unexpected, std::exit
                 * (without a using-decl), etc. Itanium ABI 5.1.5:
                 * <unscoped-name> ::= St <unqualified-name>. */
                if (g_mangle_kind == MANGLE_ITANIUM &&
                    qd && qd->type && qd->type->kind == TY_FUNC &&
                    class_tok && class_tok->len == 3 &&
                    memcmp(class_tok->loc, "std", 3) == 0 &&
                    !callee_q->qualified.resolved_class_type &&
                    callee_q->qualified.nparts == 2 &&
                    method_tok) {
                    fputs("_ZSt", stdout);
                    fprintf(stdout, "%d%.*s",
                        method_tok->len, method_tok->len, method_tok->loc);
                    mangle_param_suffix(qd->type->params, qd->type->nparams);
                    fputc('(', stdout);
                    for (int i = 0; i < n->call.nargs; i++) {
                        if (i > 0) fputs(", ", stdout);
                        emit_arg_for_param(n->call.args[i],
                            (i < qd->type->nparams) ? qd->type->params[i] : NULL);
                    }
                    fputc(')', stdout);
                    return;
                }
                if (class_tok && method_tok) {
                    /* N4659 §16.3 [over.match]: use the DECL's param
                     * types for mangling. If the Phase-2 qualified
                     * lookup (clone.c) resolved the method, use its
                     * function type's params. Otherwise fall back to
                     * call-site arg types. */
                    /* Emit mangled qualified call name. If the leading
                     * part has a template-id (e.g. Box<int>::test), include
                     * template arg suffix in the class name. */
                    Node *ltid = callee_q->qualified.lead_tid;
                    Type *callee_ft = callee_q->resolved_type;
                    /* If sema resolved the leading qualifier to a
                     * class Type (typedef alias for a class —
                     * 'stackv' → vec<T,va_stack,vl_embed>), mangle
                     * through that Type so template args land in the
                     * symbol. Otherwise emit the bare class_tok
                     * text (the common 'Class::method' case). */
                    /* Base-class member lookup: when the named class
                     * doesn't define the method directly but inherits
                     * it from a base, the def is mangled with the
                     * BASE class's tag. gcc 4.8 hash_table::dispose
                     * calls Descriptor::remove where Descriptor
                     * inherits remove() from typed_noop_remove<T>;
                     * pointer_hash<X> has the same shape (inherits
                     * remove from typed_noop_remove<X>). Sea-front
                     * would otherwise emit the call mangled with the
                     * derived class's tag — symbol that no def
                     * matches. N4659 §6.4.5 [class.qual] +
                     * §13.1 [class.derived]. */
                    Type *owner_class = NULL;
                    {
                        Type *probe_class = NULL;
                        if (callee_q->qualified.resolved_class_type) {
                            /* Try args-aware lookup first so we hit
                             * the instantiated class_def with bases
                             * patched (patch_all_types). */
                            Node *cdef = find_class_def_by_tag_args(
                                callee_q->qualified.resolved_class_type);
                            if (!cdef)
                                cdef = find_class_def_by_tag_only(
                                    callee_q->qualified.resolved_class_type);
                            if (cdef && cdef->class_def.ty)
                                probe_class = cdef->class_def.ty;
                            else
                                probe_class = callee_q->qualified.resolved_class_type;
                        } else {
                            Type stub = {0};
                            stub.kind = TY_STRUCT;
                            stub.tag = class_tok;
                            Node *cdef = find_class_def_by_tag_only(&stub);
                            if (cdef && cdef->class_def.ty)
                                probe_class = cdef->class_def.ty;
                        }
                        if (probe_class && probe_class->class_region) {
                            DeclarativeRegion *cr = probe_class->class_region;
                            Declaration *direct = region_lookup_own(cr,
                                method_tok->loc, method_tok->len);
                            if (!direct) {
                                for (int bi = 0; bi < cr->nbases; bi++) {
                                    DeclarativeRegion *br = cr->bases[bi];
                                    if (!br) continue;
                                    Declaration *bd = region_lookup_own(br,
                                        method_tok->loc, method_tok->len);
                                    if (bd && br->owner_type) {
                                        owner_class = br->owner_type;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    /* Determine the class Type to mangle through. The
                     * cascade mirrors the owner_class / resolved /
                     * stub fallback above, with template-id args
                     * from `ltid` populated onto the stub so
                     * `Class<int>::foo` mangles with the
                     * instantiation's template args.
                     *
                     * Single mangle_class_method call below
                     * replaces what was a manual concat of
                     * `mangle_class_tag + "__" + method + param-
                     * suffix` — the manual form mixed human prefix
                     * (mangle_class_tag is always human, TU-local C
                     * tag) with dispatched suffix (mangle_param_
                     * suffix), producing inconsistent symbols under
                     * --mangling=itanium. */
                    Type stub = {0};
                    Type *mangle_ctype = NULL;
                    if (owner_class) {
                        mangle_ctype = owner_class;
                    } else if (callee_q->qualified.resolved_class_type) {
                        mangle_ctype = callee_q->qualified.resolved_class_type;
                    } else {
                        stub.kind = TY_STRUCT;
                        stub.tag  = class_tok;
                        if (ltid && ltid->kind == ND_TEMPLATE_ID &&
                            ltid->template_id.nargs > 0) {
                            stub.n_template_args = ltid->template_id.nargs;
                            static Type *stub_args_buf[16];
                            int n = stub.n_template_args < 16
                                      ? stub.n_template_args : 16;
                            for (int i = 0; i < n; i++) {
                                Node *a = ltid->template_id.args[i];
                                stub_args_buf[i] = (a && a->kind == ND_VAR_DECL)
                                                     ? a->var_decl.ty : NULL;
                            }
                            stub.template_args = stub_args_buf;
                        }
                        mangle_ctype = &stub;
                    }
                    /* Member-template fallback fb_pty / fb_np set in
                     * the else branch below; declared at outer scope
                     * so the args-emit for-loop can also use them as
                     * a fallback for ref-param adaptation. */
                    Type **fb_pty = NULL;
                    int    fb_np  = -1;
                    /* Detect TY_DEPENDENT in callee_ft's params: that
                     * means sema/clone resolved the call to a method
                     * via class_region lookup, but the looked-up
                     * Declaration's type retained the class template's
                     * OWN parameter name (e.g. 'pointer_hash<T>::hash'
                     * lookup returns 'hash(Type*)' — pointer_hash's own
                     * 'Type', not the outer template's T). The outer
                     * SubstMap doesn't bind 'Type', so subst_type
                     * leaves it dependent. Fall through to the TU-walk
                     * which finds the cloned method def with concrete
                     * params. N4659 §17.7.1 [temp.inst]. */
                    bool callee_ft_has_dep = false;
                    if (callee_ft && callee_ft->kind == TY_FUNC) {
                        for (int k = 0; k < callee_ft->nparams; k++) {
                            Type *t = callee_ft->params[k];
                            while (t && (t->kind == TY_PTR ||
                                          t->kind == TY_REF ||
                                          t->kind == TY_RVALREF ||
                                          t->kind == TY_ARRAY))
                                t = t->base;
                            if (t && t->kind == TY_DEPENDENT) {
                                callee_ft_has_dep = true;
                                break;
                            }
                        }
                    }
                    /* Compute final (params, nparams) from the same
                     * cascade as before — sema-resolved callee_ft,
                     * else TU-walk fallback for member templates,
                     * else call-site arg types. */
                    Type **final_pty = NULL;
                    int    final_np  = 0;
                    if (!callee_ft_has_dep && callee_ft &&
                        callee_ft->kind == TY_FUNC &&
                        callee_ft->nparams > 0) {
                        final_pty = callee_ft->params;
                        final_np  = callee_ft->nparams;
                    } else {
                        /* Member-template fallback: sema can't resolve
                         * a class member-template name to a TY_FUNC
                         * (the template entity's type is NULL). Walk
                         * the TU for an ND_FUNC_DEF whose class tag
                         * + method name + arity match the call, and
                         * use ITS params. Without this, the call
                         * mangles using arg types — pass-by-value
                         * vec arg + literal int + cast pointer don't
                         * match the def's vec<...>& + unsigned +
                         * vec<...>* signature, link fails on 18+
                         * va_stack::alloc instantiations. */
                        if (g_tu) {
                            /* Build a flat list of candidate ND_FUNC_DEF
                             * nodes: top-level funcs (peeled from any
                             * ND_TEMPLATE_DECL wrapper) PLUS members of
                             * top-level ND_CLASS_DEFs. The class-member
                             * case covers instantiated class templates
                             * whose methods live as members rather than
                             * top-level OOL defs (e.g. pointer_hash<T>
                             * with in-class static method definitions).
                             * Without this, the TU-walk falls back to
                             * arg types and the call mangle uses the
                             * un-substituted 'Type' instead of 'int'.
                             * N4659 §17.7.1 [temp.inst]. */
                            enum { MAX_FB_CAND = 4096 };
                            Node *cand_list[MAX_FB_CAND];
                            int n_cand = 0;
                            for (int di = 0; di < g_tu->tu.ndecls && n_cand < MAX_FB_CAND; di++) {
                                Node *raw = g_tu->tu.decls[di];
                                if (!raw) continue;
                                Node *peeled = raw;
                                if (peeled->kind == ND_TEMPLATE_DECL && peeled->template_decl.decl)
                                    peeled = peeled->template_decl.decl;
                                if (peeled->kind == ND_FUNC_DEF) {
                                    cand_list[n_cand++] = peeled;
                                } else if (peeled->kind == ND_CLASS_DEF) {
                                    for (int mi = 0; mi < peeled->class_def.nmembers && n_cand < MAX_FB_CAND; mi++) {
                                        Node *m = peeled->class_def.members[mi];
                                        if (m && m->kind == ND_FUNC_DEF)
                                            cand_list[n_cand++] = m;
                                    }
                                }
                            }
                            for (int di = 0; di < n_cand; di++) {
                                Node *d = cand_list[di];
                                if (!d) continue;
                                if (d->kind != ND_FUNC_DEF) continue;
                                if (!d->func.class_type ||
                                    !d->func.class_type->tag) continue;
                                if (d->func.class_type->tag->len != class_tok->len ||
                                    memcmp(d->func.class_type->tag->loc,
                                           class_tok->loc, class_tok->len) != 0)
                                    continue;
                                if (!d->func.name ||
                                    d->func.name->len != method_tok->len ||
                                    memcmp(d->func.name->loc, method_tok->loc,
                                           method_tok->len) != 0)
                                    continue;
                                if (d->func.nparams != n->call.nargs) continue;
                                /* Build params array from ND_PARAM
                                 * children of the matched def. Skip
                                 * any def whose param list still has
                                 * TY_DEPENDENT (unsubstituted template
                                 * parameter) — that's the original
                                 * template body, not an instantiation;
                                 * mangling against it would produce
                                 * 'vec_t_T_..._te_' instead of
                                 * 'vec_t_<concrete>_..._te_'. */
                                static Type *fb_buf[16];
                                int np = d->func.nparams < 16
                                           ? d->func.nparams : 16;
                                bool has_dep = false;
                                for (int k = 0; k < np; k++) {
                                    Node *p = d->func.params[k];
                                    fb_buf[k] = (p && p->kind == ND_PARAM)
                                                  ? p->param.ty : NULL;
                                    Type *t = fb_buf[k];
                                    while (t && (t->kind == TY_PTR ||
                                                 t->kind == TY_REF ||
                                                 t->kind == TY_RVALREF ||
                                                 t->kind == TY_ARRAY))
                                        t = t->base;
                                    if (t && t->kind == TY_DEPENDENT)
                                        has_dep = true;
                                    if (t && (t->kind == TY_STRUCT || t->kind == TY_UNION) &&
                                        t->n_template_args > 0) {
                                        for (int j = 0; j < t->n_template_args; j++) {
                                            Type *ta = t->template_args[j];
                                            if (ta && ta->kind == TY_DEPENDENT) has_dep = true;
                                        }
                                    }
                                }
                                if (has_dep) continue;
                                /* Pick the instantiation whose first
                                 * param's underlying class type
                                 * (post strip of ref/ptr) matches the
                                 * call's first arg's underlying class
                                 * — same tag AND same template_args.
                                 * Without this we'd pick the FIRST
                                 * instantiation that matches arity
                                 * (e.g. va_stack::alloc<df_ref>) for
                                 * every va_stack::alloc<...> call,
                                 * regardless of T. */
                                if (np > 0 && n->call.nargs > 0 &&
                                    n->call.args[0]) {
                                    Type *pp = fb_buf[0];
                                    while (pp && (pp->kind == TY_PTR ||
                                                  pp->kind == TY_REF ||
                                                  pp->kind == TY_RVALREF ||
                                                  pp->kind == TY_ARRAY))
                                        pp = pp->base;
                                    Type *aa = n->call.args[0]->resolved_type;
                                    while (aa && (aa->kind == TY_PTR ||
                                                  aa->kind == TY_REF ||
                                                  aa->kind == TY_RVALREF ||
                                                  aa->kind == TY_ARRAY))
                                        aa = aa->base;
                                    if (pp && aa &&
                                        (pp->kind == TY_STRUCT || pp->kind == TY_UNION) &&
                                        (aa->kind == TY_STRUCT || aa->kind == TY_UNION) &&
                                        pp->tag && aa->tag &&
                                        tokens_equal(pp->tag, aa->tag) &&
                                        pp->n_template_args == aa->n_template_args) {
                                        bool args_match = true;
                                        for (int j = 0; j < pp->n_template_args && args_match; j++) {
                                            Type *pa = pp->template_args[j];
                                            Type *aaa = aa->template_args[j];
                                            /* Walk through indirection
                                             * layers (ptr/ref/array) to
                                             * compare the underlying
                                             * struct tag. df_ref vs
                                             * df_mw_hardreg_ptr are
                                             * both TY_PTR-of-struct;
                                             * shallow kind compare
                                             * would falsely match. */
                                            while (pa && (pa->kind == TY_PTR ||
                                                          pa->kind == TY_REF ||
                                                          pa->kind == TY_RVALREF ||
                                                          pa->kind == TY_ARRAY))
                                                pa = pa->base;
                                            while (aaa && (aaa->kind == TY_PTR ||
                                                           aaa->kind == TY_REF ||
                                                           aaa->kind == TY_RVALREF ||
                                                           aaa->kind == TY_ARRAY))
                                                aaa = aaa->base;
                                            if (!pa || !aaa) {
                                                args_match = (pa == aaa);
                                            } else if (pa->kind != aaa->kind) {
                                                args_match = false;
                                            } else if ((pa->kind == TY_STRUCT || pa->kind == TY_UNION) &&
                                                       pa->tag && aaa->tag) {
                                                if (pa->tag->len != aaa->tag->len ||
                                                    memcmp(pa->tag->loc, aaa->tag->loc, pa->tag->len) != 0)
                                                    args_match = false;
                                            }
                                        }
                                        if (!args_match) continue;
                                    } else if ((pp && pp->kind != TY_STRUCT &&
                                                pp->kind != TY_UNION) ||
                                               (aa && aa->kind != TY_STRUCT &&
                                                aa->kind != TY_UNION)) {
                                        /* Primitive first param/arg (e.g.
                                         * pointer_hash<int>::hash(int *p)
                                         * — first arg is int*, not a
                                         * class type). The class-arg
                                         * disambiguation rule above
                                         * doesn't apply; accept this
                                         * candidate without further
                                         * tie-breaking. The class+method
                                         * +arity match is already
                                         * sufficient when there's only
                                         * one shape per (class, method,
                                         * arity). */
                                    } else {
                                        /* Different shapes — give up
                                         * on this candidate. */
                                        continue;
                                    }
                                }
                                fb_pty = fb_buf;
                                fb_np  = np;
                                break;
                            }
                        }
                        if (fb_np >= 0) {
                            final_pty = fb_pty;
                            final_np  = fb_np;
                        } else {
                            Type **at = NULL;
                            int na = collect_call_arg_types(n->call.args,
                                                             n->call.nargs, &at);
                            final_pty = at;
                            final_np  = na;
                        }
                    }
                    /* Single mangling call — emits the full linker
                     * symbol consistently under whichever scheme is
                     * active. Replaces the manual prefix + member +
                     * suffix concat. */
                    mangle_class_method(mangle_ctype, method_tok,
                                            final_pty, final_np,
                                            /*is_const=*/false);
                    fputc('(', stdout);
                    for (int i = 0; i < n->call.nargs; i++) {
                        if (i > 0) fputs(", ", stdout);
                        /* N4659 §11.3.2 [dcl.ref]: adapt args for
                         * ref-typed params (wrap in &). Prefer
                         * callee_ft (sema-resolved); fall back to
                         * fb_pty (the def found via the member-
                         * template TU walk above) so the ref
                         * adaptation matches the def's signature. */
                        Type *pty = (callee_ft && callee_ft->kind == TY_FUNC &&
                                     i < callee_ft->nparams)
                            ? callee_ft->params[i]
                            : (fb_pty && i < fb_np ? fb_pty[i] : NULL);
                        emit_arg_for_param(n->call.args[i], pty);
                    }
                    /* Default-arg expansion via callee_ft->param_defaults
                     * (TY_FUNC's parse-time-captured defaults). The
                     * Node-based helper isn't available here — this
                     * path doesn't carry a resolved-callee Node. N4659
                     * §11.3.6 [dcl.fct.default]. With nargs==0 the first
                     * default must NOT be preceded by ", " (we just
                     * opened the arg list). */
                    if (callee_ft && callee_ft->kind == TY_FUNC &&
                        callee_ft->param_defaults &&
                        callee_ft->nparams > n->call.nargs) {
                        for (int i = n->call.nargs; i < callee_ft->nparams; i++) {
                            Node *def = callee_ft->param_defaults[i];
                            if (!def) break;
                            if (i > 0) fputs(", ", stdout);
                            emit_arg_for_param(def, callee_ft->params[i]);
                        }
                    }
                    fputc(')', stdout);
                    return;
                }
            }
        }

        /* Method call lowering: 'obj.method(args)' / 'p->method(args)'
         * becomes 'Class_method(&obj, args)' / 'Class_method(p, args)'.
         *
         * Detected when the callee is ND_MEMBER and the obj's resolved
         * type is a struct (or pointer-to-struct). The struct's tag
         * gives us the class name to mangle with.
         *
         * Also handles unqualified method calls inside another method
         * body — 'doubled()' inside 'quadrupled()'. The callee is then
         * an ND_IDENT marked implicit_this, and we recover the class
         * via the resolved declaration's home region. */
        Node *callee = n->call.callee;
        /* Don't require rd->type to be TY_FUNC — for member-template
         * decls, the lookup may find the ENTITY_TEMPLATE entry whose
         * type is NULL. The class_type + mname pair is enough for
         * resolve_overload to find the right candidate. */
        if (callee && callee->kind == ND_IDENT &&
            callee->ident.implicit_this &&
            callee->ident.resolved_decl &&
            callee->ident.resolved_decl->home &&
            callee->ident.resolved_decl->home->owner_type &&
            callee->ident.resolved_decl->home->owner_type->tag) {
            Type *class_type = callee->ident.resolved_decl->home->owner_type;
            /* The resolved decl's home is the SOURCE template class
             * (no template args). When emitting inside a class-template
             * member's body — instantiated for some <args> — prefer
             * the currently-emitting method's class type, which carries
             * the instantiated args. The mangled call then lands on
             * the same symbol the cloned def emits.
             * N4659 §6.4.1/13 [basic.lookup.unqual]: unqualified lookup
             * in a member function uses the class scope of the
             * SPECIALIZATION. Itanium C++ ABI §5.1.5: the mangled name
             * encodes those class-template args. */
            if (g_current_method_class && g_current_method_class->tag &&
                class_type->tag &&
                tokens_equal(g_current_method_class->tag, class_type->tag) &&
                g_current_method_class->n_template_args >
                    class_type->n_template_args) {
                class_type = g_current_method_class;
            }
            Token *mname = callee->ident.name;
            /* If the method belongs to a base class of the current
             * class, we need to pass &this->__sf_base.<...> instead
             * of bare 'this' so the receiver is the right
             * subobject. find_base_path returns 0 when the method's
             * home IS the current class — no walk needed. */
            int base_path[8];
            int base_len = 0;
            if (g_current_class_def) {
                Type *cur = g_current_class_def->class_def.ty;
                base_len = find_base_path(cur, class_type, base_path, 8);
            }
            Type **call_pty = NULL;
            int call_np = -1;
            if (method_is_virtual(class_type, mname)) {
                /* Virtual dispatch through the vptr-owning ancestor.
                 * The vptr lives at the polymorphic root, not on every
                 * derived class — see emit_vptr_lhs. */
                emit_vptr_lhs(class_type);
                fprintf(stdout, "->%.*s(this", mname->len, mname->loc);
            } else {
                Type **at = NULL;
                int na = collect_call_arg_types(n->call.args,
                                                 n->call.nargs, &at);
                Node *winner = NULL;
                int np = resolve_overload(class_type, mname,
                                           /*is_ctor=*/false,
                                           at, na,
                                           g_current_method_is_const,
                                           &call_pty, &winner);
                if (np < 0)
                    die_no_overload(class_type, mname, na, "ND_CALL implicit-this");
                call_np = np;
                /* Member-template candidates have TY_DEPENDENT
                 * components — at the top level (e.g. T) or nested
                 * inside TY_REF/TY_PTR/TY_ARRAY chains, OR inside
                 * the template_args of an instantiated TY_STRUCT
                 * (e.g. vec<T, va_heap, vl_embed>*& where T is a
                 * template_arg of vec<...>). At a call from a
                 * cloned/instantiated body, the call args are
                 * concrete; swap them in wholesale so the mangled
                 * name matches the instantiated method. Pattern:
                 * gcc 4.8 vec.h va_heap::reserve<T> calling
                 * release(v) where v has type
                 * 'vec<T, va_heap, vl_embed>*&'. */
                bool any_dep = ty_contains_dependent(call_pty, np);
                if (any_dep && np == na) {
                    for (int i = 0; i < np; i++) call_pty[i] = at[i];
                }
                {
                    bool mc = candidate_is_const(winner);
                    mangle_class_method(class_type, mname, call_pty, np, mc);
                }
                /* N4659 §11.4.9 [class.static]: static member functions
                 * have no implicit 'this'. Open the arg list with no
                 * receiver — the regular arg loop below will fill in
                 * the rest. */
                if (candidate_is_static(winner)) {
                    fputc('(', stdout);
                } else if (base_len > 0) {
                    /* Inherited method — receiver is the base subobject. */
                    fputs("(&this->", stdout);
                    /* emit_base_chain leaves a trailing '.', so we
                     * emit all but the last hop with the chain helper
                     * and then '__sf_base' (no trailing dot) for the
                     * final hop. */
                    if (base_len > 1)
                        emit_base_chain(base_path, base_len - 1);
                    int last = base_path[base_len - 1];
                    if (last == 0) fputs("__sf_base", stdout);
                    else           fprintf(stdout, "__sf_base%d", last);
                } else {
                    fputs("(this", stdout);
                }
                /* When we opened with just '(' (static fn case), the
                 * first arg must NOT be preceded by ", ". Track that. */
                bool wrote_receiver = !candidate_is_static(winner);
                for (int i = 0; i < n->call.nargs; i++) {
                    if (i > 0 || wrote_receiver) fputs(", ", stdout);
                    emit_arg_for_param(n->call.args[i],
                                        (call_pty && i < call_np)
                                            ? call_pty[i] : NULL);
                }
                /* Default-arg expansion — N4659 §11.3.6 [dcl.fct.default].
                 * For static fn with zero user args, the wrote_receiver
                 * is false; the helper still prepends ", " unconditionally,
                 * so handle the special case: for static fns with nargs==0,
                 * the first default needs no comma. emit_default_args_tail
                 * doesn't know that — patch around it by emitting the
                 * first default manually. */
                if (n->call.nargs == 0 && !wrote_receiver) {
                    Node *p = func_param_node(winner, 0);
                    if (p && p->param.default_value && call_np > 0) {
                        emit_arg_for_param(p->param.default_value,
                                            call_pty ? call_pty[0] : NULL);
                        emit_default_args_tail(winner, 1, call_np, call_pty);
                    }
                } else {
                    emit_default_args_tail(winner, n->call.nargs,
                                            call_np, call_pty);
                }
                fputc(')', stdout);
                return;
            }
            for (int i = 0; i < n->call.nargs; i++) {
                fputs(", ", stdout);
                emit_arg_for_param(n->call.args[i],
                                    (call_pty && i < call_np)
                                        ? call_pty[i] : NULL);
            }
            /* Qualified-id call fallthrough — the path that runs when
             * neither the static-method nor the member-method branch
             * resolved to a winner Node. Today the resolved Node isn't
             * threaded through here, so we can't expand defaults. If
             * the upstream lookup found a callee with more params than
             * the call supplied (call_np > nargs), silently emitting
             * the short call is the latent silent-miscompile pattern
             * the audit just exposed elsewhere. Hard-fail rather than
             * miscompile. Fix is to thread the resolved Node out of
             * the upstream lookup; until then we want to know when it
             * matters. TODO(seafront#default-args-qid-fallthrough). */
            if (call_np > n->call.nargs) {
                fprintf(stderr,
                    "sea-front: qualified-id call fallthrough emitted "
                    "%d arg(s) for a callee with %d param(s); default-"
                    "arg expansion not wired here. Add resolved-Node "
                    "threading to fix.\n",
                    n->call.nargs, call_np);
                abort();
            }
            fputc(')', stdout);
            return;
        }
        if (callee && callee->kind == ND_MEMBER) {
            Node *obj = callee->member.obj;
            Type *ot = obj ? obj->resolved_type : NULL;
            /* Struct-operator-overload rvalue: for '(a op b).method()'
             * the obj is ND_BINARY with NULL resolved_type (sema's
             * common_arith_type returns NULL for struct operands).
             * Infer from an operand. Pattern: gcc 4.8 gimple-fold.c
             * '(a - b).sext(p)'. */
            if (!ot && obj && (obj->kind == ND_BINARY ||
                                obj->kind == ND_UNARY ||
                                obj->kind == ND_POSTFIX)) {
                Node *op = (obj->kind == ND_BINARY)
                    ? obj->binary.lhs : obj->unary.operand;
                if (op) ot = op->resolved_type;
            }
            /* Sema-missed fallback: when obj is itself a member access
             * ('outer.inner.method()') and inner's resolved_type wasn't
             * set (happens for certain anonymous-typedef / field paths,
             * and for arrow-access through ref-to-pointer params after
             * the cloning pass), resolve the member type on-demand by
             * looking up the field name in the outer's class_region.
             * N4659 §6.4.5 [class.qual]. */
            if (!ot && obj && obj->kind == ND_MEMBER && obj->member.member) {
                Node *outer = obj->member.obj;
                Type *outer_ty = outer ? outer->resolved_type : NULL;
                /* If the outer's resolved_type is missing (common in
                 * cloned member-template bodies), compute it from the
                 * operand of an ND_UNARY('*'): '*v' where v is T*&
                 * yields T* (deref the ref + ptr). */
                if (!outer_ty && outer && outer->kind == ND_UNARY &&
                    outer->unary.op == TK_STAR && outer->unary.operand) {
                    Type *opt = outer->unary.operand->resolved_type;
                    /* Peel one ref/rvalref layer first, then one ptr
                     * layer (the actual unary '*' indirection). */
                    if (opt && ty_is_ref(opt)) opt = opt->base;
                    if (opt && opt->kind == TY_PTR) opt = opt->base;
                    outer_ty = opt;
                }
                /* Strip the same ref/ptr layers that the main resolver
                 * does below — so 'v->inner.bump()' (v is T*&) reaches
                 * the inner struct via TY_REF -> TY_PTR -> TY_STRUCT. */
                if (outer_ty && ty_is_indirect(outer_ty))
                    outer_ty = outer_ty->base;
                if (outer_ty && ty_is_ref(outer_ty))
                    outer_ty = outer_ty->base;
                if (outer_ty && outer_ty->kind == TY_PTR && outer_ty->base &&
                    obj->member.op == TK_ARROW)
                    outer_ty = outer_ty->base;
                /* Type copies that come through subst_type may have
                 * tag set but class_region NULL — only the canonical
                 * instantiated Type carries class_region. Walk the TU
                 * by (tag, template_args) to find the canonical
                 * class_def, then use ITS Type's class_region.
                 * Pattern: gcc 4.8 vec.h va_heap::release's cloned
                 * body has 'v->vecpfx_.release_overhead()' where v's
                 * resolved_type is a substituted Type copy whose
                 * class_region wasn't patched. */
                if (outer_ty && !outer_ty->class_region && outer_ty->tag) {
                    Node *cd = find_class_def_by_tag_args(outer_ty);
                    if (cd && cd->class_def.ty &&
                        cd->class_def.ty->class_region)
                        outer_ty = cd->class_def.ty;
                }
                if (outer_ty && outer_ty->class_region) {
                    Token *fn = obj->member.member;
                    Declaration *fd = lookup_in_scope(outer_ty->class_region,
                                                       fn->loc, fn->len);
                    if (fd && fd->type) ot = fd->type;
                }
            }
            /* References (TY_REF / TY_RVALREF) are lowered to C
             * pointers, so 'obj_is_ptr' is true for both — the
             * call-site must pass the ref as-is, not take its
             * address. N4659 §11.3.2 [dcl.ref]. */
            bool obj_is_ptr = ty_is_indirect(ot);
            if (obj_is_ptr) ot = ot->base;
            /* If the ref was TY_REF(TY_PTR(...)) or similar, peel
             * an inner ref/ptr once more so 'ot' lands on the class. */
            if (ty_is_ref(ot)) ot = ot->base;
            /* For 'h->m()' on a reference-to-pointer (T*&) parameter,
             * the source ARROW expects a pointer-to-struct, but after
             * the TY_REF strip we have TY_PTR(TY_STRUCT) — peel one
             * more level so 'ot' lands on the struct. The ARROW token
             * tells us the source-level intent: the user wrote `->`
             * so the receiver is conceptually a pointer-to-class.
             * Pattern: gcc 4.8 vec.h vec_safe_grow_cleared / vec_safe_
             * push, free function templates with `vec<T,A,vl_embed>*&`
             * params. */
            if (ot && ot->kind == TY_PTR && ot->base &&
                callee->member.op == TK_ARROW)
                ot = ot->base;
            /* Method dispatch lowering applies when the member resolves
             * to a method declaration (type TY_FUNC). A function pointer
             * data field has type TY_PTR(TY_FUNC) and falls through to
             * the generic call path which emits 'obj.field(args)'. Look
             * up the member in the class region to tell them apart;
             * fall back to resolved_type when the region lookup misses. */
            bool is_method_call = false;
            if (ot && (ot->kind == TY_STRUCT || ot->kind == TY_UNION) &&
                ot->tag && callee->member.member) {
                Token *mn = callee->member.member;
                Type *mty = NULL;
                if (ot->class_region) {
                    Declaration *md = lookup_in_scope(ot->class_region,
                                                      mn->loc, mn->len);
                    if (md && md->type) mty = md->type;
                }
                /* Fallback: class_def member scan when class_region
                 * isn't set (substituted Type copies from template
                 * param types don't get class_region patched).
                 * For ND_FUNC_DEF members we only set is_method_call
                 * directly — ND_FUNC_DEF has no single TY_FUNC, just
                 * ret_ty + params fields. */
                if (!mty && ot->class_def) {
                    Node *cd = ot->class_def;
                    for (int ci = 0; ci < cd->class_def.nmembers; ci++) {
                        Node *cm = cd->class_def.members[ci];
                        if (!cm) continue;
                        Token *cmn = NULL;
                        bool is_fn = false;
                        if (cm->kind == ND_FUNC_DEF) {
                            cmn = cm->func.name; is_fn = true;
                        } else if (cm->kind == ND_VAR_DECL && cm->var_decl.ty &&
                                 cm->var_decl.ty->kind == TY_FUNC) {
                            cmn = cm->var_decl.name;
                            mty = cm->var_decl.ty;
                            is_fn = true;
                        }
                        if (is_fn && cmn && cmn->len == mn->len &&
                            memcmp(cmn->loc, mn->loc, mn->len) == 0) {
                            is_method_call = true;
                            break;
                        }
                    }
                }
                if (!mty && callee->resolved_type) mty = callee->resolved_type;
                /* Don't clobber is_method_call=true if the class_def
                 * scan above matched on name alone (no TY_FUNC in hand).
                 * Only promote to true when mty is a concrete TY_FUNC;
                 * never demote from true to false here. */
                if (!is_method_call)
                    is_method_call = mty && mty->kind == TY_FUNC;
                /* If the method wasn't found via class_region, try
                 * a direct member scan through class_def. This covers
                 * Type copies where class_region was patched but the
                 * member wasn't found (e.g. different region).
                 * N4659 §6.4.5 [class.qual]. */
                if (!is_method_call && ot->class_def) {
                    Token *mn = callee->member.member;
                    Node *cd = ot->class_def;
                    for (int ci = 0; ci < cd->class_def.nmembers && !is_method_call; ci++) {
                        Node *cm = cd->class_def.members[ci];
                        if (!cm) continue;
                        Token *cmn = NULL;
                        if (cm->kind == ND_FUNC_DEF) cmn = cm->func.name;
                        else if (cm->kind == ND_VAR_DECL && cm->var_decl.ty &&
                                 cm->var_decl.ty->kind == TY_FUNC)
                            cmn = cm->var_decl.name;
                        if (cmn && mn && cmn->len == mn->len &&
                            memcmp(cmn->loc, mn->loc, mn->len) == 0)
                            is_method_call = true;
                    }
                }
                /* Last resort: for template instantiations without
                 * class_def (Type copies from function params that
                 * weren't patched), assume method call if the class
                 * has template args. This is a sound assumption for
                 * C++03 templates — data members of template classes
                 * are never callable function pointers in practice.
                 * N4659 §16.5 [over.oper]. */
                if (!is_method_call && !ot->class_def &&
                    ot->n_template_args > 0)
                    is_method_call = true;
                /* Last-last resort: if class_def/class_region are both
                 * unset (e.g. the obj is a function-call return whose
                 * TY_STRUCT wasn't wired to the original class def),
                 * but we have a tag and a member name, assume it's a
                 * method call. Class field access that happens to share
                 * a member name with a nonexistent method would fail at
                 * link time — a better diagnostic than silently emitting
                 * 'obj.member(args)' which is invalid C for a non-fptr. */
                if (!is_method_call && !ot->class_def && !ot->class_region &&
                    ot->tag && callee->member.member)
                    is_method_call = true;
            }
            if (is_method_call) {
                /* N4659 §11.3.2 [dcl.ref]: if the method returns T&,
                 * our C lowering returns T*. Wrap the call in (*...)
                 * so the caller gets a value, not a pointer.
                 * Exception: if the CURRENT function also returns a ref,
                 * we're in a ref-forwarding chain (e.g. vl_ptr::at
                 * delegates to vl_embed::at) — don't deref since the
                 * caller expects a pointer. */
                bool ref_ret = method_returns_ref(ot, callee->member.member);
                bool cur_returns_ref = g_current_func_ret_ty &&
                    (g_current_func_ret_ty->kind == TY_REF ||
                     g_current_func_ret_ty->kind == TY_RVALREF);
                if (ref_ret && !cur_returns_ref) fputs("(*", stdout);
                bool virt = method_is_virtual(ot, callee->member.member);
                /* N4659 §13.3/15 [class.virtual]: a qualified call
                 * 'obj->Class::method()' binds to Class::method
                 * directly, bypassing virtual dispatch even when the
                 * method IS virtual. The parser captured the trailing
                 * qualifier segment on member.qualifier_class. */
                if (virt && callee->member.qualifier_class)
                    virt = false;
                /* Param types of the resolved method; used for arg
                 * lowering at line below. NULL on the virtual path
                 * (no overload resolution there yet — virtual call
                 * arg coercion is a TODO). */
                Type **call_pty = NULL;
                int call_np = -1;
                Node *winner_method = NULL;
                if (virt) {
                    /* Virtual dispatch: load __sf_vptr then call slot.
                     * Emit the receiver expression once into the slot
                     * and once as the first arg (no temporary —
                     * sufficient for the lvalue/pointer cases we
                     * support).
                     *
                     * The vptr lives in the polymorphic-root ancestor,
                     * not on every derived struct. Cast through the
                     * owner — sound thanks to the offset-0 layout
                     * invariant for the first polymorphic base. */
                    Type *vowner = vptr_owner_class(ot);
                    bool need_cast = (vowner && vowner != ot);
                    if (need_cast) {
                        fputs("((struct ", stdout);
                        mangle_class_tag(vowner);
                        fputs(" *)", stdout);
                        if (!obj_is_ptr) fputc('&', stdout);
                        fputc('(', stdout);
                        /* For ref-params (lowered to T*), emit_ident
                         * auto-derefs to '(*p)'. We're about to cast
                         * the result to a polymorphic-root pointer,
                         * so the underlying T* IS what we want — skip
                         * the deref to avoid producing a struct value
                         * inside the pointer-cast. Without this,
                         * 'b.foo()' on B& emits
                         * '((struct A *)((*b)))->...' which casts a
                         * struct value to pointer. */
                        bool saved_srd = g_suppress_ref_deref;
                        if (obj_is_ptr && obj && obj->kind == ND_IDENT &&
                            is_ref_param(obj->ident.name))
                            g_suppress_ref_deref = true;
                        emit_expr(obj);
                        g_suppress_ref_deref = saved_srd;
                        fputs("))->__sf_vptr->", stdout);
                    } else if (obj_is_ptr) {
                        /* Ref-param idents would normally auto-deref
                         * to '(*x)' — but we want the pointer here so
                         * '->' can chase the vtable. Suppress the deref
                         * for the same reason the need_cast branch
                         * above does.
                         * Pattern: g++.dg/torture/pr48661.C E::n calls
                         * 'x.m()' on a const B& parameter. */
                        bool saved_srd = g_suppress_ref_deref;
                        if (obj && obj->kind == ND_IDENT &&
                            is_ref_param(obj->ident.name))
                            g_suppress_ref_deref = true;
                        fputc('(', stdout);
                        emit_expr(obj);
                        g_suppress_ref_deref = saved_srd;
                        fputs(")->__sf_vptr->", stdout);
                    } else {
                        fputc('(', stdout);
                        emit_expr(obj);
                        fputs(").__sf_vptr->", stdout);
                    }
                    fprintf(stdout, "%.*s(",
                            callee->member.member->len,
                            callee->member.member->loc);
                } else {
                    /* Check if the method is inherited from a base.
                     * If so, mangle with the base class and pass
                     * &obj.__sf_base as the this pointer. */
                    Type *method_class = ot;

                    /* Explicit qualifier 'obj->Class::method()' —
                     * N4659 §13.3/15 [class.virtual]: bind to the
                     * named class's method, not the dynamic one.
                     * Mangle against the qualifier class so the call
                     * site matches Class::method's symbol. */
                    if (callee->member.qualifier_class) {
                        Type qstub = {0};
                        qstub.kind = TY_STRUCT;
                        qstub.tag  = callee->member.qualifier_class;
                        Node *qcdef = find_class_def_by_tag_only(&qstub);
                        if (qcdef && qcdef->class_def.ty)
                            method_class = qcdef->class_def.ty;
                    }

                    if (ot->class_region && !callee->member.qualifier_class) {
                        /* Not in own class? Check bases. */
                        Token *mn = callee->member.member;
                        Declaration *own_d = region_lookup_own(
                            ot->class_region, mn->loc, mn->len);
                        if (!own_d) {
                            for (int bi = 0; bi < ot->class_region->nbases; bi++) {
                                DeclarativeRegion *br = ot->class_region->bases[bi];
                                Declaration *bd = lookup_in_scope(br, mn->loc, mn->len);
                                if (bd && br->owner_type) {
                                    method_class = br->owner_type;

                                    break;
                                }
                            }
                        }
                    }
                    {
                        Type **at = NULL;
                        int na = collect_call_arg_types(n->call.args,
                                                         n->call.nargs, &at);
                        bool recv_const = receiver_type_is_const(
                            obj ? obj->resolved_type : NULL);
                        Node *winner = NULL;
                        int np = resolve_overload(method_class,
                                                   callee->member.member,
                                                   /*is_ctor=*/false,
                                                   at, na,
                                                   recv_const,
                                                   &call_pty, &winner);
                        winner_method = winner;
                        if (np < 0) {
                            /* Method not found in class_def. For
                             * template instantiations, emit a best-
                             * effort mangled call (using arg types
                             * for the param suffix) rather than
                             * falling through to plain 'obj.method()'
                             * which is invalid C.
                             * For non-template classes, fall through. */
                            if (method_class->n_template_args > 0) {
                                /* SHORTCUT (ours, not the standard):
                                 * use CALL-SITE arg types for the param
                                 * suffix when the decl's types aren't
                                 * available (class_def NULL on this Type
                                 * copy). N4659 §16.3 [over.match] says
                                 * the DECL's param types are the mangle
                                 * source; this approximation may mismatch
                                 * if implicit conversions differ.
                                 * TODO(seafront#tmpl-resolve-full): resolve
                                 * through the actual instantiated class_def. */
                                bool mc = method_is_const(method_class,
                                    callee->member.member);
                                mangle_class_method(method_class,
                                    callee->member.member, at, na, mc);
                                call_np = na;
                                call_pty = at;
                            } else if (callee->resolved_type &&
                                       callee->resolved_type->kind == TY_FUNC) {
                                /* Non-template class with class_def
                                 * unset — sema's member lookup left a
                                 * TY_FUNC on callee->resolved_type
                                 * carrying the method's declared param
                                 * types. Use those for the mangle so
                                 * the call site matches the forward-
                                 * decl emit (which walked the same
                                 * declaration). N4659 §16.3
                                 * [over.match]. */
                                Type *fty = callee->resolved_type;
                                bool mc = method_is_const(method_class,
                                    callee->member.member);
                                mangle_class_method(method_class,
                                    callee->member.member,
                                    fty->params, fty->nparams, mc);
                                call_np = fty->nparams;
                                call_pty = fty->params;
                            } else {
                                is_method_call = false;
                                goto plain_call;
                            }
                        } else {
                            call_np = np;
                            /* Member-template call: the instantiation
                             * pipeline set callee->resolved_type to the
                             * substituted TY_FUNC carrying the deduced
                             * param types. Prefer those over the source
                             * template-decl's TY_DEPENDENT params so the
                             * mangle suffix matches the cloned def's
                             * symbol. Gate on the resolved overload
                             * actually containing TY_DEPENDENT — non-
                             * template overloads have concrete param
                             * types from resolve_overload and their
                             * arity is the source of truth (overriding
                             * with a different-arity TY_FUNC would
                             * mangle to a wrong symbol). N4659 §17.5.2
                             * [temp.mem] + §17.7.1 [temp.inst]; Itanium
                             * C++ ABI §5.1. */
                            Type *cty = callee->resolved_type;
                            if (cty && cty->kind == TY_FUNC &&
                                cty->params && cty->nparams > 0 &&
                                ty_contains_dependent(call_pty, call_np)) {
                                call_pty = cty->params;
                                call_np = cty->nparams;
                            }
                            bool mc = candidate_is_const(winner);
                            mangle_class_method(method_class,
                                                     callee->member.member,
                                                     call_pty, call_np, mc);
                        }
                    }
                    fputc('(', stdout);
                }
                /* this argument: address-of for value, as-is for pointer.
                 * For inherited methods, pass &obj.__sf_base instead. */
                int base_idx_for_this = -1;
                if (ot->class_region && callee->member.member) {
                    Token *mn = callee->member.member;
                    Declaration *own_d = region_lookup_own(
                        ot->class_region, mn->loc, mn->len);
                    if (!own_d) {
                        for (int bi = 0; bi < ot->class_region->nbases; bi++) {
                            Declaration *bd = lookup_in_scope(
                                ot->class_region->bases[bi],
                                mn->loc, mn->len);
                            if (bd) { base_idx_for_this = bi; break; }
                        }
                    }
                }
                if (base_idx_for_this >= 0) {
                    /* Use '->' not '.' when obj is a pointer (source
                     * 'p->method()' with p being a pointer to a class
                     * inheriting from the method's owner). Pattern:
                     * gcc 4.8 web.c 'entry->unionfind_root()' where
                     * unionfind_root is inherited from web_entry_base. */
                    fputs("&(", stdout);
                    emit_expr(obj);
                    fputs(obj_is_ptr ? "->" : ".", stdout);
                    if (base_idx_for_this == 0) fputs("__sf_base", stdout);
                    else fprintf(stdout, "__sf_base%d", base_idx_for_this);
                    fputc(')', stdout);
                    goto after_base_this_emit;
                } else if (obj_is_ptr) {
                    /* Obj is already ptr/ref (lowered to ptr) — pass
                     * as-is. Suppress the ref-param deref in case obj
                     * is an identifier that names a ref-param: we
                     * need the pointer itself, not its dereference.
                     *
                     * Exception: T*& (ref to pointer) is lowered to
                     * T** in C; the receiver `this` should be T*, so
                     * we DO want the deref here — emitting `(*h)`
                     * gives T*. Detect by checking the raw obj type:
                     * TY_REF(TY_PTR(struct)) means we want the deref.
                     * Pattern: gcc 4.8 vec.h vec_safe_push body —
                     * 'v->quick_push(obj)' on `vec<T>*&v` param. */
                    Type *raw_ot = obj ? obj->resolved_type : NULL;
                    bool ref_to_ptr = ty_is_ref(raw_ot) && raw_ot->base &&
                                      raw_ot->base->kind == TY_PTR;
                    bool saved_suppress = g_suppress_ref_deref;
                    g_suppress_ref_deref = !ref_to_ptr;
                    emit_expr(obj);
                    g_suppress_ref_deref = saved_suppress;
                } else {
                    emit_addrof_for_this(obj);
                }
                after_base_this_emit:;
                /* User-supplied args, then default-arg expansion via
                 * the unified helper. Pulls defaults from the winner
                 * Node's params (ND_FUNC_DEF or ND_VAR_DECL with
                 * fn_params), unifying what previously lived as two
                 * separate paths (the param_defaults-on-Type form and
                 * a never-fired ND_FUNC_DEF branch). N4659 §11.3.6
                 * [dcl.fct.default]. */
                for (int i = 0; i < n->call.nargs; i++) {
                    fputs(", ", stdout);
                    emit_arg_for_param(n->call.args[i],
                                        (call_pty && i < call_np)
                                            ? call_pty[i] : NULL);
                }
                emit_default_args_tail(winner_method, n->call.nargs,
                                        call_np, call_pty);
                fputc(')', stdout);
                if (ref_ret && !cur_returns_ref) fputc(')', stdout);
                return;
            }
        }
    plain_call:
        /* When the callee is a cast expression '(T)x', C precedence
         * would parse '(T)x(args)' as '(T)(x(args))' — cast the CALL
         * result rather than calling the cast result. Source idiom
         * '((T)func)(args)' expects the outer call. Parenthesise the
         * cast callee to force the intended grouping. N4659 §8.2.2
         * [expr.call] / C §6.5.2.2. */
        {
            bool paren_callee = n->call.callee &&
                                n->call.callee->kind == ND_CAST;
            if (paren_callee) fputc('(', stdout);
            /* Overloaded free-function call: emit the mangled name
             * using the CALL-SITE argument types. This always matches
             * the actual arity, even when sema's resolved_decl points
             * at the wrong overload (which happens when overload
             * resolution can't disambiguate — e.g. arity-mismatched
             * candidates with NULL resolved arg types from g++ NULL).
             *
             * Using resolved_decl->params instead is appealing because
             * the mangle then matches the def's signature exactly,
             * but it diverges from the call's actual arity when
             * resolution failed and the historical first-found
             * resolved_decl has the wrong nparams — emits the wrong
             * function name AND leaves the actual arg list intact,
             * producing arity-mismatch link errors.
             *
             * NULL resolved_type on an individual arg (e.g. a g++
             * builtin we don't model) is encoded as 'unknown' by the
             * mangler — wrong but consistent across calls. */
            bool emitted_mangled = false;
            Declaration *rd_callee = (n->call.callee &&
                                       n->call.callee->kind == ND_IDENT &&
                                       !n->call.callee->ident.implicit_this)
                                       ? n->call.callee->ident.resolved_decl
                                       : NULL;
            Token *asm_callee = rd_callee ? rd_callee->asm_name : NULL;
            /* Only mangle the callee name when it actually denotes a
             * function. Function-pointer variables resolve to a
             * TY_PTR(TY_FUNC) Decl — the call goes through the
             * pointer value, no name-level mangling involved.
             *
             * Two ways the callee is "a function":
             *   - rd_callee->type is TY_FUNC — the parser/sema
             *     resolved the ident to a regular free function decl.
             *   - rd_callee is NULL but the ident's resolved_type is
             *     TY_FUNC — happens for template-instantiation call
             *     rewrites (instantiate.c sets resolved_decl=NULL,
             *     resolved_type=TY_FUNC after substituting the
             *     synthetic mangled-name ident). */
            bool callee_is_fn =
                (rd_callee && rd_callee->type &&
                 rd_callee->type->kind == TY_FUNC) ||
                (!rd_callee && n->call.callee &&
                 n->call.callee->resolved_type &&
                 n->call.callee->resolved_type->kind == TY_FUNC);
            if (n->call.callee && n->call.callee->kind == ND_IDENT &&
                !n->call.callee->ident.implicit_this &&
                callee_is_fn &&
                (asm_callee ||
                 free_func_name_is_overloaded(n->call.callee->ident.name))) {
                Type *at[32];
                int na = n->call.nargs < 32 ? n->call.nargs : 32;
                for (int i = 0; i < na; i++)
                    at[i] = n->call.args[i] ? n->call.args[i]->resolved_type : NULL;
                /* Default-argument injection extends the call to
                 * match the resolved overload's nparams. The mangled
                 * name MUST include the injected param types so it
                 * matches the def's signature, not just the user-
                 * written arg count. Pattern: gcc 4.8 gengtype.c
                 *   set_gc_used_type(o->info.type, GC_POINTED_TO, NULL)
                 * (3 args, decl has 4 with bool default → emit 4
                 * args; mangled name needs the 4th param's type).
                 *
                 * Also: when an arg is a null pointer constant
                 * (resolved_type = TY_PTR(TY_VOID) per visit_nullptr),
                 * substitute the resolved param type. The null
                 * pointer converts to any pointer type at the call
                 * site (N4659 §4.10/1), and the def's mangling uses
                 * the param's actual pointee type. */
                Declaration *rd = n->call.callee->ident.resolved_decl;
                Type *rd_fty = rd && rd->type && rd->type->kind == TY_FUNC
                                ? rd->type : NULL;
                if (rd_fty && rd_fty->param_defaults &&
                    n->call.nargs < rd_fty->nparams && na < 32) {
                    bool all_tail = true;
                    for (int i = n->call.nargs; i < rd_fty->nparams; i++) {
                        if (!rd_fty->param_defaults[i]) {
                            all_tail = false; break;
                        }
                    }
                    if (all_tail) {
                        for (int i = n->call.nargs;
                             i < rd_fty->nparams && na < 32; i++) {
                            at[na++] = rd_fty->params[i];
                        }
                    }
                }
                /* N4659 §16.5 [over.oper] — the symbol identifies the
                 * specific function chosen by overload resolution.
                 * Mangle from rd_fty->params (the resolved decl's
                 * declared parameter types), not from arg types.
                 * Argument types are an approximation that breaks for
                 * every implicit conversion (char→int, T→T&, T*→const
                 * T*, null→any-ptr, etc.); the resolved decl IS the
                 * canonical signature.
                 *
                 * Falls back to arg types when sema couldn't resolve
                 * (rd_fty NULL) or the arity doesn't match — both are
                 * sema bugs that should be fixed at sema rather than
                 * masked here, but the arg-type fallback keeps emit
                 * working until then. */
                if (rd_fty && rd_fty->nparams == na) {
                    for (int i = 0; i < na; i++)
                        at[i] = rd_fty->params[i];
                }
                /* Prefer the resolved decl's PARAM types over the call's
                 * ARG types when computing the mangle suffix. This
                 * keeps the call's mangled name in sync with the def's
                 * (the def's emit also uses param types) under two
                 * cases the arg-types path mishandles:
                 *
                 *   1. Variadic: '...' has no type, so we want the
                 *      fixed-param prefix only. Using arg types would
                 *      include the variadic args (or 'unknown' tags
                 *      sema sometimes leaves on later args), making
                 *      every call site mangle differently.
                 *      N4659 §11.3.5/4 [dcl.fct].
                 *
                 *   2. Implicit conversions: derived* → base&,
                 *      int → const long&, etc. The arg's type is
                 *      the source type; the param's is the converted
                 *      type. The def mangles with the param type, so
                 *      caller must too. (Pre-existing partial fix for
                 *      this in emit_arg_for_param's compound-literal
                 *      path; this completes it for the mangling.)
                 *
                 * Fall back to arg types when no resolved decl is
                 * available (shouldn't happen for well-resolved calls,
                 * but stays safe for the rare unresolved path). */
                Type **mangle_pty = at;
                int    mangle_np  = na;
                if (rd_callee && rd_callee->type &&
                    rd_callee->type->kind == TY_FUNC &&
                    rd_callee->type->params) {
                    /* Use param types — but only if the resolved
                     * decl's arity matches the call's. Sema can pick
                     * the wrong overload (e.g. cloned vec.h template
                     * body's gt_pch_nx call resolves to a 1-arg
                     * 'extern void gt_pch_nx(T &)' from a sibling
                     * template's body instead of the local 'extern
                     * void gt_pch_nx (T*, gt_pointer_operator, void*)'.
                     * Mangling against the 1-arg overload's params
                     * for a 3-arg call yields a name no def matches
                     * AND produces a 3-arg call to a 1-arg-declared
                     * symbol — cc errors 'too many arguments'). When
                     * arity differs, fall back to arg types so at
                     * least the call's mangled name matches the
                     * actual arg-count signature. */
                    bool arity_matches =
                        rd_callee->type->is_variadic
                          ? na >= rd_callee->type->nparams
                          : na == rd_callee->type->nparams;
                    if (arity_matches) {
                        mangle_pty = rd_callee->type->params;
                        mangle_np  = rd_callee->type->nparams;
                    }
                }
                emit_free_func_symbol(n->call.callee->ident.name,
                                       asm_callee,
                                       rd_callee && rd_callee->c_linkage,
                                       mangle_pty, mangle_np);
                emitted_mangled = true;
            }
            if (!emitted_mangled) emit_expr(n->call.callee);
            if (paren_callee) fputc(')', stdout);
        }
        fputc('(', stdout);
        /* Extract callee's function type for ref-param adaptation.
         * Handles function pointers (TY_PTR(TY_FUNC)) and direct
         * function calls (TY_FUNC). */
        {
            Type *callee_ft = n->call.callee ? n->call.callee->resolved_type : NULL;
            if (callee_ft && callee_ft->kind == TY_PTR && callee_ft->base)
                callee_ft = callee_ft->base;
            if (callee_ft && callee_ft->kind != TY_FUNC) callee_ft = NULL;
            /* Arity mismatch: the ident resolved to an overload whose
             * param count doesn't match the call's arg count. Our
             * sema doesn't do full C++ overload resolution for free-
             * function calls and may pick a same-named-but-different-
             * arity overload (e.g. gcc 4.8 has 'gt_pch_nx(T&)' 1-arg
             * and 'gt_pch_nx(T*, op, cookie)' 3-arg; we sometimes
             * resolve the 3-arg call to the 1-arg overload). Using
             * its param types for ref-adaptation is wrong — the 1-arg
             * overload's T& param would trigger '&(...)' wrapping of
             * a 3-arg call's first arg, producing invalid C like
             * '&((&(*x)))'. Fall back to NULL param_ty on mismatch
             * so no spurious adaptation happens.
             * TODO(seafront#free-func-overload): proper overload
             * resolution per N4659 §16.3 [over.match]. */
            /* Default-argument injection — N4659 §11.3.6 [dcl.fct.default]:
             * if fewer args than params AND the callee has captured
             * default values for the missing tail, arity matches AFTER
             * injection. Emit the user's args then the default exprs. */
            bool arity_ok = callee_ft &&
                            callee_ft->nparams == n->call.nargs;
            /* param_defaults may be missing on the callee_ft Type
             * reached via the ident's resolved_type (sometimes a Type
             * copy without the defaults attached). Fall back to the
             * resolved_decl's Type which carries the original. */
            if (callee_ft && !callee_ft->param_defaults &&
                n->call.callee && n->call.callee->kind == ND_IDENT &&
                n->call.callee->ident.resolved_decl &&
                n->call.callee->ident.resolved_decl->type) {
                Type *dt = n->call.callee->ident.resolved_decl->type;
                if (dt->kind == TY_PTR && dt->base) dt = dt->base;
                if (dt->kind == TY_FUNC && dt->param_defaults &&
                    dt->nparams == callee_ft->nparams)
                    callee_ft = dt;
            }
            int inject_from = -1;
            if (!arity_ok && callee_ft && callee_ft->param_defaults &&
                n->call.nargs < callee_ft->nparams) {
                bool all_tail_have_defaults = true;
                for (int i = n->call.nargs; i < callee_ft->nparams; i++) {
                    if (!callee_ft->param_defaults[i]) {
                        all_tail_have_defaults = false; break;
                    }
                }
                if (all_tail_have_defaults) {
                    arity_ok = true;
                    inject_from = n->call.nargs;
                }
            }
            int total = (inject_from >= 0) ? callee_ft->nparams : n->call.nargs;
            for (int i = 0; i < total; i++) {
                if (i > 0) fputs(", ", stdout);
                Type *pt = (arity_ok && callee_ft && i < callee_ft->nparams)
                    ? callee_ft->params[i] : NULL;
                Node *arg = (i < n->call.nargs)
                    ? n->call.args[i]
                    : callee_ft->param_defaults[i];
                emit_arg_for_param(arg, pt);
            }
        }
        fputc(')', stdout);
        return;
    }
    case ND_CAST:
        /* `new T` parses as ND_CAST with cast.ty=T and operand=NULL
         * (parser reuses the CAST node — see expr.c new-expression
         * handling). Emit as `((T*)0)` — typed null. Stub: doesn't
         * actually allocate; suitable when the call site never runs
         * (most gcc 4.8 vec_alloc<T> single-template-param invocations
         * are unreachable in cc1plus). A proper fix would emit
         * `((T*)__builtin_malloc(sizeof(T)))` plus a ctor call.
         * TODO(seafront#new-expr): real allocation. */
        if (!n->cast.operand) {
            fputs("((", stdout);
            emit_type(n->cast.ty);
            fputs("*)0)", stdout);
            return;
        }
        /* 'new T(args)' / 'new T' — the parser handed us a malloc-call
         * operand plus the user's ctor args. Emit a statement-expr
         *   ({ T *__tmp = (T *)malloc(sizeof(T)); ctor(__tmp, args);
         *      __tmp; })
         * so the expression's value is the freshly-constructed pointer.
         * GNU extension (cf. cproc side-goal memory) but the only
         * portable alternative is a hoisted local + comma operator,
         * which would require statement-level rewriting. */
        if (n->cast.is_new_expr && n->cast.ty && n->cast.ty->kind == TY_PTR &&
            n->cast.ty->base &&
            (n->cast.ty->base->kind == TY_STRUCT ||
             n->cast.ty->base->kind == TY_UNION)) {
            Type *pointee = n->cast.ty->base;
            Type *p_resolved = pointee;
            if (!p_resolved->class_region && p_resolved->tag) {
                Node *d = find_class_def_by_tag_args(p_resolved);
                if (!d) d = find_class_def_by_tag_only(p_resolved);
                if (d && d->class_def.ty) p_resolved = d->class_def.ty;
            }
            int na = n->cast.new_ctor_nargs;
            bool need_ctor = (na > 0) ||
                (p_resolved && p_resolved->has_default_ctor);
            if (need_ctor) {
                /* Resolve the matching ctor overload. For na==0 the
                 * call site wants the default ctor; sea-front
                 * synthesizes one when no user-declared ctor matches,
                 * so a failed lookup with na==0 still emits a 0-arg
                 * call against the synthesized mangled symbol. */
                Type **at = NULL;
                int   nat = collect_call_arg_types(
                    n->cast.new_ctor_args, na, &at);
                Type **pty = NULL;
                int np = resolve_overload(p_resolved, /*name=*/NULL,
                                           /*is_ctor=*/true, at, nat,
                                           /*receiver_is_const=*/false,
                                           &pty, /*out_best=*/NULL);
                if (np < 0 && na == 0 && p_resolved->has_default_ctor) {
                    np = 0;
                    pty = NULL;
                }
                /* Implicit copy ctor: 'new T(arg)' where no user ctor
                 * matches but 'arg' is convertible to 'const T&'. The
                 * implicit copy ctor (N4659 §15.8.1 [class.copy.ctor])
                 * does memberwise copy, which in C lowers to a struct
                 * assignment. Emit '*__tmp = arg;' instead of a ctor
                 * call. Without this, sea-front falls through to a
                 * mismatched-arity ctor call ('default ctor' + the
                 * arg) and gcc rejects with 'too many arguments'.
                 * Pattern: g++.dg/opt/pr15054.C 'new element(x)'. */
                if ((np < 0 || np != na) && na == 1) {
                    Node *arg0 = n->cast.new_ctor_args[0];
                    Type *art = arg0 ? arg0->resolved_type : NULL;
                    /* Decide whether to deref the arg to get the
                     * underlying class value. Reference-typed args
                     * lower to C pointers; we want '*p' to get the
                     * struct value. Three shapes we treat as "is a
                     * pointer at the C level": TY_REF/RVALREF on the
                     * resolved type, an ND_IDENT naming a ref param,
                     * or a TY_PTR-to-class (the user passed an
                     * already-deref'd pointer that needs a deref
                     * here). */
                    bool arg_is_ref = art &&
                        (art->kind == TY_REF || art->kind == TY_RVALREF);
                    bool arg_is_ref_param = arg0 &&
                        arg0->kind == ND_IDENT && arg0->ident.name &&
                        is_ref_param(arg0->ident.name);
                    bool need_deref = arg_is_ref || arg_is_ref_param;
                    fputs("({ ", stdout);
                    emit_type(n->cast.ty);
                    fputs(" __sf_new_tmp = (", stdout);
                    emit_type(n->cast.ty);
                    fputc(')', stdout);
                    emit_expr(n->cast.operand);
                    fputs("; *__sf_new_tmp = ", stdout);
                    if (need_deref) {
                        /* emit_arg_for_param would auto-deref ref-
                         * params already; emit the bare name and let
                         * the outer '*' do the work. */
                        bool saved_supp = g_suppress_ref_deref;
                        g_suppress_ref_deref = true;
                        fputs("*(", stdout);
                        emit_arg_for_param(arg0, NULL);
                        fputc(')', stdout);
                        g_suppress_ref_deref = saved_supp;
                    } else {
                        emit_arg_for_param(arg0, NULL);
                    }
                    fputs("; __sf_new_tmp; })", stdout);
                    return;
                }
                if (np >= 0) {
                    fputs("({ ", stdout);
                    emit_type(n->cast.ty);
                    fputs(" __sf_new_tmp = (", stdout);
                    emit_type(n->cast.ty);
                    fputc(')', stdout);
                    emit_expr(n->cast.operand);
                    fputs("; ", stdout);
                    mangle_class_ctor(p_resolved, pty, np);
                    fputs("(__sf_new_tmp", stdout);
                    for (int i = 0; i < na; i++) {
                        fputs(", ", stdout);
                        emit_arg_for_param(n->cast.new_ctor_args[i],
                                            i < np ? pty[i] : NULL);
                    }
                    fputs("); __sf_new_tmp; })", stdout);
                    return;
                }
            }
        }
        /* Cast TO a reference type — target lowers to T*. The cast
         * binds an lvalue (or the result of a ref-yielding expr) to
         * a reference, so the C-level emission is 'address of the
         * operand'. Same shape as unary '&':
         *   - operand is a ref-param ident: bare ident (already T*)
         *   - operand is itself a ref (lowered to T*): bare expr
         *   - otherwise: '&' + operand
         *
         * Pattern: 'static_cast<T&&>(x)' from std::move and the
         * common rvalue-ref binding idiom. N4659 §8.2.9 [expr.static.cast]
         * / §8.3.2 [dcl.ref]. Without this, '(struct T*)x' where x
         * is a value or '(struct T*)(*x)' where x is a ref-param both
         * fail C type-checking. */
        if (n->cast.ty &&
            (n->cast.ty->kind == TY_REF || n->cast.ty->kind == TY_RVALREF)) {
            Node *op = n->cast.operand;
            bool op_is_ref_param = op->kind == ND_IDENT &&
                                    is_ref_param(op->ident.name);
            Type *op_rt = op->resolved_type;
            bool op_already_ptr = ty_is_ref(op_rt);
            /* Derived-to-base reference binding via static_cast — the
             * target's referenced type is a (possibly multi-step) base
             * of the operand's class. Walk the __sf_base chain so we
             * land on the right subobject, not a reinterpret of the
             * derived layout. Without this, 'static_cast<B&&>(x)' for
             * 'X : A, B' returns the address of X (offset 0) cast to
             * B*, but B lives at offset sizeof(A) and accesses through
             * the cast read the wrong fields. Pattern:
             * g++.dg/cpp0x/rv-cast3.C. */
            Type *tgt_base = n->cast.ty->base;
            Type *deriv = op_rt;
            if (deriv && (deriv->kind == TY_REF || deriv->kind == TY_RVALREF))
                deriv = deriv->base;
            /* Patch up deriv to the class_def-bearing Type so
             * find_base_path can walk the base chain. The
             * resolved_type stamp on idents/calls is sometimes a
             * stub (no class_region). */
            if (deriv && !deriv->class_region && deriv->tag) {
                Node *d = find_class_def_by_tag_args(deriv);
                if (!d) d = find_class_def_by_tag_only(deriv);
                if (d && d->class_def.ty) deriv = d->class_def.ty;
            }
            /* Also patch tgt_base, which we'll compare to deriv's
             * bases by Type identity. */
            if (tgt_base && !tgt_base->class_region && tgt_base->tag) {
                Node *d = find_class_def_by_tag_args(tgt_base);
                if (!d) d = find_class_def_by_tag_only(tgt_base);
                if (d && d->class_def.ty) tgt_base = d->class_def.ty;
            }
            if (tgt_base && deriv && tgt_base != deriv &&
                (deriv->kind == TY_STRUCT || deriv->kind == TY_UNION) &&
                deriv->class_region) {
                /* Find tgt_base in deriv's bases by tag comparison
                 * (Type identity isn't reliable across cast vs class-
                 * region origin). One-level lookup is enough for the
                 * common direct-base case; multi-level would need
                 * recursive tag comparison. */
                int direct_base = -1;
                int nb = class_nbases(deriv);
                for (int b = 0; b < nb; b++) {
                    Type *bt = class_base(deriv, b);
                    if (!bt || !bt->tag || !tgt_base->tag) continue;
                    if (bt->tag->len == tgt_base->tag->len &&
                        memcmp(bt->tag->loc, tgt_base->tag->loc,
                               tgt_base->tag->len) == 0) {
                        direct_base = b;
                        break;
                    }
                }
                int path[8];
                int len = 0;
                if (direct_base >= 0) {
                    path[0] = direct_base;
                    len = 1;
                }
                if (len > 0) {
                    fputc('(', stdout);
                    emit_type(n->cast.ty);
                    fputc(')', stdout);
                    fputc('&', stdout);
                    fputc('(', stdout);
                    if (op_is_ref_param) {
                        bool saved = g_suppress_ref_deref;
                        g_suppress_ref_deref = true;
                        emit_expr(op);
                        g_suppress_ref_deref = saved;
                        fputs(")->", stdout);
                    } else if (op_already_ptr) {
                        emit_expr(op);
                        fputs(")->", stdout);
                    } else {
                        emit_expr(op);
                        fputs(").", stdout);
                    }
                    for (int i = 0; i < len; i++) {
                        if (i > 0) fputc('.', stdout);
                        if (path[i] == 0) fputs("__sf_base", stdout);
                        else fprintf(stdout, "__sf_base%d", path[i]);
                    }
                    return;
                }
            }
            fputc('(', stdout);
            emit_type(n->cast.ty);
            fputc(')', stdout);
            if (op_is_ref_param) {
                bool saved = g_suppress_ref_deref;
                g_suppress_ref_deref = true;
                emit_expr(op);
                g_suppress_ref_deref = saved;
            } else if (op_already_ptr) {
                emit_expr(op);
            } else {
                fputc('&', stdout);
                fputc('(', stdout);
                emit_expr(op);
                fputc(')', stdout);
            }
            return;
        }
        fputc('(', stdout);
        emit_type(n->cast.ty);
        fputc(')', stdout);
        emit_expr(n->cast.operand);
        return;
    case ND_SIZEOF:
        fputs("sizeof(", stdout);
        if (n->sizeof_.is_type && n->sizeof_.ty) {
            /* Preserve array bounds in sizeof — emit_type would decay
             * TY_ARRAY to '*' giving sizeof(T*) for a typedef'd array
             * type. C's sizeof never decays; '\''sizeof(int[5])'\'' is 20.
             * Pattern: gcc 4.8 ira.c'\''s '\''XNEWVEC(move_table, N)'\'' where
             * 'typedef unsigned short move_table[N_REG_CLASSES]'.
             * N4659 §8.3.3 [expr.sizeof]/2. */
            Type *st = n->sizeof_.ty;
            if (st && st->kind == TY_ARRAY) {
                Type *elem = st;
                while (elem && elem->kind == TY_ARRAY) elem = elem->base;
                if (elem) emit_type(elem);
                for (Type *d = st; d && d->kind == TY_ARRAY; d = d->base) {
                    if (d->array_len >= 0)
                        fprintf(stdout, "[%d]", d->array_len);
                    else if (d->array_size_expr) {
                        fputc('[', stdout);
                        emit_expr(d->array_size_expr);
                        fputc(']', stdout);
                    } else {
                        fputs("[]", stdout);
                    }
                }
            } else {
                emit_type(st);
            }
        } else if (n->sizeof_.expr) {
            emit_expr(n->sizeof_.expr);
        }
        fputc(')', stdout);
        return;
    case ND_ALIGNOF:
        fputs("_Alignof(", stdout);
        if (n->alignof_.ty) emit_type(n->alignof_.ty);
        fputc(')', stdout);
        return;
    case ND_STMT_EXPR:
        /* GCC statement-expression — emit the inner block inside
         * '({ ... })'. Target compilers (gcc 4.7 / gcc 4.8) accept
         * the extension. Non-standard; needed for glibc / libiberty
         * macros (obstack_alloc, XOBNEW, etc.). */
        fputc('(', stdout);
        if (n->stmt_expr.block)
            emit_stmt(n->stmt_expr.block);
        fputc(')', stdout);
        return;

    case ND_OFFSETOF: {
        /* __builtin_offsetof(type, member) — emit with the sea-front
         * type and the member-designator tokens verbatim.
         * Fix-up: in cloned template bodies, the local typedef used
         * as the type argument may not resolve (lost block scope).
         * If the type fell back to TY_INT and we're inside a class
         * method, substitute the class's struct type — the typedef
         * was almost certainly 'typedef ThisClass vec_embedded;'. */
        Type *off_ty = n->offsetof_.ty;
        if (off_ty && off_ty->kind == TY_INT && !off_ty->tag) {
            if (g_current_class_def && g_current_class_def->class_def.ty)
                off_ty = g_current_class_def->class_def.ty;
            else if (g_current_method_class)
                off_ty = g_current_method_class;
        }
        fputs("__builtin_offsetof(", stdout);
        if (off_ty && (off_ty->kind == TY_STRUCT || off_ty->kind == TY_UNION)) {
            fputs("struct ", stdout);
            emit_mangled_class_tag(off_ty);
        } else {
            emit_type(off_ty);
        }
        fputs(", ", stdout);
        for (int i = 0; i < n->offsetof_.n_mem_toks; i++) {
            Token *t = &n->offsetof_.mem_toks[i];
            if (t->has_space && i > 0) fputc(' ', stdout);
            fprintf(stdout, "%.*s", t->len, t->loc);
        }
        fputc(')', stdout);
        return;
    }
    case ND_VA_ARG:
        /* __builtin_va_arg(ap, type) — gcc handles the actual
         * extraction; we re-emit verbatim with the sea-front type
         * spelling. */
        fputs("__builtin_va_arg(", stdout);
        emit_expr(n->va_arg_.ap);
        fputs(", ", stdout);
        emit_type(n->va_arg_.ty);
        fputc(')', stdout);
        return;
    case ND_COMMA:
        fputc('(', stdout);
        emit_expr(n->comma.lhs);
        fputs(", ", stdout);
        emit_expr(n->comma.rhs);
        fputc(')', stdout);
        return;
    case ND_INIT_LIST:
        fputc('{', stdout);
        for (int i = 0; i < n->init_list.nelems; i++) {
            if (i > 0) fputs(", ", stdout);
            emit_expr(n->init_list.elems[i]);
        }
        if (n->init_list.nelems == 0) fputc('0', stdout);  /* C99: {0} legal */
        fputc('}', stdout);
        return;
    case ND_MEMBER: {
        /* Check if the member lives in a base class and needs
         * __sf_base chain rewriting. */
        Type *raw_obj_ty = n->member.obj ? n->member.obj->resolved_type : NULL;
        Type *obj_ty = raw_obj_ty;
        if (obj_ty && obj_ty->kind == TY_PTR) obj_ty = obj_ty->base;
        if (ty_is_ref(obj_ty)) obj_ty = obj_ty->base;
        Token *mem = n->member.member;
        /* Static member access (§10.1.1/4): 's.x' or 's->x' where
         * 'x' is a static data member lowers directly to the TU-
         * scope mangled symbol 'sf__<class>__x' — no obj traversal,
         * the obj is just used for type lookup here.
         * Note: this also applies to qualified static-member
         * function refs ('Class::static_method'), but those go
         * through ND_QUALIFIED, not ND_MEMBER. */
        if (obj_ty && obj_ty->class_region && mem) {
            Declaration *sd = region_lookup_own(obj_ty->class_region,
                                                 mem->loc, mem->len);
            if (sd && sd->is_static_member) {
                mangle_class_tag(obj_ty);
                fputs("__", stdout);
                emit_token_text(mem);
                return;
            }
        }
        /* Pick the access operator. Source-level '.' must become '->'
         * when the operand has been lowered to a pointer in our C —
         * either an actual pointer (TY_PTR) or a reference parameter
         * (TY_REF / TY_RVALREF, lowered to T*). N4659 §8.2.5
         * [expr.ref] — references behave as the referenced object,
         * so source uses '.' even though our C lowers to a pointer. */
        bool obj_is_ptr_in_c = ty_is_indirect(raw_obj_ty);
        /* Ref-returning method call as the obj: the ND_CALL emit
         * wraps with '(*...)' to convert the lowered T* back to a
         * value (unless the current function also returns a ref,
         * in which case it forwards as-is). Downstream member
         * access on that wrapped value must use '.' not '->'.
         * Without this, '(*call())->field' ends up emitted but
         * '*' dereffed to a struct value which has no '->'.
         * Pattern: gcc 4.8 dwarf2out.c 'files->last().info'. */
        bool obj_is_ref_call_unwrapped = false;
        if (ty_is_ref(raw_obj_ty) && n->member.obj &&
            n->member.obj->kind == ND_CALL) {
            bool cur_returns_ref = ty_is_ref(g_current_func_ret_ty);
            /* Only treat as 'unwrapped to value' when the referent is
             * itself a struct/union (then '.' works on the unwrapped
             * value). If the ref is to a pointer (vec<T>::last() with
             * T = tree = tree_node*, returning tree&), unwrapping gives
             * a pointer and we still need '->'. Pattern: gcc 4.8 stmt.c
             * 'dispatch_table.last().exp' where T=tree is a pointer. */
            Type *referent = raw_obj_ty ? raw_obj_ty->base : NULL;
            bool referent_is_struct_value = referent &&
                (referent->kind == TY_STRUCT || referent->kind == TY_UNION);
            if (!cur_returns_ref && referent_is_struct_value)
                obj_is_ref_call_unwrapped = true;
        }
        const char *access_op =
            (!obj_is_ref_call_unwrapped &&
             (n->member.op == TK_ARROW || obj_is_ptr_in_c)) ? "->" : ".";
        bool did_base_rewrite = false;
        /* obj_ty might be a Type copy without class_region — commonly
         * for types coming through typedef or field-decl paths.
         * Look the tag up in the TU to recover the canonical class
         * def (and thus its class_region + bases) so base-member
         * rewriting works. Without this, 'agg->contains_variable'
         * on an 'ipcp_agg_lattice' (which inherits contains_variable
         * from ipcp_lattice) emits as the raw member access and
         * fails at the C compiler. Pattern: gcc 4.8 ipa-cp.c. */
        Type *resolved_obj_ty = obj_ty;
        if (obj_ty && (obj_ty->kind == TY_STRUCT || obj_ty->kind == TY_UNION) &&
            !obj_ty->class_region && obj_ty->tag) {
            Node *d = find_class_def_by_tag_args(obj_ty);
            if (!d) d = find_class_def_by_tag_only(obj_ty);
            if (d && d->class_def.ty && d->class_def.ty->class_region)
                resolved_obj_ty = d->class_def.ty;
        }
        if (resolved_obj_ty && (resolved_obj_ty->kind == TY_STRUCT ||
                                 resolved_obj_ty->kind == TY_UNION) &&
            resolved_obj_ty->class_region && mem) {
            obj_ty = resolved_obj_ty;
        }
        if (obj_ty && (obj_ty->kind == TY_STRUCT || obj_ty->kind == TY_UNION) &&
            obj_ty->class_region && mem) {
            /* Check: is this member NOT in the class itself but in a base? */
            Declaration *own = region_lookup_own(obj_ty->class_region,
                                                  mem->loc, mem->len);
            if (!own) {
                /* Not found in own class — walk base chain TRANSITIVELY
                 * to find which path's class owns the member. DFS via
                 * each direct base, then its bases, etc. Records the
                 * index chain so we can emit
                 * 'obj->__sf_baseI.__sf_baseJ.member' for member
                 * inherited via a multi-level chain (e.g. diamond:
                 * Diamond's d.text reaches SubA's Base's text via
                 * d.__sf_base.__sf_base.text). */
                Type *cls_stack[8];
                int   idx_stack[8];
                int   slen = 0;
                int   final_path[8];
                int   final_len = 0;
                if (slen < 8) { cls_stack[slen] = obj_ty; idx_stack[slen] = -1; slen++; }
                bool found = false;
                while (slen > 0 && !found) {
                    Type *cur = cls_stack[slen-1];
                    if (!cur || !cur->class_region) { slen--; continue; }
                    int nb = cur->class_region->nbases;
                    /* Pop-and-resume: when idx_stack[slen-1] is -1
                     * we're entering 'cur' for the first time. Each
                     * iteration advances the index. */
                    int next_i = idx_stack[slen-1] + 1;
                    bool descended = false;
                    while (next_i < nb) {
                        DeclarativeRegion *br = cur->class_region->bases[next_i];
                        Declaration *bd = br ? region_lookup_own(br, mem->loc, mem->len) : NULL;
                        if (bd) {
                            /* Path = each ancestor's chosen index
                             * (idx_stack[0..slen-2]) followed by the
                             * just-matched next_i. */
                            for (int s = 0; s < slen - 1; s++)
                                final_path[s] = idx_stack[s];
                            final_path[slen-1] = next_i;
                            final_len = slen;
                            found = true;
                            break;
                        }
                        /* Descend into this base. */
                        if (br && br->owner_type && slen < 8) {
                            idx_stack[slen-1] = next_i;
                            cls_stack[slen] = br->owner_type;
                            idx_stack[slen] = -1;
                            slen++;
                            descended = true;
                            break;
                        }
                        next_i++;
                    }
                    if (!descended && !found) slen--;
                }
                if (found && final_len > 0) {
                    emit_expr(n->member.obj);
                    fputs(access_op, stdout);
                    for (int j = 0; j < final_len; j++) {
                        int bi = final_path[j];
                        if (bi == 0) fputs("__sf_base.", stdout);
                        else         fprintf(stdout, "__sf_base%d.", bi);
                    }
                    fprintf(stdout, "%.*s", mem->len, mem->loc);
                    did_base_rewrite = true;
                }
            }
        }
        if (!did_base_rewrite) {
            /* Precedence: member-access '.' / '->' binds tighter than
             * cast. Source '((cast)expr)->m' must keep the grouping,
             * else '(cast)expr->m' reparses as '(cast)(expr->m)'.
             * N4659 §8.2.5 [expr.ref] / C §6.5.2.3. */
            bool needs_paren = n->member.obj &&
                               n->member.obj->kind == ND_CAST;
            if (needs_paren) fputc('(', stdout);
            /* Suppress ref-param deref: the -> already handles it.
             * Only when the referent is a STRUCT (T&) — 'a->x' on a
             * T& param lowered to T* emits correctly as 'a->x'. For
             * T*& (ref to pointer) the param is lowered to T**, and
             * source 'a->x' needs '(*a)->x' — don't suppress the
             * deref there. Pattern: gcc 4.8 tree-ssa-structalias.c
             * 'constraint_less(const constraint_t &a, ...)' with
             * typedef constraint* constraint_t. */
            bool suppress_for_member = false;
            if (obj_is_ptr_in_c) {
                if (!ty_is_ref(raw_obj_ty)) {
                    /* Plain pointer / array — no ref to unwrap. */
                    suppress_for_member = true;
                } else if (raw_obj_ty->base) {
                    /* Ref: suppress only when referent is a struct/
                     * union value (the '(*a).field' case collapses
                     * cleanly to 'a->field'). */
                    Type *refd = raw_obj_ty->base;
                    if (refd->kind == TY_STRUCT || refd->kind == TY_UNION)
                        suppress_for_member = true;
                }
            }
            if (suppress_for_member) g_suppress_ref_deref = true;
            emit_expr(n->member.obj);
            if (needs_paren) fputc(')', stdout);
            fputs(access_op, stdout);
            if (mem)
                fprintf(stdout, "%.*s", mem->len, mem->loc);
        }
        return;
    }
    case ND_SUBSCRIPT: {
        /* Subscript dispatch — N4659 §8.2.1 [expr.sub] / §16.5.5 [over.sub].
         * A subscript on a class/union VALUE is an overloaded operator[]
         * call. A subscript on a POINTER (even a pointer-to-struct) is
         * plain C pointer arithmetic and must NOT be rewritten —
         * 'ptr_to_struct[i]' is idiomatic array notation, not an
         * operator call. Key distinction: the static type of the base.
         *
         * Class template instantiations sometimes elide the class_region
         * on the instantiated type, so we don't require it to be present —
         * trust that a struct/union value with a subscript was written
         * expecting operator[] and emit the method call. */
        Type *base_ty = n->subscript.base ? n->subscript.base->resolved_type : NULL;
        /* Peel TY_REF/TY_RVALREF: a source-level 'ref[i]' uses the
         * referent's operator[] — refs have value semantics in C++.
         * In our C lowering the ref is a T*, and ND_IDENT emit adds
         * the '(*...)' wrap, so the emitted base IS a value of the
         * referent class. N4659 §11.3.2 [dcl.ref]. Pattern: gcc 4.8
         * ipa-prop.c 'descriptors[i]' where descriptors is vec<T>&. */
        if (base_ty && (base_ty->kind == TY_REF || base_ty->kind == TY_RVALREF) &&
            base_ty->base)
            base_ty = base_ty->base;
        bool base_is_class_value =
            base_ty &&
            (base_ty->kind == TY_STRUCT || base_ty->kind == TY_UNION) &&
            base_ty->tag;
        if (base_is_class_value) {
            /* operator[] → mangled method call.
             * Resolve the specific overload FIRST so we can check
             * the WINNING candidate's return type for ref-return.
             * The previous 'lookup_in_scope(..., "operator", 8)'
             * would match any operator method (==, !=, etc.) in the
             * class, not specifically operator[] — giving wrong
             * ref-return decisions for classes with multiple
             * operators. N4659 §16.5.5 [over.sub]. */
            Type *idx_ty = n->subscript.index ? n->subscript.index->resolved_type : NULL;
            Type *args[1] = { idx_ty };
            Type **pty = NULL;
            Node *winner = NULL;
            bool base_const = receiver_type_is_const(
                n->subscript.base ? n->subscript.base->resolved_type : NULL);
            int np = resolve_operator_overload(base_ty, OP_SUBSCRIPT,
                                                args, 1, base_const,
                                                &pty, &winner);
            bool ref_return = false;
            if (winner) {
                Type *ret = NULL;
                if (winner->kind == ND_FUNC_DEF)
                    ret = winner->func.ret_ty;
                else if (winner->kind == ND_VAR_DECL && winner->var_decl.ty)
                    ret = winner->var_decl.ty->ret;
                if (ty_is_ref(ret))
                    ref_return = true;
            }
            /* Fallback for template types without resolution (class_def
             * not reachable): operator[] on container templates returns
             * T& by convention. */
            if (!ref_return && !winner && base_ty->n_template_args > 0)
                ref_return = true;
            /* Ref-forwarding chain: if the current function itself
             * returns a reference (lowered to T*), a ref-returning
             * subscript is used to FORWARD the reference — emit the
             * raw pointer (no extra deref), because our
             * emit_return_expr will convert value→pointer anyway.
             * Same rule as the method-call ref_ret handling. */
            bool cur_returns_ref = ty_is_ref(g_current_func_ret_ty);
            if (ref_return && !cur_returns_ref) fputs("(*", stdout);
            mangle_class_operator(base_ty, OP_SUBSCRIPT,
                                   np >= 0 ? pty : args,
                                   np >= 0 ? np : 1,
                                   candidate_is_const(winner));
            fputs("(&", stdout);
            emit_expr(n->subscript.base);
            fputs(", ", stdout);
            emit_expr(n->subscript.index);
            fputc(')', stdout);
            if (ref_return && !cur_returns_ref) fputc(')', stdout);
        } else {
            /* Precedence: subscript [] binds tighter than cast.
             * Source '((cast)expr)[idx]' must keep the grouping, else
             * emitted '(cast)expr[idx]' reparses as '(cast)(expr[idx])'.
             * N4659 §8.2.1 [expr.sub] / C §6.5.2.1. Parenthesize when
             * the base is a cast (or any other low-prec form we'd
             * otherwise miscompose). */
            bool needs_paren = n->subscript.base &&
                               n->subscript.base->kind == ND_CAST;
            if (needs_paren) fputc('(', stdout);
            emit_expr(n->subscript.base);
            if (needs_paren) fputc(')', stdout);
            fputc('[', stdout);
            emit_expr(n->subscript.index);
            fputc(']', stdout);
        }
        return;
    }
    case ND_QUALIFIED:
        /* '::name' (single-part global qualifier) — N4659 §8.1.4.3
         * [expr.prim.id.qual]: lookup in global namespace, ignoring
         * local/class shadows. Inside class methods this matters: e.g.
         * 'vec<T>::qsort' calls '::qsort' from libc, NOT recursively
         * dispatching to the same member. Emit parts[0] verbatim
         * (target is C linkage in every gcc 4.8 site we hit). */
        if (n->qualified.global_scope && n->qualified.nparts == 1 &&
            n->qualified.parts[0]) {
            Token *nm = n->qualified.parts[0];
            fprintf(stdout, "%.*s", nm->len, nm->loc);
            return;
        }
        /* Static data-member ref: 'Class::magic' — the trailing
         * unqualified-id names a static data member. N4659 §9.4.2
         * [class.static.data]: static data members lower to TU-scope
         * 'sf__<class>__<name>' (see codegen sibling rewrites in
         * emit_ident implicit-this and ND_MEMBER). Mirror the lookup
         * shape used for static-method calls (ND_CALL above): prefer
         * sema-populated resolved_class_type (carries template-args
         * for typedef aliases), otherwise probe by tag from parts[0]. */
        if (n->qualified.nparts >= 2 && n->qualified.parts) {
            Token *class_tok = n->qualified.parts[0];
            Token *mem_tok   = n->qualified.parts[n->qualified.nparts - 1];
            Type  *probe = NULL;
            if (n->qualified.resolved_class_type) {
                Node *cdef = find_class_def_by_tag_args(n->qualified.resolved_class_type);
                if (!cdef)
                    cdef = find_class_def_by_tag_only(n->qualified.resolved_class_type);
                probe = (cdef && cdef->class_def.ty) ? cdef->class_def.ty
                                                     : n->qualified.resolved_class_type;
            } else if (class_tok) {
                /* Build a tag-only stub. For 'Wrap<A>::v' the
                 * qualified-id carries lead_tid with the template
                 * args; populate the stub's template_args from it so
                 * mangle_class_tag produces the per-instantiation
                 * mangled name ('sf__Wrap_t_A_te_'). Without the args
                 * the mangled tag matches the primary template, not
                 * the instantiation, and the emitted symbol is
                 * undefined at link time. N4659 §17.7.1 [temp.inst]. */
                Type stub = {0};
                stub.kind = TY_STRUCT;
                stub.tag  = class_tok;
                Node *ltid = n->qualified.lead_tid;
                if (ltid && ltid->kind == ND_TEMPLATE_ID &&
                    ltid->template_id.nargs > 0) {
                    stub.n_template_args = ltid->template_id.nargs;
                    Type **buf = malloc(stub.n_template_args * sizeof(Type *));
                    if (!buf) abort();
                    /* Backing storage for synthesized TY_NTTP_VALUE
                     * Types when an arg is a literal expression
                     * (ND_NUM etc.) rather than a wrapped type. */
                    Type *nttp_buf = calloc(stub.n_template_args, sizeof(Type));
                    if (!nttp_buf) abort();
                    for (int i = 0; i < stub.n_template_args; i++) {
                        Node *a = ltid->template_id.args[i];
                        buf[i] = NULL;
                        if (!a) continue;
                        if (a->kind == ND_VAR_DECL) {
                            buf[i] = a->var_decl.ty;
                            continue;
                        }
                        Token *lit_tok = NULL;
                        switch (a->kind) {
                        case ND_NUM: case ND_FNUM:
                        case ND_BOOL_LIT: case ND_NULLPTR:
                            lit_tok = a->tok; break;
                        case ND_CHAR: lit_tok = a->chr.tok; break;
                        case ND_STR:  lit_tok = a->str.tok; break;
                        default: break;
                        }
                        if (lit_tok) {
                            nttp_buf[i].kind = TY_NTTP_VALUE;
                            nttp_buf[i].tag  = lit_tok;
                            buf[i] = &nttp_buf[i];
                        }
                    }
                    stub.template_args = buf;
                }
                Node *cdef = find_class_def_by_tag_args(&stub);
                if (!cdef) cdef = find_class_def_by_tag_only(&stub);
                if (cdef && cdef->class_def.ty) probe = cdef->class_def.ty;
                else if (stub.n_template_args > 0) {
                    /* No instantiated def yet — mangle against the
                     * stub so the access matches whatever the later
                     * instantiation pass emits. */
                    probe = &stub;
                }
            }
            if (probe && probe->class_region && mem_tok) {
                Declaration *sd = region_lookup_own(probe->class_region,
                                                     mem_tok->loc, mem_tok->len);
                if (sd && sd->is_static_member) {
                    mangle_class_tag(probe);
                    fputs("__", stdout);
                    emit_token_text(mem_tok);
                    return;
                }
            }
            /* If probe came from the stub but the instantiated def
             * isn't present (probe->class_region is NULL), the
             * fallback walks the TU at instantiation time — for the
             * static-member case we can still emit the mangled
             * Class<args>__member symbol with what we have, since
             * the instantiation pass emits a matching definition. */
            if (probe && !probe->class_region && probe->tag && mem_tok &&
                probe->n_template_args > 0) {
                mangle_class_tag(probe);
                fputs("__", stdout);
                emit_token_text(mem_tok);
                return;
            }
        }
        /* Scoped-enum member access: 'kind::NONE' where 'kind' is an
         * 'enum class' / 'enum struct'. N4659 §10.2/5 [dcl.enum]:
         * scoped-enum enumerators are accessed via 'EnumName::value'
         * only. Sea-front's parser registers enumerators in the
         * enclosing scope (see type.c — sea-front doesn't yet model
         * the scoped-enum lookup distinction). The C emit puts the
         * enum body inline ('enum K { NONE, OTHER }') making the
         * enumerators flat TU-scope C constants; so a bare emit of
         * the trailing identifier resolves correctly without further
         * machinery. Real-world hit: gcc 14 libcpp/lex.cc 'kind::NONE'.
         * Standard alignment: TODO(seafront#scoped-enum-region): track
         * enum-class scope in the parser and verify membership before
         * emitting. */
        if (n->qualified.nparts >= 2 && n->qualified.parts && g_tu) {
            Token *class_tok = n->qualified.parts[0];
            Token *mem_tok   = n->qualified.parts[n->qualified.nparts - 1];
            if (class_tok && mem_tok &&
                find_enum_tag_in_tu(g_tu, class_tok)) {
                fprintf(stdout, "%.*s", mem_tok->len, mem_tok->loc);
                return;
            }
        }
        /* Class-qualified instance-member access from inside the
         * enclosing class — 'A::i' inside B which inherits from A.
         * Lower to 'this->__sf_baseN.i' via the same base-chain walk
         * emit_ident uses for unqualified members. Without this, the
         * fallback below emits a bare 'i', which the C compiler
         * either rejects ('i undeclared') or silently mis-resolves
         * to an outer variable. Pattern: g++.dg/cpp0x/nsdmi-virtual1.C
         * 'int k = A::i + A::f();'. */
        if (n->qualified.nparts >= 2 && n->qualified.parts &&
            g_current_class_def) {
            Token *class_tok = n->qualified.parts[0];
            Token *mem_tok   = n->qualified.parts[n->qualified.nparts - 1];
            if (class_tok && mem_tok) {
                Type stub = {0};
                stub.kind = TY_STRUCT;
                stub.tag  = class_tok;
                Node *qdef = find_class_def_by_tag_only(&stub);
                if (qdef && qdef->class_def.ty &&
                    qdef->class_def.ty->class_region) {
                    Type *qcls = qdef->class_def.ty;
                    Type *cur = g_current_class_def->class_def.ty;
                    int path[8];
                    int len = find_base_path(cur, qcls, path, 8);
                    if (len > 0) {
                        /* Verify the trailing name is an instance
                         * data member of the qualifying class (not a
                         * method — methods route through ND_CALL). */
                        Declaration *md = region_lookup_own(
                            qcls->class_region,
                            mem_tok->loc, mem_tok->len);
                        if (md && !md->is_static_member &&
                            md->type && md->type->kind != TY_FUNC) {
                            fputs("this->", stdout);
                            emit_base_chain(path, len);
                            emit_token_text(mem_tok);
                            return;
                        }
                    }
                }
            }
        }
        /* Namespace-qualified value access ('foo::x' where foo is a
         * namespace). Sea-front flattens namespaces (C has none) so
         * the variable lives at TU scope under its bare name. Emit
         * the trailing identifier verbatim. Real-world hit: gcc 14
         * libcpp/lex.cc 'bidi::utf8_start'.
         *
         * SHORTCUT (ours, not the standard): bare-emitting any
         * 2+ part qualified-id we don't otherwise recognise.
         * Standard alternative would be to verify parts[0] is a
         * namespace tag via the namespace-region registry — but the
         * registry isn't easily accessible from codegen and a
         * mis-match here surfaces as a link error rather than a
         * silent runtime miscompile, which is acceptable.
         * TODO(seafront#qid-emit): track namespace tags in the
         * codegen-side TU index so this branch can verify. */
        if (n->qualified.nparts >= 2 && n->qualified.parts) {
            Token *mem_tok = n->qualified.parts[n->qualified.nparts - 1];
            if (mem_tok) {
                fprintf(stdout, "%.*s", mem_tok->len, mem_tok->loc);
                return;
            }
        }
        fprintf(stderr,
                "emit_expr: unhandled ND_QUALIFIED nparts=%d global=%d",
                n->qualified.nparts, (int)n->qualified.global_scope);
        for (int i = 0; i < n->qualified.nparts; i++) {
            Token *t = n->qualified.parts[i];
            fprintf(stderr, " '%.*s'", t ? t->len : 0, t ? t->loc : "");
        }
        fputc('\n', stderr);
        abort();
    case ND_TYPE_TRAIT: {
        /* Deferred type-trait intrinsic, kept as ND_TYPE_TRAIT at
         * parse time because at least one type argument was dependent.
         * Template substitution may have replaced the args with
         * concrete types by now; re-evaluate. Falls back to '0' for
         * any still-dependent argument (eval_type_trait already
         * returns 0 for TY_DEPENDENT in every branch). */
        int r = eval_type_trait(n->type_trait.name,
                                n->type_trait.arg0,
                                n->type_trait.arg1);
        fputc(r ? '1' : '0', stdout);
        return;
    }
    case ND_TYPEID: {
        /* typeid — N4659 §8.2.7 [expr.typeid]. Lower to the sentinel
         * address pre-collected by typeid_collect_walk; comparisons
         * (==/!=) then reduce to pointer equality, correct for static
         * types. See the docstring on typeid_collect_walk for the
         * polymorphic-glvalue caveat. */
        fprintf(stdout, "((const void *)&%s)",
                typeid_sym_for_type(typeid_resolved_type(n)));
        return;
    }
    case ND_TEMPLATE_ID: {
        /* Bare template-id 'Foo<args>' in expression position. Most
         * sites that produce one (subscript base, callee, type arg)
         * have already special-cased it before falling here. Anything
         * left over is the call-site pattern that ND_CALL emit didn't
         * recognise — emit just the bare template name so the
         * surrounding C parses (call(args), unary-on-name, etc.).
         * The resulting symbol is undeclared at link time, surfacing
         * the unresolved template instantiation as a link error
         * rather than a silently-wrong C compile.
         * TODO(seafront#tmpl-id-expr): proper instantiation lookup. */
        if (n->template_id.name)
            fprintf(stdout, "%.*s",
                    n->template_id.name->len, n->template_id.name->loc);
        else
            fputc('0', stdout);
        return;
    }
    case ND_THROW: {
        /* Throw at non-statement-position (inside a ternary arm,
         * function-call argument, etc.). N4659 §8.17 [expr.throw] —
         * a throw-expression has type void but is permitted as one
         * arm of a conditional, etc. The cproc-portability goal
         * prefers ISO C; here we accept the GNU stmt-expr because
         * `goto lbl` cannot otherwise appear in expression position.
         *
         * Shape: `({ __SF_THROW_PRIM(...); 0; })`. The `0` is the
         * value yielded by the stmt-expr — never reached, since the
         * macro's `goto lbl` jumps to a handler. Type-wise it's int,
         * which converts to whatever the enclosing context demands
         * (bool/long/etc.) via implicit conversion. Class-typed
         * throws in expression position still route through
         * __SF_THROW_CLASS with the operand's address.
         *
         * Pattern: g++.dg/eh/cond5.C `(x ? true : throw 1)`. */
        if (!g_cf.func_has_cleanups) {
            /* The enclosing function wasn't pre-scanned with unwind
             * machinery — no __SF_unwind, no __SF_epilogue label. This
             * happens when a throw appears inside an NSDMI body that
             * was emitted as part of an implicitly-synthesised ctor
             * (pattern: g++.dg/cpp0x/nsdmi-eh1.C — Core issue 1351,
             * which gcc itself xfails). Fall back to the silent-drop
             * placeholder so the emitted C still compiles. */
            fputs("0 /* throw */", stdout);
            return;
        }
        Node *op = n->throw_.operand;
        Type *op_ty = op ? op->resolved_type : NULL;
        bool in_try = false;
        int target = find_throw_target_from(g_cf.nlive, &in_try);
        char buf[32];
        const char *lbl;
        if (target >= 0) {
            snprintf(buf, sizeof(buf),
                     in_try ? "__SF_try_%d_handler" : "__SF_cleanup_%d",
                     target);
            lbl = buf;
        } else {
            lbl = "__SF_epilogue";
        }
        if (n->throw_.is_rethrow) {
            fprintf(stdout,
                    "({ __sf_exc_state.state = __SF_UNWIND_THROW; "
                    "__SF_unwind = __SF_UNWIND_THROW; "
                    "goto %s; 0; })",
                    lbl);
        } else if (op_ty &&
                   (op_ty->kind == TY_STRUCT || op_ty->kind == TY_UNION)) {
            fputs("({ __SF_THROW_CLASS(&(", stdout);
            emit_expr(op);
            fprintf(stdout, "), 0, %s); 0; })", lbl);
        } else {
            fputs("({ __SF_THROW_PRIM(", stdout);
            if (op) emit_expr(op);
            else fputs("0", stdout);
            fprintf(stdout, ", &__sf_typeinfo_int, %s); 0; })", lbl);
        }
        return;
    }
    default:
        /* Silent-discard placeholder is a trap: the surrounding C
         * stays parseable (e.g. a comment-shaped placeholder
         * followed by '(v, size)' becomes a comma-expression call),
         * so the bug only surfaces at runtime as a corrupted value.
         * Hard-fail here forces unhandled cases to be addressed at
         * codegen time, not chased through ICE backtraces. */
        fprintf(stderr,
                "emit_expr: unhandled NodeKind=%d at %s:%d\n",
                (int)n->kind,
                n->tok && n->tok->loc ? "<source>" : "<synth>",
                0);
        abort();
    }
}

/* ------------------------------------------------------------------ */
/* Statement emission                                                 */
/* ------------------------------------------------------------------ */

static void emit_stmt(Node *n);

/* Emit an initializer expression, applying derived-to-base pointer
 * adjustment when the target type is 'Base *' and the init is a
 * 'Derived *' value. The matching cases are TY_PTR-to-class on both
 * sides with distinct class types where Derived has Base in its
 * inheritance chain. Without this, 'B *b = &d;' (where 'D : ..., B')
 * stored the unadjusted D-pointer in b, and downstream dispatch via
 * b reached the wrong vtable / wrong subobject. N4659 §7.11
 * [conv.ptr]. Falls through to plain emit_expr for non-conversion
 * cases. */
static void emit_init_with_target(Node *init, Type *target_ty) {
    if (!init || !target_ty || target_ty->kind != TY_PTR ||
        !target_ty->base ||
        (target_ty->base->kind != TY_STRUCT &&
         target_ty->base->kind != TY_UNION)) {
        emit_expr(init); return;
    }
    Type *src = init->resolved_type;
    if (!src || src->kind != TY_PTR || !src->base ||
        (src->base->kind != TY_STRUCT && src->base->kind != TY_UNION) ||
        src->base == target_ty->base) {
        emit_expr(init); return;
    }
    Type *deriv = src->base;
    if (!deriv->class_region && deriv->tag) {
        Node *d = find_class_def_by_tag_args(deriv);
        if (!d) d = find_class_def_by_tag_only(deriv);
        if (d && d->class_def.ty && d->class_def.ty->class_region)
            deriv = d->class_def.ty;
    }
    int base_path[8];
    int base_len = find_base_path(deriv, target_ty->base, base_path, 8);
    if (base_len == 0 && target_ty->base->tag) {
        Type *tgt = target_ty->base;
        int nb = class_nbases(deriv);
        for (int b = 0; b < nb; b++) {
            Type *base = class_base(deriv, b);
            if (!base || !base->tag) continue;
            if (base->tag->len == tgt->tag->len &&
                memcmp(base->tag->loc, tgt->tag->loc,
                       tgt->tag->len) == 0) {
                base_path[0] = b; base_len = 1; break;
            }
        }
    }
    if (base_len == 0) { emit_expr(init); return; }
    fputs("&(", stdout);
    emit_expr(init);
    fputs(")->", stdout);
    if (base_len > 1) emit_base_chain(base_path, base_len - 1);
    int last = base_path[base_len - 1];
    if (last == 0) fputs("__sf_base", stdout);
    else           fprintf(stdout, "__sf_base%d", last);
}

static void emit_var_decl_inner(Node *n) {
    Type *ty = n->var_decl.ty;
    /* Bare enum definition without a variable:
     *   enum bb_state { NOT_IN_BB, IN_ONE_BB, IN_MULTIPLE_BB };
     * Parses as ND_VAR_DECL with TY_ENUM and no name. emit_top_level
     * has an explicit branch for this at file scope; block-scope hits
     * us via emit_stmt's ND_VAR_DECL case. Without this pre-check,
     * emit_type would print only 'enum bb_state' and the body would
     * be silently dropped, leaving the enumerators undeclared.
     * Pattern: gcc 4.8 cfgrtl.c print_rtl_with_bb local enum. */
    if (ty && ty->kind == TY_ENUM && ty->enum_tokens &&
        ty->enum_ntokens > 0 && !n->var_decl.name) {
        if (enum_body_already_emitted(ty->enum_tokens)) return;
        mark_enum_body_emitted(ty->enum_tokens);
        ty->codegen_emitted = true;
        fputs("enum ", stdout);
        if (ty->tag)
            fprintf(stdout, "%.*s ", ty->tag->len, ty->tag->loc);
        fputs("{ ", stdout);
        emit_enum_body(ty);
        fputs(" }", stdout);
        return;
    }
    /* Inline enum-type var-decl: 'enum { A=0, B } x;' carries the
     * enumerator list on Type.enum_tokens. Emit the enum definition
     * in the declarator so NOT_FLOAT / AFTER_POINT etc. are visible
     * in the enclosing scope. N4659 §10.2 [dcl.enum]. Only applies
     * when we haven't already emitted this enum elsewhere (top-level
     * bare 'enum X {};' marks codegen_emitted). */
    if (ty && ty->kind == TY_ENUM && ty->enum_tokens &&
        ty->enum_ntokens > 0 && n->var_decl.name) {
        if (enum_body_already_emitted(ty->enum_tokens)) {
            /* Already emitted at top level — emit just the type
             * reference, not the body. */
            fputs("enum ", stdout);
            if (ty->tag)
                fprintf(stdout, "%.*s ", ty->tag->len, ty->tag->loc);
            fprintf(stdout, "%.*s",
                    n->var_decl.name->len, n->var_decl.name->loc);
            if (n->var_decl.init) {
                fputs(" = ", stdout);
                emit_init_with_target(n->var_decl.init, n->var_decl.ty);
            }
            return;
        }
        mark_enum_body_emitted(ty->enum_tokens);
        ty->codegen_emitted = true;
        fputs("enum ", stdout);
        if (ty->tag)
            fprintf(stdout, "%.*s ", ty->tag->len, ty->tag->loc);
        fputs("{ ", stdout);
        emit_enum_body(ty);
        fprintf(stdout, " } %.*s",
                n->var_decl.name->len, n->var_decl.name->loc);
        if (n->var_decl.init) {
            fputs(" = ", stdout);
            emit_init_with_target(n->var_decl.init, n->var_decl.ty);
        }
        return;
    }
    /* Array declarations: C requires 'int arr[10]' not 'int* arr'.
     * Emit the element type, then the name, then [N]. For unsized
     * arrays (int arr[]) emit just []. For function parameters,
     * emit_type already decays to pointer — this path handles
     * local/global variable declarations only. */
    /* Function-pointer var-decl: 'int (*name)(args)' — N4659 §11.3.1
     * [dcl.ptr]. After the grouped-declarator fix, these are
     * TY_PTR(TY_FUNC). C syntax interleaves name and params in the
     * declarator, so emit_type alone can't produce it. */
    if (ty && ty->kind == TY_PTR && ty->base && ty->base->kind == TY_FUNC &&
        n->var_decl.name) {
        Type *fty = ty->base;
        emit_type(fty->ret);
        fprintf(stdout, " (*%.*s)(", n->var_decl.name->len, n->var_decl.name->loc);
        emit_func_param_types(fty);
        fputc(')', stdout);
        if (n->var_decl.init && n->var_decl.init->kind > 0 &&
            n->var_decl.init->kind < 200) {
            fputs(" = ", stdout);
            emit_init_with_target(n->var_decl.init, n->var_decl.ty);
        }
        return;
    }
    /* Function type as var-decl: a local or top-level function
     * declaration. N4659 §11.3.5 [dcl.fct]. C syntax is
     *   ret name(params);
     * not 'functype name;'. Emit the full prototype. */
    if (ty && ty->kind == TY_FUNC && n->var_decl.name) {
        /* For an extern-C function, only emit the FIRST decl with this
         * name; later mismatched-sig decls would clash on the bare C
         * symbol. Pattern: <cstring>'s dual strchr overloads. */
        if ((n->var_decl.storage_flags & DECL_C_LINKAGE) &&
            !ffsig_is_first_c_linkage(n->var_decl.name, ty->params, ty->nparams))
            return;
        /* GCC constructor / destructor function attributes — pass
         * through to the C compiler which wires the function into
         * .init_array / .fini_array. Pattern: g++.dg/init/attrib1.C.
         * Goes before the return type so gcc sees it on the prototype;
         * the matching function definition picks the attribute up via
         * the prior declaration. Priority arg controls cross-ctor
         * order (g++.dg/special/initpri1.C). */
        if (n->var_decl.attr_constructor) {
            if (n->var_decl.ctor_priority > 0)
                fprintf(stdout, "__attribute__((constructor(%d))) ",
                        n->var_decl.ctor_priority);
            else
                fputs("__attribute__((constructor)) ", stdout);
        }
        if (n->var_decl.attr_destructor) {
            if (n->var_decl.dtor_priority > 0)
                fprintf(stdout, "__attribute__((destructor(%d))) ",
                        n->var_decl.dtor_priority);
            else
                fputs("__attribute__((destructor)) ", stdout);
        }
        emit_type(ty->ret);
        fputc(' ', stdout);
        emit_free_func_symbol(n->var_decl.name, n->var_decl.asm_name,
                               (n->var_decl.storage_flags & DECL_C_LINKAGE) != 0,
                               ty->params, ty->nparams);
        fputc('(', stdout);
        for (int i = 0; i < ty->nparams; i++) {
            if (i > 0) fputs(", ", stdout);
            emit_type(ty->params[i]);
        }
        if (ty->is_variadic) {
            if (ty->nparams > 0) fputs(", ", stdout);
            fputs("...", stdout);
        } else if (ty->nparams == 0) {
            fputs("void", stdout);
        }
        fputc(')', stdout);
        return;
    }
    /* Array (any dim) of function pointers:
     *   ret (*name[N])(args)         — 1D
     *   ret (*name[M][N])(args)      — 2D
     * C requires the dimensions and the '*' to live inside the
     * grouped declarator. Walking the TY_ARRAY chain handles
     * arbitrary rank. N4659 §11.3.4 [dcl.array] + §11.3.5 [dcl.fct].
     * Patterns:
     *   gcc 4.8 tree-vect-patterns.c — 1D
     *     static vect_recog_func_ptr vect_vect_recog_func_ptrs[10]
     *   gcc 4.8 i386.c — 2D
     *     static rtx (*gen_extract[6][2])(rtx, rtx) = ... */
    {
        Type *t = ty;
        int dims = 0;
        while (t && t->kind == TY_ARRAY) { dims++; t = t->base; }
        if (dims > 0 && t && t->kind == TY_PTR && t->base &&
            t->base->kind == TY_FUNC && n->var_decl.name) {
            Type *fty = t->base;
            emit_type(fty->ret);
            fprintf(stdout, " (*%.*s",
                    n->var_decl.name->len, n->var_decl.name->loc);
            Type *dt = ty;
            for (int i = 0; i < dims; i++) {
                if (dt->array_len >= 0) {
                    fprintf(stdout, "[%d]", dt->array_len);
                } else if (dt->array_size_expr) {
                    fputc('[', stdout);
                    emit_expr(dt->array_size_expr);
                    fputc(']', stdout);
                } else {
                    fputs("[]", stdout);
                }
                dt = dt->base;
            }
            fputs(")(", stdout);
            emit_func_param_types(fty);
            fputc(')', stdout);
            if (n->var_decl.init) {
                fputs(" = ", stdout);
                emit_init_with_target(n->var_decl.init, n->var_decl.ty);
            }
            return;
        }
    }
    /* Pointer-to-array / array-of-pointer-to-array — N4659 §11.3
     * [dcl.meaning]: C declarator syntax interleaves '*' with the
     * name. Generalized shape:
     *   ELEM (***NAME[outer_dims])[inner_dims]
     * supporting any number of leading outer-array layers, any number
     * of pointer layers, and any number of inner-array layers. Without
     * this branch the generic emit_type collapses array layers to '*'
     * and loses the inner array bounds.
     *
     * Real-world bites:
     *   gcc 4.8 ira-int.h: move_table *x_..._[MAX_MACHINE_MODE]
     *     (1 PTR + 1 inner-array — indexing depends on the layout)
     *   gcc 4.8 cgraph.h: cgraph_node **node_pointers[N]
     *     (2 PTRs — pointer-to-pointer-to-array shape)
     *
     * Detect: zero-or-more leading TY_ARRAY, then ONE-OR-MORE TY_PTR,
     * then ONE-OR-MORE TY_ARRAY, then a non-array/non-ptr element. */
    if (ty && n->var_decl.name &&
        emit_pointer_to_array_declarator(ty, n->var_decl.name)) {
        if (n->var_decl.init) {
            fputs(" = ", stdout);
            emit_init_with_target(n->var_decl.init, n->var_decl.ty);
        }
        return;
    }
    if (ty && ty->kind == TY_ARRAY) {
        /* Multi-dim arrays: 'T name[N1][N2]'. Walk the TY_ARRAY chain
         * to the INNERMOST element type, emit that, then emit name
         * followed by all dimensions in source order. emit_type would
         * decay TY_ARRAY to pointer (used in expression contexts) and
         * give us 'T* name[N1]' — wrong layout, breaks &arr[0][0] as
         * a constant initializer. Pattern: gcc 4.8 emit-rtl.c
         *   rtx const_tiny_rtx[4][(int) MAX_MACHINE_MODE]; */
        Type *elem = ty;
        while (elem->kind == TY_ARRAY && elem->base) elem = elem->base;
        /* Inline enum-with-enumerators as the element type:
         *   'enum X { A, B, C } arr[N];'
         * Emit the full enum body instead of just the type name, so
         * the enumerators are declared alongside the array. Pattern:
         * gcc 4.8 reload.c 'enum reload_usage { RELOAD_READ, ... }
         * modified[MAX_RECOG_OPERANDS];' — a block-scope local. */
        if (elem && elem->kind == TY_ENUM && elem->enum_tokens &&
            elem->enum_ntokens > 0 &&
            !enum_body_already_emitted(elem->enum_tokens)) {
            mark_enum_body_emitted(elem->enum_tokens);
            elem->codegen_emitted = true;
            fputs("enum ", stdout);
            if (elem->tag)
                fprintf(stdout, "%.*s ", elem->tag->len, elem->tag->loc);
            fputs("{ ", stdout);
            emit_enum_body(elem);
            fputs(" }", stdout);
        } else {
            emit_type(elem);
        }
        fputc(' ', stdout);
        if (n->var_decl.name)
            fprintf(stdout, "%.*s", n->var_decl.name->len,
                    n->var_decl.name->loc);
        /* Emit each dimension. Outermost first — matches source order. */
        for (Type *d = ty; d && d->kind == TY_ARRAY; d = d->base) {
            if (d->array_len >= 0) {
                fprintf(stdout, "[%d]", d->array_len);
            } else if (d->array_size_expr) {
                fputc('[', stdout);
                emit_expr(d->array_size_expr);
                fputc(']', stdout);
            } else {
                fputs("[]", stdout);
            }
        }
        /* Array init (if any) */
        if (n->var_decl.init) {
            fputs(" = ", stdout);
            emit_init_with_target(n->var_decl.init, n->var_decl.ty);
        }
        return;
    }
    /* Anonymous local struct: emit the body inline since no top-level
     * definition exists. Pattern: 'struct { char x; int y; } align;'
     * inside a function body (sbitmap.c alignment computation).
     * Detected by: no user-given tag (tag == NULL before anon_id
     * assignment) and class_def present but not yet emitted. */
    if (ty && (ty->kind == TY_STRUCT || ty->kind == TY_UNION) &&
        ty->class_def && !ty->codegen_emitted && n->var_decl.name &&
        !ty->tag) {
        Node *cd = ty->class_def;
        fputs(ty->kind == TY_UNION ? "union" : "struct", stdout);
        fputs(" { ", stdout);
        for (int i = 0; i < cd->class_def.nmembers; i++) {
            Node *m = cd->class_def.members[i];
            if (m && m->kind == ND_VAR_DECL) {
                emit_type(m->var_decl.ty);
                if (m->var_decl.name)
                    fprintf(stdout, " %.*s", m->var_decl.name->len,
                            m->var_decl.name->loc);
                fputs("; ", stdout);
            }
        }
        fprintf(stdout, "} %.*s",
                n->var_decl.name->len, n->var_decl.name->loc);
        if (n->var_decl.init) {
            fputs(" = ", stdout);
            emit_init_with_target(n->var_decl.init, n->var_decl.ty);
        }
        return;
    }
    emit_type(ty);
    fputc(' ', stdout);
    if (n->var_decl.name) {
        if (n->var_decl.class_type) {
            /* Out-of-class static data member definition. Emit the
             * mangled name so the symbol matches the in-class
             * declaration and doesn't clash with C symbols of the
             * same name (libc 'abort' etc.). N4659 §11.4.9
             * [class.static.data]. Pattern: g++.dg/init/array16.C. */
            mangle_class_static_data_member(n->var_decl.class_type,
                                             n->var_decl.name);
        } else {
            fprintf(stdout, "%.*s",
                    n->var_decl.name->len, n->var_decl.name->loc);
        }
    }
    /* Bit-field width — N4659 §12.2.4 [class.bit] */
    if (n->var_decl.bitfield_width) {
        fputs(" : ", stdout);
        emit_expr(n->var_decl.bitfield_width);
    }
    /* GCC cleanup attribute — pass through so the back-end cc handles
     * the cleanup semantics on the C-level variable. Goes between the
     * declarator-id and the initializer in gcc's accepted grammar. */
    if (n->var_decl.cleanup_attr_name) {
        fputs(" __attribute__((cleanup(", stdout);
        emit_token_text(n->var_decl.cleanup_attr_name);
        fputs(")))", stdout);
    }
    if (n->var_decl.init) {
        fputs(" = ", stdout);
        /* Zero-arg functional-cast init of a struct var:
         *   static vinsn_vec_t x = vinsn_vec_t();
         * The init is ND_CALL with a bare ident callee whose
         * resolved_decl didn't make it through sema (happens for
         * typedef names at file scope when sema can't look them up).
         * Still-invalid to emit as 'x = vinsn_vec_t()' — there's no
         * such function. Detect the shape and substitute '{0}'.
         * Gate on: callee's ident name matches the var's type's tag
         * OR resolved_decl is ENTITY_TYPE — otherwise this is a real
         * function call ('Point b = make();') that must pass through.
         * Pattern: gcc 4.8 sel-sched-ir.c
         *   static vinsn_vec_t vec_bookkeeping_blocked_vinsns
         *     = vinsn_vec_t(); */
        if (n->var_decl.init->kind == ND_CALL &&
            n->var_decl.init->call.nargs == 0 &&
            n->var_decl.init->call.callee &&
            n->var_decl.init->call.callee->kind == ND_IDENT &&
            ty && (ty->kind == TY_STRUCT || ty->kind == TY_UNION)) {
            bool is_func_cast = false;
            Node *cid = n->var_decl.init->call.callee;
            if (cid->ident.resolved_decl &&
                cid->ident.resolved_decl->entity == ENTITY_TYPE)
                is_func_cast = true;
            else if (!cid->ident.resolved_decl && cid->ident.name &&
                     ty->tag) {
                /* Unresolved ident — sema didn't find it as a function.
                 * If the LHS declaration used the same name as the
                 * ident, it's a functional cast (typedef name). */
                /* var_decl.ty->tag is the canonical struct tag, which
                 * may differ from the typedef name. Compare against
                 * the decl-specifier tag captured at parse time — not
                 * easily accessible here, so use a broader heuristic:
                 * the type's tag (may be a mangled template name) OR
                 * the original decl-specifier if present. */
                if (cid->ident.name->len == ty->tag->len &&
                    memcmp(cid->ident.name->loc, ty->tag->loc,
                           ty->tag->len) == 0) {
                    is_func_cast = true;
                } else {
                    /* Fallback: any 0-arg unresolved-ident call as a
                     * struct var init — sea-front can't emit a call
                     * to an unknown function anyway, so '{0}' is the
                     * safer lowering. For real functions, resolved_decl
                     * is set. */
                    is_func_cast = true;
                }
            }
            if (is_func_cast) {
                /* Classes with virtual methods or a default ctor need
                 * the ctor body to run — fall through to emit_expr,
                 * which uses the stmt-expr lowering for value-init.
                 * Without this, the bare '{0}' init leaves the vptr
                 * NULL and any virtual call through the var segfaults.
                 * Pattern: g++.dg/init/value1.C 'A a = A();'. */
                if (ty->has_virtual_methods) {
                    /* fall through to emit_init_with_target below;
                     * the expression-context path emits a stmt-expr
                     * that runs the ctor (vptr setup). Non-polymorphic
                     * classes stay on the '{0}' shortcut so static-
                     * storage initializers remain constant. */
                } else {
                    fputs("{0}", stdout);
                    return;
                }
            }
        }
        /* If the variable is a struct value but the init expression
         * returns a reference (TY_REF lowered to T*), dereference.
         * Patterns:
         *   T elem = func_ref();   — function returning T&
         * Detected by: resolved_type is TY_REF.
         *
         * ND_SUBSCRIPT on a class already bakes its own '(*...)'
         * into the emit when ref_return — we must NOT double-wrap
         * here. */
        Node *init_e = n->var_decl.init;
        Type *init_rt = init_e ? init_e->resolved_type : NULL;
        bool init_is_ref = ty_is_ref(init_rt);
        if (init_is_ref && init_e && init_e->kind == ND_SUBSCRIPT) {
            Type *base_ty = init_e->subscript.base ?
                init_e->subscript.base->resolved_type : NULL;
            if (base_ty && (base_ty->kind == TY_STRUCT ||
                            base_ty->kind == TY_UNION))
                init_is_ref = false;  /* subscript handles its own deref */
        }
        /* Method call returning T&: emit_call already wraps the call
         * in '(*...)' to deref the lowered T* — see the ND_CALL case
         * around 'method_returns_ref'. Don't double-wrap here.
         * Pattern: gcc 4.8 gimple-low.c
         *   return_statements_t t = data.return_statements.pop ();
         * where vec::pop returns T&. */
        if (init_is_ref && init_e && init_e->kind == ND_CALL &&
            init_e->call.callee && init_e->call.callee->kind == ND_MEMBER) {
            Node *cm = init_e->call.callee;
            Type *ot = cm->member.obj ? cm->member.obj->resolved_type : NULL;
            if (ot && ot->kind == TY_PTR && ot->base) ot = ot->base;
            if (ty_is_ref(ot)) ot = ot->base;
            if (ot && (ot->kind == TY_STRUCT || ot->kind == TY_UNION) &&
                method_returns_ref(ot, cm->member.member))
                init_is_ref = false;
        }
        bool var_is_struct = ty &&
            (ty->kind == TY_STRUCT || ty->kind == TY_UNION);
        /* Reference variable initialized from an lvalue: the C
         * lowering is a pointer that must hold the lvalue's address.
         *   T& r  = x;      →  T* r  = &x;
         *   const T& cr = x;→  const T* cr = &x;
         * Only takes address when the init's own type isn't already
         * a ref/ptr (where the address would be redundant).
         * N4659 §11.3.2 [dcl.ref]. */
        bool var_is_ref = ty_is_ref(ty);
        bool init_is_ptr = init_rt && init_rt->kind == TY_PTR;
        if (init_is_ref && var_is_struct) {
            /* The init's type is a reference; in C the underlying
             * value sits behind one pointer indirection. Suppress the
             * default ref-param deref inside the init emission so the
             * '(*...)' wrapper here is the only deref — otherwise an
             * ND_IDENT ref-param init produces '*(*a)', a double
             * deref that the C compiler rejects.
             * Pattern: g++.dg/torture/pr59163.C 'A c = a;' with A&. */
            fputs("(*", stdout);
            bool saved_supp = g_suppress_ref_deref;
            g_suppress_ref_deref = true;
            emit_init_with_target(n->var_decl.init, n->var_decl.ty);
            g_suppress_ref_deref = saved_supp;
            fputc(')', stdout);
        } else if (var_is_ref && !init_is_ref && !init_is_ptr) {
            fputc('&', stdout);
            emit_init_with_target(n->var_decl.init, n->var_decl.ty);
        } else if (var_is_ref && init_e && init_e->kind == ND_IDENT &&
                   is_ref_param(init_e->ident.name)) {
            /* Ref var bound to another ref-param: both lower to T*,
             * emit the bare ident (suppress the usual deref). Pattern:
             *   void f(T& a) { T& b = a; ... }     — same referent
             *   const T& c = a;
             *   auto& d = a;                       — auto deduces T&
             * Without this, emit_expr would produce '(*a)' and the
             * decl 'T* b = (*a)' fails C type-checking. */
            bool saved = g_suppress_ref_deref;
            g_suppress_ref_deref = true;
            emit_expr(init_e);
            g_suppress_ref_deref = saved;
        } else {
            emit_init_with_target(n->var_decl.init, n->var_decl.ty);
        }
    } else if (n->var_decl.has_ctor_init && n->var_decl.ty &&
               n->var_decl.ty->kind != TY_STRUCT &&
               n->var_decl.ctor_nargs >= 1) {
        /* Brace-init / paren-init for a non-class type:
         *   int x{7}    →  int x = 7
         *   int x(7)    →  int x = 7
         * The ctor-call branch in the ND_VAR_DECL emit_stmt case
         * is gated on TY_STRUCT, so non-class direct-inits with
         * args land here. We use the first arg as a copy-init
         * source. Multi-arg forms aren't meaningful for scalars. */
        fputs(" = ", stdout);
        emit_expr(n->var_decl.ctor_args[0]);
    }
}

/* Emit the cleanup chain for entries that were pushed onto live[]
 * after 'saved_nlive'. Emits one (label, dtor) pair per CL_VAR in
 * reverse-declaration order followed by the three-way chain-out.
 *
 * Used by both emit_block (end-of-block cleanup) and the mini-block
 * wrapper (per-full-expression temp cleanup). The shape is identical;
 * the difference is just where it's invoked. */
static void emit_cleanup_chain_for_added(int saved_nlive) {
    int added = 0;
    for (int i = saved_nlive; i < g_cf.nlive; i++)
        if (g_cf.live[i].kind == CL_VAR) added++;
    if (added == 0) return;

    for (int i = g_cf.nlive - 1; i >= saved_nlive; i--) {
        CleanupEntry *e = &g_cf.live[i];
        if (e->kind != CL_VAR) continue;
        Node *v = e->var_decl;
        emit_indent();
        fprintf(stdout, "__SF_cleanup_%d: ;\n", e->label_id);
        emit_indent();
        if (v->codegen_temp_name) {
            /* Slice D-Hoist temp: 'var_decl' is actually the
             * original ND_CALL whose result was hoisted to the
             * synthesized local. Use the call's resolved_type
             * for the class tag and the codegen_temp_name for
             * the address. */
            mangle_class_dtor(v->resolved_type);
            fprintf(stdout, "(&%s);\n", v->codegen_temp_name);
        } else if (v->var_decl.ty &&
                   v->var_decl.ty->kind == TY_ARRAY &&
                   v->var_decl.ty->base &&
                   v->var_decl.ty->base->kind == TY_STRUCT) {
            /* Array of class — emit a destruct-each-element loop in
             * REVERSE order (N4659 §15.6.2/12 [class.base.init] —
             * destruction is the reverse of construction). For
             * unsized arrays the parser may leave array_len unset;
             * fall back to a guarded sizeof / sizeof(elem) that
             * yields 0 when the element size is 0 (gcc empty-struct
             * extension — sizeof(empty_struct)==0 — would otherwise
             * trip cc's div-by-zero trap). Pattern: g++.dg/init/array14.C. */
            Type *arr_ty = v->var_decl.ty;
            Type *elem_ty = arr_ty->base;
            if (arr_ty->array_len > 0) {
                fprintf(stdout, "for (int __sf_i = %d; __sf_i >= 0; --__sf_i) ",
                        arr_ty->array_len - 1);
            } else {
                fprintf(stdout,
                    "for (int __sf_i = (int)(sizeof(%.*s[0]) ? "
                    "sizeof(%.*s)/sizeof(%.*s[0]) : 1) - 1; "
                    "__sf_i >= 0; --__sf_i) ",
                    v->var_decl.name->len, v->var_decl.name->loc,
                    v->var_decl.name->len, v->var_decl.name->loc,
                    v->var_decl.name->len, v->var_decl.name->loc);
            }
            mangle_class_dtor(elem_ty);
            fprintf(stdout, "(&%.*s[__sf_i]);\n",
                    v->var_decl.name->len, v->var_decl.name->loc);
        } else {
            mangle_class_dtor(v->var_decl.ty);
            fprintf(stdout, "(&%.*s);\n",
                    v->var_decl.name->len, v->var_decl.name->loc);
        }
    }
    /* Three-way (now four-way) chain-out. See the comment in
     * emit_block where this was originally written.
     *
     * THROW dispatch: if there's an enclosing CL_TRY between this
     * scope and the next outer CL_VAR (or no outer CL_VAR at all),
     * an in-flight throw must reach the try-block's handler — NOT
     * the function epilogue, which would skip the catch entirely.
     * Pattern: g++.dg/eh/ctor1.C — dtor throws while inside a
     * try-block; member subobjects still need to be destroyed, then
     * control transfers to catch. find_throw_target_from with
     * in_try=true detects this case. */
    char rt_buf[40], bt_buf[40], ct_buf[40], th_buf[40];
    bool have_break = false, have_cont = false;
    bool have_throw_try = false;
    bool top_is_loop = (saved_nlive > 0 &&
                        g_cf.live[saved_nlive - 1].kind == CL_LOOP);

    int rt = find_return_target_from(saved_nlive);
    if (rt >= 0)
        snprintf(rt_buf, sizeof(rt_buf), "__SF_cleanup_%d", rt);
    else
        snprintf(rt_buf, sizeof(rt_buf), "__SF_epilogue");

    int bt = find_break_target_from(saved_nlive);
    if (bt >= 0) {
        have_break = true;
        if (top_is_loop)
            snprintf(bt_buf, sizeof(bt_buf), "__SF_loop_break_%d", bt);
        else
            snprintf(bt_buf, sizeof(bt_buf), "__SF_cleanup_%d", bt);
    }

    int ct = find_cont_target_from(saved_nlive);
    if (ct >= 0) {
        have_cont = true;
        if (top_is_loop)
            snprintf(ct_buf, sizeof(ct_buf), "__SF_loop_cont_%d", ct);
        else
            snprintf(ct_buf, sizeof(ct_buf), "__SF_cleanup_%d", ct);
    }

    bool throw_in_try = false;
    int th = find_throw_target_from(saved_nlive, &throw_in_try);
    if (throw_in_try) {
        have_throw_try = true;
        snprintf(th_buf, sizeof(th_buf), "__SF_try_%d_handler", th);
    }

    if (have_throw_try) {
        emit_indent();
        fprintf(stdout, "__SF_CHAIN_THROW(%s);\n", th_buf);
    }

    bool collapse = have_break && have_cont &&
                    strcmp(rt_buf, bt_buf) == 0 &&
                    strcmp(rt_buf, ct_buf) == 0;
    if (collapse || (!have_break && !have_cont)) {
        emit_indent();
        fprintf(stdout, "__SF_CHAIN_ANY(%s);\n", rt_buf);
    } else {
        emit_indent();
        fprintf(stdout, "__SF_CHAIN_RETURN(%s);\n", rt_buf);
        if (have_break) {
            emit_indent();
            fprintf(stdout, "__SF_CHAIN_BREAK(%s);\n", bt_buf);
        }
        if (have_cont) {
            emit_indent();
            fprintf(stdout, "__SF_CHAIN_CONT(%s);\n", ct_buf);
        }
    }
}

/* Predicate: does an expression contain any class-typed temp
 * (an ND_CALL whose return type is a non-trivially-destructible
 * class)? Used by stmt_has_class_temp to decide whether a
 * statement should be wrapped in a per-full-expression mini-block. */
static bool expr_has_class_temp(Node *e) {
    if (!e) return false;
    if (is_class_temp_call(e)) return true;
    switch (e->kind) {
    case ND_CALL:
        if (expr_has_class_temp(e->call.callee)) return true;
        for (int i = 0; i < e->call.nargs; i++)
            if (expr_has_class_temp(e->call.args[i])) return true;
        return false;
    case ND_BINARY:
    case ND_ASSIGN:
        /* Operator overload on a struct-returning rvalue LHS needs
         * hoisting: the emit path does 'op(&lhs, rhs)' which can't
         * take '&' of an rvalue call. Mirror the force-hoist branch
         * in hoist_temps_in_expr's ND_BINARY/ND_ASSIGN case.
         * Pattern: gcc 4.8 fold-const.c 'c1.bitand(c2) == c1' AND
         * '(c1 & c2) == c1' (nested struct operators). */
        if (e->binary.lhs) {
            Node *lhs = e->binary.lhs;
            NodeKind lk = lhs->kind;
            if (lk == ND_CALL && lhs->resolved_type &&
                (lhs->resolved_type->kind == TY_STRUCT ||
                 lhs->resolved_type->kind == TY_UNION))
                return true;
            /* Nested struct-operator: '(a op b)' where both operands
             * are struct-typed. ND_UNARY TK_STAR is a regular pointer
             * deref (lvalue) unless the operand is itself a struct —
             * which would be 'operator*' overload. */
            if (lk == ND_BINARY) {
                Type *lhs_op_ty = lhs->binary.lhs ?
                    lhs->binary.lhs->resolved_type : NULL;
                if (lhs_op_ty && (lhs_op_ty->kind == TY_STRUCT ||
                                   lhs_op_ty->kind == TY_UNION))
                    return true;
            } else if (lk == ND_UNARY || lk == ND_POSTFIX) {
                /* Only trigger for non-deref unary: deref '*p' where
                 * p is a ptr IS an lvalue; 'operator-' etc. on struct
                 * operand is an rvalue. */
                if (lhs->unary.op != TK_STAR) {
                    Type *op_ty = lhs->unary.operand ?
                        lhs->unary.operand->resolved_type : NULL;
                    if (op_ty && (op_ty->kind == TY_STRUCT ||
                                   op_ty->kind == TY_UNION))
                        return true;
                }
            }
        }
        return expr_has_class_temp(e->binary.lhs) ||
               expr_has_class_temp(e->binary.rhs);
    case ND_UNARY:
    case ND_POSTFIX:
        /* Struct-operator-overload rvalue operand ('~(a - b)' where
         * '-' returns struct). Mirrors the force-hoist branch in
         * hoist_temps_in_expr's ND_UNARY case. */
        if (e->unary.operand) {
            Node *op = e->unary.operand;
            NodeKind ok = op->kind;
            if (ok == ND_CALL && op->resolved_type &&
                (op->resolved_type->kind == TY_STRUCT ||
                 op->resolved_type->kind == TY_UNION))
                return true;
            if (ok == ND_BINARY) {
                Type *sub_ty = op->binary.lhs ?
                    op->binary.lhs->resolved_type : NULL;
                if (sub_ty && (sub_ty->kind == TY_STRUCT ||
                                sub_ty->kind == TY_UNION))
                    return true;
            }
        }
        return expr_has_class_temp(e->unary.operand);
    case ND_TERNARY:
        return expr_has_class_temp(e->ternary.cond) ||
               expr_has_class_temp(e->ternary.then_) ||
               expr_has_class_temp(e->ternary.else_);
    case ND_MEMBER:
        /* Also true when the member access is on a struct-valued
         * RVALUE (call result, operator overload result, ternary) —
         * the ND_MEMBER emit will take '&expr' which needs the value
         * hoisted into a named temp first. Pattern: gcc 4.8
         * gimple-fold.c '(a - b).sext(p)' — the operator- overload
         * returns struct, then .sext() needs an addressable receiver. */
        if (e->member.obj) {
            Node *obj = e->member.obj;
            NodeKind k = obj->kind;
            bool is_rvalue_shape = (k == ND_CALL || k == ND_BINARY ||
                                     k == ND_ASSIGN || k == ND_UNARY ||
                                     k == ND_POSTFIX || k == ND_TERNARY);
            if (is_rvalue_shape && obj->resolved_type &&
                (obj->resolved_type->kind == TY_STRUCT ||
                 obj->resolved_type->kind == TY_UNION))
                return true;
            /* For ND_BINARY whose own resolved_type is NULL but whose
             * operands are struct-valued — likely an operator-overload
             * between structs, whose return is also a struct. sema's
             * common_arith_type returns NULL for struct operands, so
             * we can't read the return type directly — infer from the
             * operand. */
            if (k == ND_BINARY && !obj->resolved_type) {
                Type *lt = obj->binary.lhs ?
                    obj->binary.lhs->resolved_type : NULL;
                if (lt && (lt->kind == TY_STRUCT || lt->kind == TY_UNION))
                    return true;
            }
        }
        return expr_has_class_temp(e->member.obj);
    case ND_SUBSCRIPT:
        return expr_has_class_temp(e->subscript.base) ||
               expr_has_class_temp(e->subscript.index);
    case ND_CAST:
        return expr_has_class_temp(e->cast.operand);
    case ND_COMMA:
        return expr_has_class_temp(e->comma.lhs) ||
               expr_has_class_temp(e->comma.rhs);
    default:
        return false;
    }
}

/* Predicate: would hoist_stmt_temps actually hoist anything for
 * this statement? Used to decide whether to wrap the statement in
 * a mini-block for per-full-expression temp scoping. */
static bool stmt_has_class_temp(Node *s) {
    if (!s) return false;
    switch (s->kind) {
    case ND_VAR_DECL: {
        Node *init = s->var_decl.init;
        if (!init) return false;
        bool direct_init =
            init->kind == ND_CALL && init->resolved_type &&
            init->resolved_type->kind == TY_STRUCT &&
            s->var_decl.ty && s->var_decl.ty->kind == TY_STRUCT;
        if (direct_init) {
            if (expr_has_class_temp(init->call.callee)) return true;
            for (int i = 0; i < init->call.nargs; i++)
                if (expr_has_class_temp(init->call.args[i])) return true;
            return false;
        }
        return expr_has_class_temp(init);
    }
    case ND_EXPR_STMT:
        return expr_has_class_temp(s->expr_stmt.expr);
    case ND_RETURN:
        return expr_has_class_temp(s->ret.expr);
    default:
        return false;
    }
}

static void push_user_var_cleanup(Node *s);

/* Bracket the body of a decl-as-cond (if/while/for/switch with a
 * class-with-dtor variable in the condition slot) so any early
 * `return` / `break` / `continue` / `throw` inside the body chains
 * through the var's cleanup label, firing the dtor. Without this,
 * the switch case `default: return;` jumps straight to the function
 * epilogue and the dtor never runs. Pattern: g++.dg/eh/scope1.C. */
static int decl_cond_cleanup_open(Node *decl_node) {
    int saved = g_cf.nlive;
    push_user_var_cleanup(decl_node);
    return saved;   /* may equal g_cf.nlive if no cleanup was pushed */
}

static void decl_cond_cleanup_close(int saved_nlive) {
    if (g_cf.nlive == saved_nlive) return;
    emit_cleanup_chain_for_added(saved_nlive);
    g_cf.nlive = saved_nlive;
}

/* `T v = T(args);` — copy-init from a temp constructed inline —
 * should invoke T's ctor on v (with elision). The C-level
 * `T v = (T){0};` is bitwise zero, skipping T's ctor side effects.
 * Detect the explicit ctor-call shape: init is ND_CALL whose callee
 * is a type-name (ENTITY_TYPE/ENTITY_TAG) AND the target class has
 * a default ctor (otherwise zero-init via init list is the right
 * lowering for a trivially-constructible class). Plain
 * `T v = func();` does NOT match — that's a function returning T
 * and the C-level copy semantics are correct.
 * Rewrites `n` in place into has_ctor_init form so the existing
 * has_ctor_init emit path (`T v; T_ctor(&v, args);`) takes over.
 * N4659 §11.6/14 [dcl.init.aggr] — copy-init from a ctor-call
 * collapses to a single direct-init under elision. */
static void rewrite_copy_init_ctor_call(Node *n) {
    if (!n || n->kind != ND_VAR_DECL) return;
    if (!n->var_decl.init || n->var_decl.init->kind != ND_CALL) return;
    if (n->var_decl.has_ctor_init) return;
    if (!n->var_decl.ty || n->var_decl.ty->kind != TY_STRUCT) return;
    if (!n->var_decl.ty->has_default_ctor) return;
    Node *callee = n->var_decl.init->call.callee;
    if (!callee || callee->kind != ND_IDENT) return;
    Declaration *d = callee->ident.resolved_decl;
    if (!d || (d->entity != ENTITY_TYPE && d->entity != ENTITY_TAG))
        return;
    Node *call = n->var_decl.init;
    n->var_decl.has_ctor_init = true;
    n->var_decl.ctor_args = call->call.args;
    n->var_decl.ctor_nargs = call->call.nargs;
    n->var_decl.init = NULL;
}

/* Push a CL_VAR for a user-declared class local onto the live stack.
 * Pulled out so emit_block and the mini-block path share it.
 * Transparently looks through ND_LABEL wrappers — a labelled
 * declaration `again: C v;` still introduces v into scope and its
 * dtor must fire when a goto crosses out. Pattern: g++.dg/init/goto1.C.
 *
 * Also handles arrays of class-with-dtor (`C arr[N];`): the dtor must
 * fire for every element at scope exit. emit_cleanup_chain_for_added
 * detects the array shape and emits a loop. Pattern:
 * g++.dg/init/array14.C. */
static void push_user_var_cleanup(Node *s) {
    if (!g_cf.func_has_cleanups || !s)
        return;
    while (s && s->kind == ND_LABEL && s->label.stmt)
        s = s->label.stmt;
    if (s->kind != ND_VAR_DECL || !s->var_decl.ty || !s->var_decl.name)
        return;
    Type *ty = s->var_decl.ty;
    bool class_with_dtor = ty->kind == TY_STRUCT && ty->has_dtor;
    bool array_of_class_with_dtor =
        ty->kind == TY_ARRAY && ty->base &&
        ty->base->kind == TY_STRUCT && ty->base->has_dtor;
    if (!class_with_dtor && !array_of_class_with_dtor)
        return;
    cleanup_live_reserve(1);
    g_cf.live[g_cf.nlive].kind = CL_VAR;
    g_cf.live[g_cf.nlive].label_id = g_cf.next_label_id++;
    g_cf.live[g_cf.nlive].cont_label_id = -1;
    g_cf.live[g_cf.nlive].var_decl = s;
    g_cf.nlive++;
}

/* Slice D-MiniBlock: emit one statement with per-full-expression
 * temp scoping. The statement is wrapped in a synthesized C block
 * so any hoisted temps from its expressions die at the end of the
 * full-expression rather than at the end of the surrounding user
 * block.
 *
 * For ND_VAR_DECL the user-declared variable must outlive the
 * mini-block (subsequent statements need to see it), so we split:
 *   <type> name;       (uninitialized, OUTSIDE mini-block)
 *   { hoisted temps; name = init; cleanup chain; }
 *
 * For ND_EXPR_STMT and ND_RETURN no splitting is needed; just
 * wrap the whole statement.
 *
 * For other statement kinds (ND_IF/WHILE/FOR conditions etc.)
 * we currently fall back to the non-mini-block path; per-cond
 * mini-blocking is a future refinement. */
static void emit_stmt_with_miniblock(Node *s) {
    /* For var-decl: declare the user var OUTSIDE the mini-block,
     * register its cleanup BEFORE opening the mini-block (so the
     * temps inside chain to it correctly), then assign inside. */
    bool is_var_decl = (s->kind == ND_VAR_DECL && s->var_decl.name &&
                        s->var_decl.ty);
    if (is_var_decl) {
        emit_indent();
        emit_type(s->var_decl.ty);
        fprintf(stdout, " %.*s;\n",
                s->var_decl.name->len, s->var_decl.name->loc);
        push_user_var_cleanup(s);
    }

    emit_indent();
    emit_open_brace();
    int saved_nlive = g_cf.nlive;

    /* Hoist temps inside the mini-block. They get pushed onto live[]
     * above saved_nlive, so the cleanup chain at the bottom of this
     * mini-block fires their dtors before control leaves. */
    hoist_stmt_temps(s);

    emit_indent();
    if (is_var_decl) {
        /* Emit assignment instead of declaration: 'name = init;'.
         * The init expression has been processed by hoist_temps so
         * any inner calls have been substituted with __SF_temp_<n>. */
        if (s->var_decl.init) {
            fprintf(stdout, "%.*s = ",
                    s->var_decl.name->len, s->var_decl.name->loc);
            emit_expr(s->var_decl.init);
            fputs(";\n", stdout);
        } else {
            fputs(";\n", stdout);
        }
    } else {
        emit_stmt(s);
    }

    emit_cleanup_chain_for_added(saved_nlive);
    g_cf.nlive = saved_nlive;
    emit_close_brace();
}

/* Anonymous union/struct member rewrite — stack of active frames.
 * When an anonymous block-scope class-def is emitted (no source tag,
 * no instance variable), the union is given a synth instance name
 * '__sf_anon_inst_<id>' and its members are recorded here so bare
 * identifier references in the enclosing block can be rewritten to
 * the instance-qualified form. C11 6.7.2.1/13 covers anonymous
 * struct/union *members*; this lowering supports the gcc extension
 * that admits the same shape at function/file scope. */
typedef struct {
    int anon_id;
    Node *class_def_node;
} AnonRewriteFrame;
/* Stack of active anonymous-union rewrite frames. Grows to fit the
 * deepest nesting at runtime; any number of frames is supported. */
static AnonRewriteFrame *g_anon_rewrite_stack = NULL;
static int g_anon_rewrite_top = 0;
static int g_anon_rewrite_cap = 0;

static void anon_rewrite_reserve(int need) {
    if (g_anon_rewrite_top + need <= g_anon_rewrite_cap) return;
    int ncap = g_anon_rewrite_cap ? g_anon_rewrite_cap * 2 : 8;
    while (ncap < g_anon_rewrite_top + need) ncap *= 2;
    AnonRewriteFrame *n = realloc(g_anon_rewrite_stack,
                                   ncap * sizeof(AnonRewriteFrame));
    if (!n) abort();
    g_anon_rewrite_stack = n;
    g_anon_rewrite_cap = ncap;
}

/* If 'name' matches a member of any active anon-union frame, emit
 * the rewritten qualified form and return true; otherwise false. */
static bool emit_anon_member_rewrite(Token *name) {
    if (!name) return false;
    for (int f = g_anon_rewrite_top - 1; f >= 0; f--) {
        Node *cd = g_anon_rewrite_stack[f].class_def_node;
        for (int mi = 0; mi < cd->class_def.nmembers; mi++) {
            Node *m = cd->class_def.members[mi];
            if (!m || m->kind != ND_VAR_DECL || !m->var_decl.name) continue;
            if (m->var_decl.name->len == name->len &&
                memcmp(m->var_decl.name->loc, name->loc, name->len) == 0) {
                fprintf(stdout, "__sf_anon_inst_%d.%.*s",
                        g_anon_rewrite_stack[f].anon_id,
                        name->len, name->loc);
                return true;
            }
        }
    }
    return false;
}

static void emit_block(Node *n) {
    /* Flat blocks (comma-separated declarators, namespace/extern "C" bodies)
     * emit their statements directly without wrapping { } braces, so
     * variables remain visible in the enclosing scope. */
    if (n->block.is_flat) {
        for (int i = 0; i < n->block.nstmts; i++) {
            emit_indent();
            emit_stmt(n->block.stmts[i]);
        }
        return;
    }

    emit_open_brace();

    /* Per-var cleanup label refactor. We walk statements in order
     * and PUSH onto g_cf.live exactly when we see a dtor-bearing
     * var-decl — so at every point during emission innermost-
     * cleanup-label reflects construction state, not block topology. */
    int saved_nlive = g_cf.nlive;
    int saved_anon_top = g_anon_rewrite_top;

    /* Pre-scan for anonymous block-scope class-defs so member
     * references later in the same block are rewritten to
     * '__sf_anon_inst_<id>.member'. The synth instance id is
     * allocated here (matching what emit_class_def's anon branch
     * uses) so the rewrite name stays consistent. */
    for (int i = 0; i < n->block.nstmts; i++) {
        Node *s = n->block.stmts[i];
        if (s && s->kind == ND_CLASS_DEF && !s->class_def.tag &&
            s->class_def.ty &&
            (s->class_def.ty->kind == TY_STRUCT ||
             s->class_def.ty->kind == TY_UNION)) {
            if (s->class_def.ty->anon_id == 0)
                s->class_def.ty->anon_id = ++g_anon_counter;
            anon_rewrite_reserve(1);
            g_anon_rewrite_stack[g_anon_rewrite_top].anon_id =
                s->class_def.ty->anon_id;
            g_anon_rewrite_stack[g_anon_rewrite_top].class_def_node = s;
            g_anon_rewrite_top++;
        }
    }

    for (int i = 0; i < n->block.nstmts; i++) {
        Node *s = n->block.stmts[i];
        if (g_cf.func_has_cleanups && stmt_has_class_temp(s)) {
            /* Slice D-MiniBlock: wrap statement so its temps die
             * at end of full-expression, not end of outer block. */
            emit_stmt_with_miniblock(s);
        } else {
            /* Regular path: hoist (no-op if no temps), emit, then
             * register the user var as a CL_VAR if it's a class
             * local with a dtor. */
            hoist_stmt_temps(s);
            emit_indent();
            emit_stmt(s);
            push_user_var_cleanup(s);
        }
    }

    emit_cleanup_chain_for_added(saved_nlive);
    g_cf.nlive = saved_nlive;
    g_anon_rewrite_top = saved_anon_top;

    emit_close_brace();
}

static void emit_class_def(Node *n);
static void emit_fwd_decl_methods_only(Node *n);

/* If an if/while/for single-statement body needs hoisting (e.g. a
 * method call argument is itself a call whose result must be
 * addressed), the hoisted temp has no valid scope in 'if (cond)
 * stmt;' — there's no block to host the decl. Wrap non-block bodies
 * in '{ }' so hoist_stmt_temps can emit the temp decl before the
 * statement inside the block. For ND_BLOCK bodies, emit_block
 * handles per-statement hoisting itself — just delegate.
 * Pattern: gcc 4.8 cgraphunit.c assemble_thunk 'if (this_adjusting)
 * vargs.quick_push(thunk_adjust(...));'. */
/* Emit a condition expression in a Boolean context (if/while/for/?:).
 *
 * N4659 §7.3 [conv]/3 + §16.3.1.6 [over.match.copy]/2: if the
 * expression's type is a class type, contextual conversion to bool
 * invokes a user-defined `operator bool()` (or other matching
 * conversion function). Sea-front parses the conversion function as
 * a method named "operator"; we look it up in the class scope and
 * emit a method call that yields a scalar.
 *
 * If no conversion function is reachable (or the expression isn't a
 * class type), emit the expression as-is — cc handles bool / int /
 * pointer naturally. */
static void emit_bool_context_expr(Node *expr) {
    if (!expr) return;
    Type *t = expr->resolved_type;
    if (ty_is_ref(t)) t = t->base;
    if (t && (t->kind == TY_STRUCT || t->kind == TY_UNION) &&
        t->tag && t->class_region) {
        Declaration *opd = lookup_in_scope(t->class_region, "operator", 8);
        if (opd && opd->type && opd->type->kind == TY_FUNC) {
            /* Conversion operator — Itanium encodes as cv<T> per
             * §5.1.4.1; the human path encodes as `__op_<T>`.
             * Either way the mangling module owns the dispatch. */
            Token op_tok = { .kind = TK_IDENT, .loc = "operator", .len = 8 };
            bool mc = method_is_const(t, &op_tok);
            Type *target = opd->type->ret;
            mangle_class_conversion(t, target, mc);
            fputs("(&(", stdout);
            emit_expr(expr);
            fputs("))", stdout);
            return;
        }
    }
    emit_expr(expr);
}

static void emit_if_body_with_hoist(Node *body) {
    if (!body) { fputs(";\n", stdout); return; }
    if (body->kind == ND_BLOCK) { emit_stmt(body); return; }
    emit_open_brace();
    hoist_stmt_temps(body);
    emit_indent();
    emit_stmt(body);
    emit_close_brace();
}

static void emit_stmt(Node *n) {
    if (!n) { fputs(";\n", stdout); return; }
    switch (n->kind) {
    case ND_BLOCK:
        emit_block(n);
        return;
    case ND_RETURN:
        if (g_cf.func_has_cleanups) {
            /* Slice B: 'return expr' lowers to one of the __SF_RETURN
             * macros from the prelude. Picking the macro form keeps
             * the emitted C readable, drives the protocol from one
             * place, and means an unbraced 'if (cond) return;' stays
             * safe (the macro wraps a do-while). */

            /* D-Return (NRVO-style move): if the operand is a bare
             * identifier naming the topmost CL_VAR — i.e. a class
             * local that lives at the top of the live stack right
             * now — treat the return as a move. The local IS the
             * return value; its lifetime now belongs to the caller's
             * temp, so we must NOT fire its dtor in the callee.
             *
             * Implementation: bypass the local's own cleanup label
             * and target the next outer label (or __SF_epilogue).
             * This skips the dtor on the return path while leaving
             * fall-through paths to the same label still firing it
             * — so a function with conditional 'return t;' alongside
             * other paths that drop t cleanly destroys t in those
             * other paths. No runtime flag needed. */
            bool moved = false;
            if (n->ret.expr && n->ret.expr->kind == ND_IDENT &&
                g_cf.nlive > 0 &&
                g_cf.live[g_cf.nlive - 1].kind == CL_VAR) {
                Node *top_var = g_cf.live[g_cf.nlive - 1].var_decl;
                Token *top_name = top_var ? top_var->var_decl.name : NULL;
                Token *ret_name = n->ret.expr->ident.name;
                if (top_name && ret_name &&
                    top_name->len == ret_name->len &&
                    memcmp(top_name->loc, ret_name->loc, top_name->len) == 0) {
                    moved = true;
                }
            }

            int target = moved
                ? find_return_target_from(g_cf.nlive - 1)
                : return_target();
            char buf[32];
            const char *lbl;
            if (target >= 0) {
                snprintf(buf, sizeof(buf), "__SF_cleanup_%d", target);
                lbl = buf;
            } else {
                lbl = "__SF_epilogue";
            }
            if (n->ret.expr) {
                fputs("__SF_RETURN(", stdout);
                emit_return_expr(n->ret.expr);
                fprintf(stdout, ", %s);\n", lbl);
            } else {
                fprintf(stdout, "__SF_RETURN_VOID(%s);\n", lbl);
            }
        } else {
            fputs("return", stdout);
            if (n->ret.expr) {
                fputc(' ', stdout);
                emit_return_expr(n->ret.expr);
            }
            fputs(";\n", stdout);
        }
        return;
    case ND_EXPR_STMT:
        /* Throw-expression at statement position lowers via the
         * __SF_THROW_PRIM macro: stash payload + type-id, set
         * __SF_unwind = __SF_UNWIND_THROW, goto innermost cleanup
         * (mirrors how ND_RETURN lowers via __SF_RETURN). N4659
         * §18 [except] + docs/exceptions.md. ISO C: no statement
         * expressions, so we only handle throw at this position
         * — throws inside ternary / call args / etc. still fall
         * through to the emit_expr placeholder for now.
         *
         * Slice 3 supports only int operands; class-type throws
         * land later. Bare 'throw;' (rethrow) sets state and
         * jumps without re-stashing — the existing slot still
         * carries the in-flight exception that the surrounding
         * catch is handling. */
        if (n->expr_stmt.expr && n->expr_stmt.expr->kind == ND_THROW) {
            Node *thr = n->expr_stmt.expr;
            bool in_try = false;
            int target = find_throw_target_from(g_cf.nlive, &in_try);
            char buf[32];
            const char *lbl;
            if (target >= 0) {
                /* CL_TRY's label is the catch-dispatch entry; CL_VAR's
                 * is the dtor cleanup. Prefix matches the kind so the
                 * goto resolves to the right emitted label. */
                snprintf(buf, sizeof(buf),
                         in_try ? "__SF_try_%d_handler" : "__SF_cleanup_%d",
                         target);
                lbl = buf;
            } else {
                lbl = "__SF_epilogue";
            }
            if (thr->throw_.is_rethrow) {
                /* Bare 'throw;' inside a catch — re-propagate the
                 * currently-handled exception. The TLS slot still
                 * holds it (catch-clear runs only on fall-through
                 * out of the handler body). */
                fprintf(stdout,
                        "do { __sf_exc_state.state = __SF_UNWIND_THROW; "
                        "__SF_unwind = __SF_UNWIND_THROW; "
                        "goto %s; } while (0);\n",
                        lbl);
            } else {
                /* Class-type throws can't go through __SF_THROW_PRIM
                 * — the macro casts the operand to uintptr_t which is
                 * invalid for struct/union values. Route those
                 * through __SF_THROW_CLASS with the address of the
                 * operand, which works because the operand emit is
                 * either a compound literal (addressable since C99
                 * §6.5.2.5) or a hoisted named temp. Pattern:
                 * g++.dg/eh/dtor3.C 'throw excep();'. */
                Type *op_ty = thr->throw_.operand
                    ? thr->throw_.operand->resolved_type : NULL;
                bool op_is_class = op_ty &&
                    (op_ty->kind == TY_STRUCT || op_ty->kind == TY_UNION);
                if (op_is_class) {
                    /* If the operand is a polymorphic-class
                     * value-init that emits as a stmt-expr (not an
                     * lvalue), '&(stmt-expr)' is invalid. Rewrite as
                     * a stmt-expr that yields the address of the
                     * named temp. Pattern: g++.dg/init/new36.C
                     * 'throw E();' with virtual ~E() on E.
                     *
                     * Same pattern applies to non-polymorphic classes
                     * with a user-declared default ctor: the generic
                     * 'A()' emit path drops the ctor body and lowers
                     * to '(struct A){0}', which is wrong whenever the
                     * ctor has side effects. Pattern:
                     * g++.dg/eh/elide1.C 'throw A()' where A::A()
                     * itself throws — the int-throw from inside the
                     * ctor is what the outer try catches; emitting
                     * just '{0}' skips that throw entirely. */
                    Node *op = thr->throw_.operand;
                    bool is_func_cast_zero =
                        op && op->kind == ND_CALL && op->call.nargs == 0 &&
                        op->call.callee &&
                        op->call.callee->kind == ND_IDENT;
                    bool needs_ctor_call = op_ty &&
                        (op_ty->has_virtual_methods ||
                         op_ty->has_default_ctor);
                    if (is_func_cast_zero && needs_ctor_call) {
                        fputs("__SF_THROW_CLASS(({ ", stdout);
                        emit_type(op_ty);
                        fputs(" __sf_thr = {0}; ", stdout);
                        mangle_class_ctor(op_ty, NULL, 0);
                        fputs("(&__sf_thr); ", stdout);
                        /* The ctor body may itself throw; if so the
                         * outer __SF_THROW_CLASS must NOT clobber the
                         * just-set exc_state. The CHAIN check skips
                         * out via the same label the ctor used. */
                        fprintf(stdout, "__SF_CHAIN_THROW(%s); ", lbl);
                        fputs("&__sf_thr; })", stdout);
                        fprintf(stdout, ", 0, %s);\n", lbl);
                    } else {
                        fputs("__SF_THROW_CLASS(&(", stdout);
                        emit_expr(thr->throw_.operand);
                        fprintf(stdout, "), 0, %s);\n", lbl);
                    }
                } else {
                    fputs("__SF_THROW_PRIM(", stdout);
                    emit_expr(thr->throw_.operand);
                    fprintf(stdout, ", &__sf_typeinfo_int, %s);\n", lbl);
                }
            }
            return;
        }
        if (n->expr_stmt.expr)
            emit_expr(n->expr_stmt.expr);
        fputs(";\n", stdout);
        /* Phase 2 slice 5: cross-function throw propagation. After
         * any potentially-throwing statement, check the unwind
         * state and chain to the innermost handler / cleanup label.
         * Conservatively emitted whenever the function has the
         * unwind machinery — most expr-stmts contain a call which
         * could itself throw, and even a non-call statement is a
         * cheap branch to skip. The noexcept-inference phase
         * (phase 3+ per docs/exceptions.md) trims this back to
         * call sites whose callee is provably-throwing. */
        if (g_cf.func_has_cleanups) {
            bool in_try = false;
            int target = find_throw_target_from(g_cf.nlive, &in_try);
            char buf[32];
            const char *lbl;
            if (target >= 0) {
                snprintf(buf, sizeof(buf),
                         in_try ? "__SF_try_%d_handler" : "__SF_cleanup_%d",
                         target);
                lbl = buf;
            } else {
                lbl = "__SF_epilogue";
            }
            emit_indent();
            fprintf(stdout, "__SF_CHAIN_THROW(%s);\n", lbl);
        }
        return;
    case ND_NULL_STMT:
        fputs(";\n", stdout);
        return;
    case ND_CLASS_DEF:
        /* Anonymous block-scope class-def — 'static union { int i; };'
         * in the source. C11 6.7.2.1/13 makes anonymous struct/union
         * members visible in the enclosing struct/union scope, but
         * NOT at function or file scope; gcc-as-C rejects bare
         * 'union { int i; };' without an instance name.
         *
         * Lower to a normal union declaration with a synthesised
         * instance name '__sf_anon_inst_<id>', then rewrite bare
         * references to anon-union members within the enclosing block
         * to '__sf_anon_inst_<id>.member' (see g_anon_union_stack in
         * emit_ident and the block-scan in emit_stmt's ND_BLOCK case).
         * Storage class is carried from the decl-specifier seq via
         * class_def.storage_flags so 'static' survives the lowering. */
        if (!n->class_def.tag && n->class_def.ty &&
            (n->class_def.ty->kind == TY_STRUCT ||
             n->class_def.ty->kind == TY_UNION)) {
            if (n->class_def.ty->anon_id == 0)
                n->class_def.ty->anon_id = ++g_anon_counter;
            emit_indent();
            emit_var_storage_flags(n->class_def.storage_flags);
            fputs(n->class_def.ty->kind == TY_UNION ? "union " : "struct ",
                  stdout);
            emit_open_brace();
            g_indent++;
            for (int mi = 0; mi < n->class_def.nmembers; mi++) {
                Node *m = n->class_def.members[mi];
                if (!m || m->kind != ND_VAR_DECL) continue;
                emit_indent();
                emit_var_decl_inner(m);
                fputs(";\n", stdout);
            }
            g_indent--;
            emit_indent();
            fprintf(stdout, "} __sf_anon_inst_%d;\n",
                    n->class_def.ty->anon_id);
            return;
        }
        /* Block-scope struct — gcc 4.8 calls.c emit_library_call_value_1
         * defines 'struct arg' inside the function body. In C this is a
         * valid block-scope declaration, but the two-phase emit only
         * walks top-level decls for PHASE_STRUCTS, so the body was never
         * emitted and downstream uses hit 'incomplete type'. Flip phase
         * to 0 (single-pass) so emit_class_def writes the full body
         * in-place, same as the inline-struct recovery on var_decl below. */
        {
            int saved_phase = g_emit_phase;
            g_emit_phase = 0;
            emit_class_def(n);
            g_emit_phase = saved_phase;
        }
        return;
    case ND_TYPEDEF: {
        /* Block-scope typedef — e.g. 'typedef enum { A, B } T;' inside
         * a function body. C accepts block-scope typedefs.  Emit the
         * enum body if present, then 'typedef enum X <name>;'. Pattern:
         * gcc 4.8 omega.c omega_pretty_print_problem's local
         * 'typedef enum { none, le, lt } partial_order_type;'. */
        Type *uty = n->var_decl.ty;
        Type *enum_ty = uty;
        while (enum_ty && ty_is_indirect(enum_ty) && enum_ty->base)
            enum_ty = enum_ty->base;
        if (enum_ty && enum_ty->kind == TY_ENUM &&
            enum_ty->enum_tokens && enum_ty->enum_ntokens > 0 &&
            !enum_body_already_emitted(enum_ty->enum_tokens)) {
            mark_enum_body_emitted(enum_ty->enum_tokens);
            enum_ty->codegen_emitted = true;
            fputs("enum ", stdout);
            if (enum_ty->tag)
                fprintf(stdout, "%.*s ", enum_ty->tag->len, enum_ty->tag->loc);
            fputs("{ ", stdout);
            emit_enum_body(enum_ty);
            fputs(" };\n", stdout);
            emit_indent();
        }
        /* Block-scope typedef itself is generally unneeded for
         * codegen — sea-front emits types by their canonical form,
         * not by typedef aliases — but gcc uses the typedef name in
         * casts ('(partial_order_type)x'). Emit the typedef so those
         * casts remain valid. */
        if (n->var_decl.name) {
            fputs("typedef ", stdout);
            emit_var_decl_inner(n);
            fputs(";\n", stdout);
        }
        return;
    }
    case ND_VAR_DECL:
        rewrite_copy_init_ctor_call(n);
        /* Block-scope inline-struct dependency, same shape as the
         * top-level emit path: 'static const struct T { ... } arr[];'
         * in a function body hangs T's body off var_decl.ty's class_def
         * with no separate ND_CLASS_DEF. Emit the struct body in-place
         * so the var-decl that references it has a complete type.
         * Pattern: gcc 4.8 builtins.c expand_builtin_nonlocal_goto
         *   static const struct elims {const int from, to;} elim_regs[] = ...; */
        {
            Type *dep = n->var_decl.ty;
            while (dep && (dep->kind == TY_ARRAY || dep->kind == TY_PTR)
                   && dep->base) dep = dep->base;
            if (dep && (dep->kind == TY_STRUCT || dep->kind == TY_UNION) &&
                dep->class_def && !dep->codegen_emitted) {
                int saved_phase = g_emit_phase;
                g_emit_phase = 0;
                emit_class_def(dep->class_def);
                g_emit_phase = saved_phase;
                emit_indent();
            }
        }
        /* Block-scope storage-class qualifiers (static, register, etc.)
         * are meaningful at function-body scope too — N4659 §10.1.1
         * [dcl.stc]. Without this, 'static int counter = 0;' inside a
         * function emits as a plain auto local; gcc 4.8 read-rtl.c
         * relied on a function-local 'static rtx queue_head;' for
         * once-only initialization, and dropping the static turned
         * each call into an uninitialized auto whose stack-garbage
         * value bypassed the init guard, leaving codes.iterators NULL
         * → segfault. */
        {
            /* `static const T name = expr;` at block scope is legal in
             * C++ even when `expr` isn't a constant expression — N1570
             * §6.7.9/4 disallows that in C. For a const value, dropping
             * `static` is observably equivalent: the value is the same
             * each call, only its storage location changes. */
            int sf = n->var_decl.storage_flags;
            bool is_static = (sf & DECL_STATIC) != 0;
            bool is_const_ty = n->var_decl.ty && n->var_decl.ty->is_const;
            Node *init = n->var_decl.init;
            bool init_nonconst = init &&
                (init->kind == ND_IDENT || init->kind == ND_MEMBER ||
                 init->kind == ND_CALL  || init->kind == ND_SUBSCRIPT);
            if (is_static && is_const_ty && init_nonconst)
                sf &= ~DECL_STATIC;
            emit_var_storage_flags(sf);
        }
        emit_var_decl_inner(n);
        /* Value-init 'T x{}' — empty brace-init — N4659 §11.6.1/8
         * [dcl.init]: zero-initialize the object before calling the
         * default ctor. The ctor call follows below; this just adds
         * the zero-fill initializer to the declaration. */
        if (n->var_decl.has_ctor_init && n->var_decl.ctor_nargs == 0 &&
            n->var_decl.ty && n->var_decl.ty->kind == TY_STRUCT &&
            n->var_decl.name) {
            fputs(" = {0}", stdout);
        }
        fputs(";\n", stdout);
        /* Direct-init 'T x(args)' lowers to a ctor call right
         * after the declaration. The class type's tag determines
         * the mangled name (mangle_class_ctor → sf__T__ctor under
         * the human scheme); first arg is &name, the rest are the
         * user args. */
        if (n->var_decl.has_ctor_init && n->var_decl.ty &&
            n->var_decl.ty->kind == TY_STRUCT && n->var_decl.name) {
            /* N4659 §15.1/5 [class.copy.ctor]: copy construction.
             * 'Foo x(other)' where 'other' is the same struct type
             * (or deref of this) → emit as C struct assignment instead
             * of a ctor call. The implicit copy ctor is always available
             * in C++03 and is equivalent to memberwise copy. */
            bool is_copy = false;
            if (n->var_decl.ctor_nargs == 1) {
                Node *arg0 = n->var_decl.ctor_args[0];
                Type *arg_ty = arg0 ? arg0->resolved_type : NULL;
                /* *this: TY_STRUCT matching the class */
                if (arg_ty && arg_ty->kind == TY_STRUCT && n->var_decl.ty->tag &&
                    arg_ty->tag && arg_ty->tag->len == n->var_decl.ty->tag->len &&
                    memcmp(arg_ty->tag->loc, n->var_decl.ty->tag->loc,
                           arg_ty->tag->len) == 0)
                    is_copy = true;
                /* const Foo& arg: TY_REF(TY_STRUCT) */
                if (ty_is_ref(arg_ty) &&
                    arg_ty->base && arg_ty->base->kind == TY_STRUCT &&
                    n->var_decl.ty->tag && arg_ty->base->tag &&
                    arg_ty->base->tag->len == n->var_decl.ty->tag->len &&
                    memcmp(arg_ty->base->tag->loc, n->var_decl.ty->tag->loc,
                           arg_ty->base->tag->len) == 0)
                    is_copy = true;
            }
            if (is_copy) {
                emit_indent();
                fprintf(stdout, "%.*s = ",
                        n->var_decl.name->len, n->var_decl.name->loc);
                emit_expr(n->var_decl.ctor_args[0]);
                fputs(";\n", stdout);
            } else {
                emit_indent();
                {
                    Type **at = NULL;
                    int na = collect_call_arg_types(n->var_decl.ctor_args,
                                                     n->var_decl.ctor_nargs, &at);
                    Type **pty = NULL;
                    Node *resolved_ctor = NULL;
                    int np = resolve_overload(n->var_decl.ty, /*name=*/NULL,
                                               /*is_ctor=*/true,
                                               at, na,
                                               /*receiver_is_const=*/false,
                                               &pty, &resolved_ctor);
                    if (np < 0) {
                        /* No user-declared ctor matches. For 0-arg
                         * direct-init on a class with a SYNTHESIZED
                         * default ctor (has_default_ctor=true via
                         * vptr-install or member-ctor chain), call
                         * the synth ctor — mangle_class_ctor with
                         * NULL/0 params produces the same symbol
                         * emit_class_def synthesizes. Without this,
                         * `A a = A();` for a class with virtual
                         * methods (synth default ctor for vptr)
                         * died with no-overload. Pattern:
                         * g++.dg/init/value1.C. */
                        if (na == 0 && n->var_decl.ty->has_default_ctor) {
                            pty = NULL; np = 0;
                        } else {
                            die_no_overload(n->var_decl.ty, NULL, na,
                                             "direct-init ctor call");
                        }
                    }
                    mangle_class_ctor(n->var_decl.ty, pty, np);
                    fprintf(stdout, "(&%.*s",
                            n->var_decl.name->len, n->var_decl.name->loc);
                    for (int i = 0; i < n->var_decl.ctor_nargs; i++) {
                        fputs(", ", stdout);
                        emit_arg_for_param(n->var_decl.ctor_args[i],
                                            i < np ? pty[i] : NULL);
                    }
                    /* Default-arg expansion — N4659 §11.3.6
                     * [dcl.fct.default]. Trailing params unsupplied
                     * by the call site fill from param.default_value. */
                    emit_default_args_tail(resolved_ctor,
                                           n->var_decl.ctor_nargs, np, pty);
                    fputs(");\n", stdout);
                }
            }
        }
        /* Default-init 'Foo a;' (no init, no parens) calls the
         * class's user-declared default ctor when one exists.
         * Without a default ctor we leave the storage uninitialized
         * — same as C semantics, and matching what C++ does for
         * trivially-default-constructible types. */
        else if (!n->var_decl.init && !n->var_decl.has_ctor_init &&
                 n->var_decl.ty && n->var_decl.ty->kind == TY_STRUCT &&
                 n->var_decl.ty->has_default_ctor && n->var_decl.name) {
            emit_indent();
            /* Default ctor — 0 params. */
            mangle_class_ctor(n->var_decl.ty, NULL, 0);
            fprintf(stdout, "(&%.*s);\n",
                    n->var_decl.name->len, n->var_decl.name->loc);
        }
        return;
    case ND_IF:
        if (expr_has_class_temp(n->if_.cond)) {
            /* Slice D-cond: condition contains a class temp that
             * must be destroyed BEFORE the then/else branches run
             * (N4659 §6.7.7 [class.temporary]/4 — temps die at end
             * of full-expression, which here is the cond).
             *
             * Lowering: declare a synthetic int OUTSIDE the if,
             * compute the cond into it inside a mini-block, fire
             * temp dtors via the cleanup chain, then emit the
             * actual 'if (synth)' using the captured value.
             *
             *   int __SF_cond_N;
             *   {
             *       struct T __SF_temp_M;
             *       T_ctor(&__SF_temp_M, ...);
             *       __SF_cond_N = (__SF_temp_M.v == 7);
             *   __SF_cleanup_M: ;
             *       T_dtor(&__SF_temp_M);
             *       __SF_CHAIN_ANY(<parent>);
             *   }
             *   if (__SF_cond_N) ... else ...
             */
            int cond_id = g_cf.next_label_id++;
            char cond_name[24];
            snprintf(cond_name, sizeof(cond_name), "__SF_cond_%d", cond_id);

            /* Wrap the entire decl + mini-block + if sequence in an
             * outer block so the caller sees a single statement. This
             * matters when the if-stmt is the body of an unbraced
             * for/while/else (gcc 4.8 c-ada-spec.c collect_ada_nodes:
             *   for (n = t; n; n = TREE_CHAIN (n))
             *     if (LOCATION_LINE (...) > 0 && expand_location(...).file == s)
             * — the for-body must be one statement; without the wrap
             * we emit 'for (...) int __SF_cond_0;' and the mini-block
             * + if leak outside the for). */
            emit_open_brace();

            /* Synth decl at the current indent. Type is int — cond is
             * implicitly converted to bool/int for the test, and
             * int is always assignable from any scalar cond. */
            emit_indent();
            fprintf(stdout, "int %s;\n", cond_name);

            /* Open mini-block */
            emit_indent();
            emit_open_brace();
            int saved_nlive = g_cf.nlive;

            /* Hoist temps from the cond (inside the mini-block,
             * so their decls are emitted at the mini-block's
             * indent level). */
            hoist_temps_in_expr(n->if_.cond, /*in_shortcircuit=*/false);

            /* Assign the cond into the synthetic. */
            emit_indent();
            fprintf(stdout, "%s = ", cond_name);
            emit_expr(n->if_.cond);
            fputs(";\n", stdout);

            /* Cleanup chain — fires temp dtors before exiting
             * the mini-block. No-op if no CL_VARs were pushed
             * (trivially-destructible temp). */
            emit_cleanup_chain_for_added(saved_nlive);
            g_cf.nlive = saved_nlive;

            emit_close_brace();

            /* Now emit the actual if using the synthetic. */
            emit_indent();
            fprintf(stdout, "if (%s) ", cond_name);
            emit_if_body_with_hoist(n->if_.then_);
            if (n->if_.else_) {
                emit_indent();
                fputs("else ", stdout);
                emit_if_body_with_hoist(n->if_.else_);
            }

            emit_close_brace();
            return;
        }
        /* C++ init-declaration as condition:
         *   if (T *v = expr) { ... }
         * N4659 §9.4.1 [stmt.select]/2. Parser stores an ND_VAR_DECL
         * in if_.cond. C has no declarations inside parens, so lift
         * the declaration into an enclosing block and test the
         * variable's value:
         *   { T *v = expr; if (v) { ... } }
         * The block scopes the variable correctly — same visibility
         * as the source (available in then/else but not beyond). */
        if (n->if_.cond && n->if_.cond->kind == ND_VAR_DECL &&
            n->if_.cond->var_decl.name) {
            Token *nm = n->if_.cond->var_decl.name;
            rewrite_copy_init_ctor_call(n->if_.cond);
            emit_open_brace();
            emit_indent();
            emit_var_decl_inner(n->if_.cond);
            fputs(";\n", stdout);
            if (n->if_.cond->var_decl.has_ctor_init) {
                emit_indent();
                Type **at = NULL;
                int na = collect_call_arg_types(n->if_.cond->var_decl.ctor_args,
                                                 n->if_.cond->var_decl.ctor_nargs, &at);
                Type **pty = NULL;
                int np = resolve_overload(n->if_.cond->var_decl.ty,
                                           NULL, true, at, na, false,
                                           &pty, NULL);
                if (np < 0 && n->if_.cond->var_decl.ctor_nargs == 0) np = 0;
                mangle_class_ctor(n->if_.cond->var_decl.ty, pty, np);
                fprintf(stdout, "(&%.*s);\n", nm->len, nm->loc);
            }
            emit_indent();
            /* Build a synthetic ND_IDENT so emit_bool_context_expr
             * can decide whether to invoke `operator bool` based on
             * the declared variable's type. */
            Node ident_stub = {0};
            ident_stub.kind = ND_IDENT;
            ident_stub.ident.name = nm;
            ident_stub.resolved_type = n->if_.cond->var_decl.ty;
            int dc_saved = decl_cond_cleanup_open(n->if_.cond);
            fputs("if (", stdout);
            emit_bool_context_expr(&ident_stub);
            fputs(") ", stdout);
            emit_if_body_with_hoist(n->if_.then_);
            if (n->if_.else_) {
                emit_indent();
                fputs("else ", stdout);
                emit_if_body_with_hoist(n->if_.else_);
            }
            decl_cond_cleanup_close(dc_saved);
            g_indent--;
            emit_indent();
            fputs("}", stdout);
            return;
        }
        fputs("if (", stdout);
        emit_bool_context_expr(n->if_.cond);
        fputs(") ", stdout);
        emit_if_body_with_hoist(n->if_.then_);
        if (n->if_.else_) {
            emit_indent();
            fputs("else ", stdout);
            emit_if_body_with_hoist(n->if_.else_);
        }
        return;
    case ND_WHILE: {
        Node *body = n->while_.body;
        /* Declaration-as-condition: `while (T v = expr) body` —
         * N4659 §9.5.1/2. Lift to a wrapping block. Approximate the
         * re-init-per-iteration semantics with a single decl + plain
         * test — incorrect for loops where the body modifies v and
         * the next iteration would re-create it, but correct for
         * the common case where the cond evaluates false on entry
         * (operator bool with side-effecting ctor). Pattern:
         * g++.dg/eh/scope1.C. */
        if (n->while_.cond && n->while_.cond->kind == ND_VAR_DECL &&
            n->while_.cond->var_decl.name) {
            Token *nm = n->while_.cond->var_decl.name;
            rewrite_copy_init_ctor_call(n->while_.cond);
            emit_open_brace();
            emit_indent();
            emit_var_decl_inner(n->while_.cond);
            fputs(";\n", stdout);
            /* If rewritten to has_ctor_init, emit the ctor call. */
            if (n->while_.cond->var_decl.has_ctor_init) {
                emit_indent();
                Type **at = NULL;
                int na = collect_call_arg_types(n->while_.cond->var_decl.ctor_args,
                                                 n->while_.cond->var_decl.ctor_nargs, &at);
                Type **pty = NULL;
                int np = resolve_overload(n->while_.cond->var_decl.ty,
                                           NULL, /*is_ctor=*/true, at, na,
                                           /*receiver_is_const=*/false,
                                           &pty, NULL);
                if (np < 0 && n->while_.cond->var_decl.ctor_nargs == 0)
                    np = 0;  /* default ctor */
                mangle_class_ctor(n->while_.cond->var_decl.ty, pty, np);
                fprintf(stdout, "(&%.*s);\n", nm->len, nm->loc);
            }
            emit_indent();
            Node ident_stub = {0};
            ident_stub.kind = ND_IDENT;
            ident_stub.ident.name = nm;
            ident_stub.resolved_type = n->while_.cond->var_decl.ty;
            int dc_saved = decl_cond_cleanup_open(n->while_.cond);
            fputs("while (", stdout);
            emit_bool_context_expr(&ident_stub);
            fputs(") ", stdout);
            emit_stmt(body);
            decl_cond_cleanup_close(dc_saved);
            g_indent--;
            emit_indent();
            fputs("}\n", stdout);
            return;
        }
        bool body_wrap = g_cf.func_has_cleanups && subtree_has_cleanups(body);
        bool cond_wrap = expr_has_class_temp(n->while_.cond);
        /* See for-loop's brace_only comment — a non-block body in a
         * func_has_cleanups function needs braces so the trailing
         * __SF_CHAIN_THROW stays inside the loop. */
        bool brace_only = !body_wrap && g_cf.func_has_cleanups
                          && body && body->kind != ND_BLOCK;
        if (!body_wrap && !cond_wrap && !brace_only) {
            /* Simple natural form — no cleanups anywhere. */
            fputs("while (", stdout);
            emit_expr(n->while_.cond);
            fputs(") ", stdout);
            emit_stmt(body);
            return;
        }
        if (brace_only) {
            fputs("while (", stdout);
            emit_expr(n->while_.cond);
            fputs(") {\n", stdout);
            g_indent++;
            emit_indent();
            emit_stmt(body);
            emit_close_brace();
            return;
        }
        /* Slice C: when the body has cleanups, push a CL_LOOP marker
         * so body break/continue chains terminate at the loop's
         * synthetic boundary labels. */
        int brk = -1, cnt = -1;
        if (body_wrap) {
            brk = g_cf.next_label_id++;
            cnt = g_cf.next_label_id++;
            cleanup_live_reserve(1);
            g_cf.live[g_cf.nlive].kind = CL_LOOP;
            g_cf.live[g_cf.nlive].label_id = brk;
            g_cf.live[g_cf.nlive].cont_label_id = cnt;
            g_cf.live[g_cf.nlive].var_decl = NULL;
            g_cf.nlive++;
        }
        if (cond_wrap) {
            /* Slice D-cond: cond contains a class temp. Lower to
             *   while (1) {
             *       int __SF_cond_<n>;
             *       { hoist + assign + cleanup }
             *       if (!__SF_cond_<n>) break/goto __SF_loop_break_<m>;
             *       <body>
             *       __SF_loop_cont_<m>: ;   (only if body_wrap)
             *   }
             * The cond's temp dtors fire each iteration before the
             * test, matching C++'s end-of-full-expression timing. */
            int cond_id = g_cf.next_label_id++;
            char cond_name[24];
            snprintf(cond_name, sizeof(cond_name), "__SF_cond_%d", cond_id);
            fputs("while (1) {\n", stdout);
            g_indent++;
            emit_indent();
            fprintf(stdout, "int %s;\n", cond_name);
            emit_indent();
            emit_open_brace();
            int saved_nlive = g_cf.nlive;
            hoist_temps_in_expr(n->while_.cond, /*in_shortcircuit=*/false);
            emit_indent();
            fprintf(stdout, "%s = ", cond_name);
            emit_expr(n->while_.cond);
            fputs(";\n", stdout);
            emit_cleanup_chain_for_added(saved_nlive);
            g_cf.nlive = saved_nlive;
            emit_close_brace();
            emit_indent();
            if (body_wrap)
                fprintf(stdout, "if (!%s) goto __SF_loop_break_%d;\n",
                        cond_name, brk);
            else
                fprintf(stdout, "if (!%s) break;\n", cond_name);
            emit_indent();
            emit_stmt(body);
            if (body_wrap) {
                emit_indent();
                fprintf(stdout, "__SF_loop_cont_%d: ;\n", cnt);
            }
            emit_close_brace();
        } else {
            /* body_wrap only — natural while preserved (Slice C). */
            fputs("while (", stdout);
            emit_expr(n->while_.cond);
            fputs(") {\n", stdout);
            g_indent++;
            emit_indent();
            emit_stmt(body);
            emit_indent();
            fprintf(stdout, "__SF_loop_cont_%d: ;\n", cnt);
            emit_close_brace();
        }
        if (body_wrap) {
            g_cf.nlive--;  /* pop CL_LOOP marker */
            emit_indent();
            fprintf(stdout, "__SF_loop_break_%d: ;\n", brk);
        }
        return;
    }
    case ND_DO: {
        Node *body = n->do_.body;
        bool body_wrap = g_cf.func_has_cleanups && subtree_has_cleanups(body);
        bool cond_wrap = expr_has_class_temp(n->do_.cond);
        /* See for-loop's brace_only comment. */
        bool brace_only = !body_wrap && g_cf.func_has_cleanups
                          && body && body->kind != ND_BLOCK;
        if (!body_wrap && !cond_wrap && !brace_only) {
            fputs("do ", stdout);
            emit_stmt(body);
            emit_indent();
            fputs("while (", stdout);
            emit_expr(n->do_.cond);
            fputs(");\n", stdout);
            return;
        }
        if (brace_only) {
            fputs("do {\n", stdout);
            g_indent++;
            emit_indent();
            emit_stmt(body);
            g_indent--;
            emit_indent();
            fputs("} while (", stdout);
            emit_expr(n->do_.cond);
            fputs(");\n", stdout);
            return;
        }
        int brk = -1, cnt = -1;
        if (body_wrap) {
            brk = g_cf.next_label_id++;
            cnt = g_cf.next_label_id++;
            cleanup_live_reserve(1);
            g_cf.live[g_cf.nlive].kind = CL_LOOP;
            g_cf.live[g_cf.nlive].label_id = brk;
            g_cf.live[g_cf.nlive].cont_label_id = cnt;
            g_cf.live[g_cf.nlive].var_decl = NULL;
            g_cf.nlive++;
        }
        if (cond_wrap) {
            /* Slice D-cond for do-while: the synth must be declared
             * OUTSIDE the do body because the 'while (synth)' is
             * outside the body's brace pair. The cond eval mini-
             * block lives at the END of each body iteration.
             *
             *   int __SF_cond_<n>;
             *   do {
             *       <body>
             *       __SF_loop_cont_<m>: ;       (only if body_wrap)
             *       { hoist + assign + cleanup }
             *   } while (__SF_cond_<n>);
             *   __SF_loop_break_<m>: ;          (only if body_wrap) */
            int cond_id = g_cf.next_label_id++;
            char cond_name[24];
            snprintf(cond_name, sizeof(cond_name), "__SF_cond_%d", cond_id);
            fprintf(stdout, "int %s;\n", cond_name);
            emit_indent();
            fputs("do {\n", stdout);
            g_indent++;
            emit_indent();
            emit_stmt(body);
            if (body_wrap) {
                emit_indent();
                fprintf(stdout, "__SF_loop_cont_%d: ;\n", cnt);
            }
            emit_indent();
            emit_open_brace();
            int saved_nlive = g_cf.nlive;
            hoist_temps_in_expr(n->do_.cond, /*in_shortcircuit=*/false);
            emit_indent();
            fprintf(stdout, "%s = ", cond_name);
            emit_expr(n->do_.cond);
            fputs(";\n", stdout);
            emit_cleanup_chain_for_added(saved_nlive);
            g_cf.nlive = saved_nlive;
            emit_close_brace();
            g_indent--;
            emit_indent();
            fprintf(stdout, "} while (%s);\n", cond_name);
        } else {
            /* body_wrap only — natural do-while preserved (Slice C). */
            fputs("do {\n", stdout);
            g_indent++;
            emit_indent();
            emit_stmt(body);
            emit_indent();
            fprintf(stdout, "__SF_loop_cont_%d: ;\n", cnt);
            g_indent--;
            emit_indent();
            fputs("} while (", stdout);
            emit_expr(n->do_.cond);
            fputs(");\n", stdout);
        }
        if (body_wrap) {
            g_cf.nlive--;
            emit_indent();
            fprintf(stdout, "__SF_loop_break_%d: ;\n", brk);
        }
        return;
    }
    case ND_FOR: {
        Node *body = n->for_.body;
        /* Declaration-as-condition: `for (init; T v = expr; inc) body`
         * — N4659 §9.5.3/2. Lift to a wrapping block then while-loop
         * with the cond as a plain variable test. Approximate per-
         * iteration re-init (like while-decl-cond above). Pattern:
         * g++.dg/eh/scope1.C `for (; C br = C(); ) ...`. */
        if (n->for_.cond && n->for_.cond->kind == ND_VAR_DECL &&
            n->for_.cond->var_decl.name) {
            Token *nm = n->for_.cond->var_decl.name;
            rewrite_copy_init_ctor_call(n->for_.cond);
            emit_open_brace();
            if (n->for_.init) {
                emit_indent();
                emit_stmt(n->for_.init);
            }
            emit_indent();
            emit_var_decl_inner(n->for_.cond);
            fputs(";\n", stdout);
            if (n->for_.cond->var_decl.has_ctor_init) {
                emit_indent();
                Type **at = NULL;
                int na = collect_call_arg_types(n->for_.cond->var_decl.ctor_args,
                                                 n->for_.cond->var_decl.ctor_nargs, &at);
                Type **pty = NULL;
                int np = resolve_overload(n->for_.cond->var_decl.ty,
                                           NULL, true, at, na, false,
                                           &pty, NULL);
                if (np < 0 && n->for_.cond->var_decl.ctor_nargs == 0) np = 0;
                mangle_class_ctor(n->for_.cond->var_decl.ty, pty, np);
                fprintf(stdout, "(&%.*s);\n", nm->len, nm->loc);
            }
            emit_indent();
            Node ident_stub = {0};
            ident_stub.kind = ND_IDENT;
            ident_stub.ident.name = nm;
            ident_stub.resolved_type = n->for_.cond->var_decl.ty;
            int dc_saved = decl_cond_cleanup_open(n->for_.cond);
            fputs("while (", stdout);
            emit_bool_context_expr(&ident_stub);
            fputs(") ", stdout);
            /* Wrap body in braces with inc at the end. */
            fputs("{\n", stdout);
            g_indent++;
            if (body) { emit_indent(); emit_stmt(body); }
            if (n->for_.inc) {
                emit_indent();
                emit_expr(n->for_.inc);
                fputs(";\n", stdout);
            }
            emit_close_brace();
            decl_cond_cleanup_close(dc_saved);
            g_indent--;
            emit_indent();
            fputs("}\n", stdout);
            return;
        }
        bool body_wrap = g_cf.func_has_cleanups && subtree_has_cleanups(body);
        bool cond_wrap = n->for_.cond && expr_has_class_temp(n->for_.cond);
        /* If func_has_cleanups, a non-block body emits a __SF_CHAIN_THROW
         * statement after the body proper. That second statement must
         * live INSIDE the for-loop body; without braces the chain-throw
         * lands at the for-loop's enclosing scope and never fires per
         * iteration. Pattern: g++.dg/eh/loop1.C — `for (...) bar(ptr);`
         * where bar throws, catch outside. */
        bool brace_only = !body_wrap && g_cf.func_has_cleanups
                          && body && body->kind != ND_BLOCK;
        if (!body_wrap && !cond_wrap && !brace_only) {
            fputs("for (", stdout);
            if (n->for_.init) {
                Node *init = n->for_.init;
                if (init->kind == ND_VAR_DECL) {
                    emit_var_decl_inner(init);
                } else if (init->kind == ND_EXPR_STMT) {
                    emit_expr(init->expr_stmt.expr);
                } else if (init->kind == ND_BLOCK) {
                    /* Multi-declarator for-init:
                     *   for (T *a = ..., **b = ...; ...; ...)
                     * The parser packs comma-separated declarators
                     * into ND_BLOCK. C supports multiple declarators
                     * with the SAME base type but different declarator
                     * suffixes; emit them as a comma-separated list.
                     * Pattern: gcc 4.8 valtrack.c
                     *   for (struct dead_debug_use *head = debug->head,
                     *        **headp = &debug->head; head; head = *headp) */
                    bool first = true;
                    for (int i = 0; i < init->block.nstmts; i++) {
                        Node *s = init->block.stmts[i];
                        if (!s || s->kind != ND_VAR_DECL) continue;
                        if (!first) fputs(", ", stdout);
                        if (first) {
                            emit_var_decl_inner(s);
                        } else {
                            /* For comma-separated declarators C only
                             * accepts the new declarator (no base type
                             * repetition). emit_var_decl_inner emits
                             * the full type — we'd duplicate the base.
                             * For simple ptr/array suffixes, emit just
                             * the declarator portion: '*name = init'
                             * with the right number of '*'s. */
                            Type *ty = s->var_decl.ty;
                            int stars = 0;
                            while (ty && ty->kind == TY_PTR && ty->base) {
                                stars++; ty = ty->base;
                            }
                            for (int j = 0; j < stars; j++) fputc('*', stdout);
                            if (s->var_decl.name)
                                fprintf(stdout, "%.*s",
                                        s->var_decl.name->len,
                                        s->var_decl.name->loc);
                            if (s->var_decl.init) {
                                fputs(" = ", stdout);
                                emit_expr(s->var_decl.init);
                            }
                        }
                        first = false;
                    }
                }
            }
            fputs("; ", stdout);
            if (n->for_.cond) emit_expr(n->for_.cond);
            fputs("; ", stdout);
            if (n->for_.inc)  emit_expr(n->for_.inc);
            fputs(") ", stdout);
            emit_stmt(body);
            return;
        }
        if (brace_only) {
            fputs("for (", stdout);
            if (n->for_.init) {
                Node *init = n->for_.init;
                if (init->kind == ND_VAR_DECL) emit_var_decl_inner(init);
                else if (init->kind == ND_EXPR_STMT)
                    emit_expr(init->expr_stmt.expr);
            }
            fputs("; ", stdout);
            if (n->for_.cond) emit_expr(n->for_.cond);
            fputs("; ", stdout);
            if (n->for_.inc)  emit_expr(n->for_.inc);
            fputs(") {\n", stdout);
            g_indent++;
            emit_indent();
            emit_stmt(body);
            emit_close_brace();
            return;
        }
        int brk = -1, cnt = -1;
        if (body_wrap) {
            brk = g_cf.next_label_id++;
            cnt = g_cf.next_label_id++;
            cleanup_live_reserve(1);
            g_cf.live[g_cf.nlive].kind = CL_LOOP;
            g_cf.live[g_cf.nlive].label_id = brk;
            g_cf.live[g_cf.nlive].cont_label_id = cnt;
            g_cf.live[g_cf.nlive].var_decl = NULL;
            g_cf.nlive++;
        }
        if (cond_wrap) {
            /* Slice D-cond for for-loop: lower to a while(1) form
             * because the for-loop's cond slot can't hold the
             * mini-block. Init runs once in a wrapping block;
             * inc runs at the bottom of each iteration; cond runs
             * via mini-block at the top of each iteration.
             *
             *   {
             *       init;
             *       while (1) {
             *           int __SF_cond_<n>;
             *           { hoist + assign + cleanup }
             *           if (!__SF_cond_<n>) break/goto loop_break;
             *           <body>
             *           __SF_loop_cont_<m>: ;
             *           inc;
             *       }
             *       __SF_loop_break_<m>: ;
             *   }
             *
             * Init/inc temps are NOT handled here; if they have
             * class temps the lowering will be slightly wrong
             * (extended-lifetime). Documented limitation. */
            int cond_id = g_cf.next_label_id++;
            char cond_name[24];
            snprintf(cond_name, sizeof(cond_name), "__SF_cond_%d", cond_id);
            emit_open_brace();
            if (n->for_.init) {
                emit_indent();
                Node *init = n->for_.init;
                if (init->kind == ND_VAR_DECL) {
                    emit_var_decl_inner(init);
                    fputs(";\n", stdout);
                } else if (init->kind == ND_EXPR_STMT) {
                    emit_expr(init->expr_stmt.expr);
                    fputs(";\n", stdout);
                }
            }
            emit_indent();
            fputs("while (1) {\n", stdout);
            g_indent++;
            emit_indent();
            fprintf(stdout, "int %s;\n", cond_name);
            emit_indent();
            emit_open_brace();
            int saved_nlive = g_cf.nlive;
            hoist_temps_in_expr(n->for_.cond, /*in_shortcircuit=*/false);
            emit_indent();
            fprintf(stdout, "%s = ", cond_name);
            emit_expr(n->for_.cond);
            fputs(";\n", stdout);
            emit_cleanup_chain_for_added(saved_nlive);
            g_cf.nlive = saved_nlive;
            emit_close_brace();
            emit_indent();
            if (body_wrap)
                fprintf(stdout, "if (!%s) goto __SF_loop_break_%d;\n",
                        cond_name, brk);
            else
                fprintf(stdout, "if (!%s) break;\n", cond_name);
            emit_indent();
            emit_stmt(body);
            if (body_wrap) {
                emit_indent();
                fprintf(stdout, "__SF_loop_cont_%d: ;\n", cnt);
            }
            if (n->for_.inc) {
                emit_indent();
                emit_expr(n->for_.inc);
                fputs(";\n", stdout);
            }
            emit_close_brace();
            if (body_wrap) {
                emit_indent();
                fprintf(stdout, "__SF_loop_break_%d: ;\n", brk);
            }
            emit_close_brace();
            if (body_wrap) g_cf.nlive--;
            return;
        }
        /* body_wrap only — natural for preserved (Slice C). */
        fputs("for (", stdout);
        if (n->for_.init) {
            Node *init = n->for_.init;
            if (init->kind == ND_VAR_DECL) {
                emit_var_decl_inner(init);
            } else if (init->kind == ND_EXPR_STMT) {
                emit_expr(init->expr_stmt.expr);
            }
        }
        fputs("; ", stdout);
        if (n->for_.cond) emit_expr(n->for_.cond);
        fputs("; ", stdout);
        if (n->for_.inc)  emit_expr(n->for_.inc);
        fputs(") {\n", stdout);
        g_indent++;
        emit_indent();
        emit_stmt(body);
        emit_indent();
        fprintf(stdout, "__SF_loop_cont_%d: ;\n", cnt);
        emit_close_brace();
        g_cf.nlive--;
        emit_indent();
        fprintf(stdout, "__SF_loop_break_%d: ;\n", brk);
        return;
    }
    case ND_RANGE_FOR: {
        /* C++11 range-based for — N4659 §9.5.4 [stmt.ranged].
         * Desugar to a C-style loop over begin()/end() (containers)
         * or pointer/array bounds (built-in arrays). The decl variable
         * is freshly bound per iteration to '*__sf_it'.
         *
         * Two shapes supported:
         *   1. Range expr is TY_ARRAY[N] — iterate as 'T* p, *e=p+N'.
         *   2. Range expr is TY_STRUCT/UNION with begin()/end() methods
         *      — call them via mangled symbols and assume the iterator
         *      type is whatever begin() returns.
         *
         * Limitations: range expression is emitted twice (begin + end
         * call). For ident / member shapes that's harmless; for a
         * side-effecting call, we'd need to bind to a local first.
         * Doesn't yet support range-decl with deduced 'auto&' (use
         * the deduced type from sema). */
        Node *range = n->range_for.range;
        Node *decl  = n->range_for.decl;
        Node *body  = n->range_for.body;
        Type *range_ty = range ? range->resolved_type : NULL;
        if (range_ty && (range_ty->kind == TY_REF ||
                          range_ty->kind == TY_RVALREF))
            range_ty = range_ty->base;
        Token *decl_name = decl ? decl->var_decl.name : NULL;
        Type *decl_ty = decl ? decl->var_decl.ty : NULL;
        if (!decl_name || !decl_ty || !range_ty) {
            fputs("/* sf: unsupported range-for (missing decl/range type) */ {}\n",
                  stdout);
            return;
        }
        if (range_ty->kind == TY_ARRAY) {
            fputs("{\n", stdout); g_indent++;
            emit_indent(); emit_type(range_ty->base);
            fputs(" *__sf_end = (", stdout); emit_expr(range);
            if (range_ty->array_len > 0) {
                fprintf(stdout, ") + %d;\n", range_ty->array_len);
            } else {
                /* Array length not preserved on the parsed type
                 * (deduced-from-initializer case 'int a[] = {1,2,3};').
                 * Fall back to sizeof / sizeof(elem) at the use site —
                 * the C compiler still knows the size of the array
                 * variable from its declaration. N4659 §9.5.4
                 * [stmt.ranged]/1: range-for on an array uses the
                 * array bounds. */
                fputs(") + sizeof(", stdout); emit_expr(range);
                fputs(") / sizeof(*(", stdout); emit_expr(range);
                fputs("));\n", stdout);
            }
            emit_indent(); fputs("for (", stdout); emit_type(range_ty->base);
            fputs(" *__sf_it = (", stdout); emit_expr(range);
            fputs("); __sf_it != __sf_end; ++__sf_it) {\n", stdout);
            g_indent++; emit_indent(); emit_type(decl_ty);
            fprintf(stdout, " %.*s = *__sf_it;\n",
                    decl_name->len, decl_name->loc);
            emit_indent(); emit_stmt(body);
            g_indent--; emit_indent(); fputs("}\n", stdout);
            g_indent--; emit_indent(); fputs("}\n", stdout);
            return;
        }
        if (range_ty->kind == TY_STRUCT || range_ty->kind == TY_UNION) {
            Type *iter_ty = class_method_return_type(range_ty, "begin", 5);
            if (iter_ty) {
                /* The range-for desugar relies on `begin()` and
                 * `end()` member functions taking no parameters and
                 * returning the iterator type — N4659 §9.5.4
                 * [stmt.ranged]. Route through mangle_class_method so
                 * the mangling stays consistent under whichever
                 * scheme is active. */
                static char tok_begin_text[] = "begin";
                static char tok_end_text[]   = "end";
                static Token tok_begin = {
                    .kind = TK_IDENT, .loc = tok_begin_text, .len = 5
                };
                static Token tok_end = {
                    .kind = TK_IDENT, .loc = tok_end_text, .len = 3
                };
                fputs("{\n", stdout); g_indent++;
                emit_indent(); emit_type(iter_ty); fputs(" __sf_end = ", stdout);
                mangle_class_method(range_ty, &tok_end, NULL, 0,
                                        /*is_const=*/false);
                fputs("(&(", stdout);
                emit_expr(range); fputs("));\n", stdout);
                emit_indent(); fputs("for (", stdout); emit_type(iter_ty);
                fputs(" __sf_it = ", stdout);
                mangle_class_method(range_ty, &tok_begin, NULL, 0,
                                        /*is_const=*/false);
                fputs("(&(", stdout);
                emit_expr(range); fputs(")); __sf_it != __sf_end; ++__sf_it) {\n",
                                          stdout);
                g_indent++; emit_indent(); emit_type(decl_ty);
                fprintf(stdout, " %.*s = *__sf_it;\n",
                        decl_name->len, decl_name->loc);
                emit_indent(); emit_stmt(body);
                g_indent--; emit_indent(); fputs("}\n", stdout);
                g_indent--; emit_indent(); fputs("}\n", stdout);
                return;
            }
        }
        fputs("/* sf: unsupported range-for (no begin/end method) */ {}\n",
              stdout);
        return;
    }
    case ND_GOTO: {
        /* User goto. When CL_VARs are live, fire their dtors INLINE
         * (in reverse declaration order) before the actual jump.
         * The dtors are duplicated relative to the cleanup chain
         * but on a different control path — the chain's labels are
         * reached via fall-through which doesn't happen here.
         *
         * Conservative approximation: fire ALL live CL_VARs. This
         * is correct when the label is at function scope (most
         * common). For labels in a same-or-nested scope (rare and
         * C++ forbids gotos that cross initialized declarations
         * anyway), this would over-destroy. A future refinement
         * would consult sema-level label-depth tracking.
         *
         * Wrapped in '{ }' so an unbraced 'if (cond) goto X;'
         * stays correct — without the braces the if would only
         * guard the first dtor and the goto would run
         * unconditionally. Same pattern as __SF_RETURN. */
        fputc('{', stdout);
        for (int i = g_cf.nlive - 1; i >= 0; i--) {
            CleanupEntry *e = &g_cf.live[i];
            if (e->kind != CL_VAR) continue;
            Node *v = e->var_decl;
            fputc(' ', stdout);
            if (v->codegen_temp_name) {
                mangle_class_dtor(v->resolved_type);
                fprintf(stdout, "(&%s);", v->codegen_temp_name);
            } else {
                mangle_class_dtor(v->var_decl.ty);
                fprintf(stdout, "(&%.*s);",
                        v->var_decl.name->len, v->var_decl.name->loc);
            }
        }
        if (n->goto_.label)
            fprintf(stdout, " goto %.*s; }\n",
                    n->goto_.label->len, n->goto_.label->loc);
        else
            fputs(" goto __SF_unknown; }\n", stdout);
        return;
    }
    case ND_LABEL:
        if (n->label.label)
            fprintf(stdout, "%.*s: ",
                    n->label.label->len, n->label.label->loc);
        if (n->label.stmt)
            emit_stmt(n->label.stmt);
        else
            fputs(";\n", stdout);
        return;
    case ND_SWITCH:
        /* switch — N4659 §9.4.2 [stmt.switch]. Emit
         *   switch (expr) body
         * The C++17 init-statement is not lowered yet (would emit as
         * a preceding statement in a braced block). break/continue
         * inside are handled by the ND_BREAK/ND_CONTINUE cases —
         * 'break' in a switch exits the switch, not a loop.
         *
         * Declaration-as-condition: `switch (T v = expr) body` —
         * §9.4.2/2. Lift to a wrapping block then switch on v.
         * Pattern: g++.dg/eh/scope1.C `switch (C br = C()) ...`. */
        if (n->switch_.expr && n->switch_.expr->kind == ND_VAR_DECL &&
            n->switch_.expr->var_decl.name) {
            Token *nm = n->switch_.expr->var_decl.name;
            rewrite_copy_init_ctor_call(n->switch_.expr);
            emit_open_brace();
            emit_indent();
            emit_var_decl_inner(n->switch_.expr);
            fputs(";\n", stdout);
            if (n->switch_.expr->var_decl.has_ctor_init) {
                emit_indent();
                Type **at = NULL;
                int na = collect_call_arg_types(n->switch_.expr->var_decl.ctor_args,
                                                 n->switch_.expr->var_decl.ctor_nargs, &at);
                Type **pty = NULL;
                int np = resolve_overload(n->switch_.expr->var_decl.ty,
                                           NULL, true, at, na, false,
                                           &pty, NULL);
                if (np < 0 && n->switch_.expr->var_decl.ctor_nargs == 0) np = 0;
                mangle_class_ctor(n->switch_.expr->var_decl.ty, pty, np);
                fprintf(stdout, "(&%.*s);\n", nm->len, nm->loc);
            }
            emit_indent();
            Node ident_stub = {0};
            ident_stub.kind = ND_IDENT;
            ident_stub.ident.name = nm;
            ident_stub.resolved_type = n->switch_.expr->var_decl.ty;
            int dc_saved = decl_cond_cleanup_open(n->switch_.expr);
            fputs("switch (", stdout);
            emit_bool_context_expr(&ident_stub);
            fputs(") ", stdout);
            if (n->switch_.body) emit_stmt(n->switch_.body);
            else fputs(";\n", stdout);
            decl_cond_cleanup_close(dc_saved);
            g_indent--;
            emit_indent();
            fputs("}\n", stdout);
            return;
        }
        fputs("switch (", stdout);
        if (n->switch_.expr)
            emit_expr(n->switch_.expr);
        fputs(") ", stdout);
        if (n->switch_.body)
            emit_stmt(n->switch_.body);
        else
            fputs(";\n", stdout);
        return;
    case ND_CASE:
        /* case — N4659 §9.1 [stmt.label]/4. Wrap the case body in a
         * brace block and hoist temps INSIDE the block so the
         * initializer runs after the case label is jumped to. See
         * the ND_CASE comment in hoist_stmt_temps for the rationale. */
        fputs("case ", stdout);
        if (n->case_.expr)
            emit_expr(n->case_.expr);
        fputs(": {\n", stdout);
        g_indent++;
        if (n->case_.stmt) {
            hoist_stmt_temps(n->case_.stmt);
            emit_indent();
            emit_stmt(n->case_.stmt);
        }
        g_indent--;
        emit_indent();
        fputs("}\n", stdout);
        return;
    case ND_DEFAULT:
        fputs("default: {\n", stdout);
        g_indent++;
        if (n->default_.stmt) {
            hoist_stmt_temps(n->default_.stmt);
            emit_indent();
            emit_stmt(n->default_.stmt);
        }
        g_indent--;
        emit_indent();
        fputs("}\n", stdout);
        return;
    case ND_BREAK:
        if (break_needs_cleanup()) {
            /* Slice C: walk the cleanup chain before exiting the
             * loop. Topmost live entry is a CL_VAR — its label is
             * the next chain target; the chain itself terminates
             * at __SF_loop_break_<n> for the innermost CL_LOOP. */
            int target = break_target();
            if (target < 0) {
                /* CL_VAR on the stack but no enclosing CL_LOOP. The
                 * `break` is in a switch (no CL_LOOP push for plain
                 * switch) but we have a class-typed condition var
                 * pushing CL_VAR — its dtor runs after the C-level
                 * switch via the decl-cond cleanup chain emit. The
                 * break itself stays plain C. Pattern:
                 * g++.dg/eh/scope1.C f4. */
                fputs("break;\n", stdout);
            } else {
                char buf[32];
                snprintf(buf, sizeof(buf), "__SF_cleanup_%d", target);
                fprintf(stdout, "__SF_BREAK(%s);\n", buf);
            }
        } else {
            fputs("break;\n", stdout);
        }
        return;
    case ND_CONTINUE:
        if (break_needs_cleanup()) {
            /* Same shape as break — at this point top-of-stack must
             * be a CL_VAR (otherwise break_needs_cleanup() would be
             * false), so cont's first chain target is that var's
             * cleanup label. */
            int target = cont_target();
            char buf[32];
            snprintf(buf, sizeof(buf), "__SF_cleanup_%d", target);
            fprintf(stdout, "__SF_CONT(%s);\n", buf);
        } else {
            fputs("continue;\n", stdout);
        }
        return;
    case ND_TRY: {
        /* try-block landing pad — N4659 §18 [except], lowering per
         * docs/exceptions.md. Slice 4: handler dispatch by primitive
         * type-id pointer compare and catch-all. Class-type catches
         * with inheritance walking land in slice 5 / phase 5 (RTTI).
         *
         * Emit shape (id is unique per try-block):
         *   { try-body
         *     goto __SF_try_<id>_after;       // body completed normally
         *   __SF_try_<id>_handler:            // body THREW (jumped here
         *                                     // from __SF_THROW_PRIM)
         *     if (exc_type == &typeinfo_T1) { bind T1, clear state, body }
         *     else if (catch_all)            { clear state, body }
         *     else { goto outer_throw; }     // re-propagate
         *   __SF_try_<id>_after: ; }
         *
         * Cleanup-chain integration with CL_VAR locals inside the
         * try-body is deferred — slice 4 only handles bare
         * try-blocks without dtor-bearing locals. */
        int id = g_cf.next_label_id++;
        fputs("{\n", stdout);
        g_indent++;

        /* Push CL_TRY so a throw inside the body finds this handler
         * label via find_throw_target_from. */
        cleanup_live_reserve(1);
        g_cf.live[g_cf.nlive].kind = CL_TRY;
        g_cf.live[g_cf.nlive].label_id = id;
        g_cf.live[g_cf.nlive].cont_label_id = 0;
        g_cf.live[g_cf.nlive].var_decl = NULL;
        g_cf.nlive++;
        if (n->try_.body) emit_stmt(n->try_.body);
        if (g_cf.nlive > 0 && g_cf.live[g_cf.nlive - 1].kind == CL_TRY)
            g_cf.nlive--;

        /* Body completed without throw — skip handlers. */
        emit_indent();
        fprintf(stdout, "goto __SF_try_%d_after;\n", id);

        /* Handler-dispatch entry. */
        fprintf(stdout, "__SF_try_%d_handler:\n", id);
        for (int i = 0; i < n->try_.nhandlers; i++) {
            Node *h = n->try_.handlers[i];
            emit_indent();
            if (h->handler.is_catch_all) {
                fputs("if (1) {\n", stdout);
            } else {
                /* Slice 4: only int (and primitives keyed to the same
                 * single typeinfo) match. Real per-type matching ships
                 * with class-type throws. */
                fputs("if (__sf_exc_state.exc_type == &__sf_typeinfo_int) {\n",
                      stdout);
            }
            g_indent++;
            /* Bind the caught name (if any) to the in-flight payload.
             * For primitives the payload was stashed via uintptr_t
             * cast in __SF_THROW_PRIM; reverse the cast here. */
            if (!h->handler.is_catch_all && h->handler.param &&
                h->handler.param->param.name) {
                Token *nm = h->handler.param->param.name;
                emit_indent();
                emit_type(h->handler.param->param.ty);
                fprintf(stdout, " %.*s = ", nm->len, nm->loc);
                fputs("(", stdout);
                emit_type(h->handler.param->param.ty);
                fputs(")(uintptr_t)__sf_exc_state.exc_obj;\n", stdout);
            }
            /* Mark exception caught — clear BOTH the function-local
             * __SF_unwind and the TLS __sf_exc_state.state so
             * surrounding chain emissions don't re-propagate and a
             * subsequent call doesn't see stale THROW state. */
            emit_indent();
            fputs("__SF_unwind = __SF_UNWIND_NONE;\n", stdout);
            emit_indent();
            fputs("__sf_exc_state.state = __SF_UNWIND_NONE;\n", stdout);
            /* Handler body. */
            if (h->handler.body) emit_stmt(h->handler.body);
            emit_indent();
            fprintf(stdout, "goto __SF_try_%d_after;\n", id);
            g_indent--;
            emit_indent();
            fputs("}\n", stdout);
        }
        /* No handler matched — re-propagate by chaining outward. */
        {
            int outer = throw_target();
            char buf[32];
            const char *lbl;
            if (outer >= 0) {
                bool outer_in_try = false;
                /* Walk again with the same logic to determine prefix. */
                find_throw_target_from(g_cf.nlive, &outer_in_try);
                snprintf(buf, sizeof(buf),
                         outer_in_try ? "__SF_try_%d_handler"
                                      : "__SF_cleanup_%d",
                         outer);
                lbl = buf;
            } else {
                lbl = "__SF_epilogue";
            }
            emit_indent();
            fprintf(stdout, "goto %s;\n", lbl);
        }
        fprintf(stdout, "__SF_try_%d_after: ;\n", id);
        g_indent--;
        emit_indent();
        fputs("}\n", stdout);
        return;
    }
    default:
        fputs("/* stmt */;\n", stdout);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Top-level emission                                                 */
/* ------------------------------------------------------------------ */

/* Reset the per-function dtor lowering state and decide whether the
 * function needs the cleanup machinery at all. Called at the entry
 * to every function/method emission. */
static void cf_begin_function(Node *func) {
    g_cf.next_label_id = 0;
    g_cf.nlive = 0;
    g_cf.func_has_cleanups = func && func->func.body &&
                             subtree_has_cleanups(func->func.body);
    /* A no-throw spec (`noexcept` / `throw()`) needs the prologue/
     * epilogue scaffolding regardless of whether the body's body has
     * cleanups — codegen plants the terminate-check inside the
     * epilogue, so it would never fire if the function emitted as
     * a bare braceless block. N4659 §18.4 [except.spec]. */
    if (func && func->kind == ND_FUNC_DEF && func->func.is_nothrow)
        g_cf.func_has_cleanups = true;
    /* Latch the source-name for __func__ rewrites in ND_IDENT. For
     * a destructor the source name token is the class name (no
     * leading '~'), so we prepend it at emit time. */
    g_current_func_src_name = func ? func->func.name : NULL;
    g_current_func_is_dtor  = func ? func->func.is_destructor : false;
    /* Populate ref-param table for deref insertion at use sites. */
    g_nref_params = 0;
    if (func && (func->kind == ND_FUNC_DEF || func->kind == ND_FUNC_DECL)) {
        int np = func->func.nparams;
        if (np > g_ref_params_cap) {
            Token **n = realloc(g_ref_params, np * sizeof(Token *));
            if (!n) abort();
            g_ref_params = n;
            g_ref_params_cap = np;
        }
        for (int i = 0; i < np; i++) {
            Node *p = func->func.params[i];
            if (p && ty_is_ref(p->param.ty) && p->param.name)
                g_ref_params[g_nref_params++] = p->param.name;
        }
    }
}

/* When emit_class_def is iterating its method members it sets this
 * to the class def node — ctor/dtor body emission can then consult
 * it to walk the class's members in declaration order (which the
 * hash-bucketed class_region doesn't preserve). NULL outside the
 * class member loop. (Declaration is at the top of the file —
 * forward-needed by ND_IDENT and ND_CALL emission for inherited-
 * member access rewriting.) */

/* For a ctor with mem-initializers and/or class members needing
 * default construction, emit member ctor calls at the top of the
 * body — N4659 §15.6.2/13.3, in DECLARATION order. Members listed
 * in the mem-init-list use the user's args; class members not
 * listed but with non-trivial default ctors get the default ctor
 * call; non-class members listed get a plain assignment. */
static void emit_ctor_mem_init_one(Node *func, Node *m);

static void emit_ctor_member_inits(Node *func) {
    if (!func->func.is_constructor) return;
    if (!g_current_class_def) return;
    Node *cdef = g_current_class_def;
    /* Base subobject construction — N4659 §15.6.2/13 [class.base.init]:
     * "...non-static data members are initialized in the order
     *  they were declared in the class definition (again regardless
     *  of the order of the mem-initializers)" — and bases come first.
     * For each direct base, look up an explicit mem-init entry by tag
     * and emit the matching ctor with the user's args; fall through
     * to a default-ctor call when there is no explicit entry. */
    if (cdef->class_def.ty) {
        Type *cty = cdef->class_def.ty;
        int nb = class_nbases(cty);
        for (int b = 0; b < nb; b++) {
            Type *base = class_base(cty, b);
            if (!base) continue;
            /* Check the mem-init-list for an entry naming this base
             * (by tag — N4659 §15.6.2 [class.base.init] allows
             * 'Derived(args) : Base(b_args), member(m_args) {}'). */
            MemInit *base_mi = NULL;
            if (base->tag) {
                for (int k = 0; k < func->func.n_mem_inits; k++) {
                    MemInit *mi = &func->func.mem_inits[k];
                    if (mi->name && mi->name->len == base->tag->len &&
                        memcmp(mi->name->loc, base->tag->loc,
                               base->tag->len) == 0) {
                        base_mi = mi;
                        break;
                    }
                }
            }
            if (base_mi) {
                /* Explicit 'Base(args)' — resolve the overload against
                 * the user's args and emit the call. */
                Type **at = NULL;
                int na = collect_call_arg_types(base_mi->args,
                                                 base_mi->nargs, &at);
                Type **pty = NULL;
                Node *best = NULL;
                int np = resolve_overload(base, /*name=*/NULL,
                                           /*is_ctor=*/true, at, na,
                                           /*receiver_is_const=*/false,
                                           &pty, &best);
                if (np < 0) {
                    /* 1-arg base mem-init with arg-type == base type:
                     * implicit copy ctor — see the matching member
                     * mem-init fall-back's comment for the rationale.
                     * Pattern: g++.dg/init/empty1.C —
                     *   class E1 : public E0 {}; class P : ..., E1
                     *   P(...) : ..., E1(E1()) {}  */
                    bool implicit_copy = false;
                    if (na == 1 && at && at[0]) {
                        Type *src = at[0];
                        if (src->kind == TY_REF || src->kind == TY_RVALREF)
                            src = src->base;
                        if (src && src->kind == TY_STRUCT &&
                            src->tag && base->tag &&
                            tokens_equal(src->tag, base->tag))
                            implicit_copy = true;
                    }
                    if (implicit_copy) {
                        emit_indent();
                        fputs("this->", stdout);
                        if (b == 0) fputs("__sf_base", stdout);
                        else        fprintf(stdout, "__sf_base%d", b);
                        fputs(" = ", stdout);
                        emit_expr(base_mi->args[0]);
                        fputs(";\n", stdout);
                        continue;
                    }
                    if (na != 0)
                        die_no_overload(base, NULL, na,
                                         "base mem-init ctor call");
                    /* na == 0 and no resolvable default ctor: this is
                     * value-initialization of the base — N4659
                     * §11.6/8.3 [dcl.init], which for a POD-shaped
                     * subobject zero-initializes the bytes. Emit a
                     * memset so the base members start at 0 instead of
                     * indeterminate stack contents (the bug behind
                     * g++.dg/init/value7.C). */
                    if (!base->has_default_ctor) {
                        emit_indent();
                        fputs("__builtin_memset(&this->", stdout);
                        if (b == 0) fputs("__sf_base", stdout);
                        else        fprintf(stdout, "__sf_base%d", b);
                        fputs(", 0, sizeof(struct ", stdout);
                        mangle_class_tag(base);
                        fputs("));\n", stdout);
                        continue;
                    }
                    /* Otherwise fall through to default-ctor path. */
                } else {
                    /* Default-arg injection — N4659 §11.3.6
                     * [dcl.fct.default]: when fewer user args than the
                     * matching ctor's params, fill the tail from the
                     * resolved ctor's param_defaults. Without this, the
                     * mangled name (np params) and the call (na args)
                     * disagree, leaving a too-few-arguments call. */
                    Node **defaults = NULL;
                    if (best && best->kind == ND_VAR_DECL && best->var_decl.ty &&
                        best->var_decl.ty->kind == TY_FUNC)
                        defaults = best->var_decl.ty->param_defaults;
                    else if (best && best->kind == ND_FUNC_DEF) {
                        /* Per-param defaults are stored on the
                         * ND_PARAM children's default_value when the
                         * ctor was parsed as a definition. Build a
                         * temporary array. */
                        static Node *def_buf[16];
                        int nd = best->func.nparams < 16 ? best->func.nparams : 0;
                        for (int p = 0; p < nd; p++) {
                            Node *par = best->func.params[p];
                            def_buf[p] = (par && par->kind == ND_PARAM)
                                ? par->param.default_value : NULL;
                        }
                        defaults = def_buf;
                    }
                    emit_indent();
                    mangle_class_ctor(base, pty, np);
                    fputs("(&this->", stdout);
                    if (b == 0) fputs("__sf_base", stdout);
                    else        fprintf(stdout, "__sf_base%d", b);
                    for (int a = 0; a < np; a++) {
                        fputs(", ", stdout);
                        Node *arg = (a < base_mi->nargs)
                            ? base_mi->args[a]
                            : (defaults ? defaults[a] : NULL);
                        if (arg)
                            emit_arg_for_param(arg, a < np ? pty[a] : NULL);
                        else
                            fputs("0", stdout);
                    }
                    fputs(");\n", stdout);
                    continue;
                }
            }
            /* No explicit mem-init for this base — default-init iff
             * the base has a default ctor we can resolve. */
            if (!base->has_default_ctor) continue;
            emit_indent();
            mangle_class_ctor(base, NULL, 0);
            fputs("(&this->", stdout);
            if (b == 0) fputs("__sf_base", stdout);
            else        fprintf(stdout, "__sf_base%d", b);
            fputs(");\n", stdout);
        }
    }
    /* Vptr install — N4659 §15.6.2/1 [class.base.init]: a polymorphic
     * class's virtual functions become callable through the object
     * once construction is *complete*. We approximate by setting the
     * vptr at the *start* of the ctor (after base ctors so it
     * overrides any vptr the base installed, before member inits
     * and the user body).
     *
     * Skip for extern-only polymorphic classes — see
     * docs/mangling.md "System-class interop". When all virtuals
     * live in a system library (libstdc++.so for std::exception),
     * the vtable instance is never emitted, so installing a vptr
     * pointing at it would reference an undeclared symbol. */
    if (cdef->class_def.ty && cdef->class_def.ty->has_virtual_methods) {
        bool any_body = false;
        for (int i = 0; i < cdef->class_def.nmembers; i++) {
            Node *m = cdef->class_def.members[i];
            if (m && m->kind == ND_FUNC_DEF && m->func.is_virtual) {
                any_body = true; break;
            }
        }
        /* Also pick up OOL virtual definitions — same shape as the
         * vtable struct emit. An in-class 'virtual void foo();'
         * (ND_VAR_DECL) paired with an out-of-class body
         * (ND_FUNC_DEF in g_tu->tu.decls) is the gcc 4.8
         * g++.dg/ipa/pr46287 pattern; without this the vtable is
         * never installed and dispatch lands on uninitialised
         * memory. */
        if (!any_body && g_tu) {
            Type *cty = cdef->class_def.ty;
            for (int i = 0; i < cdef->class_def.nmembers && !any_body; i++) {
                Node *m = cdef->class_def.members[i];
                if (!m || m->kind != ND_VAR_DECL) continue;
                if (!m->var_decl.ty || m->var_decl.ty->kind != TY_FUNC) continue;
                if (!m->var_decl.is_virtual) continue;
                Token *vn = m->var_decl.name;
                if (!vn) continue;
                for (int j = 0; j < g_tu->tu.ndecls; j++) {
                    Node *d = g_tu->tu.decls[j];
                    if (!d || d->kind != ND_FUNC_DEF) continue;
                    if (d->func.class_type != cty) continue;
                    if (!d->func.name) continue;
                    if (d->func.name->len == vn->len &&
                        memcmp(d->func.name->loc, vn->loc, vn->len) == 0) {
                        any_body = true; break;
                    }
                }
            }
        }
        if (any_body) {
            emit_indent();
            emit_vptr_lhs(cdef->class_def.ty);
            fputs(" = &", stdout);
            mangle_class_vtable_instance(cdef->class_def.ty);
            fputs(";\n", stdout);
            /* Secondary-vtable install for each non-first polymorphic
             * base. The B-subobject's own ctor already wrote B's
             * primary vtable into its vptr; overwrite with Derived's
             * thunk-bearing secondary vtable so dispatch through B*
             * reaches Derived's overrides. N4659 §13.3 [class.virtual]
             * + the secondary-vtable emit above. */
            Type *cty = cdef->class_def.ty;
            int nb = class_nbases(cty);
            for (int bi = 1; bi < nb; bi++) {
                Type *base = class_base(cty, bi);
                if (!base || !base->has_virtual_methods) continue;
                /* Vptr lives in base's polymorphic-root ancestor. */
                Type *base_root = vptr_owner_class(base);
                if (!base_root) base_root = base;
                /* Address-of the base subobject, cast to the vptr-
                 * owner pointer-type, then write through ->__sf_vptr.
                 * The cast wins us const-vtable-pointer compatibility
                 * (the secondary vtable's struct type differs from
                 * the vptr field's declared type — both compile to
                 * the same pointer-to-struct of fn-ptrs). */
                emit_indent();
                fputs("((struct ", stdout);
                mangle_class_tag(base_root);
                fprintf(stdout, " *)&this->__sf_base%d)->__sf_vptr = ", bi);
                fputs("(const struct ", stdout);
                mangle_class_vtable_type(base_root);
                fputs(" *)&", stdout);
                mangle_class_tag(cty);
                fputs("__vtable_for_", stdout);
                mangle_class_tag(base);
                fputs("_instance;\n", stdout);
            }
        }
    }
    for (int i = 0; i < cdef->class_def.nmembers; i++) {
        Node *m = cdef->class_def.members[i];
        if (!m) continue;
        /* Flat blocks from comma-separated declarations:
         * 'int a, b, c;' parses as ND_BLOCK(is_flat) containing
         * individual ND_VAR_DECLs. Flatten and run the mem-init
         * lookup on each. Same unpacking emit_class_def does. */
        if (m->kind == ND_BLOCK && m->block.is_flat) {
            for (int j = 0; j < m->block.nstmts; j++)
                emit_ctor_mem_init_one(func, m->block.stmts[j]);
            continue;
        }
        emit_ctor_mem_init_one(func, m);
    }
    /* Transitive-base mem-inits — N4659 §15.6.2 [class.base.init].
     * 'Diamond(int t) : Base(t), SubA(...), SubB(...)' where Base is
     * reachable only through SubA/SubB (not a direct base). Standard
     * says the virtual-base ctor runs ONCE on the shared subobject.
     * Sea-front doesn't model the most-derived/intermediate split, so
     * each path's Base is independently initialised by the chain
     * (SubA's ctor → Base(), SubB's ctor → Base()). To match the test's
     * observable behaviour, run the user-supplied transitive-base init
     * AFTER the sub ctors as a re-initialisation on the FIRST path
     * find_base_path resolves. That path also matches sea-front's
     * member-access resolution, so a subsequent 'this->base_member'
     * read sees the user's values.
     *
     * Only fires for mem-init names that match neither a direct base
     * nor a class member — those are handled above. */
    if (cdef->class_def.ty) {
        Type *cty2 = cdef->class_def.ty;
        for (int k = 0; k < func->func.n_mem_inits; k++) {
            MemInit *mi = &func->func.mem_inits[k];
            if (!mi->name) continue;
            /* Skip direct bases — covered above. */
            bool is_direct_base = false;
            int nbd = class_nbases(cty2);
            for (int b = 0; b < nbd; b++) {
                Type *bb = class_base(cty2, b);
                if (bb && bb->tag &&
                    bb->tag->len == mi->name->len &&
                    memcmp(bb->tag->loc, mi->name->loc, mi->name->len) == 0) {
                    is_direct_base = true; break;
                }
            }
            if (is_direct_base) continue;
            /* Skip class members — covered above. */
            bool is_member = false;
            for (int i = 0; i < cdef->class_def.nmembers; i++) {
                Node *cm = cdef->class_def.members[i];
                if (cm && cm->kind == ND_VAR_DECL && cm->var_decl.name &&
                    cm->var_decl.name->len == mi->name->len &&
                    memcmp(cm->var_decl.name->loc, mi->name->loc,
                           mi->name->len) == 0) {
                    is_member = true; break;
                }
            }
            if (is_member) continue;
            /* Find the transitive base by tag-name. Walk each direct
             * base's chain; first hit wins (matches sea-front's
             * unqualified-name resolution order). resume_at[d] holds
             * the next child index to try at depth d, so backtracking
             * doesn't re-descend into already-explored subtrees. */
            Type *target = NULL;
            int path[8];
            int path_len = 0;
            for (int b = 0; b < nbd && !target; b++) {
                Type *bb = class_base(cty2, b);
                if (!bb || !bb->tag) continue;
                if (bb->tag->len == mi->name->len &&
                    memcmp(bb->tag->loc, mi->name->loc, mi->name->len) == 0) {
                    path[0] = b;
                    path_len = 1;
                    target = bb;
                    break;
                }
                Type *stack[16];
                int   resume_at[16];
                int   parent_idx[16];
                int   slen = 0;
                stack[slen] = bb;
                resume_at[slen] = 0;
                parent_idx[slen] = b;
                slen++;
                while (slen > 0 && !target) {
                    Type *cur = stack[slen-1];
                    int nb = class_nbases(cur);
                    int c = resume_at[slen-1];
                    if (c >= nb) { slen--; continue; }
                    /* Advance past this index for the next iteration
                     * (whether we match, descend, or skip). */
                    resume_at[slen-1] = c + 1;
                    Type *child = class_base(cur, c);
                    if (!child) continue;
                    if (child->tag &&
                        child->tag->len == mi->name->len &&
                        memcmp(child->tag->loc, mi->name->loc,
                               mi->name->len) == 0) {
                        path[0] = parent_idx[0];
                        for (int j = 1; j < slen; j++) path[j] = parent_idx[j];
                        path[slen] = c;
                        path_len = slen + 1;
                        target = child;
                        break;
                    }
                    if (slen < 16 && class_nbases(child) > 0) {
                        stack[slen] = child;
                        resume_at[slen] = 0;
                        parent_idx[slen] = c;
                        slen++;
                    }
                }
            }
            if (!target) continue;
            /* Resolve ctor overload + emit the call on the path. */
            Type **at = NULL;
            int na = collect_call_arg_types(mi->args, mi->nargs, &at);
            Type **pty = NULL;
            Node *best = NULL;
            int np = resolve_overload(target, /*name=*/NULL,
                                       /*is_ctor=*/true, at, na,
                                       /*receiver_is_const=*/false,
                                       &pty, &best);
            if (np < 0) {
                if (na == 0 && target->has_default_ctor) np = 0;
                else continue;
            }
            emit_indent();
            mangle_class_ctor(target, pty, np);
            fputs("(&this->", stdout);
            for (int j = 0; j < path_len; j++) {
                int bi = path[j];
                if (j > 0) fputc('.', stdout);
                if (bi == 0) fputs("__sf_base", stdout);
                else         fprintf(stdout, "__sf_base%d", bi);
            }
            for (int a = 0; a < na; a++) {
                fputs(", ", stdout);
                emit_arg_for_param(mi->args[a], a < np ? pty[a] : NULL);
            }
            fputs(");\n", stdout);
        }
    }
}

/* Emit a single member's mem-init assignment or ctor call. Factored
 * out so the flat-block unpacking above can reuse it without
 * duplicating the whole body. */
static void emit_ctor_mem_init_one(Node *func, Node *m) {
    if (!m || m->kind != ND_VAR_DECL) return;
    Type *mty = m->var_decl.ty;
    if (!mty) return;
    if (!m->var_decl.name) return;
    /* Skip member functions. */
    if (mty->kind == TY_FUNC) return;
    {

        /* Look up this member in the user's mem-init-list. */
        MemInit *found = NULL;
        for (int k = 0; k < func->func.n_mem_inits; k++) {
            MemInit *mi = &func->func.mem_inits[k];
            if (mi->name && mi->name->len == m->var_decl.name->len &&
                memcmp(mi->name->loc, m->var_decl.name->loc,
                       mi->name->len) == 0) {
                found = mi;
                break;
            }
        }

        if (mty->kind == TY_STRUCT) {
            /* Class member: ctor call with the user's args, or
             * default ctor if not listed. */
            if (found) {
                Type **at = NULL;
                int na = collect_call_arg_types(found->args, found->nargs, &at);
                Type **pty = NULL;
                int np = resolve_overload(mty, /*name=*/NULL,
                                           /*is_ctor=*/true, at, na,
                                           /*receiver_is_const=*/false,
                                           &pty, /*out_best=*/NULL);
                if (np < 0) {
                    /* No matching ctor. For a 0-arg mem-init (`: m()`)
                     * on a plain C struct whose has_default_ctor is
                     * transitively true, this is trivial value-init —
                     * the enclosing allocation's zero-fill covers it.
                     *
                     * For a 1-arg mem-init where the arg's type is the
                     * member's type, this is an implicit copy-init
                     * (N4659 §15.8 [class.copy]/8): a struct with no
                     * user-declared copy ctor has an implicit one that
                     * performs memberwise copy. C's struct assignment
                     * is bitwise, which matches the implicit version
                     * for plain-old-data classes (no user-defined
                     * subobject copy ctors). Pattern: g++.dg/opt/const1.C
                     *   template<class T> struct C { C(const T &t) : c(t) {} ... };
                     *   C<B>(b)  // B has no user ctor — implicit copy. */
                    bool implicit_copy = false;
                    if (na == 1 && at && at[0]) {
                        Type *src = at[0];
                        if (src->kind == TY_REF || src->kind == TY_RVALREF)
                            src = src->base;
                        if (src && src->kind == TY_STRUCT &&
                            src->tag && mty->tag &&
                            tokens_equal(src->tag, mty->tag))
                            implicit_copy = true;
                    }
                    if (implicit_copy) {
                        emit_indent();
                        fprintf(stdout, "this->%.*s = ",
                                m->var_decl.name->len,
                                m->var_decl.name->loc);
                        emit_expr(found->args[0]);
                        fputs(";\n", stdout);
                    } else if (na != 0) {
                        die_no_overload(mty, NULL, na,
                                         "mem-init ctor call");
                    }
                } else {
                    emit_indent();
                    mangle_class_ctor(mty, pty, np);
                    fprintf(stdout, "(&this->%.*s",
                            m->var_decl.name->len, m->var_decl.name->loc);
                    for (int a = 0; a < found->nargs; a++) {
                        fputs(", ", stdout);
                        emit_arg_for_param(found->args[a],
                                            a < np ? pty[a] : NULL);
                    }
                    fputs(");\n", stdout);
                }
            } else if (mty->has_default_ctor) {
                /* Emit the default-ctor call only if the class
                 * actually has a 0-arg ctor we can resolve. For a
                 * plain C struct whose has_default_ctor was set
                 * transitively (e.g. union inherits from nested
                 * class), no ctor symbol exists — default init
                 * is the C trivial kind (zero-fill by the enclosing
                 * emit's {0} or nothing). N4659 §15.1/4 [class.ctor]. */
                Type **pty = NULL;
                int np = resolve_overload(mty, /*name=*/NULL,
                                           /*is_ctor=*/true,
                                           /*arg_types=*/NULL, /*nargs=*/0,
                                           /*receiver_is_const=*/false,
                                           &pty, /*out_best=*/NULL);
                if (np >= 0) {
                    emit_indent();
                    mangle_class_ctor(mty, pty, np);
                    fprintf(stdout, "(&this->%.*s);\n",
                            m->var_decl.name->len, m->var_decl.name->loc);
                }
            }
            /* else: trivially-default-constructible member, nothing to do. */
        } else {
            /* Non-class member (scalar / pointer / array): a listed
             * entry like ': x(7)' lowers to a plain assignment
             * 'this->x = 7;'. Multi-arg forms are not meaningful for
             * a scalar and would be a parse error in C++; we ignore
             * the second-and-later args. Non-listed scalar members
             * are left default-initialized (= uninitialized) per C
             * semantics. */
            /* NSDMI ('struct B { int a = 42; };' — C++11 §12.6.2/9
             * [class.base.init]): when there's no explicit mem-init
             * AND the member declaration carries an in-class
             * initializer, the synthesized ctor must apply it as
             * if 'this->member = init;'. Without this the in-body
             * '= 42' was either silently dropped or — when forwarded
             * to the struct emit — produced invalid C struct syntax.
             * Skip when the member is itself a class type (the in-class
             * init for class members needs ctor selection; deferred). */
            if (!found && m->var_decl.init && mty->kind != TY_STRUCT &&
                mty->kind != TY_UNION &&
                !(m->var_decl.storage_flags & DECL_STATIC)) {
                emit_indent();
                fprintf(stdout, "this->%.*s = ",
                        m->var_decl.name->len, m->var_decl.name->loc);
                emit_expr(m->var_decl.init);
                fputs(";\n", stdout);
            }
            if (found && found->nargs >= 1) {
                /* N4659 §15.6.2 [class.base.init]/9: a mem-initializer
                 * initialises (not assigns to) the named member, which
                 * is why C++ permits initialising a const data member
                 * via the mem-init list. Our C lowering becomes plain
                 * assignment, which cc rejects for top-level-const
                 * fields. Cast away const at the LHS — same shape as
                 * the mutable/const handling in ND_ASSIGN. */
                bool need_cast = mty->is_const;
                emit_indent();
                if (need_cast) {
                    /* Strip the top-level const from the cast type
                     * itself: (T const*) preserves outer const so
                     * deref still yields const T; we want (T*) so
                     * the assignment target is non-const. */
                    Type stripped = *mty;
                    stripped.is_const = false;
                    fputs("*(", stdout); emit_type(&stripped);
                    fprintf(stdout, "*)&this->%.*s = ",
                            m->var_decl.name->len, m->var_decl.name->loc);
                } else {
                    fprintf(stdout, "this->%.*s = ",
                            m->var_decl.name->len, m->var_decl.name->loc);
                }
                /* Reference field (lowered to T*): the RHS must be a
                 * pointer to the referent. Route through
                 * emit_arg_for_param with the ref Type so it applies
                 * the same '&(lvalue)' / pass-through-ref-param logic
                 * function-call arguments use. Without this, a class
                 * temp like 'A()' on the RHS was assigned as a struct
                 * value to a 'const A *' field — type mismatch. */
                if (ty_is_ref(mty)) {
                    emit_arg_for_param(found->args[0], mty);
                } else {
                    emit_expr(found->args[0]);
                }
                fputs(";\n", stdout);
            }
        }
    }
}

/* Emit the body of a function with cleanup-aware wrapping when the
 * function has any non-trivial locals: declare __retval/__unwind at
 * the top, run the body, then __epilogue: return __retval; */
static void emit_func_body(Node *func) {
    if (!func->func.body) { fputs(";\n", stdout); return; }
    /* Ctors in any class need the wrapped form so emit_ctor_member_inits
     * can run — that's where mem-init list AND vptr install live.
     * Non-ctors only need the wrap when they have cleanups. */
    bool has_member_inits = func->func.is_constructor &&
                            g_current_class_def != NULL;
    if (!g_cf.func_has_cleanups && !has_member_inits) {
        emit_block(func->func.body);
        return;
    }
    /* Wrap: open a brace, expand __SF_PROLOGUE (declares __SF_retval
     * and __SF_unwind), emit member-init calls if this is a ctor,
     * emit the user body (its own ND_BLOCK prints its { }), then
     * __SF_EPILOGUE (label + return). */
    emit_open_brace();
    bool void_ret = func->func.ret_ty && func->func.ret_ty->kind == TY_VOID;
    if (g_cf.func_has_cleanups) {
        emit_indent();
        if (void_ret) {
            fputs("__SF_PROLOGUE_VOID;\n", stdout);
        } else {
            fputs("__SF_PROLOGUE(", stdout);
            emit_type(func->func.ret_ty);
            fputs(");\n", stdout);
        }
    }
    if (has_member_inits) emit_ctor_member_inits(func);
    /* Skip emitting the user body block entirely when it has zero
     * statements — the wrapper's own braces are already emitted, so
     * dropping the empty inner '{ }' is purely cosmetic and keeps
     * generated ctors/dtors readable. emit_block's cleanup-chain
     * bookkeeping is a no-op for a stmt-less body anyway. */
    Node *body = func->func.body;
    bool body_empty = body && body->kind == ND_BLOCK && body->block.nstmts == 0;
    if (!body_empty) {
        emit_indent();
        emit_block(body);
    }
    if (g_cf.func_has_cleanups) {
        emit_indent();
        if (func->func.is_nothrow) {
            /* No-throw spec: an exception propagating out of this
             * function is a spec violation. N4659 §18.4 [except.spec]:
             * `noexcept` / `throw()` violations terminate. Expand
             * __SF_EPILOGUE inline so the THROW check lands AFTER the
             * label (the cleanup-chain gotos to __SF_epilogue, so the
             * check would otherwise be unreachable). Call std::terminate
             * via the Itanium-mangled libstdc++ symbol so a user's
             * std::set_terminate handler is respected. Pattern:
             * g++.dg/eh/spec10.C. */
            fputs("__SF_epilogue: ;\n", stdout);
            emit_indent();
            fputs("if (__sf_exc_state.state == __SF_UNWIND_THROW) "
                  "__sf_terminate();\n", stdout);
            emit_indent();
            fputs(void_ret ? "return;\n" : "return __SF_retval;\n", stdout);
        } else {
            fputs(void_ret ? "__SF_EPILOGUE_VOID;\n" : "__SF_EPILOGUE;\n", stdout);
        }
    }
    emit_close_brace();
}

/* ------------------------------------------------------------------ */
/* Free-function name mangling                                         */
/* ------------------------------------------------------------------ */
/* C++ allows overloaded free functions; C does not. Sea-front
 * historically left free-function names unmangled, relying on dedup
 * to skip duplicate declarations. That works for simple cases but
 * fails when both overloads must be emitted and called (e.g. gcc
 * 4.8's gt_pch_nx with both a file-scope extern 'gt_pch_nx(edge_def*)'
 * AND a block-scope extern 'gt_pch_nx(T*, op, void*)' inside a
 * template body).
 *
 * We now produce a signature-mangled C symbol for any free function
 * whose NAME has 2+ declarations at TU scope. Singleton names (the
 * vast majority — malloc, printf, user functions that aren't
 * overloaded) stay unmangled, so interop with the C library and
 * hand-written C code is preserved without needing 'extern "C"'
 * tracking on every Declaration.
 *
 * Mangle form: '<name>_p_<param_suffix>_pe_'. Reuses the existing
 * mangle_param_suffix helper so forward decls, definitions, and
 * call sites produce the same bytes. N4659 §16.5 [over.oper] on
 * overloaded names; the C-level encoding is our own. */

/* Overload detection uses the same canonical signature key that
 * the dedup tables use (func_sig_key). See the FreeFuncSig
 * comment block below for the full rationale. */
/* Overload-detection scratch table.
 *
 * Pre-pass walks the TU collecting (name, sig_key) pairs for every
 * free function decl/def. After the walk, a name is "overloaded"
 * iff the table contains 2+ DISTINCT sig keys for that name.
 *
 * The sig key uses func_sig_key (the same canonical mangler-output
 * encoding used for emit-time dedup), so the equality check is a
 * single string compare — no per-feature predicates to forget. */
typedef struct {
    Token *name;
    char   key[FUNC_SIG_KEY_MAX];
    int    key_len;
    bool   c_linkage;   /* N4659 §10.5 — extern "C": never mangle */
    bool   has_class_param; /* any param is struct/union (or ptr/ref/array thereof) */
    Type **params;      /* malloc'd; nparams entries; for value-context
                         * emit when the call's resolved_decl is NULL. */
    int    nparams;
} FreeFuncSig;

static FreeFuncSig g_ffsig_seen[16384];
static int         g_n_ffsig_seen = 0;

static void free_ovld_walk(Node *n) {
    if (!n) return;
    if (n->kind == ND_BLOCK && n->block.is_flat) {
        for (int i = 0; i < n->block.nstmts; i++)
            free_ovld_walk(n->block.stmts[i]);
        return;
    }
    Token *name = NULL;
    Type **params = NULL;
    int    nparams = 0;
    bool   c_linkage = false;
    if (n->kind == ND_FUNC_DEF || n->kind == ND_FUNC_DECL) {
        name = n->func.name;
        if (name && n->kind == ND_FUNC_DEF && n->func.class_type)
            name = NULL;  /* class methods mangle via their own path */
        nparams = n->func.nparams;
        /* Build a Type** array from the ND_PARAM children. Lives only
         * for the func_sig_key call below; the stored copy is fresh. */
        Type **tmp = nparams > 0 ? malloc(nparams * sizeof(Type *)) : NULL;
        if (nparams > 0 && !tmp) abort();
        for (int i = 0; i < nparams; i++)
            tmp[i] = n->func.params[i] ? n->func.params[i]->param.ty : NULL;
        params = tmp;
        c_linkage = (n->func.storage_flags & DECL_C_LINKAGE) != 0;
    } else if (n->kind == ND_VAR_DECL && n->var_decl.ty &&
               n->var_decl.ty->kind == TY_FUNC) {
        name = n->var_decl.name;
        nparams = n->var_decl.ty->nparams;
        params = n->var_decl.ty->params;
        c_linkage = (n->var_decl.storage_flags & DECL_C_LINKAGE) != 0;
    }
    if (!name || g_n_ffsig_seen >= 16384) {
        if (n->kind == ND_FUNC_DEF || n->kind == ND_FUNC_DECL) free(params);
        return;
    }
    FreeFuncSig *e = &g_ffsig_seen[g_n_ffsig_seen++];
    e->name = name;
    e->key_len = func_sig_key(name, params, nparams, e->key);
    e->c_linkage = c_linkage;
    e->has_class_param = false;
    e->nparams = nparams;
    e->params = nparams > 0 ? malloc(nparams * sizeof(Type *)) : NULL;
    if (nparams > 0 && !e->params) abort();
    for (int i = 0; i < e->nparams; i++) {
        e->params[i] = params ? params[i] : NULL;
        Type *t = e->params[i];
        while (t && (t->kind == TY_PTR || t->kind == TY_REF ||
                     t->kind == TY_RVALREF || t->kind == TY_ARRAY))
            t = t->base;
        if (t && (t->kind == TY_STRUCT || t->kind == TY_UNION))
            e->has_class_param = true;
    }
    /* 'params' is e->params for the ND_VAR_DECL branch (shared with
     * the Type's param list — don't free) and a fresh malloc for the
     * ND_FUNC_* branch (free after key/has_class_param computed). */
    if (n->kind == ND_FUNC_DEF || n->kind == ND_FUNC_DECL)
        free(params);
}

static void free_ovld_populate(Node *tu) {
    g_n_ffsig_seen = 0;
    if (!tu || tu->kind != ND_TRANSLATION_UNIT) return;
    for (int i = 0; i < tu->tu.ndecls; i++)
        free_ovld_walk(tu->tu.decls[i]);
}

/* True iff the name has 2+ distinct signatures recorded. Pure name
 * lookup — does NOT consider linkage. The bare/mangle decision per
 * call site lives in emit_free_func_symbol, which gates mangling
 * on the per-decl c_linkage flag (the caller's). */
static bool free_func_name_is_overloaded(Token *name) {
    if (!name) return false;
    /* 'main' is special — implementations universally treat it as
     * extern "C" linkage; crt0 calls the unmangled symbol.
     * N4659 §6.6.1 [basic.start.main]/3 leaves the linkage
     * implementation-defined, but the de-facto ABI is C linkage. */
    if (name->len == 4 && memcmp(name->loc, "main", 4) == 0) return false;
    /* Already-mangled name (template instantiation pre-bakes the full
     * '_p_..._pe_' suffix into the synthesized ident). Returning true
     * here would re-mangle, doubling the suffix. The pre-baked name
     * is the canonical symbol — emit it as-is. */
    if (name->len >= 4 &&
        memcmp(name->loc + name->len - 4, "_pe_", 4) == 0)
        return false;
    /* Two paths into "yes, mangle":
     *
     *   1. Locally-visible overload set has 2+ distinct signatures
     *      with this name (the original C++-overload signal).
     *
     *   2. ANY visible decl with this name takes a class-typed
     *      parameter (struct/union, or pointer/ref/array thereof).
     *      C++-linked functions with class parameters are the
     *      overload-prone shape; the C++ ABI mangles every such
     *      function by signature. Sea-front's per-TU view of the
     *      overload set can miss "this function is overloaded
     *      somewhere else": e.g. bitmap.c only includes bitmap.h
     *      (sees one bitmap_set_bit overload), but reginfo.c
     *      includes both bitmap.h AND sbitmap.h (sees the other
     *      overload too) — mangling per-TU made the link fail with
     *      thousands of undefined references in cc1plus. Mangling
     *      preemptively when any class-typed param shows up matches
     *      what every other TU will do for the same signature.
     *      Plain int-and-pointer-only helpers stay unmangled —
     *      preserves sea-front's existing test coverage of free
     *      functions, function pointers, and aggregate-init
     *      function-pointer struct members. N4659 §10.5 [dcl.link]
     *      / Itanium C++ ABI §5.1. */
    /* Pre-scan: if ANY visible decl with this name has c_linkage
     * AND every visible decl shares the same signature (only one
     * distinct sig key for the name), treat ALL refs as C-linkage
     * (unmangled). N4659 §10.5/6: a name with C linkage refers to
     * a single C function regardless of how many decls exist.
     * Intra-TU mixing of extern "C" and non-extern-C declarations
     * is permissive under the standard but real headers do it —
     * libcpp/internal.h declares '_cpp_lex_token' inside extern "C"
     * while lex.c defines it at file scope without a wrapper. Per-
     * decl c_linkage made the def mangle (no DECL_C_LINKAGE on it
     * locally) while every caller that only saw the extern-C decl
     * emitted bare — link failed.
     *
     * The "all decls share the same sig" guard avoids hitting the
     * dual-overload case: <stdlib.h> has 'abs(int)' as extern "C"
     * AND <bits/std_abs.h> has C++ overloads abs(long), abs(double),
     * etc. Forcing c_linkage on the whole name set would emit ALL
     * the abs decls bare and produce a 'conflicting types for abs'
     * cascade. Treat that case as overloaded → mangled. */
    int distinct_sigs = 0;
    bool any_c_linkage = false;
    const char *seen_keys[8];
    int         seen_key_lens[8];
    for (int i = 0; i < g_n_ffsig_seen; i++) {
        FreeFuncSig *e = &g_ffsig_seen[i];
        if (e->name->len != name->len) continue;
        if (memcmp(e->name->loc, name->loc, name->len) != 0) continue;
        if (e->c_linkage) any_c_linkage = true;
        bool dup = false;
        for (int k = 0; k < distinct_sigs; k++) {
            if (seen_key_lens[k] == e->key_len &&
                memcmp(seen_keys[k], e->key, e->key_len) == 0) {
                dup = true; break;
            }
        }
        if (!dup && distinct_sigs < 8) {
            seen_keys[distinct_sigs] = e->key;
            seen_key_lens[distinct_sigs] = e->key_len;
            distinct_sigs++;
        }
    }
    if (any_c_linkage && distinct_sigs <= 1) return false;
    const char *first_key = NULL;
    int first_key_len = 0;
    for (int i = 0; i < g_n_ffsig_seen; i++) {
        FreeFuncSig *e = &g_ffsig_seen[i];
        if (e->name->len != name->len) continue;
        if (memcmp(e->name->loc, name->loc, name->len) != 0) continue;
        if (e->has_class_param) return true;
        if (!first_key) {
            first_key = e->key;
            first_key_len = e->key_len;
            continue;
        }
        if (e->key_len != first_key_len ||
            memcmp(e->key, first_key, e->key_len) != 0)
            return true;
    }
    return false;
}

/* Look up a function name in g_ffsig_seen and emit through
 * emit_free_func_symbol with the recorded params. Returns true if
 * a matching entry was found and the symbol was emitted. Used by
 * the ND_IDENT value-context path when sema didn't resolve the
 * decl (file-scope aggregate initializers etc.) — lets sea-front
 * still mangle 'add_one' to match the def even though
 * resolved_decl is NULL. Pattern: gcc 4.8 gengtype.c
 *   static const struct file_rule_st files_rules[] = {
 *     ..., source_dot_c_frul,   // ND_IDENT, rd=NULL after sema
 *     ..., header_dot_h_frul,
 *   };
 * The forward decls were emitted as mangled (class-param heuristic),
 * but the init-list idents emitted bare → "undeclared" link error. */
static bool emit_free_func_ident_via_ffsig(Token *name) {
    if (!name) return false;
    for (int i = 0; i < g_n_ffsig_seen; i++) {
        FreeFuncSig *e = &g_ffsig_seen[i];
        if (e->name->len != name->len) continue;
        if (memcmp(e->name->loc, name->loc, name->len) != 0) continue;
        emit_free_func_symbol(name, NULL, e->c_linkage,
                               e->params, e->nparams);
        return true;
    }
    return false;
}

/* N4659 §10.5/6 [dcl.link]: a name with C linkage refers to a single
 * C function regardless of how many declarations exist. Real headers
 * (e.g. <cstring>) declare both 'char *strchr(char*, int)' and
 * 'const char *strchr(const char*, int)' as extern "C", which is
 * technically ill-formed but ubiquitous. Emit only the first such
 * declaration; later ones would clash on the bare C name.
 *
 * Returns true when 'this_decl' is the first extern-C entry in
 * g_ffsig_seen with this name. */
static bool ffsig_is_first_c_linkage(Token *name, Type **params, int nparams) {
    if (!name) return true;
    char this_key[FUNC_SIG_KEY_MAX];
    int  this_key_len = func_sig_key(name, params, nparams, this_key);
    bool seen_other_c = false;
    for (int i = 0; i < g_n_ffsig_seen; i++) {
        FreeFuncSig *e = &g_ffsig_seen[i];
        if (!e->c_linkage) continue;
        if (e->name->len != name->len) continue;
        if (memcmp(e->name->loc, name->loc, name->len) != 0) continue;
        /* If THIS exact decl is the entry we're at, return based on
         * whether we've already seen a different-sig extern-C decl. */
        if (e->key_len == this_key_len &&
            memcmp(e->key, this_key, this_key_len) == 0)
            return !seen_other_c;
        seen_other_c = true;
    }
    return true;
}

/* Emit '<name>_p_<param_suffix>_pe_' directly to stdout. */
static void emit_free_func_mangled_name(Token *name, Type **param_types,
                                         int nparams) {
    if (!name) return;
    fprintf(stdout, "%.*s", name->len, name->loc);
    mangle_param_suffix(param_types, nparams);
}

/* Emit the function signature part AFTER storage-class keywords —
 * handles the overload-mangling decision. */
static void emit_free_func_header(Type *ret_ty, Token *name,
                                   Node **params, int nparams,
                                   bool variadic) {
    bool mangle = free_func_name_is_overloaded(name);
    if (!mangle) {
        emit_func_header(ret_ty, name, params, nparams, variadic);
        return;
    }
    /* Build param-type array for the mangled suffix. */
    Type *ptypes[32];
    int np = nparams < 32 ? nparams : 32;
    for (int i = 0; i < np; i++)
        ptypes[i] = params[i] ? params[i]->param.ty : NULL;
    /* Function-pointer return type: C requires the name to be
     * interleaved with the pointer, not placed after a standalone
     * type. Mirror emit_func_header's handling — without this,
     * 'gimple_predicate rhs_predicate_for(tree)' emits as
     *   _Bool (*)(union tree_node*) rhs_predicate_for_p_..._pe_(...)
     * which is invalid C (cc errors "expected identifier or '(' before
     * ')' token" on the standalone function-pointer type). The
     * interleaved form
     *   _Bool (*rhs_predicate_for_p_..._pe_(...))(union tree_node *)
     * is the standard C declarator. N4659 §11.3 [dcl.meaning]
     * / C99 §6.7.5.3 [Function declarators]. */
    bool ret_is_fptr = ret_ty && ret_ty->kind == TY_PTR &&
                       ret_ty->base && ret_ty->base->kind == TY_FUNC;
    if (ret_is_fptr) {
        Type *fty = ret_ty->base;
        emit_type(fty->ret);
        fputs(" (*", stdout);
        emit_free_func_mangled_name(name, ptypes, np);
        emit_param_list(params, nparams, variadic);
        fputs(")(", stdout);
        emit_func_param_types(fty);
        fputc(')', stdout);
        return;
    }
    emit_type(ret_ty);
    fputc(' ', stdout);
    emit_free_func_mangled_name(name, ptypes, np);
    emit_param_list(params, nparams, variadic);
}

static void emit_func_def(Node *n) {
    emit_source_comment(n->tok);
    cf_begin_function(n);
    Type *saved_ret = g_current_func_ret_ty;
    Node *saved_lam = g_current_lambda_fn;
    Node *saved_cdef_for_lambda = g_current_class_def;
    g_current_func_ret_ty = n->func.ret_ty;
    if (n->func.is_lambda_fn) {
        g_current_lambda_fn = n;
        /* Lambda body with '[this]' capture: implicit-this references
         * inside the body must walk the enclosing class's base chain
         * for inherited members ('x' resolves to a C base in a
         * MI A : B, C). Set g_current_class_def to the captured
         * class's def so emit_ident's base-path walk can fire.
         * Pattern: g++.dg/cpp0x/lambda/lambda-this5.C. */
        for (int i = 0; i < n->func.ncaptures; i++) {
            Capture *c = &n->func.captures[i];
            if (c->is_this && c->resolved_type &&
                c->resolved_type->class_def) {
                g_current_class_def = c->resolved_type->class_def;
                break;
            }
        }
    }
    /* §10.1.5/1 [dcl.constexpr]: a constexpr function is implicitly
     * inline. Sea-front doesn't enforce constexpr's restrictions
     * (compile-time evaluability) — we lower as plain runtime code
     * — but the inline implication is what the multi-TU dedup needs
     * for header-defined constexpr fns. */
    int sf = n->func.storage_flags;
    if (sf & DECL_CONSTEXPR) sf |= DECL_INLINE;
    emit_storage_flags_for_def(sf);
    /* GNU __attribute__((weak)) pass-through — the linker honours it
     * so a non-weak definition in another TU strong-overrides this
     * one. Pattern: g++.dg/eh/weak1.C. */
    if (n->func.attr_weak)
        fputs("__attribute__((weak)) ", stdout);
    /* GNU __attribute__((constructor)) / ((destructor)) on a free
     * function: the loader runs ctor functions before main and dtor
     * functions at process teardown. Priority N orders within the
     * group (lower = earlier for ctors, later for dtors). Pattern:
     * g++.dg/ext/initpri-2.C. */
    if (n->func.attr_constructor) {
        if (n->func.ctor_priority > 0)
            fprintf(stdout, "__attribute__((constructor(%d))) ",
                    n->func.ctor_priority);
        else
            fputs("__attribute__((constructor)) ", stdout);
    }
    if (n->func.attr_destructor) {
        if (n->func.dtor_priority > 0)
            fprintf(stdout, "__attribute__((destructor(%d))) ",
                    n->func.dtor_priority);
        else
            fputs("__attribute__((destructor)) ", stdout);
    }
    /* Namespace-scoped free function: emit using the same Itanium-
     * style mangling the call site (ND_QUALIFIED with parts
     * [ns, fn]) uses. Otherwise the def emits bare 'ab6' while the
     * call uses '_ZN5xyzzy3ab6EdPv' and link fails. Pattern:
     * g++.dg/ext/fnname1.C. Reuses mangle_class_method with a stub
     * Type whose tag is the namespace name — Itanium's namespace
     * and class prefixes are the same `N<len>...E` shape. */
    if (n->func.ns_token && g_mangle_kind == MANGLE_ITANIUM) {
        Type ns_stub = {0};
        ns_stub.kind = TY_STRUCT;
        ns_stub.tag  = n->func.ns_token;
        static Type *param_buf[16];
        int np = n->func.nparams < 16 ? n->func.nparams : 16;
        for (int i = 0; i < np; i++) {
            Node *p = n->func.params[i];
            param_buf[i] = (p && p->kind == ND_PARAM) ? p->param.ty : NULL;
        }
        emit_type(n->func.ret_ty);
        fputc(' ', stdout);
        mangle_class_method(&ns_stub, n->func.name, param_buf, np,
                            /*is_const=*/false);
        fputc('(', stdout);
        if (n->func.nparams == 0) {
            fputs("void", stdout);
        } else {
            for (int i = 0; i < n->func.nparams; i++) {
                if (i > 0) fputs(", ", stdout);
                Node *p = n->func.params[i];
                if (p && p->kind == ND_PARAM) {
                    emit_type(p->param.ty);
                    if (p->param.name)
                        fprintf(stdout, " %.*s",
                                p->param.name->len, p->param.name->loc);
                    else
                        fprintf(stdout, " __sf_unused_%d", i);
                }
            }
        }
        fputc(')', stdout);
    } else {
        emit_free_func_header(n->func.ret_ty, n->func.name,
                              n->func.params, n->func.nparams,
                              n->func.is_variadic);
    }
    fputc(' ', stdout);
    emit_func_body(n);
    g_current_func_ret_ty = saved_ret;
    g_current_lambda_fn   = saved_lam;
    g_current_class_def   = saved_cdef_for_lambda;
}

/* Method DEF dispatcher for overloaded operators. The mangling
 * module owns the per-scheme encoding decisions; this just decodes
 * the operator-kind from the method-name token and routes to the
 * right entry point (mangle_class_operator for operator-symbol
 * methods, mangle_class_conversion for `operator T()`). */
static void emit_operator_method_name_cv(Type *class_type, Token *name,
                                          Type *ret_ty,
                                          Type **param_types, int nparams,
                                          bool is_const) {
    OperatorKind op = operator_kind_from_method_name(name);
    if (op == OP_UNKNOWN && ret_ty) {
        /* No operator symbol detected — must be a conversion
         * operator `operator T()`, where T is the function's
         * return type. */
        mangle_class_conversion(class_type, ret_ty, is_const);
    } else {
        mangle_class_operator(class_type, op,
                               param_types, nparams, is_const);
    }
}

/*
 * Is this function name an operator keyword?
 */
static bool is_operator_name(Token *name) {
    return name && name->kind == TK_KW_OPERATOR;
}

static void emit_method_signature(Node *func, Type *class_type, bool emit_inline) {
    if (!func || func->kind != ND_FUNC_DEF) return;
    if (!class_type || !class_type->tag || !func->func.name) return;

    /* In-class definitions are implicitly inline (N4659 §10.1.6/3
     * [dcl.inline]) — emit __SF_INLINE so multi-TU compilation
     * dedupes. OOL definitions are NOT implicitly inline: emit a
     * strong global so other TUs (which only see the in-class
     * declaration) can resolve the symbol at link. See
     * docs/inline_and_dedup.md. */
    if (emit_inline) fputs("__SF_INLINE ", stdout);
    emit_type(func->func.ret_ty);
    fputc(' ', stdout);
    if (func->func.is_destructor) {
        /* The user's dtor body is emitted as Class__dtor_body — the
         * '_body' suffix names what it actually contains: just the
         * user-written body, with no member-dtor chain. The
         * synthesized Class__dtor wrapper (built by emit_class_def)
         * calls Class__dtor_body first and then chains into member
         * dtor calls. Every CALLER of a class dtor still emits the
         * unsuffixed Class__dtor name — they hit the wrapper, not
         * the body function directly. */
        mangle_class_dtor_body(class_type);
    } else if (func->func.is_constructor) {
        /* Constructors mangle as Class__ctor with a param-type suffix
         * so overloads land at distinct C symbols. N4659 §16.2
         * [over.load]. */
        Type **pty = NULL;
        int np = collect_func_param_types(func, &pty);
        mangle_class_ctor(class_type, pty, np);
    } else if (is_operator_name(func->func.name)) {
        Type **pty = NULL;
        int np = collect_func_param_types(func, &pty);
        emit_operator_method_name_cv(class_type, func->func.name,
                                      func->func.ret_ty,
                                      pty, np,
                                      func->func.is_const_method);
    } else {
        Type **pty = NULL;
        int np = collect_func_param_types(func, &pty);
        mangle_class_method(class_type, func->func.name, pty, np,
                                func->func.is_const_method);
    }
    fputc('(', stdout);
    bool is_static = (func->func.storage_flags & DECL_STATIC) != 0;
    if (!is_static) {
        /* N4659 §10.1.7.1: const method → const this */
        if (func->func.is_const_method) fputs("const ", stdout);
        fputs("struct ", stdout);
        mangle_class_tag(class_type);
        fputs(" *this", stdout);
    }
    for (int i = 0; i < func->func.nparams; i++) {
        if (i > 0 || !is_static) fputs(", ", stdout);
        Node *p = func->func.params[i];
        emit_param_declarator(p->param.ty, p->param.name, i);
    }
    if (is_static && func->func.nparams == 0) fputs("void", stdout);
    fputc(')', stdout);
}

/* Emit a method definition as a free C function with a mangled name
 * and an explicit 'this' parameter.
 *
 *   struct Point { int sum() { return x + y; } };
 * becomes
 *   int Point_sum(struct Point *this) { return this->x + this->y; }
 *
 * The 'this->' rewrite happens at the ident level — visit_ident set
 * implicit_this on each member reference, and emit_expr emits the
 * prefix when it sees the flag. */
static void emit_method_as_free_fn(Node *func, Type *class_type, bool emit_inline) {
    if (!func || func->kind != ND_FUNC_DEF) return;
    if (!class_type || !class_type->tag || !func->func.name) return;

    emit_source_comment(func->tok);
    cf_begin_function(func);
    Type *saved_ret = g_current_func_ret_ty;
    Type *saved_mc = g_current_method_class;
    bool saved_mconst = g_current_method_is_const;
    bool saved_mstatic = g_current_method_is_static;
    g_current_func_ret_ty = func->func.ret_ty;
    g_current_method_class = class_type;
    g_current_method_is_const = func->func.is_const_method;
    g_current_method_is_static = (func->func.storage_flags & DECL_STATIC) != 0;
    emit_method_signature(func, class_type, emit_inline);
    fputc(' ', stdout);
    emit_func_body(func);
    g_current_method_is_const = saved_mconst;
    g_current_method_is_static = saved_mstatic;
    g_current_func_ret_ty = saved_ret;
    g_current_method_class = saved_mc;
}

static void emit_class_def(Node *n) {
    /* Emit a C struct from the parsed class definition.
     * When g_emit_phase == PHASE_STRUCTS: emit only the struct body
     *   and method forward declarations.
     * When g_emit_phase == PHASE_METHODS: emit method bodies, vtable,
     *   synthesized ctors/dtors.
     * When g_emit_phase == 0: emit everything (single-pass mode). */
    Type *class_type = n->class_def.ty;

    /* Two-phase emission support: in PHASE_STRUCTS, emit only
     * the struct definition + method forward declarations. In
     * PHASE_METHODS, skip the struct (already emitted) and emit
     * only method bodies, vtable, synthesized ctors/dtors. */
    /* Extern-only polymorphic check — used in BOTH phases (struct
     * emit gates the vptr field; methods phase gates the vtable
     * struct and instance). Compute up here so PHASE_METHODS sees
     * a defined value past the goto below. See docs/mangling.md
     * "System-class interop" for the rationale. */
    bool any_virtual_has_body = false;
    if (class_type && class_type->has_virtual_methods) {
        for (int i = 0; i < n->class_def.nmembers; i++) {
            Node *m = n->class_def.members[i];
            if (m && m->kind == ND_FUNC_DEF && m->func.is_virtual) {
                any_virtual_has_body = true;
                break;
            }
        }
        /* A derived class that inherits a virtual dtor without declaring
         * its own virtual method still needs the vtable machinery: its
         * dtor is implicitly virtual (N4659 §15.4/12) and the vtable
         * slot must be installed. Without this, dispatch through a base
         * pointer reaches the base's vtable instance and the wrong dtor
         * runs. The user-written dtor's ND_FUNC_DEF is NOT marked
         * is_virtual (the parser only sets it when the source keyword
         * appears), so the loop above misses it.
         *
         * BUT only when an in-TU dtor body exists — for an extern-only
         * polymorphic class (only declarations, no bodies; e.g.
         * std::exception included transitively from libstdc++) we must
         * leave any_virtual_has_body false so the vtable instance and
         * D1 wrapper synthesis are skipped (docs/mangling.md
         * "System-class interop"). Walk members for an ND_FUNC_DEF
         * destructor; a declaration-only dtor is ND_VAR_DECL and does
         * not count. */
        if (!any_virtual_has_body && class_has_virtual_dtor(class_type)) {
            for (int i = 0; i < n->class_def.nmembers; i++) {
                Node *m = n->class_def.members[i];
                if (m && m->kind == ND_FUNC_DEF && m->func.is_destructor) {
                    any_virtual_has_body = true;
                    break;
                }
            }
        }
        /* Out-of-class virtual definitions: 'virtual int foo(int);'
         * declared in the class body, body emitted later as a top-level
         * ND_FUNC_DEF whose class_type points back here. The OOL
         * definition site doesn't carry the 'virtual' keyword, so
         * d->func.is_virtual is false on it; match by name against the
         * in-class virtual declarations instead. Without this scan a
         * class whose virtuals are only OOL gets no vptr field, breaking
         * any in-TU dispatch (e.g. a non-virtual method calling the
         * virtual one — pattern from gcc 4.8 g++.dg/ipa/devirt-2.C). */
        if (!any_virtual_has_body && g_tu) {
            for (int i = 0; i < n->class_def.nmembers && !any_virtual_has_body; i++) {
                Node *m = n->class_def.members[i];
                if (!m || m->kind != ND_VAR_DECL) continue;
                if (!m->var_decl.ty || m->var_decl.ty->kind != TY_FUNC) continue;
                if (!m->var_decl.is_virtual) continue;
                Token *vn = m->var_decl.name;
                if (!vn) continue;
                for (int j = 0; j < g_tu->tu.ndecls; j++) {
                    Node *d = g_tu->tu.decls[j];
                    if (!d || d->kind != ND_FUNC_DEF) continue;
                    if (d->func.class_type != class_type) continue;
                    if (!d->func.name) continue;
                    if (d->func.name->len == vn->len &&
                        memcmp(d->func.name->loc, vn->loc, vn->len) == 0) {
                        any_virtual_has_body = true;
                        break;
                    }
                }
            }
        }
    }

    if (g_emit_phase == PHASE_METHODS) goto methods_phase;

    /* Skip if already emitted (dedup for dependency-driven emit). */
    if (class_type && class_type->codegen_emitted) return;
    /* Cross-instance dedup: two different Type pointers can carry
     * the same instantiated-class tag + template_args (e.g. the
     * instantiation pass produces two ND_CLASS_DEFs for the same
     * template-id from different discovery paths). Per-Type
     * codegen_emitted doesn't catch this. Dedup by comparing tag
     * and ALL template_args by Type* identity (the instantiation
     * pass shares Type* for the same concrete args). */
    /* Gate on template instantiations only: plain classes with the
     * same tag (e.g. 'struct Thing' in two namespaces) have DIFFERENT
     * identity but the tag dedup would collapse them. The duplicate
     * problem is narrow to the template-instantiation pass producing
     * two ND_CLASS_DEFs for one template-id. */
    if (class_type) {
        enum { CLS_DEDUP_CAP = 4096 };
        static Type *seen[CLS_DEDUP_CAP];
        static int nseen = 0;
        for (int i = 0; i < nseen; i++)
            if (same_template_instantiation(seen[i], class_type)) return;
        if (nseen < CLS_DEDUP_CAP)
            seen[nseen++] = class_type;
    }
    if (class_type) class_type->codegen_emitted = true;

    /* Emit struct dependencies first: any by-value struct/union
     * member whose type has a class_def must be emitted before
     * this struct. This handles the line-map.h pattern where
     * a union contains by-value struct members. Also handles
     * arrays of structs (i386.h stringop_strategy pattern).
     * For template-instantiated by-value members, the member's Type
     * may be a copy without class_def hooked (the instantiation
     * pass only patches the usage_type it was given). Fall back to
     * a TU tag+template_args lookup so we still emit the definition
     * first. Pattern: gcc 4.8 tree-data-ref.h
     *   struct rdg_vertex { vec<data_reference_p> datarefs; ... };
     * where the vec instantiation's class_def isn't on this Type copy. */
    /* Hoist nested class/struct definitions first — N4659 §12.1
     * [class.name] permits classes nested in classes. C has no
     * nested class scope, so sea-front emits each nested class at
     * TU scope under a (potentially) qualified mangled tag. They
     * must come BEFORE the enclosing class because that class may
     * reference them by-value as field types (directly or via a
     * template instantiation). Real-world hit: gcc 14 libcpp's
     * mkdeps has a nested 'struct velt' used by 'vec<velt> vpath;'.
     * Without this hoist, velt would be emitted at the first
     * function-body local-variable use (via the inline-struct
     * recovery in emit_stmt's ND_VAR_DECL case), too late for
     * mkdeps' field type to be complete. */
    for (int i = 0; i < n->class_def.nmembers; i++) {
        Node *m = n->class_def.members[i];
        if (!m || m->kind != ND_CLASS_DEF) continue;
        Type *mty = m->class_def.ty;
        if (mty && !mty->codegen_emitted)
            emit_class_def(m);
    }
    for (int i = 0; i < n->class_def.nmembers; i++) {
        Node *m = n->class_def.members[i];
        if (!m || m->kind != ND_VAR_DECL) continue;
        Type *mty = m->var_decl.ty;
        if (!mty) continue;
        while (mty->kind == TY_ARRAY && mty->base) mty = mty->base;
        if (mty->kind != TY_STRUCT && mty->kind != TY_UNION) continue;
        Node *dep_cd = mty->class_def;
        if (!dep_cd && mty->tag) {
            Node *d = find_class_def_by_tag_args(mty);
            if (!d) d = find_class_def_by_tag_only(mty);
            if (d) dep_cd = d;
        }
        if (dep_cd && dep_cd->class_def.ty &&
            !dep_cd->class_def.ty->codegen_emitted)
            emit_class_def(dep_cd);
    }
    /* Also emit base class definitions first. */
    if (class_type && class_type->class_region) {
        for (int i = 0; i < class_type->class_region->nbases; i++) {
            DeclarativeRegion *br = class_type->class_region->bases[i];
            if (br && br->owner_type && br->owner_type->class_def &&
                !br->owner_type->codegen_emitted)
                emit_class_def(br->owner_type->class_def);
        }
    }

    emit_source_comment(n->tok);

    /* For polymorphic classes (any virtual method), forward-declare
     * the vtable struct so we can place a vptr field at offset 0 of
     * the layout. The vtable struct itself is defined later, after
     * all method forward-declarations are out, so it can name them
     * in its function-pointer slots. */
    bool poly = class_type && class_type->has_virtual_methods;
    if (poly && any_virtual_has_body) {
        fputs("struct ", stdout);
        mangle_class_vtable_type(class_type);
        fputs(";\n", stdout);
    }

    /* Member-scope anonymous enums — N4659 §10.2 [dcl.enum]:
     * 'class C { enum {A, B, M_HWM}; T arr[M_HWM]; };' declares
     * A, B, M_HWM in the class scope. C has no class-scope
     * enumerators, but our struct emission references them in array
     * bounds and field initializers. Hoist the anonymous enum to TU
     * scope BEFORE the struct so the constants resolve. Real-world
     * hit: gcc 14 libcpp/internal.h's per-class M_* sentinel.
     *
     * Conditional hoist: libstdc++ <type_traits> uses many structs
     * each with 'enum { __value = N };' as a trait constant —
     * resolved by sema, never referenced in the C output, and
     * hoisting all of them would collide on '__value'. So only
     * hoist when a subsequent class member actually uses the enum
     * (array bound expression containing a non-literal identifier).
     * Named class-scope enums and 'enum class' don't take this path
     * — named enums emit their own TU-scope body via top-level
     * emit; scoped enums use the ND_QUALIFIED enum-class rewrite. */
    for (int i = 0; i < n->class_def.nmembers; i++) {
        Node *m = n->class_def.members[i];
        if (!m || m->kind != ND_VAR_DECL) continue;
        Type *mty = m->var_decl.ty;
        if (!mty || mty->kind != TY_ENUM) continue;
        if (m->var_decl.name) continue;            /* named: 'enum E e;' */
        if (mty->tag) continue;                     /* named enum tag */
        if (!mty->enum_tokens || mty->enum_ntokens <= 0) continue;
        if (enum_body_already_emitted(mty->enum_tokens)) continue;
        bool referenced = false;
        for (int j = i + 1; j < n->class_def.nmembers && !referenced; j++) {
            Node *m2 = n->class_def.members[j];
            if (!m2 || m2->kind != ND_VAR_DECL) continue;
            Type *t = m2->var_decl.ty;
            while (t && t->kind == TY_ARRAY) {
                if (t->array_size_expr) referenced = true;
                t = t->base;
            }
        }
        if (!referenced) continue;
        mark_enum_body_emitted(mty->enum_tokens);
        mty->codegen_emitted = true;
        fputs("enum { ", stdout);
        emit_enum_body(mty);
        fputs(" };\n", stdout);
    }

    fputs((class_type && class_type->kind == TY_UNION) ? "union " : "struct ",
          stdout);
    if (class_type)
        emit_mangled_class_tag(class_type);
    else if (n->class_def.tag)
        fprintf(stdout, "%.*s",
                n->class_def.tag->len, n->class_def.tag->loc);
    fputc(' ', stdout);
    emit_open_brace();
    /* Base subobjects — N4659 §11 [class.derived] / §6.7 [class.layout].
     * For non-virtual single (or multiple) inheritance we embed each
     * direct base as a struct field at the head of the layout, in
     * declaration order. The base field is named '__sf_base' (or
     * '__sf_base<N>' for the second-and-later base) and is itself a
     * 'struct sf__<Base>'. Inherited member access lowers to
     *   this->__sf_base.x
     * via the rewriting in emit_member_via_base / emit_ident.
     *
     * Placing the base FIRST in the layout (and the polymorphic vptr
     * inside the base, not duplicated in the derived) means a
     * 'Derived*' is layout-compatible with 'Base*' for casts — the
     * is-a relationship works at the C struct level too. */
    int nb = class_nbases(class_type);
    for (int b = 0; b < nb; b++) {
        Type *base = class_base(class_type, b);
        if (!base) continue;
        emit_indent();
        fputs("struct ", stdout);
        mangle_class_tag(base);
        if (b == 0) fputs(" __sf_base;\n", stdout);
        else        fprintf(stdout, " __sf_base%d;\n", b);
    }
    /* Vptr at offset 0 — N4659 §13.3 [class.virtual]. The standard
     * doesn't mandate offset 0 but every Itanium-style ABI puts it
     * there for the simple non-multi-inheritance case, and so do we.
     *
     * If the class inherits from a polymorphic base, the vptr lives
     * inside the base subobject and we DO NOT add another vptr here
     * — that would offset the base's layout and break the is-a
     * compatibility. */
    bool has_poly_base = false;
    for (int b = 0; b < nb; b++) {
        Type *base = class_base(class_type, b);
        if (base && base->has_virtual_methods) { has_poly_base = true; break; }
    }
    /* Emit the vptr field for any polymorphic class that owns the
     * vptr slot (no polymorphic base). The earlier any_virtual_has_body
     * gate was over-conservative — it skipped the field when all
     * virtuals were pure (=0) or extern-only. Pure-virtual abstract
     * classes still need the field so derived classes' secondary
     * vtable install can write through &derived->__sf_baseN->__sf_vptr.
     * The vtable struct emit further down is still gated on
     * any_virtual_has_body, but a pointer-to-incomplete struct is
     * valid C for the field itself. N4659 §13.3 [class.virtual] —
     * polymorphism is a property of the type, not the body. */
    if (poly && !has_poly_base) {
        emit_indent();
        fputs("const struct ", stdout);
        mangle_class_vtable_type(class_type);
        fputs(" *__sf_vptr;\n", stdout);
    }
    for (int i = 0; i < n->class_def.nmembers; i++) {
        Node *m = n->class_def.members[i];
        if (!m) continue;
        /* §10.1.1/4 [dcl.stc]: static data members are not part of
         * instance layout — emit at TU scope below, not here. Static
         * member functions still go through the method emission
         * loop further down. */
        if (m->kind == ND_VAR_DECL && m->var_decl.ty &&
            m->var_decl.ty->kind != TY_FUNC &&
            (m->var_decl.storage_flags & DECL_STATIC))
            continue;
        /* Flat blocks from comma-separated declarations:
         * 'size_t precision, char_precision;' → ND_BLOCK(is_flat)
         * containing individual ND_VAR_DECLs. Flatten them. */
        if (m->kind == ND_BLOCK && m->block.is_flat) {
            for (int j = 0; j < m->block.nstmts; j++) {
                Node *s = m->block.stmts[j];
                if (!s || s->kind != ND_VAR_DECL) continue;
                if (s->var_decl.ty && s->var_decl.ty->kind == TY_FUNC) continue;
                if (s->var_decl.storage_flags & DECL_STATIC) continue;
                emit_indent();
                /* Struct-body members can't carry inline initializers
                 * in C — defer NSDMI to the synthesized ctor. */
                Node *saved_init = s->var_decl.init;
                s->var_decl.init = NULL;
                emit_var_decl_inner(s);
                s->var_decl.init = saved_init;
                fputs(";\n", stdout);
            }
            continue;
        }
        /* Anonymous struct/union as a class member — C11 6.7.2.1/13
         * makes the members of such a nested unnamed struct/union
         * accessible directly on the enclosing class. Emit the body
         * tagless and instance-less so gcc applies the same rule to
         * the lowered C struct. Pattern: g++.dg/cpp0x/union3.C and
         * expr/ptrmem4.C. */
        if (m->kind == ND_CLASS_DEF && !m->class_def.tag &&
            m->class_def.ty &&
            (m->class_def.ty->kind == TY_STRUCT ||
             m->class_def.ty->kind == TY_UNION)) {
            emit_indent();
            fputs(m->class_def.ty->kind == TY_UNION ? "union " : "struct ",
                  stdout);
            emit_open_brace();
            g_indent++;
            for (int mi = 0; mi < m->class_def.nmembers; mi++) {
                Node *mm = m->class_def.members[mi];
                if (!mm || mm->kind != ND_VAR_DECL) continue;
                if (mm->var_decl.ty && mm->var_decl.ty->kind == TY_FUNC) continue;
                emit_indent();
                Node *saved_init = mm->var_decl.init;
                mm->var_decl.init = NULL;
                emit_var_decl_inner(mm);
                mm->var_decl.init = saved_init;
                fputs(";\n", stdout);
            }
            g_indent--;
            emit_indent();
            fputs("};\n", stdout);
            continue;
        }
        if (m->kind != ND_VAR_DECL) continue;
        /* Skip anonymous enums inside the struct body. In C++ they are
         * struct-scoped (e.g. 'struct A { enum { __value = 1 }; };'
         * produces A::__value), but in C anonymous enum members are
         * at namespace scope, so multiple structs that all contain
         * 'enum { __value = ... }' (libstdc++'s __is_X type traits)
         * produce 'redeclaration of enumerator __value' errors.
         *
         * These are pure C++-compile-time constants used by template
         * metaprogramming; sema has already resolved any references
         * to literal values, so the C emit doesn't need them at all.
         * Drop them. */
        if (m->var_decl.ty && m->var_decl.ty->kind == TY_ENUM &&
            !m->var_decl.name)
            continue;
        /* Member functions (ND_VAR_DECL with TY_FUNC) are forward-declared
         * separately — skip them here. Function POINTER data members
         * have type TY_PTR(TY_FUNC) after the parser's grouped-declarator
         * wrapping, so they fall through to the generic var-decl path —
         * but C syntax needs the special 'ret (*name)(params)' shape. */
        if (m->var_decl.ty && m->var_decl.ty->kind == TY_FUNC)
            continue;  /* method declaration — handled below as forward decl */
        if (m->var_decl.ty && m->var_decl.ty->kind == TY_PTR &&
            m->var_decl.ty->base && m->var_decl.ty->base->kind == TY_FUNC &&
            m->var_decl.name) {
            /* Function pointer member: emit as 'ret (*name)(params)' */
            emit_indent();
            Type *fty = m->var_decl.ty->base;
            emit_type(fty->ret);
            fprintf(stdout, " (*%.*s)(",
                    m->var_decl.name->len, m->var_decl.name->loc);
            for (int j = 0; j < fty->nparams; j++) {
                if (j > 0) fputs(", ", stdout);
                emit_type(fty->params[j]);
            }
            if (fty->nparams == 0) fputs("void", stdout);
            fputs(");\n", stdout);
            continue;
        }
        emit_indent();
        Node *saved_init = m->var_decl.init;
        m->var_decl.init = NULL;
        emit_var_decl_inner(m);
        m->var_decl.init = saved_init;
        fputs(";\n", stdout);
    }
    g_indent--;
    /* GNU __attribute__((packed)) — pass-through to the C compiler
     * which packs the struct (no padding between fields). Goes
     * between '}' and ';' on the struct definition. Pattern:
     * g++.dg/ext/packed4.C. */
    if (class_type && class_type->is_packed)
        fputs("} __attribute__((packed));\n", stdout);
    else
        fputs("};\n", stdout);

    /* Static data members — §11.4.9.2 [class.static.data]. Skipped
     * from the struct body above; emit here as TU-scope variables
     * with mangled name 'sf__<class>__<member>'. The in-class init
     * for a const/constexpr static integral data member (§11.4.9.3)
     * is the definition.
     *
     * Cross-TU sharing via __attribute__((weak)): the same class is
     * included in many TUs, each emitting this declaration. With
     * 'static' (TU-local), every TU had its own copy of the storage
     * — breaks C++'s "one storage per static data member" semantics
     * (e.g. foo_t::count incremented in TU A's ctor wouldn't be
     * visible in TU B). With weak, the linker collapses all
     * declarations into a single shared symbol. The OOL definition
     * (emitted via the ND_VAR_DECL path with class_type set) emits
     * STRONG (no weak), so when present it strong-overrides the
     * weak headers and supplies the initializer. Patterns:
     * g++.dg/special/conpr-{3,4}.C (cross-TU foo_t::count). */
    for (int i = 0; i < n->class_def.nmembers; i++) {
        Node *m = n->class_def.members[i];
        if (!m || m->kind != ND_VAR_DECL) continue;
        if (!m->var_decl.ty || m->var_decl.ty->kind == TY_FUNC) continue;
        if (!(m->var_decl.storage_flags & DECL_STATIC)) continue;
        if (!m->var_decl.name) continue;
        emit_source_comment(m->tok);
        fputs("__attribute__((weak)) ", stdout);
        /* emit_type handles 'const' based on ty->is_const — do not
         * add an inverted 'const' here. The previous logic
         * (`if (!is_const) fputs("const ")`) made every non-const
         * static data member uneditable in C: 'static int count;'
         * came out as 'static const int sf__C__count;', breaking
         * '++count' in every member function body. */
        emit_type(m->var_decl.ty);
        fputc(' ', stdout);
        mangle_class_tag(class_type);
        fputs("__", stdout);
        emit_token_text(m->var_decl.name);
        if (m->var_decl.init) {
            fputs(" = ", stdout);
            emit_expr(m->var_decl.init);
        }
        fputs(";\n", stdout);
    }

    /* Forward-declare every method first, so they can call each
     * other regardless of source order inside the class body, and so
     * out-of-class definitions ('int Foo::bar() {}') see a prior
     * declaration of the mangled name. Both in-class definitions
     * (ND_FUNC_DEF) and pure declarations (ND_VAR_DECL with TY_FUNC)
     * count.
     *
     * Dtor handling: a user-declared dtor with non-empty body lowers
     * to mangle_class_dtor_body (sf__Class__dtor_body), forward-
     * declared and emitted by the normal method path. The
     * mangle_class_dtor wrapper (sf__Class__dtor, synthesized
     * below) is forward-declared separately when
     * class_type->has_dtor is true — that flag may be set by a
     * user dtor, by a non-trivially-destructible member, OR by a
     * non-trivially-destructible direct base — so the wrapper
     * exists even when no user dtor was written. */
    for (int i = 0; i < n->class_def.nmembers; i++) {
        Node *m = n->class_def.members[i];
        if (!m) continue;
        /* Skip dtors with empty bodies — no _dtor_body emission.
         * The class wrapper handles members directly. has_dtor may
         * still be true (because of members), so the wrapper is
         * emitted separately below. */
        if (m->kind == ND_FUNC_DEF && m->func.is_destructor) {
            Node *body = m->func.body;
            bool empty = body && body->kind == ND_BLOCK &&
                         body->block.nstmts == 0;
            if (empty) continue;
            if (!class_type || !class_type->has_dtor) continue;
        }
        /* Const/non-const overloads now get distinct mangled names
         * (via _const suffix) — no dedup needed. N4659 §16.2/3. */
        if (m->kind == ND_FUNC_DEF && class_type) {
            emit_source_comment(m->tok);
            /* In-class method body — implicitly inline. */
            emit_method_signature(m, class_type, /*emit_inline=*/true);
            fputs(";\n", stdout);
        } else if (m->kind == ND_VAR_DECL && m->var_decl.ty &&
                   m->var_decl.ty->kind == TY_FUNC && m->var_decl.name &&
                   class_type) {
            /* Skip forward decl for '= default' / '= delete' on the
             * default ctor — the synthesis path below (or the absence
             * of a body for delete) handles the linkage end-to-end.
             * Emitting a non-static decl here would collide with the
             * later __SF_INLINE synthesized body.
             * N4659 §10.1.6.4 [dcl.fct.def.default] / §10.1.6 [dcl.fct.def.delete]. */
            if ((m->var_decl.is_constructor &&
                 m->var_decl.ty->nparams == 0) ||
                m->var_decl.is_destructor) {
                Node *init = m->var_decl.init;
                if (init && init->kind == ND_NULLPTR && init->tok &&
                    (init->tok->kind == TK_KW_DEFAULT ||
                     init->tok->kind == TK_KW_DELETE))
                    continue;
            }
            /* Synthesise an emit_method_signature-like header from
             * the var-decl's type. Ctor/dtor declarations route
             * through mangle_class_ctor / mangle_class_dtor_body
             * instead of the regular mangle_class_method form. */
            emit_source_comment(m->tok);
            Type *fty = m->var_decl.ty;
            if (m->var_decl.is_destructor) {
                /* In-class declaration of a dtor whose body is
                 * defined out-of-class (Foo::~Foo() { ... }). The
                 * sf__Class__dtor wrapper (synthesized below) is
                 * forward-declared separately, but we DO need a
                 * forward decl for sf__Class__dtor_body so the
                 * wrapper can call it. */
                fputs("__SF_INLINE void ", stdout);
                mangle_class_dtor_body(class_type);
                fputs(class_type->kind == TY_UNION ? "(union " : "(struct ", stdout);
                mangle_class_tag(class_type);
                fputs(" *this);\n", stdout);
                continue;
            }
            /* Forward decl for an in-class method declaration whose
             * body is OOL: match the OOL definition's linkage.
             *
             *   - User-marked 'inline' on the in-class decl, OR on
             *     the OOL definition (e.g. 'inline double_int
             *     double_int::from_shwi (...)' — the in-class decl
             *     has no 'inline' but the OOL body does), OR
             *   - Method of a template instantiation (multi-TU body
             *     duplicated across every TU using the instantiation,
             *     same multi-TU dedup model as inline)
             *           → __SF_INLINE (== static inline).
             *   - Plain non-template OOL method (e.g. vec_prefix::
             *     calculate_allocation): strong global, defined once.
             *           → no storage class on the forward decl.
             *
             * Mismatching here yields C99 §6.2.2/4 conflicts:
             * 'static inline' fwd decl + plain OOL def turns the
             * symbol file-static (unreachable cross-TU); plain fwd
             * decl + 'static inline' def is a hard error
             * ('static declaration follows non-static'). */
            bool is_template_inst =
                class_type && class_type->n_template_args > 0;
            Type **wanted_pty = NULL;
            int    wanted_np = collect_func_param_types(m, &wanted_pty);
            bool wanted_const = m->var_decl.ty && m->var_decl.ty->is_const;
            int ool_flags = find_ool_method_storage(class_type, m->var_decl.name,
                                                     wanted_pty, wanted_np,
                                                     wanted_const,
                                                     m->var_decl.name);
            bool is_inline = ((m->var_decl.storage_flags | ool_flags)
                              & DECL_INLINE) != 0;
            if (is_inline || is_template_inst)
                fputs("__SF_INLINE ", stdout);
            emit_type(fty->ret);
            fputc(' ', stdout);
            if (m->var_decl.is_constructor) {
                Type **pty = NULL;
                int np = collect_func_param_types(m, &pty);
                mangle_class_ctor(class_type, pty, np);
            } else if (is_operator_name(m->var_decl.name)) {
                Type **pty = NULL;
                int np = collect_func_param_types(m, &pty);
                bool mc = m->var_decl.ty && m->var_decl.ty->is_const;
                Type *ret_ty = m->var_decl.ty ? m->var_decl.ty->ret : NULL;
                emit_operator_method_name_cv(class_type, m->var_decl.name,
                                              ret_ty, pty, np, mc);
            } else {
                Type **pty = NULL;
                int np = collect_func_param_types(m, &pty);
                bool mc = m->var_decl.ty && m->var_decl.ty->is_const;
                mangle_class_method(class_type, m->var_decl.name,
                                        pty, np, mc);
            }
            /* Static methods: no 'this' parameter.
             * N4659 §10.1.1/6 [dcl.stc]. */
            {
                bool m_is_static = (m->var_decl.storage_flags & DECL_STATIC) != 0;
                bool m_is_const = m->var_decl.ty && m->var_decl.ty->is_const;
                fputc('(', stdout);
                if (!m_is_static) {
                    /* N4659 §10.1.7.1 [dcl.type.cv]: const method → const this */
                    if (m_is_const) fputs("const ", stdout);
                    fputs("struct ", stdout);
                    mangle_class_tag(class_type);
                    fputs(" *this", stdout);
                }
                for (int k = 0; k < fty->nparams; k++) {
                    if (k > 0 || !m_is_static) fputs(", ", stdout);
                    emit_type(fty->params[k]);
                }
                if (m_is_static && fty->nparams == 0) fputs("void", stdout);
                fputs(");\n", stdout);
            }
        }
    }

    /* Forward-declare the sf__Class__dtor wrapper when the class is
     * non-trivially-destructible. The wrapper exists whether or not a
     * user dtor was written. */
    if (class_type && class_type->has_dtor) {
        fputs("__SF_INLINE void ", stdout);
        mangle_class_dtor(class_type);
        fputs(class_type->kind == TY_UNION ? "(union " : "(struct ", stdout);
        mangle_class_tag(class_type);
        fputs(" *this);\n", stdout);
    }

    if (g_emit_phase == PHASE_STRUCTS) return;
methods_phase:;
    /* Dedup methods phase: template instantiation can leave multiple
     * ND_CLASS_DEF nodes pointing at the same logical class (the
     * struct-body dedup at line 3452 catches them via codegen_emitted,
     * but PHASE_METHODS skips that check by goto). Use a separate
     * pointer-set on ND_CLASS_DEF identity so a class's methods only
     * emit once even if instantiation produced several copies. */
    {
        enum { METHODS_EMIT_CAP = 256 };
        static Node *seen[METHODS_EMIT_CAP];
        static int nseen = 0;
        for (int i = 0; i < nseen; i++)
            if (seen[i] == n) return;
        if (nseen < METHODS_EMIT_CAP) seen[nseen++] = n;
    }
    /* Cross-instance method-phase dedup: two distinct ND_CLASS_DEF
     * nodes for the same instantiated class (different discovery
     * paths) have different Node* identity, so the pointer-set above
     * doesn't collapse them — both emit the same weakly-linked
     * methods, producing 'redefinition' errors within one TU.
     * Mirror the tag+template_args structural dedup used by the
     * struct-phase path. */
    if (class_type) {
        enum { MCLS_DEDUP_CAP = 4096 };
        static Type *seen_ty[MCLS_DEDUP_CAP];
        static int nseen_ty = 0;
        for (int i = 0; i < nseen_ty; i++)
            if (same_template_instantiation(seen_ty[i], class_type)) return;
        if (nseen_ty < MCLS_DEDUP_CAP)
            seen_ty[nseen_ty++] = class_type;
    }

    /* Re-scan for user dtor (needed for methods phase regardless
     * of which phase we entered from). */
    Node *user_dtor_m = NULL;
    bool user_dtor_m_out_of_class = false;
    if (class_type && class_type->has_dtor) {
        for (int i = 0; i < n->class_def.nmembers; i++) {
            Node *m = n->class_def.members[i];
            if (!m) continue;
            if (m->kind == ND_FUNC_DEF && m->func.is_destructor) {
                Node *body = m->func.body;
                bool empty = body && body->kind == ND_BLOCK &&
                             body->block.nstmts == 0;
                if (!empty) user_dtor_m = m;
                break;
            }
            if (m->kind == ND_VAR_DECL && m->var_decl.ty &&
                m->var_decl.ty->kind == TY_FUNC && m->var_decl.is_destructor) {
                /* '~A() = default;' (or '~A() = delete;') has no body,
                 * so the dtor wrapper must not call a dtor_body — the
                 * forward decl that would otherwise be emitted resolves
                 * to no definition at link time. Detect the marker init
                 * (ND_NULLPTR with TK_KW_DEFAULT / TK_KW_DELETE) and
                 * leave the wrapper to chain straight into member/base
                 * dtors. N4659 §10.1.6.4 [dcl.fct.def.default].
                 * Pattern: g++.dg/cpp0x/defaulted22.C. */
                Node *init = m->var_decl.init;
                if (init && init->kind == ND_NULLPTR && init->tok &&
                    (init->tok->kind == TK_KW_DEFAULT ||
                     init->tok->kind == TK_KW_DELETE))
                    continue;
                user_dtor_m_out_of_class = true;
                break;
            }
        }
    }

    /* Extern-only polymorphic classes — see docs/mangling.md
     * "System-class interop". A class declared polymorphic via
     * 'virtual T fn();' but whose virtual methods all live in a
     * system library (libstdc++.so for std::exception, std::bad_alloc,
     * etc.) has no in-TU bodies for those virtuals. Emitting the
     * vtable instance would reference symbols like
     * 'sf__std__exception__what_p_void_pe_' that are never defined
     * anywhere — cc fails with "undeclared here" before link.
     *
     * Skip-the-vtable: when no virtual member has an ND_FUNC_DEF
     * (inline body), emit no vtable struct, no vtable instance, and
     * no vptr field in the C struct layout. The class still appears
     * as a regular struct — usable in catch clauses, member access
     * for non-virtuals, and as an opaque type for pointers — just
     * without dispatch. Aligned with the deferred design discussion
     * in docs/mangling.md.
     *
     * SHORTCUT: this misses the case where a virtual is declared
     * in the class body (ND_VAR_DECL) and defined out-of-class in
     * the SAME TU (separate ND_FUNC_DEF). Those should keep their
     * vtable. For libcpp/gcc-14 source the pattern doesn't appear
     * — out-of-class virtual defs are rare; the common shape is
     * either inline-in-class or only-in-system-library. Handle when
     * a real source surfaces it. TODO(seafront#out-of-class-virt).
     * The any_virtual_has_body flag is computed once near 'bool poly'
     * above (so the vptr-field decision can also consult it). */

    /* Vtable struct + static instance — N4659 §13.3 [class.virtual].
     *
     * The struct has one function-pointer slot per virtual method,
     * in declaration order. The static instance is filled with the
     * mangled method addresses; ctors install '&instance' into the
     * object's __sf_vptr field, and call sites dispatch through it.
     *
     * The dtor slot (`__dtor`) is FIRST so its byte offset is stable
     * across the inheritance hierarchy: every class with a virtual
     * dtor exposes __dtor at offset 0 of its vtable struct, so
     * dispatch through a base-typed vtable pointer always lands on
     * the right slot regardless of how many regular virtuals the
     * Derived adds. N4659 §15.4 [class.dtor]/12. */
    bool need_dtor_slot = class_has_virtual_dtor(class_type);
    /* Emit the vtable STRUCT for any polymorphic class so derived
     * classes' secondary vtable instances (typed as the base's
     * vtable struct) have a complete type to allocate. The STRUCT
     * is just slot types — no symbol references — so it's safe
     * even for abstract bases with pure-virtuals or extern-only
     * polymorphic classes (libstdc++'s std::exception family).
     * The INSTANCE emit further down stays gated on
     * any_virtual_has_body. N4659 §13.3 [class.virtual]. */
    if (class_type && class_type->has_virtual_methods) {
        /* The struct definition. */
        fputs("struct ", stdout);
        mangle_class_vtable_type(class_type);
        fputs(" {\n", stdout);
        g_indent++;
        if (need_dtor_slot) {
            emit_indent();
            fputs("void (*__dtor)(struct ", stdout);
            mangle_class_tag(class_type);
            fputs(" *);\n", stdout);
        }
        for (int i = 0; i < n->class_def.nmembers; i++) {
            Node *m = n->class_def.members[i];
            if (!m) continue;
            bool is_virt_funcdef = (m->kind == ND_FUNC_DEF && m->func.is_virtual);
            bool is_virt_decl = (m->kind == ND_VAR_DECL && m->var_decl.is_virtual &&
                                 m->var_decl.ty && m->var_decl.ty->kind == TY_FUNC);
            if (!is_virt_funcdef && !is_virt_decl) continue;
            /* Virtual dtors don't get a vtable slot — see the
             * TODO(seafront#virt-dtor-slot) note on the vtable struct
             * comment above. */
            if (is_virt_funcdef && m->func.is_destructor) continue;
            if (is_virt_decl && m->var_decl.is_destructor) continue;

            Type *ret_ty = is_virt_funcdef ? m->func.ret_ty
                                           : m->var_decl.ty->ret;
            Token *mname = is_virt_funcdef ? m->func.name : m->var_decl.name;
            int nparams = is_virt_funcdef ? m->func.nparams
                                          : m->var_decl.ty->nparams;

            emit_indent();
            emit_type(ret_ty);
            fprintf(stdout, " (*%.*s)(struct ",
                    mname->len, mname->loc);
            mangle_class_tag(class_type);
            fputs(" *", stdout);
            if (is_virt_funcdef) {
                for (int k = 0; k < nparams; k++) {
                    fputs(", ", stdout);
                    emit_type(m->func.params[k]->param.ty);
                }
            } else {
                for (int k = 0; k < nparams; k++) {
                    fputs(", ", stdout);
                    emit_type(m->var_decl.ty->params[k]);
                }
            }
            fputs(");\n", stdout);
        }
        g_indent--;
        fputs("};\n", stdout);
    }

    /* The static instance — only when at least one virtual has a
     * body in this TU. Abstract bases with all-pure-virtual slots
     * and extern-only polymorphic classes are skipped (the slots
     * would either be NULL placeholders or undefined references). */
    if (class_type && class_type->has_virtual_methods && any_virtual_has_body) {
        /* Each slot points at the method's mangled free-function
         * form. The forward decls for these methods were emitted
         * above, so the names are visible here. */
        fputs("static const struct ", stdout);
        mangle_class_vtable_type(class_type);
        fputc(' ', stdout);
        mangle_class_vtable_instance(class_type);
        fputs(" = {\n", stdout);
        g_indent++;
        if (need_dtor_slot) {
            emit_indent();
            /* The wrapper symbol chains user body + member dtors +
             * base dtors. It always exists when class_has_virtual_dtor
             * is true (any class with a virtual dtor — user-declared
             * or transitive — gets a wrapper emitted). */
            mangle_class_dtor(class_type);
            fputs(",\n", stdout);
        }
        for (int i = 0; i < n->class_def.nmembers; i++) {
            Node *m = n->class_def.members[i];
            if (!m) continue;
            bool is_virt_funcdef = (m->kind == ND_FUNC_DEF && m->func.is_virtual);
            bool is_virt_decl = (m->kind == ND_VAR_DECL && m->var_decl.is_virtual &&
                                 m->var_decl.ty && m->var_decl.ty->kind == TY_FUNC);
            if (!is_virt_funcdef && !is_virt_decl) continue;
            if (is_virt_funcdef && m->func.is_destructor) continue;
            if (is_virt_decl && m->var_decl.is_destructor) continue;
            Token *mname = is_virt_funcdef ? m->func.name : m->var_decl.name;
            emit_indent();
            /* Slot points at an in-TU body. ND_FUNC_DEF is the in-class
             * inline body; ND_VAR_DECL is the in-class declaration that
             * may pair with an out-of-class ND_FUNC_DEF in the same TU
             * (g++.dg/ipa/devirt-2.C pattern). Search the TU for the
             * matching OOL definition before falling back to NULL.
             *
             * NULL slot for genuinely declaration-only virtuals (pure
             * virtuals or extern definitions): the slot stays in place
             * to keep the vtable layout stable for derived classes,
             * and a dispatched call would NULL-deref — matches Itanium
             * __cxa_pure_virtual semantics (call to pure virtual is UB). */
            Node *ool_def = NULL;
            if (is_virt_decl && g_tu) {
                for (int j = 0; j < g_tu->tu.ndecls; j++) {
                    Node *d = g_tu->tu.decls[j];
                    if (!d || d->kind != ND_FUNC_DEF) continue;
                    if (d->func.class_type != class_type) continue;
                    if (!d->func.name || !mname) continue;
                    if (d->func.name->len == mname->len &&
                        memcmp(d->func.name->loc, mname->loc, mname->len) == 0) {
                        ool_def = d;
                        break;
                    }
                }
            }
            if (is_virt_funcdef || ool_def) {
                Type **pty = NULL;
                int np = is_virt_funcdef
                       ? collect_func_param_types(m, &pty)
                       : collect_func_param_types(ool_def, &pty);
                /* Mirror the actual definition's const-ness so the
                 * vtable instance references the same mangled
                 * symbol the body emits. 'B *clone() const' on a
                 * virtual base needs '_const' in the suffix. */
                bool slot_const = false;
                if (is_virt_funcdef)
                    slot_const = m->func.is_const_method;
                else if (m->kind == ND_VAR_DECL && m->var_decl.ty)
                    slot_const = m->var_decl.ty->is_const;
                mangle_class_method(class_type, mname, pty, np,
                                        slot_const);
            } else {
                fputs("0", stdout);
            }
            fputs(",\n", stdout);
        }
        g_indent--;
        fputs("};\n", stdout);
    }

    /* Multi-inheritance virtual dispatch: for each non-first polymorphic
     * base B, emit thunks (one per virtual that this class overrides in
     * B's slot set) and a secondary vtable instance typed as B's vtable
     * struct. Installed by the ctor below so that a B*-view of a Derived
     * dispatches through Derived's overrides. N4659 §13.3 [class.virtual].
     * First base shares its vptr with the polymorphic-root chain (vptr
     * at offset 0), so no thunks needed there. */
    if (class_type && class_type->has_virtual_methods &&
        any_virtual_has_body) {
        int nb = class_nbases(class_type);
        for (int bi = 1; bi < nb; bi++) {
            Type *base = class_base(class_type, bi);
            if (!base || !base->has_virtual_methods) continue;
            if (!base->class_def) continue;
            Node *base_def = base->class_def;
            /* Dtor thunk for B-as-base — N4659 §15.4 [class.dtor]/12.
             * When base has a virtual dtor, its vtable struct exposes
             * a __dtor slot at offset 0; the secondary vtable for
             * Derived-as-B must populate the same slot with a thunk
             * so `delete bp;` (bp: B*) reaches Derived's dtor chain. */
            bool base_virt_dtor = class_has_virtual_dtor(base);
            if (base_virt_dtor && class_type->has_dtor) {
                fputs("static void ", stdout);
                mangle_class_tag(class_type);
                fputs("__thunk_for_", stdout);
                mangle_class_tag(base);
                fputs("__dtor(struct ", stdout);
                mangle_class_tag(base);
                fputs(" *__bp) {\n    ", stdout);
                mangle_class_dtor(class_type);
                fputs("((struct ", stdout);
                mangle_class_tag(class_type);
                fputs(" *)((char *)__bp - offsetof(struct ", stdout);
                mangle_class_tag(class_type);
                fprintf(stdout, ", __sf_base%d)));\n}\n", bi);
            }
            /* Pass 1: for every virtual slot in B that THIS class
             * overrides (same name + same param count), emit a thunk
             * function. The thunk adjusts B* down to Derived* and
             * calls the most-derived implementation. */
            for (int j = 0; j < base_def->class_def.nmembers; j++) {
                Node *bm = base_def->class_def.members[j];
                if (!bm) continue;
                bool b_funcdef = (bm->kind == ND_FUNC_DEF && bm->func.is_virtual);
                bool b_decl = (bm->kind == ND_VAR_DECL && bm->var_decl.is_virtual &&
                               bm->var_decl.ty && bm->var_decl.ty->kind == TY_FUNC);
                if (!b_funcdef && !b_decl) continue;
                if (b_funcdef && bm->func.is_destructor) continue;
                if (b_decl && bm->var_decl.is_destructor) continue;
                Token *bname = b_funcdef ? bm->func.name : bm->var_decl.name;
                int b_np = b_funcdef ? bm->func.nparams : bm->var_decl.ty->nparams;
                /* Does THIS class re-declare a same-named method with
                 * the same arity? If so, the slot needs a thunk. */
                Node *override_m = NULL;
                Type **override_pty = NULL;
                int override_np = -1;
                bool override_is_funcdef = false;
                for (int k = 0; k < n->class_def.nmembers; k++) {
                    Node *dm = n->class_def.members[k];
                    if (!dm) continue;
                    bool d_funcdef = (dm->kind == ND_FUNC_DEF && dm->func.is_virtual);
                    bool d_decl = (dm->kind == ND_VAR_DECL && dm->var_decl.is_virtual &&
                                   dm->var_decl.ty && dm->var_decl.ty->kind == TY_FUNC);
                    if (!d_funcdef && !d_decl) continue;
                    Token *dn = d_funcdef ? dm->func.name : dm->var_decl.name;
                    int d_np = d_funcdef ? dm->func.nparams : dm->var_decl.ty->nparams;
                    if (!dn || !bname) continue;
                    if (dn->len != bname->len ||
                        memcmp(dn->loc, bname->loc, bname->len) != 0) continue;
                    if (d_np != b_np) continue;
                    override_m = dm;
                    override_is_funcdef = d_funcdef;
                    override_np = collect_func_param_types(dm, &override_pty);
                    break;
                }
                if (!override_m) continue;
                /* Pure-virtual override (ND_VAR_DECL with no in-TU
                 * body) — no concrete symbol exists for the thunk
                 * to call. Skip emitting the thunk and let the
                 * secondary-vtable slot fall back to 0 below (gcc 4.8
                 * g++.dg/abi/covariant4 — RA::clone() is = 0). */
                if (override_m->kind == ND_VAR_DECL && g_tu) {
                    Token *on = override_m->var_decl.name;
                    bool has_body = false;
                    for (int j = 0; j < g_tu->tu.ndecls && !has_body; j++) {
                        Node *d = g_tu->tu.decls[j];
                        if (!d || d->kind != ND_FUNC_DEF) continue;
                        if (d->func.class_type != class_type) continue;
                        if (!d->func.name || !on) continue;
                        if (d->func.name->len == on->len &&
                            memcmp(d->func.name->loc, on->loc, on->len) == 0)
                            has_body = true;
                    }
                    if (!has_body) continue;
                }
                /* Emit the thunk. Signature mirrors B's slot:
                 *   <ret> sf__<Deriv>__thunk_for_<Base>_<method-suffix>(
                 *       struct sf__<Base> *bp, <other params>) {
                 *     <return> sf__<Deriv>__<method>_p_..._pe_(
                 *       (struct sf__<Deriv> *)((char *)bp -
                 *           offsetof(struct sf__<Deriv>, __sf_base<bi>)),
                 *       <other params...>);
                 *   } */
                Type *ret_ty = b_funcdef ? bm->func.ret_ty
                                         : bm->var_decl.ty->ret;
                /* Param types for both the suffix and the thunk's
                 * own declaration. Funcdef carries ND_PARAM children;
                 * decl carries them as Type** on the function type. */
                Type **b_pty = NULL;
                int b_pn = 0;
                if (b_funcdef) b_pn = collect_func_param_types(bm, &b_pty);
                else { b_pty = bm->var_decl.ty->params; b_pn = b_np; }
                fputs("static ", stdout);
                emit_type(ret_ty);
                fputc(' ', stdout);
                mangle_class_tag(class_type);
                fputs("__thunk_for_", stdout);
                mangle_class_tag(base);
                fputs("__", stdout);
                emit_token_text(bname);
                /* Param suffix keeps overloaded virtuals distinct. */
                mangle_param_suffix(b_pty, b_pn);
                fputs("(struct ", stdout);
                mangle_class_tag(base);
                fputs(" *__bp", stdout);
                for (int p = 0; p < b_np; p++) {
                    fputs(", ", stdout);
                    Type *pty = b_funcdef ? bm->func.params[p]->param.ty
                                          : bm->var_decl.ty->params[p];
                    emit_type(pty);
                    fprintf(stdout, " __a%d", p);
                }
                fputs(") {\n    ", stdout);
                if (ret_ty && ret_ty->kind != TY_VOID) fputs("return ", stdout);
                /* Const-ness mirrors the derived class's override: a
                 * 'B *clone() const' in the base demands an override
                 * that's also const, so the derived's symbol carries
                 * the _const suffix. Mangling 'is_const=false' here
                 * referenced a non-existent non-const symbol — the
                 * defs had _const, the thunks called without it. */
                bool ov_const = false;
                if (override_is_funcdef)
                    ov_const = override_m->func.is_const_method;
                else if (override_m->var_decl.ty)
                    ov_const = override_m->var_decl.ty->is_const;
                mangle_class_method(class_type, bname, override_pty,
                                     override_np, ov_const);
                fputs("((struct ", stdout);
                mangle_class_tag(class_type);
                fputs(" *)((char *)__bp - offsetof(struct ", stdout);
                mangle_class_tag(class_type);
                fprintf(stdout, ", __sf_base%d))", bi);
                for (int p = 0; p < b_np; p++)
                    fprintf(stdout, ", __a%d", p);
                fputs(");\n}\n", stdout);
                (void)override_is_funcdef;
            }
            /* Pass 2: emit the secondary vtable instance. Same struct
             * type as B's vtable; slot values are the thunk (when
             * overridden) or B's own slot value otherwise. */
            fputs("static const struct ", stdout);
            mangle_class_vtable_type(base);
            fputc(' ', stdout);
            mangle_class_tag(class_type);
            fputs("__vtable_for_", stdout);
            mangle_class_tag(base);
            fputs("_instance = {\n", stdout);
            g_indent++;
            /* Slot 0 of B's vtable struct is __dtor when B has a
             * virtual dtor — match that layout. */
            if (base_virt_dtor) {
                emit_indent();
                if (class_type->has_dtor) {
                    mangle_class_tag(class_type);
                    fputs("__thunk_for_", stdout);
                    mangle_class_tag(base);
                    fputs("__dtor", stdout);
                } else {
                    /* No Derived dtor to thunk to — fall back to B's
                     * own wrapper (which still runs B's chain). */
                    mangle_class_dtor(base);
                }
                fputs(",\n", stdout);
            }
            for (int j = 0; j < base_def->class_def.nmembers; j++) {
                Node *bm = base_def->class_def.members[j];
                if (!bm) continue;
                bool b_funcdef = (bm->kind == ND_FUNC_DEF && bm->func.is_virtual);
                bool b_decl = (bm->kind == ND_VAR_DECL && bm->var_decl.is_virtual &&
                               bm->var_decl.ty && bm->var_decl.ty->kind == TY_FUNC);
                if (!b_funcdef && !b_decl) continue;
                if (b_funcdef && bm->func.is_destructor) continue;
                if (b_decl && bm->var_decl.is_destructor) continue;
                Token *bname = b_funcdef ? bm->func.name : bm->var_decl.name;
                int b_np = b_funcdef ? bm->func.nparams : bm->var_decl.ty->nparams;
                Node *override_m = NULL;
                for (int k = 0; k < n->class_def.nmembers; k++) {
                    Node *dm = n->class_def.members[k];
                    if (!dm) continue;
                    bool d_funcdef = (dm->kind == ND_FUNC_DEF && dm->func.is_virtual);
                    bool d_decl = (dm->kind == ND_VAR_DECL && dm->var_decl.is_virtual &&
                                   dm->var_decl.ty && dm->var_decl.ty->kind == TY_FUNC);
                    if (!d_funcdef && !d_decl) continue;
                    Token *dn = d_funcdef ? dm->func.name : dm->var_decl.name;
                    int d_np = d_funcdef ? dm->func.nparams : dm->var_decl.ty->nparams;
                    if (!dn || !bname) continue;
                    if (dn->len != bname->len ||
                        memcmp(dn->loc, bname->loc, bname->len) != 0) continue;
                    if (d_np != b_np) continue;
                    override_m = dm;
                    break;
                }
                /* Pure-virtual override has no thunk emitted by pass 1
                 * (no body to call). Clear override_m so the slot falls
                 * through to the pass-through / base case below — and
                 * THAT case will also produce 0 if base's method is
                 * pure too. */
                if (override_m && override_m->kind == ND_VAR_DECL && g_tu) {
                    Token *on = override_m->var_decl.name;
                    bool has_body = false;
                    for (int j = 0; j < g_tu->tu.ndecls && !has_body; j++) {
                        Node *d = g_tu->tu.decls[j];
                        if (!d || d->kind != ND_FUNC_DEF) continue;
                        if (d->func.class_type != class_type) continue;
                        if (!d->func.name || !on) continue;
                        if (d->func.name->len == on->len &&
                            memcmp(d->func.name->loc, on->loc, on->len) == 0)
                            has_body = true;
                    }
                    if (!has_body) override_m = NULL;
                }
                Type **slot_pty = NULL;
                int slot_pn = 0;
                if (b_funcdef) slot_pn = collect_func_param_types(bm, &slot_pty);
                else { slot_pty = bm->var_decl.ty->params; slot_pn = b_np; }
                emit_indent();
                if (override_m) {
                    mangle_class_tag(class_type);
                    fputs("__thunk_for_", stdout);
                    mangle_class_tag(base);
                    fputs("__", stdout);
                    emit_token_text(bname);
                    mangle_param_suffix(slot_pty, slot_pn);
                } else {
                    /* Pass-through: emit base's method symbol IFF a
                     * body exists somewhere. Pure-virtual base methods
                     * have no body anywhere — fall back to 0. */
                    bool base_has_body = b_funcdef;  /* ND_FUNC_DEF is in-class body */
                    if (!base_has_body && b_decl && g_tu) {
                        Token *bn = bm->var_decl.name;
                        for (int j = 0; j < g_tu->tu.ndecls && !base_has_body; j++) {
                            Node *d = g_tu->tu.decls[j];
                            if (!d || d->kind != ND_FUNC_DEF) continue;
                            if (d->func.class_type != base) continue;
                            if (!d->func.name || !bn) continue;
                            if (d->func.name->len == bn->len &&
                                memcmp(d->func.name->loc, bn->loc, bn->len) == 0)
                                base_has_body = true;
                        }
                    }
                    if (!base_has_body) {
                        fputs("0", stdout);
                    } else {
                        bool b_const = false;
                        if (b_funcdef) b_const = bm->func.is_const_method;
                        else if (bm->var_decl.ty) b_const = bm->var_decl.ty->is_const;
                        mangle_class_method(base, bname, slot_pty, slot_pn,
                                             b_const);
                    }
                }
                fputs(",\n", stdout);
            }
            g_indent--;
            fputs("};\n", stdout);
        }
    }

    /* Now emit each method (ND_FUNC_DEF in the member list) as a
     * separate free function. Trivial dtors are skipped to match
     * the forward-decl loop above. g_current_class_def is set so
     * ctor body emission can walk this class's members in
     * declaration order to emit member-init calls. */
    Node *saved_cdef = g_current_class_def;
    g_current_class_def = n;
    for (int i = 0; i < n->class_def.nmembers; i++) {
        Node *m = n->class_def.members[i];
        if (!m || m->kind != ND_FUNC_DEF || !class_type) continue;
        if (m->func.is_destructor) {
            Node *body = m->func.body;
            bool empty = body && body->kind == ND_BLOCK &&
                         body->block.nstmts == 0;
            if (empty || !class_type->has_dtor) continue;
        }
        /* Const/non-const overloads now get distinct mangled names
         * (via _const suffix) — no dedup needed. N4659 §16.2/3.
         * In-class method bodies are implicitly inline. */
        emit_method_as_free_fn(m, class_type, /*emit_inline=*/true);
    }
    g_current_class_def = saved_cdef;

    /* Synthesize the sf__Class__dtor wrapper. Calls
     * sf__Class__dtor_body (if a user dtor existed) FIRST, then
     * chains into each non-trivially-destructible member's dtor in
     * REVERSE declaration order — N4659 §15.4 [class.dtor]/9.
     * After the members, chains into each base subobject's dtor,
     * also in REVERSE declaration order. */
    if (class_type && class_type->has_dtor) {
        fputs("__SF_INLINE void ", stdout);
        mangle_class_dtor(class_type);
        fputs(class_type->kind == TY_UNION ? "(union " : "(struct ", stdout);
        mangle_class_tag(class_type);
        fputs(" *this) {\n", stdout);
        g_indent++;
        if (user_dtor_m || user_dtor_m_out_of_class) {
            emit_indent();
            mangle_class_dtor_body(class_type);
            fputs("(this);\n", stdout);
        }
        for (int i = n->class_def.nmembers - 1; i >= 0; i--) {
            Node *m = n->class_def.members[i];
            if (!m || m->kind != ND_VAR_DECL) continue;
            if (!m->var_decl.ty || m->var_decl.ty->kind != TY_STRUCT) continue;
            if (!m->var_decl.ty->has_dtor) continue;
            if (!m->var_decl.name) continue;
            emit_indent();
            mangle_class_dtor(m->var_decl.ty);
            fprintf(stdout, "(&this->%.*s);\n",
                    m->var_decl.name->len, m->var_decl.name->loc);
        }
        /* Base subobject destruction — reverse declaration order. */
        int nb_d = class_nbases(class_type);
        for (int b = nb_d - 1; b >= 0; b--) {
            Type *base = class_base(class_type, b);
            if (!base || !base->has_dtor) continue;
            emit_indent();
            mangle_class_dtor(base);
            fputs("(&this->", stdout);
            if (b == 0) fputs("__sf_base", stdout);
            else        fprintf(stdout, "__sf_base%d", b);
            fputs(");\n", stdout);
        }
        g_indent--;
        fputs("}\n", stdout);
    }

    /* Synthesize a default ctor when the class has has_default_ctor
     * set but no user-declared ctor exists — N4659 §15.1 [class.ctor]/4.
     * Body just chains into each member's default ctor in declaration
     * order. Trivially-default-constructible members contribute
     * nothing (no call). */
    /* Distinguish:
     *   - user-provided default ctor with body (blocks synthesis)
     *   - 'A() = default;' (explicitly requests synthesis,
     *     N4659 §10.1.6.4 [dcl.fct.def.default])
     *   - any user-declared ctor at all (blocks the implicit-default
     *     ctor rule, §15.1 [class.ctor]/4)
     * The defaulted-default request must always produce a body even
     * when other user ctors like A(int) are declared. */
    bool user_default_ctor_with_body = false;
    bool has_defaulted_default_ctor = false;
    bool any_user_ctor = false;
    for (int i = 0; i < n->class_def.nmembers; i++) {
        Node *m = n->class_def.members[i];
        if (!m) continue;
        if (m->kind == ND_FUNC_DEF && m->func.is_constructor) {
            any_user_ctor = true;
            if (m->func.nparams == 0) user_default_ctor_with_body = true;
        } else if (m->kind == ND_VAR_DECL && m->var_decl.ty &&
                   m->var_decl.ty->kind == TY_FUNC &&
                   m->var_decl.is_constructor) {
            any_user_ctor = true;
            int nparams = m->var_decl.ty->nparams;
            Node *init = m->var_decl.init;
            bool is_defaulted = init && init->kind == ND_NULLPTR &&
                init->tok && init->tok->kind == TK_KW_DEFAULT;
            bool is_deleted = init && init->kind == ND_NULLPTR &&
                init->tok && init->tok->kind == TK_KW_DELETE;
            if (nparams == 0) {
                if (is_defaulted) has_defaulted_default_ctor = true;
                else if (!is_deleted) user_default_ctor_with_body = true;
            }
        }
    }
    bool needs_default_ctor_synth =
        (has_defaulted_default_ctor ||
         (class_type && class_type->has_default_ctor && !any_user_ctor)) &&
        !user_default_ctor_with_body;
    if (class_type && needs_default_ctor_synth) {
        const char *kw_open =
            class_type->kind == TY_UNION ? "(union " : "(struct ";
        /* Forward decl + body for the synthesized ctor (0-param). */
        fputs("__SF_INLINE void ", stdout);
        mangle_class_ctor(class_type, NULL, 0);
        fputs(kw_open, stdout);
        mangle_class_tag(class_type);
        fputs(" *this);\n", stdout);

        fputs("__SF_INLINE void ", stdout);
        mangle_class_ctor(class_type, NULL, 0);
        fputs(kw_open, stdout);
        mangle_class_tag(class_type);
        fputs(" *this) {\n", stdout);
        /* NSDMI emission inside this body needs g_current_class_def
         * pointed at the enclosing class so emit_ident's implicit-this
         * branch can walk the __sf_base chain to find inherited
         * members. The method-body loop above sets/unsets this around
         * its own emission; the synthesis path emits *after* that, so
         * re-establish the context here. */
        Node *saved_cdef_synth = g_current_class_def;
        g_current_class_def = n;
        g_indent++;
        /* Base subobject construction first — declaration order. */
        int nb_c = class_nbases(class_type);
        for (int b = 0; b < nb_c; b++) {
            Type *base = class_base(class_type, b);
            if (!base || !base->has_default_ctor) continue;
            emit_indent();
            mangle_class_ctor(base, NULL, 0);
            fputs("(&this->", stdout);
            if (b == 0) fputs("__sf_base", stdout);
            else        fprintf(stdout, "__sf_base%d", b);
            fputs(");\n", stdout);
        }
        /* Install vptr next for polymorphic classes — see the
         * matching emit_ctor_member_inits path for the rationale.
         * Skip for extern-only polymorphic classes (no in-TU virtual
         * body): the vtable instance isn't emitted. */
        if (class_type->has_virtual_methods && any_virtual_has_body) {
            emit_indent();
            emit_vptr_lhs(class_type);
            fputs(" = &", stdout);
            mangle_class_vtable_instance(class_type);
            fputs(";\n", stdout);
            /* Secondary-vtable install for non-first poly bases. Same
             * shape as emit_ctor_member_inits. */
            for (int bi = 1; bi < nb_c; bi++) {
                Type *base = class_base(class_type, bi);
                if (!base || !base->has_virtual_methods) continue;
                Type *base_root = vptr_owner_class(base);
                if (!base_root) base_root = base;
                emit_indent();
                fputs("((struct ", stdout);
                mangle_class_tag(base_root);
                fprintf(stdout, " *)&this->__sf_base%d)->__sf_vptr = ", bi);
                fputs("(const struct ", stdout);
                mangle_class_vtable_type(base_root);
                fputs(" *)&", stdout);
                mangle_class_tag(class_type);
                fputs("__vtable_for_", stdout);
                mangle_class_tag(base);
                fputs("_instance;\n", stdout);
            }
        }
        for (int i = 0; i < n->class_def.nmembers; i++) {
            Node *m = n->class_def.members[i];
            if (!m || m->kind != ND_VAR_DECL) continue;
            if (!m->var_decl.ty || m->var_decl.ty->kind == TY_FUNC) continue;
            if (m->var_decl.storage_flags & DECL_STATIC) continue;
            if (!m->var_decl.name) continue;
            if (m->var_decl.ty->kind == TY_STRUCT ||
                m->var_decl.ty->kind == TY_UNION) {
                /* NSDMI for class-typed member: 'A a = f();' or
                 * 'A a{args};' inside the class body. N4659 §12.6.2/9
                 * [class.base.init] runs the NSDMI in lieu of the
                 * default ctor when no member-init list mention names
                 * this member. Sea-front previously called the default
                 * ctor and dropped the initializer; flip the order so
                 * the NSDMI expression's value goes into the member.
                 * Pattern: g++.dg/cpp0x/nsdmi4.C 'A a = f();'. */
                if (m->var_decl.init) {
                    emit_indent();
                    fprintf(stdout, "this->%.*s = ",
                            m->var_decl.name->len, m->var_decl.name->loc);
                    emit_expr(m->var_decl.init);
                    fputs(";\n", stdout);
                    continue;
                }
                if (!m->var_decl.ty->has_default_ctor) continue;
                emit_indent();
                mangle_class_ctor(m->var_decl.ty, NULL, 0);
                fprintf(stdout, "(&this->%.*s);\n",
                        m->var_decl.name->len, m->var_decl.name->loc);
                continue;
            }
            /* NSDMI for scalar / pointer / enum members — apply the
             * in-class default-member-init as an assignment in the
             * synthesized default ctor. N4659 §12.6.2/9. */
            if (m->var_decl.init) {
                emit_indent();
                fprintf(stdout, "this->%.*s = ",
                        m->var_decl.name->len, m->var_decl.name->loc);
                emit_expr(m->var_decl.init);
                fputs(";\n", stdout);
            }
        }
        g_indent--;
        fputs("}\n", stdout);
        g_current_class_def = saved_cdef_synth;
    }
}

static void emit_top_level(Node *n) {
    if (!n) return;
    /* Phase-aware split for function definitions: PHASE_STRUCTS is
     * the source-order types/vars/enums pass — emit only the
     * forward declaration here (so later var-decl initializers can
     * reference it). The body waits for PHASE_METHODS. Caught here
     * (not just at the driver loop) so recursion through
     * ND_TEMPLATE_DECL / ND_BLOCK lands in the right phase. */
    if (n->kind == ND_FUNC_DEF && g_emit_phase == PHASE_STRUCTS) {
        emit_fwd_decl_methods_only(n);
        return;
    }
    switch (n->kind) {
    case ND_FUNC_DEF:
        /* Out-of-class method definition 'int Foo::bar() {}' was
         * tagged by the parser with the resolved class type. Emit
         * it as a mangled free function with the 'this' parameter
         * prepended.
         *
         * For ctors specifically, set g_current_class_def from the
         * class type's back-pointer so emit_ctor_member_inits can
         * walk the class members in declaration order — same
         * machinery the in-class method-emission loop already uses. */
        if (n->func.class_type && n->func.class_type->tag) {
            /* Const/non-const overloads now get distinct mangled names
             * (via _const suffix) — no dedup needed. N4659 §16.2/3. */
            Node *saved = g_current_class_def;
            /* Set the enclosing class for any OOL method, not just
             * constructors. Implicit-this name resolution
             * (find_base_path / emit_base_chain) needs g_current_class_def
             * to walk through __sf_base for inherited-member access in
             * the body. Without this, 'b' in 'void D::foo() { ...b... }'
             * (b inherited from C, D's first base) emits as 'this->b'
             * which has no such field — N4659 §10.2 [class.member.lookup]. */
            if (n->func.class_type->class_def)
                g_current_class_def = n->func.class_type->class_def;
            /* OOL definition: emit as static inline iff the user
             * marked it 'inline' (DECL_INLINE) OR the owner class is
             * a template instantiation (multi-TU body, same shape as
             * inline). Otherwise strong global (N4659 §10.1.6/4) —
             * resolvable from other TUs that only saw the declaration. */
            bool ool_inline = (n->func.storage_flags & DECL_INLINE) != 0
                || (n->func.class_type
                    && n->func.class_type->n_template_args > 0);
            emit_method_as_free_fn(n, n->func.class_type, ool_inline);
            g_current_class_def = saved;
        } else {
            /* Dedup: inline functions from headers may be included
             * multiple times in the preprocessed output. The dedup
             * key is the full mangled signature (name + canonical
             * param suffix) — same key = same function. */
            int np = n->func.nparams;
            Type **params = np > 0 ? malloc(np * sizeof(Type *)) : NULL;
            if (np > 0 && !params) abort();
            for (int i = 0; i < np; i++)
                params[i] = (n->func.params[i] &&
                             n->func.params[i]->kind == ND_PARAM)
                              ? n->func.params[i]->param.ty : NULL;
            bool dup = func_def_dedup_check_sig(n->func.name, params, np,
                                                 n->func.storage_flags);
            free(params);
            if (dup) return;
            emit_func_def(n);
        }
        return;
    case ND_CLASS_DEF: emit_class_def(n); return;
    case ND_VAR_DECL:
        /* Dedup top-level var-decls: the two-phase emit walks ND_BLOCK
         * nodes in both passes, so vars inside extern "C" blocks /
         * multi-declarator groups get visited twice. Per-node bool
         * flag — replaces a fixed-size static array that overflowed on
         * large TUs (gcc 4.8 i386.c has > 1024 file-scope vars,
         * causing duplicate emissions of ix86_first_cycle_multipass_data
         * et al). */
        if (n->var_decl.name) {
            if (n->codegen_emitted) return;
            n->codegen_emitted = true;
        }
        /* Bare enum definition: 'enum Color { RED, GREEN };' becomes
         * ND_VAR_DECL with type TY_ENUM and no name. Emit the enum
         * body as a C enum definition. */
        if (n->var_decl.ty && n->var_decl.ty->kind == TY_ENUM &&
            n->var_decl.ty->enum_tokens && n->var_decl.ty->enum_ntokens > 0 &&
            !n->var_decl.name) {
            if (enum_body_already_emitted(n->var_decl.ty->enum_tokens)) return;
            mark_enum_body_emitted(n->var_decl.ty->enum_tokens);
            n->var_decl.ty->codegen_emitted = true;
            emit_source_comment(n->tok);
            fputs("enum ", stdout);
            if (n->var_decl.ty->tag)
                fprintf(stdout, "%.*s ", n->var_decl.ty->tag->len,
                        n->var_decl.ty->tag->loc);
            fputs("{ ", stdout);
            emit_enum_body(n->var_decl.ty);
            fputs(" }", stdout);
            if (n->var_decl.ty->is_packed)
                fputs(" __attribute__((packed))", stdout);
            fputs(";\n", stdout);
            return;
        }
        /* Top-level free function declaration: 'int foo();' parses
         * as ND_VAR_DECL with TY_FUNC. emit_var_decl_inner doesn't
         * know how to print TY_FUNC (emit_type falls back to int),
         * so we synthesize the C declaration shape directly here. */
        if (n->var_decl.ty && n->var_decl.ty->kind == TY_FUNC &&
            n->var_decl.name) {
            if (func_decl_dedup_check_sig(n->var_decl.name,
                                            n->var_decl.ty->params,
                                            n->var_decl.ty->nparams))
                return;
            /* Extern-C: only emit the first decl with this name (see
             * ffsig_is_first_c_linkage rationale). */
            if ((n->var_decl.storage_flags & DECL_C_LINKAGE) &&
                !ffsig_is_first_c_linkage(n->var_decl.name,
                                           n->var_decl.ty->params,
                                           n->var_decl.ty->nparams))
                return;
            emit_source_comment(n->tok);
            {
                int sf = n->var_decl.storage_flags;
                if (sf & DECL_CONSTEXPR) sf |= DECL_INLINE;
                emit_storage_flags(sf & ~DECL_CONSTEXPR);
            }
            /* GCC constructor / destructor function attributes — pass
             * through. See emit_var_decl_inner's matching branch for
             * the rationale (g++.dg/init/attrib1.C). Priority arg
             * gets passed via N-form when non-zero (initpri1.C). */
            if (n->var_decl.attr_constructor) {
                if (n->var_decl.ctor_priority > 0)
                    fprintf(stdout, "__attribute__((constructor(%d))) ",
                            n->var_decl.ctor_priority);
                else
                    fputs("__attribute__((constructor)) ", stdout);
            }
            if (n->var_decl.attr_destructor) {
                if (n->var_decl.dtor_priority > 0)
                    fprintf(stdout, "__attribute__((destructor(%d))) ",
                            n->var_decl.dtor_priority);
                else
                    fputs("__attribute__((destructor)) ", stdout);
            }
            Type *fty = n->var_decl.ty;
            /* Function returning a function pointer requires
             * declarator-interleaving (N4659 §11.3):
             *   void (*signal(int, void(*)(int)))(int);
             * rather than the invalid 'void (*)(int) signal(...)'. */
            /* Function returning a pointer-to-array — name interleaves
             * with the '*' inside the parens. N4659 §11.3 [dcl.meaning]. */
            {
                Type *t = fty->ret;
                int n_ptrs = 0;
                while (t && t->kind == TY_PTR) { n_ptrs++; t = t->base; }
                if (n_ptrs >= 1 && t && t->kind == TY_ARRAY) {
                    Type *inner_dims = t;
                    Type *elem = inner_dims;
                    while (elem && elem->kind == TY_ARRAY) elem = elem->base;
                    if (elem && elem->kind != TY_ARRAY &&
                        elem->kind != TY_PTR && elem->kind != TY_FUNC) {
                        emit_type(elem);
                        fputs(" (", stdout);
                        for (int i = 0; i < n_ptrs; i++) fputc('*', stdout);
                        fprintf(stdout, "%.*s(",
                                n->var_decl.name->len, n->var_decl.name->loc);
                        if (fty->nparams == 0) {
                            fputs("void", stdout);
                        } else {
                            for (int i = 0; i < fty->nparams; i++) {
                                if (i > 0) fputs(", ", stdout);
                                emit_type(fty->params[i]);
                            }
                            if (fty->is_variadic) fputs(", ...", stdout);
                        }
                        fputs("))", stdout);
                        emit_array_dims(inner_dims);
                        fputs(";\n", stdout);
                        return;
                    }
                }
            }
            bool ret_is_fptr = fty->ret && fty->ret->kind == TY_PTR &&
                               fty->ret->base && fty->ret->base->kind == TY_FUNC;
            if (ret_is_fptr) {
                Type *inner = fty->ret->base;
                emit_type(inner->ret);
                fprintf(stdout, " (*%.*s(",
                        n->var_decl.name->len, n->var_decl.name->loc);
                if (fty->nparams == 0) {
                    fputs("void", stdout);
                } else {
                    for (int i = 0; i < fty->nparams; i++) {
                        if (i > 0) fputs(", ", stdout);
                        emit_type(fty->params[i]);
                    }
                    if (fty->is_variadic) fputs(", ...", stdout);
                }
                fputs("))(", stdout);
                for (int i = 0; i < inner->nparams; i++) {
                    if (i > 0) fputs(", ", stdout);
                    emit_type(inner->params[i]);
                }
                if (inner->is_variadic) {
                    if (inner->nparams > 0) fputs(", ", stdout);
                    fputs("...", stdout);
                } else if (inner->nparams == 0) {
                    fputs("void", stdout);
                }
                fputs(");\n", stdout);
                return;
            }
            emit_type(fty->ret);
            fputc(' ', stdout);
            emit_free_func_symbol(n->var_decl.name, n->var_decl.asm_name,
                                   (n->var_decl.storage_flags & DECL_C_LINKAGE) != 0,
                                   fty->params, fty->nparams);
            fputc('(', stdout);
            if (fty->nparams == 0) {
                fputs("void", stdout);
            } else {
                for (int i = 0; i < fty->nparams; i++) {
                    if (i > 0) fputs(", ", stdout);
                    emit_type(fty->params[i]);
                }
                if (fty->is_variadic) fputs(", ...", stdout);
            }
            fputs(");\n", stdout);
            return;
        }
        /* Emit dependency struct/union definitions before the var-decl
         * references them. Covers patterns like
         *   static const struct { ... } table[] = {...};
         * where the anonymous/named struct is defined inline with the
         * array and has no separate ND_CLASS_DEF at top level. Without
         * this the C output references 'struct __sf_anon_N' that was
         * never defined. Also peel TY_PTR — the 'static struct X
         * { ... } *p;' idiom defines X inline but declares a pointer.
         * Pattern: gcc 4.8 tree-cfg.c label_for_bb, tree-eh.c labels. */
        {
            Type *dep = n->var_decl.ty;
            while (dep && (dep->kind == TY_ARRAY || dep->kind == TY_PTR)
                   && dep->base) dep = dep->base;
            if (dep && (dep->kind == TY_STRUCT || dep->kind == TY_UNION) &&
                dep->class_def && !dep->codegen_emitted) {
                /* Force PHASE_STRUCTS for the eager emit even if the
                 * caller is in PHASE_METHODS — we want the struct body
                 * (so the var-decl can reference its members) but NOT
                 * the method bodies. Method bodies will emit when the
                 * driver's PHASE_METHODS pass visits the class def. If
                 * the class is inline (no separate ND_CLASS_DEF), it
                 * has no methods anyway, so skipping them is harmless.
                 *
                 * Critically: do NOT use single-pass (phase=0) here.
                 * That would emit method bodies referencing OTHER
                 * still-incomplete instantiations, producing
                 * 'invalid use of undefined type' chains across the
                 * gcc 4.8 vec.h instantiations. */
                int saved_phase = g_emit_phase;
                g_emit_phase = PHASE_STRUCTS;
                emit_class_def(dep->class_def);
                g_emit_phase = saved_phase;
            }
        }
        /* File-scope dynamic initialisation: C requires constant
         * initialisers for objects of static storage duration (C11
         * §6.7.9/4). For non-class scalar/pointer globals whose init
         * is a function call, qualified cast through a non-folded
         * pointer, etc., defer the init to __sf_global_init so the
         * file-scope emit just declares the variable. Don't intercept
         * class-typed inits — those have their own copy-from-temp or
         * ctor-call lowering elsewhere. N4659 §6.6.2 [basic.start.init].
         * Skipped for extern / function decls (handled above) and for
         * obvious constant shapes (literals, address-of-global,
         * compound literals). */
        if (n->var_decl.name && n->var_decl.init && n->var_decl.ty &&
            n->var_decl.ty->kind != TY_FUNC &&
            n->var_decl.ty->kind != TY_ARRAY &&
            n->var_decl.ty->kind != TY_STRUCT &&
            n->var_decl.ty->kind != TY_UNION &&
            n->var_decl.ty->kind != TY_REF &&
            n->var_decl.ty->kind != TY_RVALREF &&
            /* const-qualified globals can't be reassigned in C's
             * __sf_global_init body (read-only error). Leave the init
             * in place and rely on gcc's folding for the common cases
             * gcc accepts. The cast-through-pointer pattern that
             * needs deferral (cast4) typically isn't const-typed. */
            !n->var_decl.ty->is_const &&
            !(n->var_decl.storage_flags & DECL_EXTERN) &&
            !is_const_initializer_for_static(n->var_decl.init)) {
            n->var_decl.deferred_to_global_init = true;
            Node *saved_init = n->var_decl.init;
            n->var_decl.init = NULL;
            emit_var_storage_flags(n->var_decl.storage_flags);
            emit_var_decl_inner(n);
            fputs(";\n", stdout);
            n->var_decl.init = saved_init;
            return;
        }
        /* File-scope class-typed direct-init ('const T x(args);'):
         * the ctor runs in __sf_global_init and writes to the global
         * storage, so 'const' on the C-level decl would either fail
         * the cast-through-T* (warning) or — worse — put the global
         * in .rodata and SIGSEGV at startup. Temporarily clear the
         * type's is_const for the emit so the file-scope storage is
         * writable; restore after. */
        if (n->var_decl.ty && n->var_decl.ty->is_const &&
            (n->var_decl.ty->kind == TY_STRUCT || n->var_decl.ty->kind == TY_UNION) &&
            n->var_decl.has_ctor_init && n->var_decl.ctor_nargs > 0) {
            bool saved_const = n->var_decl.ty->is_const;
            n->var_decl.ty->is_const = false;
            emit_var_storage_flags(n->var_decl.storage_flags);
            emit_var_decl_inner(n);
            fputs(";\n", stdout);
            n->var_decl.ty->is_const = saved_const;
            return;
        }
        /* OOL static data member definition: the in-class declaration
         * is emitted as `__attribute__((weak)) T sf__C__m;` in every
         * TU that includes the class header. The OOL emits STRONG
         * (no weak) so it overrides those weak siblings — that's
         * what supplies the initializer and gives one well-defined
         * storage location for foo_t::count etc. across TUs.
         * N4659 §11.4.9.2 [class.static.data]. */
        emit_var_storage_flags(n->var_decl.storage_flags);
        emit_var_decl_inner(n);
        fputs(";\n", stdout);
        /* init_priority(N) is handled separately in the per-var
         * prioritized-init synthesis at the bottom of the TU emit;
         * cc warns "init_priority attribute directive ignored" if
         * we re-emit the attribute on the var def, so don't. */
        return;
    case ND_BLOCK:
        /* The parser packs namespace contents (and extern "C" blocks)
         * into ND_BLOCK at top level. Recurse into the inner decls. */
        for (int i = 0; i < n->block.nstmts; i++)
            emit_top_level(n->block.stmts[i]);
        return;
    case ND_TEMPLATE_DECL:
        /* Full specializations (nparams == 0) are concrete — emit them.
         * N4659 §17.7.3 [temp.expl.spec] — an explicit specialization
         * is not itself a template; it's a regular declaration. */
        if (n->template_decl.nparams == 0 && n->template_decl.decl)
            emit_top_level(n->template_decl.decl);
        return;
    case ND_TYPEDEF: {
        /* Emit the underlying type definition if applicable. */
        Type *uty = n->var_decl.ty;
        while (ty_is_indirect(uty) && uty->base) uty = uty->base;
        /* Struct/union with body */
        if (uty && (uty->kind == TY_STRUCT || uty->kind == TY_UNION) &&
            uty->class_def)
            emit_class_def(uty->class_def);
        /* Enum with body */
        if (uty && uty->kind == TY_ENUM &&
            uty->enum_tokens && uty->enum_ntokens > 0 &&
            !enum_body_already_emitted(uty->enum_tokens)) {
            mark_enum_body_emitted(uty->enum_tokens);
            uty->codegen_emitted = true;
            fputs("enum ", stdout);
            if (uty->tag)
                fprintf(stdout, "%.*s ", uty->tag->len, uty->tag->loc);
            fputs("{ ", stdout);
            emit_enum_body(uty);
            fputs(" };\n", stdout);
        }
        return;
    }
    default:
        fputs("/* unsupported top-level */\n", stdout);
        return;
    }
}

/* Sea-front cleanup-protocol prelude. Emitted at the top of every
 * translation unit. Identifiers are __SF_-prefixed: ISO C reserves
 * leading-double-underscore for the implementation, which we are.
 *
 * The unused-label pragma is needed because per-var cleanup labels
 * (one per dtor-bearing local) are not always referenced — only the
 * labels that some return/break/continue actually targets are reached
 * via goto; the others are walked only by fall-through. Suppressing
 * the warning is much cheaper than tracking per-label reference
 * counts at codegen time. */
/* ----- typeid sentinels — N4659 §8.2.7 [expr.typeid] -------------
 * Sea-front lowers typeid(T) (and typeid(expr) when expr is not a
 * polymorphic glvalue) to the address of a per-type sentinel char.
 * Tests that compare typeids via == / != become pointer comparisons
 * over those addresses, which is correct for static types.
 *
 * The polymorphic-glvalue case (operand has virtual methods) needs
 * a vtable typeinfo slot — see docs/rtti.md. Currently emit-equivalent
 * to the static type: works for `typeid(x) == typeid(int)` but gives
 * the static-not-dynamic type for `typeid(*virtual_base_ptr)`. Marked
 * TODO; documented in docs/rtti.md as the next RTTI slice.
 *
 * The set is collected by typeid_collect_walk over the TU before
 * emit, then each entry forward-declared at the top of the emitted
 * C. emit_expr for ND_TYPEID resolves to '((const void *)&sym)'. */
#define TYPEID_SYM_BUFLEN 512
typedef struct { char *sym; } TypeidSetEntry;
static TypeidSetEntry *g_typeid_set = NULL;
static int g_typeid_set_n = 0;
static int g_typeid_set_cap = 0;

static const char *typeid_sym_for_type(Type *ty) {
    static char buf[TYPEID_SYM_BUFLEN];
    /* Prefix __sf_typeid_ (not __sf_typeinfo_) so this sentinel
     * doesn't clash with the EH path's hard-coded
     * __sf_typeinfo_int (see emit_prelude — slice-3 throw machinery).
     * The two prefixes will merge once the per-type typeinfo emit
     * lands (docs/rtti.md); for now they're independent. */
    int pos = snprintf(buf, sizeof(buf), "__sf_typeid_");
    if (ty) pos = mangle_type_to_buf(ty, buf, pos, (int)sizeof(buf));
    else { /* fallback for typeid(expr) where the static type is unknown */
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "unknown");
    }
    if (pos < 0 || pos >= (int)sizeof(buf)) pos = (int)sizeof(buf) - 1;
    buf[pos] = 0;
    return buf;
}

static void typeid_set_add(const char *sym) {
    for (int i = 0; i < g_typeid_set_n; i++)
        if (strcmp(g_typeid_set[i].sym, sym) == 0) return;
    if (g_typeid_set_n == g_typeid_set_cap) {
        g_typeid_set_cap = g_typeid_set_cap ? g_typeid_set_cap * 2 : 16;
        g_typeid_set = realloc(g_typeid_set,
            (size_t)g_typeid_set_cap * sizeof(*g_typeid_set));
        if (!g_typeid_set) abort();
    }
    g_typeid_set[g_typeid_set_n].sym = xstrdup(sym);
    g_typeid_set_n++;
}

/* Pick the type whose sentinel a typeid expression should yield.
 * For typeid(type-id) it's stored on the node; for typeid(expr) it's
 * the operand's resolved_type. NULL when neither is set — emit will
 * fall back to "unknown". */
static Type *typeid_resolved_type(Node *n) {
    if (!n || n->kind != ND_TYPEID) return NULL;
    if (n->typeid_.static_type) return n->typeid_.static_type;
    if (n->typeid_.operand) return n->typeid_.operand->resolved_type;
    return NULL;
}

/* Recursively walk the TU collecting typeid sentinel names so we can
 * emit `static const char` definitions for them before any use site.
 * Mirrors the shape of the broader emit-time walks (call args, member
 * inits, control flow) — kept terse since it just descends to find
 * ND_TYPEID, never produces output. */
static void typeid_collect_walk(Node *n) {
    if (!n) return;
    if (n->kind == ND_TYPEID) {
        typeid_set_add(typeid_sym_for_type(typeid_resolved_type(n)));
    }
    switch (n->kind) {
    case ND_BINARY:
        typeid_collect_walk(n->binary.lhs);
        typeid_collect_walk(n->binary.rhs); break;
    case ND_UNARY:    typeid_collect_walk(n->unary.operand); break;
    case ND_POSTFIX:  typeid_collect_walk(n->unary.operand); break;
    case ND_TERNARY:
        typeid_collect_walk(n->ternary.cond);
        typeid_collect_walk(n->ternary.then_);
        typeid_collect_walk(n->ternary.else_); break;
    case ND_COMMA:
        typeid_collect_walk(n->comma.lhs);
        typeid_collect_walk(n->comma.rhs); break;
    case ND_ASSIGN:
        typeid_collect_walk(n->binary.lhs);
        typeid_collect_walk(n->binary.rhs); break;
    case ND_CALL:
        typeid_collect_walk(n->call.callee);
        for (int i = 0; i < n->call.nargs; i++)
            typeid_collect_walk(n->call.args[i]);
        break;
    case ND_MEMBER:    typeid_collect_walk(n->member.obj); break;
    case ND_SUBSCRIPT:
        typeid_collect_walk(n->subscript.base);
        typeid_collect_walk(n->subscript.index); break;
    case ND_CAST:      typeid_collect_walk(n->cast.operand); break;
    case ND_TYPEID:    typeid_collect_walk(n->typeid_.operand); break;
    case ND_VAR_DECL:
        typeid_collect_walk(n->var_decl.init);
        for (int i = 0; i < n->var_decl.ctor_nargs; i++)
            typeid_collect_walk(n->var_decl.ctor_args[i]);
        break;
    case ND_FUNC_DEF:  typeid_collect_walk(n->func.body); break;
    case ND_BLOCK:
        for (int i = 0; i < n->block.nstmts; i++)
            typeid_collect_walk(n->block.stmts[i]);
        break;
    case ND_RETURN:    typeid_collect_walk(n->ret.expr); break;
    case ND_EXPR_STMT: typeid_collect_walk(n->expr_stmt.expr); break;
    case ND_IF:
        typeid_collect_walk(n->if_.init);
        typeid_collect_walk(n->if_.cond);
        typeid_collect_walk(n->if_.then_);
        typeid_collect_walk(n->if_.else_); break;
    case ND_WHILE:
        typeid_collect_walk(n->while_.cond);
        typeid_collect_walk(n->while_.body); break;
    case ND_DO:
        typeid_collect_walk(n->do_.body);
        typeid_collect_walk(n->do_.cond); break;
    case ND_FOR:
        typeid_collect_walk(n->for_.init);
        typeid_collect_walk(n->for_.cond);
        typeid_collect_walk(n->for_.inc);
        typeid_collect_walk(n->for_.body); break;
    case ND_SWITCH:
        typeid_collect_walk(n->switch_.init);
        typeid_collect_walk(n->switch_.expr);
        typeid_collect_walk(n->switch_.body); break;
    case ND_CASE:
        typeid_collect_walk(n->case_.expr);
        typeid_collect_walk(n->case_.stmt); break;
    case ND_DEFAULT:   typeid_collect_walk(n->default_.stmt); break;
    case ND_LABEL:     typeid_collect_walk(n->label.stmt); break;
    case ND_TRY:
        typeid_collect_walk(n->try_.body);
        for (int i = 0; i < n->try_.nhandlers; i++)
            typeid_collect_walk(n->try_.handlers[i]);
        break;
    case ND_HANDLER:   typeid_collect_walk(n->handler.body); break;
    case ND_THROW:     typeid_collect_walk(n->throw_.operand); break;
    case ND_CLASS_DEF:
        for (int i = 0; i < n->class_def.nmembers; i++)
            typeid_collect_walk(n->class_def.members[i]);
        break;
    case ND_TRANSLATION_UNIT:
        for (int i = 0; i < n->tu.ndecls; i++)
            typeid_collect_walk(n->tu.decls[i]);
        break;
    default: break;
    }
}

static void emit_typeid_sentinels(void) {
    if (g_typeid_set_n == 0) return;
    fputs("\n/* typeid sentinels — N4659 §8.2.7 [expr.typeid] */\n",
          stdout);
    for (int i = 0; i < g_typeid_set_n; i++)
        fprintf(stdout, "static const char %s = 0;\n",
                g_typeid_set[i].sym);
    fputc('\n', stdout);
}

static void emit_prelude(void) {
    fputs("/* generated by sea-front --emit-c */\n", stdout);
    fputs("#include <stdint.h>\n", stdout);
    fputs("#include <stddef.h>\n", stdout);  /* wchar_t, size_t, NULL */
    fputs("\n", stdout);
    fputs("/* sea-front cleanup protocol — see emit_c.c.\n", stdout);
    fputs(" * The goto-chain destructor cleanup (N4659 §15.4 [class.dtor]/9)\n",
          stdout);
    fputs(" * emits __SF_cleanup_N labels structurally; not all paths\n", stdout);
    fputs(" * reach every label, triggering -Wunused-label under -Wall. */\n",
          stdout);
    fputs("#if defined(__GNUC__) || defined(__clang__)\n", stdout);
    fputs("#  pragma GCC diagnostic ignored \"-Wunused-label\"\n", stdout);
    fputs("#endif\n", stdout);
    fputs("typedef enum {\n", stdout);
    fputs("    __SF_UNWIND_NONE   = 0,\n", stdout);
    fputs("    __SF_UNWIND_RETURN = 1,\n", stdout);
    fputs("    __SF_UNWIND_BREAK  = 2,\n", stdout);
    fputs("    __SF_UNWIND_CONT   = 3,\n", stdout);
    /* Exception-throw unwind state — N4659 §18 [except], lowering
     * detail in docs/exceptions.md. The chain machinery treats THROW
     * the same as RETURN/BREAK/CONT; the difference is at try-block
     * landing pads (see __SF_CHAIN_THROW below). */
    fputs("    __SF_UNWIND_THROW  = 4,\n", stdout);
    fputs("} __SF_unwind_t;\n", stdout);
    /* Rewrite macros — set the unwind state and jump to the
     * innermost cleanup label (which then chains outward). */
    fputs("#define __SF_RETURN(v, lbl) "
          "do { __SF_retval = (v); __SF_unwind = __SF_UNWIND_RETURN; "
          "goto lbl; } while (0)\n",
          stdout);
    fputs("#define __SF_RETURN_VOID(lbl) "
          "do { __SF_unwind = __SF_UNWIND_RETURN; goto lbl; } while (0)\n",
          stdout);
    fputs("#define __SF_BREAK(lbl) "
          "do { __SF_unwind = __SF_UNWIND_BREAK; goto lbl; } while (0)\n",
          stdout);
    fputs("#define __SF_CONT(lbl) "
          "do { __SF_unwind = __SF_UNWIND_CONT; goto lbl; } while (0)\n",
          stdout);
    /* Function prologue/epilogue. PROLOGUE declares __SF_retval and
     * __SF_unwind for functions whose body has any non-trivial local;
     * the void variant skips __SF_retval. EPILOGUE emits the
     * __SF_epilogue: label and the actual C return. No do-while
     * wrapping — these emit declarations/labels, which can't sit
     * inside a statement expression. */
    /* '= {0}' is the universal C initializer — works for scalars
     * AND structs/unions/arrays. The bare '= 0' we used initially
     * blew up for struct return types. */
    fputs("#define __SF_PROLOGUE(ret_type) "
          "ret_type __SF_retval = {0}; "
          "__SF_unwind_t __SF_unwind = __SF_UNWIND_NONE; "
          "(void)__SF_unwind\n",
          stdout);
    fputs("#define __SF_PROLOGUE_VOID "
          "__SF_unwind_t __SF_unwind = __SF_UNWIND_NONE; "
          "(void)__SF_unwind\n",
          stdout);
    fputs("#define __SF_EPILOGUE __SF_epilogue: ; return __SF_retval\n", stdout);
    fputs("#define __SF_EPILOGUE_VOID __SF_epilogue: ; return\n", stdout);
    /* Chain macros — used at the bottom of each block-with-cleanups
     * after the dtor calls. ANY collapses to a single check when all
     * three unwind kinds target the same place; otherwise the three
     * per-kind macros emit a list of intentions, one per line. */
    fputs("#define __SF_CHAIN_ANY(lbl) "
          "do { if (__SF_unwind) goto lbl; } while (0)\n",
          stdout);
    fputs("#define __SF_CHAIN_RETURN(lbl) "
          "do { if (__SF_unwind == __SF_UNWIND_RETURN) goto lbl; } while (0)\n",
          stdout);
    fputs("#define __SF_CHAIN_BREAK(lbl) "
          "do { if (__SF_unwind == __SF_UNWIND_BREAK)  goto lbl; } while (0)\n",
          stdout);
    fputs("#define __SF_CHAIN_CONT(lbl) "
          "do { if (__SF_unwind == __SF_UNWIND_CONT)   goto lbl; } while (0)\n",
          stdout);
    /* __SF_CHAIN_THROW reads the TLS slot, not the function-local
     * __SF_unwind. The other CHAIN macros above are intra-function
     * (return/break/cont don't cross call boundaries), but
     * exception propagation must survive a function call: the
     * callee's local __SF_unwind is invisible to the caller, so the
     * cross-function handshake rides on __sf_exc_state.state.
     * __SF_THROW_PRIM keeps both in sync. */
    fputs("#define __SF_CHAIN_THROW(lbl) "
          "do { if (__sf_exc_state.state == __SF_UNWIND_THROW) goto lbl; } "
          "while (0)\n",
          stdout);

    /* Exception-handling runtime — see docs/exceptions.md (TLS
     * polling). Slice 2 emits the declarations only; lowering of
     * 'throw' / 'catch' against this state lands in slices 3+. The
     * exception-state instance is currently a plain TU-scope global;
     * cross-TU propagation (slice 5) will move the storage to a
     * single shared definition (e.g. via a small runtime .c).
     *
     * Layout matches docs/exceptions.md: state piggybacks on the
     * existing __SF_unwind chain (see __SF_unwind_t above) — the
     * dedicated `state` field is reserved for cross-function
     * exception scenarios where a separate slot is more convenient
     * than threading __SF_unwind through every call. */
    fputs("struct __sf_type_info {\n", stdout);
    fputs("    const char *name;\n", stdout);
    /* Single-inheritance fast path — parent chain walk for
     * catch-by-base. NULL for primitives and root classes. */
    fputs("    const struct __sf_type_info *parent;\n", stdout);
    /* Multi-inheritance bases: deferred (see docs/rtti.md). The
     * field is reserved so the layout stays stable when MI ships. */
    fputs("    int n_parents;\n", stdout);
    fputs("    const void *parents;\n", stdout);
    fputs("};\n", stdout);
    fputs("struct __sf_exception_state {\n", stdout);
    fputs("    __SF_unwind_t state;\n", stdout);
    /* Heap-allocated thrown object for class-type throws; for
     * primitive throws (slice 3 first cut) this slot is reused as
     * inline storage via uintptr_t cast (positive int up to one
     * pointer-width). The eventual heap pathway lands when class-
     * type throws ship. */
    fputs("    void *exc_obj;\n", stdout);
    fputs("    const struct __sf_type_info *exc_type;\n", stdout);
    fputs("    void (*exc_dtor)(void *);\n", stdout);
    fputs("};\n", stdout);
    /* C11 thread-local storage. Standard rather than __thread so
     * cproc / non-GNU C compilers stay supported. Single-threaded
     * targets see this as a regular global with no extra cost.
     *
     * Cross-TU sharing via __attribute__((weak)): every TU declares-
     * and-defines __sf_exc_state, but the weak attribute lets the
     * linker collapse all definitions into a single shared symbol.
     * Required for cross-function-cross-TU exception propagation
     * (a throw in TU A's function reaches a catch in TU B's main).
     * Without this, each TU has its own static slot and the throw
     * is invisible to other TUs' chain-checks. N4659 §18 [except].
     * Pattern: g++.dg/eh/weak1.C. */
    fputs("#if __STDC_VERSION__ >= 201112L\n", stdout);
    fputs("__attribute__((weak)) _Thread_local "
          "struct __sf_exception_state __sf_exc_state;\n", stdout);
    fputs("#elif defined(__GNUC__) || defined(__clang__)\n", stdout);
    fputs("__attribute__((weak)) __thread "
          "struct __sf_exception_state __sf_exc_state;\n", stdout);
    fputs("#else\n", stdout);
    fputs("__attribute__((weak)) "
          "struct __sf_exception_state __sf_exc_state;\n", stdout);
    fputs("#endif\n", stdout);
    /* Per-primitive type_info — pointer-compared at catch-dispatch
     * (slice 4). Only the primitives we actually throw need a slot;
     * a single 'int' instance is sufficient for slice 3 since
     * unmatched throws aren't caught yet. Future slices will emit
     * one per primitive type encountered as a throw operand. */
    fputs("static const struct __sf_type_info __sf_typeinfo_int = "
          "{ \"int\", 0, 0, 0 };\n",
          stdout);
    /* __SF_THROW_PRIM — set the TLS exception state and jump to
     * the innermost cleanup label, mirroring __SF_RETURN's shape.
     * For primitive integral throws the value fits in a pointer-
     * width slot via uintptr_t cast; class-type throws will land in
     * a later slice with a heap allocation. The exc_dtor slot is
     * NULL because primitives have no destructor. */
    fputs("#define __SF_THROW_PRIM(val, ti, lbl) "
          "do { __sf_exc_state.exc_obj = (void *)(uintptr_t)(val); "
          "__sf_exc_state.exc_type = (ti); "
          "__sf_exc_state.exc_dtor = 0; "
          "__sf_exc_state.state = __SF_UNWIND_THROW; "
          "__SF_unwind = __SF_UNWIND_THROW; "
          "goto lbl; } while (0)\n",
          stdout);
    /* __SF_THROW_CLASS — class-type throw. The operand is a pointer
     * to the exception object (compound literal '&(struct X){...}'
     * at the call site); we stash that pointer in exc_obj without
     * the uintptr_t cast that breaks for struct values. Type-id is
     * a placeholder for now (no class typeinfo registry yet); catch
     * dispatch on class types is part of EH phase 2+. */
    fputs("#define __SF_THROW_CLASS(p_val, ti, lbl) "
          "do { __sf_exc_state.exc_obj = (void *)(p_val); "
          "__sf_exc_state.exc_type = (ti); "
          "__sf_exc_state.exc_dtor = 0; "
          "__sf_exc_state.state = __SF_UNWIND_THROW; "
          "__SF_unwind = __SF_UNWIND_THROW; "
          "goto lbl; } while (0)\n",
          stdout);
    /* __sf_terminate — invoked at the epilogue of a no-throw function
     * when an exception is in flight, N4659 §18.4 [except.spec]. When
     * libstdc++ is linked, dispatches to std::terminate (which
     * respects std::set_terminate). When the binary doesn't link
     * libstdc++ — sea-front targets bare C too — the weak symbol
     * resolves to NULL and we fall back to abort(). */
    fputs("extern void abort(void);\n", stdout);
    fputs("extern void _ZSt9terminatev(void) __attribute__((weak));\n",
          stdout);
    fputs("static inline void __sf_terminate(void) {\n"
          "    if (_ZSt9terminatev) _ZSt9terminatev();\n"
          "    abort();\n"
          "}\n", stdout);
    /* __SF_INLINE — multi-TU dedup for inline-eligible functions
     * (in-class methods, synthesized ctor/dtor wrappers, dtor body
     * functions, eventually template instantiations).
     *
     * 'static inline' — each TU gets its own private copy. The C
     * compiler can drop the body if it's not called from this TU,
     * which matters when a header-defined inline calls a function
     * that isn't linked into THIS executable (e.g. gcc 4.8's
     * dump_bitmap inline calls bitmap_print which lives in libbackend
     * but isn't linked into the small gen-tools). Per-TU dead-code
     * elimination drops the unused body; the call site never resolves
     * the unused-target's external refs.
     *
     * Cross-TU dedup is automatic: two TUs that both call the same
     * inline each get their own static copy — no link-time collision
     * because there is no external symbol. See
     * docs/inline_and_dedup.md for the full trade-off analysis. */
    fputs("#define __SF_INLINE static inline\n", stdout);
    fputs("\n", stdout);
}

/* Recursively emit forward declarations for all struct/union types
 * in the TU so ordering between classes (and template instantiations)
 * doesn't matter. */
/* Phase A — struct/union predecls only. Walks nested namespaces and
 * descends into full specializations. */
static void emit_fwd_decl_structs_only(Node *n) {
    if (!n) return;
    switch (n->kind) {
    case ND_CLASS_DEF: {
        Type *ty = n->class_def.ty;
        if (ty && ty->tag) {
            fputs(ty->kind == TY_UNION ? "union " : "struct ", stdout);
            emit_mangled_class_tag(ty);
            fputs(";\n", stdout);
        }
        break;
    }
    case ND_TYPEDEF: {
        /* 'typedef struct X { ... } Y;' — predeclare the struct tag
         * so method forward declarations that reference 'struct X*'
         * as a param type land in file scope, not in the param list. */
        Type *uty = n->var_decl.ty;
        while (ty_is_indirect(uty) && uty->base) uty = uty->base;
        if (uty && (uty->kind == TY_STRUCT || uty->kind == TY_UNION) &&
            uty->tag) {
            fputs(uty->kind == TY_UNION ? "union " : "struct ", stdout);
            emit_mangled_class_tag(uty);
            fputs(";\n", stdout);
        }
        break;
    }
    case ND_BLOCK:
        for (int i = 0; i < n->block.nstmts; i++)
            emit_fwd_decl_structs_only(n->block.stmts[i]);
        break;
    case ND_TEMPLATE_DECL:
        /* Full specializations (nparams == 0) are concrete — forward-declare.
         * N4659 §17.7.3 [temp.expl.spec]. */
        if (n->template_decl.nparams == 0 && n->template_decl.decl)
            emit_fwd_decl_structs_only(n->template_decl.decl);
        break;
    default:
        break;
    }
}

/* Phase B — method forward declarations for instantiated member
 * templates. Must run AFTER all struct predecls so parameter lists
 * that reference other class tags resolve to the file-scope type
 * rather than creating a param-list-scoped implicit type. */
static void emit_fwd_decl_methods_only(Node *n) {
    if (!n) return;
    switch (n->kind) {
    case ND_BLOCK:
        for (int i = 0; i < n->block.nstmts; i++)
            emit_fwd_decl_methods_only(n->block.stmts[i]);
        break;
    case ND_TEMPLATE_DECL:
        if (n->template_decl.nparams == 0 && n->template_decl.decl)
            emit_fwd_decl_methods_only(n->template_decl.decl);
        break;
    case ND_FUNC_DEF:
        /* Forward-declare every function definition so that file-
         * scope var-decl initializers (e.g. an array of function
         * pointers) referencing later-defined functions resolve
         * cleanly. Source-order emission defers function bodies to
         * pass 2; forward decls upfront make the names visible to
         * pass 1's var-decl initializers.
         *
         * Three shapes:
         *   - OOL method definition (class_type set): emit the
         *     mangled method signature.
         *   - Instantiated function template (mangled '_t_..._te_'
         *     suffix): emit the free-function header.
         *   - Plain user-written free function: emit the free-
         *     function header. */
        if (n->func.class_type && n->func.name) {
            /* Forward decl matches the actual OOL definition's
             * linkage: inline if the user wrote 'inline' OR the owner
             * is a template instantiation (multi-TU body needs static
             * inline for the same dedup story); otherwise strong. */
            bool ool_inline = (n->func.storage_flags & DECL_INLINE) != 0
                || (n->func.class_type
                    && n->func.class_type->n_template_args > 0);
            emit_method_signature(n, n->func.class_type, ool_inline);
            fputs(";\n", stdout);
        } else if (n->func.name && n->func.body) {
            /* constexpr fns are implicitly inline (§10.1.5/1) — the
             * forward decl needs to match the def's linkage. */
            int sf = n->func.storage_flags;
            if (sf & DECL_CONSTEXPR) sf |= DECL_INLINE;
            emit_storage_flags_for_def(sf);
            emit_free_func_header(n->func.ret_ty, n->func.name,
                                  n->func.params, n->func.nparams,
                                  n->func.is_variadic);
            fputs(";\n", stdout);
        }
        break;
    default:
        break;
    }
}

static void emit_forward_decl_structs(Node *tu) {
    for (int i = 0; i < tu->tu.ndecls; i++)
        emit_fwd_decl_structs_only(tu->tu.decls[i]);
}

void emit_c(Node *tu) {
    if (!tu || tu->kind != ND_TRANSLATION_UNIT) return;
    g_tu = tu;
    /* Pre-scan: names with 2+ free-function declarations get
     * signature-mangled C symbols so each overload has a unique
     * symbol at link time. */
    free_ovld_populate(tu);
    emit_prelude();
    typeid_collect_walk(tu);
    emit_typeid_sentinels();

    /* Forward-declare ALL struct types so pointer references
     * resolve regardless of definition order. Function forward
     * decls happen in source order during pass 1 — they can use
     * enum-by-tag in their signatures, so they have to come AFTER
     * the relevant enum body. */
    emit_forward_decl_structs(tu);

    /* Source-order architecture (replacing the older 'all enums
     * first' pre-pass + tentative-array forward-decl heuristics):
     *
     * Pass 1 (PHASE_STRUCTS): walk tu->tu.decls in source order
     *   and emit struct bodies, typedefs, enum bodies, and var
     *   definitions (with their initializers). emit_class_def
     *   in PHASE_STRUCTS emits the struct body + method forward
     *   decls but skips method bodies. Free function definitions
     *   are skipped here — deferred to pass 2.
     *
     * Pass 2 (PHASE_METHODS): walk tu->tu.decls in source order
     *   and emit method bodies + vtables + synthesized ctors/dtors
     *   (via emit_class_def's PHASE_METHODS branch) + free function
     *   definitions. Var-decls already emitted in pass 1 are
     *   skipped via Node.codegen_emitted.
     *
     * Why source order works:
     *   The source is required to be valid C/C++, which mandates
     *   name-before-use at file scope (with the standard exceptions
     *   for tag/forward-decl/extern). Preserving that order in
     *   emission preserves correctness without us needing
     *   topological-sort heuristics. Enums precede structs that
     *   use their constants because the source already does so;
     *   arrays precede the enums that sizeof them for the same
     *   reason. The c-pch.c shape
     *     static const struct E ... arr[] = ...;
     *     enum { N = sizeof(arr)/sizeof(arr[0]) };
     *   compiles because the source already orders them this way.
     *
     * Why bodies-last works:
     *   Function/method bodies can reference any type by name; if
     *   we emit ALL bodies after ALL type definitions, every
     *   referenced type is complete. This subsumes the earlier
     *   two-phase struct-body / method-body split while letting
     *   non-body decls (enums, vars) interleave with struct
     *   bodies in source order. */
    g_emit_phase = PHASE_STRUCTS;
    for (int i = 0; i < tu->tu.ndecls; i++) {
        Node *n = tu->tu.decls[i];
        if (!n) continue;
        /* emit_top_level itself skips ND_FUNC_DEF in PHASE_STRUCTS
         * (covers nesting through ND_TEMPLATE_DECL / ND_BLOCK). */
        fputc('\n', stdout);
        emit_top_level(n);
    }

    g_emit_phase = PHASE_METHODS;
    for (int i = 0; i < tu->tu.ndecls; i++) {
        fputc('\n', stdout);
        emit_top_level(tu->tu.decls[i]);
    }
    g_emit_phase = 0;

    /* File-scope dynamic initialisation: C++ runs class-type globals
     * through their ctor before main; C requires constant initialisers
     * and has no equivalent. Synthesize a single constructor function
     * '__sf_global_init' (attribute-tagged so the linker runs it
     * pre-main) that:
     *   - calls the default ctor on each class-typed file-scope variable
     *   - assigns any global var whose initializer is non-constant from
     *     C's perspective (function call, deref of a non-const-folded
     *     variable, etc.) to the runtime-evaluated value
     * in declaration order. N4659 §6.6.2 [basic.start.init]. */
    bool need_global_init = false;
    for (int i = 0; i < tu->tu.ndecls; i++) {
        Node *n = tu->tu.decls[i];
        if (!n || n->kind != ND_VAR_DECL) continue;
        if (!n->var_decl.ty || !n->var_decl.name) continue;
        if (n->var_decl.storage_flags & DECL_EXTERN) continue;
        Type *ty = n->var_decl.ty;
        if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) &&
            ty->has_default_ctor) {
            need_global_init = true;
            break;
        }
        /* Direct-init at file scope ('A a(42);') — has ctor_nargs > 0
         * even if the class lacks a default ctor. Each such global
         * needs its ctor to run at startup with the user args. */
        if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) &&
            n->var_decl.has_ctor_init && n->var_decl.ctor_nargs > 0) {
            need_global_init = true;
            break;
        }
        /* Array of class type with default ctor — each element needs
         * the ctor to run at startup. Walk through TY_ARRAY layers. */
        if (ty->kind == TY_ARRAY) {
            Type *elem = ty;
            while (elem && elem->kind == TY_ARRAY) elem = elem->base;
            if (elem && (elem->kind == TY_STRUCT || elem->kind == TY_UNION) &&
                elem->has_default_ctor) {
                need_global_init = true;
                break;
            }
        }
        if (n->var_decl.init && n->var_decl.deferred_to_global_init) {
            need_global_init = true;
            break;
        }
    }
    if (need_global_init) {
        /* Per-var prioritized init — N4659 doesn't model this; GNU's
         * __attribute__((init_priority(N))) controls cross-TU global-
         * ctor execution order (lower N first). Sea-front lowers to C
         * which doesn't honour init_priority on variable decls (cc
         * warns 'init_priority attribute directive ignored'), so we
         * synthesize a separate __sf_init_pri_<idx>(void)
         * __attribute__((constructor(N))) per prioritized var and
         * skip the var in __sf_global_init's main loop.
         * Pattern: g++.dg/special/conpr-{2,3,4}.C. */
        for (int i = 0; i < tu->tu.ndecls; i++) {
            Node *n = tu->tu.decls[i];
            if (!n || n->kind != ND_VAR_DECL) continue;
            if (!n->var_decl.ty || !n->var_decl.name) continue;
            if (n->var_decl.storage_flags & DECL_EXTERN) continue;
            if (n->var_decl.init_priority <= 0) continue;
            Type *ty = n->var_decl.ty;
            if (ty->kind != TY_STRUCT && ty->kind != TY_UNION) continue;
            if (!ty->has_default_ctor) continue;
            fprintf(stdout,
                "\nstatic void __sf_init_pri_%d_%.*s(void) "
                "__attribute__((constructor(%d)));\n",
                i, n->var_decl.name->len, n->var_decl.name->loc,
                n->var_decl.init_priority);
            fprintf(stdout,
                "static void __sf_init_pri_%d_%.*s(void) {\n    ",
                i, n->var_decl.name->len, n->var_decl.name->loc);
            mangle_class_ctor(ty, NULL, 0);
            fprintf(stdout, "(&%.*s);\n}\n",
                    n->var_decl.name->len, n->var_decl.name->loc);
        }
        fputs("\nstatic void __sf_global_init(void) __attribute__((constructor));\n",
              stdout);
        fputs("static void __sf_global_init(void) {\n", stdout);
        for (int i = 0; i < tu->tu.ndecls; i++) {
            Node *n = tu->tu.decls[i];
            if (!n || n->kind != ND_VAR_DECL) continue;
            if (!n->var_decl.ty || !n->var_decl.name) continue;
            if (n->var_decl.storage_flags & DECL_EXTERN) continue;
            /* Prioritized vars are handled by per-var ctor functions
             * above; skip them here. */
            if (n->var_decl.init_priority > 0) continue;
            Type *ty = n->var_decl.ty;
            bool is_class = (ty->kind == TY_STRUCT || ty->kind == TY_UNION) &&
                            ty->has_default_ctor &&
                            !(n->var_decl.has_ctor_init && n->var_decl.ctor_nargs > 0);
            bool is_class_direct_init =
                (ty->kind == TY_STRUCT || ty->kind == TY_UNION) &&
                n->var_decl.has_ctor_init && n->var_decl.ctor_nargs > 0;
            /* Single-dim array of class — emit a loop calling the
             * element ctor. Multi-dim arrays are skipped for now
             * (their TY_ARRAY chain would need recursive unrolling). */
            bool is_array_of_class = false;
            int  arr_size = 0;
            Type *elem_ty = NULL;
            if (ty->kind == TY_ARRAY && ty->base &&
                (ty->base->kind == TY_STRUCT || ty->base->kind == TY_UNION) &&
                ty->base->has_default_ctor) {
                is_array_of_class = true;
                arr_size = ty->array_len;
                elem_ty = ty->base;
            }
            bool deferred = n->var_decl.init && n->var_decl.deferred_to_global_init;
            if (!is_class && !is_class_direct_init && !is_array_of_class && !deferred)
                continue;
            if (is_class) {
                fputs("    ", stdout);
                mangle_class_ctor(ty, NULL, 0);
                fprintf(stdout, "(&%.*s);\n",
                        n->var_decl.name->len, n->var_decl.name->loc);
            }
            if (is_class_direct_init) {
                fputs("    ", stdout);
                Type **at = NULL;
                int na = collect_call_arg_types(n->var_decl.ctor_args,
                                                 n->var_decl.ctor_nargs, &at);
                Type **pty = NULL;
                Node *resolved_ctor = NULL;
                int np = resolve_overload(ty, /*name=*/NULL,
                                           /*is_ctor=*/true, at, na,
                                           /*receiver_is_const=*/false,
                                           &pty, &resolved_ctor);
                if (np < 0) {
                    /* No matching ctor — emit a comment placeholder and
                     * skip rather than producing broken C. */
                    fprintf(stdout, "/* sf: no ctor match for %.*s direct-init */;\n",
                            n->var_decl.name->len, n->var_decl.name->loc);
                } else {
                    mangle_class_ctor(ty, pty, np);
                    /* Cast away const on the receiver: C++ allows
                     * const-qualified globals like 'const T x(...)' to
                     * run their ctor on the storage at init time;
                     * sea-front's __sf_global_init runs after the var
                     * is declared const, so the ctor signature ('T*')
                     * conflicts with '&const_var'. Cast through to a
                     * non-const receiver. */
                    fputs("(", stdout);
                    fputs("(", stdout);
                    if (ty->kind == TY_UNION) fputs("union ", stdout);
                    else fputs("struct ", stdout);
                    mangle_class_tag(ty);
                    fputs(" *)&", stdout);
                    fprintf(stdout, "%.*s",
                            n->var_decl.name->len, n->var_decl.name->loc);
                    for (int j = 0; j < n->var_decl.ctor_nargs; j++) {
                        fputs(", ", stdout);
                        emit_arg_for_param(n->var_decl.ctor_args[j],
                                            j < np ? pty[j] : NULL);
                    }
                    emit_default_args_tail(resolved_ctor,
                                           n->var_decl.ctor_nargs, np, pty);
                    fputs(");\n", stdout);
                }
            }
            if (is_array_of_class && arr_size > 0) {
                fprintf(stdout,
                        "    for (int __sf_i = 0; __sf_i < %d; ++__sf_i) ",
                        arr_size);
                mangle_class_ctor(elem_ty, NULL, 0);
                fprintf(stdout, "(&%.*s[__sf_i]);\n",
                        n->var_decl.name->len, n->var_decl.name->loc);
            }
            if (deferred) {
                fputs("    ", stdout);
                fprintf(stdout, "%.*s = ",
                        n->var_decl.name->len, n->var_decl.name->loc);
                emit_init_with_target(n->var_decl.init, n->var_decl.ty);
                fputs(";\n", stdout);
            }
        }
        fputs("}\n", stdout);
    }
}
