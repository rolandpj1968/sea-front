/*
 * type.c — Type construction helpers.
 *
 * Types are arena-allocated and never freed individually.
 * See N4659 §6.9 [basic.types] for the C++17 type system.
 */

#include "parse.h"

/* ------------------------------------------------------------------ */
/* Type constructors                                                   */
/* ------------------------------------------------------------------ */

Type *new_type(Parser *p, TypeKind kind) {
    Type *ty = arena_alloc(p->arena, sizeof(Type));
    ty->kind = kind;
    ty->array_len = -1;  /* -1 = unsized for arrays; irrelevant for others */
    return ty;
}

/*
 * Pointer type — N4659 §11.3.1 [dcl.ptr]
 *   ptr-operator: * cv-qualifier-seq(opt)
 */
Type *new_ptr_type(Parser *p, Type *base) {
    Type *ty = new_type(p, TY_PTR);
    ty->base = base;
    return ty;
}

/*
 * Lvalue reference type — N4659 §11.3.2 [dcl.ref]
 *   ptr-operator: & attribute-specifier-seq(opt)
 *
 * N4659 §11.3.2/5: "There shall be no references to references,
 * no arrays of references, and no pointers to references."
 * (Reference collapsing is a sema concern, not parser.)
 */
Type *new_ref_type(Parser *p, Type *base) {
    Type *ty = new_type(p, TY_REF);
    ty->base = base;
    return ty;
}

/*
 * Rvalue reference type — N4659 §11.3.2 [dcl.ref] (C++11)
 *   ptr-operator: && attribute-specifier-seq(opt)
 *
 * C++11: rvalue references enable move semantics.
 * C++20/23: unchanged.
 */
Type *new_rvalref_type(Parser *p, Type *base) {
    Type *ty = new_type(p, TY_RVALREF);
    ty->base = base;
    return ty;
}

/*
 * Array type — N4659 §11.3.4 [dcl.array]
 *   noptr-declarator [ constant-expression(opt) ]
 *   len == -1 means unsized (e.g., 'int a[]').
 */
Type *new_array_type(Parser *p, Type *base, int len) {
    Type *ty = new_type(p, TY_ARRAY);
    ty->base = base;
    ty->array_len = len;
    return ty;
}

/*
 * Function type — N4659 §11.3.5 [dcl.fct]
 *   parameters-and-qualifiers:
 *       ( parameter-declaration-clause ) cv-qualifier-seq(opt)
 *           ref-qualifier(opt) noexcept-specifier(opt)
 *           attribute-specifier-seq(opt)
 *
 * C++11: trailing return types (auto f() -> int)
 * C++17: deduction guides
 * C++20: abbreviated function templates (void f(auto x))
 * C++23: deducing this (explicit object parameter)
 */
Type *new_func_type(Parser *p, Type *ret, Type **params, int nparams,
                    bool variadic) {
    Type *ty = new_type(p, TY_FUNC);
    ty->ret = ret;
    ty->params = params;
    ty->nparams = nparams;
    ty->is_variadic = variadic;
    return ty;
}

/*
 * parse_type_specifiers — N4659 §10.1.7 [dcl.type]
 *
 * Parses the decl-specifier-seq and produces a base Type.
 * Uses a counter-based state machine per §10.1.7.2/Table 10:
 *
 *   Specifier(s)                    Type
 *   ─────────────────────────────── ────────────
 *   void                            TY_VOID
 *   bool                            TY_BOOL
 *   char                            TY_CHAR
 *   signed char                     TY_CHAR (is_signed implicit)
 *   unsigned char                   TY_CHAR + is_unsigned
 *   char16_t                        TY_CHAR16
 *   char32_t                        TY_CHAR32
 *   wchar_t                         TY_WCHAR
 *   short [int]                     TY_SHORT
 *   unsigned short [int]            TY_SHORT + is_unsigned
 *   int                             TY_INT
 *   signed [int]                    TY_INT
 *   unsigned [int]                  TY_INT + is_unsigned
 *   long [int]                      TY_LONG
 *   unsigned long [int]             TY_LONG + is_unsigned
 *   long long [int]                 TY_LLONG
 *   unsigned long long [int]        TY_LLONG + is_unsigned
 *   float                           TY_FLOAT
 *   double                          TY_DOUBLE
 *   long double                     TY_LDOUBLE
 *
 * C++20: adds char8_t (§6.9.1)
 * C++23: no new fundamental types
 */
