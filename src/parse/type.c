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
DeclSpec parse_type_specifiers(Parser *p) {
    DeclSpec result = {0};

    /* Counters for specifier combination tracking */
    int cnt_void = 0, cnt_bool = 0, cnt_char = 0;
    int cnt_short = 0, cnt_int = 0, cnt_long = 0;
    int cnt_float = 0, cnt_double = 0;
    int cnt_signed = 0, cnt_unsigned = 0;
    int cnt_char16 = 0, cnt_char32 = 0, cnt_wchar = 0;
    bool is_const = false, is_volatile = false;
    bool seen_any = false;

    /* Terminates: each iteration either consumes a type-specifier keyword
     * (advancing pos) and continues, or breaks. The token array is finite
     * and non-keyword tokens cause the break. */
    for (;;) {
        /* GCC __attribute__((...)) — may appear anywhere in the
         * decl-specifier-seq in glibc/libstdc++ headers. */
        parser_skip_gnu_attributes(p);

        Token *tok = parser_peek(p);

        /* Storage-class specifiers and cv-qualifiers are part of the
         * decl-specifier-seq (§10.1) but are NOT defining-type-specifiers.
         * They don't prevent a subsequent identifier from being a type name.
         * N4659 §10.1/3 [dcl.spec]: "a name is a type-name if no prior
         * defining-type-specifier has been seen." */
        if (tok->kind == TK_KW_CONST)     { parser_advance(p); is_const = true; continue; }
        if (tok->kind == TK_KW_VOLATILE)  { parser_advance(p); is_volatile = true; continue; }
        if (tok->kind == TK_KW_STATIC)    { parser_advance(p); result.flags |= DECL_STATIC; continue; }
        if (tok->kind == TK_KW_EXTERN)    { parser_advance(p); result.flags |= DECL_EXTERN; continue; }
        if (tok->kind == TK_KW_INLINE)    { parser_advance(p); result.flags |= DECL_INLINE; continue; }
        if (tok->kind == TK_KW_REGISTER)  { parser_advance(p); result.flags |= DECL_REGISTER; continue; }
        if (tok->kind == TK_KW_CONSTEXPR) { parser_advance(p); result.flags |= DECL_CONSTEXPR; continue; }
        if (tok->kind == TK_KW_VIRTUAL)   { parser_advance(p); result.flags |= DECL_VIRTUAL; continue; }
        if (tok->kind == TK_KW_EXPLICIT)  { parser_advance(p); result.flags |= DECL_EXPLICIT; continue; }
        if (tok->kind == TK_KW_MUTABLE)   { parser_advance(p); result.flags |= DECL_MUTABLE; continue; }
        /* typename qualified-name — N4659 §17.6 [temp.res]
         * Disambiguates a dependent qualified name as a type. Consume the
         * keyword and let the following identifier/qualified-id be parsed
         * as the type-specifier. */
        if (tok->kind == TK_KW_TYPENAME)  { parser_advance(p); continue; }
        /* auto — N4659 §10.1.7.4 [dcl.spec.auto]
         * Placeholder type, deduced from initializer or trailing return.
         * Treat as opaque (TY_INT placeholder); sema does deduction. */
        if (tok->kind == TK_KW_AUTO) {
            parser_advance(p);
            Type *ty = new_type(p, TY_INT);
            ty->is_const = is_const;
            ty->is_volatile = is_volatile;
            result.type = ty;
            return result;
        }
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

        /* decltype(expression) — N4659 §10.1.7.2 [dcl.type.simple]
         *   simple-type-specifier: decltype ( expression )
         * Acts as the entire type-specifier; consume balanced parens and
         * return an opaque type. Sema deduces the actual type. */
        if (tok->kind == TK_KW_DECLTYPE) {
            parser_advance(p);
            parser_expect(p, TK_LPAREN);
            int depth = 1;
            while (depth > 0 && !parser_at_eof(p)) {
                if (parser_at(p, TK_LPAREN)) depth++;
                else if (parser_at(p, TK_RPAREN)) {
                    depth--;
                    if (depth == 0) break;
                }
                parser_advance(p);
            }
            parser_expect(p, TK_RPAREN);
            Type *ty = new_type(p, TY_INT);  /* opaque placeholder */
            ty->is_const = is_const;
            ty->is_volatile = is_volatile;
            result.type = ty;
            return result;
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

            /* GCC attributes between 'class'/'struct' and the tag name. */
            parser_skip_gnu_attributes(p);

            /* Optional tag name. May be qualified — 'class A::B { ... }'
             * — and segments may themselves be template-ids:
             * 'class basic_ostream<C, T>::sentry'. */
            if (parser_at(p, TK_IDENT)) {
                ty->tag = parser_advance(p);
                if (parser_at(p, TK_LT))
                    parse_template_id(p, ty->tag);
                while (parser_consume(p, TK_SCOPE)) {
                    if (parser_at(p, TK_IDENT)) {
                        ty->tag = parser_advance(p);
                        if (parser_at(p, TK_LT))
                            parse_template_id(p, ty->tag);
                    } else {
                        break;
                    }
                }
            }

            /* N4659 §17.2 [temp.names]: struct/class followed by a
             * template-id (e.g., 'struct Foo<int>') */
            if (ty->tag && parser_at(p, TK_LT)) {
                /* Speculative — in 'class Foo<...>' position '<' is
                 * always a template-argument-list. Don't require lookup,
                 * since the template name may live in an inline namespace
                 * we don't yet model. */
                parse_template_id(p, ty->tag);
            }

            /* N4659 §6.3.2/7 [basic.scope.pdecl]: register tag name
             * before parsing the body so members can reference the class.
             * N4659 §6.3.10/2 [basic.scope.hiding]: C++ class name
             * injection — bare 'Foo' works without 'struct' prefix. */
            if (ty->tag) {
                region_declare(p, ty->tag->loc, ty->tag->len, ENTITY_TAG, ty);
                region_declare(p, ty->tag->loc, ty->tag->len, ENTITY_TYPE, ty);
                /* If we're inside a template-parameter region, this class
                 * is itself a template. Register the injected-class-name
                 * as ENTITY_TEMPLATE so its body can refer to itself with
                 * a template-id (e.g. 'pair<U1,U2>' inside 'pair'). */
                for (DeclarativeRegion *r = p->region; r; r = r->enclosing) {
                    if (r->kind == REGION_TEMPLATE) {
                        region_declare(p, ty->tag->loc, ty->tag->len,
                                       ENTITY_TEMPLATE, /*type=*/NULL);
                        break;
                    }
                }
            }

            /* Base clause — N4659 §13.1 [class.derived]
             *   base-clause: : base-specifier-list
             *   base-specifier:
             *       attribute-specifier-seq(opt) class-or-decltype
             *     | attribute-specifier-seq(opt) virtual access-specifier(opt) class-or-decltype
             *     | attribute-specifier-seq(opt) access-specifier virtual(opt) class-or-decltype
             *
             * We parse each base-specifier as a type-name and remember
             * the resolved class_region (if any). The bases will be
             * attached to the class scope after region_push below. */
            #define MAX_BASES 16
            DeclarativeRegion *base_regions[MAX_BASES];
            int n_base_regions = 0;
            if (parser_consume(p, TK_COLON)) {
                for (;;) {
                    parser_skip_cxx_attributes(p);
                    parser_skip_gnu_attributes(p);
                    /* Optional virtual + access-specifier in either order. */
                    for (int i = 0; i < 2; i++) {
                        if (parser_consume(p, TK_KW_VIRTUAL)) continue;
                        if (parser_consume(p, TK_KW_PUBLIC) ||
                            parser_consume(p, TK_KW_PROTECTED) ||
                            parser_consume(p, TK_KW_PRIVATE))
                            continue;
                        break;
                    }
                    /* Parse the base-class-name as a type-specifier.
                     * Tentative — a SFINAE template-arg that contains
                     * arbitrary expressions could trip us up; on
                     * failure, fall back to the depth-tracking skipper. */
                    ParseState saved = parser_save(p);
                    bool prev_t = p->tentative;
                    bool saved_failed = p->tentative_failed;
                    p->tentative = true;
                    p->tentative_failed = false;
                    Type *base_ty = parse_type_specifiers(p).type;
                    bool ok = (base_ty != NULL) && !p->tentative_failed &&
                              (parser_at(p, TK_COMMA) || parser_at(p, TK_LBRACE));
                    p->tentative = prev_t;
                    p->tentative_failed = saved_failed;
                    if (!ok) {
                        /* Couldn't parse this base-specifier as a clean
                         * type — skip with depth tracking and bail out
                         * of the base-list. */
                        parser_restore(p, saved);
                        int paren = 0, angle = 0;
                        TokenKind prev = TK_EOF;
                        while (!parser_at_eof(p)) {
                            if (paren == 0 && angle == 0 && parser_at(p, TK_LBRACE))
                                break;
                            if (parser_at(p, TK_LBRACE)) {
                                int d = 0;
                                do {
                                    if (parser_at(p, TK_LBRACE)) d++;
                                    else if (parser_at(p, TK_RBRACE)) d--;
                                    parser_advance(p);
                                } while (d > 0 && !parser_at_eof(p));
                                continue;
                            }
                            if (parser_at(p, TK_LPAREN)) paren++;
                            else if (parser_at(p, TK_RPAREN)) paren--;
                            else if (parser_at(p, TK_LT)) {
                                if (paren == 0 && prev != TK_RPAREN && prev != TK_NUM)
                                    angle++;
                            }
                            else if (parser_at(p, TK_GT)) {
                                if (paren == 0) angle--;
                            }
                            else if (parser_at(p, TK_SHR)) {
                                if (paren == 0) angle -= 2;
                            }
                            prev = parser_peek(p)->kind;
                            parser_advance(p);
                        }
                        break;
                    }
                    /* Re-parse committed and record the class_region. */
                    parser_restore(p, saved);
                    base_ty = parse_type_specifiers(p).type;
                    if (base_ty && base_ty->class_region &&
                        n_base_regions < MAX_BASES)
                        base_regions[n_base_regions++] = base_ty->class_region;
                    /* Pack expansion '...' on a base-class type. */
                    parser_consume(p, TK_ELLIPSIS);
                    if (!parser_consume(p, TK_COMMA))
                        break;
                }
            }
            #undef MAX_BASES

            /* Class body { member-specification } */
            if (parser_consume(p, TK_LBRACE)) {
                /* N4659 §6.3.7 [basic.scope.class]: push class scope */
                region_push(p, REGION_CLASS, /*name=*/NULL);
                /* Record the class's body region on the type so qualified
                 * lookups (Foo::bar) and base-class chains can find it. */
                ty->class_region = p->region;

                /* Attach base-class regions captured above. Lookup of an
                 * unqualified name in this scope walks bases after the
                 * class's own buckets. */
                for (int i = 0; i < n_base_regions; i++)
                    region_add_base(p, base_regions[i]);

                /* N4659 §9.2/2 [class]: the class-name is also inserted
                 * into the scope of the class itself ("injected class
                 * name"). This makes the bare name usable inside the body
                 * even when it would otherwise be shadowed. */
                if (ty->tag) {
                    region_declare(p, ty->tag->loc, ty->tag->len,
                                   ENTITY_TYPE, ty);
                    region_declare(p, ty->tag->loc, ty->tag->len,
                                   ENTITY_TAG, ty);
                }

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

                    /* C++11 / GCC attributes before any member declaration. */
                    parser_skip_cxx_attributes(p);
                    parser_skip_gnu_attributes(p);

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

                /* Record class definition on the result */
                result.class_def = new_class_def_node(p, ty->tag,
                    (Node **)members.data, members.len,
                    ty->tag ? ty->tag : parser_peek(p));
            }

            result.type = ty; return result;
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
                parse_type_specifiers(p);  /* consume the underlying type */
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
            result.type = ty; return result;
        }

        break;  /* not a type specifier — stop */
    }

    /* N4659 §10.1.7.1 [dcl.type.simple]: a type-name (typedef-name,
     * class-name, or enum-name) can appear as a simple-type-specifier.
     * Also handles qualified type names: A::B::Type (§8.1.4.3).
     * If no keyword specifiers have been seen, check if the current
     * token is a user-defined type-name via name lookup (§6.4). */
    /* Leading global scope: ::A::B::C is a fully qualified type name. */
    if (!seen_any && parser_at(p, TK_SCOPE)) {
        parser_advance(p);  /* consume leading :: */
        /* fall through to qualified-name handling below */
    }
    if (!seen_any && parser_peek(p)->kind == TK_IDENT &&
        parser_peek_ahead(p, 1)->kind == TK_SCOPE) {
        /* Qualified type name: A::B::C — N4659 §6.4.3 [basic.lookup.qual].
         *
         * Walk the chain via real lookup when possible. Each segment is
         * resolved in the previous segment's scope (a namespace's
         * ns_region or a class's class_region). If at any point lookup
         * fails or a segment has no nested scope, we fall back to the
         * opaque-type behavior the parser always had. */
        Token *first = parser_advance(p);
        Declaration *resolved = lookup_unqualified(p, first->loc, first->len);
        DeclarativeRegion *scope = NULL;
        if (resolved) {
            if (resolved->entity == ENTITY_NAMESPACE)
                scope = resolved->ns_region;
            else if (resolved->type && resolved->type->class_region)
                scope = resolved->type->class_region;
        }
        Token *last_seg = first;
        while (parser_consume(p, TK_SCOPE)) {
            parser_consume(p, TK_KW_TEMPLATE);
            if (parser_at(p, TK_IDENT)) {
                Token *seg = parser_advance(p);
                last_seg = seg;
                /* Speculative template-id on each segment: A::B<int>::C.
                 * Template arguments make the segment's type opaque from
                 * lookup's perspective — drop scope. */
                if (parser_at(p, TK_LT)) {
                    parse_template_id(p, seg);
                    resolved = NULL;
                    scope = NULL;
                    continue;
                }
                /* Look up this segment in the current scope. If we still
                 * have a scope, use it; if not, we're walking blindly. */
                Declaration *next = scope
                    ? lookup_in_scope(scope, seg->loc, seg->len)
                    : NULL;
                resolved = next;
                if (next && next->entity == ENTITY_NAMESPACE)
                    scope = next->ns_region;
                else if (next && next->type && next->type->class_region)
                    scope = next->type->class_region;
                else
                    scope = NULL;
            } else {
                break;
            }
        }
        /* Trailing east-const cv-qualifiers: 'A::B const' / 'A::B volatile' */
        while (parser_at(p, TK_KW_CONST) || parser_at(p, TK_KW_VOLATILE)) {
            if (parser_consume(p, TK_KW_CONST))    is_const = true;
            if (parser_consume(p, TK_KW_VOLATILE)) is_volatile = true;
        }
        /* If the chain resolved cleanly to a known type, return THAT
         * type with its tag and class_region intact. Otherwise opaque. */
        if (resolved && resolved->type &&
            (resolved->entity == ENTITY_TYPE ||
             resolved->entity == ENTITY_TAG)) {
            Type *copy = arena_alloc(p->arena, sizeof(Type));
            *copy = *resolved->type;
            copy->is_const = is_const;
            copy->is_volatile = is_volatile;
            result.type = copy; return result;
        }
        Type *ty = new_type(p, TY_STRUCT);  /* opaque — sema resolves */
        ty->is_const = is_const;
        ty->is_volatile = is_volatile;
        ty->tag = last_seg;
        result.type = ty; return result;
    }
    /* GCC/Clang type intrinsics: __underlying_type(T), __remove_cv(T), etc.
     * Recognise __identifier ( ... ) and consume the balanced parens. */
    if (!seen_any && parser_peek(p)->kind == TK_IDENT &&
        parser_peek(p)->len >= 2 && parser_peek(p)->loc[0] == '_' &&
        parser_peek(p)->loc[1] == '_' &&
        parser_peek_ahead(p, 1)->kind == TK_LPAREN &&
        lookup_unqualified(p, parser_peek(p)->loc, parser_peek(p)->len) == NULL) {
        Token *name_tok = parser_advance(p);
        parser_advance(p);  /* ( */
        int depth = 1;
        while (depth > 0 && !parser_at_eof(p)) {
            if (parser_at(p, TK_LPAREN)) depth++;
            else if (parser_at(p, TK_RPAREN)) {
                depth--;
                if (depth == 0) break;
            }
            parser_advance(p);
        }
        parser_expect(p, TK_RPAREN);
        Type *ty = new_type(p, TY_INT);  /* opaque */
        ty->is_const = is_const;
        ty->is_volatile = is_volatile;
        ty->tag = name_tok;
        result.type = ty;
        return result;
    }

    /* Unknown identifier followed by < or :: in a type position — assume
     * it's a (template) type-name. This handles names inherited from base
     * classes, which we don't yet model with proper inheritance lookup. */
    if (!seen_any && parser_peek(p)->kind == TK_IDENT &&
        lookup_unqualified(p, parser_peek(p)->loc, parser_peek(p)->len) == NULL &&
        (parser_peek_ahead(p, 1)->kind == TK_LT ||
         parser_peek_ahead(p, 1)->kind == TK_SCOPE)) {
        Token *name_tok = parser_advance(p);
        if (parser_at(p, TK_LT))
            parse_template_id(p, name_tok);
        while (parser_consume(p, TK_SCOPE)) {
            parser_consume(p, TK_KW_TEMPLATE);
            if (parser_at(p, TK_IDENT)) {
                Token *seg = parser_advance(p);
                if (parser_at(p, TK_LT))
                    parse_template_id(p, seg);
            }
        }
        while (parser_at(p, TK_KW_CONST) || parser_at(p, TK_KW_VOLATILE)) {
            if (parser_consume(p, TK_KW_CONST))    is_const = true;
            if (parser_consume(p, TK_KW_VOLATILE)) is_volatile = true;
        }
        Type *ty = new_type(p, TY_STRUCT);
        ty->is_const = is_const;
        ty->is_volatile = is_volatile;
        ty->tag = name_tok;
        result.type = ty;
        return result;
    }

    if (!seen_any && parser_peek(p)->kind == TK_IDENT) {
        /* Kind-specific lookup: a name may be ambiguously registered as
         * both a type and (e.g.) a constructor function. We want to find
         * any type-like declaration in the chain even when a same-named
         * variable shadows it. */
        Declaration *d =
            lookup_unqualified_kind(p, parser_peek(p)->loc, parser_peek(p)->len, ENTITY_TYPE);
        if (!d)
            d = lookup_unqualified_kind(p, parser_peek(p)->loc, parser_peek(p)->len, ENTITY_TAG);
        if (!d)
            d = lookup_unqualified_kind(p, parser_peek(p)->loc, parser_peek(p)->len, ENTITY_TEMPLATE);
        if (d) {
            Token *name_tok = parser_advance(p);

            /* N4659 §17.2 [temp.names]: if the name is (also) a template-name
             * and is followed by <, parse the template-argument-list to form a
             * simple-template-id used as a type-specifier.
             * A name can be both a type and a template (e.g., after explicit
             * specialization registers the name as ENTITY_TYPE while the
             * primary template registered it as ENTITY_TEMPLATE). */
            /* Speculative — '<' after a user-defined type-name in a type
             * position is overwhelmingly a template-argument-list, not
             * less-than. Our lookup may not find the template entity if
             * it lives in an inline namespace we don't model. */
            if (parser_at(p, TK_LT)) {
                parse_template_id(p, name_tok);  /* consumes <args> */
                /* Trailing nested-name: enable_if<...>::type, possibly chained.
                 * Each segment may itself be a template-id. */
                while (parser_consume(p, TK_SCOPE)) {
                    /* N4659 §17.2/4 [temp.names]: 'template' keyword
                     * disambiguates a dependent member template-id. */
                    parser_consume(p, TK_KW_TEMPLATE);
                    if (parser_at(p, TK_IDENT)) {
                        Token *seg = parser_advance(p);
                        if (parser_at(p, TK_LT))
                            parse_template_id(p, seg);
                    }
                }
                /* For now, the resulting type is opaque — sema resolves
                 * the template specialization. */
                Type *ty = new_type(p, TY_STRUCT);
                ty->is_const = is_const;
                ty->is_volatile = is_volatile;
                ty->tag = name_tok;
                result.type = ty; return result;
            }

            /* Trailing nested-name: T::U::V, possibly with template-ids.
             * Walk via real lookup when possible — N4659 §6.4.3. */
            DeclarativeRegion *qscope2 =
                (d->type && d->type->class_region) ? d->type->class_region : NULL;
            Declaration *qres2 = d;
            while (parser_consume(p, TK_SCOPE)) {
                parser_consume(p, TK_KW_TEMPLATE);
                if (parser_at(p, TK_IDENT)) {
                    Token *seg = parser_advance(p);
                    if (parser_at(p, TK_LT) &&
                        lookup_is_template_name(p, seg)) {
                        parse_template_id(p, seg);
                        qres2 = NULL;
                        qscope2 = NULL;
                    } else if (qscope2) {
                        Declaration *next = lookup_in_scope(qscope2, seg->loc, seg->len);
                        qres2 = next;
                        if (next && next->type && next->type->class_region)
                            qscope2 = next->type->class_region;
                        else if (next && next->entity == ENTITY_NAMESPACE)
                            qscope2 = next->ns_region;
                        else
                            qscope2 = NULL;
                    } else {
                        qres2 = NULL;
                    }
                }
            }
            /* If the chain resolved through to a real type, use it
             * instead of d->type (the leading segment). */
            if (qres2 && qres2 != d && qres2->type &&
                (qres2->entity == ENTITY_TYPE || qres2->entity == ENTITY_TAG))
                d = qres2;

            /* Trailing decl-specifiers: cv-qualifiers (§10.1.7.1) and
             * storage-class / function-specifier keywords that may
             * appear in any order with the type-specifier (§10.1). */
            for (;;) {
                if (parser_consume(p, TK_KW_CONST))    { is_const = true;    continue; }
                if (parser_consume(p, TK_KW_VOLATILE)) { is_volatile = true; continue; }
                if (parser_consume(p, TK_KW_INLINE) ||
                    parser_consume(p, TK_KW_STATIC) ||
                    parser_consume(p, TK_KW_EXTERN) ||
                    parser_consume(p, TK_KW_REGISTER) ||
                    parser_consume(p, TK_KW_CONSTEXPR) ||
                    parser_consume(p, TK_KW_VIRTUAL) ||
                    parser_consume(p, TK_KW_EXPLICIT) ||
                    parser_consume(p, TK_KW_MUTABLE))
                    continue;
                break;
            }

            Type *ty = d->type;
            if (ty) {
                /* Copy the type so we don't modify the declaration's type
                 * when adding cv-qualifiers */
                Type *copy = arena_alloc(p->arena, sizeof(Type));
                *copy = *ty;
                copy->is_const = is_const;
                copy->is_volatile = is_volatile;
                result.type = copy; return result;
            }
            /* Fallback: unknown type structure, create an opaque type */
            ty = new_type(p, TY_INT);
            ty->is_const = is_const;
            ty->is_volatile = is_volatile;
            ty->tag = name_tok;
            result.type = ty; return result;
        }
    }

    /* Heuristic fallback: an unknown identifier in a type-position
     * context — accept it as an opaque type-name when followed by a
     * token that can't begin/continue an expression but CAN follow a
     * type-name (another ident, '*'/'&'/'&&', or template-arg/cast
     * delimiters '>' / ')' / ','). This handles inherited member
     * typedefs and templated-namespace types we don't yet resolve. */
    if (!seen_any && parser_peek(p)->kind == TK_IDENT) {
        Token *t1 = parser_peek_ahead(p, 1);
        bool decl_shape =
            t1->kind == TK_IDENT ||
            t1->kind == TK_STAR || t1->kind == TK_AMP || t1->kind == TK_LAND ||
            t1->kind == TK_GT || t1->kind == TK_SHR ||
            t1->kind == TK_COMMA || t1->kind == TK_RPAREN ||
            t1->kind == TK_LT ||  /* unknown<...> as opaque template-id type */
            t1->kind == TK_LBRACKET ||  /* 'new T[N]' array */
            t1->kind == TK_LBRACE ||  /* '-> T { body }' trailing return */
            t1->kind == TK_SEMI;  /* 'T;' bare statement / decl */
        if (decl_shape) {
            Token *name = parser_advance(p);
            /* If followed by '<', consume the template-arg-list and any
             * trailing '::ident' chain so the caller doesn't see them. */
            if (parser_at(p, TK_LT))
                parse_template_id(p, name);
            while (parser_consume(p, TK_SCOPE)) {
                parser_consume(p, TK_KW_TEMPLATE);
                if (parser_at(p, TK_IDENT)) {
                    Token *seg = parser_advance(p);
                    if (parser_at(p, TK_LT))
                        parse_template_id(p, seg);
                } else {
                    break;
                }
            }
            Type *ty = new_type(p, TY_INT);
            ty->is_const = is_const;
            ty->is_volatile = is_volatile;
            ty->tag = name;
            result.type = ty;
            return result;
        }
    }

    if (!seen_any) {
        /* Conversion function (operator T()), constructor (ClassName()),
         * or destructor (~ClassName()) — none of these have a return type
         * in the decl-specifier-seq position. Return a void placeholder so
         * the declarator parser handles the operator/ctor/dtor name. */
        TokenKind k = parser_peek(p)->kind;
        if (k == TK_KW_OPERATOR || k == TK_TILDE) {
            Type *ty = new_type(p, TY_VOID);
            result.type = ty;
            return result;
        }
        if (p->tentative) {
            p->tentative_failed = true;
            return result;  /* type is NULL — failure */
        }
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
    result.type = ty; return result;
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
    case TK_KW_AUTO: case TK_KW_DECLTYPE: case TK_KW_TYPENAME:
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
        /* A template-name followed by '<' forms a simple-template-id
         * (§17.2/4) — that's a type-specifier. A bare template-name
         * alone is only a type-specifier if it also names a type (e.g.
         * the injected-class-name inside its own class template body).
         * Otherwise — function templates like swap, alias templates —
         * it must NOT be parsed as a type, or 'swap(args)' in expression
         * context would be mis-parsed as a declaration. */
        if (lookup_is_type_name(p, parser_peek(p)))
            return true;
        if (lookup_is_template_name(p, parser_peek(p)))
            return parser_peek_ahead(p, 1)->kind == TK_LT;
        return false;
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
 * Handles pointer (*), lvalue reference (&), and rvalue reference (&&).
 */
Type *parse_type_name(Parser *p) {
    Type *base = parse_type_specifiers(p).type;
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

    /* Grouped abstract declarator — N4659 §11.3 [dcl.meaning]
     *   ( ptr-abstract-declarator )
     * Handles things like 'T (*)[]' (pointer to array of T) and
     * 'T (*)(int)' (pointer to function). We accept '(' followed by
     * '*' or '&' / cv-qualifiers / ')', then treat the result as a
     * pointer to the rest. The parameter list / array suffix that
     * follows is then consumed as part of the surrounding type. */
    /* Only enter the grouped form if it's actually shaped like '(*)' or
     * '(&)' (optionally followed by cv-quals) — i.e. the next token after
     * the '*'/'&' chain is ')'. Otherwise the '(' is some caller's init
     * paren (e.g. 'new T(*expr, ...)') and we must leave it alone. */
    if (parser_at(p, TK_LPAREN) &&
        (parser_peek_ahead(p, 1)->kind == TK_STAR ||
         parser_peek_ahead(p, 1)->kind == TK_AMP)) {
        int n = 1;
        while (parser_peek_ahead(p, n)->kind == TK_STAR ||
               parser_peek_ahead(p, n)->kind == TK_AMP ||
               parser_peek_ahead(p, n)->kind == TK_KW_CONST ||
               parser_peek_ahead(p, n)->kind == TK_KW_VOLATILE)
            n++;
        if (parser_peek_ahead(p, n)->kind != TK_RPAREN)
            goto no_grouped_abstract;
        parser_advance(p);  /* ( */
        while (parser_consume(p, TK_STAR) || parser_consume(p, TK_AMP) ||
               parser_consume(p, TK_KW_CONST) || parser_consume(p, TK_KW_VOLATILE))
            ;
        parser_expect(p, TK_RPAREN);
        base = new_ptr_type(p, base);
        /* Optional function-parameter list of the pointed-to function. */
        if (parser_consume(p, TK_LPAREN)) {
            int depth = 1;
            while (depth > 0 && !parser_at_eof(p)) {
                if (parser_at(p, TK_LPAREN)) depth++;
                else if (parser_at(p, TK_RPAREN)) {
                    depth--;
                    if (depth == 0) break;
                }
                parser_advance(p);
            }
            parser_expect(p, TK_RPAREN);
        }
    }
no_grouped_abstract:;

    /* Pointer-to-member — N4659 §11.3.3 [dcl.mptr]
     *   ptr-operator: nested-name-specifier * cv-qualifier-seq(opt)
     * E.g. 'T C::*' is pointer-to-member of C of type T. We accept any
     * 'ident :: ident :: ... :: *' chain after the base type and produce
     * an opaque pointer type. */
    if (parser_at(p, TK_IDENT) && parser_peek_ahead(p, 1)->kind == TK_SCOPE) {
        /* Look ahead for '*' at the end of the nested-name-specifier. */
        int n = 0;
        while (parser_peek_ahead(p, n)->kind == TK_IDENT &&
               parser_peek_ahead(p, n + 1)->kind == TK_SCOPE)
            n += 2;
        if (parser_peek_ahead(p, n)->kind == TK_STAR) {
            for (int i = 0; i <= n; i++) parser_advance(p);
            base = new_ptr_type(p, base);
            while (parser_at(p, TK_KW_CONST) || parser_at(p, TK_KW_VOLATILE)) {
                if (parser_consume(p, TK_KW_CONST))    base->is_const = true;
                if (parser_consume(p, TK_KW_VOLATILE)) base->is_volatile = true;
            }
        }
    }

    /* Abstract array suffix — N4659 §11.3.4 [dcl.array]
     *   abstract-declarator [ constant-expression(opt) ] */
    while (parser_consume(p, TK_LBRACKET)) {
        int len = -1;
        if (!parser_at(p, TK_RBRACKET)) {
            Node *size = parse_assign_expr(p);
            if (size && size->kind == ND_NUM)
                len = (int)size->num.lo;
        }
        parser_expect(p, TK_RBRACKET);
        base = new_array_type(p, base, len);
    }

    return base;
}
