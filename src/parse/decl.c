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
 *       using-declaration               (deferred)
 *       using-directive                 (deferred)
 *       static_assert-declaration       (deferred)
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
 * This first pass handles simple-declaration and function-definition
 * with built-in type specifiers only. User-defined type names,
 * templates, namespaces, and using-declarations are deferred.
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
Node *parse_declarator(Parser *p, Type *base_ty) {
    /* ptr-operator: consume leading *'s */
    while (consume(p, TK_STAR)) {
        base_ty = new_ptr_type(p, base_ty);
        /* cv-qualifiers after * — N4659 §11.3.1/1 [dcl.ptr] */
        while (at(p, TK_KW_CONST) || at(p, TK_KW_VOLATILE)) {
            if (consume(p, TK_KW_CONST))    base_ty->is_const = true;
            if (consume(p, TK_KW_VOLATILE)) base_ty->is_volatile = true;
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
    if (at(p, TK_LPAREN) && !at_type_specifier(p)) {
        Token *next = peek_ahead(p, 1);
        if (next && (next->kind == TK_STAR || next->kind == TK_LPAREN ||
                     (next->kind == TK_IDENT && !lookup_is_type_name(p, next)))) {
            /* Grouping parens or redundant parens around a name.
             * E.g.: int (*fp)(int,int)  or  T(x)  */
            advance(p);  /* skip ( */
            Node *inner = parse_declarator(p, base_ty);
            expect(p, TK_RPAREN);

            name = inner->var_decl.name;
            ty = inner->var_decl.ty;
            goto parse_suffixes;
        }
    }

    /* declarator-id — N4659 §11.3 [dcl.meaning]
     *   declarator-id: ...(opt) id-expression
     * For the first pass, just an identifier (or absent for abstract declarators). */
    if (at(p, TK_IDENT))
        name = advance(p);

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
    if (at(p, TK_LPAREN) && name) {
        /* Peek inside the parens to decide: parameter list or init?
         * A parameter list starts with: ), void), type-specifier, or ...
         * Anything else (literal, non-type identifier, etc.) is init. */
        Token *after_paren = peek_ahead(p, 1);
        bool looks_like_params =
            after_paren->kind == TK_RPAREN ||
            after_paren->kind == TK_ELLIPSIS ||
            after_paren->kind == TK_KW_VOID ||
            /* Check if it's a type-specifier keyword or known type-name */
            (after_paren->kind >= TK_KW_ALIGNAS /* any keyword */ ||
             (after_paren->kind == TK_IDENT &&
              lookup_is_type_name(p, after_paren)));

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
            looks_like_params = lookup_is_type_name(p, after_paren);
        } else if (after_paren->kind != TK_RPAREN &&
                   after_paren->kind != TK_ELLIPSIS &&
                   after_paren->kind != TK_KW_VOID) {
            looks_like_params = false;
        }

        if (looks_like_params) {
            advance(p);  /* consume ( */
            Vec params = vec_new(p->arena);
            Vec param_types = vec_new(p->arena);
            bool variadic = false;

            if (!at(p, TK_RPAREN)) {
                if (at(p, TK_KW_VOID) &&
                    peek_ahead(p, 1)->kind == TK_RPAREN) {
                    advance(p);  /* (void) — no params */
                } else {
                    for (;;) {
                        if (consume(p, TK_ELLIPSIS)) {
                            variadic = true;
                            break;
                        }

                        Type *param_base = parse_type_specifiers(p);
                        Node *param_decl = parse_declarator(p, param_base);
                        param_decl->kind = ND_PARAM;

                        vec_push(&params, param_decl);
                        vec_push(&param_types, param_decl->var_decl.ty);

                        if (!consume(p, TK_COMMA))
                            break;

                        if (at(p, TK_ELLIPSIS)) {
                            advance(p);
                            variadic = true;
                            break;
                        }
                    }
                }
            }
            expect(p, TK_RPAREN);

            ty = new_func_type(p, ty, (Type **)param_types.data,
                               param_types.len, variadic);

            Node *node = new_node(p, ND_VAR_DECL, name ? name : peek(p));
            node->var_decl.ty = ty;
            node->var_decl.name = name;
            node->func.params = (Node **)params.data;
            node->func.nparams = params.len;
            return node;
        }
        /* else: not a parameter list — fall through, leave ( for caller */
    }
    /* Same logic for unnamed declarators (abstract) — always params */
    if (!name && consume(p, TK_LPAREN)) {
        Vec params = vec_new(p->arena);
        Vec param_types = vec_new(p->arena);
        bool variadic = false;

        if (!at(p, TK_RPAREN)) {
            if (at(p, TK_KW_VOID) &&
                peek_ahead(p, 1)->kind == TK_RPAREN) {
                advance(p);
            } else {
                for (;;) {
                    if (consume(p, TK_ELLIPSIS)) { variadic = true; break; }
                    Type *param_base = parse_type_specifiers(p);
                    Node *param_decl = parse_declarator(p, param_base);
                    param_decl->kind = ND_PARAM;
                    vec_push(&params, param_decl);
                    vec_push(&param_types, param_decl->var_decl.ty);
                    if (!consume(p, TK_COMMA)) break;
                    if (at(p, TK_ELLIPSIS)) { advance(p); variadic = true; break; }
                }
            }
        }
        expect(p, TK_RPAREN);

        ty = new_func_type(p, ty, (Type **)param_types.data,
                           param_types.len, variadic);

        Node *node = new_node(p, ND_VAR_DECL, name ? name : peek(p));
        node->var_decl.ty = ty;
        node->var_decl.name = name;
        node->func.params = (Node **)params.data;
        node->func.nparams = params.len;
        return node;
    }

    /* Array declarator suffix — N4659 §11.3.4 [dcl.array]
     *   noptr-declarator [ constant-expression(opt) ] attribute-specifier-seq(opt) */
    while (consume(p, TK_LBRACKET)) {
        int len = -1;  /* unsized */
        if (!at(p, TK_RBRACKET)) {
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
        expect(p, TK_RBRACKET);
        ty = new_array_type(p, ty, len);
    }

    Node *node = new_node(p, ND_VAR_DECL, name ? name : peek(p));
    node->var_decl.ty = ty;
    node->var_decl.name = name;
    return node;
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
    Token *start_tok = peek(p);

    /* typedef — N4659 §10.1.3 [dcl.typedef]
     *   typedef-name: identifier
     *
     * N4659 §6.3.2/1 [basic.scope.pdecl]: the point of declaration
     * is after the complete declarator, so the typedef name becomes
     * visible for subsequent declarations in the same scope.
     *
     * C++11 also allows: using identifier = type-id (alias declaration)
     * — deferred. */
    if (consume(p, TK_KW_TYPEDEF)) {
        Type *base_ty = parse_type_specifiers(p);
        Node *decl = parse_declarator(p, base_ty);
        expect(p, TK_SEMI);

        /* Register the typedef-name into the current declarative region */
        if (decl->var_decl.name)
            region_declare(p, decl->var_decl.name->loc,
                          decl->var_decl.name->len, ENTITY_TYPE,
                          decl->var_decl.ty);

        Node *node = new_node(p, ND_TYPEDEF, start_tok);
        node->var_decl.ty = decl->var_decl.ty;
        node->var_decl.name = decl->var_decl.name;
        return node;
    }

    /* decl-specifier-seq — §10.1 [dcl.spec] */
    Type *base_ty = parse_type_specifiers(p);
    if (!base_ty)
        error_tok(start_tok, "expected declaration");

    /* Bare type with no declarator: 'struct Foo { ... };' */
    if (at(p, TK_SEMI)) {
        advance(p);
        Node *node = new_node(p, ND_VAR_DECL, start_tok);
        node->var_decl.ty = base_ty;
        return node;
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
        at(p, TK_LBRACE)) {

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
        region_push(p, REGION_PROTOTYPE);
        for (int i = 0; i < func->func.nparams; i++) {
            Node *param = func->func.params[i];
            if (param->param.name)
                region_declare(p, param->param.name->loc,
                              param->param.name->len, ENTITY_VARIABLE,
                              param->param.ty);
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
    if (consume(p, TK_ASSIGN)) {
        decl->var_decl.init = parse_assign_expr(p);
    } else if (consume(p, TK_LPAREN)) {
        /* Direct-initialization: T x(expr, ...) — N4659 §11.6/16 */
        decl->var_decl.init = parse_expr(p);
        expect(p, TK_RPAREN);
    }

    /* N4659 §6.3.2/1 [basic.scope.pdecl]: register the variable name
     * at its point of declaration (after declarator, before initializer
     * — but we've already parsed the initializer, which is fine for a
     * bootstrap tool processing valid source). */
    if (decl->var_decl.name)
        region_declare(p, decl->var_decl.name->loc,
                      decl->var_decl.name->len, ENTITY_VARIABLE,
                      decl->var_decl.ty);

    /* TODO: handle comma-separated declarators:
     *   int x = 1, y = 2, *z;
     * For the first pass, single declarator only. */

    expect(p, TK_SEMI);
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
    if (consume(p, TK_SEMI))
        return NULL;

    /* Skip preprocessor leftovers — #pragma, #line, etc.
     * These should have been consumed by the preprocessor, but mcpp
     * in independent mode passes through unknown pragmas. We skip
     * everything from # to the end of its line. */
    if (at(p, TK_HASH)) {
        int line = peek(p)->line;
        while (!at_eof(p) && peek(p)->line == line)
            advance(p);
        return NULL;
    }

    /* template-declaration — N4659 §17.1 [temp] (Annex A.12)
     *   template < template-parameter-list > declaration */
    if (at(p, TK_KW_TEMPLATE))
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
    if (at(p, TK_KW_EXTERN) &&
        peek_ahead(p, 1)->kind == TK_STR) {
        advance(p);  /* consume 'extern' */
        advance(p);  /* consume string literal ("C" or "C++") */

        if (at(p, TK_LBRACE)) {
            /* extern "C" { ... } — parse as a block of declarations.
             * We reuse ND_BLOCK to hold the inner declarations.
             * The linkage specifier is ignored for now. */
            Token *brace = peek(p);
            advance(p);
            Vec decls = vec_new(p->arena);
            while (!at(p, TK_RBRACE) && !at_eof(p)) {
                Node *decl = parse_top_level_decl(p);
                if (decl)
                    vec_push(&decls, decl);
            }
            expect(p, TK_RBRACE);
            Node *block = new_node(p, ND_BLOCK, brace);
            block->block.stmts = (Node **)decls.data;
            block->block.nstmts = decls.len;
            return block;
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
    Token *tok = peek(p);

    /* type-parameter: typename T or class T
     * N4659 §17.1/1: type-parameter-key is 'class' or 'typename' */
    if (tok->kind == TK_KW_TYPENAME || tok->kind == TK_KW_CLASS) {
        /* Disambiguate: 'typename T' (type param) vs 'typename Foo::bar' (dependent name)
         * and 'class X' (type param) vs elaborated-type-specifier.
         * Heuristic: if next token is ident, comma, >, >>, =, or ..., it's a type param. */
        Token *next = peek_ahead(p, 1);
        if (next->kind == TK_IDENT || next->kind == TK_COMMA ||
            next->kind == TK_GT || next->kind == TK_SHR ||
            next->kind == TK_ASSIGN || next->kind == TK_ELLIPSIS ||
            next->kind == TK_EOF) {

            advance(p);  /* consume class/typename */

            /* Optional pack expansion ... */
            bool is_pack = consume(p, TK_ELLIPSIS);
            (void)is_pack;  /* used in later stages */

            /* Optional identifier */
            Token *name = NULL;
            if (at(p, TK_IDENT))
                name = advance(p);

            /* Register the type parameter name in the template scope.
             * N4659 §6.3.9 [basic.scope.temp]: template parameter names
             * are in the template's declarative region. */
            if (name)
                region_declare(p, name->loc, name->len, ENTITY_TYPE, NULL);

            /* Optional default: = type-id */
            if (consume(p, TK_ASSIGN)) {
                /* Skip the default type-id — consume until , or > or >> */
                int depth = 0;
                while (!at_eof(p)) {
                    if (depth == 0 && (at(p, TK_COMMA) || at(p, TK_GT) || at(p, TK_SHR)))
                        break;
                    if (at(p, TK_LT)) depth++;
                    if (at(p, TK_GT)) depth--;
                    advance(p);
                }
            }

            Node *node = new_node(p, ND_PARAM, tok);
            node->param.name = name;
            node->param.ty = NULL;  /* type params have no type — they ARE types */
            return node;
        }
        /* else: fall through to non-type parameter parsing */
    }

    /* template-template parameter:
     * template < template-parameter-list > class/typename identifier(opt)
     * Deferred: just skip to , or > for now */
    if (tok->kind == TK_KW_TEMPLATE) {
        /* Skip nested template<...> */
        advance(p);  /* template */
        expect(p, TK_LT);
        int depth = 1;
        while (depth > 0 && !at_eof(p)) {
            if (at(p, TK_LT)) depth++;
            if (at(p, TK_GT)) { depth--; if (depth == 0) break; }
            if (at(p, TK_SHR) && depth <= 1) { depth--; break; }
            advance(p);
        }
        if (at(p, TK_GT)) advance(p);

        /* Consume class/typename */
        if (at(p, TK_KW_CLASS) || at(p, TK_KW_TYPENAME))
            advance(p);

        /* Optional identifier */
        Token *name = NULL;
        if (at(p, TK_IDENT)) {
            name = advance(p);
            region_declare(p, name->loc, name->len, ENTITY_TEMPLATE, NULL);
        }

        /* Optional default */
        if (consume(p, TK_ASSIGN)) {
            int d = 0;
            while (!at_eof(p)) {
                if (d == 0 && (at(p, TK_COMMA) || at(p, TK_GT) || at(p, TK_SHR)))
                    break;
                if (at(p, TK_LT)) d++;
                if (at(p, TK_GT)) d--;
                advance(p);
            }
        }

        Node *node = new_node(p, ND_PARAM, tok);
        node->param.name = name;
        return node;
    }

    /* Non-type template parameter: parsed as a parameter-declaration
     * e.g., 'int N', 'bool B = true', 'auto V' */
    Type *ty = parse_type_specifiers(p);
    Node *param = parse_declarator(p, ty);
    param->kind = ND_PARAM;

    /* Register non-type parameter name */
    if (param->var_decl.name)
        region_declare(p, param->var_decl.name->loc,
                      param->var_decl.name->len, ENTITY_VARIABLE, ty);

    /* Optional default value: = constant-expression */
    if (consume(p, TK_ASSIGN)) {
        /* Skip default — consume until , or > or >> */
        int depth = 0;
        while (!at_eof(p)) {
            if (depth == 0 && (at(p, TK_COMMA) || at(p, TK_GT) || at(p, TK_SHR)))
                break;
            if (at(p, TK_LT)) depth++;
            if (at(p, TK_GT)) depth--;
            advance(p);
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
    Token *tok = expect(p, TK_KW_TEMPLATE);

    /* explicit-specialization: template <> declaration */
    if (at(p, TK_LT) && peek_ahead(p, 1)->kind == TK_GT) {
        advance(p);  /* < */
        advance(p);  /* > */
        /* Parse the specialized declaration */
        Node *decl = parse_declaration(p);
        Node *node = new_node(p, ND_TEMPLATE_DECL, tok);
        node->template_decl.params = NULL;
        node->template_decl.nparams = 0;
        node->template_decl.decl = decl;
        return node;
    }

    expect(p, TK_LT);

    /* Push template parameter scope — §6.3.9 [basic.scope.temp] */
    region_push(p, REGION_TEMPLATE);

    /* Parse template-parameter-list */
    Vec params = vec_new(p->arena);
    if (!at(p, TK_GT) && !at(p, TK_SHR)) {
        vec_push(&params, parse_template_parameter(p));
        while (consume(p, TK_COMMA))
            vec_push(&params, parse_template_parameter(p));
    }

    expect(p, TK_GT);

    /* Parse the templated declaration.
     * This can be a class, function, variable, alias, or nested template. */
    Node *decl;
    if (at(p, TK_KW_TEMPLATE))
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
        default:
            break;
        }
    }
    if (tmpl_name)
        region_declare(p, tmpl_name->loc, tmpl_name->len,
                      ENTITY_TEMPLATE, NULL);

    Node *node = new_node(p, ND_TEMPLATE_DECL, tok);
    node->template_decl.params = (Node **)params.data;
    node->template_decl.nparams = params.len;
    node->template_decl.decl = decl;
    return node;
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
    expect(p, TK_LT);
    p->template_depth++;

    Vec args = vec_new(p->arena);

    if (!at(p, TK_GT) && !(at(p, TK_SHR) && p->template_depth > 0)) {
        for (;;) {
            /* template-argument: type-id or constant-expression or id-expression.
             * N4659 §17.3/2 [temp.arg] Rule 5: "type-id always wins."
             * For the first pass, try type first if at a type specifier. */
            if (at_type_specifier(p)) {
                /* Tentative: try type-id */
                ParseState saved = parser_save(p);
                p->tentative = true;
                Type *ty = parse_type_name(p);
                bool ty_ok = (ty != NULL);
                p->tentative = false;

                if (ty_ok && (at(p, TK_COMMA) || at(p, TK_GT) ||
                              (at(p, TK_SHR) && p->template_depth > 0))) {
                    /* Successfully parsed as type-id, and next is , or > */
                    /* Create a node to represent the type argument */
                    parser_restore(p, saved);
                    ty = parse_type_name(p);
                    Node *arg = new_node(p, ND_VAR_DECL, peek(p));
                    arg->var_decl.ty = ty;
                    arg->var_decl.name = NULL;
                    vec_push(&args, arg);
                } else {
                    /* Not a clean type-id — parse as expression */
                    parser_restore(p, saved);
                    vec_push(&args, parse_assign_expr(p));
                }
            } else {
                /* Not a type specifier — parse as expression */
                vec_push(&args, parse_assign_expr(p));
            }

            if (!consume(p, TK_COMMA))
                break;
        }
    }

    /* Closing > — handle >> splitting.
     * N4659 §17.2/3 [temp.names]: "When parsing a template-argument-list,
     * the first non-nested > is taken as the ending delimiter rather than
     * a greater-than operator."
     *
     * For >>: advance past the >> token and set split_shr so the next
     * peek()/at()/advance() returns a virtual TK_GT for the outer
     * template-argument-list. No token mutation — safe for tentative parsing. */
    if (at(p, TK_SHR) && p->template_depth > 0) {
        advance(p);         /* consume the >> token */
        p->split_shr = true; /* leave a virtual > for the outer consumer */
    } else {
        expect(p, TK_GT);
    }

    p->template_depth--;

    Node *node = new_node(p, ND_TEMPLATE_ID, tok);
    node->template_id.name = name;
    node->template_id.args = (Node **)args.data;
    node->template_id.nargs = args.len;
    return node;
}
