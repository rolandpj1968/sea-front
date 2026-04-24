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
#include <string.h>
#include "emit_c.h"
#include "mangle.h"
#include "../sea-front.h"

bool g_emit_line_directives = true;

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
/* Class type of the OOL method currently being emitted. Used by
 * ND_OFFSETOF to substitute unresolvable local typedefs. */
static Type *g_current_method_class = NULL;
/* Const-qualification of the current method. Used to pick the right
 * overload when an unqualified method call inside a const method
 * invokes another method — N4659 §16.3.1.4 [over.match.funcs]/4
 * says the implicit 'this' is 'const C*' when the enclosing method
 * is const, which biases overload resolution toward const overloads. */
static bool g_current_method_is_const = false;

/* Translation unit root — set by emit_c at entry, consulted by
 * helpers that need to find ND_CLASS_DEF nodes by tag+template_args
 * (template-instantiated method bodies sometimes carry Type copies
 * with class_def=NULL and we need the real body for overload
 * resolution / const-qualification). */
static Node *g_tu = NULL;

/* Reference-parameter tracking — N4659 §11.3.2 [dcl.ref].
 * C has no references; we lower T& to T* in the C signature.
 * When the body uses a ref-param as a value, codegen must deref it.
 * cf_begin_function populates this table from the func's params;
 * is_ref_param checks it at ident-emission time. */
#define REF_PARAM_CAP 32
static Token *g_ref_params[REF_PARAM_CAP];
static int    g_nref_params = 0;

static bool is_ref_param(Token *name) {
    if (!name) return false;
    for (int i = 0; i < g_nref_params; i++) {
        Token *rp = g_ref_params[i];
        if (rp && rp->len == name->len &&
            memcmp(rp->loc, name->loc, name->len) == 0)
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
} CleanupKind;

typedef struct CleanupEntry {
    CleanupKind kind;
    int         label_id;       /* CL_VAR: cleanup label / CL_LOOP: break_label */
    int         cont_label_id;  /* CL_LOOP only */
    Node       *var_decl;       /* CL_VAR only */
} CleanupEntry;

#define CLEANUP_LIVE_MAX 64

static struct {
    bool          func_has_cleanups;  /* function-wide flag, set by pre-scan */
    int           next_label_id;      /* fresh-id counter */
    CleanupEntry  live[CLEANUP_LIVE_MAX];
    int           nlive;              /* number of live entries currently */
} g_cf;

/* For RETURN: walk outward, skip CL_LOOP markers, find first CL_VAR.
 * Returns label_id, or -1 meaning "jump straight to __SF_epilogue". */
static int find_return_target_from(int top) {
    for (int i = top - 1; i >= 0; i--)
        if (g_cf.live[i].kind == CL_VAR)
            return g_cf.live[i].label_id;
    return -1;
}

/* For BREAK: the topmost live entry IS the next target — both
 * CL_VAR (chain to its label) and CL_LOOP (its break_label) live
 * in the same field. Returns -1 only if break is used outside any
 * loop or var scope (malformed; should already be a sema error). */
static int find_break_target_from(int top) {
    if (top <= 0) return -1;
    return g_cf.live[top - 1].label_id;
}

/* For CONT: same as break, but CL_LOOP resolves to its cont_label
 * instead of its break_label. CL_VAR is unchanged. */
static int find_cont_target_from(int top) {
    if (top <= 0) return -1;
    CleanupEntry *e = &g_cf.live[top - 1];
    return (e->kind == CL_VAR) ? e->label_id : e->cont_label_id;
}

/* Top-of-stack views for ND_RETURN/BREAK/CONTINUE rewrite sites. */
static int return_target(void) { return find_return_target_from(g_cf.nlive); }
static int break_target(void)  { return find_break_target_from(g_cf.nlive); }
static int cont_target(void)   { return find_cont_target_from(g_cf.nlive); }

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
static void emit_mangled_class_tag(Type *class_type);
static void emit_stmt(Node *n);
static int collect_call_arg_types(Node **args, int nargs, Type ***out_types);
static int resolve_overload(Type *class_type, Token *name, bool is_ctor,
                             Type **arg_types, int nargs,
                             bool receiver_is_const,
                             Type ***out_param_types,
                             Node **out_best);
static const char *operator_suffix_for_name(Token *name);
static int resolve_operator_overload(Type *class_type,
                                      const char *op_suffix,
                                      Type **arg_types, int nargs,
                                      bool receiver_is_const,
                                      Type ***out_param_types,
                                      Node **out_best);

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

static void emit_arg_for_param(Node *arg, Type *param_ty) {
    if (!arg) return;
    if (!ty_is_ref(param_ty)) { emit_expr(arg); return; }
    Type *at = arg->resolved_type;
    /* Already a ref in the AST (lowered to pointer) — pass as-is.
     * Suppress ref-param deref: if the arg is an ND_IDENT naming a
     * ref-param, we want the pointer value, not its dereference. */
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
        /* Fall back to the ref param's base type if arg has no
         * resolved_type — happens for ND_NULLPTR / ND_NUM literals
         * whose sema-phase type isn't recorded but the call-site
         * context tells us exactly what type they'll be converted
         * to. Using the expected type keeps the compound literal
         * valid. */
        Type *lit_ty = at;
        if (!lit_ty && ty_is_ref(param_ty)) lit_ty = param_ty->base;
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

/* Emit a mangled parameter-type suffix used for overload
 * disambiguation on operator methods and inline operator-call
 * rewrites. Mirrors mangle.c's emit_type_for_mangle shape but
 * lives in emit_c.c so call-site rewrites that don't go through
 * the mangle_class_method helper can share it. The two encoders
 * MUST stay in sync. */
static void emit_local_param_suffix(Type **param_types, int nparams) {
    fputs("_p_", stdout);
    if (nparams == 0) {
        fputs("void", stdout);
    } else {
        for (int i = 0; i < nparams; i++) {
            if (i > 0) fputc('_', stdout);
            Type *pt = param_types[i];
            if (!pt) { fputs("unknown", stdout); continue; }
            switch (pt->kind) {
            case TY_VOID:    fputs("void", stdout); break;
            case TY_BOOL:    fputs("bool", stdout); break;
            case TY_CHAR:    fputs(pt->is_unsigned ? "uchar" : "char", stdout); break;
            case TY_SHORT:   fputs(pt->is_unsigned ? "ushort" : "short", stdout); break;
            case TY_INT:     fputs(pt->is_unsigned ? "uint" : "int", stdout); break;
            case TY_LONG:    fputs(pt->is_unsigned ? "ulong" : "long", stdout); break;
            case TY_LLONG:   fputs(pt->is_unsigned ? "ullong" : "llong", stdout); break;
            case TY_FLOAT:   fputs("float", stdout); break;
            case TY_DOUBLE:  fputs("double", stdout); break;
            case TY_PTR:     fputs("ptr", stdout); break;
            case TY_REF:     fputs("ref", stdout); break;
            case TY_STRUCT: case TY_UNION:
                if (pt->tag) fprintf(stdout, "%.*s", pt->tag->len, pt->tag->loc);
                else fputs("anon", stdout);
                break;
            default: fputs("unknown", stdout); break;
            }
        }
    }
    fputs("_pe_", stdout);
}

/* Emit template-arg suffix (_t_<type>_te_) from an ND_TEMPLATE_ID node.
 * Used when a qualified call's leading part has explicit template args
 * (e.g. Box<int>::test → sf__Box_t_int_te___test_p_void_pe_). */
static void emit_template_id_suffix(Node *tid) {
    if (!tid || tid->kind != ND_TEMPLATE_ID || tid->template_id.nargs == 0)
        return;
    fputs("_t_", stdout);
    for (int i = 0; i < tid->template_id.nargs; i++) {
        if (i > 0) fputc('_', stdout);
        Node *arg = tid->template_id.args[i];
        Type *ty = NULL;
        if (arg && arg->kind == ND_VAR_DECL) ty = arg->var_decl.ty;
        if (!ty) { fputs("unknown", stdout); continue; }
        switch (ty->kind) {
        case TY_VOID:    fputs("void", stdout); break;
        case TY_BOOL:    fputs("bool", stdout); break;
        case TY_CHAR:    fputs(ty->is_unsigned ? "uchar" : "char", stdout); break;
        case TY_SHORT:   fputs(ty->is_unsigned ? "ushort" : "short", stdout); break;
        case TY_INT:     fputs(ty->is_unsigned ? "uint" : "int", stdout); break;
        case TY_LONG:    fputs(ty->is_unsigned ? "ulong" : "long", stdout); break;
        case TY_LLONG:   fputs(ty->is_unsigned ? "ullong" : "llong", stdout); break;
        case TY_FLOAT:   fputs("float", stdout); break;
        case TY_DOUBLE:  fputs("double", stdout); break;
        case TY_PTR:     fputs("ptr", stdout); break;
        case TY_STRUCT: case TY_UNION:
            if (ty->tag) fprintf(stdout, "%.*s", ty->tag->len, ty->tag->loc);
            else fputs("anon", stdout);
            break;
        default: fputs("unknown", stdout); break;
        }
    }
    fputs("_te_", stdout);
}

/* Shared diagnostic for the "no matching overload" case. We fail
 * loudly rather than fall through to arg-type mangling — the prior
 * silent fallback would emit a symbol that doesn't resolve at link
 * time, which we've repeatedly diagnosed as a poor tradeoff. */
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

static bool is_class_temp_call(Node *n) {
    if (!n || n->kind != ND_CALL || !n->resolved_type ||
        n->resolved_type->kind != TY_STRUCT || n->codegen_temp_name)
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

static void hoist_emit_decl(Node *call) {
    int id = g_cf.next_label_id++;
    /* The temp name lives in a small static buffer pool — one per
     * temp, since multiple temps can be live in the same statement
     * and codegen_temp_name is just a borrowed pointer. */
    static char name_pool[CLEANUP_LIVE_MAX][24];
    static int name_idx = 0;
    if (name_idx >= CLEANUP_LIVE_MAX) name_idx = 0;  /* wrap */
    char *name = name_pool[name_idx++];
    snprintf(name, 24, "__SF_temp_%d", id);

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
    if (call->call.callee && call->call.callee->kind == ND_IDENT) {
        Declaration *d = call->call.callee->ident.resolved_decl;
        if (d && (d->entity == ENTITY_TYPE || d->entity == ENTITY_TAG))
            is_ctor_call = true;
    }

    if (is_ctor_call) {
        emit_indent();
        emit_type(call->resolved_type);
        fprintf(stdout, " %s;\n", name);
        emit_indent();
        {
            Type **at = NULL;
            int na = collect_call_arg_types(call->call.args,
                                             call->call.nargs, &at);
            Type **pty = NULL;
            int np = resolve_overload(call->resolved_type, NULL, true,
                                       at, na, false, &pty, NULL);
            if (np < 0) {
                /* No matching ctor — skip ctor call. For plain C
                 * structs whose has_default_ctor was transitively
                 * set, this is trivial default construction (the
                 * zero-fill from '{0}' covers it). */
                goto hoist_done;
            }
            mangle_class_ctor(call->resolved_type, pty, np);
        }
        fprintf(stdout, "(&%s", name);
        for (int i = 0; i < call->call.nargs; i++) {
            fputs(", ", stdout);
            emit_expr(call->call.args[i]);
        }
        fputs(");\n", stdout);
    } else {
        /* Function call returning a class — direct copy form. */
        emit_indent();
        emit_type(call->resolved_type);
        fprintf(stdout, " %s = ", name);
        emit_expr(call);  /* call's children may already be substituted */
        fputs(";\n", stdout);
    }

hoist_done:
    /* Tag the call so emit_expr now substitutes the temp name. */
    call->codegen_temp_name = name;

    /* Push as a CL_VAR onto the live stack so the cleanup chain
     * runs the dtor at end of block. Skip the push for trivially-
     * destructible types (no dtor to call) — the temp is still a
     * named local that the rest of the expression uses, but it
     * doesn't need cleanup tracking. */
    if (call->resolved_type && call->resolved_type->has_dtor &&
        g_cf.nlive < CLEANUP_LIVE_MAX) {
        g_cf.live[g_cf.nlive].kind = CL_VAR;
        g_cf.live[g_cf.nlive].label_id = id;
        g_cf.live[g_cf.nlive].cont_label_id = -1;
        g_cf.live[g_cf.nlive].var_decl = call;  /* not an ND_VAR_DECL — flagged via codegen_temp_name */
        g_cf.nlive++;
    }
}

static void hoist_temps_in_expr(Node *n) {
    if (!n) return;
    switch (n->kind) {
    case ND_CALL:
        if (n->call.callee) hoist_temps_in_expr(n->call.callee);
        for (int i = 0; i < n->call.nargs; i++)
            hoist_temps_in_expr(n->call.args[i]);
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
                if (pass_by_ref) hoist_emit_decl(arg);
            }
        }
        if (is_class_temp_call(n)) hoist_emit_decl(n);
        return;
    case ND_BINARY:
    case ND_ASSIGN:
        /* Both share the 'binary' member layout. */
        hoist_temps_in_expr(n->binary.lhs);
        hoist_temps_in_expr(n->binary.rhs);
        /* Struct-typed lhs of a binary/compound-assign op dispatches
         * through an overloaded operator: 'a == b' → 'sf__T__eq(&a, b)'.
         * If lhs is an rvalue call 'f() == b', the emitted '&f()' is
         * illegal C. Force-hoist same as the ND_MEMBER case below.
         * Pattern: gcc 4.8 cgraph.c cgraph_add_thunk
         *   tree_to_double_int(virtual_offset) == double_int::from_shwi(...) */
        if (n->binary.lhs && n->binary.lhs->kind == ND_CALL &&
            !n->binary.lhs->codegen_temp_name &&
            n->binary.lhs->resolved_type &&
            (n->binary.lhs->resolved_type->kind == TY_STRUCT ||
             n->binary.lhs->resolved_type->kind == TY_UNION))
            hoist_emit_decl(n->binary.lhs);
        return;
    case ND_UNARY:
    case ND_POSTFIX:
        /* Both share the 'unary' member layout. */
        hoist_temps_in_expr(n->unary.operand);
        /* Unary overload on a struct operand: '-x' → 'sf__T__minus(&x)'.
         * Force-hoist an rvalue struct-returning call operand so the
         * emitted '&f()' stays valid. N4659 §16.5 [over.oper]. */
        if (n->unary.operand && n->unary.operand->kind == ND_CALL &&
            !n->unary.operand->codegen_temp_name &&
            n->unary.operand->resolved_type &&
            (n->unary.operand->resolved_type->kind == TY_STRUCT ||
             n->unary.operand->resolved_type->kind == TY_UNION))
            hoist_emit_decl(n->unary.operand);
        return;
    case ND_TERNARY:
        hoist_temps_in_expr(n->ternary.cond);
        hoist_temps_in_expr(n->ternary.then_);
        hoist_temps_in_expr(n->ternary.else_);
        return;
    case ND_MEMBER:
        hoist_temps_in_expr(n->member.obj);
        /* Method dispatch needs an lvalue for '&obj'. If the obj is
         * an rvalue struct-returning call that wasn't already hoisted
         * (e.g. no dtor), force-hoist so '&__SF_temp_N' is valid C.
         * Pattern: 'make_value().method()' or
         * 'double_int::from_shwi(1).lshift(...)'.
         * N4659 §16.3.1.4 [over.match.funcs] — implicit this must
         * bind to an lvalue that the callee can take the address of. */
        if (n->member.obj && n->member.obj->kind == ND_CALL &&
            !n->member.obj->codegen_temp_name &&
            n->member.obj->resolved_type &&
            (n->member.obj->resolved_type->kind == TY_STRUCT ||
             n->member.obj->resolved_type->kind == TY_UNION))
            hoist_emit_decl(n->member.obj);
        return;
    case ND_SUBSCRIPT:
        hoist_temps_in_expr(n->subscript.base);
        hoist_temps_in_expr(n->subscript.index);
        /* Class-type subscript lowers to 'sf__T__subscript(&base, idx)'.
         * Force-hoist an rvalue struct-returning call base so '&f()'
         * stays valid. N4659 §16.5.5 [over.sub]. */
        if (n->subscript.base && n->subscript.base->kind == ND_CALL &&
            !n->subscript.base->codegen_temp_name &&
            n->subscript.base->resolved_type &&
            (n->subscript.base->resolved_type->kind == TY_STRUCT ||
             n->subscript.base->resolved_type->kind == TY_UNION))
            hoist_emit_decl(n->subscript.base);
        return;
    case ND_CAST:
        hoist_temps_in_expr(n->cast.operand);
        return;
    case ND_COMMA:
        hoist_temps_in_expr(n->comma.lhs);
        hoist_temps_in_expr(n->comma.rhs);
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
            if (init->call.callee) hoist_temps_in_expr(init->call.callee);
            for (int i = 0; i < init->call.nargs; i++)
                hoist_temps_in_expr(init->call.args[i]);
        } else {
            hoist_temps_in_expr(init);
        }
        return;
    }
    case ND_EXPR_STMT:
        hoist_temps_in_expr(s->expr_stmt.expr);
        return;
    case ND_RETURN:
        hoist_temps_in_expr(s->ret.expr);
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
typedef struct {
    const char *loc;
    int len;
    bool is_def;
    /* Coarse signature fingerprint: (nparams, first_param_kind,
     * first_param_tag). Used to detect same-name-different-type
     * overloads at the decl↔def boundary.
     *
     * nparams + first_param_kind alone was insufficient when both
     * overloads took a TY_PTR (e.g. gcc 4.8's
     *   extern bool bitmap_bit_p(bitmap_head_def*, int)   [bitmap.h]
     *   static inline ulong bitmap_bit_p(simple_bitmap_def*, int)  [sbitmap.h]
     * ). Record the first param's struct/union TAG too for TY_PTR,
     * so the two overloads stay distinct and we skip the conflicting
     * definition. */
    int nparams;
    int first_param_kind;
    const char *first_param_tag;  /* NULL when not TY_PTR to a tagged struct/union */
    int first_param_tag_len;
} FuncSeen;
static FuncSeen g_func_seen[8192];
static int g_func_nseen = 0;

static int func_first_param_kind(Type *fty) {
    if (!fty || fty->kind != TY_FUNC || fty->nparams < 1) return -1;
    Type *p = fty->params[0];
    return p ? (int)p->kind : -1;
}

/* First param's struct/union tag (for disambiguating TY_PTR overloads
 * like bitmap_bit_p(bitmap_head_def*,int) vs bitmap_bit_p(simple_bitmap_def*,int)).
 * Returns NULL when not a pointer to a tagged struct/union. */
static Token *func_first_param_ptr_tag(Type *fty) {
    if (!fty || fty->kind != TY_FUNC || fty->nparams < 1) return NULL;
    Type *p = fty->params[0];
    if (!p || p->kind != TY_PTR || !p->base) return NULL;
    Type *pointee = p->base;
    if ((pointee->kind == TY_STRUCT || pointee->kind == TY_UNION))
        return pointee->tag;
    return NULL;
}

/* Compare two (tag-pointer, len) fingerprints. NULL matches NULL. */
static bool func_tag_match(const char *a, int a_len,
                            const char *b, int b_len) {
    if (!a && !b) return true;
    if (!a || !b) return false;
    if (a_len != b_len) return false;
    return memcmp(a, b, a_len) == 0;
}

static FuncSeen *func_seen_find(Token *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_func_nseen; i++) {
        if (g_func_seen[i].len == name->len &&
            memcmp(g_func_seen[i].loc, name->loc, name->len) == 0)
            return &g_func_seen[i];
    }
    return NULL;
}

