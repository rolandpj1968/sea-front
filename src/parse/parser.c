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
/* '>>' splitting per N4659 §17.2/3 [temp.names]: "the first non-     */
/* nested >> is treated as two consecutive but distinct > tokens".    */
/* The lexer produces a single TK_SHR token. When the inner template- */
/* id consumes a TK_SHR (closing two angle brackets at once), it      */
/* sets p->split_shr = true to leave a "virtual >" behind for the     */
/* outer template-id to consume. While split_shr is true,             */
/* parser_peek / parser_at / parser_consume / parser_advance act as   */
/* if a synthetic TK_GT token sat at the current position; consuming  */
/* it via advance/consume/expect clears the flag and the real cursor  */
/* moves on.                                                          */
/* ------------------------------------------------------------------ */

/* Synthetic TK_GT returned by parser_peek when p->split_shr is set. */
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
        if (p->tentative) {
            p->tentative_failed = true;
            return NULL;
        }
        error_tok(&p->tokens[p->pos], "expected '%s', got '>'",
                  token_kind_name(k));
    }
    if (p->tokens[p->pos].kind == k)
        return parser_advance(p);
    if (p->tentative) {
        p->tentative_failed = true;
        return NULL;
    }
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
/* GCC extensions                                                      */
/* ------------------------------------------------------------------ */

/*
 * Skip a sequence of __attribute__((...)) GNU attribute specifiers.
 * Lexer treats __attribute__ as a plain identifier; recognise by name.
 *
 * Grammar (informal): __attribute__ ( ( attribute-list ) )
 * The body is balanced parens, so we just count.
 *
 * Ubiquitous in libstdc++/glibc headers; not part of ISO C++. They have
 * no impact on the parser's understanding of the type system, so we drop
 * them entirely.
 */
/* Skip C++11 attributes: [[ ... ]] (possibly multiple).
 * The lexer doesn't have a TK_LBRACKETBRACKET token; we recognise the
 * pair '[' '[' and balance toward the matching ']' ']'. */
void parser_skip_cxx_attributes(Parser *p) {
    while (parser_at(p, TK_LBRACKET) &&
           parser_peek_ahead(p, 1)->kind == TK_LBRACKET) {
        parser_advance(p);  /* [ */
        parser_advance(p);  /* [ */
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
        /* Outer ']' — must be followed by another ']' to close the
         * attribute-specifier. */
        if (parser_at(p, TK_RBRACKET))
            parser_advance(p);
    }
}

void parser_skip_gnu_attributes(Parser *p) {
    while (parser_at(p, TK_IDENT) &&
           (token_equal(parser_peek(p), "__attribute__") ||
            token_equal(parser_peek(p), "__attribute"))) {
        parser_advance(p);                  /* __attribute__ */
        parser_expect(p, TK_LPAREN);
        parser_expect(p, TK_LPAREN);
        /* Now inside the inner paren — balance toward its matching ).
         * Terminates: paren counting; advances each iteration. */
        int depth = 1;
        while (depth > 0 && !parser_at_eof(p)) {
            if (parser_at(p, TK_LPAREN)) depth++;
            else if (parser_at(p, TK_RPAREN)) { depth--; if (depth == 0) break; }
            parser_advance(p);
        }
        parser_expect(p, TK_RPAREN);        /* inner ) */
        parser_expect(p, TK_RPAREN);        /* outer ) */
    }
}

/* ------------------------------------------------------------------ */
/* Node constructors (arena-allocated)                                 */
/* ------------------------------------------------------------------ */

