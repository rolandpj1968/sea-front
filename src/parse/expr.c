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

/* Default-capture body walker — N4659 §8.1.5.2 [expr.prim.lambda
 * .capture]. With '[&]' or '[=]' specified, every identifier in the
 * body that names an entity in the reaching scope (auto local /
 * parameter of the enclosing fn) becomes an implicit capture; any
 * reference to an enclosing class member implicitly captures *this.
 * Walked AFTER the body parses (so parse_compound_stmt's REGION_
 * BLOCK has popped — block-local decls inside the lambda body
 * don't reach lookup and won't be treated as captures). */
typedef struct {
    Capture **captures;
    int      *ncaptures;
    int      *ccap;
    int       default_kind;  /* 1 = '&', 2 = '=' */
    bool      has_this;
} CaptureWalkCtx;

static bool capture_already_has(CaptureWalkCtx *ctx, Token *name) {
    for (int i = 0; i < *ctx->ncaptures; i++) {
        Capture *c = &(*ctx->captures)[i];
        if (c->is_this || !c->name) continue;
        if (c->name->len == name->len &&
            memcmp(c->name->loc, name->loc, name->len) == 0)
            return true;
    }
    return false;
}

static void capture_grow(Parser *p, CaptureWalkCtx *ctx) {
    if (*ctx->ncaptures == *ctx->ccap) {
        int nc = *ctx->ccap ? *ctx->ccap * 2 : 4;
        Capture *cn = arena_alloc(p->arena, sizeof(*cn) * nc);
        if (*ctx->captures)
            memcpy(cn, *ctx->captures, sizeof(*cn) * *ctx->ncaptures);
        *ctx->captures = cn;
        *ctx->ccap = nc;
    }
}

static void capture_add_named(Parser *p, CaptureWalkCtx *ctx,
                               Token *name, bool by_ref) {
    capture_grow(p, ctx);
    Capture *c = &(*ctx->captures)[(*ctx->ncaptures)++];
    memset(c, 0, sizeof(*c));
    c->name   = name;
    c->by_ref = by_ref;
}

static void capture_add_this(Parser *p, CaptureWalkCtx *ctx) {
    if (ctx->has_this) return;
    capture_grow(p, ctx);
    Capture *c = &(*ctx->captures)[(*ctx->ncaptures)++];
    memset(c, 0, sizeof(*c));
    c->is_this = true;
    c->by_ref  = true;
    ctx->has_this = true;
}

static void default_capture_walk(Parser *p, Node *n, CaptureWalkCtx *ctx) {
    if (!n) return;
    /* Don't descend into nested lambdas — they own their own
     * capture set. The outer default capture only sees identifiers
     * lexically in the immediately enclosing lambda's body. */
    if (n->kind == ND_LAMBDA) return;
    switch (n->kind) {
    case ND_IDENT: {
        Token *nm = n->ident.name;
        if (!nm) return;
        if (nm->kind == TK_KW_THIS) { capture_add_this(p, ctx); return; }
        if (nm->kind != TK_IDENT)   return;
        if (capture_already_has(ctx, nm)) return;
        Declaration *d = lookup_unqualified(p, nm->loc, nm->len);
        if (!d || !d->home) return;
        /* Class member referenced unqualifiedly → implicit *this. */
        if (d->home->kind == REGION_CLASS) {
            capture_add_this(p, ctx);
            return;
        }
        /* Auto local / function parameter → implicit named capture.
         * Default '[&]' captures by reference; '[=]' by value. */
        if (d->home->kind == REGION_BLOCK ||
            d->home->kind == REGION_PROTOTYPE) {
            capture_add_named(p, ctx, nm, ctx->default_kind == 1);
        }
        /* Namespace / global / template / other: not captured. */
        return;
    }
    case ND_BLOCK:
        for (int i = 0; i < n->block.nstmts; i++)
            default_capture_walk(p, n->block.stmts[i], ctx);
        break;
    case ND_BINARY: case ND_ASSIGN: case ND_COMMA:
        default_capture_walk(p, n->binary.lhs, ctx);
        default_capture_walk(p, n->binary.rhs, ctx);
        break;
    case ND_UNARY: case ND_POSTFIX:
        default_capture_walk(p, n->unary.operand, ctx);
        break;
    case ND_TERNARY:
        default_capture_walk(p, n->ternary.cond,  ctx);
        default_capture_walk(p, n->ternary.then_, ctx);
        default_capture_walk(p, n->ternary.else_, ctx);
        break;
    case ND_CALL:
        default_capture_walk(p, n->call.callee, ctx);
        for (int i = 0; i < n->call.nargs; i++)
            default_capture_walk(p, n->call.args[i], ctx);
        break;
    case ND_MEMBER:
        default_capture_walk(p, n->member.obj, ctx);
        break;
    case ND_SUBSCRIPT:
        default_capture_walk(p, n->subscript.base, ctx);
        default_capture_walk(p, n->subscript.index, ctx);
        break;
    case ND_CAST:
        default_capture_walk(p, n->cast.operand, ctx);
        break;
    case ND_RETURN:
        default_capture_walk(p, n->ret.expr, ctx);
        break;
    case ND_EXPR_STMT:
        default_capture_walk(p, n->expr_stmt.expr, ctx);
        break;
    case ND_IF:
        default_capture_walk(p, n->if_.cond,  ctx);
        default_capture_walk(p, n->if_.then_, ctx);
        default_capture_walk(p, n->if_.else_, ctx);
        break;
    case ND_WHILE: case ND_DO:
        default_capture_walk(p, n->while_.cond, ctx);
        default_capture_walk(p, n->while_.body, ctx);
        break;
    case ND_FOR:
        default_capture_walk(p, n->for_.init, ctx);
        default_capture_walk(p, n->for_.cond, ctx);
        default_capture_walk(p, n->for_.inc,  ctx);
        default_capture_walk(p, n->for_.body, ctx);
        break;
    case ND_RANGE_FOR:
        default_capture_walk(p, n->range_for.range, ctx);
        default_capture_walk(p, n->range_for.body,  ctx);
        break;
    case ND_SWITCH:
        default_capture_walk(p, n->switch_.expr, ctx);
        default_capture_walk(p, n->switch_.body, ctx);
        break;
    case ND_CASE:
        default_capture_walk(p, n->case_.expr, ctx);
        default_capture_walk(p, n->case_.stmt, ctx);
        break;
    case ND_DEFAULT:
        default_capture_walk(p, n->default_.stmt, ctx);
        break;
    case ND_LABEL:
        default_capture_walk(p, n->label.stmt, ctx);
        break;
    case ND_INIT_LIST:
        for (int i = 0; i < n->init_list.nelems; i++)
            default_capture_walk(p, n->init_list.elems[i], ctx);
        break;
    case ND_VAR_DECL:
        default_capture_walk(p, n->var_decl.init, ctx);
        for (int i = 0; i < n->var_decl.ctor_nargs; i++)
            default_capture_walk(p, n->var_decl.ctor_args[i], ctx);
        break;
    default:
        break;
    }
}

