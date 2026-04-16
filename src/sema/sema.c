/*
 * sema.c — semantic analysis pass.
 *
 * Visits the AST after parsing and fills in node->resolved_type
 * for expression nodes plus a few side-channel fields used by
 * codegen. Currently handles:
 *   - integer / floating / char / bool literal types
 *   - identifier resolution against the current scope chain
 *     (sets resolved_decl + implicit_this for class members)
 *   - binary / unary / ternary / assignment with usual arithmetic
 *     conversions (a coarse approximation of N4659 §8/2-3)
 *   - address-of (& → ptr) and indirection (* → base)
 *   - member access via TY_STRUCT.class_region lookup
 *   - subscript (element type from array/pointer base)
 *   - function calls (return type from TY_FUNC.ret) and
 *     functional-cast / temporary-construction shape Foo(args)
 *
 * Everything else leaves resolved_type as NULL — codegen has
 * source-form fallbacks for the gaps. As sema grows, more node
 * kinds will be filled in here rather than guessed at codegen
 * time.
 */

#include <stdlib.h>
#include <string.h>
#include "sema.h"
#include "../sea-front.h"

typedef struct {
    Arena *arena;
    /* Current lexical scope. The parser stashes a region pointer on
     * each ND_BLOCK / ND_FUNC_DEF; visit() walks them as it descends.
     * Lookup of an unqualified identifier walks this chain via the
     * region's enclosing pointer. */
    DeclarativeRegion *cur_scope;
    /* Current method's enclosing class type, or NULL if not in a
     * method body. Used to give 'this' a resolved_type so overload
     * resolution can match copy ctors against '*this'. */
    Type *cur_class_type;
} Sema;

/* ------------------------------------------------------------------ */
/* Type construction (sema-side, no Parser)                           */
/* ------------------------------------------------------------------ */

static Type *sema_new_type(Sema *s, TypeKind kind) {
    Type *t = arena_alloc(s->arena, sizeof(Type));
    memset(t, 0, sizeof(Type));
    t->kind = kind;
    return t;
}

/* Convenience constructors for the well-known built-in types.
 *
 * These currently allocate a fresh Type per call out of the arena
 * — we do NOT intern singletons. The TODO would be to cache one
 * Type per fundamental kind on the Sema struct so equality checks
 * could be pointer comparisons; with a per-TU arena the per-call
 * cost is small enough that we haven't bothered yet. */
static Type *ty_int(Sema *s)  { return sema_new_type(s, TY_INT); }
static Type *ty_long(Sema *s) { return sema_new_type(s, TY_LONG); }
static Type *ty_bool(Sema *s) { return sema_new_type(s, TY_BOOL); }
static Type *ty_double(Sema *s) { return sema_new_type(s, TY_DOUBLE); }
static Type *ty_char(Sema *s) { return sema_new_type(s, TY_CHAR); }

/* ------------------------------------------------------------------ */
/* Type predicates                                                    */
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
/* Forward declaration                                                */
/* ------------------------------------------------------------------ */

static void visit(Sema *s, Node *n);

/* ------------------------------------------------------------------ */
/* Expression visitors                                                */
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
    /* Resolve the identifier against the current lexical scope.
     * Walks the enclosing chain (block → prototype → namespace → ...)
     * via lookup_unqualified_from. The parser registered local
     * variables / parameters as ENTITY_VARIABLE with their Type,
     * so we just propagate the type onto the node.
     *
     * If the resolved declaration lives in a REGION_CLASS scope, it's
     * a class member referenced unqualifiedly inside a method body —
     * mark the ident so codegen rewrites it to 'this->name'. */
    Token *name = n->ident.name;
    /* 'this' inside a non-static method has type 'C *' for class C
     * (N4659 §16.2.2.1 [over.match.funcs]/4). It's not a normal
     * declared name — handle it before the scope lookup so '*this'
     * gets the right resolved_type for overload resolution. */
    if (name && (name->kind == TK_KW_THIS ||
                 (name->kind == TK_IDENT && name->len == 4 &&
                  memcmp(name->loc, "this", 4) == 0)) &&
        s->cur_class_type) {
        Type *ptr = sema_new_type(s, TY_PTR);
        ptr->base = s->cur_class_type;
        n->resolved_type = ptr;
        return;
    }
    if (!s->cur_scope) return;
    if (!name || name->kind != TK_IDENT) return;
    Declaration *d = lookup_unqualified_from(s->cur_scope, name->loc, name->len);
    /* N4659 §6.3.10 [basic.scope.hiding] / C §6.2.3 [Name spaces of
     * identifiers] — a tag and an ordinary identifier (variable,
     * function, typedef) with the same name coexist in separate name
     * spaces; in expression context the ordinary identifier wins.
     * Classic pattern:
     *   struct stat { ... };                      // tag
     *   int stat(const char *, struct stat *);    // function
     * Unqualified lookup may land on the tag (first-seen in the
     * bucket); re-query with ENTITY_VARIABLE preference so an
     * expression 'stat(args)' resolves to the function, not a ctor
     * call on the tag type. */
    if (d && (d->entity == ENTITY_TAG || d->entity == ENTITY_TYPE)) {
        Declaration *ord =
            lookup_kind_from(s->cur_scope, name->loc, name->len, ENTITY_VARIABLE);
        if (ord) d = ord;
    }
    if (!d) return;
    n->ident.resolved_decl = d;
    if (d->type)
        n->resolved_type = d->type;
    if (d->home && d->home->kind == REGION_CLASS)
        n->ident.implicit_this = true;
}

