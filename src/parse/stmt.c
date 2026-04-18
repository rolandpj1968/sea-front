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
 *       try-block                        (§9.7 — NOT YET, no exceptions)
 *
 * Attributes (§10.6 [dcl.attr]) are PARSED but DISCARDED — we call
 * parser_skip_cxx_attributes / parser_skip_gnu_attributes at the
 * sites where they're allowed (e.g. before substatements after
 * 'if (cond)') so they don't trip the parser, but we don't build
 * AST nodes for them.
 *
 * C++17 changes (implemented): init-statement in if and switch
 *   (§9.4.1, §9.4.2).
 * C++17 changes (still NOT YET): structured bindings in
 *   declarations.
 * C++20 changes (NOT YET): co_return-statement (§9.6.3.1).
 * C++23 changes (NOT YET): if consteval (N4950 §9.4.2).
 */

#include "parse.h"

/*
 * compound-statement — N4659 §9.3 [stmt.block]
 *   { statement-seq(opt) }
 *
 * C++20: unchanged.
 * C++23: allows label-seq at end of compound-statement (labels
 *   before '}' without a following statement). We already accept
 *   this — our loop parses 'case:' / 'default:' labels before
 *   the next statement, and a '}' simply ends the block.
 */
Node *parse_compound_stmt(Parser *p) {
    Token *tok = parser_expect(p, TK_LBRACE);

    /* N4659 §6.3.3/1 [basic.scope.block]: "A name declared in a block
     * (9.3) is local to that block." */
    region_push(p, REGION_BLOCK, /*name=*/NULL);
    DeclarativeRegion *block_region = p->region;

    Vec stmts = vec_new(p->arena);
    while (!parser_at(p, TK_RBRACE) && !parser_at_eof(p)) {
        /* Skip preprocessor leftovers (#line directives etc.) — mcpp
         * emits these on their own lines. */
        if (parser_at(p, TK_HASH)) {
            int line = parser_peek(p)->line;
            while (!parser_at_eof(p) && parser_peek(p)->line == line)
                parser_advance(p);
            continue;
        }
        Node *s = parse_stmt(p);
        if (s)
            vec_push(&stmts, s);
    }
    parser_expect(p, TK_RBRACE);

    region_pop(p);

    Node *node = new_block_node(p, (Node **)stmts.data, stmts.len, tok);
    node->block.scope = block_region;
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
    Token *tok = parser_expect(p, TK_KW_IF);
    Node *node = new_node(p, ND_IF, tok);

    /* C++17: if constexpr — N4659 §9.4.1/2 */
    if (parser_consume(p, TK_KW_CONSTEXPR))
        node->if_.is_constexpr = true;

    parser_expect(p, TK_LPAREN);

    /* N4659 §9.4.1 [stmt.select]: a declaration in an if-condition is
     * scoped to the if-statement. Push a block region so the name
     * doesn't leak into the enclosing function. */
    region_push(p, REGION_BLOCK, /*name=*/NULL);

    /* Declaration in condition — N4659 §9.4.1 [stmt.select]
     *   condition: expression
     *            | attribute-specifier-seq(opt) decl-specifier-seq declarator
     *              brace-or-equal-initializer
     * Used as e.g. 'if (T x = expr)'. We tentatively try a declaration; if
     * it doesn't end at ')', restore and parse as expression. */
    /* Same IDENT-IDENT shortcut as parse_stmt — see the long
     * comment block there for the standard rule, our deviation,
     * and the TODO(seafront#stmt-ambig) tracking. */
    bool if_decl_ident = false;
    if (parser_peek(p)->kind == TK_IDENT && !parser_at_type_specifier(p)) {
        Token *t1 = parser_peek_ahead(p, 1);
        if (t1->kind == TK_IDENT &&
            !lookup_unqualified(p, t1->loc, t1->len))
            if_decl_ident = true;
    }
    if (parser_at_type_specifier(p) || if_decl_ident) {
        ParseState saved = parser_save(p);
        bool prev_tentative = p->tentative;
        p->tentative = true;
        bool saved_failed = p->tentative_failed;
        p->tentative_failed = false;
        Type *base = parse_type_specifiers(p).type;
        Node *decl = base ? parse_declarator(p, base) : NULL;
        if (decl && parser_consume(p, TK_ASSIGN))
            decl->var_decl.init = parse_assign_expr(p);
        else if (decl && parser_at(p, TK_LBRACE)) {
            /* Braced-init: 'if (T x{...})'. Skip the brace-balanced
             * init expression. */
            int depth = 0;
            while (!parser_at_eof(p)) {
                if (parser_at(p, TK_LBRACE)) depth++;
                if (parser_at(p, TK_RBRACE)) {
                    depth--;
                    if (depth <= 0) { parser_advance(p); break; }
                }
                parser_advance(p);
            }
        }
        /* Valid if-condition declaration needs a name — N4659 §9.4.1
         * [stmt.select]/2 the condition form is 'decl-specifier-seq
         * declarator brace-or-equal-initializer'. An abstract
         * declarator with no name is not a condition. This matters
         * for 'if (answer)' when 'answer' is BOTH a struct tag and
         * a variable in scope — without the name check the tentative
         * parse accepts 'struct answer' and emits an empty decl. */
        bool ok = decl && decl->var_decl.name &&
                  parser_at(p, TK_RPAREN) && !p->tentative_failed;
        p->tentative = prev_tentative;
        p->tentative_failed = saved_failed;
        parser_restore(p, saved);
        if (ok) {
            base = parse_type_specifiers(p).type;
            decl = parse_declarator(p, base);
            if (parser_consume(p, TK_ASSIGN))
                decl->var_decl.init = parse_assign_expr(p);
            else if (parser_at(p, TK_LBRACE)) {
                int depth = 0;
                while (!parser_at_eof(p)) {
                    if (parser_at(p, TK_LBRACE)) depth++;
                    if (parser_at(p, TK_RBRACE)) {
                        depth--;
                        if (depth <= 0) { parser_advance(p); break; }
                    }
                    parser_advance(p);
                }
            }
            if (decl->var_decl.name)
                region_declare(p, decl->var_decl.name->loc,
                              decl->var_decl.name->len, ENTITY_VARIABLE,
                              decl->var_decl.ty);
            node->if_.cond = decl;
        } else {
            node->if_.cond = parse_expr(p);
        }
    } else {
        node->if_.cond = parse_expr(p);
    }

    parser_expect(p, TK_RPAREN);

    /* C++20: attribute-specifier-seq before the substatement.
     * 'if (cond) [[likely]] { ... }' is allowed by the grammar
     * (N4861 §9.4.1) — and libstdc++ uses [[__unlikely__]] in
     * a few places. Skip the attributes. */
    parser_skip_cxx_attributes(p);
    parser_skip_gnu_attributes(p);

    node->if_.then_ = parse_stmt(p);

    /* else clause — §9.4.1/1 — same attribute treatment. */
    if (parser_consume(p, TK_KW_ELSE)) {
        parser_skip_cxx_attributes(p);
        parser_skip_gnu_attributes(p);
        node->if_.else_ = parse_stmt(p);
    }

    region_pop(p);  /* pop if-statement scope */
    return node;
}

