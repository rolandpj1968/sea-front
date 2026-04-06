/*
 * decl.c — Declaration parser.
 *
 * N4659 §10 [dcl.dcl] — Declarations
 *
 *   declaration:
 *       block-declaration
 *       function-definition
 *       template-declaration            (deferred — Stage 3)
 *       explicit-instantiation          (deferred)
 *       explicit-specialization         (deferred)
 *       linkage-specification           (deferred — extern "C")
 *       namespace-definition            (deferred — Stage 2)
 *       empty-declaration               ( ; )
 *       attribute-declaration           (deferred)
 *
 *   block-declaration:
 *       simple-declaration
 *       asm-declaration                 (deferred)
 *       namespace-alias-definition      (deferred)
 *       using-declaration               (partial — using T = type)
 *       using-directive                 (implemented)
 *       static_assert-declaration       (skip-parsed)
 *       alias-declaration               (deferred — using T = ...)
 *       opaque-enum-declaration         (deferred)
 *
 *   simple-declaration:
 *       decl-specifier-seq init-declarator-list(opt) ;
 *       attribute-specifier-seq decl-specifier-seq init-declarator-list ;
 *       // C++17: attribute-specifier-seq(opt) decl-specifier-seq
 *       //        ref-qualifier(opt) [ identifier-list ] initializer ;
 *       //        (structured bindings — deferred)
 *
 * C++20 changes: adds concept-definition, module-declaration,
 *   export-declaration, constinit, consteval.
 * C++23 changes: adds deducing this (explicit object parameter).
 *
 * Handles: simple-declaration, function-definition, template-declaration,
 * namespace-definition, linkage-specification, using-declaration/directive,
 * friend-declaration, static_assert. User-defined type names are resolved
 * via name lookup (§6.4).
 */

#include "parse.h"

/*
 * parse_declarator — N4659 §11.3 [dcl.meaning]
 *
 *   declarator:
 *       ptr-declarator
 *       noptr-declarator parameters-and-qualifiers trailing-return-type
 *
 *   ptr-declarator:
 *       noptr-declarator
 *       ptr-operator ptr-declarator
 *
 *   ptr-operator:
 *       * cv-qualifier-seq(opt)                    (pointer)
 *       &                                          (deferred — lvalue ref)
 *       &&                                         (deferred — rvalue ref)
 *       nested-name-specifier * cv-qualifier-seq   (deferred — ptr-to-member)
 *
 *   noptr-declarator:
 *       declarator-id attribute-specifier-seq(opt)
 *       noptr-declarator parameters-and-qualifiers  (function)
 *       noptr-declarator [ constant-expression(opt) ] attribute-specifier-seq(opt) (array)
 *       ( ptr-declarator )                          (grouping parens)
 *
 * C++11: trailing return types (auto f() -> int)
 * C++20: abbreviated function templates (void f(auto x))
 * C++23: deducing this
 *
 * Returns an ND_VAR_DECL node (or ND_PARAM) with the fully-built type
 * and the declared name. For function declarators, the type is TY_FUNC.
 *
 * The declarator is one of the most complex parts of C/C++ syntax.
 * The "inside-out" reading rule: pointer operators wrap from the left,
 * while array/function suffixes wrap from the right, and parentheses
 * override the default grouping. We handle this recursively.
 */

/*
 * Consume trailing function qualifiers after the closing ')' of a
 * function parameter list.
 *
 * N4659 §11.3.5 [dcl.fct]: parameters-and-qualifiers includes
 *   cv-qualifier-seq(opt) ref-qualifier(opt) noexcept-specifier(opt)
 *
 * Also consumes virt-specifiers (override, final) per §12.3 [class.virtual].
 * Terminates: each iteration consumes a qualifier token or breaks.
 */
