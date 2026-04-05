/*
 * stmt.c — Statement parser.
 *
 * N4659 §9 [stmt.stmt] — Statements
 *
 *   statement:
 *       labeled-statement               (§9.1)
 *       attribute-specifier-seq(opt) expression-statement    (§9.2)
 *       attribute-specifier-seq(opt) compound-statement      (§9.3)
 *       attribute-specifier-seq(opt) selection-statement     (§9.4)
 *       attribute-specifier-seq(opt) iteration-statement     (§9.5)
 *       attribute-specifier-seq(opt) jump-statement          (§9.6)
 *       declaration-statement           (§9.7)
 *       attribute-specifier-seq(opt) try-block (deferred — no exceptions)
 *
 * C++17 changes: init-statement in if and switch (§9.4.1, §9.4.2)
 * C++20 changes: co_return-statement (§9.6.3.1 — deferred)
 * C++23 changes: if consteval (N4950 §9.4.2 — deferred)
 *
 * Attributes (§10.6) are deferred to a later stage.
 */

#include "parse.h"

/*
 * compound-statement — N4659 §9.3 [stmt.block]
 *   { statement-seq(opt) }
 *
 * Unchanged in C++20/23.
 */
Node *parse_compound_stmt(Parser *p) {
    Token *tok = expect(p, TK_LBRACE);

    /* N4659 §6.3.3/1 [basic.scope.block]: "A name declared in a block
     * (9.3) is local to that block." */
    region_push(p, REGION_BLOCK);

    Vec stmts = vec_new(p->arena);
    while (!at(p, TK_RBRACE) && !at_eof(p)) {
        Node *s = parse_stmt(p);
        if (s)
            vec_push(&stmts, s);
    }
    expect(p, TK_RBRACE);

    region_pop(p);

    Node *node = new_node(p, ND_BLOCK, tok);
    node->block.stmts = (Node **)stmts.data;
    node->block.nstmts = stmts.len;
    return node;
}

/*
 * if-statement — N4659 §9.4.1 [stmt.if]
 *
 *   if constexpr(opt) ( init-statement(opt) condition ) statement
 *   if constexpr(opt) ( init-statement(opt) condition ) statement else statement
 *
 * C++17: adds init-statement and constexpr form.
 *   init-statement: expression-statement | simple-declaration
 * C++23 (N4950 §9.4.2): adds 'if consteval' (not yet implemented).
 */
static Node *parse_if_stmt(Parser *p) {
    Token *tok = expect(p, TK_KW_IF);
    Node *node = new_node(p, ND_IF, tok);

    /* C++17: if constexpr — N4659 §9.4.1/2 */
    if (consume(p, TK_KW_CONSTEXPR))
        node->if_.is_constexpr = true;

    expect(p, TK_LPAREN);

    /* TODO: C++17 init-statement (deferred — needs stmt-vs-decl disambig
     * within the if-condition context) */
    node->if_.cond = parse_expr(p);

    expect(p, TK_RPAREN);

    node->if_.then_ = parse_stmt(p);

    /* else clause — §9.4.1/1 */
    if (consume(p, TK_KW_ELSE))
        node->if_.else_ = parse_stmt(p);

    return node;
}

/*
 * while-statement — N4659 §9.5.1 [stmt.while]
 *   while ( condition ) statement
 *
 * Unchanged in C++20/23.
 */
static Node *parse_while_stmt(Parser *p) {
    Token *tok = expect(p, TK_KW_WHILE);
    expect(p, TK_LPAREN);
    Node *cond = parse_expr(p);
    expect(p, TK_RPAREN);
    Node *body = parse_stmt(p);

    Node *node = new_node(p, ND_WHILE, tok);
    node->while_.cond = cond;
    node->while_.body = body;
    return node;
}

/*
 * do-while-statement — N4659 §9.5.2 [stmt.do]
 *   do statement while ( expression ) ;
 *
 * Unchanged in C++20/23.
 */
