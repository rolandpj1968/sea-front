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
/* Name encoding — Itanium ABI §5.1.6 [mangle.name]                   */
/* ------------------------------------------------------------------ */

/* Source-name: <length><identifier>. Itanium ABI §5.1.6.4. */
static void emit_source_name(Token *name) {
    if (!name) { fputs("4anon", stdout); return; }
    fprintf(stdout, "%d%.*s", name->len, name->len, name->loc);
}

/* True iff `name` is exactly "std" — qualifies for the St standard
 * substitution (§5.1.6.5 [mangle.subst]). */
static bool tok_is_std(Token *name) {
    return name && name->len == 3 && memcmp(name->loc, "std", 3) == 0;
}

/* Walk a TY_STRUCT/UNION/ENUM Type's enclosing namespace chain,
 * outermost-first. Stage 2 supports only namespace and direct-class
 * scope (no nested classes / template prefixes — those land in
 * Stages 3-4). Returns the count of namespace prefixes collected;
 * the caller decides whether to wrap them with N...E. */
enum { ITAN_MAX_NS = 16 };

typedef struct {
    Token *names[ITAN_MAX_NS];
    int    n;
} NsChain;

static void collect_namespace_chain(Type *ty, NsChain *out) {
    out->n = 0;
    if (!ty || !ty->class_region) return;
    DeclarativeRegion *r = ty->class_region->enclosing;
    while (r && out->n < ITAN_MAX_NS) {
        if (r->kind == REGION_NAMESPACE && r->name)
            out->names[out->n++] = r->name;
        r = r->enclosing;
    }
}

/* Emit a single substitution for a previously-pushed prefix Type, if
 * present. Returns true on hit. Used between prefix steps so a
 * second reference to `ns::T` becomes `S0_` instead of repeating the
 * full prefix. */
static bool try_emit_prefix_sub(ItanCtx *c, Type *ty) {
    return ctx_try_emit_sub(c, ty);
}

/* Synthetic Type slot for tracking intermediate prefixes (e.g. the
 * `std::` prefix as its own entity, distinct from `std::T`). One
 * slot per namespace level. */
static Type g_ns_prefix_slots[ITAN_MAX_NS];

/* Emit the full TY_STRUCT/UNION/ENUM name, with nested-name wrapping
 * if there are namespace qualifiers. Pushes intermediate prefixes
 * onto the sub table per §5.1.6.5 so future references back-ref. */