static void consume_trailing_qualifiers(Parser *p) {
    while (parser_consume(p, TK_KW_CONST) ||
           parser_consume(p, TK_KW_VOLATILE) ||
           parser_consume(p, TK_KW_NOEXCEPT))
        ;
    /* throw(type-id-list(opt)) — N4659 §15.4 [except.spec], deprecated in
     * C++17 but still pervasive in libstdc++ headers (e.g. throw()). */
    if (parser_consume(p, TK_KW_THROW)) {
        parser_expect(p, TK_LPAREN);
        int depth = 1;
        while (depth > 0 && !parser_at_eof(p)) {
            if (parser_at(p, TK_LPAREN)) depth++;
            else if (parser_at(p, TK_RPAREN)) { depth--; if (depth == 0) break; }
            parser_advance(p);
        }
        parser_expect(p, TK_RPAREN);
    }
    parser_skip_gnu_attributes(p);
    while (parser_at(p, TK_IDENT) &&
           (token_equal(parser_peek(p), "override") ||
            token_equal(parser_peek(p), "final")))
        parser_advance(p);
    parser_consume(p, TK_AMP);
    parser_consume(p, TK_LAND);
    if (parser_consume(p, TK_KW_NOEXCEPT)) {
        if (parser_consume(p, TK_LPAREN)) {
            parse_expr(p);
            parser_expect(p, TK_RPAREN);
        }
    }
    /* pure-specifier: = 0 (for virtual methods) */
    if (parser_at(p, TK_ASSIGN) && parser_peek_ahead(p, 1)->kind == TK_NUM) {
        parser_advance(p);
        parser_advance(p);
    }
}
Node *parse_declarator(Parser *p, Type *base_ty) {
    /* ptr-operator — N4659 §11.3 [dcl.meaning]
     *   ptr-operator:
     *       * cv-qualifier-seq(opt)                    — pointer
     *       & attribute-specifier-seq(opt)             — lvalue reference
     *       && attribute-specifier-seq(opt)            — rvalue reference
     *       nested-name-specifier * cv-qualifier-seq   — ptr-to-member (deferred)
     *
     * N4659 §11.3.2 [dcl.ref]:
     *   "In a declaration T D where D has either of the forms
     *    & attribute-specifier-seq(opt) D1
     *    && attribute-specifier-seq(opt) D1
     *    and the type of the identifier in the declaration T D1 is
     *    'derived-declarator-type-list T', then the type of the
     *    identifier of D is 'derived-declarator-type-list reference to T'."
     *
     * Note: && in declarator context is TK_LAND (logical AND token),
     * not two separate & tokens. This is unambiguous because we're
     * already committed to parsing a declarator.
     * Terminates: each iteration consumes a ptr-operator token (*, &, &&)
     * or breaks. Finite tokens, each consumed at most once for & and &&. */
    for (;;) {
        if (parser_consume(p, TK_STAR)) {
            base_ty = new_ptr_type(p, base_ty);
            /* cv-qualifiers after * — N4659 §11.3.1/1 [dcl.ptr] */
            while (parser_at(p, TK_KW_CONST) || parser_at(p, TK_KW_VOLATILE)) {
                if (parser_consume(p, TK_KW_CONST))    base_ty->is_const = true;
                if (parser_consume(p, TK_KW_VOLATILE)) base_ty->is_volatile = true;
            }
        } else if (parser_consume(p, TK_LAND)) {
            /* && — rvalue reference (C++11) */
            base_ty = new_rvalref_type(p, base_ty);
        } else if (parser_consume(p, TK_AMP)) {
            /* & — lvalue reference */
            base_ty = new_ref_type(p, base_ty);
        } else {
            break;
        }
    }

    Token *name = NULL;
    Type *ty = base_ty;

    /* Parenthesized declarator: ( ptr-declarator )
     *
     * N4659 §11.2 [dcl.ambig.res] — Rule 2:
     * "The disambiguation is purely syntactic; that is, the meaning of
     *  the names occurring in such a statement ... is not generally used
     *  in or changed by the disambiguation."
     *
     * When we see '(' at the start of a declarator, it could be:
     *   (a) Grouping parens: int (*fp)(int)  — '(' starts a nested declarator
     *   (b) A function parameter list after an unnamed declarator
     *   (c) Redundant parens around a name: T(x) means variable x of type T
     *
     * Heuristic: '(' followed by *, (, or a non-type identifier is grouping.
     * '(' followed by a type keyword is a parameter list. */
    if (parser_at(p, TK_LPAREN) && !parser_at_type_specifier(p)) {
        Token *next = parser_peek_ahead(p, 1);
        if (next && (next->kind == TK_STAR || next->kind == TK_LPAREN ||
                     (next->kind == TK_IDENT && !lookup_is_type_name(p, next)))) {
            /* Grouping parens or redundant parens around a name.
             * E.g.: int (*fp)(int,int)  or  T(x)  */
            parser_advance(p);  /* skip ( */
            Node *inner = parse_declarator(p, base_ty);
            parser_expect(p, TK_RPAREN);

            name = inner->var_decl.name;
            ty = inner->var_decl.ty;
            goto parse_suffixes;
        }
    }

    /* declarator-id — N4659 §11.3 [dcl.meaning]
     *   declarator-id: ...(opt) id-expression
     *   id-expression: unqualified-id | qualified-id
     *
     *   unqualified-id:
     *       identifier
     *       operator-function-id      (§16.5 [over.oper])
     *       conversion-function-id    (deferred)
     *       ~ class-name              (destructor)
     *
     * Handles bare identifiers, qualified names (Foo::bar),
     * operator overloads (operator[]), and destructors (~Foo). */
    if (parser_at(p, TK_IDENT)) {
        name = parser_advance(p);
        /* Template-id in declarator: vec<T, A, vl_embed>::operator[] */
        if (parser_at(p, TK_LT) && lookup_is_template_name(p, name))
            parse_template_id(p, name);
        /* Consume qualified-id: ident(opt <args>) :: ident :: ... :: ident
         * Terminates: each iteration consumes :: + ident/operator, or breaks. */
        while (parser_at(p, TK_SCOPE)) {
            Token *after = parser_peek_ahead(p, 1);
            if (after->kind == TK_IDENT) {
                parser_advance(p);  /* :: */
                name = parser_advance(p);
            } else if (after->kind == TK_TILDE) {
                /* Qualified destructor: Foo::~Foo */
                parser_advance(p);  /* :: */
                parser_advance(p);  /* ~ */
                if (parser_at(p, TK_IDENT))
                    name = parser_advance(p);
                break;
            } else if (after->kind == TK_KW_OPERATOR) {
                /* Qualified operator: Foo::operator[] */
                parser_advance(p);  /* :: */
                goto parse_operator_id;
            } else {
                break;
            }
        }
    }
    /* Destructor at current scope: ~ClassName */
    if (!name && parser_at(p, TK_TILDE) && parser_peek_ahead(p, 1)->kind == TK_IDENT) {
        parser_advance(p);  /* ~ */
        name = parser_advance(p);
    }
    /* operator-function-id — N4659 §16.5 [over.oper]
     *   operator-function-id: operator operator-symbol
     * Handles: operator+, operator[], operator(), operator<<, etc. */
    if (!name && parser_at(p, TK_KW_OPERATOR)) {
parse_operator_id:
        name = parser_advance(p);  /* consume 'operator' */
        /* Consume the operator symbol(s).
         * Special cases: operator() and operator[] are multi-token. */
        if (parser_consume(p, TK_LPAREN)) {
            parser_expect(p, TK_RPAREN);   /* operator() */
        } else if (parser_consume(p, TK_LBRACKET)) {
            parser_expect(p, TK_RBRACKET); /* operator[] */
        } else if (parser_at(p, TK_KW_NEW) || parser_at(p, TK_KW_DELETE)) {
            parser_advance(p);  /* operator new / operator delete */
            /* operator new[] / operator delete[] */
            if (parser_consume(p, TK_LBRACKET))
                parser_expect(p, TK_RBRACKET);
        } else if (parser_peek(p)->kind >= TK_LPAREN &&
                   parser_peek(p)->kind <= TK_HASHHASH) {
            /* Any single operator/punctuator token */
            parser_advance(p);
        }
    }

parse_suffixes:
    /* Function declarator suffix — N4659 §11.3.5 [dcl.fct]
     *   parameters-and-qualifiers:
     *       ( parameter-declaration-clause ) cv-qualifier-seq(opt)
     *           ref-qualifier(opt) noexcept-specifier(opt)
     *
     * C++11: trailing return type (-> type-id)
     * C++20: requires-clause after param list
     * C++23: explicit object parameter (this auto& self)
     *
     * N4659 §9.8 [stmt.ambig] / §11.2 [dcl.ambig.res]:
     * When we see '(' after a declarator name, it could be:
     *   (a) A function parameter list:  T f(int x);
     *   (b) Direct-initialization:      T x(42);
     *   (c) The "most vexing parse":    T x(T());  — function decl!
     *
     * Per §9.8: "any statement that could be a declaration IS a declaration."
     * We use tentative parsing: try to parse as a parameter list; if the
     * first token after '(' is not a type-specifier, ')', or '...', it's
     * not a parameter list — leave it for the caller to handle as init. */
    if (parser_at(p, TK_LPAREN) && name) {
        /* Peek inside the parens to decide: parameter list or init?
         * A parameter list starts with: ), void), type-specifier, or ...
         * Anything else (literal, non-type identifier, etc.) is init. */
        Token *after_paren = parser_peek_ahead(p, 1);
        bool looks_like_params =
            after_paren->kind == TK_RPAREN ||
            after_paren->kind == TK_ELLIPSIS ||
            after_paren->kind == TK_KW_VOID ||
            /* Check if it's a type-specifier keyword or known type/template name */
            (after_paren->kind >= TK_KW_ALIGNAS /* any keyword */ ||
             (after_paren->kind == TK_IDENT &&
              (lookup_is_type_name(p, after_paren) ||
               lookup_is_template_name(p, after_paren))));

        /* For the most vexing parse, we need the full heuristic.
         * Refine: a keyword that's a type-specifier signals params. */
        if (after_paren->kind >= TK_KW_ALIGNAS &&
            after_paren->kind <= TK_KW_WHILE) {
            /* It's a keyword — check if it could start a type */
            switch (after_paren->kind) {
            case TK_KW_VOID: case TK_KW_BOOL: case TK_KW_CHAR:
            case TK_KW_SHORT: case TK_KW_INT: case TK_KW_LONG:
            case TK_KW_FLOAT: case TK_KW_DOUBLE:
            case TK_KW_SIGNED: case TK_KW_UNSIGNED:
            case TK_KW_WCHAR_T: case TK_KW_CHAR16_T: case TK_KW_CHAR32_T:
            case TK_KW_CONST: case TK_KW_VOLATILE:
            case TK_KW_STRUCT: case TK_KW_UNION: case TK_KW_ENUM:
            case TK_KW_AUTO:
                looks_like_params = true;
                break;
            default:
                looks_like_params = false;
                break;
            }
        } else if (after_paren->kind == TK_IDENT) {
            looks_like_params = lookup_is_type_name(p, after_paren) ||
                                lookup_is_template_name(p, after_paren);
        } else if (after_paren->kind != TK_RPAREN &&
                   after_paren->kind != TK_ELLIPSIS &&
                   after_paren->kind != TK_KW_VOID) {
            looks_like_params = false;
        }

        if (looks_like_params) {
            parser_advance(p);  /* consume ( */
            Vec params = vec_new(p->arena);
            Vec param_types = vec_new(p->arena);
            bool variadic = false;

            if (!parser_at(p, TK_RPAREN)) {
                if (parser_at(p, TK_KW_VOID) &&
                    parser_peek_ahead(p, 1)->kind == TK_RPAREN) {
                    parser_advance(p);  /* (void) — no params */
                } else {
                    /* Terminates: each iteration parses a parameter
                     * (consuming tokens), then breaks on non-comma. */
                    for (;;) {
                        if (parser_consume(p, TK_ELLIPSIS)) {
                            variadic = true;
                            break;
                        }

                        Type *param_base = parse_type_specifiers(p).type;
                        Node *param_decl = parse_declarator(p, param_base);
                        param_decl->kind = ND_PARAM;

                        /* Default argument — N4659 §11.3.6 [dcl.fct.default]
                         *   parameter-declaration = assignment-expression */
                        if (parser_consume(p, TK_ASSIGN))
                            parse_assign_expr(p);  /* consume and discard */

                        vec_push(&params, param_decl);
                        vec_push(&param_types, param_decl->var_decl.ty);

                        if (!parser_consume(p, TK_COMMA))
                            break;

                        if (parser_at(p, TK_ELLIPSIS)) {
                            parser_advance(p);
                            variadic = true;
                            break;
                        }
                    }
                }
            }
            parser_expect(p, TK_RPAREN);

            consume_trailing_qualifiers(p);

            ty = new_func_type(p, ty, (Type **)param_types.data,
                               param_types.len, variadic);

            Node *node = new_var_decl_node(p, ty, name,
                                           name ? name : parser_peek(p));
            node->func.params = (Node **)params.data;
            node->func.nparams = params.len;
            return node;
        }
        /* else: not a parameter list — fall through, leave ( for caller */
    }
    /* Same logic for unnamed declarators (abstract) — always params */
    if (!name && parser_consume(p, TK_LPAREN)) {
        Vec params = vec_new(p->arena);
        Vec param_types = vec_new(p->arena);
        bool variadic = false;

        if (!parser_at(p, TK_RPAREN)) {
            if (parser_at(p, TK_KW_VOID) &&
                parser_peek_ahead(p, 1)->kind == TK_RPAREN) {
                parser_advance(p);
            } else {
                /* Terminates: same as named-param loop above. */
                for (;;) {
                    if (parser_consume(p, TK_ELLIPSIS)) { variadic = true; break; }
                    Type *param_base = parse_type_specifiers(p).type;
                    Node *param_decl = parse_declarator(p, param_base);
                    param_decl->kind = ND_PARAM;
                    if (parser_consume(p, TK_ASSIGN))
                        parse_assign_expr(p);  /* default arg — §11.3.6 */
                    vec_push(&params, param_decl);
                    vec_push(&param_types, param_decl->var_decl.ty);
                    if (!parser_consume(p, TK_COMMA)) break;
                    if (parser_at(p, TK_ELLIPSIS)) { parser_advance(p); variadic = true; break; }
                }
            }
        }
        parser_expect(p, TK_RPAREN);

        consume_trailing_qualifiers(p);

        ty = new_func_type(p, ty, (Type **)param_types.data,
                           param_types.len, variadic);

        Node *node = new_var_decl_node(p, ty, name,
                                       name ? name : parser_peek(p));
        node->func.params = (Node **)params.data;
        node->func.nparams = params.len;
        return node;
    }

    /* Array declarator suffix — N4659 §11.3.4 [dcl.array]
     *   noptr-declarator [ constant-expression(opt) ] attribute-specifier-seq(opt) */
    while (parser_consume(p, TK_LBRACKET)) {
        int len = -1;  /* unsized */
        if (!parser_at(p, TK_RBRACKET)) {
            /* For the first pass, only handle integer constant array sizes */
            /* N4659 §11.3.4 [dcl.array]: the expression must be a
             * converted constant expression of type std::size_t.
             * For the first pass, we accept any expression but only
             * extract the value from integer literals. Non-literal
             * sizes are stored as -1 (unsized) — sema evaluates later. */
            Node *size_expr = parse_assign_expr(p);
            if (size_expr && size_expr->kind == ND_NUM)
                len = (int)size_expr->num.lo;
        }
        parser_expect(p, TK_RBRACKET);
        ty = new_array_type(p, ty, len);
    }

    return new_var_decl_node(p, ty, name, name ? name : parser_peek(p));
}