static Node *parse_do_stmt(Parser *p) {
    Token *tok = expect(p, TK_KW_DO);
    Node *body = parse_stmt(p);
    expect(p, TK_KW_WHILE);
    expect(p, TK_LPAREN);
    Node *cond = parse_expr(p);
    expect(p, TK_RPAREN);
    expect(p, TK_SEMI);

    Node *node = new_node(p, ND_DO, tok);
    node->do_.cond = cond;
    node->do_.body = body;
    return node;
}

/*
 * for-statement — N4659 §9.5.3 [stmt.for]
 *   for ( init-statement condition(opt) ; expression(opt) ) statement
 *
 * init-statement can be an expression-statement or a simple-declaration.
 * We use the type-specifier heuristic for stmt-vs-decl disambiguation.
 *
 * C++11 (§9.5.4 [stmt.ranged]): range-based for — deferred.
 * C++20/23: no changes to traditional for syntax.
 */
static Node *parse_for_stmt(Parser *p) {
    Token *tok = expect(p, TK_KW_FOR);
    expect(p, TK_LPAREN);

    /* N4659 §6.3.3/4 [basic.scope.block]: "Names declared in the
     * init-statement ... are local to the ... for statement." */
    region_push(p, REGION_BLOCK);

    /* init-statement: declaration or expression-statement */
    Node *init = NULL;
    if (!at(p, TK_SEMI)) {
        if (at_type_specifier(p))
            init = parse_declaration(p);  /* includes trailing ; */
        else {
            init = new_node(p, ND_EXPR_STMT, peek(p));
            init->expr_stmt.expr = parse_expr(p);
            expect(p, TK_SEMI);
        }
    } else {
        expect(p, TK_SEMI);
    }

    /* condition (optional) */
    Node *cond = NULL;
    if (!at(p, TK_SEMI))
        cond = parse_expr(p);
    expect(p, TK_SEMI);

    /* increment (optional) */
    Node *inc = NULL;
    if (!at(p, TK_RPAREN))
        inc = parse_expr(p);
    expect(p, TK_RPAREN);

    Node *body = parse_stmt(p);

    region_pop(p);

    Node *node = new_node(p, ND_FOR, tok);
    node->for_.init = init;
    node->for_.cond = cond;
    node->for_.inc = inc;
    node->for_.body = body;
    return node;
}

/*
 * switch-statement — N4659 §9.4.2 [stmt.switch]
 *   switch ( init-statement(opt) condition ) statement
 *
 * C++17: adds init-statement (same as if-statement).
 * C++20/23: unchanged.
 */
static Node *parse_switch_stmt(Parser *p) {
    Token *tok = expect(p, TK_KW_SWITCH);
    expect(p, TK_LPAREN);
    Node *expr = parse_expr(p);
    expect(p, TK_RPAREN);
    Node *body = parse_stmt(p);

    Node *node = new_node(p, ND_SWITCH, tok);
    node->switch_.expr = expr;
    node->switch_.body = body;
    return node;
}

/*
 * case-label — N4659 §9.1 [stmt.label]
 *   case constant-expression : statement
 *
 * Unchanged in C++20/23.
 */
static Node *parse_case_stmt(Parser *p) {
    Token *tok = expect(p, TK_KW_CASE);
    Node *expr = parse_assign_expr(p);  /* constant-expression */
    expect(p, TK_COLON);
    Node *stmt = parse_stmt(p);

    Node *node = new_node(p, ND_CASE, tok);
    node->case_.expr = expr;
    node->case_.stmt = stmt;
    return node;
}

/*
 * default-label — N4659 §9.1 [stmt.label]
 *   default : statement
 */
static Node *parse_default_stmt(Parser *p) {
    Token *tok = expect(p, TK_KW_DEFAULT);
    expect(p, TK_COLON);
    Node *stmt = parse_stmt(p);

    Node *node = new_node(p, ND_DEFAULT, tok);
    node->default_.stmt = stmt;
    return node;
}

