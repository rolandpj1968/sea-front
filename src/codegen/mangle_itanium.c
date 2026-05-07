/*
 * src/codegen/mangle_itanium.c — Itanium C++ ABI symbol mangling.
 *
 * Itanium C++ ABI <https://itanium-cxx-abi.github.io/cxx-abi/abi.html#mangling>.
 *
 * Selected at codegen entry by --mangling=itanium. Each public entry
 * point starts a fresh per-symbol substitution table, walks the
 * type / class tree per the ABI's traversal rules, and emits the
 * mangled name to stdout.
 *
 * Stage 1 (this commit): built-in types, compound prefixes
 * (P/R/O/A/F), CV qualifiers on pointees (K/V/r), per-symbol
 * substitution table with pointer-equality dedup. Class types
 * (TY_STRUCT/UNION/ENUM) remain stubbed — Stage 2 implements names.
 *
 * See memory/project_itanium_mangling_slice.md for the slice plan.
 */
#include "mangle.h"
#include "../parse/parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Per-symbol mangling context                                        */
/* ------------------------------------------------------------------ */

/* Substitution table — Itanium ABI §5.1.6.5 [mangle.subst].
 * One per mangled symbol; reset at the start of each public entry
 * point. Holds the Type pointers that have been emitted as
 * substitution candidates, in order. The Nth entry is referenced
 * later as 'S<N-1>_' for N >= 1, with N == 0 being the special 'S_'.
 *
 * Pointer-equality dedup is correct when sea-front's canonicalisation
 * (project_canonicalize_template_types.md) gives every template-id
 * Type a single canonical pointer. For built-in pointers like 'int*'
 * coming from distinct parse sites the pointers differ; we miss the
 * substitution and emit the long form. That's correct, just not
 * minimum-length. Stage 3 may switch to structural comparison. */
enum { ITAN_MAX_SUBS = 256 };

typedef struct {
    Type *subs[ITAN_MAX_SUBS];
    int   nsubs;
} ItanCtx;

static void ctx_reset(ItanCtx *c) { c->nsubs = 0; }

/* Structural equality for substitution lookup. Itanium subs match
 * type-identity, not pointer-identity: two `int*` Types from
 * distinct parse sites must compare equal so the second occurrence
 * back-references the first. Recurses through compound types,
 * checks kind + cv + base/ret/params, and compares struct tags by
 * spelling. */
static bool ty_eq(Type *a, Type *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    if (a->is_const != b->is_const) return false;
    if (a->is_volatile != b->is_volatile) return false;
    if (a->is_unsigned != b->is_unsigned) return false;
    switch (a->kind) {
    case TY_PTR: case TY_REF: case TY_RVALREF:
        return ty_eq(a->base, b->base);
    case TY_ARRAY:
        return a->array_len == b->array_len && ty_eq(a->base, b->base);
    case TY_FUNC:
        if (a->nparams != b->nparams) return false;
        if (!ty_eq(a->ret, b->ret)) return false;
        for (int i = 0; i < a->nparams; i++)
            if (!ty_eq(a->params[i], b->params[i])) return false;
        return true;
    case TY_STRUCT: case TY_UNION: case TY_ENUM:
        if (!a->tag || !b->tag) return a->tag == b->tag;
        if (a->tag->len != b->tag->len) return false;
        if (memcmp(a->tag->loc, b->tag->loc, a->tag->len) != 0) return false;
        if (a->n_template_args != b->n_template_args) return false;
        for (int i = 0; i < a->n_template_args; i++)
            if (!ty_eq(a->template_args[i], b->template_args[i])) return false;
        return true;
    default:
        return true;  /* same fundamental kind + cv/sign already checked */
    }
}

/* Try to emit 'ty' as a back-reference to an existing slot. Returns
 * true if emitted, false otherwise. */