/*
 * while-statement — N4659 §9.5.1 [stmt.while]
 *   while ( condition ) statement
 *
 * Unchanged in C++20/23.
 */
static Node *parse_while_stmt(Parser *p) {
    Token *tok = parser_expect(p, TK_KW_WHILE);
    parser_expect(p, TK_LPAREN);
    Node *cond = parse_expr(p);
    parser_expect(p, TK_RPAREN);
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
    Token *tok = parser_expect(p, TK_KW_DO);
    Node *body = parse_stmt(p);
    parser_expect(p, TK_KW_WHILE);
    parser_expect(p, TK_LPAREN);
    Node *cond = parse_expr(p);
    parser_expect(p, TK_RPAREN);
    parser_expect(p, TK_SEMI);

    Node *node = new_node(p, ND_DO, tok);
    node->do_.cond = cond;
    node->do_.body = body;
    return node;
}

/*
 * for-statement — N4659 §9.5.3 [stmt.for] / §9.5.4 [stmt.ranged]
 *   for ( init-statement condition(opt) ; expression(opt) ) statement
 *   for ( for-range-declaration : for-range-initializer ) statement
 *
 * Both forms are handled. We tentatively detect the range-based
 * form by parsing a declarator and checking for ':' before falling
 * through to the traditional form. The traditional form's
 * init-statement can be an expression-statement or a simple-
 * declaration; stmt-vs-decl disambiguation uses the same approach
 * as parse_stmt below.
 *
 * C++20/23: no changes to either for syntax.
 */