/*
 * return-statement — N4659 §9.6.3 [stmt.return]
 *   return expression(opt) ;
 *   return braced-init-list ;   (deferred — initializer lists)
 *
 * C++20: adds co_return (§9.6.3.1 — deferred, no coroutines).
 */
static Node *parse_return_stmt(Parser *p) {
    Token *tok = expect(p, TK_KW_RETURN);
    Node *node = new_node(p, ND_RETURN, tok);

    if (!at(p, TK_SEMI))
        node->ret.expr = parse_expr(p);

    expect(p, TK_SEMI);
    return node;
}

/*
 * statement — N4659 §9 [stmt.stmt]
 *
 * Dispatches on the current token to the appropriate sub-parser.
 * The stmt-vs-decl disambiguation (§9.8 [stmt.ambig]) is handled
 * here: if the token starts a type-specifier, it's a declaration.
 */
Node *parse_stmt(Parser *p) {
    Token *tok = peek(p);

    /* compound-statement — §9.3 */
    if (tok->kind == TK_LBRACE)
        return parse_compound_stmt(p);

    /* selection-statements — §9.4 */
    if (tok->kind == TK_KW_IF)      return parse_if_stmt(p);
    if (tok->kind == TK_KW_SWITCH)  return parse_switch_stmt(p);

    /* iteration-statements — §9.5 */
    if (tok->kind == TK_KW_WHILE)   return parse_while_stmt(p);
    if (tok->kind == TK_KW_DO)      return parse_do_stmt(p);
    if (tok->kind == TK_KW_FOR)     return parse_for_stmt(p);

    /* jump-statements — §9.6 */
    if (tok->kind == TK_KW_RETURN)  return parse_return_stmt(p);

    if (tok->kind == TK_KW_BREAK) {
        advance(p);
        expect(p, TK_SEMI);
        return new_node(p, ND_BREAK, tok);
    }
    if (tok->kind == TK_KW_CONTINUE) {
        advance(p);
        expect(p, TK_SEMI);
        return new_node(p, ND_CONTINUE, tok);
    }
    if (tok->kind == TK_KW_GOTO) {
        advance(p);
        Node *node = new_node(p, ND_GOTO, tok);
        node->goto_.label = expect(p, TK_IDENT);
        expect(p, TK_SEMI);
        return node;
    }

    /* case / default labels — §9.1 */
    if (tok->kind == TK_KW_CASE)    return parse_case_stmt(p);
    if (tok->kind == TK_KW_DEFAULT) return parse_default_stmt(p);

    /* labeled-statement — §9.1 [stmt.label]
     *   identifier : statement
     * Check for ident followed by colon (not ::). */
    if (tok->kind == TK_IDENT && peek_ahead(p, 1)->kind == TK_COLON) {
        Token *label = advance(p);  /* consume ident */
        advance(p);                 /* consume : */
        Node *node = new_node(p, ND_LABEL, tok);
        node->label.label = label;
        node->label.stmt = parse_stmt(p);
        return node;
    }

    /* empty statement — §9.2 */
    if (tok->kind == TK_SEMI) {
        advance(p);
        return new_node(p, ND_NULL_STMT, tok);
    }

    /* declaration-statement — §9.7 [stmt.dcl]
     *   N4659 §9.8 [stmt.ambig]: "any statement that could be a
     *   declaration IS a declaration."
     *
     * First-pass heuristic: if current token is a type-specifier keyword,
     * parse as declaration. User-defined type names need symbol table. */
    if (at_type_specifier(p))
        return parse_declaration(p);

    /* expression-statement — §9.2 [stmt.expr]
     *   expression(opt) ; */
    Node *node = new_node(p, ND_EXPR_STMT, tok);
    node->expr_stmt.expr = parse_expr(p);
    expect(p, TK_SEMI);
    return node;
}
