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
Type *parse_type_specifiers(Parser *p, Node **class_def_out) {
    if (class_def_out) *class_def_out = NULL;
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
        Token *tok = parser_peek(p);

        /* Storage-class specifiers and cv-qualifiers are part of the
         * decl-specifier-seq (§10.1) but are NOT defining-type-specifiers.
         * They don't prevent a subsequent identifier from being a type name.
         * N4659 §10.1/3 [dcl.spec]: "a name is a type-name if no prior
         * defining-type-specifier has been seen." */
        if (tok->kind == TK_KW_CONST)    { parser_advance(p); is_const = true; continue; }
        if (tok->kind == TK_KW_VOLATILE) { parser_advance(p); is_volatile = true; continue; }
        if (tok->kind == TK_KW_STATIC)    { parser_advance(p); is_static = true; continue; }
        if (tok->kind == TK_KW_EXTERN)    { parser_advance(p); is_extern = true; continue; }
        if (tok->kind == TK_KW_INLINE)    { parser_advance(p); is_inline = true; continue; }
        if (tok->kind == TK_KW_REGISTER)  { parser_advance(p); continue; }
        if (tok->kind == TK_KW_CONSTEXPR) { parser_advance(p); continue; }
        if (tok->kind == TK_KW_VIRTUAL)   { parser_advance(p); continue; }
        if (tok->kind == TK_KW_EXPLICIT)  { parser_advance(p); continue; }
        if (tok->kind == TK_KW_MUTABLE)   { parser_advance(p); continue; }
        /* alignas(expr) — N4659 §10.1.2 [dcl.align] */
        if (tok->kind == TK_KW_ALIGNAS) {
            parser_advance(p);
            if (parser_consume(p, TK_LPAREN)) {
                /* Consume the alignment expression or type */
                int depth = 1;
                /* Terminates: balanced paren counting */
                while (depth > 0 && !parser_at_eof(p)) {
                    if (parser_at(p, TK_LPAREN)) depth++;
                    if (parser_at(p, TK_RPAREN)) depth--;
                    if (depth > 0) parser_advance(p);
                }
                parser_expect(p, TK_RPAREN);
            }
            continue;
        }

        if (tok->kind == TK_KW_VOID)     { parser_advance(p); cnt_void++; seen_any = true; continue; }
        if (tok->kind == TK_KW_BOOL)     { parser_advance(p); cnt_bool++; seen_any = true; continue; }
        if (tok->kind == TK_KW_CHAR)     { parser_advance(p); cnt_char++; seen_any = true; continue; }
        if (tok->kind == TK_KW_SHORT)    { parser_advance(p); cnt_short++; seen_any = true; continue; }
        if (tok->kind == TK_KW_INT)      { parser_advance(p); cnt_int++; seen_any = true; continue; }
        if (tok->kind == TK_KW_LONG)     { parser_advance(p); cnt_long++; seen_any = true; continue; }
        if (tok->kind == TK_KW_FLOAT)    { parser_advance(p); cnt_float++; seen_any = true; continue; }
        if (tok->kind == TK_KW_DOUBLE)   { parser_advance(p); cnt_double++; seen_any = true; continue; }
        if (tok->kind == TK_KW_SIGNED)   { parser_advance(p); cnt_signed++; seen_any = true; continue; }
        if (tok->kind == TK_KW_UNSIGNED) { parser_advance(p); cnt_unsigned++; seen_any = true; continue; }
        if (tok->kind == TK_KW_CHAR16_T) { parser_advance(p); cnt_char16++; seen_any = true; continue; }
        if (tok->kind == TK_KW_CHAR32_T) { parser_advance(p); cnt_char32++; seen_any = true; continue; }
        if (tok->kind == TK_KW_WCHAR_T)  { parser_advance(p); cnt_wchar++; seen_any = true; continue; }

        /* struct/union/class — N4659 §12 [class], §10.1.7.3 [dcl.type.elab]
         *
         *   class-specifier:
         *       class-head { member-specification(opt) }
         *   class-head:
         *       class-key attribute-specifier-seq(opt) class-head-name
         *           base-clause(opt)
         *   class-key: struct | class | union
         *
         *   member-specification:
         *       member-declaration member-specification(opt)
         *       access-specifier : member-specification(opt)
         *
         *   member-declaration:
         *       decl-specifier-seq(opt) member-declarator-list(opt) ;
         *       function-definition
         *       template-declaration
         *       // and others (using, static_assert, etc.)
         *
         * C++20: no structural grammar changes to class bodies.
         * C++23: deducing this.
         */
        if (tok->kind == TK_KW_STRUCT || tok->kind == TK_KW_UNION ||
            tok->kind == TK_KW_CLASS) {
            TypeKind tk = (tok->kind == TK_KW_UNION) ? TY_UNION : TY_STRUCT;
            parser_advance(p);
            Type *ty = new_type(p, tk);
            ty->is_const = is_const;
            ty->is_volatile = is_volatile;

            /* Optional tag name */
            if (parser_at(p, TK_IDENT))
                ty->tag = parser_advance(p);

            /* N4659 §17.2 [temp.names]: struct/class followed by a
             * template-id (e.g., 'struct Foo<int>') */
            if (ty->tag && parser_at(p, TK_LT) &&
                lookup_is_template_name(p, ty->tag)) {
                parse_template_id(p, ty->tag);
            }

            /* N4659 §6.3.2/7 [basic.scope.pdecl]: register tag name
             * before parsing the body so members can reference the class.
             * N4659 §6.3.10/2 [basic.scope.hiding]: C++ class name
             * injection — bare 'Foo' works without 'struct' prefix. */
            if (ty->tag) {
                region_declare(p, ty->tag->loc, ty->tag->len, ENTITY_TAG, ty);
                region_declare(p, ty->tag->loc, ty->tag->len, ENTITY_TYPE, ty);
            }

            /* Base clause: : public Base, private Other (deferred — just skip)
             * N4659 §13.1 [class.derived] */
            if (parser_consume(p, TK_COLON)) {
                /* Skip base-specifier-list until { */
                /* Terminates: advances toward { or EOF. */
                while (!parser_at(p, TK_LBRACE) && !parser_at_eof(p))
                    parser_advance(p);
            }

            /* Class body { member-specification } */
            if (parser_consume(p, TK_LBRACE)) {
                /* N4659 §6.3.7 [basic.scope.class]: push class scope */
                region_push(p, REGION_CLASS, /*name=*/NULL);

                Vec members = vec_new(p->arena);

                /* Terminates: each iteration consumes at least one token
                 * (a member declaration, access specifier, or ;). Breaks
                 * on } or EOF. */
                while (!parser_at(p, TK_RBRACE) && !parser_at_eof(p)) {
                    /* access-specifier : — §12.2 [class.access.spec]
                     *   public: | protected: | private: */
                    if ((parser_at(p, TK_KW_PUBLIC) || parser_at(p, TK_KW_PROTECTED) ||
                         parser_at(p, TK_KW_PRIVATE)) &&
                        parser_peek_ahead(p, 1)->kind == TK_COLON) {
                        Token *acc_tok = parser_advance(p);
                        parser_advance(p);  /* consume : */
                        Node *acc = new_node(p, ND_ACCESS_SPEC, acc_tok);
                        acc->access_spec.access = acc_tok->kind;
                        vec_push(&members, acc);
                        continue;
                    }

                    /* Empty declaration ( ; ) */
                    if (parser_consume(p, TK_SEMI))
                        continue;

                    /* Skip preprocessor leftovers inside class body */
                    if (parser_at(p, TK_HASH)) {
                        int line = parser_peek(p)->line;
                        while (!parser_at_eof(p) && parser_peek(p)->line == line)
                            parser_advance(p);
                        continue;
                    }

                    /* template-declaration inside class */
                    if (parser_at(p, TK_KW_TEMPLATE)) {
                        vec_push(&members, parse_template_declaration(p));
                        continue;
                    }

                    /* member-declaration: parsed as a regular declaration.
                     * This handles data members, method declarations, method
                     * definitions (with body), nested types, typedefs, etc.
                     * N4659 §12.1 [class.mem] */
                    Node *member = parse_declaration(p);
                    if (member)
                        vec_push(&members, member);
                }

                parser_expect(p, TK_RBRACE);
                region_pop(p);

                /* Return class definition via out-parameter */
                if (class_def_out) {
                    Node *cdef = new_node(p, ND_CLASS_DEF, ty->tag ? ty->tag : parser_peek(p));
                    cdef->class_def.tag = ty->tag;
                    cdef->class_def.members = (Node **)members.data;
                    cdef->class_def.nmembers = members.len;
                    *class_def_out = cdef;
                }
            }

            return ty;
        }
        /* enum — N4659 §10.2 [dcl.enum]
         * Parse optional tag name and skip enumerator-list { ... }. */
        if (tok->kind == TK_KW_ENUM) {
            parser_advance(p);
            Type *ty = new_type(p, TY_ENUM);
            ty->is_const = is_const;
            ty->is_volatile = is_volatile;
            /* enum class / enum struct (C++11 scoped enum) */
            if (parser_at(p, TK_KW_CLASS) || parser_at(p, TK_KW_STRUCT))
                parser_advance(p);
            if (parser_at(p, TK_IDENT))
                ty->tag = parser_advance(p);
            /* Optional underlying type: enum E : int { ... } */
            if (parser_consume(p, TK_COLON))
                parse_type_specifiers(p, /*class_def_out=*/NULL);  /* consume the underlying type */
            /* Enumerator list { ... } */
            if (parser_consume(p, TK_LBRACE)) {
                int depth = 1;
                while (depth > 0 && !parser_at_eof(p)) {
                    if (parser_consume(p, TK_LBRACE)) depth++;
                    else if (parser_consume(p, TK_RBRACE)) depth--;
                    else parser_advance(p);
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
     * Also handles qualified type names: A::B::Type (§8.1.4.3).
     * If no keyword specifiers have been seen, check if the current
     * token is a user-defined type-name via name lookup (§6.4). */
    if (!seen_any && parser_peek(p)->kind == TK_IDENT &&
        parser_peek_ahead(p, 1)->kind == TK_SCOPE) {
        /* Qualified type name: A::B::C — consume the chain.
         * The parser doesn't resolve qualified lookup; treat as opaque type.
         * Terminates: each iteration consumes ident + ::, or breaks. */
        Token *first = parser_advance(p);
        while (parser_consume(p, TK_SCOPE)) {
            if (parser_at(p, TK_IDENT))
                parser_advance(p);
            else
                break;
        }
        /* Consume trailing template-id if present: A::B<int> */
        if (parser_at(p, TK_LT)) {
            /* Try to parse as template-id — may or may not be one.
             * For qualified names we speculatively consume < args >.
             * If it's actually less-than, sema will catch it. */
            Token *last = &p->tokens[p->pos > 0 ? p->pos - 1 : 0];
            if (lookup_is_template_name(p, last))
                parse_template_id(p, last);
        }
        Type *ty = new_type(p, TY_STRUCT);  /* opaque — sema resolves */
        ty->is_const = is_const;
        ty->is_volatile = is_volatile;
        ty->tag = first;
        return ty;
    }
    if (!seen_any && parser_peek(p)->kind == TK_IDENT) {
        Declaration *d = lookup_unqualified(p, parser_peek(p)->loc, parser_peek(p)->len);
        if (d && (d->entity == ENTITY_TYPE || d->entity == ENTITY_TAG ||
                  d->entity == ENTITY_TEMPLATE)) {
            Token *name_tok = parser_advance(p);

            /* N4659 §17.2 [temp.names]: if the name is (also) a template-name
             * and is followed by <, parse the template-argument-list to form a
             * simple-template-id used as a type-specifier.
             * A name can be both a type and a template (e.g., after explicit
             * specialization registers the name as ENTITY_TYPE while the
             * primary template registered it as ENTITY_TEMPLATE). */
            if (parser_at(p, TK_LT) && (d->entity == ENTITY_TEMPLATE ||
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
        error_tok(parser_peek(p), "expected type specifier");
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
bool parser_at_type_specifier(Parser *p) {
    switch (parser_peek(p)->kind) {
    case TK_KW_VOID: case TK_KW_BOOL: case TK_KW_CHAR:
    case TK_KW_SHORT: case TK_KW_INT: case TK_KW_LONG:
    case TK_KW_FLOAT: case TK_KW_DOUBLE:
    case TK_KW_SIGNED: case TK_KW_UNSIGNED:
    case TK_KW_WCHAR_T: case TK_KW_CHAR16_T: case TK_KW_CHAR32_T:
    case TK_KW_CONST: case TK_KW_VOLATILE:
    case TK_KW_STATIC: case TK_KW_EXTERN: case TK_KW_REGISTER:
    case TK_KW_INLINE:
    case TK_KW_TYPEDEF:
    case TK_KW_STRUCT: case TK_KW_CLASS: case TK_KW_UNION: case TK_KW_ENUM:
    case TK_KW_AUTO:
    case TK_KW_CONSTEXPR: case TK_KW_VIRTUAL: case TK_KW_EXPLICIT:
    case TK_KW_MUTABLE:
        return true;
    case TK_IDENT:
        /* N4659 §10.1.7.1 [dcl.type.simple]: a type-name (typedef-name,
         * class-name, or enum-name) is a valid simple-type-specifier.
         * A template-name followed by <args> is also a type-specifier
         * (simple-template-id, §17.2).
         * A qualified name (A::B) is treated as a potential type —
         * the parser can't resolve qualified lookup, so we assume
         * it's a type if it starts with ident::. Sema validates. */
        if (parser_peek_ahead(p, 1)->kind == TK_SCOPE)
            return true;  /* qualified name — assume type */
        return lookup_is_type_name(p, parser_peek(p)) ||
               lookup_is_template_name(p, parser_peek(p));
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
    Type *base = parse_type_specifiers(p, /*class_def_out=*/NULL);
    if (!base) return NULL;

    /* Abstract ptr-operator: consume *, &, && — N4659 §11.3 [dcl.meaning]
     * Terminates: each iteration consumes one token (*, &, or &&) or breaks.
     * At most one & or && (references don't stack), and finite *'s. */
    for (;;) {
        if (parser_consume(p, TK_STAR)) {
            base = new_ptr_type(p, base);
            while (parser_at(p, TK_KW_CONST) || parser_at(p, TK_KW_VOLATILE)) {
                if (parser_consume(p, TK_KW_CONST))    base->is_const = true;
                if (parser_consume(p, TK_KW_VOLATILE)) base->is_volatile = true;
            }
        } else if (parser_consume(p, TK_LAND)) {
            base = new_rvalref_type(p, base);
        } else if (parser_consume(p, TK_AMP)) {
            base = new_ref_type(p, base);
        } else {
            break;
        }
    }

    return base;
}
