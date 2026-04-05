/*
 * parser.c — Parser infrastructure and top-level entry point.
 *
 * Provides the token stream abstraction, tentative parse save/restore,
 * arena-allocated node constructors, and the top-level parse() function.
 */

#include "parse.h"

/* ------------------------------------------------------------------ */
/* Token stream operations                                             */
/* ------------------------------------------------------------------ */

Token *peek(Parser *p) {
    return p->tok;
}

Token *advance(Parser *p) {
    Token *tok = p->tok;
    p->prev = tok;
    if (tok->kind != TK_EOF)
        p->tok = tok->next;
    return tok;
}

bool at(Parser *p, TokenKind k) {
    return p->tok->kind == k;
}

bool consume(Parser *p, TokenKind k) {
    if (p->tok->kind == k) {
        advance(p);
        return true;
    }
    return false;
}

Token *expect(Parser *p, TokenKind k) {
    if (p->tok->kind == k)
        return advance(p);
    if (p->tentative)
        return NULL;
    error_tok(p->tok, "expected '%s', got '%s'",
              token_kind_name(k), token_kind_name(p->tok->kind));
}

bool at_eof(Parser *p) {
    return p->tok->kind == TK_EOF;
}

/* ------------------------------------------------------------------ */
/* Tentative parsing — save/restore                                    */
/*                                                                     */
/* The token stream is a singly-linked list. Saving position is just   */
/* capturing the current pointer; restoring is setting it back.        */
/* No copies, no allocator state to unwind.                            */
/*                                                                     */
/* Used for disambiguation rules:                                      */
/*   Rule 1 (N4659 §9.8): stmt vs decl                                */
/*   Rule 2 (N4659 §11.2): declarator ambiguities                     */
/*   Rule 5 (N4659 §17.3/2): type-id vs expression in template args   */
/* ------------------------------------------------------------------ */

Token *parser_save(Parser *p) {
    return p->tok;
}

void parser_restore(Parser *p, Token *saved) {
    p->tok = saved;
    p->prev = NULL;  /* prev is unreliable after restore */
}

/* ------------------------------------------------------------------ */
/* Node constructors (arena-allocated)                                 */
/* ------------------------------------------------------------------ */

Node *new_node(Parser *p, NodeKind kind, Token *tok) {
    Node *node = arena_alloc(p->arena, sizeof(Node));
    node->kind = kind;
    node->tok = tok;
    return node;
}

/*
 * Integer literal node — N4659 §5.13.2 [lex.icon]
 *
 * Converts the token's lexer-computed value into the 128-bit
 * representation (lo/hi + sign flag). The token carries suffix
 * info (u, l, ll, etc.) for type determination by sema.
 */
Node *new_num_node(Parser *p, Token *tok) {
    Node *node = new_node(p, ND_NUM, tok);
    node->num.lo = (uint64_t)tok->ival;
    node->num.hi = 0;
    node->num.is_signed = true;  /* default; sema refines based on suffix */
    return node;
}

/*
 * Floating-point literal node — N4659 §5.13.4 [lex.fcon]
 */
Node *new_fnum_node(Parser *p, Token *tok) {
    Node *node = new_node(p, ND_FNUM, tok);
    node->fnum.fval = tok->fval;
    return node;
}

/*
 * Binary expression node — covers N4659 §8.5 through §8.18
 * (multiplicative through assignment).
 */
Node *new_binary_node(Parser *p, TokenKind op, Node *lhs, Node *rhs,
                      Token *tok) {
    Node *node = new_node(p, ND_BINARY, tok);
    node->binary.op = op;
    node->binary.lhs = lhs;
    node->binary.rhs = rhs;
    return node;
}

/*
 * Unary expression node — N4659 §8.3 [expr.unary]
 */
Node *new_unary_node(Parser *p, TokenKind op, Node *operand, Token *tok) {
    Node *node = new_node(p, ND_UNARY, tok);
    node->unary.op = op;
    node->unary.operand = operand;
    return node;
}

/* ------------------------------------------------------------------ */
/* Top-level entry point                                               */
/* ------------------------------------------------------------------ */

/*
 * translation-unit — N4659 §6.1 [basic.link]
 *   translation-unit: declaration-seq(opt)
 *
 * C++20: extends with module-declaration, export-declaration
 *   (N4861 §10.1 [module.unit])
 *
 * Parses the entire token stream into an AST rooted at
 * ND_TRANSLATION_UNIT containing a list of top-level declarations.
 */
Node *parse(Token *tok, Arena *arena, CppStandard std) {
    Parser p;
    p.tok = tok;
    p.prev = NULL;
    p.file = tok->file;
    p.arena = arena;
    p.std = std;
    p.tentative = false;

    Vec decls = vec_new(arena);

    while (!at_eof(&p)) {
        Node *decl = parse_top_level_decl(&p);
        if (decl)
            vec_push(&decls, decl);
    }

    Node *tu = new_node(&p, ND_TRANSLATION_UNIT, tok);
    tu->tu.decls = (Node **)decls.data;
    tu->tu.ndecls = decls.len;
    return tu;
}
