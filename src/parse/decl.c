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
     * This handles function pointers: int (*fp)(int, int)
     *
     * The trick: when we see '(', we need to distinguish between
     * a grouping paren and a function parameter list. If the token
     * after '(' could start a declarator (* or identifier), it's grouping.
     * Otherwise it's a parameter list.
     *
     * N4659 §11.2 [dcl.ambig.res] — Rule 2: the disambiguation rule
     * says "if it could be a declaration, it IS a declaration."
     * For the first pass we use a simpler heuristic. */
    if (at(p, TK_LPAREN) && !at_type_specifier(p)) {
        /* Check if this looks like a grouping paren:
         * '(' followed by '*' or '(' is grouping.
         * '(' followed by a type keyword is a parameter list. */
        Token *next = peek(p)->next;
        if (next && (next->kind == TK_STAR || next->kind == TK_LPAREN)) {
            /* Grouping parens — parse inner declarator */
            advance(p);  /* skip ( */
            Node *inner = parse_declarator(p, base_ty);
            expect(p, TK_RPAREN);

            /* Now parse any array/function suffixes, which wrap the inner type */
            name = inner->var_decl.name;
            ty = inner->var_decl.ty;
            /* Fall through to suffix parsing below */
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
     * C++23: explicit object parameter (this auto& self) */
    if (consume(p, TK_LPAREN)) {
        Vec params = vec_new(p->arena);
        Vec param_types = vec_new(p->arena);
        bool variadic = false;

        if (!at(p, TK_RPAREN)) {
            /* parameter-declaration-clause — §11.3.5/3
             *   parameter-declaration-list(opt) ...(opt)
             *   parameter-declaration-list , ...
             *
             * Special case: (void) means no parameters. */
            if (at(p, TK_KW_VOID) && peek(p)->next &&
                peek(p)->next->kind == TK_RPAREN) {
                advance(p);  /* consume 'void' — no params */
            } else {
                for (;;) {
                    if (consume(p, TK_ELLIPSIS)) {
                        variadic = true;
                        break;
                    }

                    /* parameter-declaration — §11.3.5/3
                     *   decl-specifier-seq declarator
                     *   decl-specifier-seq abstract-declarator(opt) */
                    Type *param_base = parse_type_specifiers(p);
                    Node *param_decl = parse_declarator(p, param_base);
                    param_decl->kind = ND_PARAM;

                    vec_push(&params, param_decl);
                    vec_push(&param_types, param_decl->var_decl.ty);

                    if (!consume(p, TK_COMMA))
                        break;

                    /* Check for trailing ... after comma */
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
        /* Stash params for func_def conversion later */
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
     * For the first pass, just skip over typedef declarations. */
    if (consume(p, TK_KW_TYPEDEF)) {
        /* Consume everything until ; */
        while (!at(p, TK_SEMI) && !at_eof(p))
            advance(p);
        expect(p, TK_SEMI);
        return new_node(p, ND_TYPEDEF, start_tok);
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

    /* Function definition: type + declarator(func-type) + '{' body '}' */
    if (decl->var_decl.ty && decl->var_decl.ty->kind == TY_FUNC &&
        at(p, TK_LBRACE)) {

        Node *func = new_node(p, ND_FUNC_DEF, decl->tok);
        func->func.ret_ty = decl->var_decl.ty->ret;
        func->func.name = decl->var_decl.name;
        func->func.params = decl->func.params;
        func->func.nparams = decl->func.nparams;
        func->func.body = parse_compound_stmt(p);
        return func;
    }

    /* Variable with initializer: = assignment-expression */
    if (consume(p, TK_ASSIGN))
        decl->var_decl.init = parse_assign_expr(p);

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
    if (at(p, TK_KW_EXTERN) && peek(p)->next &&
        peek(p)->next->kind == TK_STR) {
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