static void emit_class_or_enum_name(ItanCtx *c, Type *ty) {
    NsChain chain;
    collect_namespace_chain(ty, &chain);

    /* Outermost first (chain is innermost-first; iterate reversed). */
    /* Reverse into out_outer for clarity. */
    Token *outer[ITAN_MAX_NS];
    for (int i = 0; i < chain.n; i++)
        outer[i] = chain.names[chain.n - 1 - i];

    if (chain.n == 0) {
        /* Unscoped — just <source-name>. §5.1.6.4. */
        emit_source_name(ty->tag);
        return;
    }

    /* Nested-name N...E. */
    fputc('N', stdout);

    /* Walk outermost → innermost emitting each prefix. The first
     * namespace gets the St shortcut if it's "std"; later prefixes
     * just emit source-name. After each prefix is emitted, push a
     * synthetic Type marker so future references can short-cut. */
    int start = 0;
    if (tok_is_std(outer[0])) {
        fputs("St", stdout);
        /* St doesn't go on the sub table itself (standard subs are
         * not added per §5.1.6.5). */
        start = 1;
    }

    for (int i = start; i < chain.n; i++) {
        /* Try sub for the prefix-up-to-here. We use a synthetic
         * Type slot keyed by the namespace token's loc; structural
         * comparison would otherwise rebuild the same string. */
        Type *slot = &g_ns_prefix_slots[i];
        slot->kind = TY_STRUCT;
        slot->tag  = outer[i];
        if (try_emit_prefix_sub(c, slot)) {
            /* Already in table — just shortcut and we're done with
             * the namespace portion. Continue from i+1 for any
             * deeper prefix. */
            /* For Stage 2 we don't yet handle deeper prefixes
             * after a sub hit; the simple case is "all of the
             * namespace prefix back-refs to one slot". */
            start = i + 1;
            /* Re-emit the source-names from start onwards. */
            for (int j = start; j < chain.n; j++) {
                emit_source_name(outer[j]);
                ctx_push(c, &g_ns_prefix_slots[j]);
            }
            emit_source_name(ty->tag);
            fputc('E', stdout);
            return;
        }
        emit_source_name(outer[i]);
        ctx_push(c, slot);
    }

    /* Class name as the unqualified terminal. Push the FULL class
     * Type as a sub candidate. */
    emit_source_name(ty->tag);
    /* The class itself is pushed by the caller (emit_type's
     * TY_STRUCT branch), not here, to keep the push-once invariant. */
    fputc('E', stdout);
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
        if (ctx_try_emit_sub(c, ty)) return;
        if (ty->is_const || ty->is_volatile) {
            /* `const T` mangles as `K1T` (and `K1T` is itself a sub
             * candidate, distinct from the bare `1T`). Itanium ABI
             * §5.1.5 + §5.1.6.5: emit cv wrapper, recurse on the
             * unqualified form (which is its own sub candidate),
             * then push the qualified form. The unqualified form
             * is staged in a static slot keyed by depth so its
             * address remains stable for sub-table lookups within
             * the symbol. */
            emit_qual_wrapper(c, ty);
            static Type unq_buf[ITAN_MAX_SUBS];
            int slot = c->nsubs;  /* roughly per recursion depth */
            if (slot < 0 || slot >= ITAN_MAX_SUBS) slot = 0;
            unq_buf[slot] = *ty;
            unq_buf[slot].is_const = false;
            unq_buf[slot].is_volatile = false;
            emit_type(c, &unq_buf[slot]);
            ctx_push(c, ty);
            return;
        }
        emit_class_or_enum_name(c, ty);
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

/* mangle_class_tag is no longer dispatched — see mangle.c comment.
 * Kept here as an unreachable abort to flag any future call site
 * that does end up dispatching, so the divergence isn't silent. */
void itan_mangle_class_tag(Type *class_type) {
    (void)class_type;
    fputs("sea-front internal error: itan_mangle_class_tag is "
          "unreachable; see src/codegen/mangle.c\n", stderr);
    abort();
}

/* Method mangling — Itanium ABI §5.1.4 [mangle.entity-name].
 * Shape: _Z N <cv?> <prefix> <method-name> E <params>
 * where <cv> is K (const) and/or V (volatile) on the implicit object
 * parameter (§5.1.5.2 [mangle.member-fn]). */
void itan_mangle_class_method_cv(Type *class_type, Token *method_name,
                                  Type **param_types, int nparams,
                                  bool is_const) {
    ItanCtx ctx;
    ctx_reset(&ctx);
    fputs("_Z", stdout);

    /* Build prefix from namespace chain + class. The class always
     * adds at least one nesting level, so we always emit N...E. */
    NsChain chain;
    collect_namespace_chain(class_type, &chain);

    Token *outer[ITAN_MAX_NS];
    for (int i = 0; i < chain.n; i++)
        outer[i] = chain.names[chain.n - 1 - i];

    fputc('N', stdout);
    if (is_const) fputc('K', stdout);

    int start = 0;
    if (chain.n > 0 && tok_is_std(outer[0])) {
        fputs("St", stdout);
        start = 1;
    }

    /* Emit namespace prefixes, pushing each. */
    for (int i = start; i < chain.n; i++) {
        Type *slot = &g_ns_prefix_slots[i];
        slot->kind = TY_STRUCT;
        slot->tag  = outer[i];
        emit_source_name(outer[i]);
        ctx_push(&ctx, slot);
    }
    /* Class as the next prefix step — push it so the same class
     * appearing in a parameter type later back-refs. */
    emit_source_name(class_type->tag);
    ctx_push(&ctx, class_type);

    /* Method name (the unqualified terminal). Itanium has special
     * encodings for ctors/dtors/operators (Stages 4-5); a regular
     * method is just <source-name>. */
    emit_source_name(method_name);
    fputc('E', stdout);

    /* Parameter type list — substitution table CONTINUES from the
     * prefix walk (the class Type at top of stack can be back-
     * referenced if it appears in a param type). */
    if (nparams == 0) {
        fputc('v', stdout);
    } else {
        for (int i = 0; i < nparams; i++)
            emit_type(&ctx, normalize_param(param_types[i], i));
    }
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

/* Vtable entries — also no longer dispatched. Same flagging-only
 * abort to surface any unexpected call. */
void itan_mangle_class_vtable_type(Type *class_type) {
    (void)class_type;
    fputs("sea-front internal error: itan_mangle_class_vtable_type "
          "is unreachable; see src/codegen/mangle.c\n", stderr);
    abort();
}

void itan_mangle_class_vtable_instance(Type *class_type) {
    (void)class_type;
    fputs("sea-front internal error: itan_mangle_class_vtable_instance "
          "is unreachable; see src/codegen/mangle.c\n", stderr);
    abort();
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