Node *new_node(Parser *p, NodeKind kind, Token *tok) {
    Node *node = arena_alloc(p->arena, sizeof(Node));
    node->kind = kind;
    node->tok = tok;
    node->resolved_type = NULL;
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

Node *new_ternary_node(Parser *p, Node *cond, Node *then_, Node *else_,
                       Token *tok) {
    Node *node = new_node(p, ND_TERNARY, tok);
    node->ternary.cond = cond;
    node->ternary.then_ = then_;
    node->ternary.else_ = else_;
    return node;
}

Node *new_cast_node(Parser *p, Type *ty, Node *operand, Token *tok) {
    Node *node = new_node(p, ND_CAST, tok);
    node->cast.ty = ty;
    node->cast.operand = operand;
    return node;
}

Node *new_call_node(Parser *p, Node *callee, Node **args, int nargs,
                    Token *tok) {
    Node *node = new_node(p, ND_CALL, tok);
    node->call.callee = callee;
    node->call.args = args;
    node->call.nargs = nargs;
    return node;
}

Node *new_subscript_node(Parser *p, Node *base, Node *index, Token *tok) {
    Node *node = new_node(p, ND_SUBSCRIPT, tok);
    node->subscript.base = base;
    node->subscript.index = index;
    return node;
}

Node *new_member_node(Parser *p, Node *obj, Token *member, TokenKind op,
                      Token *tok) {
    Node *node = new_node(p, ND_MEMBER, tok);
    node->member.obj = obj;
    node->member.member = member;
    node->member.op = op;
    return node;
}

Node *new_qualified_node(Parser *p, Token **parts, int nparts,
                         bool global_scope, Token *tok) {
    Node *node = new_node(p, ND_QUALIFIED, tok);
    node->qualified.parts = parts;
    node->qualified.nparts = nparts;
    node->qualified.global_scope = global_scope;
    return node;
}

Node *new_block_node(Parser *p, Node **stmts, int nstmts, Token *tok) {
    Node *node = new_node(p, ND_BLOCK, tok);
    node->block.stmts = stmts;
    node->block.nstmts = nstmts;
    node->block.scope = NULL;  /* set by parse_compound_stmt when applicable */
    return node;
}

Node *new_for_node(Parser *p, Node *init, Node *cond, Node *inc, Node *body,
                   Token *tok) {
    Node *node = new_node(p, ND_FOR, tok);
    node->for_.init = init;
    node->for_.cond = cond;
    node->for_.inc = inc;
    node->for_.body = body;
    return node;
}

Node *new_var_decl_node(Parser *p, Type *ty, Token *name, Token *tok) {
    Node *node = new_node(p, ND_VAR_DECL, tok);
    node->var_decl.ty = ty;
    node->var_decl.name = name;
    return node;
}

Node *new_typedef_node(Parser *p, Type *ty, Token *name, Token *tok) {
    Node *node = new_node(p, ND_TYPEDEF, tok);
    node->var_decl.ty = ty;
    node->var_decl.name = name;
    return node;
}

Node *new_param_node(Parser *p, Type *ty, Token *name, Token *tok) {
    Node *node = new_node(p, ND_PARAM, tok);
    node->param.ty = ty;
    node->param.name = name;
    return node;
}

Node *new_class_def_node(Parser *p, Token *tag, Node **members, int nmembers,
                         Token *tok) {
    Node *node = new_node(p, ND_CLASS_DEF, tok);
    node->class_def.tag = tag;
    node->class_def.ty = NULL;
    node->class_def.members = members;
    node->class_def.nmembers = nmembers;
    return node;
}

Node *new_template_decl_node(Parser *p, Node **params, int nparams,
                             Node *decl, Token *tok) {
    Node *node = new_node(p, ND_TEMPLATE_DECL, tok);
    node->template_decl.params = params;
    node->template_decl.nparams = nparams;
    node->template_decl.decl = decl;
    return node;
}

Node *new_template_id_node(Parser *p, Token *name, Node **args, int nargs,
                           Token *tok) {
    Node *node = new_node(p, ND_TEMPLATE_ID, tok);
    node->template_id.name = name;
    node->template_id.args = args;
    node->template_id.nargs = nargs;
    return node;
}

/* ------------------------------------------------------------------ */
/* Top-level entry point                                               */
/* ------------------------------------------------------------------ */

/*
 * parse — top-level entry point. Builds an ND_TRANSLATION_UNIT
 * over the entire token array.
 *
 * translation-unit — N4659 §A.5 (grammar) / §5.2/2 [lex.phases]:
 *   translation-unit: declaration-seq(opt)
 * The outermost declarative region of a translation unit is the
 * global namespace (§6.3.6/3 [basic.scope.namespace]); we push it
 * here as REGION_NAMESPACE before parsing.
 *
 * Pre-pass: filter out preprocessor leftover lines (#line N "file")
 * that mcpp emits anywhere in its output. The filter is line-based:
 * any token sequence whose first token is TK_HASH is dropped from
 * the start of that token's source line through end-of-line.
 *
 * C++20: adds module-declaration / export-declaration at translation-
 * unit start (N4861 §10.1 [module.unit]) — not yet handled.
 */
Node *parse(TokenArray tokens, Arena *arena, CppStandard std) {
    Parser p;
    /* Filter preprocessor leftovers (#line directives) once, up front.
     * mcpp emits '#line N "file"' tokens that can appear anywhere — we
     * drop entire #-prefixed lines so the parser never has to think
     * about them. */
    Token *filtered = arena_alloc(arena, sizeof(Token) * tokens.len);
    int n = 0;
    for (int i = 0; i < tokens.len; ) {
        if (tokens.tokens[i].kind == TK_HASH) {
            int line = tokens.tokens[i].line;
            while (i < tokens.len && tokens.tokens[i].line == line &&
                   tokens.tokens[i].kind != TK_EOF)
                i++;
            continue;
        }
        filtered[n++] = tokens.tokens[i++];
    }
    p.tokens = filtered;
    p.ntokens = n;
    p.pos = 0;
    p.file = tokens.tokens[0].file;
    p.arena = arena;
    p.std = std;
    p.tentative = false;
    p.tentative_failed = false;
    p.qualified_decl_scope = NULL;
    p.region = NULL;
    p.template_depth = 0;
    p.split_shr = false;

    /* N4659 §6.3.6/3 [basic.scope.namespace]:
     * "The outermost declarative region of a translation unit is also
     *  a namespace, called the global namespace." */
    region_push(&p, REGION_NAMESPACE, /*name=*/NULL);

    Vec decls = vec_new(arena);

    while (!parser_at_eof(&p)) {
        /* Skip preprocessor leftovers (#line directives) at top level. */
        if (parser_at(&p, TK_HASH)) {
            int line = parser_peek(&p)->line;
            while (!parser_at_eof(&p) && parser_peek(&p)->line == line)
                parser_advance(&p);
            continue;
        }
        Node *decl = parse_top_level_decl(&p);
        if (decl)
            vec_push(&decls, decl);
    }

    Node *tu = new_node(&p, ND_TRANSLATION_UNIT, &tokens.tokens[0]);
    tu->tu.decls = (Node **)decls.data;
    tu->tu.ndecls = decls.len;
    return tu;
}