/*
 * parse_declaration — N4659 §10 [dcl.dcl]
 *
 * Parses a simple-declaration or function-definition.
 *
 *   simple-declaration:
 *       decl-specifier-seq init-declarator-list(opt) ;
 *
 *   function-definition:
 *       decl-specifier-seq declarator function-body
 *
 *   init-declarator:
 *       declarator initializer(opt)
 *
 *   initializer:
 *       brace-or-equal-initializer
 *       ( expression-list )             (direct-init — deferred)
 *
 *   brace-or-equal-initializer:
 *       = initializer-clause
 *       braced-init-list               (deferred)
 */
Node *parse_declaration(Parser *p) {
    Token *start_tok = parser_peek(p);

    /* typedef — N4659 §10.1.3 [dcl.typedef]
     *   typedef-name: identifier
     *
     * N4659 §6.3.2/1 [basic.scope.pdecl]: the point of declaration
     * is after the complete declarator, so the typedef name becomes
     * visible for subsequent declarations in the same scope.
     *
     * C++11 also allows: using identifier = type-id (alias declaration)
     * — deferred. */
    if (parser_consume(p, TK_KW_TYPEDEF)) {
        Type *base_ty = parse_type_specifiers(p).type;
        Node *decl = parse_declarator(p, base_ty);
        parser_expect(p, TK_SEMI);

        /* Register the typedef-name into the current declarative region */
        if (decl->var_decl.name)
            region_declare(p, decl->var_decl.name->loc,
                          decl->var_decl.name->len, ENTITY_TYPE,
                          decl->var_decl.ty);

        return new_typedef_node(p, decl->var_decl.ty, decl->var_decl.name,
                                start_tok);
    }

    /* friend declaration — N4659 §14.3 [class.friend]
     *   friend declaration
     *   friend function-definition
     *
     * §14.3/1: "A friend of a class is a function or class that is
     * given permission to use the private and protected member names
     * from the class."
     *
     * §14.3/11: "For a friend class declaration, if there is no prior
     * declaration, the class that is specified belongs to the innermost
     * enclosing non-class scope, but if it is subsequently referenced,
     * its name is not found by name lookup until a matching declaration
     * is provided in the innermost enclosing non-class scope."
     *
     * Strictly, the friend declaration should NOT make the name visible
     * for lookup. However, for a bootstrap tool processing valid source
     * (where the name IS declared elsewhere), we eagerly register it
     * in the enclosing namespace so template-id parsing works. This is
     * a pragmatic deviation from the standard's lookup rules.
     *
     * The inner declaration is wrapped in ND_FRIEND for sema. */
    if (parser_consume(p, TK_KW_FRIEND)) {
        Node *inner;
        if (parser_at(p, TK_KW_TEMPLATE))
            inner = parse_template_declaration(p);
        else
            inner = parse_declaration(p);

        Node *node = new_node(p, ND_FRIEND, start_tok);
        node->friend_decl.decl = inner;
        return node;
    }

    /* decl-specifier-seq — §10.1 [dcl.spec] */
    DeclSpec spec = parse_type_specifiers(p);
    Type *base_ty = spec.type;
    Node *class_def = spec.class_def;
    if (!base_ty)
        error_tok(start_tok, "expected declaration");

    /* Bare type with no declarator: 'struct Foo { ... };' */
    if (parser_at(p, TK_SEMI)) {
        parser_advance(p);
        if (class_def)
            return class_def;
        return new_var_decl_node(p, base_ty, /*name=*/NULL, start_tok);
    }

    /* declarator — §11.3 [dcl.meaning] */
    Node *decl = parse_declarator(p, base_ty);

    /* Function definition: type + declarator(func-type) + '{' body '}'
     *
     * N4659 §6.3.4 [basic.scope.proto]: function parameters have
     * function prototype scope. We push a REGION_PROTOTYPE before
     * parsing the body so parameter names are visible inside.
     * The compound-statement pushes its own REGION_BLOCK as a child. */
    if (decl->var_decl.ty && decl->var_decl.ty->kind == TY_FUNC &&
        (parser_at(p, TK_LBRACE) || parser_at(p, TK_COLON))) {

        Node *func = new_node(p, ND_FUNC_DEF, decl->tok);
        func->func.ret_ty = decl->var_decl.ty->ret;
        func->func.name = decl->var_decl.name;
        func->func.params = decl->func.params;
        func->func.nparams = decl->func.nparams;

        /* Register the function name in the enclosing scope */
        if (func->func.name)
            region_declare(p, func->func.name->loc,
                          func->func.name->len, ENTITY_VARIABLE,
                          decl->var_decl.ty);

        /* Push prototype scope and register parameter names */
        region_push(p, REGION_PROTOTYPE, /*name=*/NULL);
        for (int i = 0; i < func->func.nparams; i++) {
            Node *param = func->func.params[i];
            if (param->param.name)
                region_declare(p, param->param.name->loc,
                              param->param.name->len, ENTITY_VARIABLE,
                              param->param.ty);
        }

        /* ctor-initializer — N4659 §15.6.2 [class.base.init]
         *   : mem-initializer-list
         *   mem-initializer: identifier ( expression-list(opt) )
         *                  | identifier braced-init-list
         *                  | identifier ... (pack expansion)
         * Currently parsed and discarded. */
        if (parser_consume(p, TK_COLON)) {
            /* Terminates: each iteration consumes one mem-initializer; loop
             * exits when no comma follows. */
            for (;;) {
                /* Skip the (possibly qualified or template) member name. */
                while (parser_at(p, TK_IDENT) || parser_at(p, TK_SCOPE) ||
                       parser_at(p, TK_LT) || parser_at(p, TK_GT) ||
                       parser_at(p, TK_SHR))
                    parser_advance(p);
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
                } else if (parser_consume(p, TK_LBRACE)) {
                    int depth = 1;
                    while (depth > 0 && !parser_at_eof(p)) {
                        if (parser_at(p, TK_LBRACE)) depth++;
                        else if (parser_at(p, TK_RBRACE)) {
                            depth--;
                            if (depth == 0) break;
                        }
                        parser_advance(p);
                    }
                    parser_expect(p, TK_RBRACE);
                }
                parser_consume(p, TK_ELLIPSIS);  /* pack expansion */
                if (!parser_consume(p, TK_COMMA)) break;
            }
        }

        func->func.body = parse_compound_stmt(p);
        region_pop(p);  /* pop prototype scope */
        return func;
    }

    /* Variable with initializer — N4659 §11.6 [dcl.init]
     *
     *   initializer:
     *       brace-or-equal-initializer
     *       ( expression-list )                (direct-initialization)
     *
     *   brace-or-equal-initializer:
     *       = initializer-clause
     *       braced-init-list                   (deferred)
     *
     * Direct-initialization T x(expr) is distinguished from a function
     * declarator by the heuristic in parse_declarator() — if the '(' was
     * not consumed as a parameter list, it arrives here as init. */
    /* Bit-field — N4659 §12.2.4 [class.bit]
     *   member-declarator: identifier(opt) : constant-expression
     * The colon after a declarator name in a class indicates bit-field width.
     * Consume the width expression and continue. */
    if (parser_consume(p, TK_COLON)) {
        parse_assign_expr(p);  /* bit-field width — consumed and discarded */
    }

    if (parser_consume(p, TK_ASSIGN)) {
        /* = initializer-clause — could be expression or braced-init-list */
        if (parser_at(p, TK_LBRACE)) {
            /* Skip braced-init-list { ... } — N4659 §11.6.4 [dcl.init.list]
             * Terminates: balanced brace counting toward } */
            int depth = 0;
            while (!parser_at_eof(p)) {
                if (parser_at(p, TK_LBRACE)) depth++;
                if (parser_at(p, TK_RBRACE)) { depth--; if (depth <= 0) { parser_advance(p); break; } }
                parser_advance(p);
            }
        } else {
            decl->var_decl.init = parse_assign_expr(p);
        }
    } else if (parser_consume(p, TK_LPAREN)) {
        /* Direct-initialization: T x(expr, ...) — N4659 §11.6/16 */
        decl->var_decl.init = parse_expr(p);
        parser_expect(p, TK_RPAREN);
    } else if (parser_at(p, TK_LBRACE)) {
        /* Braced-init-list without = : T x{ ... } — N4659 §11.6.4
         * Terminates: balanced brace counting toward } */
        int depth = 0;
        while (!parser_at_eof(p)) {
            if (parser_at(p, TK_LBRACE)) depth++;
            if (parser_at(p, TK_RBRACE)) { depth--; if (depth <= 0) { parser_advance(p); break; } }
            parser_advance(p);
        }
    }

    /* N4659 §6.3.2/1 [basic.scope.pdecl]: register the variable name */
    if (decl->var_decl.name)
        region_declare(p, decl->var_decl.name->loc,
                      decl->var_decl.name->len, ENTITY_VARIABLE,
                      decl->var_decl.ty);

    /* Comma-separated declarators — N4659 §10 [dcl.dcl]
     *   init-declarator-list:
     *       init-declarator
     *       init-declarator-list , init-declarator
     *
     * Each subsequent declarator shares the same base type (decl-specifier-seq)
     * but may add its own pointer/array/function modifiers.
     * E.g.: int x = 1, *y, z[10]; */
    if (parser_at(p, TK_COMMA)) {
        Vec decls = vec_new(p->arena);
        vec_push(&decls, decl);

        while (parser_consume(p, TK_COMMA)) {
            Node *next_decl = parse_declarator(p, base_ty);

            if (parser_consume(p, TK_ASSIGN))
                next_decl->var_decl.init = parse_assign_expr(p);
            else if (parser_consume(p, TK_LPAREN)) {
                next_decl->var_decl.init = parse_expr(p);
                parser_expect(p, TK_RPAREN);
            }

            if (next_decl->var_decl.name)
                region_declare(p, next_decl->var_decl.name->loc,
                              next_decl->var_decl.name->len, ENTITY_VARIABLE,
                              next_decl->var_decl.ty);

            vec_push(&decls, next_decl);
        }

        parser_expect(p, TK_SEMI);

        /* Wrap multiple declarators in a block node */
        return new_block_node(p, (Node **)decls.data, decls.len, start_tok);
    }

    parser_expect(p, TK_SEMI);
    return decl;
}

