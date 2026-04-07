/*
 * expr.c — Expression parser using precedence climbing.
 *
 * Implements the C++17 expression grammar (N4659 §8 [expr]).
 * Each precedence level maps to a section of the standard:
 *
 *   Level    Grammar                      Standard
 *   ─────── ──────────────────────────── ─────────────────
 *   comma    comma-expression             §8.19 [expr.comma]
 *   assign   assignment-expression        §8.18 [expr.ass]
 *   ternary  conditional-expression       §8.16 [expr.cond]
 *   logor    logical-or-expression        §8.15 [expr.log.or]
 *   logand   logical-and-expression       §8.14 [expr.log.and]
 *   bitor    inclusive-or-expression       §8.13 [expr.or]
 *   bitxor   exclusive-or-expression       §8.12 [expr.xor]
 *   bitand   and-expression               §8.11 [expr.bit.and]
 *   eq       equality-expression          §8.10 [expr.eq]
 *   rel      relational-expression        §8.9  [expr.rel]
 *            (C++20: compare-expression   N4861 §7.6.8 [expr.spaceship])
 *   shift    shift-expression             §8.8  [expr.shift]
 *   add      additive-expression          §8.7  [expr.add]
 *   mul      multiplicative-expression    §8.6  [expr.mul]
 *            (pointer-to-member §8.5 [expr.mptr.oper] — deferred)
 *   unary    unary-expression             §8.3  [expr.unary]
 *   postfix  postfix-expression           §8.2  [expr.post]
 *   primary  primary-expression           §8.1  [expr.prim]
 *
 * Precedence climbing handles levels mul through logor in a single
 * table-driven loop. Ternary and assignment are right-associative
 * and handled by dedicated recursive functions. Comma is left-assoc
 * but separated because most callers want assignment-expression
 * (comma only valid in full expression context).
 */

#include "parse.h"

/* Forward declarations for mutual recursion */
static Node *ternary_expr(Parser *p);
static Node *binary_expr(Parser *p, int min_prec);
static Node *unary_expr(Parser *p);
static Node *postfix_expr(Parser *p);
static Node *primary_expr(Parser *p);

/* ------------------------------------------------------------------ */
/* Precedence table                                                    */
/*                                                                     */
/* Lower number = lower precedence (binds less tightly).               */
/* All levels in this table are left-associative.                      */
/* Ternary (prec 3), assignment (prec 2), comma (prec 1) are NOT in   */
/* this table — they have dedicated functions.                         */
/*                                                                     */
/* C++20 change: <=> (spaceship) inserted between relational (prec 10) */
/* and shift (prec 11). We use prec 10 for relational, 11 for <=> in  */
/* C++20 mode, and 12 for shift.  To keep the table simple, we just   */
/* renumber: see below.                                                */
/* ------------------------------------------------------------------ */

/*
 * Precedence values. We use a scheme where there's room for <=>
 * between relational and shift without renumbering everything.
 */
enum {
    PREC_LOGOR    = 4,    /* §8.15 [expr.log.or]      || */
    PREC_LOGAND   = 5,    /* §8.14 [expr.log.and]     && */
    PREC_BITOR    = 6,    /* §8.13 [expr.or]          | */
    PREC_BITXOR   = 7,    /* §8.12 [expr.xor]         ^ */
    PREC_BITAND   = 8,    /* §8.11 [expr.bit.and]     & */
    PREC_EQUALITY = 9,    /* §8.10 [expr.eq]          == != */
    PREC_RELAT    = 10,   /* §8.9  [expr.rel]         < <= > >= */
    PREC_COMPARE  = 11,   /* N4861 §7.6.8 [expr.spaceship]  <=> (C++20) */
    PREC_SHIFT    = 12,   /* §8.8  [expr.shift]       << >> */
    PREC_ADD      = 13,   /* §8.7  [expr.add]         + - */
    PREC_MUL      = 14,   /* §8.6  [expr.mul]         * / % */
    PREC_MPTR     = 15,   /* §8.5  [expr.mptr.oper]   .* ->* */
};

