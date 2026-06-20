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

/* Active scheme. CLI flag --mangling= sets this in main.c.
 * Default is Itanium so sea-front output links interoperably with
 * gcc/clang/libstdc++. Human-readable mangling stays available
 * behind --mangling=human for grep-friendly disassembly. */
MangleKind g_mangle_kind = MANGLE_ITANIUM;

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

/* Non-dispatching human type encoder. Used directly by paths that
 * MUST stay in human encoding regardless of g_mangle_kind — namely
 * C struct tag emission (mangle_class_tag and friends), since
 * struct tags are TU-local C identifiers that benefit from
 * grep-friendly readable form even when linker symbols are
 * Itanium. The recursive calls go through this same function so
 * the encoding stays human all the way down. */
static void hum_emit_type(Type *ty) {
    if (!ty) { fputs("unknown", stdout); return; }
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
    case TY_PTR:     hum_emit_type(ty->base); fputs("_ptr", stdout); return;
    case TY_REF:     hum_emit_type(ty->base); fputs("_ref", stdout); return;
    case TY_RVALREF: hum_emit_type(ty->base); fputs("_rref", stdout); return;
    /* Array-as-parameter decays to pointer — N4659 §11.3.4/5
     * [dcl.array]. */
    case TY_ARRAY:   hum_emit_type(ty->base); fputs("_ptr", stdout); return;
    case TY_STRUCT: case TY_UNION:
        if (ty->tag) fprintf(stdout, "%.*s", ty->tag->len, ty->tag->loc);
        else fputs("anon", stdout);
        if (ty->n_template_args > 0) {
            fputs("_t_", stdout);
            for (int i = 0; i < ty->n_template_args; i++) {
                if (i > 0) fputc('_', stdout);
                hum_emit_type(ty->template_args[i]);
            }
            fputs("_te_", stdout);
        }
        return;
    case TY_ENUM:
        if (ty->tag) fprintf(stdout, "%.*s", ty->tag->len, ty->tag->loc);
        else fputs("anon_enum", stdout);
        return;
    case TY_FUNC:
        fputs("fn_", stdout);
        hum_emit_type(ty->ret);
        for (int i = 0; i < ty->nparams; i++) {
            fputc('_', stdout);
            hum_emit_type(ty->params[i]);
        }
        fputs("_fne_", stdout);
        return;
    /* Template type parameter (T, U, ...). Encode by tag name —
     * unsubstituted dependent types reaching the mangler is usually
     * a sema/clone gap, but emitting the tag is at least stable
     * (so two TY_DEPENDENT('T') produce the same encoding). */
    case TY_DEPENDENT:
        if (ty->tag) fprintf(stdout, "%.*s", ty->tag->len, ty->tag->loc);
        else fputs("dep", stdout);
        return;
    /* Literal-valued NTTP placeholder (instantiate.c synthesizes
     * these from ND_NUM / ND_BOOL_LIT / etc. arg nodes). The tag
     * holds the literal token; the parameter's declared type is on
     * `nttp_decl_type` (set during instantiation). Two NTTP slots
     * with the same literal text but different declared types
     * (`template<int N>` vs `template<long N>` with `<42>`) name
     * different specialisations per N4659 §17.4 [temp.type] and
     * must mangle distinctly. Encode the type as a small prefix
     * before the literal text. The Itanium path
     * (mangle_itanium.c) does the same dispatch via
     * builtin_code(). */
    case TY_NTTP_VALUE:
        if (ty->nttp_decl_type) {
            switch (ty->nttp_decl_type->kind) {
            case TY_BOOL:   fputs("bool_",   stdout); break;
            case TY_CHAR:   fputs(ty->nttp_decl_type->is_unsigned
                                  ? "uchar_" : "char_", stdout); break;
            case TY_SHORT:  fputs(ty->nttp_decl_type->is_unsigned
                                  ? "ushort_" : "short_", stdout); break;
            case TY_INT:    fputs(ty->nttp_decl_type->is_unsigned
                                  ? "uint_" : "int_", stdout); break;
            case TY_LONG:   fputs(ty->nttp_decl_type->is_unsigned
                                  ? "ulong_" : "long_", stdout); break;
            case TY_LLONG:  fputs(ty->nttp_decl_type->is_unsigned
                                  ? "ullong_" : "llong_", stdout); break;
            default: break;
            }
        }
        if (ty->tag && ty->tag->len > 0) {
            for (int i = 0; i < ty->tag->len; i++) {
                unsigned char c = (unsigned char)ty->tag->loc[i];
                bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '_';
                fputc(ok ? c : '_', stdout);
            }
        } else {
            fputs("nttp_unknown", stdout);
        }
        return;
    default:
        fputs("unknown", stdout);
        return;
    }
}