/*
 * parse_top_level_decl — top-level declaration
 *
 * At file scope, C++ allows:
 *   - function definitions
 *   - variable declarations
 *   - linkage-specification (extern "C")
 *   - namespace definitions (deferred)
 *   - template declarations (deferred)
 *   - using declarations (deferred)
 *   - empty declarations ( ; )
 */
Node *parse_top_level_decl(Parser *p) {
    /* Empty declaration — N4659 §10/6 */
    if (parser_consume(p, TK_SEMI))
        return NULL;

    /* Skip preprocessor leftovers — #pragma, #line, etc.
     * These should have been consumed by the preprocessor, but mcpp
     * in independent mode passes through unknown pragmas. We skip
     * everything from # to the end of its line. */
    if (parser_at(p, TK_HASH)) {
        int line = parser_peek(p)->line;
        while (!parser_at_eof(p) && parser_peek(p)->line == line)
            parser_advance(p);
        return NULL;
    }

    /* namespace-definition — N4659 §10.3.1 [namespace.def]
     *   namespace identifier(opt) { namespace-body }
     *   inline namespace identifier(opt) { namespace-body }
     *
     * C++17: nested namespace: namespace A::B::C { ... }
     * C++20: inline nested: namespace A::inline B { ... }
     *
     * Also handles:
     *   using-directive: using namespace name ;  (§10.3.4)
     *   using-declaration: using name ;          (§10.3.3)
     *   using alias: using T = type-id ;         (deferred)
     */
    if (parser_at(p, TK_KW_USING)) {
        Token *tok = parser_advance(p);

        /* using namespace foo; — §10.3.4 [namespace.udir]
         * Find the named namespace's region and add it to the current
         * region's using list so its declarations become visible. */
        if (parser_consume(p, TK_KW_NAMESPACE)) {
            Token *ns_tok = NULL;
            if (parser_at(p, TK_IDENT))
                ns_tok = parser_advance(p);
            /* Skip any qualified parts (A::B::C) for now */
            /* Terminates: advances past :: and ident pairs */
            while (parser_consume(p, TK_SCOPE)) {
                if (parser_at(p, TK_IDENT))
                    ns_tok = parser_advance(p);
            }
            /* Find and register the using-directive */
            if (ns_tok) {
                DeclarativeRegion *ns = region_find_namespace(
                    p, ns_tok->loc, ns_tok->len);
                if (ns)
                    region_add_using(p, ns);
            }
            parser_expect(p, TK_SEMI);
            return NULL;
        }

        /* using T = type-id; — C++11 alias declaration (§10.1.3 [dcl.typedef])
         *   alias-declaration: using identifier = type-id ;
         * Equivalent to: typedef type-id identifier; */
        if (parser_at(p, TK_IDENT) && parser_peek_ahead(p, 1)->kind == TK_ASSIGN) {
            Token *alias_name = parser_advance(p);
            parser_advance(p);  /* consume = */
            Type *ty = parse_type_name(p);
            parser_expect(p, TK_SEMI);

            if (alias_name && ty)
                region_declare(p, alias_name->loc, alias_name->len,
                              ENTITY_TYPE, ty);

            return new_typedef_node(p, ty, alias_name, tok);
        }

        /* using-declaration: using Base::member; — (§10.3.3 [namespace.udecl])
         * For now, skip until ; */
        /* Terminates: advances toward ; or EOF */
        while (!parser_at(p, TK_SEMI) && !parser_at_eof(p))
            parser_advance(p);
        parser_expect(p, TK_SEMI);
        (void)tok;
        return NULL;
    }

    /* C++11 'inline namespace X' — N4659 §10.3.1 [namespace.def]/8.
     * The 'inline' may precede the 'namespace' keyword. */
    if (parser_at(p, TK_KW_INLINE) &&
        parser_peek_ahead(p, 1)->kind == TK_KW_NAMESPACE)
        parser_advance(p);

    if (parser_at(p, TK_KW_NAMESPACE)) {
        Token *tok = parser_advance(p);

        /* Optional 'inline' namespace */
        parser_consume(p, TK_KW_INLINE);

        /* Optional namespace name (unnamed namespaces are valid) */
        Token *ns = NULL;
        if (parser_at(p, TK_IDENT))
            ns = parser_advance(p);

        /* C++17 nested namespace: namespace A::B { }
         * Terminates: each iteration consumes :: and ident */
        while (parser_consume(p, TK_SCOPE)) {
            parser_consume(p, TK_KW_INLINE);  /* C++20: inline nested */
            if (parser_at(p, TK_IDENT))
                parser_advance(p);
        }

        /* Register the namespace name in the current (enclosing) scope
         * BEFORE pushing the namespace's own scope. */
        Declaration *ns_decl = NULL;
        if (ns)
            ns_decl = region_declare(p, ns->loc, ns->len,
                                     ENTITY_NAMESPACE, /*type=*/NULL);

        /* GCC __attribute__((__abi_tag__(...))) and friends after the
         * namespace name — pervasive in libstdc++. */
        parser_skip_gnu_attributes(p);

        parser_expect(p, TK_LBRACE);

        /* N4659 §6.3.6 [basic.scope.namespace]: push named namespace scope */
        region_push(p, REGION_NAMESPACE, ns);

        Vec decls = vec_new(p->arena);
        /* Terminates: each iteration parses a declaration or hits } / EOF */
        while (!parser_at(p, TK_RBRACE) && !parser_at_eof(p)) {
            Node *decl = parse_top_level_decl(p);
            if (decl)
                vec_push(&decls, decl);
        }
        parser_expect(p, TK_RBRACE);

        /* Stash the namespace region on the declaration so
         * 'using namespace foo' can find it after the region is popped. */
        if (ns_decl)
            ns_decl->ns_region = p->region;

        region_pop(p);

        /* Return as a block of declarations */
        return new_block_node(p, (Node **)decls.data, decls.len, tok);
    }

    /* static_assert — N4659 §10.1.4 [dcl.dcl]
     *   static_assert ( constant-expression , string-literal ) ;
     *   C++17: static_assert ( constant-expression ) ;
     * Parse and skip for now. */
    if (parser_at(p, TK_KW_STATIC_ASSERT)) {
        parser_advance(p);
        parser_expect(p, TK_LPAREN);
        /* Terminates: advances through balanced parens toward ) */
        int depth = 1;
        while (depth > 0 && !parser_at_eof(p)) {
            if (parser_at(p, TK_LPAREN)) depth++;
            if (parser_at(p, TK_RPAREN)) depth--;
            if (depth > 0) parser_advance(p);
        }
        parser_expect(p, TK_RPAREN);
        parser_expect(p, TK_SEMI);
        return NULL;
    }

    /* template-declaration — N4659 §17.1 [temp] (Annex A.12)
     *   template < template-parameter-list > declaration */
    if (parser_at(p, TK_KW_TEMPLATE))
        return parse_template_declaration(p);

    /* linkage-specification — N4659 §10.5 [dcl.link]
     *   extern string-literal { declaration-seq(opt) }
     *   extern string-literal declaration
     *
     * The string-literal is "C" or "C++". This controls name mangling:
     * extern "C" suppresses C++ mangling.
     *
     * For now, we parse the syntax and ignore the linkage specifier.
     * The semantic layer will need to track it for name mangling and
     * overload resolution (C linkage prohibits overloading).
     *
     * C++20/23: unchanged. */
    if (parser_at(p, TK_KW_EXTERN) &&
        parser_peek_ahead(p, 1)->kind == TK_STR) {
        parser_advance(p);  /* consume 'extern' */
        parser_advance(p);  /* consume string literal ("C" or "C++") */

        if (parser_at(p, TK_LBRACE)) {
            /* extern "C" { ... } — parse as a block of declarations.
             * We reuse ND_BLOCK to hold the inner declarations.
             * The linkage specifier is ignored for now. */
            Token *brace = parser_peek(p);
            parser_advance(p);
            Vec decls = vec_new(p->arena);
            while (!parser_at(p, TK_RBRACE) && !parser_at_eof(p)) {
                Node *decl = parse_top_level_decl(p);
                if (decl)
                    vec_push(&decls, decl);
            }
            parser_expect(p, TK_RBRACE);
            return new_block_node(p, (Node **)decls.data, decls.len, brace);
        }

        /* extern "C" single-declaration */
        return parse_declaration(p);
    }

    return parse_declaration(p);
}

