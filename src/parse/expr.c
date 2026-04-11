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
 *   mptr     pointer-to-member            §8.5  [expr.mptr.oper] (.* ->*)
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
    case TK_LT:                                      return PREC_RELAT;
    case TK_LE:                                      return PREC_RELAT;
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
/*                                                                    */
/*   primary-expression:                                              */
/*       literal                  — implemented (num/fnum/str/char/   */
/*                                  bool/nullptr)                     */
/*       this                     — implemented (§8.1.3)              */
/*       ( expression )           — implemented                       */
/*       id-expression            — implemented (incl. qualified-id)  */
/*       lambda-expression        — parsed-and-discarded (§8.1.5);    */
/*                                  the brace-balanced skip in this   */
/*                                  function recognises the shape so  */
/*                                  surrounding code parses, but the  */
/*                                  body isn't built into an AST node */
/*       fold-expression          — implemented (§8.1.6, C++17)       */
/*       requires-expression      — NOT YET (§8.1.7.3, C++20)         */
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
        node->ident.name = tok; node->ident.implicit_this = false; node->ident.resolved_decl = NULL;
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

    case TK_LBRACKET: {
        /* lambda-expression — N4659 §8.1.5 [expr.prim.lambda]
         *   lambda-introducer lambda-declarator(opt) compound-statement
         *   lambda-introducer: [ lambda-capture(opt) ]
         *
         * C++20: adds explicit template-parameter-list
         *   (lambda-introducer <T-params> requires-clause(opt) ...),
         *   pack-expansion in init-capture.
         * C++23: adds static lambda-specifier, restructures
         *   lambda-declarator with lambda-specifier-seq.
         *
         * '[' as a primary expression is unambiguously a lambda
         * (postfix '[' subscripts an existing operand and is handled
         * by the postfix loop, not here). Skip-and-discard for now —
         * we don't lower lambdas yet, but we MUST parse past them so
         * surrounding code stays parseable. The result is an opaque
         * placeholder.
         *
         * Sequence to skip:
         *   1. capture list  [ ... ]    (balanced bracket count)
         *   2. optional template-parameter-list (C++20)
         *   3. optional lambda-declarator (params + specifiers + ret)
         *   4. compound-statement { ... }
         */
        parser_advance(p);  /* consume [ */
        int depth = 1;
        while (depth > 0 && !parser_at_eof(p)) {
            if (parser_at(p, TK_LBRACKET)) depth++;
            else if (parser_at(p, TK_RBRACKET)) {
                depth--;
                if (depth == 0) break;
            }
            parser_advance(p);
        }
        parser_expect(p, TK_RBRACKET);
        /* Optional template parameter list (C++20 generic lambda
         * with explicit template params). */
        if (parser_at(p, TK_LT)) {
            int adepth = 1;
            parser_advance(p);
            while (adepth > 0 && !parser_at_eof(p)) {
                if (parser_at(p, TK_LT)) adepth++;
                else if (parser_at(p, TK_GT)) {
                    adepth--;
                    if (adepth == 0) break;
                } else if (parser_at(p, TK_SHR)) {
                    adepth -= 2;
                    if (adepth <= 0) break;
                }
                parser_advance(p);
            }
            if (parser_at(p, TK_GT) || parser_at(p, TK_SHR))
                parser_advance(p);
        }
        /* Optional lambda-declarator (params + specifiers). */
        if (parser_consume(p, TK_LPAREN)) {
            int pdepth = 1;
            while (pdepth > 0 && !parser_at_eof(p)) {
                if (parser_at(p, TK_LPAREN)) pdepth++;
                else if (parser_at(p, TK_RPAREN)) {
                    pdepth--;
                    if (pdepth == 0) break;
                }
                parser_advance(p);
            }
            parser_expect(p, TK_RPAREN);
            /* Skip declarator suffixes: mutable, constexpr,
             * noexcept(...), -> trailing-return-type, attributes.
             * Stop at '{' which begins the body. */
            while (!parser_at(p, TK_LBRACE) && !parser_at_eof(p)) {
                if (parser_at(p, TK_LBRACE)) break;
                parser_advance(p);
            }
        }
        /* Compound-statement body — must be present. */
        if (parser_consume(p, TK_LBRACE)) {
            int bdepth = 1;
            while (bdepth > 0 && !parser_at_eof(p)) {
                if (parser_at(p, TK_LBRACE)) bdepth++;
                else if (parser_at(p, TK_RBRACE)) {
                    bdepth--;
                    if (bdepth == 0) break;
                }
                parser_advance(p);
            }
            parser_expect(p, TK_RBRACE);
        }
        /* Opaque placeholder — sema/codegen don't model lambdas. */
        return new_node(p, ND_NULLPTR, tok);
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
          parser_peek_ahead(p, 1)->kind == TK_LBRACE)) ||
        /* T{} braced functional cast even when T is an identifier
         * type-name — e.g. '__tag{}'. We can be more permissive than
         * the LPAREN case because '{' after a name is unambiguously
         * a constructor/braced-init. */
        (tok->kind == TK_IDENT && parser_peek_ahead(p, 1)->kind == TK_LBRACE &&
         parser_at_type_specifier(p))) {
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
    /* throw-expression — N4659 §8.17 [expr.throw]
     *   throw assignment-expression(opt)
     * Yields a void prvalue. We parse and discard the operand. */
    if (tok->kind == TK_KW_THROW) {
        parser_advance(p);
        /* Operand is optional — 'throw;' alone is a re-throw. */
        if (parser_peek(p)->kind != TK_SEMI &&
            parser_peek(p)->kind != TK_RPAREN &&
            parser_peek(p)->kind != TK_COMMA &&
            parser_peek(p)->kind != TK_RBRACE)
            unary_expr(p);  /* parse, discard */
        return new_node(p, ND_NULLPTR, tok);
    }

    /* Bare 'operator OP' as an id-expression — N4659 §16.5 [over.oper].
     * E.g. 'return !operator==(__arg)' calls the member operator==
     * explicitly. Consume 'operator' + symbol and treat as a plain
     * identifier; postfix '(' will then parse it as a call. */
    if (tok->kind == TK_KW_OPERATOR) {
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
        Node *node = new_node(p, ND_IDENT, op_tok);
        node->ident.name = op_tok; node->ident.implicit_this = false; node->ident.resolved_decl = NULL;
        return node;
    }

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
            /* Speculative template-id: parse 'name<args>' if 'name' is
             * a known template OR if the token after '<' is something
             * that can only start a type/template argument (a type
             * keyword), since 'less-than' would never be followed by
             * a bare type keyword. */
            if (parser_at(p, TK_LT)) {
                Token *after = parser_peek_ahead(p, 1);
                bool looks_template_arg = false;
                switch (after->kind) {
                case TK_KW_VOID: case TK_KW_BOOL: case TK_KW_CHAR:
                case TK_KW_SHORT: case TK_KW_INT: case TK_KW_LONG:
                case TK_KW_FLOAT: case TK_KW_DOUBLE:
                case TK_KW_SIGNED: case TK_KW_UNSIGNED:
                case TK_KW_WCHAR_T: case TK_KW_CHAR16_T: case TK_KW_CHAR32_T:
                case TK_KW_CONST: case TK_KW_VOLATILE:
                case TK_KW_STRUCT: case TK_KW_CLASS: case TK_KW_UNION:
                case TK_KW_ENUM: case TK_KW_TYPENAME: case TK_KW_DECLTYPE:
                case TK_KW_AUTO:
                    looks_template_arg = true;
                    break;
                default:
                    break;
                }
                /* Inside another template-argument-list (or default
                 * value), '<' is *usually* a nested template-id — but
                 * not always: 'template<unsigned __w, bool = __w < 5>'
                 * has __w as a non-type parameter and '<' is less-than.
                 * Treat as template-id when:
                 *   - the name is a known template, or
                 *   - the next token can only start a type/template arg
                 *     (a type keyword), or
                 *   - we're in a template-arg context AND the name is
                 *     NOT in lookup as a non-type entity (variable /
                 *     non-type template parameter). */
                /* Check if a non-template entity (variable, function)
                 * shadows a template name in the current scope. Per
                 * §6.3.10/2, a variable hides a class/template name.
                 * If the FIRST match in lookup is a variable, '<' is
                 * less-than, not template-args. */
                bool is_nontype_var = false;
                {
                    Declaration *d = lookup_unqualified(p, name->loc, name->len);
                    if (d && d->entity == ENTITY_VARIABLE &&
                        !(d->type && d->type->kind == TY_FUNC))
                        is_nontype_var = true;
                }
                if (!is_nontype_var &&
                    (lookup_is_template_name(p, name) || looks_template_arg ||
                     (p->template_depth > 0))) {
                    Node *tid = parse_template_id(p, name);
                    /* If no :: follows, this was a standalone
                     * template-id (e.g. max_of<int>(...)) — return
                     * the ND_TEMPLATE_ID directly so the call site
                     * can use it for instantiation. If :: follows,
                     * it's an intermediate segment in a qualified
                     * name and we continue building parts. */
                    if (!parser_at(p, TK_SCOPE) && parts.len == 1)
                        return tid;
                }
            }

            while (parser_at(p, TK_SCOPE)) {
                parser_advance(p);  /* consume :: */

                /* N4659 §17.2/4 [temp.names]: 'template' disambiguator
                 * for a dependent member template-id, e.g. 'T::template f<X>()'. */
                parser_consume(p, TK_KW_TEMPLATE);

                if (parser_at(p, TK_IDENT)) {
                    name = parser_advance(p);
                    vec_push(&parts, name);
                    /* Template-id in a qualified-name chain: A::B<int>::C.
                     * Speculative tentative parse: only commit to '<'
                     * as template-args when the parse closes cleanly
                     * AND is followed by '::' (continuing the qualified
                     * chain) or by '(' (function-style call on a
                     * member template). Otherwise '<' is a relational
                     * operator on the fully-qualified value, e.g.
                     * 'numeric_limits<T>::digits < 64'. */
                    if (parser_at(p, TK_LT)) {
                        ParseState saved2 = parser_save(p);
                        bool prev_t = p->tentative;
                        bool saved_failed = p->tentative_failed;
                        p->tentative = true;
                        p->tentative_failed = false;
                        parse_template_id(p, name);
                        bool ok = !p->tentative_failed &&
                                  (parser_at(p, TK_SCOPE) ||
                                   parser_at(p, TK_LPAREN) ||
                                   parser_at(p, TK_LBRACE));
                        p->tentative = prev_t;
                        p->tentative_failed = saved_failed;
                        parser_restore(p, saved2);
                        if (ok)
                            parse_template_id(p, name);
                    }
                } else if (parser_at(p, TK_TILDE)) {
                    /* Qualified destructor: A::~B */
                    parser_advance(p);
                    if (parser_at(p, TK_IDENT))
                        vec_push(&parts, parser_advance(p));
                    break;
                } else if (parser_at(p, TK_KW_OPERATOR)) {
                    /* Qualified operator: A::operator[] / A::operator+ */
                    Token *op_tok = parser_advance(p);
                    vec_push(&parts, op_tok);
                    if (parser_consume(p, TK_LPAREN))
                        parser_expect(p, TK_RPAREN);   /* operator() */
                    else if (parser_consume(p, TK_LBRACKET))
                        parser_expect(p, TK_RBRACKET); /* operator[] */
                    else if (parser_at(p, TK_KW_NEW) || parser_at(p, TK_KW_DELETE)) {
                        parser_advance(p);
                        if (parser_consume(p, TK_LBRACKET))
                            parser_expect(p, TK_RBRACKET);
                    } else if (parser_peek(p)->kind >= TK_LPAREN &&
                               parser_peek(p)->kind <= TK_HASHHASH) {
                        parser_advance(p);
                    }
                    break;
                } else {
                    break;
                }
            }
        }

        /* If we consumed just one name with no ::, it's a simple ident */
        if (parts.len == 1 && !global_scope) {
            Token *name = (Token *)parts.data[0];

            /* Rule 4: template-name followed by < → template-id.
             * But if a variable/function shadows the template name
             * in the current scope (§6.3.10/2), '<' is less-than. */
            {
                Declaration *shadow = lookup_unqualified(p, name->loc, name->len);
                if (shadow && shadow->entity == ENTITY_VARIABLE &&
                    !(shadow->type && shadow->type->kind == TY_FUNC))
                    goto simple_ident;
            }
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

        simple_ident:;
            Node *node = new_node(p, ND_IDENT, name);
            node->ident.name = name; node->ident.implicit_this = false; node->ident.resolved_decl = NULL;
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
            bool prev_tentative = p->tentative;
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
            p->tentative = prev_tentative;
            if (ok) {
                parser_advance(p);  /* ) */
                Node *operand = unary_expr(p);
                return new_cast_node(p, ty, operand, tok);
            }
            parser_restore(p, saved);
        }

        /* Inside '(...)' we are no longer at the immediate template-arg
         * level, so '>'/'>='/'>>' regain their relational/shift meanings.
         * Save and zero template_depth across the inner parse. */
        int saved_depth = p->template_depth;
        p->template_depth = 0;
        Node *node = parse_expr(p);
        /* C++17 fold-expression — N4659 §8.1.6 [expr.prim.fold]
         *   ( cast-expression op ... )           — unary right fold
         *   ( ... op cast-expression )           — unary left fold (handled below)
         *   ( cast-expression op ... op cast-expression )  — binary fold
         *
         * After parse_expr returns, the binary parser stops at any
         * operator followed by '...'. Consume the trailing 'op...' or
         * 'op... op expr' pattern as opaque so the caller sees ')'. */
        for (;;) {
            TokenKind k = parser_peek(p)->kind;
            bool is_binop = (k == TK_PLUS || k == TK_MINUS || k == TK_STAR ||
                             k == TK_SLASH || k == TK_PERCENT || k == TK_AMP ||
                             k == TK_PIPE || k == TK_CARET || k == TK_LAND ||
                             k == TK_LOR || k == TK_SHL || k == TK_SHR ||
                             k == TK_LT || k == TK_LE || k == TK_GT || k == TK_GE ||
                             k == TK_EQ || k == TK_NE || k == TK_COMMA ||
                             k == TK_ASSIGN);
            if (!is_binop) break;
            if (parser_peek_ahead(p, 1)->kind != TK_ELLIPSIS) break;
            parser_advance(p);  /* op */
            parser_advance(p);  /* ... */
            /* Optional second operand: 'op expr' for a binary fold. */
            k = parser_peek(p)->kind;
            is_binop = (k == TK_PLUS || k == TK_MINUS || k == TK_STAR ||
                        k == TK_SLASH || k == TK_PERCENT || k == TK_AMP ||
                        k == TK_PIPE || k == TK_CARET || k == TK_LAND ||
                        k == TK_LOR || k == TK_SHL || k == TK_SHR ||
                        k == TK_LT || k == TK_LE || k == TK_GT || k == TK_GE ||
                        k == TK_EQ || k == TK_NE || k == TK_COMMA ||
                        k == TK_ASSIGN);
            if (is_binop && parser_peek_ahead(p, 1)->kind != TK_RPAREN) {
                parser_advance(p);
                parse_assign_expr(p);
            }
        }
        p->template_depth = saved_depth;
        parser_expect(p, TK_RPAREN);
        return node;
    }

    /* sizeof — N4659 §8.3.3 [expr.sizeof]
     *   sizeof unary-expression
     *   sizeof ( type-id )
     *   sizeof ... ( identifier )    — C++11 parameter pack (handled
     *                                   below: consumed but the pack
     *                                   identity is discarded) */
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

        /* Braced-init temporary after a template-id / type-name —
         * 'make_index_sequence<N>{}'. Treat as a postfix-style braced
         * init: balance the braces and treat as opaque init expression. */
        if (tok->kind == TK_LBRACE) {
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
            /* Optional 'template' disambiguator: x.template f<int>().
             * When present, the user is asserting that '<' after the
             * next name IS a template-arg-list — bypass the heuristic
             * lookahead in the IDENT branch below. */
            bool member_template_kw = parser_consume(p, TK_KW_TEMPLATE);
            /* Qualified member: x.A::B::method.
             * Only enter when followed by '::' (we'd otherwise eat the
             * '<' of a relational expression like 'x.first < y.first'). */
            while (parser_at(p, TK_IDENT) &&
                   parser_peek_ahead(p, 1)->kind == TK_SCOPE) {
                parser_advance(p);  /* segment */
                parser_advance(p);  /* :: */
                parser_consume(p, TK_KW_TEMPLATE);
            }
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
                /* Pseudo-destructor with template-id:
                 *   p->~Class<args>()
                 * Speculatively consume the '<...>' so the call
                 * suffix can pick up. The dtor name is opaque to
                 * us — we just need to skip the template args. */
                if (parser_at(p, TK_LT))
                    parse_template_id(p, member);
            } else {
                member = parser_expect(p, TK_IDENT);
                /* Member template-id: 'obj.method<T>(args)'.
                 * If the explicit 'template' disambiguator preceded
                 * the name, trust it and parse '<...>' unconditionally.
                 * Otherwise speculate based on the leading token. */
                if (parser_at(p, TK_LT)) {
                    bool looks_template = member_template_kw;
                    if (!looks_template) {
                        Token *after = parser_peek_ahead(p, 1);
                        switch (after->kind) {
                        case TK_KW_VOID: case TK_KW_BOOL: case TK_KW_CHAR:
                        case TK_KW_SHORT: case TK_KW_INT: case TK_KW_LONG:
                        case TK_KW_FLOAT: case TK_KW_DOUBLE:
                        case TK_KW_SIGNED: case TK_KW_UNSIGNED:
                        case TK_KW_WCHAR_T: case TK_KW_CHAR16_T: case TK_KW_CHAR32_T:
                        case TK_KW_CONST: case TK_KW_VOLATILE:
                        case TK_KW_TYPENAME: case TK_KW_DECLTYPE:
                        case TK_KW_AUTO:
                            looks_template = true; break;
                        case TK_IDENT:
                            if (lookup_is_type_name(p, after) ||
                                lookup_is_template_name(p, after))
                                looks_template = true;
                            break;
                        default: break;
                        }
                    }
                    if (looks_template)
                        parse_template_id(p, member);
                }
                /* Qualified continuation through a class-template-id
                 * member access:
                 *   this->Base<T>::operator=(...)
                 *   this->Inner::method()
                 * After the (possibly templated) member name, allow
                 * '::id' or '::operator <op>' segments. The resulting
                 * call is opaque to sema/codegen — we just need to
                 * walk the tokens so the call's argument list gets
                 * picked up by the postfix loop's '(' suffix. */
                while (parser_consume(p, TK_SCOPE)) {
                    parser_consume(p, TK_KW_TEMPLATE);
                    if (parser_consume(p, TK_KW_OPERATOR)) {
                        /* operator-function-id: consume the operator
                         * symbol(s). Special cases: () and []. */
                        if (parser_consume(p, TK_LPAREN))
                            parser_expect(p, TK_RPAREN);
                        else if (parser_consume(p, TK_LBRACKET))
                            parser_expect(p, TK_RBRACKET);
                        else if (parser_at(p, TK_KW_NEW) ||
                                 parser_at(p, TK_KW_DELETE)) {
                            parser_advance(p);
                            if (parser_consume(p, TK_LBRACKET))
                                parser_expect(p, TK_RBRACKET);
                        } else if (parser_peek(p)->kind >= TK_LPAREN &&
                                   parser_peek(p)->kind <= TK_HASHHASH) {
                            parser_advance(p);
                        }
                    } else if (parser_at(p, TK_IDENT)) {
                        member = parser_advance(p);
                        if (parser_at(p, TK_LT))
                            parse_template_id(p, member);
                    } else {
                        break;
                    }
                }
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
/*       noexcept-expression         — implemented in primary_expr    */
/*       new-expression              — implemented (§8.3.4)            */
/*       delete-expression           — implemented (§8.3.5)            */
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
            bool prev_tentative = p->tentative;
            p->tentative = true;
            parser_advance(p);  /* ( */
            parse_expr(p);
            bool ok = parser_at(p, TK_RPAREN);
            p->tentative = prev_tentative;
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

        /* Type being allocated. If it's an unknown bare identifier
         * (e.g. a class member type used before its point of declaration
         * in the same class body), accept it as an opaque type. */
        Type *ty = NULL;
        if (parser_peek(p)->kind == TK_IDENT &&
            !parser_at_type_specifier(p) &&
            parser_peek_ahead(p, 1)->kind != TK_LT &&
            parser_peek_ahead(p, 1)->kind != TK_SCOPE &&
            lookup_unqualified(p, parser_peek(p)->loc, parser_peek(p)->len) == NULL) {
            Token *name_tok = parser_advance(p);
            ty = new_type(p, TY_STRUCT);
            ty->tag = name_tok;
            /* Optional ptr/ref operators */
            while (parser_consume(p, TK_STAR) || parser_consume(p, TK_AMP) ||
                   parser_consume(p, TK_LAND))
                ;
            /* Optional array extents: new T[n] / new T[n][m] */
            while (parser_consume(p, TK_LBRACKET)) {
                if (!parser_at(p, TK_RBRACKET)) parse_assign_expr(p);
                parser_expect(p, TK_RBRACKET);
            }
        } else {
            ty = parse_type_name(p);
        }

        /* Optional initializer: (args) or braced-init-list {args}
         * — N4659 §8.3.4 [expr.new]/15. */
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
        } else if (parser_consume(p, TK_LBRACE)) {
            /* Braced new-initializer. Skip-and-discard via balanced
             * brace count — sema doesn't model the initializer
             * structure, just needs to walk past the tokens. */
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

        /* C++17 fold-expression boundary: 'op ...' inside a paren-
         * expression is a fold marker, not a binary op. Stop here so
         * the enclosing '(' handler can pick up the trailing 'op ...'
         * and produce a fold. */
        if (parser_peek_ahead(p, 1)->kind == TK_ELLIPSIS)
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
/*       throw-expression — parsed in primary_expr; lowered as an     */
/*                          opaque NULLPTR placeholder until           */
/*                          exceptions land (no try/catch yet)         */
/*       // C++20: yield-expression (co_yield — NOT YET)               */
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