static bool ctx_try_emit_sub(ItanCtx *c, Type *ty) {
    if (!ty) return false;
    for (int i = 0; i < c->nsubs; i++) {
        if (ty_eq(c->subs[i], ty)) {
            /* Slot 0 → S_, slot N>0 → S<N-1 in base 36, uppercase>_. */
            if (i == 0) { fputs("S_", stdout); return true; }
            int n = i - 1;
            char buf[8];
            int len = 0;
            do {
                int d = n % 36;
                buf[len++] = (d < 10) ? (char)('0' + d) : (char)('A' + d - 10);
                n /= 36;
            } while (n > 0);
            fputc('S', stdout);
            for (int k = len - 1; k >= 0; k--) fputc(buf[k], stdout);
            fputc('_', stdout);
            return true;
        }
    }
    return false;
}

static void ctx_push(ItanCtx *c, Type *ty) {
    if (!ty) return;
    if (c->nsubs >= ITAN_MAX_SUBS) return;  /* silent overflow — name still valid, just no sub */
    c->subs[c->nsubs++] = ty;
}

/* ------------------------------------------------------------------ */
/* Built-in type encoding — Itanium ABI §5.1.5 [mangle.builtin]       */
/* ------------------------------------------------------------------ */

/* Returns the single-letter (or 'D'-prefixed) builtin code for a
 * fundamental Type, or NULL when the kind isn't a fundamental. */
