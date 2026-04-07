/*
 * sema.c — semantic analysis pass (first slice).
 *
 * Visits the AST after parsing and fills in node->resolved_type for
 * expression nodes. Currently handles built-in arithmetic types only;
 * everything else stays as resolved_type = NULL.
 */

#include <stdlib.h>
#include <string.h>
#include "sema.h"
#include "../sea-front.h"

typedef struct {
    Arena *arena;
} Sema;

/* ------------------------------------------------------------------ */
/* Type construction (sema-side, no Parser)                            */
/* ------------------------------------------------------------------ */

static Type *sema_new_type(Sema *s, TypeKind kind) {
    Type *t = arena_alloc(s->arena, sizeof(Type));
    memset(t, 0, sizeof(Type));
    t->kind = kind;
    return t;
}

/* The well-known built-in singletons. We allocate one per kind on
 * first use, since they're immutable for our purposes. */
static Type *ty_int(Sema *s)  { return sema_new_type(s, TY_INT); }
static Type *ty_long(Sema *s) { return sema_new_type(s, TY_LONG); }
static Type *ty_bool(Sema *s) { return sema_new_type(s, TY_BOOL); }
static Type *ty_double(Sema *s) { return sema_new_type(s, TY_DOUBLE); }
static Type *ty_char(Sema *s) { return sema_new_type(s, TY_CHAR); }

/* ------------------------------------------------------------------ */
/* Type predicates                                                     */
/* ------------------------------------------------------------------ */

static bool is_integer(const Type *t) {
    if (!t) return false;
    switch (t->kind) {
    case TY_BOOL: case TY_CHAR: case TY_CHAR16: case TY_CHAR32:
    case TY_WCHAR: case TY_SHORT: case TY_INT: case TY_LONG: case TY_LLONG:
        return true;
    default:
        return false;
    }
}

static bool is_floating(const Type *t) {
    return t && (t->kind == TY_FLOAT || t->kind == TY_DOUBLE || t->kind == TY_LDOUBLE);
}

static bool is_arithmetic(const Type *t) {
    return is_integer(t) || is_floating(t);
}

/* Rank for the usual arithmetic conversions — higher wins.
 * N4659 §8/2 [conv.prom], §8/3 [conv.arith]. Conservative: we just
 * pick the wider operand. */
static int arith_rank(const Type *t) {
    if (!t) return -1;
    switch (t->kind) {
    case TY_BOOL:    return 0;
    case TY_CHAR:    return 1;
    case TY_SHORT:   return 2;
    case TY_INT:     return 3;
    case TY_LONG:    return 4;
    case TY_LLONG:   return 5;
    case TY_FLOAT:   return 6;
    case TY_DOUBLE:  return 7;
    case TY_LDOUBLE: return 8;
    default:         return -1;
    }
}

/* Result type of a binary arithmetic op — usual arithmetic conversions. */
static Type *common_arith_type(Sema *s, const Type *a, const Type *b) {
    if (!is_arithmetic(a) || !is_arithmetic(b)) return NULL;
    int ra = arith_rank(a), rb = arith_rank(b);
    const Type *winner = (ra >= rb) ? a : b;
    /* int promotion: anything narrower than int gets promoted to int.
     * arith_rank(TY_INT) is 3 by construction. */
    if (arith_rank(winner) < 3)
        return ty_int(s);
    /* Return a fresh copy so callers can mutate freely. */
    return sema_new_type(s, winner->kind);
}

/* ------------------------------------------------------------------ */
/* Forward declaration                                                 */
/* ------------------------------------------------------------------ */

static void visit(Sema *s, Node *n);

/* ------------------------------------------------------------------ */
/* Expression visitors                                                 */
/* ------------------------------------------------------------------ */

static void visit_num(Sema *s, Node *n) {
    /* Integer literal — N4659 §5.13.2 [lex.icon]. The token's
     * suffix decides the type (l, ll, u, ul, etc.); for the first
     * slice we just say 'int' for plain literals and 'long' for
     * anything that needed more bits. */
    if (n->num.hi != 0 || n->num.lo > 0x7fffffffu)
        n->resolved_type = ty_long(s);
    else
        n->resolved_type = ty_int(s);
}

static void visit_bool_lit(Sema *s, Node *n) {
    n->resolved_type = ty_bool(s);
}

static void visit_fnum(Sema *s, Node *n) {
    n->resolved_type = ty_double(s);
}

static void visit_chr(Sema *s, Node *n) {
    n->resolved_type = ty_char(s);
}

static void visit_ident(Sema *s, Node *n) {
    /* Look up the identifier in the lexical scope. The parser already
     * registered local variables in their REGION_BLOCK as ENTITY_VARIABLE
     * with their declared Type. We don't currently have a parser->sema
     * handoff for the symbol table, so for now we leave the resolution
     * to the codegen-time fallback (just emit the name verbatim) and
     * mark resolved_type NULL. */
    (void)s;
    (void)n;
}