/* Lambda auto-return deduction — N4659 §8.1.5.4 [expr.prim.lambda
 * .closure]/4 + §10.1.7.4 [dcl.spec.auto] (return-type deduction).
 * When the lambda omits a trailing '->' return type, deduce from
 * the first return statement's expression. Conservative: handles
 * literal kinds and ident-by-lookup. Anything more complex (calls,
 * arithmetic, casts) returns NULL and the caller falls back to
 * the skip-and-discard path. Bodies with no return statement
 * deduce to 'void' (§8.1.5.4/4). */
static Type *deduce_lambda_return(Parser *p, Node *body);

static Type *deduce_lambda_return_expr(Parser *p, Node *e) {
    if (!e) return new_type(p, TY_VOID);
    switch (e->kind) {
    case ND_NUM:      return new_type(p, TY_INT);
    case ND_FNUM:     return new_type(p, TY_DOUBLE);
    case ND_BOOL_LIT: return new_type(p, TY_BOOL);
    case ND_CHAR:     return new_type(p, TY_INT);  /* char literal is int in C */
    case ND_NULLPTR:  {
        Type *t = new_type(p, TY_PTR);
        t->base  = new_type(p, TY_VOID);
        return t;
    }
    case ND_IDENT: {
        Token *nm = e->ident.name;
        if (!nm || nm->kind != TK_IDENT) return NULL;
        Declaration *d = lookup_unqualified(p, nm->loc, nm->len);
        return (d && d->type) ? d->type : NULL;
    }
    default:
        return NULL;
    }
}

static Type *deduce_lambda_return(Parser *p, Node *body) {
    if (!body) return NULL;
    switch (body->kind) {
    case ND_RETURN:
        return deduce_lambda_return_expr(p, body->ret.expr);
    case ND_BLOCK:
        for (int i = 0; i < body->block.nstmts; i++) {
            Type *t = deduce_lambda_return(p, body->block.stmts[i]);
            if (t) return t;
        }
        /* No return at all: body falls off the end → void
         * (§8.1.5.4/4). Only at outermost block — nested blocks
         * without returns just don't contribute. */
        return NULL;
    case ND_IF:
        {
            Type *t = deduce_lambda_return(p, body->if_.then_);
            if (t) return t;
            return deduce_lambda_return(p, body->if_.else_);
        }
    default:
        return NULL;
    }
}