Type *parse_type_specifiers(Parser *p) {
    /* Counters for specifier combination tracking */
    int cnt_void = 0, cnt_bool = 0, cnt_char = 0;
    int cnt_short = 0, cnt_int = 0, cnt_long = 0;
    int cnt_float = 0, cnt_double = 0;
    int cnt_signed = 0, cnt_unsigned = 0;
    int cnt_char16 = 0, cnt_char32 = 0, cnt_wchar = 0;
    bool is_const = false, is_volatile = false;
    bool seen_any = false;

    /* Also consume storage-class and other specifiers that start declarations
     * N4659 §10.1.1 [dcl.stc]: static, extern, register, thread_local
     * N4659 §10.1.6 [dcl.inline]: inline
     * These don't affect the type but mark this as a declaration. */
    bool is_static = false, is_extern = false, is_inline = false;
    (void)is_static; (void)is_extern; (void)is_inline; /* used in later stages */

    /* Terminates: each iteration either consumes a type-specifier keyword
     * (advancing pos) and continues, or breaks. The token array is finite
     * and non-keyword tokens cause the break. */
    for (;;) {
        Token *tok = peek(p);

        if (tok->kind == TK_KW_CONST)    { advance(p); is_const = true; seen_any = true; continue; }
        if (tok->kind == TK_KW_VOLATILE) { advance(p); is_volatile = true; seen_any = true; continue; }
        if (tok->kind == TK_KW_STATIC)   { advance(p); is_static = true; seen_any = true; continue; }
        if (tok->kind == TK_KW_EXTERN)   { advance(p); is_extern = true; seen_any = true; continue; }
        if (tok->kind == TK_KW_INLINE)   { advance(p); is_inline = true; seen_any = true; continue; }
        if (tok->kind == TK_KW_REGISTER) { advance(p); seen_any = true; continue; }

        if (tok->kind == TK_KW_VOID)     { advance(p); cnt_void++; seen_any = true; continue; }
        if (tok->kind == TK_KW_BOOL)     { advance(p); cnt_bool++; seen_any = true; continue; }
        if (tok->kind == TK_KW_CHAR)     { advance(p); cnt_char++; seen_any = true; continue; }
        if (tok->kind == TK_KW_SHORT)    { advance(p); cnt_short++; seen_any = true; continue; }
        if (tok->kind == TK_KW_INT)      { advance(p); cnt_int++; seen_any = true; continue; }
        if (tok->kind == TK_KW_LONG)     { advance(p); cnt_long++; seen_any = true; continue; }
        if (tok->kind == TK_KW_FLOAT)    { advance(p); cnt_float++; seen_any = true; continue; }
        if (tok->kind == TK_KW_DOUBLE)   { advance(p); cnt_double++; seen_any = true; continue; }
        if (tok->kind == TK_KW_SIGNED)   { advance(p); cnt_signed++; seen_any = true; continue; }
        if (tok->kind == TK_KW_UNSIGNED) { advance(p); cnt_unsigned++; seen_any = true; continue; }
        if (tok->kind == TK_KW_CHAR16_T) { advance(p); cnt_char16++; seen_any = true; continue; }
        if (tok->kind == TK_KW_CHAR32_T) { advance(p); cnt_char32++; seen_any = true; continue; }
        if (tok->kind == TK_KW_WCHAR_T)  { advance(p); cnt_wchar++; seen_any = true; continue; }

        /* struct/union — N4659 §12 [class], §10.1.7.3 [dcl.type.elab]
         * For the first pass, consume tag name and skip brace-enclosed
         * body if present. Full member parsing deferred to Stage 2. */
        if (tok->kind == TK_KW_STRUCT || tok->kind == TK_KW_UNION) {
            TypeKind tk = (tok->kind == TK_KW_STRUCT) ? TY_STRUCT : TY_UNION;
            advance(p);
            Type *ty = new_type(p, tk);
            ty->is_const = is_const;
            ty->is_volatile = is_volatile;
            if (at(p, TK_IDENT))
                ty->tag = advance(p);
            /* N4659 §17.2 [temp.names]: struct/class followed by a
             * template-id: 'struct Foo<int>' — consume the template
             * argument list. This occurs in explicit specializations
             * and in elaborated-type-specifiers with template args. */
            if (ty->tag && at(p, TK_LT) && lookup_is_template_name(p, ty->tag)) {
                parse_template_id(p, ty->tag);
            }
            /* Skip class body { ... } if present — N4659 §12.1 [class.mem] */
            if (consume(p, TK_LBRACE)) {
                int depth = 1;
                while (depth > 0 && !at_eof(p)) {
                    if (consume(p, TK_LBRACE)) depth++;
                    else if (consume(p, TK_RBRACE)) depth--;
                    else advance(p);
                }
            }
            /* N4659 §6.3.2/7 [basic.scope.pdecl]: register tag name.
             * N4659 §6.3.10/2 [basic.scope.hiding]: also inject as
             * a type-name (C++ class name injection, §12.1/2) so
             * bare 'Foo' works without 'struct' prefix. */
            if (ty->tag) {
                region_declare(p, ty->tag->loc, ty->tag->len, ENTITY_TAG, ty);
                region_declare(p, ty->tag->loc, ty->tag->len, ENTITY_TYPE, ty);
            }
            return ty;
        }
        /* enum — N4659 §10.2 [dcl.enum]
         * Parse optional tag name and skip enumerator-list { ... }. */
        if (tok->kind == TK_KW_ENUM) {
            advance(p);
            Type *ty = new_type(p, TY_ENUM);
            ty->is_const = is_const;
            ty->is_volatile = is_volatile;
            /* enum class / enum struct (C++11 scoped enum) */
            if (at(p, TK_KW_CLASS) || at(p, TK_KW_STRUCT))
                advance(p);
            if (at(p, TK_IDENT))
                ty->tag = advance(p);
            /* Optional underlying type: enum E : int { ... } */
            if (consume(p, TK_COLON))
                parse_type_specifiers(p);  /* consume the underlying type */
            /* Enumerator list { ... } */
            if (consume(p, TK_LBRACE)) {
                int depth = 1;
                while (depth > 0 && !at_eof(p)) {
                    if (consume(p, TK_LBRACE)) depth++;
                    else if (consume(p, TK_RBRACE)) depth--;
                    else advance(p);
                }
            }
            /* Register enum tag — same as struct (§6.3.2/3) */
            if (ty->tag) {
                region_declare(p, ty->tag->loc, ty->tag->len, ENTITY_TAG, ty);
                region_declare(p, ty->tag->loc, ty->tag->len, ENTITY_TYPE, ty);
            }
            return ty;
        }

        break;  /* not a type specifier — stop */
    }

    /* N4659 §10.1.7.1 [dcl.type.simple]: a type-name (typedef-name,
     * class-name, or enum-name) can appear as a simple-type-specifier.
     * If no keyword specifiers have been seen, check if the current
     * token is a user-defined type-name via name lookup (§6.4). */
    if (!seen_any && peek(p)->kind == TK_IDENT) {
        Declaration *d = lookup_unqualified(p, peek(p)->loc, peek(p)->len);
        if (d && (d->entity == ENTITY_TYPE || d->entity == ENTITY_TAG ||
                  d->entity == ENTITY_TEMPLATE)) {
            Token *name_tok = advance(p);

            /* N4659 §17.2 [temp.names]: if the name is (also) a template-name
             * and is followed by <, parse the template-argument-list to form a
             * simple-template-id used as a type-specifier.
             * A name can be both a type and a template (e.g., after explicit
             * specialization registers the name as ENTITY_TYPE while the
             * primary template registered it as ENTITY_TEMPLATE). */
            if (at(p, TK_LT) && (d->entity == ENTITY_TEMPLATE ||
                lookup_unqualified_kind(p, name_tok->loc, name_tok->len,
                                        ENTITY_TEMPLATE) != NULL)) {
                parse_template_id(p, name_tok);  /* consumes <args> */
                /* For now, the resulting type is opaque — sema resolves
                 * the template specialization. */
                Type *ty = new_type(p, TY_STRUCT);
                ty->is_const = is_const;
                ty->is_volatile = is_volatile;
                ty->tag = name_tok;
                return ty;
            }

            Type *ty = d->type;
            if (ty) {
                /* Copy the type so we don't modify the declaration's type
                 * when adding cv-qualifiers */
                Type *copy = arena_alloc(p->arena, sizeof(Type));
                *copy = *ty;
                copy->is_const = is_const;
                copy->is_volatile = is_volatile;
                return copy;
            }
            /* Fallback: unknown type structure, create an opaque type */
            ty = new_type(p, TY_INT);
            ty->is_const = is_const;
            ty->is_volatile = is_volatile;
            ty->tag = name_tok;
            return ty;
        }
    }

    if (!seen_any) {
        if (p->tentative) return NULL;
        error_tok(peek(p), "expected type specifier");
    }

    /* Map specifier combination to TypeKind per §10.1.7.2/Table 10 */
    TypeKind kind;
    bool is_unsigned_flag = (cnt_unsigned > 0);

    if (cnt_void)        kind = TY_VOID;
    else if (cnt_bool)   kind = TY_BOOL;
    else if (cnt_char16) kind = TY_CHAR16;
    else if (cnt_char32) kind = TY_CHAR32;
    else if (cnt_wchar)  kind = TY_WCHAR;
    else if (cnt_float)  kind = TY_FLOAT;
    else if (cnt_double && cnt_long) kind = TY_LDOUBLE;
    else if (cnt_double) kind = TY_DOUBLE;
    else if (cnt_char)   kind = TY_CHAR;
    else if (cnt_short)  kind = TY_SHORT;
    else if (cnt_long >= 2) kind = TY_LLONG;
    else if (cnt_long == 1) kind = TY_LONG;
    else                 kind = TY_INT;  /* 'int', 'signed', 'unsigned' alone */

    Type *ty = new_type(p, kind);
    ty->is_unsigned = is_unsigned_flag;
    ty->is_const = is_const;
    ty->is_volatile = is_volatile;
    return ty;
}

