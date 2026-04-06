/*
 * parser.c — Parser infrastructure and top-level entry point.
 *
 * Provides the token stream abstraction, tentative parse save/restore,
 * arena-allocated node constructors, and the top-level parse() function.
 */

#include "parse.h"

/* ------------------------------------------------------------------ */
/* Token stream operations — index-based cursor into contiguous array  */
/*                                                                     */
/* The split_shr flag handles >> splitting (N4659 §17.2/3):            */
/* When a >> is split inside template args, the first > is consumed    */
/* and split_shr is set. Subsequent peek/at/advance see a virtual      */
/* TK_GT until the flag is cleared by advance.                         */
/* ------------------------------------------------------------------ */

/* Synthetic TK_GT token used when split_shr is active */
static Token synthetic_gt = { .kind = TK_GT, .loc = ">", .len = 1 };

Token *parser_peek(Parser *p) {
    if (p->split_shr)
        return &synthetic_gt;
    return &p->tokens[p->pos];
}

Token *parser_peek_ahead(Parser *p, int n) {
    if (p->split_shr) {
        if (n == 0) return &synthetic_gt;
        n--;  /* the virtual > occupies slot 0 */
    }
    int idx = p->pos + n;
    if (idx >= p->ntokens)
        idx = p->ntokens - 1;
    return &p->tokens[idx];
}

Token *parser_advance(Parser *p) {
    if (p->split_shr) {
        p->split_shr = false;
        return &synthetic_gt;
    }
    Token *tok = &p->tokens[p->pos];
    if (tok->kind != TK_EOF)
        p->pos++;
    return tok;
}

bool parser_at(Parser *p, TokenKind k) {
    if (p->split_shr)
        return k == TK_GT;
    return p->tokens[p->pos].kind == k;
}

bool parser_consume(Parser *p, TokenKind k) {
    if (p->split_shr) {
        if (k == TK_GT) {
            p->split_shr = false;
            return true;
        }
        return false;
    }
    if (p->tokens[p->pos].kind == k) {
        parser_advance(p);
        return true;
    }
    return false;
}

Token *parser_expect(Parser *p, TokenKind k) {
    if (p->split_shr) {
        if (k == TK_GT) {
            p->split_shr = false;
            return &synthetic_gt;
        }
        if (p->tentative)
            return NULL;
        error_tok(&p->tokens[p->pos], "expected '%s', got '>'",
                  token_kind_name(k));
    }
    if (p->tokens[p->pos].kind == k)
        return parser_advance(p);
    if (p->tentative)
        return NULL;
    error_tok(&p->tokens[p->pos], "expected '%s', got '%s'",
              token_kind_name(k), token_kind_name(p->tokens[p->pos].kind));
}

bool parser_at_eof(Parser *p) {
    if (p->split_shr)
        return false;
    return p->tokens[p->pos].kind == TK_EOF;
}

/* ------------------------------------------------------------------ */
/* Tentative parsing — save/restore                                    */
/*                                                                     */
/* The token stream is a contiguous array. Saving position is just     */
/* capturing the current index; restoring is setting it back.          */
/* No copies, no pointer chasing, no allocator state to unwind.        */
/*                                                                     */
/* Used for disambiguation rules:                                      */
/*   Rule 1 (N4659 §9.8): stmt vs decl                                */
/*   Rule 2 (N4659 §11.2): declarator ambiguities                     */
/*   Rule 5 (N4659 §17.3/2): type-id vs expression in template args   */
/* ------------------------------------------------------------------ */

ParseState parser_save(Parser *p) {
    ParseState s;
    s.pos = p->pos;
    s.region = p->region;
    s.template_depth = p->template_depth;
    s.split_shr = p->split_shr;
    return s;
}

void parser_restore(Parser *p, ParseState saved) {
    p->pos = saved.pos;
    p->region = saved.region;
    p->template_depth = saved.template_depth;
    p->split_shr = saved.split_shr;
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
 */
Node *new_num_node(Parser *p, Token *tok) {
    Node *node = new_node(p, ND_NUM, tok);
    node->num.lo = (uint64_t)tok->ival;
    node->num.hi = 0;
    node->num.is_signed = true;
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
 */
Node *parse(TokenArray tokens, Arena *arena, CppStandard std) {
    Parser p;
    p.tokens = tokens.tokens;
    p.ntokens = tokens.len;
    p.pos = 0;
    p.file = tokens.tokens[0].file;
    p.arena = arena;
    p.std = std;
    p.tentative = false;
    p.region = NULL;
    p.template_depth = 0;
    p.split_shr = false;

    /* N4659 §6.3.6/3 [basic.scope.namespace]:
     * "The outermost declarative region of a translation unit is also
     *  a namespace, called the global namespace." */
    region_push(&p, REGION_NAMESPACE, /*name=*/NULL);

    Vec decls = vec_new(arena);

    while (!parser_at_eof(&p)) {
        Node *decl = parse_top_level_decl(&p);
        if (decl)
            vec_push(&decls, decl);
    }

    Node *tu = new_node(&p, ND_TRANSLATION_UNIT, &tokens.tokens[0]);
    tu->tu.decls = (Node **)decls.data;
    tu->tu.ndecls = decls.len;
    return tu;
}