static const char *builtin_code(Type *ty) {
    if (!ty) return NULL;
    switch (ty->kind) {
    case TY_VOID:   return "v";
    case TY_BOOL:   return "b";
    /* sea-front's TY_CHAR carries is_unsigned to distinguish unsigned
     * from default char; signed-char (the third C++ type) isn't
     * tracked separately, so map to plain 'c'. The Itanium 'a'
     * (signed char) is reachable only when sea-front grows that
     * distinction. */
    case TY_CHAR:   return ty->is_unsigned ? "h" : "c";
    case TY_CHAR16: return "Ds";
    case TY_CHAR32: return "Di";
    case TY_WCHAR:  return "w";
    case TY_SHORT:  return ty->is_unsigned ? "t" : "s";
    case TY_INT:    return ty->is_unsigned ? "j" : "i";
    case TY_LONG:   return ty->is_unsigned ? "m" : "l";
    case TY_LLONG:  return ty->is_unsigned ? "y" : "x";
    case TY_FLOAT:  return "f";
    case TY_DOUBLE: return "d";
    case TY_LDOUBLE: return "e";
    default:        return NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Type encoding (recursive)                                          */
/* ------------------------------------------------------------------ */

static void emit_type(ItanCtx *c, Type *ty);

/* Emit a "qualified" wrapper (K / V / r) when the pointee under
 * P/R/O carries cv-qualifiers, then the inner type. The qualified
 * form itself is a substitution candidate (Itanium §5.1.6.5). */
static void emit_qual_wrapper(ItanCtx *c, Type *ty) {
    (void)c;
    /* Itanium §5.1.5: order is r V K when multiple are present, with
     * the type substituted as the qualified form added en bloc.
     * For the Stage 1 fixture set we only have const & volatile;
     * 'r' (restrict) is reachable but rare. */
    if (ty->is_const)    fputc('K', stdout);
    if (ty->is_volatile) fputc('V', stdout);
    /* No restrict tracking on Type currently; would emit 'r' here. */
}

static void emit_type(ItanCtx *c, Type *ty) {
    if (!ty) { fputc('v', stdout); return; }  /* defensive — encode void */

    /* Substitution lookup BEFORE encoding. Built-in types and CV-
     * qualified built-ins are NOT subs (their codes are already
     * minimal); compound types ARE. The sub check below is gated by
     * the kind switch — only call it for sub-eligible kinds. */

    switch (ty->kind) {
    case TY_VOID: case TY_BOOL: case TY_CHAR: case TY_CHAR16: case TY_CHAR32:
    case TY_WCHAR: case TY_SHORT: case TY_INT: case TY_LONG: case TY_LLONG:
    case TY_FLOAT: case TY_DOUBLE: case TY_LDOUBLE: {
        const char *code = builtin_code(ty);
        if (!code) { fputc('v', stdout); return; }
        /* CV-qualified builtin (e.g. 'const int' as a pointee): emit
         * K/V then the code. The qualified form IS a sub candidate
         * (the bare builtin isn't). */
        if (ty->is_const || ty->is_volatile) {
            if (ctx_try_emit_sub(c, ty)) return;
            emit_qual_wrapper(c, ty);
            fputs(code, stdout);
            ctx_push(c, ty);
            return;
        }
        fputs(code, stdout);
        return;
    }
    case TY_PTR: {
        if (ctx_try_emit_sub(c, ty)) return;
        /* CV on the pointer itself (top-level when this Type is a
         * parameter is stripped before we get here; otherwise applies
         * to the pointer-typed pointee — e.g. 'int *const *' has K
         * on the inner pointer). */
        if (ty->is_const || ty->is_volatile) emit_qual_wrapper(c, ty);
        fputc('P', stdout);
        emit_type(c, ty->base);
        ctx_push(c, ty);
        return;
    }
    case TY_REF: {
        if (ctx_try_emit_sub(c, ty)) return;
        fputc('R', stdout);
        emit_type(c, ty->base);
        ctx_push(c, ty);
        return;
    }
    case TY_RVALREF: {
        if (ctx_try_emit_sub(c, ty)) return;
        fputc('O', stdout);
        emit_type(c, ty->base);
        ctx_push(c, ty);
        return;
    }
    case TY_ARRAY: {
        if (ctx_try_emit_sub(c, ty)) return;
        /* Itanium §5.1.5: array-as-parameter decays to pointer per
         * C++ §11.3.4/5 — same rule we apply in human mangling. The
         * caller (mangle_param_suffix) is expected to have decayed
         * already. If we still see a TY_ARRAY here it's in a context
         * that genuinely wants the array form (e.g. a member type);
         * encode A<dim>_<elem>. dim 0 emitted as 'A_' (incomplete
         * array). */
        if (ty->array_len > 0)
            fprintf(stdout, "A%d_", ty->array_len);
        else
            fputs("A_", stdout);
        emit_type(c, ty->base);
        ctx_push(c, ty);
        return;
    }
    case TY_FUNC: {
        if (ctx_try_emit_sub(c, ty)) return;
        fputc('F', stdout);
        emit_type(c, ty->ret);
        if (ty->nparams == 0) {
            fputc('v', stdout);
        } else {
            for (int i = 0; i < ty->nparams; i++)
                emit_type(c, ty->params[i]);
        }
        fputc('E', stdout);
        ctx_push(c, ty);
        return;
    }
    case TY_STRUCT: case TY_UNION: case TY_ENUM:
        /* Stage 2 implements names. Until then, emit a placeholder
         * that's clearly distinguishable in test output and won't be
         * confused with a real symbol. The 'u' (vendor-extended type)
         * prefix per §5.1.5 takes a source-name; we use it as a
         * distinguishable marker. */
        if (ctx_try_emit_sub(c, ty)) return;
        if (ty->tag)
            fprintf(stdout, "u%d%.*s", ty->tag->len, ty->tag->len, ty->tag->loc);
        else
            fputs("u4anon", stdout);
        ctx_push(c, ty);
        return;
    case TY_DEPENDENT:
        /* Template-parameter-dependent type — should be substituted
         * away before reaching mangling. If we see one here it's a
         * sea-front bug; emit a placeholder. */
        fputs("u9_DEPENDENT", stdout);
        return;
    default:
        fputc('v', stdout);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Top-level cv-stripping for parameter types — Itanium ABI §5.1.5    */
/* ------------------------------------------------------------------ */

/* Same shape as the human-side strip_top_cv: a parameter declared
 * `const T` or `T * const` mangles as if `T` / `T *`. The cv on the
 * outermost layer of the param type is dropped from the signature.
 * Pointee cv (e.g. `const T *`) is part of the type and stays.
 *
 * Also decays `T[N]` to `T *` per N4659 §11.3.4/5 [dcl.array]: an
 * array-typed parameter is the equivalent pointer-to-element. The
 * Itanium ABI follows this — `f(int[5])` and `f(int *)` are the
 * same symbol. */
static Type g_stripped_buf[16];
static Type g_decayed_buf[16];
static Type *normalize_param(Type *ty, int slot) {
    if (!ty) return ty;
    if (slot < 0 || slot >= 16) return ty;
    Type *cur = ty;
    /* Decay array → pointer before cv-stripping (a `const T[N]` param
     * is `T *const` which cv-strips to `T *`, equivalent to the array
     * decay's `T *`). */
    if (cur->kind == TY_ARRAY) {
        Type *decayed = &g_decayed_buf[slot];
        *decayed = *cur;
        decayed->kind = TY_PTR;
        cur = decayed;
    }
    if (cur->is_const || cur->is_volatile) {
        Type *copy = &g_stripped_buf[slot];
        *copy = *cur;
        copy->is_const = false;
        copy->is_volatile = false;
        cur = copy;
    }
    return cur;
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                */
/* ------------------------------------------------------------------ */

/* The not-yet-implemented entries (Stages 2-5) keep aborting with
 * a clear message. */
static void itan_unimpl(const char *what) {
    fprintf(stderr,
        "sea-front: --mangling=itanium not yet implemented (%s)\n"
        "  See memory/project_itanium_mangling_slice.md for plan.\n",
        what);
    abort();
}

void itan_mangle_class_tag(Type *class_type) {
    (void)class_type; itan_unimpl("mangle_class_tag (Stage 2)");
}

void itan_mangle_class_method_cv(Type *class_type, Token *method_name,
                                  Type **param_types, int nparams,
                                  bool is_const) {
    (void)class_type; (void)method_name; (void)param_types;
    (void)nparams; (void)is_const;
    itan_unimpl("mangle_class_method_cv (Stage 2)");
}

void itan_mangle_class_ctor(Type *class_type,
                             Type **param_types, int nparams) {
    (void)class_type; (void)param_types; (void)nparams;
    itan_unimpl("mangle_class_ctor (Stage 5)");
}

void itan_mangle_class_dtor(Type *class_type) {
    (void)class_type; itan_unimpl("mangle_class_dtor (Stage 5)");
}

void itan_mangle_class_dtor_body(Type *class_type) {
    (void)class_type; itan_unimpl("mangle_class_dtor_body (Stage 5)");
}

void itan_mangle_class_vtable_type(Type *class_type) {
    (void)class_type; itan_unimpl("mangle_class_vtable_type (deferred slice)");
}

void itan_mangle_class_vtable_instance(Type *class_type) {
    (void)class_type; itan_unimpl("mangle_class_vtable_instance (deferred slice)");
}

/* Single-type encoding — used at template-arg-list sites in emit_c.
 * Each call is its own substitution scope. */
void itan_emit_type_for_mangle(Type *ty) {
    ItanCtx ctx;
    ctx_reset(&ctx);
    emit_type(&ctx, ty);
}

/* Parameter type list — used after the function-name encoding by
 * the public mangle_class_method etc. helpers. Stage 1 only handles
 * the type-list encoding; the surrounding name encoding is in Stage
 * 2. The substitution scope here is local to one call.
 *
 * Itanium emits no "_p_..._pe_" delimiters — params follow the
 * function name directly. With the empty-param case encoded as a
 * single 'v' (void). */
void itan_mangle_param_suffix(Type **param_types, int nparams) {
    ItanCtx ctx;
    ctx_reset(&ctx);
    if (nparams == 0) {
        fputc('v', stdout);
        return;
    }
    for (int i = 0; i < nparams; i++)
        emit_type(&ctx, normalize_param(param_types[i], i));
}