/* ------------------------------------------------------------------ */
/* Template declarations — N4659 §17 [temp]                            */
/*                          N4861 §13 [temp] (C++20)                   */
/*                          N4950 §13 [temp] (C++23)                   */
/* ------------------------------------------------------------------ */

/*
 * Parse a single template parameter.
 *
 * N4659 §17.1 [temp] (Annex A.12):
 *   template-parameter:
 *       type-parameter
 *       parameter-declaration
 *
 *   type-parameter:
 *       type-parameter-key ...(opt) identifier(opt)
 *       type-parameter-key identifier(opt) = type-id
 *       template < template-parameter-list > type-parameter-key ...(opt) identifier(opt)
 *       template < template-parameter-list > type-parameter-key identifier(opt) = id-expression
 *
 *   type-parameter-key: class | typename
 *
 * If the parameter starts with 'class' or 'typename' (and isn't followed
 * by something that makes it a non-type parameter like 'class Foo' where
 * Foo is already a type), it's a type parameter. Otherwise it's a non-type
 * parameter (parsed as a regular parameter-declaration).
 */
static Node *parse_template_parameter(Parser *p) {
    Token *tok = parser_peek(p);

    /* type-parameter: typename T or class T
     * N4659 §17.1/1: type-parameter-key is 'class' or 'typename' */
    if (tok->kind == TK_KW_TYPENAME || tok->kind == TK_KW_CLASS) {
        /* Disambiguate: 'typename T' (type param) vs 'typename Foo::bar' (dependent name)
         * and 'class X' (type param) vs elaborated-type-specifier.
         * Heuristic: if next token is ident, comma, >, >>, =, or ..., it's a type param. */
        Token *next = parser_peek_ahead(p, 1);
        if (next->kind == TK_IDENT || next->kind == TK_COMMA ||
            next->kind == TK_GT || next->kind == TK_SHR ||
            next->kind == TK_ASSIGN || next->kind == TK_ELLIPSIS ||
            next->kind == TK_EOF) {

            parser_advance(p);  /* consume class/typename */

            /* Optional pack expansion ... */
            bool is_pack = parser_consume(p, TK_ELLIPSIS);
            (void)is_pack;  /* used in later stages */

            /* Optional identifier */
            Token *name = NULL;
            if (parser_at(p, TK_IDENT))
                name = parser_advance(p);

            /* Register the type parameter name in the template scope.
             * N4659 §6.3.9 [basic.scope.temp]: template parameter names
             * are in the template's declarative region. */
            if (name)
                region_declare(p, name->loc, name->len, ENTITY_TYPE, /*type=*/NULL);

            /* Optional default: = type-id */
            if (parser_consume(p, TK_ASSIGN)) {
                /* Skip the default type-id — consume until , or > or >> */
                int depth = 0;
                while (!parser_at_eof(p)) {
                    if (depth == 0 && (parser_at(p, TK_COMMA) || parser_at(p, TK_GT) || parser_at(p, TK_SHR)))
                        break;
                    if (parser_at(p, TK_LT)) depth++;
                    if (parser_at(p, TK_GT)) depth--;
                    parser_advance(p);
                }
            }

            return new_param_node(p, /*ty=*/NULL, name, tok);
        }
        /* else: fall through to non-type parameter parsing */
    }

    /* template-template parameter:
     * template < template-parameter-list > class/typename identifier(opt)
     * Deferred: just skip to , or > for now */
    if (tok->kind == TK_KW_TEMPLATE) {
        /* Skip nested template<...> */
        parser_advance(p);  /* template */
        parser_expect(p, TK_LT);
        int depth = 1;
        while (depth > 0 && !parser_at_eof(p)) {
            if (parser_at(p, TK_LT)) depth++;
            if (parser_at(p, TK_GT)) { depth--; if (depth == 0) break; }
            if (parser_at(p, TK_SHR) && depth <= 1) { depth--; break; }
            parser_advance(p);
        }
        if (parser_at(p, TK_GT)) parser_advance(p);

        /* Consume class/typename */
        if (parser_at(p, TK_KW_CLASS) || parser_at(p, TK_KW_TYPENAME))
            parser_advance(p);

        /* Optional identifier */
        Token *name = NULL;
        if (parser_at(p, TK_IDENT)) {
            name = parser_advance(p);
            region_declare(p, name->loc, name->len, ENTITY_TEMPLATE, /*type=*/NULL);
        }

        /* Optional default */
        if (parser_consume(p, TK_ASSIGN)) {
            int d = 0;
            while (!parser_at_eof(p)) {
                if (d == 0 && (parser_at(p, TK_COMMA) || parser_at(p, TK_GT) || parser_at(p, TK_SHR)))
                    break;
                if (parser_at(p, TK_LT)) d++;
                if (parser_at(p, TK_GT)) d--;
                parser_advance(p);
            }
        }

        return new_param_node(p, /*ty=*/NULL, name, tok);
    }

    /* Non-type template parameter: parsed as a parameter-declaration
     * e.g., 'int N', 'bool B = true', 'auto V' */
    Type *ty = parse_type_specifiers(p).type;
    Node *param = parse_declarator(p, ty);
    param->kind = ND_PARAM;

    /* Register non-type parameter name */
    if (param->var_decl.name)
        region_declare(p, param->var_decl.name->loc,
                      param->var_decl.name->len, ENTITY_VARIABLE, ty);

    /* Optional default value: = constant-expression */
    if (parser_consume(p, TK_ASSIGN)) {
        /* Skip default — consume until , or > or >> */
        int depth = 0;
        while (!parser_at_eof(p)) {
            if (depth == 0 && (parser_at(p, TK_COMMA) || parser_at(p, TK_GT) || parser_at(p, TK_SHR)))
                break;
            if (parser_at(p, TK_LT)) depth++;
            if (parser_at(p, TK_GT)) depth--;
            parser_advance(p);
        }
    }

    return param;
}