static Node *primary_expr(Parser *p) {
    Token *tok = parser_peek(p);

    /* GCC statement-expression — '__extension__ ({ stmts; expr; })'
     * or bare '({ ... })'. Non-standard. Captured as a raw token
     * range; codegen re-emits the extension verbatim for gcc.
     * Common in glibc / libiberty macros (obstack_alloc, XOBNEW). */
    {
        Token *first = tok;
        int start = p->pos;
        bool is_stmt_expr = false;
        if (first->kind == TK_IDENT && first->len == 13 &&
            memcmp(first->loc, "__extension__", 13) == 0 &&
            parser_peek_ahead(p, 1)->kind == TK_LPAREN &&
            parser_peek_ahead(p, 2)->kind == TK_LBRACE) {
            parser_advance(p);  /* __extension__ */
            is_stmt_expr = true;
        } else if (first->kind == TK_LPAREN &&
                   parser_peek_ahead(p, 1)->kind == TK_LBRACE) {
            is_stmt_expr = true;
        }
        if (is_stmt_expr) {
            parser_advance(p);  /* ( */
            Node *block = parse_compound_stmt(p);
            parser_expect(p, TK_RPAREN);
            Node *node = new_node(p, ND_STMT_EXPR, first);
            node->stmt_expr.block = block;
            return node;
        }
        (void)start;
    }

    switch (tok->kind) {
    case TK_NUM:        /* §5.13.2 [lex.icon] */
        parser_advance(p);
        return new_num_node(p, tok);

    case TK_FNUM:       /* §5.13.4 [lex.fcon] */
        parser_advance(p);
        return new_fnum_node(p, tok);

    case TK_STR: {      /* §5.13.5/13 [lex.string]: adjacent string
                         * literals concatenate in translation phase 6.
                         * Capture the run as (first-token, count) so
                         * codegen can re-emit the whole sequence. */
        parser_advance(p);
        int ntoks = 1;
        while (parser_at(p, TK_STR)) { parser_advance(p); ntoks++; }
        Node *node = new_node(p, ND_STR, tok);
        node->str.tok = tok;
        node->str.ntoks = ntoks;
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
        node->ident.overload_set = NULL; node->ident.n_overloads = 0;
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
        parser_skip_to_matching_rparen(p);
        return new_node(p, ND_BOOL_LIT, tok);
    }

    case TK_LBRACE: {
        /* braced-init-list — N4659 §11.6.4 [dcl.init.list]
         *   { initializer-list(opt) ,(opt) }
         * Parses into ND_INIT_LIST so aggregate init of arrays and
         * plain structs is preserved for codegen emission. Each
         * element is an assignment-expression; nested '{...}'
         * elements recurse back into this same branch. */
        parser_advance(p);
        Node *node = new_node(p, ND_INIT_LIST, tok);
        Vec elems = vec_new(p->arena);
        if (!parser_at(p, TK_RBRACE)) {
            vec_push(&elems, parse_assign_expr(p));
            /* Pack expansion: 'expr...' — consume the ellipsis if
             * present (rare in aggregate init, legal after unpacking
             * an argument pack inside a braced-init-list). */
            parser_consume(p, TK_ELLIPSIS);
            while (parser_consume(p, TK_COMMA)) {
                if (parser_at(p, TK_RBRACE)) break;  /* trailing comma */
                vec_push(&elems, parse_assign_expr(p));
                parser_consume(p, TK_ELLIPSIS);
            }
        }
        parser_expect(p, TK_RBRACE);
        node->init_list.elems  = (Node **)elems.data;
        node->init_list.nelems = elems.len;
        return node;
    }

    case TK_LBRACKET: {
        /* lambda-expression — N4659 §8.1.5 [expr.prim.lambda]
         *   lambda-introducer lambda-declarator(opt) compound-statement
         *   lambda-introducer: [ lambda-capture(opt) ]
         *   lambda-capture: capture-default | capture-list
         *                 | capture-default ',' capture-list
         *
         * Captureless lambdas with an explicit trailing return type
         * lower to a synthesised free function '__sf_lambda_<N>' at
         * TU scope; the expression evaluates to that name and the
         * closure has the §8.1.5/6 conversion to pointer-to-function
         * (sea-front skips the intermediate closure type — there are
         * no captures, so a bare ND_IDENT naming the fn is sound).
         * Capturing lambdas: closure type per §8.1.5/2 (a unique,
         * unnamed non-union class), members per §8.1.5.2 [expr.prim
         * .lambda.capture]. Sema fills in the captured-var types
         * (resolved against the enclosing scope) and the parser
         * prepends a __self pointer parameter at index 0. Init-
         * captures (C++14, [var = expr]) and '[this]' still fall
         * back to skip-and-discard for now.
         */
        ParseState saved = parser_save(p);
        parser_advance(p);  /* consume [ */

        /* Parse capture-list. Anything unexpected → restore + fallback. */
        Capture *captures = NULL;
        int      ncaptures = 0, ccap = 0;
        int      default_kind = 0;  /* 0=none, 1=[&], 2=[=] */
        bool     captures_ok = true;

        /* capture-default: '&' or '=' followed by ',' or ']' */
        if (parser_at(p, TK_AMP) &&
            (parser_peek_ahead(p, 1)->kind == TK_RBRACKET ||
             parser_peek_ahead(p, 1)->kind == TK_COMMA)) {
            parser_advance(p);
            default_kind = 1;
            parser_consume(p, TK_COMMA);
        } else if (parser_at(p, TK_ASSIGN) &&
                   (parser_peek_ahead(p, 1)->kind == TK_RBRACKET ||
                    parser_peek_ahead(p, 1)->kind == TK_COMMA)) {
            parser_advance(p);
            default_kind = 2;
            parser_consume(p, TK_COMMA);
        }

        /* capture-list: simple-capture (',' simple-capture)*
         * simple-capture: identifier | '&' identifier | 'this' */
        while (captures_ok && !parser_at(p, TK_RBRACKET)) {
            bool   by_ref  = false;
            bool   is_this = false;
            Token *cname   = NULL;
            if (parser_consume(p, TK_AMP)) by_ref = true;
            if (parser_consume(p, TK_KW_THIS)) {
                is_this = true;
                by_ref  = true;
            } else if (parser_at(p, TK_IDENT)) {
                cname = parser_peek(p);
                parser_advance(p);
            } else {
                captures_ok = false;
                break;
            }
            /* Init-capture [var = expr] (C++14) — defer; bail to fallback. */
            if (parser_at(p, TK_ASSIGN)) {
                captures_ok = false;
                break;
            }
            if (ncaptures == ccap) {
                ccap = ccap ? ccap * 2 : 4;
                Capture *nc = arena_alloc(p->arena, sizeof(*nc) * ccap);
                if (captures) memcpy(nc, captures, sizeof(*nc) * ncaptures);
                captures = nc;
            }
            memset(&captures[ncaptures], 0, sizeof(Capture));
            captures[ncaptures].name    = cname;
            captures[ncaptures].by_ref  = by_ref;
            captures[ncaptures].is_this = is_this;
            ncaptures++;
            if (!parser_consume(p, TK_COMMA)) break;
        }

        if (captures_ok && parser_consume(p, TK_RBRACKET) &&
            parser_consume(p, TK_LPAREN)) {
            /* Parse param list inline — small reimplementation of the
             * loop in decl.c; lambdas have no default args, no
             * variadic, no class-scoped param-name shadowing concerns. */
            Node **params = NULL; int nparams = 0; int cap = 0;
            if (!parser_at(p, TK_RPAREN)) {
                for (;;) {
                    DeclSpec pspec = parse_type_specifiers(p);
                    Node *pd = pspec.type ? parse_declarator(p, pspec.type) : NULL;
                    if (!pd) break;
                    pd->kind = ND_PARAM;
                    if (nparams == cap) {
                        cap = cap ? cap * 2 : 4;
                        Node **n = arena_alloc(p->arena, sizeof(*n) * cap);
                        if (params) memcpy(n, params, sizeof(*n) * nparams);
                        params = n;
                    }
                    params[nparams++] = pd;
                    if (!parser_consume(p, TK_COMMA)) break;
                }
            }
            if (parser_consume(p, TK_RPAREN)) {
                /* Optional 'mutable', 'constexpr', 'noexcept(...)' —
                 * skip until '->' or '{'. */
                while (!parser_at(p, TK_ARROW) &&
                       !parser_at(p, TK_LBRACE) && !parser_at_eof(p))
                    parser_advance(p);
                Type *ret_ty = NULL;
                if (parser_consume(p, TK_ARROW)) {
                    /* Trailing return type — parse_type_specifiers +
                     * an abstract declarator. Use the same machinery as
                     * function returns. */
                    DeclSpec rspec = parse_type_specifiers(p);
                    ret_ty = rspec.type;
                    /* Honor pointer/reference qualifiers in the
                     * trailing-return — '-> int*' / '-> T&'. */
                    while (parser_consume(p, TK_STAR)) {
                        Type *pt = new_type(p, TY_PTR);
                        pt->base = ret_ty;
                        ret_ty = pt;
                    }
                    if (parser_consume(p, TK_AMP)) {
                        Type *rt = new_type(p, TY_REF);
                        rt->base = ret_ty;
                        ret_ty = rt;
                    }
                }
                /* Auto-return deduction (§8.1.5.4/4) when '->' was
                 * omitted: parse the body, then look at the first
                 * return statement. NULL return means we couldn't
                 * deduce from the parser-side cheap shapes (literal,
                 * scope-resolved ident) and fall back to skip-and-
                 * discard. */
                if (parser_at(p, TK_LBRACE)) {
                        Node *body = parse_compound_stmt(p);
                        if (!ret_ty) ret_ty = deduce_lambda_return(p, body);
                        if (ret_ty) {
                        /* Synthesize a unique name token. The arena
                         * holds the buffer; tokens reference it.
                         *
                         * Naming: '__sf_lambda_<enclosing>__<N>'
                         * where <enclosing> is the immediately-
                         * enclosing function's name (set by
                         * parse_func_body) and <N> is per-enclosing-
                         * function so adding a lambda elsewhere
                         * doesn't shift this one's symbol. Outside
                         * any function body (TU-scope initializer,
                         * etc.) fall back to the global counter so
                         * the symbol is still unique. */
                        char buf[80];
                        int  percount = p->cur_func_lambda_count;
                        int  n;
                        if (p->cur_func_name) {
                            n = snprintf(buf, sizeof(buf),
                                "__sf_lambda_%.*s__%d",
                                p->cur_func_name->len,
                                p->cur_func_name->loc, percount);
                        } else {
                            n = snprintf(buf, sizeof(buf),
                                "__sf_lambda_%d", p->lambda_count);
                        }
                        char *name_buf = arena_alloc(p->arena, n + 1);
                        memcpy(name_buf, buf, n);
                        name_buf[n] = '\0';
                        Token *name_tok = arena_alloc(p->arena, sizeof(*name_tok));
                        memset(name_tok, 0, sizeof(*name_tok));
                        name_tok->kind = TK_IDENT;
                        name_tok->loc = name_buf;
                        name_tok->len = n;
                        name_tok->file = tok->file;
                        name_tok->line = tok->line;
                        name_tok->col = tok->col;
                        Node *fd = new_node(p, ND_FUNC_DEF, tok);
                        fd->func.name = name_tok;
                        fd->func.ret_ty = ret_ty;
                        fd->func.params = params;
                        fd->func.nparams = nparams;
                        fd->func.body = body;
                        fd->func.body_start_pos = -1;
                        /* Captureless: lambda-expression decays
                         * directly to a fn pointer (§8.1.5/6).
                         * Return ND_IDENT naming the synthesised
                         * fn — auto deduction does the decay.
                         *
                         * Capturing: build the closure ND_CLASS_DEF
                         * (struct holding T for by-value captures,
                         * T* for by-ref) and a __self parameter at
                         * fd->func.params[0]; the lambda-expression
                         * is then a struct rvalue of the closure
                         * type. Default captures '[&]'/'[=]' and
                         * '[this]' need a body walk / class context
                         * not yet wired; bail to the skip+discard
                         * fallback when we hit them. */
                        Node *closure_cdef = NULL;
                        Type *closure_type = NULL;
                        if (ncaptures > 0 || default_kind != 0) {
                            bool ok = true;
                            /* For [this]: the captured entity is the
                             * implicit object parameter of the
                             * enclosing non-static member function.
                             * §8.1.5.2/8 [expr.prim.lambda.capture] —
                             * type of the corresponding closure data
                             * member is "pointer to the class type of
                             * the enclosing function". Walk the
                             * region chain for REGION_CLASS to find
                             * it. */
                            Type *enclosing_class = NULL;
                            for (DeclarativeRegion *r = p->region;
                                 r; r = r->enclosing) {
                                if (r->kind == REGION_CLASS &&
                                    r->owner_type) {
                                    enclosing_class = r->owner_type;
                                    break;
                                }
                            }
                            /* Default captures '[&]' / '[=]' —
                             * §8.1.5.2/7,/8 specify implicit captures
                             * for every odr-used local/parameter and
                             * (for class-member refs) *this. Walk the
                             * parsed body to enumerate them — by now
                             * parse_compound_stmt has popped its
                             * REGION_BLOCK so internal lambda-body
                             * decls won't be picked up by lookup. */
                            if (default_kind != 0) {
                                CaptureWalkCtx ctx;
                                ctx.captures     = &captures;
                                ctx.ncaptures    = &ncaptures;
                                ctx.ccap         = &ccap;
                                ctx.default_kind = default_kind;
                                ctx.has_this     = false;
                                /* Seed has_this from any explicit
                                 * '[this]' the user wrote alongside
                                 * the default capture. */
                                for (int i = 0; i < ncaptures; i++)
                                    if (captures[i].is_this) {
                                        ctx.has_this = true; break;
                                    }
                                default_capture_walk(p, body, &ctx);
                            }
                            for (int i = 0; ok && i < ncaptures; i++) {
                                if (captures[i].is_this) {
                                    if (!enclosing_class) {
                                        ok = false; break;
                                    }
                                    captures[i].resolved_type =
                                        enclosing_class;
                                    continue;
                                }
                                Token *cn = captures[i].name;
                                Declaration *d = lookup_unqualified(p,
                                    cn->loc, cn->len);
                                if (!d || !d->type) { ok = false; break; }
                                captures[i].resolved_decl = d;
                                captures[i].resolved_type = d->type;
                            }
                            if (!ok) {
                                /* Drop the synthesised fd on the floor
                                 * and emit a placeholder; caller will
                                 * see ND_NULLPTR same as legacy. */
                                return new_node(p, ND_NULLPTR, tok);
                            }
                            /* Synthesise closure tag — pair with the
                             * lambda fn name above so they read
                             * naturally in --emit-c output. */
                            char cbuf[80];
                            int  cn_len;
                            if (p->cur_func_name) {
                                cn_len = snprintf(cbuf, sizeof(cbuf),
                                    "__sf_closure_%.*s__%d",
                                    p->cur_func_name->len,
                                    p->cur_func_name->loc, percount);
                            } else {
                                cn_len = snprintf(cbuf, sizeof(cbuf),
                                    "__sf_closure_%d", p->lambda_count);
                            }
                            char *cstr = arena_alloc(p->arena, cn_len + 1);
                            memcpy(cstr, cbuf, cn_len);
                            cstr[cn_len] = '\0';
                            Token *ctag = arena_alloc(p->arena, sizeof(*ctag));
                            memset(ctag, 0, sizeof(*ctag));
                            ctag->kind = TK_IDENT;
                            ctag->loc  = cstr;
                            ctag->len  = cn_len;
                            ctag->file = tok->file;
                            ctag->line = tok->line;
                            ctag->col  = tok->col;
                            closure_type      = new_type(p, TY_STRUCT);
                            closure_type->tag = ctag;
                            /* Member ND_VAR_DECLs — one per capture.
                             * §8.1.5.2/12: by-value capture stores T
                             * (a member with the captured entity's
                             * referenced type); by-ref capture binds
                             * the captured entity by reference. C has
                             * no native references, so we model the
                             * '&'-capture as T* and deref at access
                             * (see emit_ident's lambda capture rewrite). */
                            Node **mems = arena_alloc(p->arena,
                                sizeof(*mems) * ncaptures);
                            for (int i = 0; i < ncaptures; i++) {
                                Type *mty;
                                Token *mname;
                                if (captures[i].is_this) {
                                    /* §8.1.5.2/8: closure stores
                                     * 'pointer to enclosing class';
                                     * member name is '__this' (the
                                     * leading '__' makes it
                                     * implementation-reserved per
                                     * ISO §7.1.3, so it can't shadow
                                     * a user member). */
                                    mty = new_type(p, TY_PTR);
                                    mty->base = captures[i].resolved_type;
                                    Token *tn = arena_alloc(p->arena,
                                        sizeof(*tn));
                                    memset(tn, 0, sizeof(*tn));
                                    tn->kind = TK_IDENT;
                                    tn->loc  = "__this";
                                    tn->len  = 6;
                                    tn->file = tok->file;
                                    tn->line = tok->line;
                                    tn->col  = tok->col;
                                    mname = tn;
                                } else if (captures[i].by_ref) {
                                    mty = new_type(p, TY_PTR);
                                    mty->base = captures[i].resolved_type;
                                    mname = captures[i].name;
                                } else {
                                    mty = captures[i].resolved_type;
                                    mname = captures[i].name;
                                }
                                Node *m = new_node(p, ND_VAR_DECL, mname);
                                m->var_decl.name = mname;
                                m->var_decl.ty   = mty;
                                m->var_decl.init = NULL;
                                mems[i] = m;
                            }
                            closure_cdef = new_class_def_node(p, ctag,
                                mems, ncaptures, tok);
                            closure_cdef->class_def.ty = closure_type;
                            closure_type->class_def    = closure_cdef;
                            closure_type->lambda_fn    = fd;
                            fd->func.is_lambda_fn         = true;
                            fd->func.closure_struct_type  = closure_type;
                            fd->func.captures             = captures;
                            fd->func.ncaptures            = ncaptures;
                            /* Prepend __self parameter (closure*). */
                            Type *self_ptr = new_type(p, TY_PTR);
                            self_ptr->base = closure_type;
                            Token *sn = arena_alloc(p->arena, sizeof(*sn));
                            memset(sn, 0, sizeof(*sn));
                            sn->kind = TK_IDENT;
                            sn->loc  = "__self";
                            sn->len  = 6;
                            sn->file = tok->file;
                            sn->line = tok->line;
                            sn->col  = tok->col;
                            Node *sp = new_node(p, ND_PARAM, tok);
                            sp->kind        = ND_PARAM;
                            sp->param.name  = sn;
                            sp->param.ty    = self_ptr;
                            Node **np = arena_alloc(p->arena,
                                sizeof(*np) * (nparams + 1));
                            np[0] = sp;
                            for (int i = 0; i < nparams; i++)
                                np[i + 1] = params[i];
                            fd->func.params  = np;
                            fd->func.nparams = nparams + 1;
                        }
                        /* Hoist closure (if any) and lambda fn to TU
                         * top — closure first so its struct is visible
                         * before the function that takes a pointer to
                         * it.
                         *
                         * EXCEPTION: lambdas inside a template body
                         * (any REGION_TEMPLATE on the enclosing chain)
                         * carry TY_DEPENDENT through their captures /
                         * return type / body. Hoisting them now would
                         * leak unsubstituted types into TU-scope decls
                         * the C compiler rejects, and a single hoisted
                         * fn can't service multiple instantiations.
                         * Defer: leave func_def + closure attached to
                         * the ND_LAMBDA expression so the template-
                         * instantiation clone pass walks them and
                         * produces a fresh closure + fn per concrete
                         * arg set. N4659 §17.6.4 [temp.point]. */
                        bool in_template = false;
                        for (DeclarativeRegion *r = p->region; r;
                             r = r->enclosing) {
                            if (r->kind == REGION_TEMPLATE) {
                                in_template = true; break;
                            }
                        }
                        if (!in_template) {
                            int needed = closure_cdef ? 2 : 1;
                            if (p->lambda_count + needed > p->lambda_cap) {
                                int ncap = p->lambda_cap ? p->lambda_cap * 2 : 8;
                                while (p->lambda_count + needed > ncap) ncap *= 2;
                                Node **n2 = arena_alloc(p->arena,
                                    sizeof(*n2) * ncap);
                                if (p->lambda_decls)
                                    memcpy(n2, p->lambda_decls,
                                           sizeof(*n2) * p->lambda_count);
                                p->lambda_decls = n2;
                                p->lambda_cap   = ncap;
                            }
                            if (closure_cdef)
                                p->lambda_decls[p->lambda_count++] = closure_cdef;
                            p->lambda_decls[p->lambda_count++] = fd;
                        }
                        /* Bump the per-enclosing-fn counter so the
                         * next lambda in this function gets a fresh
                         * suffix. The global lambda_count was bumped
                         * above (and is still used as a fallback when
                         * cur_func_name is NULL). */
                        if (p->cur_func_name) p->cur_func_lambda_count++;
                        if (closure_type) {
                            Node *lam = new_node(p, ND_LAMBDA, tok);
                            lam->lambda.func_def     = fd;
                            lam->lambda.captures     = captures;
                            lam->lambda.ncaptures    = ncaptures;
                            lam->lambda.default_kind = default_kind;
                            lam->lambda.closure_type = closure_type;
                            lam->lambda.closure_tag  = closure_type->tag;
                            lam->resolved_type       = closure_type;
                            return lam;
                        }
                        /* Captureless: as before, return ND_IDENT
                         * with TY_FUNC for fn-pointer decay. */
                        Type **pt = NULL;
                        if (nparams > 0) {
                            pt = arena_alloc(p->arena,
                                sizeof(*pt) * nparams);
                            for (int i = 0; i < nparams; i++)
                                pt[i] = params[i]->var_decl.ty;
                        }
                        Type *fty = new_func_type(p, ret_ty, pt,
                                                   nparams, false);
                        Node *ref = new_node(p, ND_IDENT, name_tok);
                        ref->ident.name     = name_tok;
                        ref->resolved_type  = fty;
                        return ref;
                    }
                }
            }
        }
        /* Fallback: skip-and-discard. Restore position and re-skip
         * with the legacy bracket/paren/brace skipper. */
        parser_restore(p, saved);
        parser_advance(p);  /* consume [ */
        parser_skip_to_matching_rbracket(p);
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
            while (!parser_at(p, TK_LBRACE) && !parser_at_eof(p))
                parser_advance(p);
        }
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
        parser_skip_balanced(p, open, close);
        return new_cast_node(p, ty, /*operand=*/NULL, tok);
    }

    /* GCC __builtin_offsetof(type, member). Parse type + member-token
     * range; codegen re-emits for gcc. Used by <stddef.h> offsetof
     * and libcpp's DEFAULT_ALIGNMENT computation. */
    if (tok->kind == TK_IDENT && tok->len == 18 &&
        memcmp(tok->loc, "__builtin_offsetof", 18) == 0 &&
        parser_peek_ahead(p, 1)->kind == TK_LPAREN) {
        parser_advance(p);  /* __builtin_offsetof */
        parser_advance(p);  /* ( */
        Type *ty = parse_type_name(p);
        parser_expect(p, TK_COMMA);
        int mem_start_pos = p->pos;
        parser_skip_to_matching_rparen(p);
        int mem_end_pos = p->pos - 1;   /* helper consumed the ')' */
        Node *node = new_node(p, ND_OFFSETOF, tok);
        node->offsetof_.ty = ty;
        node->offsetof_.mem_toks = &p->tokens[mem_start_pos];
        node->offsetof_.n_mem_toks = mem_end_pos - mem_start_pos;
        return node;
    }

    /* GCC __builtin_va_arg(ap, type) — variadic-arg extraction.
     * The second argument is a TYPE, not an expression — non-standard
     * call syntax that the generic call parser would mis-handle.
     * Codegen re-emits verbatim for gcc. Pattern: gcc 4.8
     * tree-data-ref.c conflict_fn's va_arg loop. */
    if (tok->kind == TK_IDENT && tok->len == 16 &&
        memcmp(tok->loc, "__builtin_va_arg", 16) == 0 &&
        parser_peek_ahead(p, 1)->kind == TK_LPAREN) {
        parser_advance(p);  /* __builtin_va_arg */
        parser_advance(p);  /* ( */
        Node *ap = parse_assign_expr(p);
        parser_expect(p, TK_COMMA);
        Type *ty = parse_type_name(p);
        parser_expect(p, TK_RPAREN);
        Node *node = new_node(p, ND_VA_ARG, tok);
        node->va_arg_.ap = ap;
        node->va_arg_.ty = ty;
        return node;
    }

    /* GCC __builtin_expect(expr, hint) — branch prediction hint.
     * Value is the first argument; the second is a literal hint we
     * can drop. Without this, the generic 'unknown __builtin' path
     * below returns a BOOL_LIT(0), collapsing hot-path conditionals
     * like search_line_acc_char's match check to 'if (0) ...' and
     * producing infinite loops at runtime. N4659 does NOT include
     * this (gcc extension). */
    if (tok->kind == TK_IDENT && tok->len == 16 &&
        memcmp(tok->loc, "__builtin_expect", 16) == 0 &&
        parser_peek_ahead(p, 1)->kind == TK_LPAREN) {
        parser_advance(p);  /* name */
        parser_advance(p);  /* ( */
        Node *expr = parse_assign_expr(p);
        parser_expect(p, TK_COMMA);
        parse_assign_expr(p);  /* hint — discard */
        parser_expect(p, TK_RPAREN);
        return expr;
    }

    /* GCC __builtin_unreachable() — marks unreachable code.
     * Returns void; in gcc_assert macros it's placed inside a comma
     * expression '__builtin_unreachable(), 0'. Emit as a regular
     * function call so the C compiler sees it natively. */
    if (tok->kind == TK_IDENT && tok->len == 21 &&
        memcmp(tok->loc, "__builtin_unreachable", 21) == 0 &&
        parser_peek_ahead(p, 1)->kind == TK_LPAREN) {
        Token *name_tok = parser_advance(p);
        parser_advance(p);  /* ( */
        parser_expect(p, TK_RPAREN);
        Node *node = new_node(p, ND_CALL, name_tok);
        Node *callee = new_node(p, ND_IDENT, name_tok);
        callee->ident.name = name_tok;
        node->call.callee = callee;
        node->call.args = NULL;
        node->call.nargs = 0;
        return node;
    }

    /* GCC __builtin_alloca(size) — stack allocation.
     * Emitted as a regular call; the C compiler handles it. */
    if (tok->kind == TK_IDENT && tok->len == 16 &&
        memcmp(tok->loc, "__builtin_alloca", 16) == 0 &&
        parser_peek_ahead(p, 1)->kind == TK_LPAREN) {
        Token *name_tok = parser_advance(p);
        parser_advance(p);  /* ( */
        Node *arg = parse_assign_expr(p);
        parser_expect(p, TK_RPAREN);
        Node *node = new_node(p, ND_CALL, name_tok);
        Node *callee = new_node(p, ND_IDENT, name_tok);
        callee->ident.name = name_tok;
        node->call.callee = callee;
        node->call.args = arena_alloc(p->arena, sizeof(Node *));
        node->call.args[0] = arg;
        node->call.nargs = 1;
        return node;
    }

    /* GCC/Clang type-trait intrinsics in expression context:
     *   __is_trivial(T), __is_assignable(T, U), __is_same(T, U),
     *   __has_trivial_constructor(T), __underlying_type(T), etc.
     * These are bool-valued built-ins whose arguments are TYPES, not
     * expressions, so we can't let parse_assign_expr try to evaluate
     * 'T&' as bitwise-and. Detect the known type-trait families and
     * skip the balanced parens.
     *
     * Crucially, __builtin_* must NOT match here — those are real
     * compiler intrinsics (e.g. __builtin_ctzl, __builtin_clz,
     * __builtin_popcount, __builtin_constant_p) whose arguments are
     * expressions and whose return value matters. Discarding them
     * collapses, e.g., exact_log2(4096) to return 0, which wedges
     * the ggc allocator at runtime. Type-arg __builtin_* such as
     * __builtin_offsetof / __builtin_va_arg / __builtin_types_compatible_p
     * are handled (or absent) elsewhere. */
    bool is_type_trait =
        tok->kind == TK_IDENT && tok->len >= 5 &&
        ((tok->loc[0] == '_' && tok->loc[1] == '_' && tok->loc[2] == 'i' &&
          tok->loc[3] == 's' && tok->loc[4] == '_') ||
         (tok->loc[0] == '_' && tok->loc[1] == '_' && tok->loc[2] == 'h' &&
          tok->loc[3] == 'a' && tok->loc[4] == 's' && tok->len > 5 &&
          tok->loc[5] == '_') ||
         (tok->len == 17 && memcmp(tok->loc, "__underlying_type", 17) == 0) ||
         /* __alignof / __alignof__: GCC alignment-of operator. Accepts
          * either a type or an expression. We can't disambiguate cheaply
          * here, so swallow the parens and emit an opaque bool — the
          * concrete value is rarely needed in practice (default template
          * args, never instantiated by gcc 4.8). */
         (tok->len == 9 && memcmp(tok->loc, "__alignof", 9) == 0) ||
         (tok->len == 11 && memcmp(tok->loc, "__alignof__", 11) == 0));
    if (is_type_trait &&
        parser_peek_ahead(p, 1)->kind == TK_LPAREN &&
        lookup_unqualified(p, tok->loc, tok->len) == NULL) {
        Token *name_tok = parser_advance(p);
        parser_advance(p);  /* ( */
        parser_skip_to_matching_rparen(p);
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
     * Yields a void prvalue. The operand (when present) is captured
     * on the ND_THROW node so codegen can lower into a copy-construct
     * + TLS-state set per docs/exceptions.md. A bare 'throw;' is a
     * re-throw of the currently-handled exception; legality outside
     * a catch is sema's concern. */
    if (tok->kind == TK_KW_THROW) {
        parser_advance(p);
        Node *thr = new_node(p, ND_THROW, tok);
        if (parser_peek(p)->kind != TK_SEMI &&
            parser_peek(p)->kind != TK_RPAREN &&
            parser_peek(p)->kind != TK_COMMA &&
            parser_peek(p)->kind != TK_RBRACE) {
            thr->throw_.operand = unary_expr(p);
            thr->throw_.is_rethrow = false;
        } else {
            thr->throw_.operand = NULL;
            thr->throw_.is_rethrow = true;
        }
        return thr;
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
        node->ident.overload_set = NULL; node->ident.n_overloads = 0;
        return node;
    }

    if (tok->kind == TK_IDENT || tok->kind == TK_SCOPE) {
        bool global_scope = false;
        Vec parts = vec_new(p->arena);
        Node *lead_tid = NULL;  /* template-id for leading qualified segment */

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
                bool looks_like_template_id_arg = false;
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
                    looks_like_template_id_arg = true;
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
                /* N4659 §6.3.10/2 [basic.scope.hiding]: a variable
                 * hides a class/template name. Same rule and same
                 * SHORTCUT as the simple-ident path above — see the
                 * full comment there. */
                bool is_nontype_var = false;
                {
                    Declaration *d = lookup_unqualified(p, name->loc, name->len);
                    if (d && d->entity == ENTITY_VARIABLE &&
                        !(d->type && d->type->kind == TY_FUNC))
                        is_nontype_var = true;
                }
                if (!is_nontype_var &&
                    (lookup_is_template_name(p, name) || looks_like_template_id_arg ||
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
                    /* Qualified template-id: e.g. Box<int>::test.
                     * Save the leading template-id so the ND_QUALIFIED
                     * node can carry the template args for mangling. */
                    if (parts.len == 1)
                        lead_tid = tid;
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

            /* Rule 4 — N4659 §17.2/2 [temp.names]: after name lookup
             * finds a template-name, '<' is always the start of a
             * template-argument-list.
             *
             * Standard rule — N4659 §6.3.10/2 [basic.scope.hiding]:
             * "A class name or enumeration name can be hidden by the
             * name of a variable, data member, function, or enumerator
             * declared in the same scope." When a non-function
             * variable shadows a template name, the variable wins
             * and '<' is less-than.
             *
             * SHORTCUT (ours, not the standard): we exempt function-
             * typed variables (constructors, methods) because they
             * share the class name via the injected-class-name but
             * should NOT block template-id parsing. The standard
             * uses a more nuanced "elaborated-type-specifier" lookup
             * distinction; our function-type check is simpler.
             * TODO(seafront#tmpl-shadow): use the standard's
             * elaborated-type-specifier lookup rules. */
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
            node->ident.overload_set = NULL; node->ident.n_overloads = 0;
            return node;
        }

        /* Qualified name: A::B::C or ::foo */
        Token *last = parts.len > 0 ? (Token *)parts.data[parts.len - 1] : tok;

        /* Rule 4: final name might be a template-id: A::B<int> */
        if (parser_at(p, TK_LT) && lookup_is_template_name(p, last))
            return parse_template_id(p, last);

        {
            Node *qn = new_qualified_node(p, (Token **)parts.data, parts.len,
                                          global_scope, tok);
            qn->qualified.lead_tid = lead_tid;
            return qn;
        }
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
            parser_skip_to_matching_rbrace(p);
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
            Node *member_tid = NULL;  /* explicit '<args>' on member */
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
                    /* SHORTCUT (ours, not the standard): member template-id
                     * disambiguation for 'obj.member<...>'.
                     * Standard rule — N4659 §17.2/4 [temp.names]: after
                     * '.'/'->' + optional 'template' + name, '<' is always
                     * a template-argument-list opener if the name is a
                     * template. Without full class-scope lookup we can't
                     * always know, so we use a conservative heuristic:
                     *   - If 'template' keyword preceded the name, trust it.
                     *   - If the token after '<' is a type keyword, it's
                     *     unambiguously a template arg (less-than never has
                     *     a bare type keyword on the right).
                     *   - If the token after '<' is a type-name ident,
                     *     treat as template arg — 'member < TypeName' is
                     *     unusual compared to 'member<TypeName>'.
                     * We intentionally do NOT check if the after-token is a
                     * template name: 'obj.field < template_func(args)' is
                     * common (the comparison pattern in gcc source) and
                     * would be a false positive.
                     * TODO(seafront#member-tmpl-id): full class-scope lookup
                     * would eliminate this heuristic entirely. */
                    bool looks_like_template_id = member_template_kw;
                    if (!looks_like_template_id) {
                        Token *after = parser_peek_ahead(p, 1);
                        switch (after->kind) {
                        case TK_KW_VOID: case TK_KW_BOOL: case TK_KW_CHAR:
                        case TK_KW_SHORT: case TK_KW_INT: case TK_KW_LONG:
                        case TK_KW_FLOAT: case TK_KW_DOUBLE:
                        case TK_KW_SIGNED: case TK_KW_UNSIGNED:
                        case TK_KW_WCHAR_T: case TK_KW_CHAR16_T: case TK_KW_CHAR32_T:
                        case TK_KW_CONST: case TK_KW_VOLATILE:
                        case TK_KW_STRUCT: case TK_KW_CLASS:
                        case TK_KW_UNION: case TK_KW_ENUM:
                        case TK_KW_TYPENAME: case TK_KW_DECLTYPE:
                        case TK_KW_AUTO:
                            looks_like_template_id = true; break;
                        case TK_IDENT:
                            /* Only type names, not template names — avoids
                             * false positive on 'field < template_func(x)'. */
                            if (lookup_is_type_name(p, after))
                                looks_like_template_id = true;
                            break;
                        default: break;
                        }
                    }
                    if (looks_like_template_id)
                        member_tid = parse_template_id(p, member);
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
            node->member.template_id = member_tid;
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
            /* Braced new-initializer. Skip-and-discard — sema doesn't
             * model the initializer structure, just walk past it. */
            parser_skip_to_matching_rbrace(p);
        }

        /* Lower 'new T' to '(T *)malloc(sizeof(T))'. C++03 default-
         * initializes T (no-op for class-with-trivial-ctor / POD;
         * non-trivial ctor isn't modelled here yet). For the bootstrap
         * path the storage allocation is the load-bearing piece —
         * gcc 4.8's vec.h does 'new vec<T>; v->create(n)', so the
         * ctor is replaced by an explicit init call right after.
         *
         * TODO(seafront#new-ctor): run the ctor on the allocated
         * storage when T has a non-trivial one. */
        Node *malloc_call;
        {
            Node *callee = new_node(p, ND_IDENT, tok);
            callee->ident.name = arena_alloc(p->arena, sizeof(Token));
            *callee->ident.name = *tok;
            callee->ident.name->loc = (char *)"malloc";
            callee->ident.name->len = 6;
            callee->ident.name->kind = TK_IDENT;
            Node *sizeof_arg = new_node(p, ND_SIZEOF, tok);
            sizeof_arg->sizeof_.ty = ty;
            sizeof_arg->sizeof_.is_type = true;
            malloc_call = new_node(p, ND_CALL, tok);
            malloc_call->call.callee = callee;
            malloc_call->call.args = arena_alloc(p->arena, sizeof(Node *));
            malloc_call->call.args[0] = sizeof_arg;
            malloc_call->call.nargs = 1;
        }
        Type *ptr_ty = new_type(p, TY_PTR);
        ptr_ty->base = ty;
        return new_cast_node(p, ptr_ty, malloc_call, tok);
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
