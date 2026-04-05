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
    /* PREC_MPTR  = 15,      §8.5  [expr.mptr.oper]   .* ->*  (deferred) */
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
    Token *tok = peek(p);

    /* Integer literal — §5.13.2 [lex.icon] */
    if (tok->kind == TK_NUM) {
        advance(p);
        return new_num_node(p, tok);
    }

    /* Floating literal — §5.13.4 [lex.fcon] */
    if (tok->kind == TK_FNUM) {
        advance(p);
        return new_fnum_node(p, tok);
    }

    /* String literal — §5.13.5 [lex.string]
     * Adjacent string literals are concatenated in translation phase 6
     * (§5.13.5/13). We consume all consecutive string tokens here.
     * The semantic layer handles actual concatenation and encoding. */
    if (tok->kind == TK_STR) {
        advance(p);
        /* Skip adjacent string literals — they form one logical string */
        while (at(p, TK_STR))
            advance(p);
        Node *node = new_node(p, ND_STR, tok);
        node->str.tok = tok;
        return node;
    }

    /* Character literal — §5.13.3 [lex.ccon] */
    if (tok->kind == TK_CHAR) {
        advance(p);
        Node *node = new_node(p, ND_CHAR, tok);
        node->chr.tok = tok;
        return node;
    }

    /* Boolean literals — N4659 §5.13.6 [lex.bool]
     * Distinct from integer 0/1 — sema needs to know the type is bool.
     * The token (TK_KW_TRUE vs TK_KW_FALSE) carries the value. */
    if (tok->kind == TK_KW_TRUE || tok->kind == TK_KW_FALSE) {
        advance(p);
        return new_node(p, ND_BOOL_LIT, tok);
    }

    /* nullptr — N4659 §5.13.7 [lex.nullptr]
     * Type is std::nullptr_t (§21.2.4 [support.nullptr]),
     * distinct from integer 0. Sema handles the type. */
    if (tok->kind == TK_KW_NULLPTR) {
        advance(p);
        return new_node(p, ND_NULLPTR, tok);
    }

    /* Identifier — §8.1.4 [expr.prim.id]
     *   id-expression: unqualified-id | qualified-id
     *
     * N4659 §17.2/3 [temp.names] — Rule 4 disambiguation:
     *   If the identifier is a template-name and is followed by '<',
     *   parse as a template-id (simple-template-id) rather than treating
     *   '<' as less-than.
     *   "After name lookup finds that a name is a template-name, if this
     *    name is followed by a <, the < is always taken as the delimiter
     *    of a template-argument-list and never as the less-than operator."
     */
    if (tok->kind == TK_IDENT) {
        advance(p);

        /* Rule 4: template-name followed by < → template-id */
        if (at(p, TK_LT) && lookup_is_template_name(p, tok))
            return parse_template_id(p, tok);

        Node *node = new_node(p, ND_IDENT, tok);
        node->ident.name = tok;
        return node;
    }

    /* Parenthesized expression — §8.1.5 [expr.prim.paren]
     *   ( expression )
     *
     * Note: this is also where C-style casts (type)expr would be
     * disambiguated. For the first pass, if the token after '(' is
     * a type keyword, we parse a cast. Otherwise, parenthesized expr.
     * Full disambiguation (Rule 1/2) requires the type-name oracle. */
    if (tok->kind == TK_LPAREN) {
        advance(p);

        /* Try C-style cast: (type)expr — §8.4 [expr.cast]
         * cast-expression: unary-expression | ( type-id ) cast-expression
         * First-pass approximation: check for built-in type keyword. */
        if (at_type_specifier(p)) {
            Type *ty = parse_type_name(p);
            expect(p, TK_RPAREN);
            Node *operand = unary_expr(p);
            Node *node = new_node(p, ND_CAST, tok);
            node->cast.ty = ty;
            node->cast.operand = operand;
            return node;
        }

        Node *node = parse_expr(p);
        expect(p, TK_RPAREN);
        return node;
    }

    /* sizeof — N4659 §8.3.3 [expr.sizeof]
     *   sizeof unary-expression
     *   sizeof ( type-id )
     *   sizeof ... ( identifier )  (C++11 parameter pack — deferred) */
    if (tok->kind == TK_KW_SIZEOF) {
        advance(p);
        Node *node = new_node(p, ND_SIZEOF, tok);

        if (consume(p, TK_LPAREN)) {
            if (at_type_specifier(p)) {
                node->sizeof_.ty = parse_type_name(p);
                node->sizeof_.is_type = true;
            } else {
                node->sizeof_.expr = parse_expr(p);
                node->sizeof_.is_type = false;
            }
            expect(p, TK_RPAREN);
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
        advance(p);
        Node *node = new_node(p, ND_ALIGNOF, tok);
        expect(p, TK_LPAREN);
        node->alignof_.ty = parse_type_name(p);
        expect(p, TK_RPAREN);
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
/*       // Also: dynamic_cast, static_cast, etc. (deferred)           */
/*       // C++20: no changes to postfix grammar                       */
/*       // C++23: adds multidimensional subscript a[i,j]              */
/* ------------------------------------------------------------------ */

static Node *postfix_expr(Parser *p) {
    Node *node = primary_expr(p);
    if (!node) return NULL;

    for (;;) {
        Token *tok = peek(p);

        /* Function call — §8.2.2 [expr.call]
         *   postfix-expression ( expression-list(opt) ) */
        if (tok->kind == TK_LPAREN) {
            advance(p);
            Vec args = vec_new(p->arena);
            if (!at(p, TK_RPAREN)) {
                vec_push(&args, parse_assign_expr(p));
                while (consume(p, TK_COMMA))
                    vec_push(&args, parse_assign_expr(p));
            }
            expect(p, TK_RPAREN);

            Node *call = new_node(p, ND_CALL, tok);
            call->call.callee = node;
            call->call.args = (Node **)args.data;
            call->call.nargs = args.len;
            node = call;
            continue;
        }

        /* Subscript — §8.2.1 [expr.sub]
         *   postfix-expression [ expression ]
         * C++23: a[i, j] multidimensional — deferred */
        if (tok->kind == TK_LBRACKET) {
            advance(p);
            Node *index = parse_expr(p);
            expect(p, TK_RBRACKET);

            Node *sub = new_node(p, ND_SUBSCRIPT, tok);
            sub->subscript.base = node;
            sub->subscript.index = index;
            node = sub;
            continue;
        }

        /* Member access — §8.2.5 [expr.ref]
         *   postfix-expression . id-expression
         *   postfix-expression -> id-expression */
        if (tok->kind == TK_DOT || tok->kind == TK_ARROW) {
            TokenKind op = tok->kind;
            advance(p);
            Token *member = expect(p, TK_IDENT);

            Node *mem = new_node(p, ND_MEMBER, tok);
            mem->member.obj = node;
            mem->member.member = member;
            mem->member.op = op;
            node = mem;
            continue;
        }

        /* Post-increment/decrement — §8.2.6 [expr.post.incr] */
        if (tok->kind == TK_INC || tok->kind == TK_DEC) {
            TokenKind op = tok->kind;
            advance(p);
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
    Token *tok = peek(p);

    /* Pre-increment/decrement — §8.3.1 [expr.pre.incr] */
    if (tok->kind == TK_INC || tok->kind == TK_DEC) {
        advance(p);
        Node *operand = unary_expr(p);
        return new_unary_node(p, tok->kind, operand, tok);
    }

    /* Unary operators — §8.3.1 [expr.unary.op]
     *   * (indirection), & (address-of), + - (arithmetic), ! ~ (logical/bitwise NOT) */
    if (tok->kind == TK_STAR || tok->kind == TK_AMP ||
        tok->kind == TK_PLUS || tok->kind == TK_MINUS ||
        tok->kind == TK_EXCL || tok->kind == TK_TILDE) {
        advance(p);
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

    for (;;) {
        Token *op_tok = peek(p);
        int prec = get_binop_prec(p, op_tok->kind);
        if (prec == 0 || prec < min_prec)
            break;

        TokenKind op = op_tok->kind;
        advance(p);

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

    if (!consume(p, TK_QUESTION))
        return cond;

    Token *tok = &p->tokens[p->pos > 0 ? p->pos - 1 : 0];

    /* §8.16/1: "expression" in then-branch (comma is allowed) */
    Node *then_ = parse_expr(p);
    expect(p, TK_COLON);
    /* §8.16/1: "assignment-expression" in else-branch */
    Node *else_ = parse_assign_expr(p);

    Node *node = new_node(p, ND_TERNARY, tok);
    node->ternary.cond = cond;
    node->ternary.then_ = then_;
    node->ternary.else_ = else_;
    return node;
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

    Token *tok = peek(p);
    if (is_assign_op(tok->kind)) {
        TokenKind op = tok->kind;
        advance(p);
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

    while (consume(p, TK_COMMA)) {
        Token *tok = &p->tokens[p->pos > 0 ? p->pos - 1 : 0];
        Node *rhs = parse_assign_expr(p);
        Node *node = new_node(p, ND_COMMA, tok);
        node->comma.lhs = lhs;
        node->comma.rhs = rhs;
        lhs = node;
    }

    return lhs;
}