/* Public type encoder. Dispatches on g_mangle_kind: human callers
 * (struct tag emission) reach this with the active kind already
 * being human, and itanium callers route through here from their
 * own emission paths. The non-dispatching hum_emit_type is the
 * fallback for code paths that need always-human regardless of
 * mode (sf__*-prefixed C struct tags). */
void emit_type_for_mangle(Type *ty) {
    if (g_mangle_kind == MANGLE_ITANIUM) { itan_emit_type_for_mangle(ty); return; }
    hum_emit_type(ty);
}

/* Buffer-output version of emit_type_for_mangle. Same encoding,
 * different sink. Used by template instantiation to build a
 * function-symbol mangled name in an arena buffer. Keep in lock-
 * step with emit_type_for_mangle — adding a TypeKind here means
 * adding it there too. */
static int append_str(char *buf, int pos, int max, const char *s) {
    while (*s && pos < max - 1) buf[pos++] = *s++;
    return pos;
}
/* The buffered variants (mangle_*_to_buf) are sea-front-internal —
 * used as opaque keys for dedup of template instantiations. They
 * never become emitted symbols, so they always use the human
 * encoding regardless of g_mangle_kind. Keeping them human-only
 * also avoids burdening the Itanium impl with substitution-table
 * state across these internal-use paths. */