/*
 * Check if the current token could start a type-specifier.
 *
 * Used for stmt-vs-decl disambiguation:
 *   N4659 §9.8 [stmt.ambig] (C++17)
 *   N4861 §8.9 [stmt.ambig] (C++20)
 *   N4950 §8.9 [stmt.ambig] (C++23)
 *
 * "any statement that could be a declaration IS a declaration"
 *
 * Checks built-in type keywords AND, via name lookup (§6.4), whether
 * an identifier is a user-defined type-name (§10.1.7.1).
 */
bool at_type_specifier(Parser *p) {
    switch (peek(p)->kind) {
    case TK_KW_VOID: case TK_KW_BOOL: case TK_KW_CHAR:
    case TK_KW_SHORT: case TK_KW_INT: case TK_KW_LONG:
    case TK_KW_FLOAT: case TK_KW_DOUBLE:
    case TK_KW_SIGNED: case TK_KW_UNSIGNED:
    case TK_KW_WCHAR_T: case TK_KW_CHAR16_T: case TK_KW_CHAR32_T:
    case TK_KW_CONST: case TK_KW_VOLATILE:
    case TK_KW_STATIC: case TK_KW_EXTERN: case TK_KW_REGISTER:
    case TK_KW_INLINE:
    case TK_KW_TYPEDEF:
    case TK_KW_STRUCT: case TK_KW_UNION: case TK_KW_ENUM:
    case TK_KW_AUTO:
        return true;
    case TK_IDENT:
        /* N4659 §10.1.7.1 [dcl.type.simple]: a type-name (typedef-name,
         * class-name, or enum-name) is a valid simple-type-specifier.
         * A template-name followed by <args> is also a type-specifier
         * (simple-template-id, §17.2).
         * Consult name lookup (§6.4) to determine if this identifier
         * refers to a type or template. */
        return lookup_is_type_name(p, peek(p)) ||
               lookup_is_template_name(p, peek(p));
    default:
        return false;
    }
}