static Node *parse_for_stmt(Parser *p) {
    Token *tok = parser_expect(p, TK_KW_FOR);
    parser_expect(p, TK_LPAREN);

    /* N4659 §6.3.3/4 [basic.scope.block]: "Names declared in the
     * init-statement ... are local to the ... for statement." */
    region_push(p, REGION_BLOCK, /*name=*/NULL);

    /* C++11 range-based for: 'for (decl : expr) stmt'.
     * We don't structure it — just consume the decl-spec/declarator
     * up to ':' then the range expression up to ')'. */
    {
        ParseState saved_pos = parser_save(p);
        bool prev_t = p->tentative;
        bool saved_failed = p->tentative_failed;
        p->tentative = true;
        p->tentative_failed = false;
        /* Same IDENT-IDENT shortcut as parse_stmt (see the long
         * comment block there). Used here to recognise a range-based
         * for declaration whose loop variable's type is an unknown
         * IDENT — typically a template parameter visible only via
         * the enclosing template-decl that we don't model in lookup. */
        bool ident_decl_rf = false;
        if (parser_peek(p)->kind == TK_IDENT && !parser_at_type_specifier(p)) {
            Token *t1 = parser_peek_ahead(p, 1);
            if (t1->kind == TK_IDENT &&
                !lookup_unqualified(p, t1->loc, t1->len))
                ident_decl_rf = true;
        }
        if (parser_at_type_specifier(p)) {
            Type *bt = parse_type_specifiers(p).type;
            if (bt) parse_declarator(p, bt);
        } else if (ident_decl_rf) {
            /* Skip past the unknown type-name and declarator-id. */
            parser_advance(p);
            while (parser_consume(p, TK_STAR) || parser_consume(p, TK_AMP) ||
                   parser_consume(p, TK_LAND) || parser_consume(p, TK_KW_CONST))
                ;
            if (parser_at(p, TK_IDENT)) parser_advance(p);
        }
        bool is_range = !p->tentative_failed && parser_at(p, TK_COLON);
        p->tentative = prev_t;
        p->tentative_failed = saved_failed;
        parser_restore(p, saved_pos);
        if (is_range) {
            Type *bt = NULL;
            Node *decl = NULL;
            if (parser_at_type_specifier(p)) {
                bt = parse_type_specifiers(p).type;
                if (bt) decl = parse_declarator(p, bt);
            } else {
                /* Unknown type-name (template parameter we don't see in
                 * lookup). Skip past it; this is only a parser walkthrough. */
                parser_advance(p);
                while (parser_consume(p, TK_STAR) || parser_consume(p, TK_AMP) ||
                       parser_consume(p, TK_LAND) || parser_consume(p, TK_KW_CONST))
                    ;
                if (parser_at(p, TK_IDENT)) parser_advance(p);
            }
            parser_expect(p, TK_COLON);
            Node *range = parse_expr(p);
            parser_expect(p, TK_RPAREN);
            Node *body = parse_stmt(p);
            region_pop(p);
            (void)decl; (void)range;
            return new_for_node(p, /*init=*/NULL, /*cond=*/NULL,
                                /*inc=*/NULL, body, tok);
        }
    }

    /* init-statement: declaration or expression-statement */
    Node *init = NULL;
    if (!parser_at(p, TK_SEMI)) {
        /* Same IDENT-IDENT shortcut as parse_stmt (see the long
         * comment block there) — used in the for-loop's traditional
         * init-statement position. */
        bool ident_decl = false;
        if (parser_peek(p)->kind == TK_IDENT && !parser_at_type_specifier(p)) {
            Token *t1 = parser_peek_ahead(p, 1);
            if (t1->kind == TK_IDENT &&
                !lookup_unqualified(p, t1->loc, t1->len))
                ident_decl = true;
        }
        if (parser_at_type_specifier(p) || ident_decl)
            init = parse_declaration(p);  /* includes trailing ; */
        else {
            init = new_node(p, ND_EXPR_STMT, parser_peek(p));
            init->expr_stmt.expr = parse_expr(p);
            parser_expect(p, TK_SEMI);
        }
    } else {
        parser_expect(p, TK_SEMI);
    }

    /* condition (optional) */
    Node *cond = NULL;
    if (!parser_at(p, TK_SEMI))
        cond = parse_expr(p);
    parser_expect(p, TK_SEMI);

    /* increment (optional) */
    Node *inc = NULL;
    if (!parser_at(p, TK_RPAREN))
        inc = parse_expr(p);
    parser_expect(p, TK_RPAREN);

    Node *body = parse_stmt(p);

    region_pop(p);

    return new_for_node(p, init, cond, inc, body, tok);
}