static int get_binop_prec(Parser *p, TokenKind k) {
    switch (k) {
    case TK_LOR:                                     return PREC_LOGOR;
    case TK_LAND:                                    return PREC_LOGAND;
    case TK_PIPE:                                    return PREC_BITOR;
    case TK_CARET:                                   return PREC_BITXOR;
    case TK_AMP:                                     return PREC_BITAND;
    case TK_EQ: case TK_NE:                          return PREC_EQUALITY;
    case TK_LT: case TK_LE:                          return PREC_RELAT;
    case TK_GT: case TK_GE:
        /* N4659 §17.2/3 [temp.names]: inside a template-argument-list,
         * > is the closing delimiter, not greater-than. Similarly >>
         * splits into two >'s. Don't treat these as binary operators
         * when parsing template arguments. */
        if (p->template_depth > 0) return 0;
        return PREC_RELAT;
    case TK_SPACESHIP:
        if (p->std >= CPP20) return PREC_COMPARE;
        return 0;
    case TK_SHL:                                      return PREC_SHIFT;
    case TK_SHR:
        /* >> inside template args is two >'s, not shift */
        if (p->template_depth > 0) return 0;
        return PREC_SHIFT;
    case TK_PLUS: case TK_MINUS:                     return PREC_ADD;
    case TK_STAR: case TK_SLASH: case TK_PERCENT:    return PREC_MUL;
    case TK_DOTSTAR: case TK_ARROWSTAR:              return PREC_MPTR;
    default:                                          return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Primary expression — N4659 §8.1 [expr.prim]                        */
/*                                                                     */
/*   primary-expression:                                               */
/*       literal                                                       */
/*       this                     (deferred — classes)                  */
/*       ( expression )                                                */
/*       id-expression                                                 */
/*       lambda-expression        (deferred — C++11 §8.1.5)            */
/*       fold-expression          (deferred — C++17 §8.1.6)            */
/*       requires-expression      (deferred — C++20 §8.1.7.3)         */
/* ------------------------------------------------------------------ */

static Node *primary_expr(Parser *p) {
    Token *tok = parser_peek(p);

    switch (tok->kind) {
    case TK_NUM:        /* §5.13.2 [lex.icon] */
        parser_advance(p);
        return new_num_node(p, tok);

    case TK_FNUM:       /* §5.13.4 [lex.fcon] */
        parser_advance(p);
        return new_fnum_node(p, tok);

    case TK_STR: {      /* §5.13.5 [lex.string]
                         * Adjacent string literals are concatenated in
                         * translation phase 6 (§5.13.5/13). We consume all
                         * consecutive string tokens here; the semantic
                         * layer handles actual concatenation and encoding. */
        parser_advance(p);
        while (parser_at(p, TK_STR))
            parser_advance(p);
        Node *node = new_node(p, ND_STR, tok);
        node->str.tok = tok;
        return node;
    }

    case TK_CHAR: {     /* §5.13.3 [lex.ccon] */
        parser_advance(p);
        Node *node = new_node(p, ND_CHAR, tok);
        node->chr.tok = tok;
        return node;
    }

    /* Boolean literals — N4659 §5.13.6 [lex.bool]
     * Distinct from integer 0/1 — sema needs to know the type is bool. */
    case TK_KW_TRUE:
    case TK_KW_FALSE:
        parser_advance(p);
        return new_node(p, ND_BOOL_LIT, tok);

    case TK_KW_THIS: {  /* §8.1.3 [expr.prim.this] */
        parser_advance(p);
        Node *node = new_node(p, ND_IDENT, tok);
        node->ident.name = tok;
        return node;
    }

    case TK_KW_NULLPTR: /* §5.13.7 [lex.nullptr] — type std::nullptr_t */
        parser_advance(p);
        return new_node(p, ND_NULLPTR, tok);

    case TK_KW_NOEXCEPT: {
        /* noexcept-operator — N4659 §8.3.7 [expr.unary.noexcept]
         *   noexcept ( expression )
         * Yields a bool prvalue. Currently parsed and discarded into an
         * opaque BOOL_LIT-shaped node. */
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
        return new_node(p, ND_BOOL_LIT, tok);
    }

    case TK_LBRACE: {
        /* braced-init-list — N4659 §11.6.4 [dcl.init.list]
         *   { initializer-list(opt) ,(opt) }
         * As a primary expression in C++11 list-initialization (e.g.
         * 'return {};', 'f({1,2})'). Currently parsed and discarded into
         * an opaque NULLPTR-shaped node. */
        parser_advance(p);
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
        return new_node(p, ND_NULLPTR, tok);  /* opaque placeholder */
    }

    default:
        break;
    }

    /* Functional-cast / explicit type conversion — N4659 §8.2.3 [expr.type.conv]
     *   simple-type-specifier ( expression-list(opt) )
     *   simple-type-specifier braced-init-list
     * E.g. 'bool(x)', 'int(0)', 'typename T::U{}'. */
    if ((tok->kind == TK_KW_TYPENAME) ||
        (parser_at_type_specifier(p) && tok->kind != TK_IDENT &&
         (parser_peek_ahead(p, 1)->kind == TK_LPAREN ||
          parser_peek_ahead(p, 1)->kind == TK_LBRACE))) {
        /* Just the simple-type-specifier — no abstract declarator. The
         * '(' / '{' that follows is the init expression-list, NOT a
         * pointer or array suffix on the type. */
        Type *ty = parse_type_specifiers(p).type;
        TokenKind open  = parser_at(p, TK_LBRACE) ? TK_LBRACE : TK_LPAREN;
        TokenKind close = (open == TK_LBRACE)     ? TK_RBRACE : TK_RPAREN;
        parser_expect(p, open);
        int depth = 1;
        while (depth > 0 && !parser_at_eof(p)) {
            if (parser_at(p, open)) depth++;
            else if (parser_at(p, close)) {
                depth--;
                if (depth == 0) break;
            }
            parser_advance(p);
        }
        parser_expect(p, close);
        return new_cast_node(p, ty, /*operand=*/NULL, tok);
    }

    /* GCC/Clang type-trait intrinsics in expression context:
     *   __is_trivial(T), __is_assignable(T, U), __is_same(T, U), etc.
     * These are bool-valued built-ins whose arguments are TYPES, not
     * expressions, so we can't let parse_assign_expr try to evaluate
     * 'T&' as bitwise-and. Detect '__name (' for any unknown leading-
     * underscore identifier and skip the balanced parens. */
    if (tok->kind == TK_IDENT && tok->len >= 2 &&
        tok->loc[0] == '_' && tok->loc[1] == '_' &&
        parser_peek_ahead(p, 1)->kind == TK_LPAREN &&
        lookup_unqualified(p, tok->loc, tok->len) == NULL) {
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
        return new_node(p, ND_BOOL_LIT, name_tok);  /* opaque bool */
    }

    /* Identifier / qualified-id — N4659 §8.1.4 [expr.prim.id]
     *   id-expression: unqualified-id | qualified-id
     *   qualified-id: nested-name-specifier template(opt) unqualified-id
     *   nested-name-specifier: :: | type-name :: | namespace-name :: | ...
     *
     * Also handles global scope: ::foo
     *
     * N4659 §17.2/3 [temp.names] — Rule 4: template-name followed by
     * < opens a template-argument-list.
     */
    if (tok->kind == TK_IDENT || tok->kind == TK_SCOPE) {
        bool global_scope = false;
        Vec parts = vec_new(p->arena);

        /* Leading :: means global scope */
        if (tok->kind == TK_SCOPE) {
            global_scope = true;
            parser_advance(p);
        }

        /* ::operator new / ::operator delete / ::operator-symbol  —
         * after global '::' (or after a class qualifier), an operator-id
         * may appear. Consume 'operator' plus the operator symbol(s). */
        if (parser_at(p, TK_KW_OPERATOR)) {
            Token *op_tok = parser_advance(p);
            if (parser_at(p, TK_KW_NEW) || parser_at(p, TK_KW_DELETE)) {
                parser_advance(p);
                if (parser_consume(p, TK_LBRACKET))
                    parser_expect(p, TK_RBRACKET);
            } else if (parser_consume(p, TK_LPAREN)) {
                parser_expect(p, TK_RPAREN);
            } else if (parser_consume(p, TK_LBRACKET)) {
                parser_expect(p, TK_RBRACKET);
            } else if (parser_peek(p)->kind >= TK_LPAREN &&
                       parser_peek(p)->kind <= TK_HASHHASH) {
                parser_advance(p);
            }
            vec_push(&parts, op_tok);
        }
        /* Consume the name chain: A :: B :: C  or  A<int> :: B
         * Terminates: each iteration consumes ident (+ optional <args>) + ::, or breaks. */
        if (parser_at(p, TK_IDENT)) {
            Token *name = parser_advance(p);
            vec_push(&parts, name);

            /* If this name is a template and followed by <, consume the
             * template-argument-list before checking for :: */
            if (parser_at(p, TK_LT) && lookup_is_template_name(p, name)) {
                parse_template_id(p, name);  /* consumes <args> */
            }

            while (parser_at(p, TK_SCOPE)) {
                parser_advance(p);  /* consume :: */

                /* N4659 §17.2/4 [temp.names]: 'template' disambiguator
                 * for a dependent member template-id, e.g. 'T::template f<X>()'. */
                parser_consume(p, TK_KW_TEMPLATE);

                if (parser_at(p, TK_IDENT)) {
                    name = parser_advance(p);
                    vec_push(&parts, name);
                    /* Template-id in the chain: A::B<int>::C.
                     * In a qualified-name, '<' after a segment is
                     * overwhelmingly a template-argument-list — we can't
                     * do qualified lookup, so accept it speculatively. */
                    if (parser_at(p, TK_LT)) {
                        parse_template_id(p, name);
                    }
                } else if (parser_at(p, TK_TILDE)) {
                    /* Qualified destructor: A::~B */
                    parser_advance(p);
                    if (parser_at(p, TK_IDENT))
                        vec_push(&parts, parser_advance(p));
                    break;
                } else {
                    break;
                }
            }
        }

        /* If we consumed just one name with no ::, it's a simple ident */
        if (parts.len == 1 && !global_scope) {
            Token *name = (Token *)parts.data[0];

            /* Rule 4: template-name followed by < → template-id */
            if (parser_at(p, TK_LT) && lookup_is_template_name(p, name)) {
                Node *tid = parse_template_id(p, name);
                /* template-id may be followed by :: nested-name segments
                 * (e.g. conjunction<...>::value). Consume the trailing
                 * chain — the value is opaque to the parser, sema resolves. */
                while (parser_consume(p, TK_SCOPE)) {
                    parser_consume(p, TK_KW_TEMPLATE);
                    if (parser_at(p, TK_IDENT)) {
                        Token *seg = parser_advance(p);
                        if (parser_at(p, TK_LT) &&
                            lookup_is_template_name(p, seg))
                            parse_template_id(p, seg);
                    }
                }
                return tid;
            }

            Node *node = new_node(p, ND_IDENT, name);
            node->ident.name = name;
            return node;
        }

        /* Qualified name: A::B::C or ::foo */
        Token *last = parts.len > 0 ? (Token *)parts.data[parts.len - 1] : tok;

        /* Rule 4: final name might be a template-id: A::B<int> */
        if (parser_at(p, TK_LT) && lookup_is_template_name(p, last))
            return parse_template_id(p, last);

        return new_qualified_node(p, (Token **)parts.data, parts.len,
                                  global_scope, tok);
    }

    /* C++ named casts — N4659 §8.2.3-§8.2.7 [expr.cast]
     *   static_cast < type-id > ( expression )
     *   dynamic_cast < type-id > ( expression )
     *   reinterpret_cast < type-id > ( expression )
     *   const_cast < type-id > ( expression )
     *
     * C++20/23: unchanged. */
    if (tok->kind == TK_KW_STATIC_CAST || tok->kind == TK_KW_DYNAMIC_CAST ||
        tok->kind == TK_KW_REINTERPRET_CAST || tok->kind == TK_KW_CONST_CAST) {
        parser_advance(p);
        parser_expect(p, TK_LT);
        Type *ty = parse_type_name(p);
        parser_expect(p, TK_GT);
        parser_expect(p, TK_LPAREN);
        Node *operand = parse_expr(p);
        parser_expect(p, TK_RPAREN);
        return new_cast_node(p, ty, operand, tok);
    }

    /* Parenthesized expression — §8.1.5 [expr.prim.paren]
     *   ( expression )
     *
     * Note: this is also where C-style casts (type)expr would be
     * disambiguated. For the first pass, if the token after '(' is
     * a type keyword, we parse a cast. Otherwise, parenthesized expr.
     * Full disambiguation (Rule 1/2) requires the type-name oracle. */
    if (tok->kind == TK_LPAREN) {
        parser_advance(p);

        /* Try C-style cast: (type)expr — §8.4 [expr.cast]
         * cast-expression: unary-expression | ( type-id ) cast-expression
         *
         * A type-name like 'std::foo<T>::value' could syntactically be
         * parsed as a type AND as a qualified-id expression. We commit to
         * the cast interpretation only if the token after ')' can start a
         * unary-expression — anything else (binary op, ',', ';', ']', etc.)
         * means we're really parsing a parenthesized expression. */
        if (parser_at_type_specifier(p)) {
            ParseState saved = parser_save(p);
            p->tentative = true;
            Type *ty = parse_type_name(p);
            bool ok = (ty != NULL) && parser_at(p, TK_RPAREN);
            if (ok) {
                /* Look at the token after the ')' */
                TokenKind k = parser_peek_ahead(p, 1)->kind;
                /* Tokens that cannot start a unary-expression — these
                 * indicate we're really parsing a parenthesized expression,
                 * not a cast. Note: '+', '-', '*', '&', '!', '~' CAN start
                 * a unary-expression and so are not in this list. */
                switch (k) {
                case TK_RPAREN: case TK_RBRACE: case TK_RBRACKET:
                case TK_SEMI: case TK_COMMA: case TK_COLON: case TK_QUESTION:
                case TK_ASSIGN: case TK_DOT: case TK_ARROW:
                case TK_SLASH: case TK_PERCENT: case TK_CARET: case TK_PIPE:
                case TK_LT: case TK_GT: case TK_LE: case TK_GE:
                case TK_EQ: case TK_NE: case TK_LAND: case TK_LOR:
                case TK_SHL: case TK_SHR: case TK_DOTSTAR: case TK_ARROWSTAR:
                    ok = false;
                    break;
                default:
                    break;
                }
            }
            p->tentative = false;
            if (ok) {
                parser_advance(p);  /* ) */
                Node *operand = unary_expr(p);
                return new_cast_node(p, ty, operand, tok);
            }
            parser_restore(p, saved);
        }

        Node *node = parse_expr(p);
        parser_expect(p, TK_RPAREN);
        return node;
    }

    /* sizeof — N4659 §8.3.3 [expr.sizeof]
     *   sizeof unary-expression
     *   sizeof ( type-id )
     *   sizeof ... ( identifier )  (C++11 parameter pack — deferred) */
    if (tok->kind == TK_KW_SIZEOF) {
        parser_advance(p);
        Node *node = new_node(p, ND_SIZEOF, tok);

        /* C++11 sizeof... pack — N4659 §8.3.3 [expr.sizeof]/5
         *   sizeof ... ( identifier ) */
        if (parser_consume(p, TK_ELLIPSIS)) {
            parser_expect(p, TK_LPAREN);
            if (parser_at(p, TK_IDENT)) parser_advance(p);
            parser_expect(p, TK_RPAREN);
            node->sizeof_.is_type = false;
            return node;
        }

        if (parser_consume(p, TK_LPAREN)) {
            if (parser_at_type_specifier(p)) {
                node->sizeof_.ty = parse_type_name(p);
                node->sizeof_.is_type = true;
            } else {
                node->sizeof_.expr = parse_expr(p);
                node->sizeof_.is_type = false;
            }
            parser_expect(p, TK_RPAREN);
        } else {
            node->sizeof_.expr = unary_expr(p);
            node->sizeof_.is_type = false;
        }
        return node;
    }

    /* alignof — N4659 §8.3.6 [expr.alignof]
     *   alignof ( type-id )
     * Always takes a type, never an expression. */
    if (tok->kind == TK_KW_ALIGNOF) {
        parser_advance(p);
        Node *node = new_node(p, ND_ALIGNOF, tok);
        parser_expect(p, TK_LPAREN);
        node->alignof_.ty = parse_type_name(p);
        parser_expect(p, TK_RPAREN);
        return node;
    }

    if (p->tentative) return NULL;
    error_tok(tok, "expected expression");
}

/* ------------------------------------------------------------------ */
/* Postfix expression — N4659 §8.2 [expr.post]                        */
/*                                                                     */
/*   postfix-expression:                                               */
/*       primary-expression                                            */
/*       postfix-expression [ expr-or-braced-init-list ]  (subscript)  */
/*       postfix-expression ( expression-list(opt) )      (call)       */
/*       postfix-expression . id-expression               (member)     */
/*       postfix-expression -> id-expression              (member)     */
/*       postfix-expression ++                            (post-inc)   */
/*       postfix-expression --                            (post-dec)   */
/*       // Also: simple-type-specifier/typename ( ... )  (functional cast) */
/*       // Also: simple-type-specifier/typename ( ... ) (functional cast) */
/*       // C++20: no changes to postfix grammar                       */
/*       // C++23: adds multidimensional subscript a[i,j]              */
/* ------------------------------------------------------------------ */

static Node *postfix_expr(Parser *p) {
    Node *node = primary_expr(p);
    if (!node) return NULL;

    /* Terminates: each iteration consumes at least one token (the
     * postfix operator). Breaks when current token is not a postfix op.
     * Token array is finite, so pos advances toward EOF. */
    for (;;) {
        Token *tok = parser_peek(p);

        /* Function call — §8.2.2 [expr.call]
         *   postfix-expression ( expression-list(opt) ) */
        if (tok->kind == TK_LPAREN) {
            parser_advance(p);
            Vec args = vec_new(p->arena);
            if (!parser_at(p, TK_RPAREN)) {
                vec_push(&args, parse_assign_expr(p));
                parser_consume(p, TK_ELLIPSIS);  /* pack expansion */
                while (parser_consume(p, TK_COMMA)) {
                    vec_push(&args, parse_assign_expr(p));
                    parser_consume(p, TK_ELLIPSIS);
                }
            }
            parser_expect(p, TK_RPAREN);

            node = new_call_node(p, node, (Node **)args.data, args.len, tok);
            continue;
        }

        /* Subscript — §8.2.1 [expr.sub]
         *   postfix-expression [ expression ]
         * C++23: a[i, j] multidimensional — deferred */
        if (tok->kind == TK_LBRACKET) {
            parser_advance(p);
            Node *index = parse_expr(p);
            parser_expect(p, TK_RBRACKET);

            node = new_subscript_node(p, node, index, tok);
            continue;
        }

        /* Member access — §8.2.5 [expr.ref]
         *   postfix-expression . id-expression
         *   postfix-expression -> id-expression */
        if (tok->kind == TK_DOT || tok->kind == TK_ARROW) {
            TokenKind op = tok->kind;
            parser_advance(p);
            /* Optional 'template' disambiguator: x.template f<int>() */
            parser_consume(p, TK_KW_TEMPLATE);
            /* Pseudo-destructor / explicit operator method call:
             *   x.operator OP    (e.g. __t.operator->())
             *   x.~T()           (pseudo-destructor) */
            Token *member;
            if (parser_at(p, TK_KW_OPERATOR)) {
                member = parser_advance(p);
                /* Consume the operator-symbol (one or two tokens). */
                if (parser_consume(p, TK_LPAREN))
                    parser_expect(p, TK_RPAREN);
                else if (parser_consume(p, TK_LBRACKET))
                    parser_expect(p, TK_RBRACKET);
                else if (parser_at(p, TK_KW_NEW) || parser_at(p, TK_KW_DELETE)) {
                    parser_advance(p);
                    if (parser_consume(p, TK_LBRACKET))
                        parser_expect(p, TK_RBRACKET);
                } else if (parser_peek(p)->kind >= TK_LPAREN &&
                           parser_peek(p)->kind <= TK_HASHHASH) {
                    parser_advance(p);
                }
            } else if (parser_at(p, TK_TILDE)) {
                parser_advance(p);
                member = parser_expect(p, TK_IDENT);
            } else {
                member = parser_expect(p, TK_IDENT);
            }

            node = new_member_node(p, node, member, op, tok);
            continue;
        }

        /* Post-increment/decrement — §8.2.6 [expr.post.incr] */
        if (tok->kind == TK_INC || tok->kind == TK_DEC) {
            TokenKind op = tok->kind;
            parser_advance(p);
            Node *post = new_node(p, ND_POSTFIX, tok);
            post->unary.op = op;
            post->unary.operand = node;
            node = post;
            continue;
        }

        break;
    }

    return node;
}

/* ------------------------------------------------------------------ */
/* Unary expression — N4659 §8.3 [expr.unary]                         */
/*                                                                     */
/*   unary-expression:                                                 */
/*       postfix-expression                                            */
/*       ++ cast-expression          (pre-increment)                   */
/*       -- cast-expression          (pre-decrement)                   */
/*       unary-operator cast-expression                                */
/*       sizeof unary-expression                                       */
/*       sizeof ( type-id )                                            */
/*       alignof ( type-id )                                           */
/*       noexcept-expression         (deferred)                        */
/*       new-expression              (deferred — §8.3.4)               */
/*       delete-expression           (deferred — §8.3.5)               */
/*       // C++20: co_await cast-expression (§8.3.8 [expr.await])      */
/*                                                                     */
/*   unary-operator: * & + - ! ~                                       */
/* ------------------------------------------------------------------ */

static Node *unary_expr(Parser *p) {
    Token *tok = parser_peek(p);

    /* new-expression — N4659 §8.3.4 [expr.new]
     *   ::opt new new-placement(opt) new-type-id new-initializer(opt)
     *   ::opt new new-placement(opt) ( type-id ) new-initializer(opt)
     *
     * new-placement: ( expression-list )
     * new-initializer: ( expression-list(opt) ) | braced-init-list
     *
     * Also handles global scope ::new and ::delete. */
    if (tok->kind == TK_KW_NEW ||
        (tok->kind == TK_SCOPE && parser_peek_ahead(p, 1)->kind == TK_KW_NEW)) {
        if (tok->kind == TK_SCOPE) parser_advance(p);
        parser_advance(p);  /* consume 'new' */

        /* Optional placement args: new (args) Type
         * The ( could be placement or a parenthesized type-id.
         * Heuristic: if what follows ( looks like a type and ) follows,
         * it's the type. Otherwise it's placement args.
         * For a bootstrap tool, we try placement first (tentative),
         * then fall through to type parsing. */
        if (parser_at(p, TK_LPAREN) &&
            !(parser_peek_ahead(p, 1)->kind == TK_RPAREN)) {
            /* Tentative: try as placement args */
            ParseState saved = parser_save(p);
            p->tentative = true;
            parser_advance(p);  /* ( */
            parse_expr(p);
            bool ok = parser_at(p, TK_RPAREN);
            p->tentative = false;
            if (ok) {
                /* Check: after ), does a type follow? If so, it was placement. */
                parser_restore(p, saved);
                parser_advance(p);  /* ( */
                parse_expr(p);
                parser_expect(p, TK_RPAREN);
            } else {
                parser_restore(p, saved);
            }
        }

        /* Type being allocated */
        Type *ty = parse_type_name(p);

        /* Optional initializer: (args) or {} */
        if (parser_consume(p, TK_LPAREN)) {
            if (!parser_at(p, TK_RPAREN)) {
                /* Parse comma-separated args; consume optional '...' pack
                 * expansion after each. */
                parse_assign_expr(p);
                parser_consume(p, TK_ELLIPSIS);
                while (parser_consume(p, TK_COMMA)) {
                    parse_assign_expr(p);
                    parser_consume(p, TK_ELLIPSIS);
                }
            }
            parser_expect(p, TK_RPAREN);
        }

        return new_cast_node(p, ty, /*operand=*/NULL, tok);  /* reuse CAST for now */
    }

    /* delete-expression — N4659 §8.3.5 [expr.delete]
     *   ::opt delete cast-expression
     *   ::opt delete [] cast-expression
     *
     * Special case: '= delete' / '= default' as a function-body
     * substitute (N4659 §10.1.6 [dcl.fct.def.delete] /
     * §10.1.6.4 [dcl.fct.def.default]). When parsed in an expression
     * context where the next token is ';' (or ',' for init-declarators),
     * treat 'delete' / 'default' as an opaque marker rather than a
     * delete-expression with no operand. */
    if ((tok->kind == TK_KW_DELETE || tok->kind == TK_KW_DEFAULT) &&
        (parser_peek_ahead(p, 1)->kind == TK_SEMI ||
         parser_peek_ahead(p, 1)->kind == TK_COMMA)) {
        parser_advance(p);
        return new_node(p, ND_NULLPTR, tok);
    }
    if (tok->kind == TK_KW_DELETE ||
        (tok->kind == TK_SCOPE && parser_peek_ahead(p, 1)->kind == TK_KW_DELETE)) {
        if (tok->kind == TK_SCOPE) parser_advance(p);
        parser_advance(p);  /* consume 'delete' */
        /* delete[] */
        if (parser_consume(p, TK_LBRACKET))
            parser_expect(p, TK_RBRACKET);
        Node *operand = unary_expr(p);
        return new_unary_node(p, TK_KW_DELETE, operand, tok);
    }

    /* Pre-increment/decrement — §8.3.1 [expr.pre.incr] */
    if (tok->kind == TK_INC || tok->kind == TK_DEC) {
        parser_advance(p);
        Node *operand = unary_expr(p);
        return new_unary_node(p, tok->kind, operand, tok);
    }

    /* Unary operators — §8.3.1 [expr.unary.op]
     *   * (indirection), & (address-of), + - (arithmetic), ! ~ (logical/bitwise NOT) */
    if (tok->kind == TK_STAR || tok->kind == TK_AMP ||
        tok->kind == TK_PLUS || tok->kind == TK_MINUS ||
        tok->kind == TK_EXCL || tok->kind == TK_TILDE) {
        parser_advance(p);
        Node *operand = unary_expr(p);  /* cast-expression in the grammar, but we simplify */
        return new_unary_node(p, tok->kind, operand, tok);
    }

    /* sizeof and alignof are handled in primary_expr (they need parens logic) */

    return postfix_expr(p);
}

/* ------------------------------------------------------------------ */
/* Binary expression — N4659 §8.5-§8.15                                */
/*                                                                     */
/* Precedence climbing: a single loop handles all left-associative     */
/* binary operators from multiplicative (§8.6) through logical-or      */
/* (§8.15), using the table returned by get_binop_prec().              */
/*                                                                     */
/* The loop invariant: parse operators with precedence >= min_prec.    */
/* For left-associative operators, the right operand is parsed with    */
/* min_prec + 1, ensuring left-to-right grouping.                      */
/*                                                                     */
/* C++20 change: three-way comparison <=> (N4861 §7.6.8) is inserted  */
/* at PREC_COMPARE between relational and shift. It's left-assoc.     */
/* C++23: no changes to binary expression precedence.                  */
/* ------------------------------------------------------------------ */

static Node *binary_expr(Parser *p, int min_prec) {
    Node *lhs = unary_expr(p);
    if (!lhs) return NULL;

    /* Terminates: each iteration consumes the binary operator token
     * plus at least one token for the RHS (via unary_expr). Breaks
     * when prec == 0 (not a binary op) or prec < min_prec. EOF
     * has prec 0, guaranteeing exit. */
    for (;;) {
        Token *op_tok = parser_peek(p);
        int prec = get_binop_prec(p, op_tok->kind);
        if (prec == 0 || prec < min_prec)
            break;

        TokenKind op = op_tok->kind;
        parser_advance(p);

        /* Left-associative: right side binds tighter (prec + 1) */
        Node *rhs = binary_expr(p, prec + 1);
        if (!rhs) return NULL;

        lhs = new_binary_node(p, op, lhs, rhs, op_tok);
    }

    return lhs;
}

/* ------------------------------------------------------------------ */
/* Conditional (ternary) expression — N4659 §8.16 [expr.cond]          */
/*                                                                     */
/*   conditional-expression:                                           */
/*       logical-or-expression                                         */
/*       logical-or-expression ? expression : assignment-expression    */
/*                                                                     */
/* Right-associative: a ? b : c ? d : e  ==  a ? b : (c ? d : e)      */
/* The 'then' branch is a full expression (comma allowed).             */
/* The 'else' branch is an assignment-expression.                      */
/*                                                                     */
/* C++20/23: unchanged.                                                */
/* ------------------------------------------------------------------ */

static Node *ternary_expr(Parser *p) {
    Node *cond = binary_expr(p, PREC_LOGOR);
    if (!cond) return NULL;

    if (!parser_consume(p, TK_QUESTION))
        return cond;

    Token *tok = &p->tokens[p->pos > 0 ? p->pos - 1 : 0];

    /* §8.16/1: "expression" in then-branch (comma is allowed) */
    Node *then_ = parse_expr(p);
    parser_expect(p, TK_COLON);
    /* §8.16/1: "assignment-expression" in else-branch */
    Node *else_ = parse_assign_expr(p);

    return new_ternary_node(p, cond, then_, else_, tok);
}

/* ------------------------------------------------------------------ */
/* Assignment expression — N4659 §8.18 [expr.ass]                      */
/*                                                                     */
/*   assignment-expression:                                            */
/*       conditional-expression                                        */
/*       logical-or-expression assignment-operator initializer-clause  */
/*       throw-expression               (deferred — no exceptions)     */
/*       // C++20: yield-expression (co_yield — deferred)              */
/*                                                                     */
/*   assignment-operator: = *= /= %= += -= >>= <<= &= ^= |=          */
/*                                                                     */
/* Right-associative: a = b = c  ==  a = (b = c)                       */
/* ------------------------------------------------------------------ */

static bool is_assign_op(TokenKind k) {
    switch (k) {
    case TK_ASSIGN:
    case TK_PLUS_ASSIGN: case TK_MINUS_ASSIGN:
    case TK_STAR_ASSIGN: case TK_SLASH_ASSIGN: case TK_PERCENT_ASSIGN:
    case TK_SHL_ASSIGN: case TK_SHR_ASSIGN:
    case TK_AMP_ASSIGN: case TK_PIPE_ASSIGN: case TK_CARET_ASSIGN:
        return true;
    default:
        return false;
    }
}

Node *parse_assign_expr(Parser *p) {
    Node *lhs = ternary_expr(p);
    if (!lhs) return NULL;

    Token *tok = parser_peek(p);
    if (is_assign_op(tok->kind)) {
        TokenKind op = tok->kind;
        parser_advance(p);
        /* Right-associative: recurse into assign_expr for the RHS */
        Node *rhs = parse_assign_expr(p);

        Node *node = new_node(p, ND_ASSIGN, tok);
        node->binary.op = op;
        node->binary.lhs = lhs;
        node->binary.rhs = rhs;
        return node;
    }

    return lhs;
}

/* ------------------------------------------------------------------ */
/* Comma expression — N4659 §8.19 [expr.comma]                        */
/*                                                                     */
/*   expression:                                                       */
/*       assignment-expression                                         */
/*       expression , assignment-expression                            */
/*                                                                     */
/* Left-associative. Most parse contexts use assignment-expression;    */
/* comma is only valid in full expression context (expression-         */
/* statements, for-loop init/inc, function-style init).                */
/*                                                                     */
/* C++20/23: unchanged.                                                */
/* ------------------------------------------------------------------ */

Node *parse_expr(Parser *p) {
    Node *lhs = parse_assign_expr(p);
    if (!lhs) return NULL;

    while (parser_consume(p, TK_COMMA)) {
        Token *tok = &p->tokens[p->pos > 0 ? p->pos - 1 : 0];
        Node *rhs = parse_assign_expr(p);
        Node *node = new_node(p, ND_COMMA, tok);
        node->comma.lhs = lhs;
        node->comma.rhs = rhs;
        lhs = node;
    }

    return lhs;
}