/* Signature-aware variant: find by name + nparams + first_param_kind.
 * Returns NULL if no exact match exists, so overloaded free functions
 * (like union_or_struct_p(enum) vs union_or_struct_p(type_p)) aren't
 * incorrectly deduped. */
static FuncSeen *func_seen_find_sig(Token *name, int nparams,
                                      int first_param_kind,
                                      Token *first_param_tag) {
    if (!name) return NULL;
    const char *tag_loc = first_param_tag ? first_param_tag->loc : NULL;
    int tag_len = first_param_tag ? first_param_tag->len : 0;
    for (int i = 0; i < g_func_nseen; i++) {
        if (g_func_seen[i].len != name->len) continue;
        if (memcmp(g_func_seen[i].loc, name->loc, name->len) != 0) continue;
        if (g_func_seen[i].nparams == nparams &&
            g_func_seen[i].first_param_kind == first_param_kind &&
            func_tag_match(g_func_seen[i].first_param_tag,
                           g_func_seen[i].first_param_tag_len,
                           tag_loc, tag_len))
            return &g_func_seen[i];
    }
    return NULL;
}

/* Both kinds' dedup take (nparams, first_param_kind, first_param_tag)
 * to compare signatures. Callers pass -1/NULL when info is unavailable;
 * that matches any recorded signature (conservative — preserves pre-
 * signature behavior for older call sites). */
static bool func_decl_dedup_check_sig(Token *name, int nparams,
                                       int first_param_kind,
                                       Token *first_param_tag) {
    if (!name) return false;
    if (func_seen_find(name)) return true;
    if (g_func_nseen < 8192) {
        g_func_seen[g_func_nseen].loc = name->loc;
        g_func_seen[g_func_nseen].len = name->len;
        g_func_seen[g_func_nseen].is_def = false;
        g_func_seen[g_func_nseen].nparams = nparams;
        g_func_seen[g_func_nseen].first_param_kind = first_param_kind;
        g_func_seen[g_func_nseen].first_param_tag =
            first_param_tag ? first_param_tag->loc : NULL;
        g_func_seen[g_func_nseen].first_param_tag_len =
            first_param_tag ? first_param_tag->len : 0;
        g_func_nseen++;
    }
    return false;
}

static bool func_def_dedup_check_sig(Token *name, int nparams,
                                      int first_param_kind,
                                      Token *first_param_tag) {
    if (!name) return false;
    const char *tag_loc = first_param_tag ? first_param_tag->loc : NULL;
    int tag_len = first_param_tag ? first_param_tag->len : 0;
    FuncSeen *fs = func_seen_find(name);
    if (fs) {
        if (fs->is_def) return true;
        /* Decl→def upgrade only when signatures match. */
        if (fs->nparams != nparams ||
            fs->first_param_kind != first_param_kind ||
            !func_tag_match(fs->first_param_tag, fs->first_param_tag_len,
                            tag_loc, tag_len))
            return true;
        fs->is_def = true;
        return false;
    }
    if (g_func_nseen < 8192) {
        g_func_seen[g_func_nseen].loc = name->loc;
        g_func_seen[g_func_nseen].len = name->len;
        g_func_seen[g_func_nseen].is_def = true;
        g_func_seen[g_func_nseen].nparams = nparams;
        g_func_seen[g_func_nseen].first_param_kind = first_param_kind;
        g_func_seen[g_func_nseen].first_param_tag = tag_loc;
        g_func_seen[g_func_nseen].first_param_tag_len = tag_len;
        g_func_nseen++;
    }
    return false;
}

/* Does this struct/union (or any of its bases) have member function
 * definitions? A class with at least one ND_FUNC_DEF in its body —
 * or in an ancestor's body — is a C++ class: unqualified TY_FUNC
 * members/inherited names are method declarations. A class with none
 * is a plain C struct from a typedef'd header; TY_FUNC members are
 * function-pointer data fields (the typedef flattened away the
 * pointer level). Both struct emission and call-site method-vs-fptr
 * disambiguation depend on this. */
static bool class_has_method_bodies(Type *class_type) {
    if (!class_type) return false;
    if (class_type->class_def) {
        Node *cd = class_type->class_def;
        for (int i = 0; i < cd->class_def.nmembers; i++) {
            Node *m = cd->class_def.members[i];
            if (m && m->kind == ND_FUNC_DEF) return true;
            /* Pure virtual / pure-decl methods also count. */
            if (m && m->kind == ND_VAR_DECL && m->var_decl.ty &&
                m->var_decl.ty->kind == TY_FUNC &&
                (m->var_decl.is_constructor || m->var_decl.is_destructor ||
                 m->var_decl.is_virtual))
                return true;
        }
    }
    if (class_type->class_region) {
        for (int i = 0; i < class_type->class_region->nbases; i++) {
            DeclarativeRegion *br = class_type->class_region->bases[i];
            if (br && br->owner_type && class_has_method_bodies(br->owner_type))
                return true;
        }
    }
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
            m->func.name->len == method_name->len &&
            memcmp(m->func.name->loc, method_name->loc, method_name->len) == 0)
            return m->func.is_virtual;
        if (m->kind == ND_VAR_DECL && m->var_decl.name &&
            m->var_decl.ty && m->var_decl.ty->kind == TY_FUNC &&
            m->var_decl.name->len == method_name->len &&
            memcmp(m->var_decl.name->loc, method_name->loc, method_name->len) == 0)
            return m->var_decl.is_virtual;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Type emission                                                      */
/* ------------------------------------------------------------------ */