int mangle_type_to_buf(Type *ty, char *buf, int pos, int max) {
    if (!ty) return append_str(buf, pos, max, "unknown");
    if (ty->is_const)    pos = append_str(buf, pos, max, "const_");
    if (ty->is_volatile) pos = append_str(buf, pos, max, "volatile_");
    switch (ty->kind) {
    case TY_VOID:    return append_str(buf, pos, max, "void");
    case TY_BOOL:    return append_str(buf, pos, max, "bool");
    case TY_CHAR:    return append_str(buf, pos, max, ty->is_unsigned ? "uchar" : "char");
    case TY_CHAR16:  return append_str(buf, pos, max, "char16");
    case TY_CHAR32:  return append_str(buf, pos, max, "char32");
    case TY_WCHAR:   return append_str(buf, pos, max, "wchar");
    case TY_SHORT:   return append_str(buf, pos, max, ty->is_unsigned ? "ushort" : "short");
    case TY_INT:     return append_str(buf, pos, max, ty->is_unsigned ? "uint" : "int");
    case TY_LONG:    return append_str(buf, pos, max, ty->is_unsigned ? "ulong" : "long");
    case TY_LLONG:   return append_str(buf, pos, max, ty->is_unsigned ? "ullong" : "llong");
    case TY_FLOAT:   return append_str(buf, pos, max, "float");
    case TY_DOUBLE:  return append_str(buf, pos, max, "double");
    case TY_LDOUBLE: return append_str(buf, pos, max, "ldouble");
    case TY_PTR:
        pos = mangle_type_to_buf(ty->base, buf, pos, max);
        return append_str(buf, pos, max, "_ptr");
    case TY_REF:
        pos = mangle_type_to_buf(ty->base, buf, pos, max);
        return append_str(buf, pos, max, "_ref");
    case TY_RVALREF:
        pos = mangle_type_to_buf(ty->base, buf, pos, max);
        return append_str(buf, pos, max, "_rref");
    case TY_ARRAY:
        pos = mangle_type_to_buf(ty->base, buf, pos, max);
        return append_str(buf, pos, max, "_ptr");
    case TY_STRUCT: case TY_UNION:
        if (ty->tag) {
            int n = ty->tag->len;
            if (pos + n > max - 1) n = max - 1 - pos;
            if (n > 0) memcpy(buf + pos, ty->tag->loc, n);
            pos += n;
        } else {
            pos = append_str(buf, pos, max, "anon");
        }
        if (ty->n_template_args > 0) {
            pos = append_str(buf, pos, max, "_t_");
            for (int i = 0; i < ty->n_template_args; i++) {
                if (i > 0 && pos < max - 1) buf[pos++] = '_';
                pos = mangle_type_to_buf(ty->template_args[i], buf, pos, max);
            }
            pos = append_str(buf, pos, max, "_te_");
        }
        return pos;
    case TY_ENUM:
        if (ty->tag) {
            int n = ty->tag->len;
            if (pos + n > max - 1) n = max - 1 - pos;
            if (n > 0) memcpy(buf + pos, ty->tag->loc, n);
            pos += n;
            return pos;
        }
        return append_str(buf, pos, max, "anon_enum");
    /* See emit_type_for_mangle for the encoding rationale. */
    case TY_FUNC:
        pos = append_str(buf, pos, max, "fn_");
        pos = mangle_type_to_buf(ty->ret, buf, pos, max);
        for (int i = 0; i < ty->nparams; i++) {
            if (pos < max - 1) buf[pos++] = '_';
            pos = mangle_type_to_buf(ty->params[i], buf, pos, max);
        }
        return append_str(buf, pos, max, "_fne_");
    case TY_DEPENDENT:
        if (ty->tag) {
            int n = ty->tag->len;
            if (pos + n > max - 1) n = max - 1 - pos;
            if (n > 0) memcpy(buf + pos, ty->tag->loc, n);
            pos += n;
            return pos;
        }
        return append_str(buf, pos, max, "dep");
    /* See emit_type_for_mangle for the encoding rationale. */
    case TY_NTTP_VALUE:
        if (ty->nttp_decl_type) {
            switch (ty->nttp_decl_type->kind) {
            case TY_BOOL:   pos = append_str(buf, pos, max, "bool_");   break;
            case TY_CHAR:   pos = append_str(buf, pos, max,
                                ty->nttp_decl_type->is_unsigned ? "uchar_" : "char_");   break;
            case TY_SHORT:  pos = append_str(buf, pos, max,
                                ty->nttp_decl_type->is_unsigned ? "ushort_" : "short_"); break;
            case TY_INT:    pos = append_str(buf, pos, max,
                                ty->nttp_decl_type->is_unsigned ? "uint_" : "int_");     break;
            case TY_LONG:   pos = append_str(buf, pos, max,
                                ty->nttp_decl_type->is_unsigned ? "ulong_" : "long_");   break;
            case TY_LLONG:  pos = append_str(buf, pos, max,
                                ty->nttp_decl_type->is_unsigned ? "ullong_" : "llong_"); break;
            default: break;
            }
        }
        if (ty->tag && ty->tag->len > 0) {
            for (int i = 0; i < ty->tag->len && pos < max - 1; i++) {
                unsigned char c = (unsigned char)ty->tag->loc[i];
                bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '_';
                buf[pos++] = ok ? (char)c : '_';
            }
            return pos;
        }
        return append_str(buf, pos, max, "nttp_unknown");
    default:
        return append_str(buf, pos, max, "unknown");
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
    /* Template argument suffix — always human, since this is the
     * C struct tag emission path. Calling hum_emit_type directly
     * (not the dispatching emit_type_for_mangle) keeps the
     * encoding consistent regardless of g_mangle_kind. */
    if (class_type && class_type->n_template_args > 0) {
        fputs("_t_", stdout);
        for (int i = 0; i < class_type->n_template_args; i++) {
            if (i > 0) fputc('_', stdout);
            hum_emit_type(class_type->template_args[i]);
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

/* C struct tags are TU-local C identifiers, never linker symbols.
 * Always emit the human form regardless of g_mangle_kind so
 * disassembly and `gdb` print readable names like
 * `struct sf__vec_t_int_te_` even when linker symbols are
 * Itanium-mangled. The struct tag and the linker symbol live in
 * different name spaces (C compiler vs linker), so coexisting
 * encodings doesn't cause mixing — emit_class_open's nested
 * type-arg emission goes through hum_emit_type directly to keep
 * the entire tag in human form. */
void mangle_class_tag(Type *class_type) {
    emit_class_open(class_type);
    emit_class_close();
}

/* Append the parameter-type list as a mangled suffix —
 *   _p_<t0>_<t1>_..._pe_
 * with 'void' for the empty list. Mirrors the _t_..._te_ shape used
 * for template arguments. N4659 §16.2 [over.load]. */
/* Strip top-level cv-qualifiers from a parameter type for mangling.
 * Itanium C++ ABI §5.1.5: top-level cv on parameter types is
 * dropped (a function declared 'f(int)' and 'f(const int)' is the
 * SAME function — they overload-resolve identically and link to the
 * same symbol). Without stripping, a definition like
 *   void mangle_decl (const tree decl);
 * mangles as '_p_const_tree_node_ptr_pe_' on the def, but every
 * caller passing a non-const tree mangles as '_p_tree_node_ptr_pe_'
 * — link fails with unresolved refs. The const on the pointer
 * parameter is top-level (the pointer itself is const, not the
 * pointee); ABI says drop it. */
static Type g_stripped_param_buf[16];
static Type *strip_top_cv(Type *ty, int slot) {
    if (!ty) return ty;
    if (!ty->is_const && !ty->is_volatile) return ty;
    if (slot < 0 || slot >= 16) return ty;
    Type *copy = &g_stripped_param_buf[slot];
    *copy = *ty;
    copy->is_const = false;
    copy->is_volatile = false;
    return copy;
}

void mangle_param_suffix(Type **param_types, int nparams, bool is_variadic) {
    if (g_mangle_kind == MANGLE_ITANIUM) {
        itan_mangle_param_suffix(param_types, nparams, is_variadic); return;
    }
    fputs("_p_", stdout);
    if (nparams == 0) {
        fputs("void", stdout);
    } else {
        for (int i = 0; i < nparams; i++) {
            if (i > 0) fputc('_', stdout);
            emit_type_for_mangle(strip_top_cv(param_types[i], i));
        }
    }
    if (is_variadic) fputs("_var_", stdout);
    fputs("_pe_", stdout);
}

/* Buffered version of mangle_param_suffix. Used by the canonical
 * function-signature key builder so two decls' equivalence can be
 * decided by string comparison on the mangler's own output —
 * eliminating the need for partial-signature predicates that have
 * to be patched every time a new C++ distinction appears. */
int mangle_param_suffix_to_buf(Type **param_types, int nparams, bool is_variadic,
                                char *buf, int pos, int max) {
    int n = snprintf(buf + pos, (size_t)(max - pos), "_p_");
    if (n > 0) pos += n;
    if (nparams == 0) {
        n = snprintf(buf + pos, (size_t)(max - pos), "void");
        if (n > 0) pos += n;
    } else {
        for (int i = 0; i < nparams; i++) {
            if (i > 0 && pos < max - 1) buf[pos++] = '_';
            pos = mangle_type_to_buf(strip_top_cv(param_types[i], i),
                                      buf, pos, max);
        }
    }
    if (is_variadic) {
        n = snprintf(buf + pos, (size_t)(max - pos), "_var_");
        if (n > 0) pos += n;
    }
    n = snprintf(buf + pos, (size_t)(max - pos), "_pe_");
    if (n > 0) pos += n;
    return pos;
}

void mangle_class_method(Type *class_type, Token *method_name,
                          Type **param_types, int nparams,
                          bool is_const) {
    mangle_class_method_tid(class_type, method_name,
                             /*method_targs=*/NULL, /*n_method_targs=*/0,
                             param_types, nparams, is_const);
}

void mangle_class_method_tid(Type *class_type, Token *method_name,
                              Type **method_targs, int n_method_targs,
                              Type **param_types, int nparams,
                              bool is_const) {
    if (g_mangle_kind == MANGLE_ITANIUM) {
        itan_mangle_class_method_tid(class_type, method_name,
                                      method_targs, n_method_targs,
                                      param_types, nparams, is_const);
        return;
    }
    emit_class_open(class_type);
    g_mangler->append_member(g_mangler, method_name);
    /* Human-mangling appends method-template args as a grep-friendly
     * suffix on the source-name: `<int>` in source becomes `_tid_int`
     * in the symbol. Uses the same type-renderer as the param-suffix
     * so the two stay in lockstep when new Type kinds appear. */
    if (n_method_targs > 0 && method_targs) {
        fputs("_tid", stdout);
        for (int i = 0; i < n_method_targs; i++) {
            fputc('_', stdout);
            emit_type_for_mangle(method_targs[i]);
        }
    }
    mangle_param_suffix(param_types, nparams, /*is_variadic=*/false);
    /* N4659 §10.1.7.1 [dcl.type.cv] / §16.3.1/4: const qualifier
     * on the implicit object parameter distinguishes overloads
     * like operator[](int) vs operator[](int) const. */
    if (is_const) fputs("_const", stdout);
    emit_class_close();
}

void mangle_class_static_data_member(Type *class_type, Token *member_name) {
    /* Static data members always use the human-form symbol
     * 'sf__Class__member' regardless of g_mangle_kind so the three
     * emit sites stay consistent: the in-class declaration
     * (emit_class_def), the qualified access in expressions
     * (emit_c.c ND_MEMBER static path), and this OOL definition
     * branch. Switching just this one to Itanium-scheme would make
     * the OOL '_ZN3Foo3barE' link against the access's 'sf__Foo__bar'
     * and fail. A future cleanup pass can lift all three to Itanium
     * together. */
    mangle_class_tag(class_type);
    fputs("__", stdout);
    if (member_name)
        fprintf(stdout, "%.*s", member_name->len, member_name->loc);
}

void mangle_class_ctor(Type *class_type,
                        Type **param_types, int nparams) {
    if (g_mangle_kind == MANGLE_ITANIUM) {
        itan_mangle_class_ctor(class_type, param_types, nparams); return;
    }
    emit_class_open(class_type);
    g_mangler->append_ctor(g_mangler);
    mangle_param_suffix(param_types, nparams, /*is_variadic=*/false);
    emit_class_close();
}

/* Operator-kind dispatch tables. The human suffix is what follows
 * the class tag in the mangled name (`__plus`, `__subscript`, ...);
 * the Itanium op-id is the 2-letter code per §5.1.4.1
 * [mangle.operator-name]. NULL entries mean "fallback to method-
 * name encoding" (rare; unknown ops). */
static const char *const op_human_suffix[] = {
    [OP_PLUS]            = "__plus",
    [OP_MINUS]           = "__minus",
    [OP_STAR]            = "__deref",        /* unary; binary uses __mul */
    [OP_SLASH]           = "__div",
    [OP_MOD]             = "__mod",
    [OP_AMP]             = "__bitand",
    [OP_PIPE]            = "__bitor",
    [OP_CARET]           = "__xor",
    [OP_TILDE]           = "__compl",
    [OP_BANG]            = "__not",
    [OP_EQ]              = "__eq",
    [OP_NE]              = "__ne",
    [OP_LT]              = "__lt",
    [OP_GT]              = "__gt",
    [OP_LE]              = "__le",
    [OP_GE]              = "__ge",
    [OP_LSHIFT]          = "__lshift",
    [OP_RSHIFT]          = "__rshift",
    [OP_LAND]            = "__land",
    [OP_LOR]             = "__lor",
    [OP_ASSIGN]          = "__assign",
    [OP_PLUS_ASSIGN]     = "__plus_assign",
    [OP_MINUS_ASSIGN]    = "__minus_assign",
    [OP_MUL_ASSIGN]      = "__mul_assign",
    [OP_DIV_ASSIGN]      = "__div_assign",
    [OP_MOD_ASSIGN]      = "__mod_assign",
    [OP_BITAND_ASSIGN]   = "__bitand_assign",
    [OP_BITOR_ASSIGN]    = "__bitor_assign",
    [OP_XOR_ASSIGN]      = "__xor_assign",
    [OP_LSHIFT_ASSIGN]   = "__lshift_assign",
    [OP_RSHIFT_ASSIGN]   = "__rshift_assign",
    [OP_INCR]            = "__incr",
    [OP_DECR]            = "__decr",
    [OP_SUBSCRIPT]       = "__subscript",
    [OP_CALL]            = "__call",
    [OP_ARROW]           = "__arrow",
    [OP_NEW]             = "__new",
    [OP_NEW_ARRAY]       = "__new_array",
    [OP_DELETE]          = "__delete",
    [OP_DELETE_ARRAY]    = "__delete_array",
    [OP_UNKNOWN]         = "__operator",
};

OperatorKind operator_kind_from_method_name(Token *name) {
    if (!name) return OP_UNKNOWN;
    const char *after = name->loc + name->len;
    while (*after == ' ' || *after == '\t') after++;
    /* Word-form operators new / delete with optional `[]` suffix —
     * N4659 §16.5 [over.oper]. Recognised before the symbol matches
     * so `new[` doesn't get misread as `[` (operator subscript). */
    if (after[0] == 'n' && after[1] == 'e' && after[2] == 'w') {
        const char *p = after + 3;
        while (*p == ' ' || *p == '\t') p++;
        if (p[0] == '[' && p[1] == ']') return OP_NEW_ARRAY;
        return OP_NEW;
    }
    if (after[0] == 'd' && after[1] == 'e' && after[2] == 'l' &&
        after[3] == 'e' && after[4] == 't' && after[5] == 'e') {
        const char *p = after + 6;
        while (*p == ' ' || *p == '\t') p++;
        if (p[0] == '[' && p[1] == ']') return OP_DELETE_ARRAY;
        return OP_DELETE;
    }
    /* Three-char patterns first to avoid prefix-shadowing. */
    if (after[0] == '<' && after[1] == '<' && after[2] == '=') return OP_LSHIFT_ASSIGN;
    if (after[0] == '>' && after[1] == '>' && after[2] == '=') return OP_RSHIFT_ASSIGN;
    /* Two-char. */
    if (after[0] == '<' && after[1] == '<') return OP_LSHIFT;
    if (after[0] == '>' && after[1] == '>') return OP_RSHIFT;
    if (after[0] == '=' && after[1] == '=') return OP_EQ;
    if (after[0] == '!' && after[1] == '=') return OP_NE;
    if (after[0] == '<' && after[1] == '=') return OP_LE;
    if (after[0] == '>' && after[1] == '=') return OP_GE;
    if (after[0] == '+' && after[1] == '+') return OP_INCR;
    if (after[0] == '-' && after[1] == '-') return OP_DECR;
    if (after[0] == '+' && after[1] == '=') return OP_PLUS_ASSIGN;
    if (after[0] == '-' && after[1] == '=') return OP_MINUS_ASSIGN;
    if (after[0] == '*' && after[1] == '=') return OP_MUL_ASSIGN;
    if (after[0] == '/' && after[1] == '=') return OP_DIV_ASSIGN;
    if (after[0] == '%' && after[1] == '=') return OP_MOD_ASSIGN;
    if (after[0] == '&' && after[1] == '=') return OP_BITAND_ASSIGN;
    if (after[0] == '|' && after[1] == '=') return OP_BITOR_ASSIGN;
    if (after[0] == '^' && after[1] == '=') return OP_XOR_ASSIGN;
    if (after[0] == '&' && after[1] == '&') return OP_LAND;
    if (after[0] == '|' && after[1] == '|') return OP_LOR;
    if (after[0] == '-' && after[1] == '>') return OP_ARROW;
    if (after[0] == '(' && after[1] == ')') return OP_CALL;
    /* Single-char. */
    if (after[0] == '[') return OP_SUBSCRIPT;
    if (after[0] == '+') return OP_PLUS;
    if (after[0] == '-') return OP_MINUS;
    if (after[0] == '*') return OP_STAR;
    if (after[0] == '/') return OP_SLASH;
    if (after[0] == '%') return OP_MOD;
    if (after[0] == '&') return OP_AMP;
    if (after[0] == '|') return OP_PIPE;
    if (after[0] == '^') return OP_CARET;
    if (after[0] == '~') return OP_TILDE;
    if (after[0] == '!') return OP_BANG;
    if (after[0] == '<') return OP_LT;
    if (after[0] == '>') return OP_GT;
    if (after[0] == '=') return OP_ASSIGN;
    /* Conversion operator (`operator T()`) — caller should detect
     * via the function's return type and use mangle_class_conversion. */
    return OP_UNKNOWN;
}

void mangle_class_operator(Type *class_type, OperatorKind op,
                            Type **param_types, int nparams,
                            bool is_const) {
    if (g_mangle_kind == MANGLE_ITANIUM) {
        itan_mangle_class_operator(class_type, op,
                                    param_types, nparams, is_const);
        return;
    }
    emit_class_open(class_type);
    fputs(op_human_suffix[op], stdout);
    mangle_param_suffix(param_types, nparams, /*is_variadic=*/false);
    if (is_const) fputs("_const", stdout);
    emit_class_close();
}

void mangle_class_conversion(Type *class_type, Type *target_type,
                              bool is_const) {
    if (g_mangle_kind == MANGLE_ITANIUM) {
        itan_mangle_class_conversion(class_type, target_type, is_const);
        return;
    }
    emit_class_open(class_type);
    fputs("__op_", stdout);
    hum_emit_type(target_type);
    /* No param suffix — conversion operators take only `this`. */
    if (is_const) fputs("_const", stdout);
    emit_class_close();
}

void mangle_class_dtor(Type *class_type) {
    if (g_mangle_kind == MANGLE_ITANIUM) { itan_mangle_class_dtor(class_type); return; }
    emit_class_open(class_type);
    g_mangler->append_dtor(g_mangler);
    emit_class_close();
}

void mangle_class_dtor_body(Type *class_type) {
    if (g_mangle_kind == MANGLE_ITANIUM) { itan_mangle_class_dtor_body(class_type); return; }
    emit_class_open(class_type);
    g_mangler->append_dtor_body(g_mangler);
    emit_class_close();
}

/* Vtable type and instance — same TU-local C identifier story as
 * mangle_class_tag, always human. The Itanium vtable LINKER symbol
 * (`_ZTV<class>`) and typeinfo (`_ZTI<class>`) are separate
 * concepts that emit elsewhere when those slices land. */
void mangle_class_vtable_type(Type *class_type) {
    emit_class_open(class_type);
    fputs("__vtable", stdout);
    emit_class_close();
}

void mangle_class_vtable_instance(Type *class_type) {
    emit_class_open(class_type);
    fputs("__vtable_instance", stdout);
    emit_class_close();
}