/*
 * parse_template_declaration — N4659 §17.1 [temp]
 *
 *   template-declaration:
 *       template < template-parameter-list > declaration
 *
 * N4659 §6.3.9 [basic.scope.temp]:
 *   "The declarative region of the name of a template parameter
 *    is the smallest template-declaration in which the name
 *    was introduced."
 */
Node *parse_template_declaration(Parser *p) {
    Token *tok = parser_expect(p, TK_KW_TEMPLATE);

    /* explicit-specialization: template <> declaration */
    if (parser_at(p, TK_LT) && parser_peek_ahead(p, 1)->kind == TK_GT) {
        parser_advance(p);  /* < */
        parser_advance(p);  /* > */
        /* Parse the specialized declaration */
        Node *decl = parse_declaration(p);
        return new_template_decl_node(p, /*params=*/NULL, /*nparams=*/0,
                                      decl, tok);
    }

    parser_expect(p, TK_LT);

    /* Push template parameter scope — §6.3.9 [basic.scope.temp] */
    region_push(p, REGION_TEMPLATE, /*name=*/NULL);

    /* Parse template-parameter-list */
    Vec params = vec_new(p->arena);
    if (!parser_at(p, TK_GT) && !parser_at(p, TK_SHR)) {
        vec_push(&params, parse_template_parameter(p));
        while (parser_consume(p, TK_COMMA))
            vec_push(&params, parse_template_parameter(p));
    }

    parser_expect(p, TK_GT);

    /* Parse the templated declaration.
     * This can be a class, function, variable, alias, or nested template. */
    Node *decl;
    if (parser_at(p, TK_KW_TEMPLATE))
        decl = parse_template_declaration(p);  /* nested template */
    else
        decl = parse_declaration(p);

    /* Pop template parameter scope */
    region_pop(p);

    /* Register the template name in the enclosing scope.
     * The name comes from the inner declaration — may be in different
     * places depending on the declaration kind:
     *   - function: func.name
     *   - variable: var_decl.name
     *   - struct/class/enum with no declarator: var_decl.ty->tag */
    Token *tmpl_name = NULL;
    if (decl) {
        switch (decl->kind) {
        case ND_FUNC_DEF:
        case ND_FUNC_DECL:
            tmpl_name = decl->func.name;
            break;
        case ND_VAR_DECL:
            tmpl_name = decl->var_decl.name;
            /* Bare struct/class/enum: name is in the type's tag */
            if (!tmpl_name && decl->var_decl.ty && decl->var_decl.ty->tag)
                tmpl_name = decl->var_decl.ty->tag;
            break;
        case ND_CLASS_DEF:
            tmpl_name = decl->class_def.tag;
            break;
        case ND_FRIEND:
            /* friend template: extract name from the inner declaration */
            if (decl->friend_decl.decl) {
                Node *inner = decl->friend_decl.decl;
                if (inner->kind == ND_VAR_DECL && inner->var_decl.ty &&
                    inner->var_decl.ty->tag)
                    tmpl_name = inner->var_decl.ty->tag;
                else if (inner->kind == ND_CLASS_DEF)
                    tmpl_name = inner->class_def.tag;
                else if (inner->kind == ND_FUNC_DEF || inner->kind == ND_FUNC_DECL)
                    tmpl_name = inner->func.name;
            }
            break;
        default:
            break;
        }
    }
    if (tmpl_name) {
        region_declare(p, tmpl_name->loc, tmpl_name->len,
                      ENTITY_TEMPLATE, /*type=*/NULL);

        /* N4659 §14.3/11 [class.friend]: strictly, a friend-declared
         * name is NOT found by lookup until a matching declaration
         * appears at namespace scope. But for a bootstrap tool
         * processing valid source (where the declaration exists),
         * we eagerly register in the enclosing namespace so
         * template-id parsing works without forward-declaration order
         * sensitivity. Pragmatic deviation from the standard. */
        if (decl && decl->kind == ND_FRIEND) {
            DeclarativeRegion *ns = p->region;
            while (ns && ns->kind != REGION_NAMESPACE)
                ns = ns->enclosing;
            if (ns && ns != p->region)
                region_declare_in(p, ns, tmpl_name->loc, tmpl_name->len,
                                  ENTITY_TEMPLATE, /*type=*/NULL);
        }
    }

    return new_template_decl_node(p, (Node **)params.data, params.len,
                                  decl, tok);
}