static void emit_type(Type *ty) {
    if (!ty) { fputs("/*?*/ int", stdout); return; }

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
    }

    switch (ty->kind) {
    case TY_VOID:    fputs("void", stdout); return;
    case TY_BOOL:    fputs("_Bool", stdout); return;  /* C spelling */
    case TY_CHAR:    fputs(ty->is_unsigned ? "unsigned char" : "char", stdout); return;
    case TY_CHAR16:  fputs("char16_t", stdout); return;
    case TY_CHAR32:  fputs("char32_t", stdout); return;
    case TY_WCHAR:   fputs("wchar_t", stdout); return;
    case TY_SHORT:   fputs(ty->is_unsigned ? "unsigned short" : "short", stdout); return;
    case TY_INT:     fputs(ty->is_unsigned ? "unsigned int" : "int", stdout); return;
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
                fputc(')', stdout);
                return;
            }
        }
        emit_type(ty->base);
        fputs("*", stdout);
        if (ty->is_const)    fputs(" const", stdout);
        if (ty->is_volatile) fputs(" volatile", stdout);
        return;
    /* References — emit as pointer in C (caller passes &x). */
    case TY_REF:     emit_type(ty->base); fputs("*", stdout); return;
    case TY_RVALREF: emit_type(ty->base); fputs("*", stdout); return;
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
    if (for_definition && is_inline && !is_static && !is_extern) {
        fputs("__attribute__((weak)) ", stdout);
        return;  /* skip the 'inline' keyword — conflicts with weak */
    }
    if (is_extern)  fputs("extern ", stdout);
    if (is_static)  fputs("static ", stdout);
    if (is_inline)  fputs("inline ", stdout);
}

static void emit_storage_flags(int flags) {
    emit_storage_flags_impl(flags, false);
}