/*
 * parse_type_name — N4659 §11.6.1 [dcl.name]
 *   type-id: type-specifier-seq abstract-declarator(opt)
 *
 * Used in casts: (int *)x, sizeof(unsigned long), etc.
 * The abstract-declarator is a declarator with no name.
 * For the first pass, we handle pointer-to and arrays only.
 */
Type *parse_type_name(Parser *p) {
    Type *base = parse_type_specifiers(p);
    if (!base) return NULL;

    /* Abstract ptr-operator: consume *, &, && — N4659 §11.3 [dcl.meaning]
     * Terminates: each iteration consumes one token (*, &, or &&) or breaks.
     * At most one & or && (references don't stack), and finite *'s. */
    for (;;) {
        if (consume(p, TK_STAR)) {
            base = new_ptr_type(p, base);
            while (at(p, TK_KW_CONST) || at(p, TK_KW_VOLATILE)) {
                if (consume(p, TK_KW_CONST))    base->is_const = true;
                if (consume(p, TK_KW_VOLATILE)) base->is_volatile = true;
            }
        } else if (consume(p, TK_LAND)) {
            base = new_rvalref_type(p, base);
        } else if (consume(p, TK_AMP)) {
            base = new_ref_type(p, base);
        } else {
            break;
        }
    }

    return base;
}