static void visit_binary(Sema *s, Node *n) {
    if (n->binary.lhs) visit(s, n->binary.lhs);
    if (n->binary.rhs) visit(s, n->binary.rhs);

    if (!n->binary.lhs || !n->binary.rhs) return;
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
    case TK_AMP: {
        /* Address-of: result is pointer-to-operand-type. */
        if (!ot) break;
        Type *ptr = sema_new_type(s, TY_PTR);
        ptr->base = ot;
        n->resolved_type = ptr;
        break;
    }
    case TK_STAR: {
        /* Indirection: operand should be a pointer; result is the
         * pointed-to type. Conservative — if operand isn't a pointer,
         * leave NULL and let codegen fall back. */
        if (ot && (ot->kind == TY_PTR || ot->kind == TY_ARRAY))
            n->resolved_type = ot->base;
        break;
    }
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
/* Statement / declaration visitors                                   */
/* ------------------------------------------------------------------ */

static void visit_var_decl(Sema *s, Node *n) {
    if (n->var_decl.init)
        visit(s, n->var_decl.init);
    /* Direct-init args ('T x(args)') are part of the construction
     * expression — sema must walk them so identifiers in the arg list
     * get resolved_type, which downstream overload resolution needs
     * to pick the right ctor (e.g. copy vs converting). */
    for (int i = 0; i < n->var_decl.ctor_nargs; i++)
        if (n->var_decl.ctor_args[i]) visit(s, n->var_decl.ctor_args[i]);
    /* The declared type is already on var_decl.ty (set by the parser).
     * We just propagate it onto the node's resolved_type so consumers
     * can ask 'what's the type of this declaration?' uniformly. */
    n->resolved_type = n->var_decl.ty;
}

static void visit_block(Sema *s, Node *n) {
    DeclarativeRegion *saved = s->cur_scope;
    if (n->block.scope) s->cur_scope = n->block.scope;
    for (int i = 0; i < n->block.nstmts; i++)
        visit(s, n->block.stmts[i]);
    s->cur_scope = saved;
}

static void visit_func_def(Sema *s, Node *n) {
    DeclarativeRegion *saved = s->cur_scope;
    Type *saved_class = s->cur_class_type;
    /* Enter the function's prototype scope so parameter names resolve. */
    if (n->func.param_scope) s->cur_scope = n->func.param_scope;
    if (n->func.class_type) s->cur_class_type = n->func.class_type;
    /* Mem-initializers (N4659 §15.6.2 [class.base.init]) are part of
     * the constructor — their initializer expressions need sema'ing
     * too, otherwise references like 'o.v' on a TY_REF parameter 'o'
     * stay un-typed and codegen falls back to '.' instead of '->'. */
    for (int i = 0; i < n->func.n_mem_inits; i++) {
        MemInit *mi = &n->func.mem_inits[i];
        for (int k = 0; k < mi->nargs; k++)
            if (mi->args[k]) visit(s, mi->args[k]);
    }
    if (n->func.body) visit(s, n->func.body);
    s->cur_scope = saved;
    s->cur_class_type = saved_class;
}

static void visit_class_def(Sema *s, Node *n) {
    /* Visit class members so in-class method bodies get sema'd.
     * Method bodies need the class scope active so unqualified
     * member references resolve via the chain. The parser already
     * set up the function body's enclosing chain to include the
     * class scope (during in-class parsing), so we don't need to
     * push anything extra here — visit_func_def will pick up the
     * func.param_scope which itself has the class region as its
     * enclosing.
     *
     * Push the enclosing class type so 'this' inside in-class method
     * bodies (which don't have class_type stamped on each ND_FUNC_DEF
     * — only out-of-class definitions get it) gets a resolved_type. */
    Type *saved = s->cur_class_type;
    if (n->class_def.ty) s->cur_class_type = n->class_def.ty;
    for (int i = 0; i < n->class_def.nmembers; i++)
        visit(s, n->class_def.members[i]);
    s->cur_class_type = saved;
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

static void visit_while(Sema *s, Node *n) {
    if (n->while_.cond) visit(s, n->while_.cond);
    if (n->while_.body) visit(s, n->while_.body);
}

static void visit_do(Sema *s, Node *n) {
    if (n->do_.body) visit(s, n->do_.body);
    if (n->do_.cond) visit(s, n->do_.cond);
}

static void visit_for(Sema *s, Node *n) {
    if (n->for_.init) visit(s, n->for_.init);
    if (n->for_.cond) visit(s, n->for_.cond);
    if (n->for_.inc)  visit(s, n->for_.inc);
    if (n->for_.body) visit(s, n->for_.body);
}

static void visit_member(Sema *s, Node *n) {
    visit(s, n->member.obj);
    /* For p->member, the obj's type is a pointer to a struct/union;
     * for s.member it's the struct/union directly. */
    Type *ot = n->member.obj->resolved_type;
    if (!ot) return;
    if (ot->kind == TY_PTR && ot->base) ot = ot->base;
    if (ot->kind != TY_STRUCT && ot->kind != TY_UNION) return;
    if (!ot->class_region) return;
    Token *m = n->member.member;
    if (!m || m->kind != TK_IDENT) return;
    /* Member lookup is qualified-name lookup against just the
     * class scope — no enclosing-chain walk. N4659 §6.4.3
     * [basic.lookup.qual] / §6.4.5 [class.qual]. lookup_in_scope
     * is the right primitive: it walks the class's own buckets
     * and base-class chain (so inherited members resolve) but
     * stops at the class — it does NOT climb out to the
     * enclosing namespace. */
    Declaration *d = lookup_in_scope(ot->class_region, m->loc, m->len);
    if (d && d->type)
        n->resolved_type = d->type;
}

static void visit_subscript(Sema *s, Node *n) {
    visit(s, n->subscript.base);
    visit(s, n->subscript.index);
    /* arr[i] / p[i] — element type. */
    Type *bt = n->subscript.base->resolved_type;
    if (bt && (bt->kind == TY_ARRAY || bt->kind == TY_PTR) && bt->base)
        n->resolved_type = bt->base;
}

static void visit_call(Sema *s, Node *n) {
    visit(s, n->call.callee);
    for (int i = 0; i < n->call.nargs; i++)
        visit(s, n->call.args[i]);
    /* Functional-cast / ctor temp construction: 'Foo(args)' where
     * Foo is a type-name. The callee is an ND_IDENT whose resolved
     * declaration is either ENTITY_TYPE or ENTITY_TAG (a class tag
     * may be registered as both). The expression's value is a
     * temporary of that type — codegen materializes it via D-Hoist
     * when the type has a non-trivial dtor. */
    if (n->call.callee && n->call.callee->kind == ND_IDENT) {
        Declaration *d = n->call.callee->ident.resolved_decl;
        Token *nm = n->call.callee->ident.name;
        if (nm && nm->len == 4 && memcmp(nm->loc, "stat", 4) == 0)
            fprintf(stderr, "DBG visit_call for stat: d=%p entity=%d\n",
                    (void*)d, d ? d->entity : -1);
        if (d && (d->entity == ENTITY_TYPE || d->entity == ENTITY_TAG) &&
            d->type && d->type->kind == TY_STRUCT) {
            n->resolved_type = d->type;
            return;
        }
    }
    /* Result type comes from the callee's TY_FUNC.ret. The callee may
     * be a function pointer (TY_PTR → TY_FUNC); handle that too. */
    Type *ct = n->call.callee->resolved_type;
    if (ct && ct->kind == TY_PTR && ct->base) ct = ct->base;
    if (ct && ct->kind == TY_FUNC && ct->ret)
        n->resolved_type = ct->ret;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                           */
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
    case ND_CAST:
        if (n->cast.operand) visit(s, n->cast.operand);
        return;
    case ND_SIZEOF:
        if (n->sizeof_.expr) visit(s, n->sizeof_.expr);
        return;
    case ND_COMMA:     visit_binary(s, n);    return;

    /* Statements */
    case ND_BLOCK:     visit_block(s, n);     return;
    case ND_RETURN:    visit_return(s, n);    return;
    case ND_EXPR_STMT: visit_expr_stmt(s, n); return;
    case ND_IF:        visit_if(s, n);        return;
    case ND_WHILE:     visit_while(s, n);     return;
    case ND_DO:        visit_do(s, n);        return;
    case ND_FOR:       visit_for(s, n);       return;
    case ND_CALL:      visit_call(s, n);      return;
    case ND_SUBSCRIPT: visit_subscript(s, n); return;
    case ND_MEMBER:    visit_member(s, n);    return;

    /* Declarations */
    case ND_VAR_DECL:  visit_var_decl(s, n);  return;
    case ND_FUNC_DEF:  visit_func_def(s, n);  return;
    case ND_CLASS_DEF: visit_class_def(s, n); return;

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
/* Public entry point                                                 */
/* ------------------------------------------------------------------ */

void sema_run(Node *tu, Arena *arena) {
    Sema s = { .arena = arena, .cur_scope = NULL };
    visit(&s, tu);
}