/*
 * parse_template_id — N4659 §17.2 [temp.names]
 *
 *   simple-template-id:
 *       template-name < template-argument-list(opt) >
 *
 *   template-argument:
 *       constant-expression
 *       type-id
 *       id-expression
 *
 * Called from primary_expr/postfix_expr when an identifier is known
 * to be a template-name (via lookup_is_template_name).
 *
 * Handles >> splitting: when template_depth > 0 and we see TK_SHR,
 * we treat it as two '>' tokens. The first closes the inner template-
 * argument-list; the remaining '>' is left for the outer.
 *
 * N4659 §17.2/3: "When parsing a template-argument-list, the first
 *   non-nested > is taken as the ending delimiter rather than a
 *   greater-than operator."
 */
Node *parse_template_id(Parser *p, Token *name) {
    Token *tok = name;
    parser_expect(p, TK_LT);
    p->template_depth++;

    Vec args = vec_new(p->arena);

    if (!parser_at(p, TK_GT) && !(parser_at(p, TK_SHR) && p->template_depth > 0)) {
        /* Terminates: each iteration parses a template argument
         * (consuming tokens), then breaks on non-comma. */
        for (;;) {
            /* template-argument: type-id or constant-expression or id-expression.
             * N4659 §17.3/2 [temp.arg] Rule 5: "type-id always wins."
             * For the first pass, try type first if at a type specifier. */
            if (parser_at_type_specifier(p)) {
                /* Tentative: try type-id */
                ParseState saved = parser_save(p);
                p->tentative = true;
                Type *ty = parse_type_name(p);
                bool ty_ok = (ty != NULL);
                p->tentative = false;

                if (ty_ok && (parser_at(p, TK_COMMA) || parser_at(p, TK_GT) ||
                              (parser_at(p, TK_SHR) && p->template_depth > 0))) {
                    /* Successfully parsed as type-id, and next is , or > */
                    /* Create a node to represent the type argument */
                    parser_restore(p, saved);
                    ty = parse_type_name(p);
                    vec_push(&args, new_var_decl_node(p, ty, /*name=*/NULL,
                                                      parser_peek(p)));
                } else {
                    /* Not a clean type-id — parse as expression */
                    parser_restore(p, saved);
                    vec_push(&args, parse_assign_expr(p));
                }
            } else {
                /* Not a type specifier — parse as expression */
                vec_push(&args, parse_assign_expr(p));
            }

            if (!parser_consume(p, TK_COMMA))
                break;
        }
    }

    /* Closing > — handle >> splitting.
     * N4659 §17.2/3 [temp.names]: "When parsing a template-argument-list,
     * the first non-nested > is taken as the ending delimiter rather than
     * a greater-than operator."
     *
     * For >>: advance past the >> token and set split_shr so the next
     * parser_peek()/parser_at()/parser_advance() returns a virtual TK_GT for the outer
     * template-argument-list. No token mutation — safe for tentative parsing. */
    if (parser_at(p, TK_SHR) && p->template_depth > 0) {
        parser_advance(p);         /* consume the >> token */
        p->split_shr = true; /* leave a virtual > for the outer consumer */
    } else {
        parser_expect(p, TK_GT);
    }

    p->template_depth--;

    return new_template_id_node(p, name, (Node **)args.data, args.len, tok);
}