static void visit_binary(Sema *s, Node *n) {
    visit(s, n->binary.lhs);
    visit(s, n->binary.rhs);

    Type *lt = n->binary.lhs->resolved_type;
    Type *rt = n->binary.rhs->resolved_type;

    /* Comparison operators always yield bool. */
    switch (n->binary.op) {
    case TK_EQ: case TK_NE:
    case TK_LT: case TK_LE: case TK_GT: case TK_GE:
    case TK_LAND: case TK_LOR:
        n->resolved_type = ty_bool(s);
        return;
    default:
        break;
    }

    /* Arithmetic ops use the usual arithmetic conversions. */
    n->resolved_type = common_arith_type(s, lt, rt);
}

static void visit_unary(Sema *s, Node *n) {
    visit(s, n->unary.operand);
    Type *ot = n->unary.operand->resolved_type;
    switch (n->unary.op) {
    case TK_EXCL:
        n->resolved_type = ty_bool(s);
        break;
    case TK_PLUS: case TK_MINUS: case TK_TILDE:
        n->resolved_type = ot;  /* preserves type, modulo promotion */
        break;
    default:
        n->resolved_type = ot;
        break;
    }
}

static void visit_assign(Sema *s, Node *n) {
    visit(s, n->binary.lhs);
    visit(s, n->binary.rhs);
    /* Result of assignment is the lvalue's type. */
    n->resolved_type = n->binary.lhs->resolved_type;
}

static void visit_ternary(Sema *s, Node *n) {
    visit(s, n->ternary.cond);
    visit(s, n->ternary.then_);
    visit(s, n->ternary.else_);
    /* Conservative: pick the then-branch type. The full rules in
     * §8.16/6 are intricate; this is fine for the first slice. */
    n->resolved_type = n->ternary.then_->resolved_type;
}

/* ------------------------------------------------------------------ */
/* Statement / declaration visitors                                    */
/* ------------------------------------------------------------------ */

static void visit_var_decl(Sema *s, Node *n) {
    if (n->var_decl.init)
        visit(s, n->var_decl.init);
    /* The declared type is already on var_decl.ty (set by the parser).
     * We just propagate it onto the node's resolved_type so consumers
     * can ask 'what's the type of this declaration?' uniformly. */
    n->resolved_type = n->var_decl.ty;
}

static void visit_block(Sema *s, Node *n) {
    for (int i = 0; i < n->block.nstmts; i++)
        visit(s, n->block.stmts[i]);
}

static void visit_func_def(Sema *s, Node *n) {
    if (n->func.body) visit(s, n->func.body);
}

static void visit_return(Sema *s, Node *n) {
    if (n->ret.expr) visit(s, n->ret.expr);
}

static void visit_expr_stmt(Sema *s, Node *n) {
    if (n->expr_stmt.expr) visit(s, n->expr_stmt.expr);
}

static void visit_if(Sema *s, Node *n) {
    if (n->if_.init)  visit(s, n->if_.init);
    if (n->if_.cond)  visit(s, n->if_.cond);
    if (n->if_.then_) visit(s, n->if_.then_);
    if (n->if_.else_) visit(s, n->if_.else_);
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

static void visit(Sema *s, Node *n) {
    if (!n) return;
    switch (n->kind) {
    /* Literals */
    case ND_NUM:       visit_num(s, n);       return;
    case ND_FNUM:      visit_fnum(s, n);      return;
    case ND_CHAR:      visit_chr(s, n);       return;
    case ND_BOOL_LIT:  visit_bool_lit(s, n);  return;
    case ND_IDENT:     visit_ident(s, n);     return;

    /* Operators */
    case ND_BINARY:    visit_binary(s, n);    return;
    case ND_UNARY:     visit_unary(s, n);     return;
    case ND_POSTFIX:   visit_unary(s, n);     return;
    case ND_ASSIGN:    visit_assign(s, n);    return;
    case ND_TERNARY:   visit_ternary(s, n);   return;

    /* Statements */
    case ND_BLOCK:     visit_block(s, n);     return;
    case ND_RETURN:    visit_return(s, n);    return;
    case ND_EXPR_STMT: visit_expr_stmt(s, n); return;
    case ND_IF:        visit_if(s, n);        return;

    /* Declarations */
    case ND_VAR_DECL:  visit_var_decl(s, n);  return;
    case ND_FUNC_DEF:  visit_func_def(s, n);  return;

    case ND_TRANSLATION_UNIT:
        for (int i = 0; i < n->tu.ndecls; i++)
            visit(s, n->tu.decls[i]);
        return;

    default:
        /* Everything else: walk children we know about, leave
         * resolved_type as NULL. The codegen falls back to source-form
         * dumping for these. */
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                  */
/* ------------------------------------------------------------------ */

void sema_run(Node *tu, Arena *arena) {
    Sema s = { .arena = arena };
    visit(&s, tu);
}