static void emit_storage_flags_for_def(int flags) {
    emit_storage_flags_impl(flags, true);
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

/* Match a single candidate member against a call's arg types.
 * Returns -1 if nparams ≠ nargs (hard reject); otherwise a score in
 * [0 .. nparams] counting position-wise type-kind matches.
 * For methods, name must already have been pre-filtered by the caller. */
static int overload_match_score(Node *m, Type **arg_types, int nargs) {
    bool is_def = m->kind == ND_FUNC_DEF;
    int nparams = is_def ? m->func.nparams : m->var_decl.ty->nparams;
    if (nparams != nargs) return -1;
    int score = 0;
    for (int k = 0; k < nparams && k < 64; k++) {
        Type *pt = is_def ? m->func.params[k]->param.ty
                          : m->var_decl.ty->params[k];
        Type *at = arg_types && k < nargs ? arg_types[k] : NULL;
        if (!pt || !at) continue;
        /* Identical kind: best score (e.g. int vs int, struct vs struct
         * with the same tag — the prototypical copy-ctor pattern is
         * scored separately below). */
        if (pt->kind == at->kind) {
            score += 2;
            /* Same struct/union tag → bonus, so 'fpos vs fpos' beats
             * 'anyclass vs fpos' when there's overload ambiguity. */
            if ((pt->kind == TY_STRUCT || pt->kind == TY_UNION) &&
                pt->tag && at->tag &&
                pt->tag->len == at->tag->len &&
                memcmp(pt->tag->loc, at->tag->loc, pt->tag->len) == 0)
                score++;
            continue;
        }
        /* Reference parameter (lowered to T* in C) accepting an arg of
         * the underlying type: this is the copy/move-ctor pattern.
         * 'Box(const Box&)' with arg 'Box' (e.g. '*this') should beat
         * 'Box(int)' with the same arg. N4659 §16.3.3.2.1 [over.ics.rank]. */
        if (ty_is_ref(pt) && pt->base &&
            pt->base->kind == at->kind) {
            score += 2;
            if ((at->kind == TY_STRUCT || at->kind == TY_UNION) &&
                pt->base->tag && at->tag &&
                pt->base->tag->len == at->tag->len &&
                memcmp(pt->base->tag->loc, at->tag->loc, pt->base->tag->len) == 0)
                score++;
        }
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
 * SHORTCUT (ours, not the standard): template_args match by Type*
 * identity — fine because the instantiation pass shares Type* for
 * the same concrete args, but would miss a logically-equal arg
 * constructed from a different pointer. */
static Node *find_class_def_by_tag_args(Type *class_type) {
    if (!g_tu || !class_type || !class_type->tag ||
        class_type->n_template_args <= 0)
        return NULL;
    for (int i = 0; i < g_tu->tu.ndecls; i++) {
        Node *d = g_tu->tu.decls[i];
        if (!d || d->kind != ND_CLASS_DEF) continue;
        Type *t = d->class_def.ty;
        if (same_template_instantiation(t, class_type)) return d;
    }
    return NULL;
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
                                         const char *op_suffix,
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
        const char *s = operator_suffix_for_name(mn);
        if (strcmp(s, op_suffix) != 0) continue;
        if (*nfound < cap) found[(*nfound)++] = m;
    }
    if (class_type->class_region) {
        for (int i = 0; i < class_type->class_region->nbases; i++) {
            DeclarativeRegion *br = class_type->class_region->bases[i];
            if (br && br->owner_type)
                collect_operator_candidates(br->owner_type, op_suffix,
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
                                      const char *op_suffix,
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
    collect_operator_candidates(class_type, op_suffix,
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
static int emit_class_op_mangled_name(Type *class_ty, const char *op_suffix,
                                       Type **args, int nargs,
                                       bool receiver_is_const,
                                       Type ***out_pty,
                                       Node **out_winner) {
    Node *winner = NULL;
    int np = resolve_operator_overload(class_ty, op_suffix, args, nargs,
                                        receiver_is_const, out_pty, &winner);
    mangle_class_tag(class_ty);
    fputs(op_suffix, stdout);
    if (np >= 0) mangle_param_suffix(*out_pty, np);
    if (candidate_is_const(winner)) fputs("_const", stdout);
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
static void emit_func_header(Type *ret_ty, Token *name,
                              Node **params, int nparams, bool variadic) {
    bool ret_is_fptr = ret_ty && ret_ty->kind == TY_PTR &&
                       ret_ty->base && ret_ty->base->kind == TY_FUNC;
    if (ret_is_fptr) {
        Type *fty = ret_ty->base;
        emit_type(fty->ret);
        fputs(" (*", stdout);
        if (name)
            fprintf(stdout, "%.*s", name->len, name->loc);
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
        fputs("))(", stdout);
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
        fputc(')', stdout);
        return;
    }
    emit_type(ret_ty);
    fputc(' ', stdout);
    if (name)
        fprintf(stdout, "%.*s", name->len, name->loc);
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

/* Emit a parameter's 'type name' pair. C interleaves the name with
 * the declarator for function-pointer parameters ('int (*p)(int)'),
 * so we can't just emit_type then name. Arrays also decay to pointer
 * in parameter position (N4659 §11.3.4/5 [dcl.array]); emit_type
 * already does that decay. Unnamed parameters get __sf_unused_N
 * because C requires named params in definitions. */
static void emit_param_declarator(Type *ty, Token *name, int idx) {
    /* N4659 §11.3 [dcl.meaning]: function pointer parameters
     * require declarator-interleaving in C:
     *   void (*name)(int)     — pointer to function
     *   void (**name)(int)    — pointer to pointer to function
     * Count the indirection levels to emit the right number of *s. */
    Type *fty = NULL;
    int ptr_depth = 0;
    {
        Type *t = ty;
        while (t && t->kind == TY_PTR && t->base) {
            ptr_depth++;
            if (t->base->kind == TY_FUNC) { fty = t->base; break; }
            t = t->base;
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
static void emit_free_func_mangled_name(Token *name, Type **param_types,
                                         int nparams);

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
static const char *binop_to_operator_suffix(TokenKind op) {
    switch (op) {
    case TK_PLUS:           return "__plus";
    case TK_MINUS:          return "__minus";
    case TK_STAR:           return "__deref";  /* operator* (binary = multiply) */
    case TK_SLASH:          return "__div";
    case TK_PERCENT:        return "__mod";
    case TK_AMP:            return "__bitand";
    case TK_PIPE:           return "__bitor";
    case TK_CARET:          return "__xor";
    case TK_SHL:            return "__lshift";
    case TK_SHR:            return "__rshift";
    case TK_EQ:             return "__eq";
    case TK_NE:             return "__ne";
    case TK_LT:             return "__lt";
    case TK_GT:             return "__gt";
    case TK_LE:             return "__le";
    case TK_GE:             return "__ge";
    case TK_LAND:           return "__land";
    case TK_LOR:            return "__lor";
    case TK_PLUS_ASSIGN:    return "__plus_assign";
    case TK_MINUS_ASSIGN:   return "__minus_assign";
    case TK_STAR_ASSIGN:    return "__mul_assign";
    case TK_SLASH_ASSIGN:   return "__div_assign";
    case TK_AMP_ASSIGN:     return "__bitand_assign";
    case TK_PIPE_ASSIGN:    return "__bitor_assign";
    case TK_CARET_ASSIGN:   return "__xor_assign";
    case TK_SHL_ASSIGN:     return "__lshift_assign";
    case TK_SHR_ASSIGN:     return "__rshift_assign";
    case TK_PERCENT_ASSIGN: return "__mod_assign";
    default:                return NULL;
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

static void emit_expr(Node *n) {
    if (!n) return;
    switch (n->kind) {
    case ND_NUM:
        if (n->num.is_signed)
            fprintf(stdout, "%lld", (long long)n->num.lo);
        else
            fprintf(stdout, "%lluU", (unsigned long long)n->num.lo);
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
    case ND_STR:
        if (n->str.tok) fprintf(stdout, "%.*s", n->str.tok->len, n->str.tok->loc);
        return;
    case ND_IDENT:
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
        if (n->ident.implicit_this) {
            fputs("this->", stdout);
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
            /* Overloaded free-function reference — emit the signature-
             * mangled C symbol of the resolved overload. The
             * resolved_decl carries the specific overload (sema set
             * it via free-function overload resolution); without the
             * mangle, all overloads collide on one C name. */
            Declaration *rd = n->ident.resolved_decl;
            if (!n->ident.implicit_this && rd &&
                rd->type && rd->type->kind == TY_FUNC &&
                free_func_name_is_overloaded(n->ident.name)) {
                emit_free_func_mangled_name(n->ident.name,
                                             rd->type->params,
                                             rd->type->nparams);
            } else {
                emit_token_text(n->ident.name);
            }
        }
        return;
    case ND_BINARY: {
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
        if (lhs_ty && (lhs_ty->kind == TY_STRUCT || lhs_ty->kind == TY_UNION) &&
            lhs_ty->tag) {
            const char *suffix = binop_to_operator_suffix(n->binary.op);
            if (suffix) {
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
                int np = emit_class_op_mangled_name(lhs_ty, suffix, args, 1,
                                                     lhs_const, &pty, NULL);
                fputs("(&", stdout);
                emit_expr(n->binary.lhs);
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
        /* Compound assignment on struct: a += b → Class__plus_assign(&a, b)
         * Same value-vs-pointer distinction as ND_BINARY — TY_PTR lhs
         * is native C pointer arithmetic, not a class operator. */
        Type *lhs_ty = n->binary.lhs ? n->binary.lhs->resolved_type : NULL;
        if (ty_is_ref(lhs_ty)) lhs_ty = lhs_ty->base;
        if (lhs_ty && (lhs_ty->kind == TY_STRUCT || lhs_ty->kind == TY_UNION) &&
            lhs_ty->tag && n->binary.op != TK_ASSIGN) {
            const char *suffix = binop_to_operator_suffix(n->binary.op);
            if (suffix) {
                Type *rhs_ty = n->binary.rhs ? n->binary.rhs->resolved_type : NULL;
                Type *args[1] = { rhs_ty };
                Type **pty = NULL;
                bool lhs_const = receiver_type_is_const(
                    n->binary.lhs ? n->binary.lhs->resolved_type : NULL);
                int np = emit_class_op_mangled_name(lhs_ty, suffix, args, 1,
                                                     lhs_const, &pty, NULL);
                fputs("(&", stdout);
                emit_expr(n->binary.lhs);
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
        const char *usuf = NULL;
        if (ot && (ot->kind == TY_STRUCT || ot->kind == TY_UNION) &&
            ot->tag) {
            switch (n->unary.op) {
            case TK_MINUS: usuf = "__minus"; break;
            case TK_PLUS:  usuf = "__plus";  break;
            case TK_EXCL:  usuf = "__not";   break;
            case TK_TILDE: usuf = "__compl"; break;
            default: break;
            }
        }
        if (usuf) {
            Type **pty = NULL;
            Node *winner = NULL;
            bool op_const = receiver_type_is_const(
                n->unary.operand ? n->unary.operand->resolved_type : NULL);
            int np = resolve_operator_overload(ot, usuf, NULL, 0,
                                                op_const, &pty, &winner);
            if (np == 0) {
                mangle_class_tag(ot);
                fputs(usuf, stdout);
                mangle_param_suffix(NULL, 0);
                if (candidate_is_const(winner)) fputs("_const", stdout);
                fputs("(&", stdout);
                emit_expr(n->unary.operand);
                fputc(')', stdout);
                return;
            }
        }
        /* For &(ref_param): the ref is already a pointer, so &(*x)
         * cancels out to just x. Suppress the deref. */
        if (n->unary.op == TK_AMP) g_suppress_ref_deref = true;
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
                    fputs("sf__", stdout);
                    fprintf(stdout, "%.*s",
                            class_tok->len, class_tok->loc);
                    if (ltid && ltid->kind == ND_TEMPLATE_ID)
                        emit_template_id_suffix(ltid);
                    fprintf(stdout, "__%.*s",
                            method_tok->len, method_tok->loc);
                    if (callee_ft && callee_ft->kind == TY_FUNC &&
                        callee_ft->nparams > 0) {
                        mangle_param_suffix(callee_ft->params,
                                             callee_ft->nparams);
                    } else {
                        Type **at = NULL;
                        int na = collect_call_arg_types(n->call.args,
                                                         n->call.nargs, &at);
                        mangle_param_suffix(at, na);
                    }
                    fputc('(', stdout);
                    for (int i = 0; i < n->call.nargs; i++) {
                        if (i > 0) fputs(", ", stdout);
                        /* N4659 §11.3.2 [dcl.ref]: adapt args for
                         * ref-typed params (wrap in &). */
                        Type *pty = (callee_ft && callee_ft->kind == TY_FUNC &&
                                     i < callee_ft->nparams)
                            ? callee_ft->params[i] : NULL;
                        emit_arg_for_param(n->call.args[i], pty);
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
        if (callee && callee->kind == ND_IDENT &&
            callee->ident.implicit_this &&
            callee->ident.resolved_decl &&
            callee->ident.resolved_decl->type &&
            callee->ident.resolved_decl->type->kind == TY_FUNC &&
            callee->ident.resolved_decl->home &&
            callee->ident.resolved_decl->home->owner_type &&
            callee->ident.resolved_decl->home->owner_type->tag) {
            Type *class_type = callee->ident.resolved_decl->home->owner_type;
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
                /* Virtual dispatch: this->__sf_vptr->m(this, args) */
                fprintf(stdout, "this->__sf_vptr->%.*s(this",
                        mname->len, mname->loc);
            } else {
                Type **at = NULL;
                int na = collect_call_arg_types(n->call.args,
                                                 n->call.nargs, &at);
                Node *winner = NULL;
                int np = resolve_overload(class_type, mname, false,
                                           at, na,
                                           g_current_method_is_const,
                                           &call_pty, &winner);
                if (np < 0)
                    die_no_overload(class_type, mname, na, "ND_CALL implicit-this");
                call_np = np;
                {
                    bool mc = candidate_is_const(winner);
                    mangle_class_method_cv(class_type, mname, call_pty, np, mc);
                }
                if (base_len > 0) {
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
            }
            for (int i = 0; i < n->call.nargs; i++) {
                fputs(", ", stdout);
                emit_arg_for_param(n->call.args[i],
                                    (call_pty && i < call_np)
                                        ? call_pty[i] : NULL);
            }
            fputc(')', stdout);
            return;
        }
        if (callee && callee->kind == ND_MEMBER) {
            Node *obj = callee->member.obj;
            Type *ot = obj ? obj->resolved_type : NULL;
            /* Sema-missed fallback: when obj is itself a member access
             * ('outer.inner.method()') and inner's resolved_type wasn't
             * set (happens for certain anonymous-typedef / field paths),
             * resolve the member type on-demand by looking up the field
             * name in the outer's class_region. This keeps method
             * dispatch working when sema's member-type propagation is
             * incomplete. N4659 §6.4.5 [class.qual]. */
            /* References (TY_REF / TY_RVALREF) are lowered to C
             * pointers, so 'obj_is_ptr' is true for both — the
             * call-site must pass the ref as-is, not take its
             * address. N4659 §11.3.2 [dcl.ref]. */
            bool obj_is_ptr = ty_is_indirect(ot);
            if (obj_is_ptr) ot = ot->base;
            /* If the ref was TY_REF(TY_PTR(...)) or similar, peel
             * an inner ref/ptr once more so 'ot' lands on the class. */
            if (ty_is_ref(ot)) ot = ot->base;
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
                     * support). */
                    if (obj_is_ptr) {
                        fputc('(', stdout);
                        emit_expr(obj);
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

                    if (ot->class_region) {
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
                                                   false, at, na,
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
                                mangle_class_method_cv(method_class,
                                    callee->member.member, at, na, mc);
                                call_np = na;
                                call_pty = at;
                            } else {
                                is_method_call = false;
                                goto plain_call;
                            }
                        } else {
                            call_np = np;
                            bool mc = candidate_is_const(winner);
                            mangle_class_method_cv(method_class,
                                                     callee->member.member,
                                                     call_pty, np, mc);
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
                    fputs("&(", stdout);
                    emit_expr(obj);
                    fputs(").", stdout);
                    if (base_idx_for_this == 0) fputs("__sf_base", stdout);
                    else fprintf(stdout, "__sf_base%d", base_idx_for_this);
                } else if (obj_is_ptr) {
                    /* Obj is already ptr/ref (lowered to ptr) — pass
                     * as-is. Suppress the ref-param deref in case obj
                     * is an identifier that names a ref-param: we
                     * need the pointer itself, not its dereference. */
                    bool saved_suppress = g_suppress_ref_deref;
                    g_suppress_ref_deref = true;
                    emit_expr(obj);
                    g_suppress_ref_deref = saved_suppress;
                } else {
                    fputc('&', stdout);
                    emit_expr(obj);
                }
                /* Default-argument injection for method calls. Reach
                 * the method's TY_FUNC through the winner node to pull
                 * its param_defaults. N4659 §11.3.6 [dcl.fct.default].
                 * Pattern: gcc 4.8 gimplify.c 'stack.reserve(8)' where
                 * vec::reserve(unsigned, bool exact = false). */
                Type *win_fty = NULL;
                if (winner_method) {
                    if (winner_method->kind == ND_VAR_DECL &&
                        winner_method->var_decl.ty &&
                        winner_method->var_decl.ty->kind == TY_FUNC)
                        win_fty = winner_method->var_decl.ty;
                    /* ND_FUNC_DEF has no single TY_FUNC, but we can
                     * recover param_defaults from the method's
                     * recorded TY_FUNC when in-class. Skipped here —
                     * defaults are typically on declarations, not
                     * OOL definitions. */
                }
                int total = n->call.nargs;
                if (win_fty && win_fty->param_defaults &&
                    win_fty->nparams > n->call.nargs) {
                    bool all_tail = true;
                    for (int i = n->call.nargs; i < win_fty->nparams; i++)
                        if (!win_fty->param_defaults[i]) { all_tail = false; break; }
                    if (all_tail) total = win_fty->nparams;
                }
                for (int i = 0; i < total; i++) {
                    fputs(", ", stdout);
                    Node *arg = (i < n->call.nargs)
                        ? n->call.args[i]
                        : win_fty->param_defaults[i];
                    emit_arg_for_param(arg,
                                        (call_pty && i < call_np)
                                            ? call_pty[i] : NULL);
                }
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
             * using the CALL-SITE argument types as the signature.
             * Using resolved_decl->type->params instead falls over
             * when sema's resolver points at a template declaration
             * whose params are still TY_DEPENDENT — the suffix would
             * come out as 'unknown_ptr_unknown_ptr_...'. Arg types
             * are always concrete (by the time sema has visited the
             * args). */
            bool emitted_mangled = false;
            if (n->call.callee && n->call.callee->kind == ND_IDENT &&
                !n->call.callee->ident.implicit_this &&
                free_func_name_is_overloaded(n->call.callee->ident.name)) {
                Type *at[32];
                int na = n->call.nargs < 32 ? n->call.nargs : 32;
                bool all_resolved = na == n->call.nargs;
                for (int i = 0; i < na; i++) {
                    at[i] = n->call.args[i] ? n->call.args[i]->resolved_type : NULL;
                    if (!at[i]) { all_resolved = false; break; }
                }
                if (getenv("SF_DBG_CALL")) {
                    Token *nm = n->call.callee->ident.name;
                    if (nm && nm->len == 9 && memcmp(nm->loc, "gt_pch_nx", 9) == 0 && na == 3) {
                        fprintf(stderr, "DBG call gt_pch_nx na=3 arg0.kind=%d",
                            n->call.args[0] ? n->call.args[0]->kind : -1);
                        /* Walk into arg[0]. */
                        Node *a = n->call.args[0];
                        int depth = 0;
                        while (a && depth < 6) {
                            fprintf(stderr, " | d=%d kind=%d rt=%p",
                                depth, a->kind, (void*)a->resolved_type);
                            if (a->resolved_type)
                                fprintf(stderr, "(tk=%d)", a->resolved_type->kind);
                            if (a->kind == ND_UNARY) a = a->unary.operand;
                            else if (a->kind == ND_SUBSCRIPT) a = a->subscript.base;
                            else if (a->kind == ND_CALL) a = a->call.callee;
                            else break;
                            depth++;
                        }
                        fprintf(stderr, "\n");
                    }
                }
                if (all_resolved) {
                    emit_free_func_mangled_name(
                        n->call.callee->ident.name, at, na);
                    emitted_mangled = true;
                }
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
        fputc('(', stdout);
        emit_type(n->cast.ty);
        fputc(')', stdout);
        emit_expr(n->cast.operand);
        return;
    case ND_SIZEOF:
        fputs("sizeof(", stdout);
        if (n->sizeof_.is_type && n->sizeof_.ty)
            emit_type(n->sizeof_.ty);
        else if (n->sizeof_.expr)
            emit_expr(n->sizeof_.expr);
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
        /* Pick the access operator. Source-level '.' must become '->'
         * when the operand has been lowered to a pointer in our C —
         * either an actual pointer (TY_PTR) or a reference parameter
         * (TY_REF / TY_RVALREF, lowered to T*). N4659 §8.2.5
         * [expr.ref] — references behave as the referenced object,
         * so source uses '.' even though our C lowers to a pointer. */
        bool obj_is_ptr_in_c = ty_is_indirect(raw_obj_ty);
        const char *access_op =
            (n->member.op == TK_ARROW || obj_is_ptr_in_c) ? "->" : ".";
        bool did_base_rewrite = false;
        if (obj_ty && (obj_ty->kind == TY_STRUCT || obj_ty->kind == TY_UNION) &&
            obj_ty->class_region && mem) {
            /* Check: is this member NOT in the class itself but in a base? */
            Declaration *own = region_lookup_own(obj_ty->class_region,
                                                  mem->loc, mem->len);
            if (!own) {
                /* Not found in own class — check bases */
                /* We need to find WHICH base has this member.
                 * Walk each base's region looking for the member. */
                for (int bi = 0; bi < obj_ty->class_region->nbases; bi++) {
                    DeclarativeRegion *base_r = obj_ty->class_region->bases[bi];
                    Declaration *bd = lookup_in_scope(base_r, mem->loc, mem->len);
                    if (bd) {
                        emit_expr(n->member.obj);
                        fputs(access_op, stdout);
                        if (bi == 0) fputs("__sf_base.", stdout);
                        else fprintf(stdout, "__sf_base%d.", bi);
                        fprintf(stdout, "%.*s", mem->len, mem->loc);
                        did_base_rewrite = true;
                        break;
                    }
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
            /* Suppress ref-param deref: the -> already handles it. */
            if (obj_is_ptr_in_c) g_suppress_ref_deref = true;
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
            int np = resolve_operator_overload(base_ty, "__subscript",
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
            mangle_class_tag(base_ty);
            fputs("__subscript", stdout);
            if (np >= 0) mangle_param_suffix(pty, np);
            if (candidate_is_const(winner)) fputs("_const", stdout);
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
    default:
        fputs("/* expr */", stdout);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Statement emission                                                 */
/* ------------------------------------------------------------------ */

static void emit_stmt(Node *n);

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
        for (int i = 0; i < ty->enum_ntokens; i++) {
            Token *t = &ty->enum_tokens[i];
            if (t->has_space && i > 0) fputc(' ', stdout);
            fprintf(stdout, "%.*s", t->len, t->loc);
        }
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
                emit_expr(n->var_decl.init);
            }
            return;
        }
        mark_enum_body_emitted(ty->enum_tokens);
        ty->codegen_emitted = true;
        fputs("enum ", stdout);
        if (ty->tag)
            fprintf(stdout, "%.*s ", ty->tag->len, ty->tag->loc);
        fputs("{ ", stdout);
        for (int i = 0; i < ty->enum_ntokens; i++) {
            Token *t = &ty->enum_tokens[i];
            if (t->has_space && i > 0) fputc(' ', stdout);
            fprintf(stdout, "%.*s", t->len, t->loc);
        }
        fprintf(stdout, " } %.*s",
                n->var_decl.name->len, n->var_decl.name->loc);
        if (n->var_decl.init) {
            fputs(" = ", stdout);
            emit_expr(n->var_decl.init);
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
        fputc(')', stdout);
        if (n->var_decl.init && n->var_decl.init->kind > 0 &&
            n->var_decl.init->kind < 200) {
            fputs(" = ", stdout);
            emit_expr(n->var_decl.init);
        }
        return;
    }
    /* Function type as var-decl: a local or top-level function
     * declaration. N4659 §11.3.5 [dcl.fct]. C syntax is
     *   ret name(params);
     * not 'functype name;'. Emit the full prototype. */
    if (ty && ty->kind == TY_FUNC && n->var_decl.name) {
        emit_type(ty->ret);
        fputc(' ', stdout);
        if (free_func_name_is_overloaded(n->var_decl.name)) {
            emit_free_func_mangled_name(n->var_decl.name,
                                         ty->params, ty->nparams);
        } else {
            fprintf(stdout, "%.*s", n->var_decl.name->len,
                    n->var_decl.name->loc);
        }
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
    if (ty && ty->kind == TY_ARRAY) {
        emit_type(ty->base);
        fputc(' ', stdout);
        if (n->var_decl.name)
            fprintf(stdout, "%.*s", n->var_decl.name->len,
                    n->var_decl.name->loc);
        if (ty->array_len >= 0) {
            fprintf(stdout, "[%d]", ty->array_len);
        } else if (ty->array_size_expr) {
            fputc('[', stdout);
            emit_expr(ty->array_size_expr);
            fputc(']', stdout);
        } else {
            fputs("[]", stdout);
        }
        /* Array init (if any) */
        if (n->var_decl.init) {
            fputs(" = ", stdout);
            emit_expr(n->var_decl.init);
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
            emit_expr(n->var_decl.init);
        }
        return;
    }
    emit_type(ty);
    fputc(' ', stdout);
    if (n->var_decl.name)
        fprintf(stdout, "%.*s", n->var_decl.name->len, n->var_decl.name->loc);
    /* Bit-field width — N4659 §12.2.4 [class.bit] */
    if (n->var_decl.bitfield_width) {
        fputs(" : ", stdout);
        emit_expr(n->var_decl.bitfield_width);
    }
    if (n->var_decl.init) {
        fputs(" = ", stdout);
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
            fputs("(*", stdout);
            emit_expr(n->var_decl.init);
            fputc(')', stdout);
        } else if (var_is_ref && !init_is_ref && !init_is_ptr) {
            fputc('&', stdout);
            emit_expr(n->var_decl.init);
        } else {
            emit_expr(n->var_decl.init);
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
        } else {
            mangle_class_dtor(v->var_decl.ty);
            fprintf(stdout, "(&%.*s);\n",
                    v->var_decl.name->len, v->var_decl.name->loc);
        }
    }
    /* Three-way chain-out. See the comment in emit_block where this
     * was originally written. */
    char rt_buf[40], bt_buf[40], ct_buf[40];
    bool have_break = false, have_cont = false;
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
        return expr_has_class_temp(e->binary.lhs) ||
               expr_has_class_temp(e->binary.rhs);
    case ND_UNARY:
    case ND_POSTFIX:
        return expr_has_class_temp(e->unary.operand);
    case ND_TERNARY:
        return expr_has_class_temp(e->ternary.cond) ||
               expr_has_class_temp(e->ternary.then_) ||
               expr_has_class_temp(e->ternary.else_);
    case ND_MEMBER:
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

/* Push a CL_VAR for a user-declared class local onto the live stack.
 * Pulled out so emit_block and the mini-block path share it. */
static void push_user_var_cleanup(Node *s) {
    if (!g_cf.func_has_cleanups || !s ||
        s->kind != ND_VAR_DECL || !s->var_decl.ty ||
        s->var_decl.ty->kind != TY_STRUCT || !s->var_decl.ty->has_dtor ||
        !s->var_decl.name || g_cf.nlive >= CLEANUP_LIVE_MAX)
        return;
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
    fputs("{\n", stdout);
    g_indent++;
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
    g_indent--;
    emit_indent();
    fputs("}\n", stdout);
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

    fputs("{\n", stdout);
    g_indent++;

    /* Per-var cleanup label refactor. We walk statements in order
     * and PUSH onto g_cf.live exactly when we see a dtor-bearing
     * var-decl — so at every point during emission innermost-
     * cleanup-label reflects construction state, not block topology. */
    int saved_nlive = g_cf.nlive;

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

    g_indent--;
    emit_indent();
    fputs("}\n", stdout);
}

static void emit_class_def(Node *n);

/* If an if/while/for single-statement body needs hoisting (e.g. a
 * method call argument is itself a call whose result must be
 * addressed), the hoisted temp has no valid scope in 'if (cond)
 * stmt;' — there's no block to host the decl. Wrap non-block bodies
 * in '{ }' so hoist_stmt_temps can emit the temp decl before the
 * statement inside the block. For ND_BLOCK bodies, emit_block
 * handles per-statement hoisting itself — just delegate.
 * Pattern: gcc 4.8 cgraphunit.c assemble_thunk 'if (this_adjusting)
 * vargs.quick_push(thunk_adjust(...));'. */
static void emit_if_body_with_hoist(Node *body) {
    if (!body) { fputs(";\n", stdout); return; }
    if (body->kind == ND_BLOCK) { emit_stmt(body); return; }
    fputs("{\n", stdout);
    g_indent++;
    hoist_stmt_temps(body);
    emit_indent();
    emit_stmt(body);
    g_indent--;
    emit_indent();
    fputs("}\n", stdout);
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
        if (n->expr_stmt.expr)
            emit_expr(n->expr_stmt.expr);
        fputs(";\n", stdout);
        return;
    case ND_NULL_STMT:
        fputs(";\n", stdout);
        return;
    case ND_CLASS_DEF:
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
    case ND_VAR_DECL:
        /* Block-scope inline-struct dependency, same shape as the
         * top-level emit path: 'static const struct T { ... } arr[];'
         * in a function body hangs T's body off var_decl.ty's class_def
         * with no separate ND_CLASS_DEF. Emit the struct body in-place
         * so the var-decl that references it has a complete type.
         * Pattern: gcc 4.8 builtins.c expand_builtin_nonlocal_goto
         *   static const struct elims {const int from, to;} elim_regs[] = ...; */
        {
            Type *dep = n->var_decl.ty;
            while (dep && dep->kind == TY_ARRAY) dep = dep->base;
            if (dep && (dep->kind == TY_STRUCT || dep->kind == TY_UNION) &&
                dep->class_def && !dep->codegen_emitted) {
                int saved_phase = g_emit_phase;
                g_emit_phase = 0;
                emit_class_def(dep->class_def);
                g_emit_phase = saved_phase;
                emit_indent();
            }
        }
        emit_var_decl_inner(n);
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
                    int np = resolve_overload(n->var_decl.ty, NULL, true,
                                               at, na, false, &pty, NULL);
                    if (np < 0)
                        die_no_overload(n->var_decl.ty, NULL, na,
                                         "direct-init ctor call");
                    mangle_class_ctor(n->var_decl.ty, pty, np);
                    fprintf(stdout, "(&%.*s",
                            n->var_decl.name->len, n->var_decl.name->loc);
                    for (int i = 0; i < n->var_decl.ctor_nargs; i++) {
                        fputs(", ", stdout);
                        emit_arg_for_param(n->var_decl.ctor_args[i],
                                            i < np ? pty[i] : NULL);
                    }
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

            /* Synth decl at the current indent (emit_block already
             * emitted indent for us). Type is int — cond is
             * implicitly converted to bool/int for the test, and
             * int is always assignable from any scalar cond. */
            fprintf(stdout, "int %s;\n", cond_name);

            /* Open mini-block */
            emit_indent();
            fputs("{\n", stdout);
            g_indent++;
            int saved_nlive = g_cf.nlive;

            /* Hoist temps from the cond (inside the mini-block,
             * so their decls are emitted at the mini-block's
             * indent level). */
            hoist_temps_in_expr(n->if_.cond);

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

            g_indent--;
            emit_indent();
            fputs("}\n", stdout);

            /* Now emit the actual if using the synthetic. */
            emit_indent();
            fprintf(stdout, "if (%s) ", cond_name);
            emit_stmt(n->if_.then_);
            if (n->if_.else_) {
                emit_indent();
                fputs("else ", stdout);
                emit_stmt(n->if_.else_);
            }
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
            fputs("{\n", stdout);
            g_indent++;
            emit_indent();
            emit_var_decl_inner(n->if_.cond);
            fputs(";\n", stdout);
            emit_indent();
            fprintf(stdout, "if (%.*s) ", nm->len, nm->loc);
            emit_stmt(n->if_.then_);
            if (n->if_.else_) {
                emit_indent();
                fputs("else ", stdout);
                emit_stmt(n->if_.else_);
            }
            g_indent--;
            emit_indent();
            fputs("}", stdout);
            return;
        }
        fputs("if (", stdout);
        emit_expr(n->if_.cond);
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
        bool body_wrap = g_cf.func_has_cleanups && subtree_has_cleanups(body);
        bool cond_wrap = expr_has_class_temp(n->while_.cond);
        if (!body_wrap && !cond_wrap) {
            /* Simple natural form — no cleanups anywhere. */
            fputs("while (", stdout);
            emit_expr(n->while_.cond);
            fputs(") ", stdout);
            emit_stmt(body);
            return;
        }
        /* Slice C: when the body has cleanups, push a CL_LOOP marker
         * so body break/continue chains terminate at the loop's
         * synthetic boundary labels. */
        int brk = -1, cnt = -1;
        if (body_wrap) {
            brk = g_cf.next_label_id++;
            cnt = g_cf.next_label_id++;
            if (g_cf.nlive < CLEANUP_LIVE_MAX) {
                g_cf.live[g_cf.nlive].kind = CL_LOOP;
                g_cf.live[g_cf.nlive].label_id = brk;
                g_cf.live[g_cf.nlive].cont_label_id = cnt;
                g_cf.live[g_cf.nlive].var_decl = NULL;
                g_cf.nlive++;
            }
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
            fputs("{\n", stdout);
            g_indent++;
            int saved_nlive = g_cf.nlive;
            hoist_temps_in_expr(n->while_.cond);
            emit_indent();
            fprintf(stdout, "%s = ", cond_name);
            emit_expr(n->while_.cond);
            fputs(";\n", stdout);
            emit_cleanup_chain_for_added(saved_nlive);
            g_cf.nlive = saved_nlive;
            g_indent--;
            emit_indent();
            fputs("}\n", stdout);
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
            g_indent--;
            emit_indent();
            fputs("}\n", stdout);
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
            g_indent--;
            emit_indent();
            fputs("}\n", stdout);
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
        if (!body_wrap && !cond_wrap) {
            fputs("do ", stdout);
            emit_stmt(body);
            emit_indent();
            fputs("while (", stdout);
            emit_expr(n->do_.cond);
            fputs(");\n", stdout);
            return;
        }
        int brk = -1, cnt = -1;
        if (body_wrap) {
            brk = g_cf.next_label_id++;
            cnt = g_cf.next_label_id++;
            if (g_cf.nlive < CLEANUP_LIVE_MAX) {
                g_cf.live[g_cf.nlive].kind = CL_LOOP;
                g_cf.live[g_cf.nlive].label_id = brk;
                g_cf.live[g_cf.nlive].cont_label_id = cnt;
                g_cf.live[g_cf.nlive].var_decl = NULL;
                g_cf.nlive++;
            }
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
            fputs("{\n", stdout);
            g_indent++;
            int saved_nlive = g_cf.nlive;
            hoist_temps_in_expr(n->do_.cond);
            emit_indent();
            fprintf(stdout, "%s = ", cond_name);
            emit_expr(n->do_.cond);
            fputs(";\n", stdout);
            emit_cleanup_chain_for_added(saved_nlive);
            g_cf.nlive = saved_nlive;
            g_indent--;
            emit_indent();
            fputs("}\n", stdout);
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
        bool body_wrap = g_cf.func_has_cleanups && subtree_has_cleanups(body);
        bool cond_wrap = n->for_.cond && expr_has_class_temp(n->for_.cond);
        if (!body_wrap && !cond_wrap) {
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
            fputs(") ", stdout);
            emit_stmt(body);
            return;
        }
        int brk = -1, cnt = -1;
        if (body_wrap) {
            brk = g_cf.next_label_id++;
            cnt = g_cf.next_label_id++;
            if (g_cf.nlive < CLEANUP_LIVE_MAX) {
                g_cf.live[g_cf.nlive].kind = CL_LOOP;
                g_cf.live[g_cf.nlive].label_id = brk;
                g_cf.live[g_cf.nlive].cont_label_id = cnt;
                g_cf.live[g_cf.nlive].var_decl = NULL;
                g_cf.nlive++;
            }
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
            fputs("{\n", stdout);
            g_indent++;
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
            fputs("{\n", stdout);
            g_indent++;
            int saved_nlive = g_cf.nlive;
            hoist_temps_in_expr(n->for_.cond);
            emit_indent();
            fprintf(stdout, "%s = ", cond_name);
            emit_expr(n->for_.cond);
            fputs(";\n", stdout);
            emit_cleanup_chain_for_added(saved_nlive);
            g_cf.nlive = saved_nlive;
            g_indent--;
            emit_indent();
            fputs("}\n", stdout);
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
            g_indent--;
            emit_indent();
            fputs("}\n", stdout);
            if (body_wrap) {
                emit_indent();
                fprintf(stdout, "__SF_loop_break_%d: ;\n", brk);
            }
            g_indent--;
            emit_indent();
            fputs("}\n", stdout);
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
        g_indent--;
        emit_indent();
        fputs("}\n", stdout);
        g_cf.nlive--;
        emit_indent();
        fprintf(stdout, "__SF_loop_break_%d: ;\n", brk);
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
         * 'break' in a switch exits the switch, not a loop. */
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
        /* case — N4659 §9.1 [stmt.label]/4 */
        fputs("case ", stdout);
        if (n->case_.expr)
            emit_expr(n->case_.expr);
        fputs(": ", stdout);
        if (n->case_.stmt)
            emit_stmt(n->case_.stmt);
        else
            fputs(";\n", stdout);
        return;
    case ND_DEFAULT:
        fputs("default: ", stdout);
        if (n->default_.stmt)
            emit_stmt(n->default_.stmt);
        else
            fputs(";\n", stdout);
        return;
    case ND_BREAK:
        if (break_needs_cleanup()) {
            /* Slice C: walk the cleanup chain before exiting the
             * loop. Topmost live entry is a CL_VAR — its label is
             * the next chain target; the chain itself terminates
             * at __SF_loop_break_<n> for the innermost CL_LOOP. */
            int target = break_target();
            char buf[32];
            snprintf(buf, sizeof(buf), "__SF_cleanup_%d", target);
            fprintf(stdout, "__SF_BREAK(%s);\n", buf);
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
    /* Populate ref-param table for deref insertion at use sites. */
    g_nref_params = 0;
    if (func && (func->kind == ND_FUNC_DEF || func->kind == ND_FUNC_DECL)) {
        for (int i = 0; i < func->func.nparams && g_nref_params < REF_PARAM_CAP; i++) {
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
     * For each direct base, call its default ctor if it has one.
     * (Mem-init-list base-class entries with explicit args are NOT
     * yet handled — that needs a richer mem-init parse to recognise
     * 'Base(args)' versus 'member(args)'.) */
    if (cdef->class_def.ty) {
        Type *cty = cdef->class_def.ty;
        int nb = class_nbases(cty);
        for (int b = 0; b < nb; b++) {
            Type *base = class_base(cty, b);
            if (!base || !base->has_default_ctor) continue;
            emit_indent();
            /* Default-ctor chain into the base. 0-arg signature. */
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
     * and the user body). */
    if (cdef->class_def.ty && cdef->class_def.ty->has_virtual_methods) {
        emit_indent();
        fputs("this->__sf_vptr = &", stdout);
        mangle_class_vtable_instance(cdef->class_def.ty);
        fputs(";\n", stdout);
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
                int np = resolve_overload(mty, NULL, true, at, na,
                                           false, &pty, NULL);
                if (np < 0) {
                    /* No matching ctor. For a 0-arg mem-init (`: m()`)
                     * on a plain C struct whose has_default_ctor is
                     * transitively true, this is trivial value-init —
                     * the enclosing allocation's zero-fill covers it.
                     * For a non-zero arg count, this is a real error. */
                    if (na != 0)
                        die_no_overload(mty, NULL, na,
                                         "mem-init ctor call");
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
                int np = resolve_overload(mty, NULL, true, NULL, 0,
                                           false, &pty, NULL);
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
            if (found && found->nargs >= 1) {
                emit_indent();
                fprintf(stdout, "this->%.*s = ",
                        m->var_decl.name->len, m->var_decl.name->loc);
                emit_expr(found->args[0]);
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
    fputs("{\n", stdout);
    g_indent++;
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
        fputs(void_ret ? "__SF_EPILOGUE_VOID;\n" : "__SF_EPILOGUE;\n", stdout);
    }
    g_indent--;
    emit_indent();
    fputs("}\n", stdout);
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

/* Which free-function names at TU scope have >1 declaration?
 * Populated once at emit_c entry; consulted by every free-func
 * emission site. We store pointers to Token.loc (owned by the
 * source buffer) so no copies; equality is length+memcmp. */
#define FREE_OVLD_NAMES_CAP 4096
static Token *g_free_ovld_names[FREE_OVLD_NAMES_CAP];
static int    g_n_free_ovld_names = 0;

/* Called by emit_c at entry. Walks tu->tu.decls[] (plus flat blocks
 * like 'extern "C"' or namespace contents) counting free-function
 * declarations per name; every name with count >= 2 gets its Token
 * added to g_free_ovld_names. Quadratic scan is fine for one-shot
 * emission. Block-scope externs aren't seen here — those get
 * handled opportunistically at their call site. */
static void free_ovld_register(Token *name) {
    if (!name) return;
    for (int i = 0; i < g_n_free_ovld_names; i++) {
        Token *e = g_free_ovld_names[i];
        if (e->len == name->len &&
            memcmp(e->loc, name->loc, name->len) == 0)
            return;  /* already registered */
    }
    if (g_n_free_ovld_names < FREE_OVLD_NAMES_CAP)
        g_free_ovld_names[g_n_free_ovld_names++] = name;
}

/* (name-ptr, per-param signature key) used to detect OVERLOADING:
 * multiple declarations with the same name AND DIFFERENT signatures.
 * A forward decl + matching definition is NOT overloading — same
 * signature → same function. Per-param tag matters for cases like
 * 'dump_bitmap(FILE*, bitmap_head_def*)' vs
 * 'dump_bitmap(FILE*, simple_bitmap_def*)' — first param matches
 * but second diverges. */
#define FFSIG_MAX_PARAMS 16
typedef struct {
    Token *name;
    int    nparams;
    int    kind[FFSIG_MAX_PARAMS];      /* TypeKind or -1 */
    const char *tag[FFSIG_MAX_PARAMS];  /* struct/union tag for TY_PTR->TY_STRUCT, or NULL */
    int    tag_len[FFSIG_MAX_PARAMS];
} FreeFuncSig;

static bool ffsig_matches_name(const FreeFuncSig *a, Token *name) {
    return a->name && name && a->name->len == name->len &&
           memcmp(a->name->loc, name->loc, name->len) == 0;
}

static bool ffsig_same_sig(const FreeFuncSig *a, const FreeFuncSig *b) {
    if (a->nparams != b->nparams) return false;
    int n = a->nparams < FFSIG_MAX_PARAMS ? a->nparams : FFSIG_MAX_PARAMS;
    for (int i = 0; i < n; i++) {
        if (a->kind[i] != b->kind[i]) return false;
        if (!!a->tag[i] != !!b->tag[i]) return false;
        if (a->tag[i]) {
            if (a->tag_len[i] != b->tag_len[i]) return false;
            if (memcmp(a->tag[i], b->tag[i], a->tag_len[i]) != 0) return false;
        }
    }
    return true;
}

/* Fill a FreeFuncSig from a TY_FUNC Type. */
static void ffsig_fill(FreeFuncSig *out, Token *name, Type *fty) {
    out->name = name;
    out->nparams = fty->nparams;
    int n = fty->nparams < FFSIG_MAX_PARAMS ? fty->nparams : FFSIG_MAX_PARAMS;
    for (int i = 0; i < n; i++) {
        Type *p = fty->params[i];
        out->kind[i] = p ? (int)p->kind : -1;
        out->tag[i] = NULL;
        out->tag_len[i] = 0;
        if (p && p->kind == TY_PTR && p->base &&
            (p->base->kind == TY_STRUCT || p->base->kind == TY_UNION) &&
            p->base->tag) {
            out->tag[i] = p->base->tag->loc;
            out->tag_len[i] = p->base->tag->len;
        }
    }
}

static void free_ovld_walk(Node *n, FreeFuncSig *seen, int *nseen, int cap) {
    if (!n) return;
    if (n->kind == ND_BLOCK && n->block.is_flat) {
        for (int i = 0; i < n->block.nstmts; i++)
            free_ovld_walk(n->block.stmts[i], seen, nseen, cap);
        return;
    }
    Token *name = NULL;
    Type  *fty  = NULL;
    if (n->kind == ND_FUNC_DEF || n->kind == ND_FUNC_DECL) {
        name = n->func.name;
        /* Synthesize a signature view from func.params. */
        static Type tmp;
        static Type *tmp_params[32];
        tmp.kind = TY_FUNC;
        int np = n->func.nparams < 32 ? n->func.nparams : 32;
        for (int i = 0; i < np; i++)
            tmp_params[i] = n->func.params[i] ? n->func.params[i]->param.ty : NULL;
        tmp.params = tmp_params;
        tmp.nparams = np;
        fty = &tmp;
    } else if (n->kind == ND_VAR_DECL && n->var_decl.ty &&
               n->var_decl.ty->kind == TY_FUNC) {
        name = n->var_decl.name;
        fty = n->var_decl.ty;
    }
    /* Skip class methods — they mangle via their own path. */
    if (name && n->kind == ND_FUNC_DEF && n->func.class_type) name = NULL;
    if (!name || !fty) return;
    FreeFuncSig cur;
    memset(&cur, 0, sizeof(cur));
    ffsig_fill(&cur, name, fty);
    /* Look for another decl with same name, and check whether any
     * of those has a DIFFERENT signature — then this name is
     * overloaded. A match of (name, same signature) just means
     * forward-decl + definition for the same function. */
    for (int i = 0; i < *nseen; i++) {
        if (!ffsig_matches_name(&seen[i], name)) continue;
        if (!ffsig_same_sig(&seen[i], &cur)) {
            free_ovld_register(name);
            free_ovld_register(seen[i].name);
        }
    }
    if (*nseen < cap) seen[(*nseen)++] = cur;
}

static void free_ovld_populate(Node *tu) {
    g_n_free_ovld_names = 0;
    if (!tu || tu->kind != ND_TRANSLATION_UNIT) return;
    enum { SEEN_CAP = 16384 };
    static FreeFuncSig seen[SEEN_CAP];
    int nseen = 0;
    for (int i = 0; i < tu->tu.ndecls; i++)
        free_ovld_walk(tu->tu.decls[i], seen, &nseen, SEEN_CAP);
}

static bool free_func_name_is_overloaded(Token *name) {
    if (!name) return false;
    for (int i = 0; i < g_n_free_ovld_names; i++) {
        Token *e = g_free_ovld_names[i];
        if (e->len == name->len &&
            memcmp(e->loc, name->loc, name->len) == 0)
            return true;
    }
    return false;
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
    emit_type(ret_ty);
    fputc(' ', stdout);
    emit_free_func_mangled_name(name, ptypes, np);
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

static void emit_func_def(Node *n) {
    emit_source_comment(n->tok);
    cf_begin_function(n);
    Type *saved_ret = g_current_func_ret_ty;
    g_current_func_ret_ty = n->func.ret_ty;
    emit_storage_flags_for_def(n->func.storage_flags);
    emit_free_func_header(n->func.ret_ty, n->func.name,
                          n->func.params, n->func.nparams, n->func.is_variadic);
    fputc(' ', stdout);
    emit_func_body(n);
    g_current_func_ret_ty = saved_ret;
}

/* Emit just the signature of a method as a mangled free function.
 * Used for forward declarations so methods can call each other in
 * any order regardless of source ordering inside the class body.
 *
 * 'class_type' carries class_region for namespace walking; the bare
 * tag alone is not enough because two classes named the same in
 * different namespaces would collide. */
/*
 * Emit a mangled operator method name. 'operator[]' → '__subscript',
 * 'operator=' → '__assign', 'operator==' → '__eq', etc. The name
 * token for operator functions is the 'operator' keyword; the
 * actual operator is the next token(s) in the source. We look at
 * the token following 'operator' to determine the suffix.
 *
 * Falls back to '__operator' for unrecognised operators.
 */
/* Compute the mangled suffix for an operator method from its token.
 * Returns a constant string like "__plus", "__subscript", "__assign".
 * Shared between the decl-site emitter and the call-site candidate
 * matcher so both agree on which operator is which. */
static const char *operator_suffix_for_name(Token *name) {
    if (!name) return "__operator";
    const char *after = name->loc + name->len;
    while (*after == ' ' || *after == '\t') after++;

    if (after[0] == '[')       return "__subscript";
    if (after[0] == '(' && after[1] == ')') return "__call";
    if (after[0] == '=' && after[1] == '=') return "__eq";
    if (after[0] == '!' && after[1] == '=') return "__ne";
    if (after[0] == '<' && after[1] == '=') return "__le";
    if (after[0] == '>' && after[1] == '=') return "__ge";
    if (after[0] == '<' && after[1] != '<') return "__lt";
    if (after[0] == '>' && after[1] != '>') return "__gt";
    if (after[0] == '+' && after[1] == '=') return "__plus_assign";
    if (after[0] == '-' && after[1] == '=') return "__minus_assign";
    if (after[0] == '*' && after[1] == '=') return "__mul_assign";
    if (after[0] == '/' && after[1] == '=') return "__div_assign";
    if (after[0] == '+' && after[1] == '+') return "__incr";
    if (after[0] == '-' && after[1] == '-') return "__decr";
    if (after[0] == '+')  return "__plus";
    if (after[0] == '-' && after[1] == '>') return "__arrow";
    if (after[0] == '-')  return "__minus";
    if (after[0] == '*')  return "__deref";
    if (after[0] == '/')  return "__div";
    if (after[0] == '%')  return "__mod";
    if (after[0] == '&' && after[1] == '&') return "__land";
    if (after[0] == '|' && after[1] == '|') return "__lor";
    if (after[0] == '&' && after[1] == '=') return "__bitand_assign";
    if (after[0] == '|' && after[1] == '=') return "__bitor_assign";
    if (after[0] == '^' && after[1] == '=') return "__xor_assign";
    if (after[0] == '%' && after[1] == '=') return "__mod_assign";
    if (after[0] == '&')  return "__bitand";
    if (after[0] == '|')  return "__bitor";
    if (after[0] == '^')  return "__xor";
    if (after[0] == '~')  return "__compl";
    if (after[0] == '!')  return "__not";
    if (after[0] == '<' && after[1] == '<' && after[2] == '=') return "__lshift_assign";
    if (after[0] == '>' && after[1] == '>' && after[2] == '=') return "__rshift_assign";
    if (after[0] == '<' && after[1] == '<') return "__lshift";
    if (after[0] == '>' && after[1] == '>') return "__rshift";
    if (after[0] == '=')  return "__assign";
    return "__operator";
}

static void emit_operator_method_name_cv(Type *class_type, Token *name,
                                          Type **param_types, int nparams,
                                          bool is_const) {
    mangle_class_tag(class_type);
    fputs(operator_suffix_for_name(name), stdout);
    mangle_param_suffix(param_types, nparams);
    if (is_const) fputs("_const", stdout);
}

/*
 * Is this function name an operator keyword?
 */
static bool is_operator_name(Token *name) {
    return name && name->kind == TK_KW_OPERATOR;
}

static void emit_method_signature(Node *func, Type *class_type) {
    if (!func || func->kind != ND_FUNC_DEF) return;
    if (!class_type || !class_type->tag || !func->func.name) return;

    /* Class methods (in-class definitions are implicitly inline)
     * and dtor body functions get __SF_INLINE so multi-TU compilation
     * dedupes via weak symbols. See docs/inline_and_dedup.md. */
    fputs("__SF_INLINE ", stdout);
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
        emit_operator_method_name_cv(class_type, func->func.name, pty, np,
                                      func->func.is_const_method);
    } else {
        Type **pty = NULL;
        int np = collect_func_param_types(func, &pty);
        mangle_class_method_cv(class_type, func->func.name, pty, np,
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
static void emit_method_as_free_fn(Node *func, Type *class_type) {
    if (!func || func->kind != ND_FUNC_DEF) return;
    if (!class_type || !class_type->tag || !func->func.name) return;

    emit_source_comment(func->tok);
    cf_begin_function(func);
    Type *saved_ret = g_current_func_ret_ty;
    Type *saved_mc = g_current_method_class;
    bool saved_mconst = g_current_method_is_const;
    g_current_func_ret_ty = func->func.ret_ty;
    g_current_method_class = class_type;
    g_current_method_is_const = func->func.is_const_method;
    emit_method_signature(func, class_type);
    fputc(' ', stdout);
    emit_func_body(func);
    g_current_method_is_const = saved_mconst;
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
     * arrays of structs (i386.h stringop_strategy pattern). */
    for (int i = 0; i < n->class_def.nmembers; i++) {
        Node *m = n->class_def.members[i];
        if (!m || m->kind != ND_VAR_DECL) continue;
        Type *mty = m->var_decl.ty;
        if (!mty) continue;
        while (mty->kind == TY_ARRAY && mty->base) mty = mty->base;
        if ((mty->kind == TY_STRUCT || mty->kind == TY_UNION) &&
            mty->class_def && !mty->codegen_emitted)
            emit_class_def(mty->class_def);
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
    if (poly) {
        fputs("struct ", stdout);
        mangle_class_vtable_type(class_type);
        fputs(";\n", stdout);
    }

    fputs((class_type && class_type->kind == TY_UNION) ? "union " : "struct ",
          stdout);
    if (class_type)
        emit_mangled_class_tag(class_type);
    else if (n->class_def.tag)
        fprintf(stdout, "%.*s",
                n->class_def.tag->len, n->class_def.tag->loc);
    fputc(' ', stdout);
    fputs("{\n", stdout);
    g_indent++;
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
    if (poly && !has_poly_base) {
        emit_indent();
        fputs("const struct ", stdout);
        mangle_class_vtable_type(class_type);
        fputs(" *__sf_vptr;\n", stdout);
    }
    for (int i = 0; i < n->class_def.nmembers; i++) {
        Node *m = n->class_def.members[i];
        if (!m) continue;
        /* Flat blocks from comma-separated declarations:
         * 'size_t precision, char_precision;' → ND_BLOCK(is_flat)
         * containing individual ND_VAR_DECLs. Flatten them. */
        if (m->kind == ND_BLOCK && m->block.is_flat) {
            for (int j = 0; j < m->block.nstmts; j++) {
                Node *s = m->block.stmts[j];
                if (!s || s->kind != ND_VAR_DECL) continue;
                if (s->var_decl.ty && s->var_decl.ty->kind == TY_FUNC) continue;
                emit_indent();
                emit_var_decl_inner(s);
                fputs(";\n", stdout);
            }
            continue;
        }
        if (m->kind != ND_VAR_DECL) continue;
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
        emit_var_decl_inner(m);
        fputs(";\n", stdout);
    }
    g_indent--;
    fputs("};\n", stdout);

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
            emit_method_signature(m, class_type);
            fputs(";\n", stdout);
        } else if (m->kind == ND_VAR_DECL && m->var_decl.ty &&
                   m->var_decl.ty->kind == TY_FUNC && m->var_decl.name &&
                   class_type) {
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
                fputs("(struct ", stdout);
                mangle_class_tag(class_type);
                fputs(" *this);\n", stdout);
                continue;
            }
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
                emit_operator_method_name_cv(class_type, m->var_decl.name,
                                              pty, np, mc);
            } else {
                Type **pty = NULL;
                int np = collect_func_param_types(m, &pty);
                bool mc = m->var_decl.ty && m->var_decl.ty->is_const;
                mangle_class_method_cv(class_type, m->var_decl.name,
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

    /* Forward-declare and (later) emit the sf__Class__dtor wrapper
     * when the class is non-trivially-destructible. The wrapper
     * exists whether or not a user dtor was written. */
    Node *user_dtor = NULL;
    bool user_dtor_out_of_class = false;
    if (class_type && class_type->has_dtor) {
        for (int i = 0; i < n->class_def.nmembers; i++) {
            Node *m = n->class_def.members[i];
            if (!m) continue;
            if (m->kind == ND_FUNC_DEF && m->func.is_destructor) {
                Node *body = m->func.body;
                bool empty = body && body->kind == ND_BLOCK &&
                             body->block.nstmts == 0;
                if (!empty) user_dtor = m;
                break;
            }
            if (m->kind == ND_VAR_DECL && m->var_decl.ty &&
                m->var_decl.ty->kind == TY_FUNC && m->var_decl.is_destructor) {
                /* Out-of-class definition: the body lives at namespace
                 * scope as Foo::~Foo() {...}. We don't have direct
                 * access to the body here, so we can't check for
                 * emptiness — assume it has content (matches the
                 * has_dtor=true assumption in type.c). */
                user_dtor_out_of_class = true;
                break;
            }
        }
        fputs("__SF_INLINE void ", stdout);
        mangle_class_dtor(class_type);
        fputs("(struct ", stdout);
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
                user_dtor_m_out_of_class = true;
                break;
            }
        }
    }

    /* Vtable struct + static instance — N4659 §13.3 [class.virtual].
     *
     * The struct has one function-pointer slot per virtual method,
     * in declaration order. The static instance is filled with the
     * mangled method addresses; ctors install '&instance' into the
     * object's __sf_vptr field, and call sites dispatch through it.
     *
     * First-slice limitation: virtual destructors aren't yet given
     * a vtable slot — they'd require slot-routing for the dtor wrapper
     * which we'll add when we tackle delete-through-base. */
    if (class_type && class_type->has_virtual_methods) {
        /* The struct definition. */
        fputs("struct ", stdout);
        mangle_class_vtable_type(class_type);
        fputs(" {\n", stdout);
        g_indent++;
        for (int i = 0; i < n->class_def.nmembers; i++) {
            Node *m = n->class_def.members[i];
            if (!m) continue;
            bool is_virt_funcdef = (m->kind == ND_FUNC_DEF && m->func.is_virtual);
            bool is_virt_decl = (m->kind == ND_VAR_DECL && m->var_decl.is_virtual &&
                                 m->var_decl.ty && m->var_decl.ty->kind == TY_FUNC);
            if (!is_virt_funcdef && !is_virt_decl) continue;
            /* Skip virtual destructors for the first slice (see above). */
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

        /* The static instance. Each slot points at the method's
         * mangled free-function form. The forward decls for these
         * methods were emitted above, so the names are visible here. */
        fputs("static const struct ", stdout);
        mangle_class_vtable_type(class_type);
        fputc(' ', stdout);
        mangle_class_vtable_instance(class_type);
        fputs(" = {\n", stdout);
        g_indent++;
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
            {
                Type **pty = NULL;
                int np = collect_func_param_types(m, &pty);
                mangle_class_method(class_type, mname, pty, np);
            }
            fputs(",\n", stdout);
        }
        g_indent--;
        fputs("};\n", stdout);
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
         * (via _const suffix) — no dedup needed. N4659 §16.2/3. */
        emit_method_as_free_fn(m, class_type);
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
        fputs("(struct ", stdout);
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
    bool any_user_ctor = false;
    for (int i = 0; i < n->class_def.nmembers; i++) {
        Node *m = n->class_def.members[i];
        if (!m) continue;
        if ((m->kind == ND_FUNC_DEF && m->func.is_constructor) ||
            (m->kind == ND_VAR_DECL && m->var_decl.ty &&
             m->var_decl.ty->kind == TY_FUNC && m->var_decl.is_constructor)) {
            any_user_ctor = true;
            break;
        }
    }
    if (class_type && class_type->has_default_ctor && !any_user_ctor) {
        /* Forward decl + body for the synthesized ctor (0-param). */
        fputs("__SF_INLINE void ", stdout);
        mangle_class_ctor(class_type, NULL, 0);
        fputs("(struct ", stdout);
        mangle_class_tag(class_type);
        fputs(" *this);\n", stdout);

        fputs("__SF_INLINE void ", stdout);
        mangle_class_ctor(class_type, NULL, 0);
        fputs("(struct ", stdout);
        mangle_class_tag(class_type);
        fputs(" *this) {\n", stdout);
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
         * matching emit_ctor_member_inits path for the rationale. */
        if (class_type->has_virtual_methods) {
            emit_indent();
            fputs("this->__sf_vptr = &", stdout);
            mangle_class_vtable_instance(class_type);
            fputs(";\n", stdout);
        }
        for (int i = 0; i < n->class_def.nmembers; i++) {
            Node *m = n->class_def.members[i];
            if (!m || m->kind != ND_VAR_DECL) continue;
            if (!m->var_decl.ty || m->var_decl.ty->kind != TY_STRUCT) continue;
            if (!m->var_decl.ty->has_default_ctor) continue;
            if (!m->var_decl.name) continue;
            emit_indent();
            mangle_class_ctor(m->var_decl.ty, NULL, 0);
            fprintf(stdout, "(&this->%.*s);\n",
                    m->var_decl.name->len, m->var_decl.name->loc);
        }
        g_indent--;
        fputs("}\n", stdout);
    }
}

static void emit_top_level(Node *n) {
    if (!n) return;
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
            if (n->func.is_constructor && n->func.class_type->class_def)
                g_current_class_def = n->func.class_type->class_def;
            emit_method_as_free_fn(n, n->func.class_type);
            g_current_class_def = saved;
        } else {
            /* Dedup: inline functions from headers may be included multiple
             * times in the preprocessed output. Skip redefinitions. */
            {
                Type *p0 = (n->func.nparams > 0 &&
                            n->func.params[0] &&
                            n->func.params[0]->param.ty)
                    ? n->func.params[0]->param.ty : NULL;
                Token *p0_tag = NULL;
                if (p0 && p0->kind == TY_PTR && p0->base &&
                    (p0->base->kind == TY_STRUCT || p0->base->kind == TY_UNION))
                    p0_tag = p0->base->tag;
                if (func_def_dedup_check_sig(n->func.name,
                                              n->func.nparams,
                                              p0 ? (int)p0->kind : -1,
                                              p0_tag))
                    return;
            }
            emit_func_def(n);
        }
        return;
    case ND_CLASS_DEF: emit_class_def(n); return;
    case ND_VAR_DECL:
        /* Dedup top-level var-decls: the two-phase emit walks ND_BLOCK
         * nodes in both passes, so vars inside extern "C" blocks get
         * emitted twice. Track by Node pointer to skip duplicates. */
        {
            enum { VAR_DEDUP_CAP = 1024 };
            static Node *var_seen[VAR_DEDUP_CAP];
            static int var_nseen = 0;
            if (n->var_decl.name) {
                for (int i = 0; i < var_nseen; i++)
                    if (var_seen[i] == n) return;
                if (var_nseen < VAR_DEDUP_CAP) var_seen[var_nseen++] = n;
            }
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
            for (int i = 0; i < n->var_decl.ty->enum_ntokens; i++) {
                Token *t = &n->var_decl.ty->enum_tokens[i];
                if (t->has_space && i > 0) fputc(' ', stdout);
                fprintf(stdout, "%.*s", t->len, t->loc);
            }
            fputs(" };\n", stdout);
            return;
        }
        /* Top-level free function declaration: 'int foo();' parses
         * as ND_VAR_DECL with TY_FUNC. emit_var_decl_inner doesn't
         * know how to print TY_FUNC (emit_type falls back to int),
         * so we synthesize the C declaration shape directly here. */
        if (n->var_decl.ty && n->var_decl.ty->kind == TY_FUNC &&
            n->var_decl.name) {
            {
                Type *fty = n->var_decl.ty;
                int np = fty ? fty->nparams : -1;
                int k  = func_first_param_kind(fty);
                Token *tag = func_first_param_ptr_tag(fty);
                if (func_decl_dedup_check_sig(n->var_decl.name, np, k, tag))
                    return;
            }
            emit_source_comment(n->tok);
            emit_storage_flags(n->var_decl.storage_flags);
            Type *fty = n->var_decl.ty;
            /* Function returning a function pointer requires
             * declarator-interleaving (N4659 §11.3):
             *   void (*signal(int, void(*)(int)))(int);
             * rather than the invalid 'void (*)(int) signal(...)'. */
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
            /* Overloaded free-function names get signature-mangled C
             * symbols. Singleton names (malloc, printf, user code that
             * isn't overloaded) stay unmangled so interop is preserved
             * without per-Declaration 'extern "C"' tracking. */
            if (free_func_name_is_overloaded(n->var_decl.name)) {
                emit_free_func_mangled_name(n->var_decl.name,
                                             fty->params, fty->nparams);
            } else {
                fprintf(stdout, "%.*s", n->var_decl.name->len,
                        n->var_decl.name->loc);
            }
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
         * never defined. */
        {
            Type *dep = n->var_decl.ty;
            while (dep && dep->kind == TY_ARRAY) dep = dep->base;
            if (dep && (dep->kind == TY_STRUCT || dep->kind == TY_UNION) &&
                dep->class_def && !dep->codegen_emitted) {
                /* Pass 2 (PHASE_METHODS) skips the struct body, so we
                 * have to flip the phase to emit the definition. This
                 * preserves the two-phase guarantee for user-declared
                 * classes (whose bodies are emitted in pass 1) while
                 * catching anonymous/inline structs that only appear
                 * as the element type of a var-decl's array. */
                int saved_phase = g_emit_phase;
                g_emit_phase = 0;  /* single-pass: emit everything */
                emit_class_def(dep->class_def);
                g_emit_phase = saved_phase;
            }
        }
        emit_storage_flags(n->var_decl.storage_flags);
        emit_var_decl_inner(n);
        fputs(";\n", stdout);
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
            for (int i = 0; i < uty->enum_ntokens; i++) {
                Token *t = &uty->enum_tokens[i];
                if (t->has_space && i > 0) fputc(' ', stdout);
                fprintf(stdout, "%.*s", t->len, t->loc);
            }
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
    /* __SF_INLINE — multi-TU dedup for inline-eligible functions
     * (in-class methods, synthesized ctor/dtor wrappers, dtor body
     * functions, eventually template instantiations). Expands to a
     * weak-symbol attribute so the linker picks one survivor when
     * the same function is emitted in multiple TUs. See
     * docs/inline_and_dedup.md for the design rationale and full
     * trade-off analysis. */
    fputs("#if defined(__GNUC__) || defined(__clang__)\n", stdout);
    fputs("#  define __SF_INLINE __attribute__((weak))\n", stdout);
    fputs("#elif defined(_MSC_VER)\n", stdout);
    fputs("#  define __SF_INLINE __declspec(selectany)\n", stdout);
    fputs("#else\n", stdout);
    fputs("#  define __SF_INLINE   /* fall back: each TU has its own copy */\n",
          stdout);
    fputs("#endif\n", stdout);
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
        /* Forward-declare instantiated member template functions so they're
         * visible before any class method body that calls them.
         * N4659 §17.5.2 [temp.mem]. */
        if (n->func.class_type && n->func.name) {
            emit_method_signature(n, n->func.class_type);
            fputs(";\n", stdout);
        } else if (n->func.name && n->func.body) {
            /* Forward-declare instantiated function templates so
             * call sites in earlier-emitted functions don't hit C's
             * implicit-int declaration (then 'conflicting types' at
             * the real definition). Narrow to the mangled shape
             * '<name>_t_..._te_' produced by the instantiation pass
             * so user-written free-function overloads (emitted with
             * their plain source names and sometimes deduped) don't
             * get false-positive duplicate decls.
             *
             * SHORTCUT (ours, not the standard): string-match the
             * mangled suffix rather than carrying a
             * came-from-instantiation flag on the Node. The flag
             * would be cleaner but the string match is free and
             * hasn't bitten us yet.
             * TODO(seafront#inst-flag): add Node flag, drop the
             * string match. */
            Token *nm = n->func.name;
            bool looks_instantiated = false;
            if (nm && nm->len >= 7) {
                const char *s = nm->loc;
                int L = nm->len;
                if (L >= 4 && memcmp(s + L - 4, "_te_", 4) == 0) {
                    for (int i = 0; i + 3 < L; i++)
                        if (s[i]=='_' && s[i+1]=='t' && s[i+2]=='_') {
                            looks_instantiated = true; break;
                        }
                }
            }
            if (looks_instantiated) {
                emit_storage_flags_for_def(n->func.storage_flags);
                emit_free_func_header(n->func.ret_ty, n->func.name,
                                      n->func.params, n->func.nparams,
                                      n->func.is_variadic);
                fputs(";\n", stdout);
            }
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

static void emit_forward_decl_funcs(Node *tu) {
    for (int i = 0; i < tu->tu.ndecls; i++)
        emit_fwd_decl_methods_only(tu->tu.decls[i]);
}

void emit_c(Node *tu) {
    if (!tu || tu->kind != ND_TRANSLATION_UNIT) return;
    g_tu = tu;
    /* Pre-scan: names with 2+ free-function declarations get
     * signature-mangled C symbols so each overload has a unique
     * symbol at link time. */
    free_ovld_populate(tu);
    emit_prelude();

    /* Forward-declare ALL struct types so pointer references
     * resolve regardless of definition order. */
    emit_forward_decl_structs(tu);

    /* Emit all enum definitions before any struct bodies, so enum
     * members used as struct fields have complete types. Enums
     * appear as ND_VAR_DECL with TY_ENUM (bare enum definition)
     * or as members of typedef/class nodes. */
    for (int i = 0; i < tu->tu.ndecls; i++) {
        Node *n = tu->tu.decls[i];
        if (!n) continue;
        /* Walk into blocks (namespace/extern "C" contents) */
        if (n->kind == ND_BLOCK) {
            for (int j = 0; j < n->block.nstmts; j++) {
                Node *s = n->block.stmts[j];
                if (!s) continue;
                Type *ety = NULL;
                if (s->kind == ND_VAR_DECL && s->var_decl.ty &&
                    s->var_decl.ty->kind == TY_ENUM)
                    ety = s->var_decl.ty;
                if (s->kind == ND_TYPEDEF && s->var_decl.ty &&
                    s->var_decl.ty->kind == TY_ENUM)
                    ety = s->var_decl.ty;
                if (ety && ety->enum_tokens &&
                    !enum_body_already_emitted(ety->enum_tokens)) {
                    mark_enum_body_emitted(ety->enum_tokens);
                    ety->codegen_emitted = true;
                    fputs("enum ", stdout);
                    if (ety->tag)
                        fprintf(stdout, "%.*s ", ety->tag->len, ety->tag->loc);
                    fputs("{ ", stdout);
                    for (int k = 0; k < ety->enum_ntokens; k++) {
                        Token *t = &ety->enum_tokens[k];
                        if (t->has_space && k > 0) fputc(' ', stdout);
                        fprintf(stdout, "%.*s", t->len, t->loc);
                    }
                    fputs(" };\n", stdout);
                }
            }
            continue;
        }
        /* Top-level enum */
        Type *ety = NULL;
        if (n->kind == ND_VAR_DECL && n->var_decl.ty &&
            n->var_decl.ty->kind == TY_ENUM)
            ety = n->var_decl.ty;
        if (n->kind == ND_TYPEDEF && n->var_decl.ty &&
            n->var_decl.ty->kind == TY_ENUM)
            ety = n->var_decl.ty;
        if (ety && ety->enum_tokens &&
            !enum_body_already_emitted(ety->enum_tokens)) {
            mark_enum_body_emitted(ety->enum_tokens);
            ety->codegen_emitted = true;
            fputs("enum ", stdout);
            if (ety->tag)
                fprintf(stdout, "%.*s ", ety->tag->len, ety->tag->loc);
            fputs("{ ", stdout);
            for (int k = 0; k < ety->enum_ntokens; k++) {
                Token *t = &ety->enum_tokens[k];
                if (t->has_space && k > 0) fputc(' ', stdout);
                fprintf(stdout, "%.*s", t->len, t->loc);
            }
            fputs(" };\n", stdout);
        }
    }

    /* Two-pass emit: first all struct definitions (with forward
     * declarations for their methods), then all method bodies,
     * vtables, synthesized ctors/dtors, and non-class top-level
     * declarations. This ensures that ALL struct types are fully
     * defined before any method body dereferences a pointer to
     * another struct (e.g. vl_ptr methods accessing vl_embed
     * members through a pointer). */

    /* Forward-declare methods + instantiated function templates
     * AFTER enums. The method signatures can reference enum types by
     * pointer (e.g. 'enum ld_plugin_symbol_resolution *'); without
     * the enum body already visible, '-std=c99' lets gcc accept the
     * pointer type but implicitly declares the enum, then the real
     * enum definition emitted by the struct-body pass conflicts.
     * C11 §6.7.2.3 — the enum type must be complete in the
     * translation-unit scope before its name can be used. */
    emit_forward_decl_funcs(tu);

    /* Pass 1: struct bodies only */
    g_emit_phase = PHASE_STRUCTS;
    for (int i = 0; i < tu->tu.ndecls; i++) {
        Node *n = tu->tu.decls[i];
        if (!n) continue;
        if (n->kind == ND_CLASS_DEF || n->kind == ND_TYPEDEF) {
            fputc('\n', stdout);
            emit_top_level(n);
        } else if (n->kind == ND_BLOCK) {
            /* Namespace blocks may contain class definitions */
            fputc('\n', stdout);
            emit_top_level(n);
        }
    }

    /* Pass 2: method bodies + everything else */
    g_emit_phase = PHASE_METHODS;
    for (int i = 0; i < tu->tu.ndecls; i++) {
        fputc('\n', stdout);
        emit_top_level(tu->tu.decls[i]);
    }
    g_emit_phase = 0;
}