/*
 * switch-statement — N4659 §9.4.2 [stmt.switch]
 *   switch ( init-statement(opt) condition ) statement
 *
 * C++17: adds init-statement (same as if-statement).
 * C++20/23: unchanged.
 */
static Node *parse_switch_stmt(Parser *p) {
    Token *tok = parser_expect(p, TK_KW_SWITCH);
    parser_expect(p, TK_LPAREN);
    Node *expr = parse_expr(p);
    parser_expect(p, TK_RPAREN);
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
 * C++20: unchanged.
 * C++23: labeled-statement refactored — labels are separated from
 *   their statement (label-seq + statement). The grammar change
 *   allows labels at end-of-block without a following statement.
 *   Our implementation is unaffected: we parse the label then
 *   always parse a statement (which may be empty/null).
 */
static Node *parse_case_stmt(Parser *p) {
    Token *tok = parser_expect(p, TK_KW_CASE);
    Node *expr = parse_assign_expr(p);  /* constant-expression */
    parser_expect(p, TK_COLON);
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
    Token *tok = parser_expect(p, TK_KW_DEFAULT);
    parser_expect(p, TK_COLON);
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
    Token *tok = parser_expect(p, TK_KW_RETURN);
    Node *node = new_node(p, ND_RETURN, tok);

    if (!parser_at(p, TK_SEMI))
        node->ret.expr = parse_expr(p);

    parser_expect(p, TK_SEMI);
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
    Token *tok = parser_peek(p);

    switch (tok->kind) {
    case TK_LBRACE:         return parse_compound_stmt(p);  /* §9.3 */

    /* selection-statements — §9.4 */
    case TK_KW_IF:          return parse_if_stmt(p);
    case TK_KW_SWITCH:      return parse_switch_stmt(p);

    /* iteration-statements — §9.5 */
    case TK_KW_WHILE:       return parse_while_stmt(p);
    case TK_KW_DO:          return parse_do_stmt(p);
    case TK_KW_FOR:         return parse_for_stmt(p);

    /* jump-statements — §9.6 */
    case TK_KW_RETURN:      return parse_return_stmt(p);

    case TK_KW_BREAK:
        parser_advance(p);
        parser_expect(p, TK_SEMI);
        return new_node(p, ND_BREAK, tok);

    case TK_KW_CONTINUE:
        parser_advance(p);
        parser_expect(p, TK_SEMI);
        return new_node(p, ND_CONTINUE, tok);

    case TK_KW_GOTO: {
        parser_advance(p);
        Node *node = new_node(p, ND_GOTO, tok);
        node->goto_.label = parser_expect(p, TK_IDENT);
        parser_expect(p, TK_SEMI);
        return node;
    }

    /* case / default labels — §9.1 */
    case TK_KW_CASE:        return parse_case_stmt(p);
    case TK_KW_DEFAULT:     return parse_default_stmt(p);

    /* empty statement — §9.2 */
    case TK_SEMI:
        parser_advance(p);
        return new_node(p, ND_NULL_STMT, tok);

    /* asm-definition — N4659 §10.4 [dcl.asm]
     *   asm ( string-literal ) ;
     * GCC extended-asm additionally allows
     *   asm ( string-literal : outputs : inputs : clobbers : labels ) ;
     * We don't lower inline assembly; swallow the balanced parens
     * and emit a null statement. Lowering correctly would require
     * codegen support for asm which is out of scope. */
    case TK_KW_ASM: {
        parser_advance(p);
        /* Optional asm-qualifiers: volatile / goto / inline. */
        while (parser_at(p, TK_KW_VOLATILE) ||
               parser_at(p, TK_KW_INLINE) ||
               parser_at(p, TK_KW_GOTO))
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
        parser_consume(p, TK_SEMI);
        return new_node(p, ND_NULL_STMT, tok);
    }

    /* using declaration/directive/alias inside blocks
     * (§10.3.3 [namespace.udecl], §10.3.4 [namespace.udir])
     * static_assert can also appear inside blocks. */
    case TK_KW_USING:
    case TK_KW_STATIC_ASSERT:
        return parse_top_level_decl(p);

    default:
        break;
    }

    /* labeled-statement — §9.1 [stmt.label]
     *   identifier : statement
     * Check for ident followed by colon (not ::). */
    if (tok->kind == TK_IDENT && parser_peek_ahead(p, 1)->kind == TK_COLON) {
        Token *label = parser_advance(p);  /* consume ident */
        parser_advance(p);                 /* consume : */
        Node *node = new_node(p, ND_LABEL, tok);
        node->label.label = label;
        node->label.stmt = parse_stmt(p);
        return node;
    }

    /* declaration-statement vs expression-statement
     *
     * N4659 §9.8 [stmt.ambig] (C++20: §8.9, C++23: §8.9):
     *   "There is an ambiguity in the grammar involving expression-
     *    statements and declarations: An expression-statement with a
     *    function-style explicit type conversion as its leftmost
     *    subexpression can be indistinguishable from a declaration
     *    where the first declarator starts with a (. In those cases
     *    the statement is a declaration."
     *
     * §9.8/2: "the whole statement might need to be examined to
     *   determine whether this is the case."
     *
     * §9.8/3: "The disambiguation is purely syntactic."
     *
     * Three cases:
     *   (a) Built-in type keyword (int, char, const, etc.) — always a
     *       declaration, no ambiguity. Parse directly.
     *   (b) User-defined type-name — potentially ambiguous.
     *       Use tentative parse: try declaration, fall back to expression.
     *   (c) Not a type at all — expression-statement.
     */

    /* IDENT-IDENT shape disambiguation.
     *
     * Standard rule — N4659 §9.8 [stmt.ambig] / §17.2/2 [temp.names]:
     * the parser must use name lookup to decide whether the leading
     * IDENT is a type-name. If lookup finds it as a type-name, treat
     * the statement as a declaration; otherwise as an expression.
     *
     * --- SHORTCUT (our implementation, not the standard's):
     * Sea-front's name lookup doesn't yet model every place a
     * type-name can come from (notably: typedefs inherited from a
     * base class via a dependent template-parameter, where we'd
     * need template instantiation to know the base; and a few
     * inline-namespace forms). When the leading IDENT is not in
     * lookup AT ALL but is followed by a second IDENT (or by
     * '* IDENT' / '& IDENT' / '&& IDENT' or by '__name(') we GUESS
     * that it's a declaration, because the alternative — parsing
     * 'unknown_ident other_ident' as an expression — is never
     * valid C++. The guess is biased by §9.8's "any statement that
     * could be a declaration IS a declaration" but is not the
     * literal §9.8 algorithm (which requires positive lookup
     * confirmation).
     *
     * False positives: if a header introduces 'foo bar' where 'foo'
     * is genuinely undeclared, we silently accept it as a
     * declaration of 'bar' of type 'foo'. Real compilers would
     * diagnose. We never produce wrong code from this — the
     * downstream sema/codegen sees an opaque-typed 'bar' and
     * either resolves it later or leaves it as-is.
     *
     * TODO(seafront#stmt-ambig): when name lookup covers inherited
     * member typedefs through dependent bases, restore the literal
     * §9.8 algorithm and remove this guess. The four sites that
     * use it (here in parse_stmt, plus parse_if_stmt's condition
     * path, parse_for_stmt's range-based detection, and
     * parse_for_stmt's init-statement) should all collapse. */
    bool might_be_decl_ident = false;
    if (parser_peek(p)->kind == TK_IDENT &&
        !parser_at_type_specifier(p) &&
        /* Only fire when the leading ident is unknown to lookup.
         * If lookup found it as a non-type (variable, function,
         * enumerator), the statement is an expression — the
         * §6.3.10 hiding rule says variables hide type-names. */
        lookup_unqualified(p, parser_peek(p)->loc, parser_peek(p)->len) == NULL) {
        Token *t1 = parser_peek_ahead(p, 1);
        Token *t2 = parser_peek_ahead(p, 2);
        if (t1->kind == TK_IDENT &&
            !lookup_unqualified(p, t1->loc, t1->len)) {
            might_be_decl_ident = true;
        }
        /* 'IDENT * IDENT' / 'IDENT & IDENT' / 'IDENT && IDENT' —
         * pointer / lvalue-ref / rvalue-ref of an unknown type. */
        if ((t1->kind == TK_STAR || t1->kind == TK_AMP || t1->kind == TK_LAND) &&
            t2->kind == TK_IDENT &&
            !lookup_unqualified(p, t2->loc, t2->len)) {
            might_be_decl_ident = true;
        }
        /* '__name(...)' followed by IDENT is a GCC type intrinsic
         * (e.g. __decltype(x)) used as a declaration type. */
        Token *first = parser_peek(p);
        if (t1->kind == TK_LPAREN &&
            first->len >= 2 && first->loc[0] == '_' && first->loc[1] == '_') {
            might_be_decl_ident = true;
        }
    }

    /* N4659 §17.7/5 [temp.res]: a qualified-id into an unknown
     * specialization that is NOT prefixed by 'typename' does NOT
     * refer to a type. So 'A::release(data)' where A is a dependent
     * template parameter cannot be a declaration — it must be an
     * expression-statement (function call).
     *
     * Detect the shape IDENT :: IDENT ( where the leading ident
     * resolves to TY_DEPENDENT. */
    if (tok->kind == TK_IDENT &&
        parser_peek_ahead(p, 1)->kind == TK_SCOPE &&
        parser_peek_ahead(p, 2)->kind == TK_IDENT &&
        parser_peek_ahead(p, 3)->kind == TK_LPAREN) {
        Declaration *ld = lookup_unqualified(p, tok->loc, tok->len);
        if (ld && ld->type && ld->type->kind == TY_DEPENDENT)
            goto parse_as_expr;
    }
    if (parser_at_type_specifier(p) || might_be_decl_ident) {
        /* Case (a): built-in type keyword — definitely a declaration */
        if (parser_peek(p)->kind != TK_IDENT)
            return parse_declaration(p);

        /* Case (b): identifier that is a type-name — potentially ambiguous.
         * Per §9.8: "any statement that could be a declaration IS a
         * declaration." We must try parsing as a declaration first.
         *
         * §9.8/2: "the whole statement might need to be examined"
         *
         * Strategy: save state, try parse_declaration() in tentative mode.
         * Check if the previous token consumed was ';' (i.e., the
         * declaration parse completed successfully through the semicolon).
         * If so, restore and re-parse in committed mode.
         * If not, restore and parse as expression-statement. */
        ParseState saved = parser_save(p);
        p->tentative = true;
        bool saved_failed = p->tentative_failed;
        p->tentative_failed = false;
        Node *trial = parse_declaration(p);
        /* Check: did the tentative parse consume through a ';' WITHOUT
         * any silenced errors? Both conditions are needed — a clean
         * advance to ';' could still mask intermediate failures. */
        bool decl_ok = (p->pos > saved.pos &&
                        p->tokens[p->pos - 1].kind == TK_SEMI &&
                        !p->tentative_failed);
        /* Most-vexing-parse guard — N4659 §9.8/3 [stmt.ambig]:
         * the rule "any statement that could be a declaration IS a
         * declaration" only applies when the statement is *actually*
         * a valid declaration. A var-decl with no declarator-id
         * (e.g. 'g<true,false>()' parsed as type 'g<true,false>'
         * with empty abstract declarator '()') is not a valid
         * declaration — fall back to expression-statement. */
        if (decl_ok && trial && trial->kind == ND_VAR_DECL &&
            trial->var_decl.name == NULL)
            decl_ok = false;
        p->tentative = false;
        p->tentative_failed = saved_failed;
        parser_restore(p, saved);

        if (decl_ok)
            return parse_declaration(p);

        /* Fall through to expression-statement */
    }

    /* expression-statement — §9.2 [stmt.expr]
     *   expression(opt) ; */
parse_as_expr:;
    Node *node = new_node(p, ND_EXPR_STMT, tok);
    node->expr_stmt.expr = parse_expr(p);
    parser_expect(p, TK_SEMI);
    return node;
}
