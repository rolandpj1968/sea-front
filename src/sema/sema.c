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
/* Two-phase lookup: dependency tracking (N4659 §17.7 [temp.res])    */
/* ------------------------------------------------------------------ */

static bool type_is_dependent(Type *ty) {
    if (!ty) return false;
    if (ty->kind == TY_DEPENDENT) return true;
    if ((ty->kind == TY_PTR || ty->kind == TY_REF ||
         ty->kind == TY_RVALREF || ty->kind == TY_ARRAY) && ty->base)
        return type_is_dependent(ty->base);
    return false;
}

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
static Type *ty_int   (Sema *s) { return sema_new_type(s, TY_INT); }
static Type *ty_long  (Sema *s) { return sema_new_type(s, TY_LONG); }
static Type *ty_bool  (Sema *s) { return sema_new_type(s, TY_BOOL); }
static Type *ty_double(Sema *s) { return sema_new_type(s, TY_DOUBLE); }
static Type *ty_char  (Sema *s) { return sema_new_type(s, TY_CHAR); }

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

static bool is_fp(const Type *t) {
    return t && (t->kind == TY_FLOAT || t->kind == TY_DOUBLE || t->kind == TY_LDOUBLE);
}

static bool is_arithmetic(const Type *t) {
    return is_integer(t) || is_fp(t);
}

/* Rank for the usual arithmetic conversions — higher wins.
 * N4659 §8/2 [conv.prom], §8/3 [conv.arith]. Conservative: we just
 * pick the wider operand. Named constants so the threshold used by
 * the integer-promotion rule reads as intent, not as '3'. */
enum {
    RANK_BOOL = 0,
    RANK_CHAR,
    RANK_SHORT,
    RANK_INT,       /* = 3; integer-promotion threshold */
    RANK_LONG,
    RANK_LLONG,
    RANK_FLOAT,
    RANK_DOUBLE,
    RANK_LDOUBLE,
};

static int arith_rank(const Type *t) {
    if (!t) return -1;
    switch (t->kind) {
    case TY_BOOL:    return RANK_BOOL;
    case TY_CHAR:    return RANK_CHAR;
    case TY_SHORT:   return RANK_SHORT;
    case TY_INT:     return RANK_INT;
    case TY_LONG:    return RANK_LONG;
    case TY_LLONG:   return RANK_LLONG;
    case TY_FLOAT:   return RANK_FLOAT;
    case TY_DOUBLE:  return RANK_DOUBLE;
    case TY_LDOUBLE: return RANK_LDOUBLE;
    default:         return -1;
    }
}

/* Result type of a binary arithmetic op — usual arithmetic conversions. */
static Type *common_arith_type(Sema *s, const Type *a, const Type *b) {
    if (!is_arithmetic(a) || !is_arithmetic(b)) return NULL;
    int ra = arith_rank(a), rb = arith_rank(b);
    const Type *winner = (ra >= rb) ? a : b;
    /* Integer promotion: anything narrower than int gets promoted to int.
     * N4659 §7.6 [conv.prom]. */
    if (arith_rank(winner) < RANK_INT)
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
    /* Integer literal — N4659 §5.13.2 [lex.icon]. First-slice
     * approximation: 'int' when the value fits in signed 32-bit,
     * 'long' otherwise. This conflates two independent questions:
     *   (a) Suffix: l, ll, u, ul, etc. — steers the resolved type.
     *   (b) Platform: on LP64 'long' is 64-bit so overflow into it
     *       is the right widening; on LLP64 we'd want 'long long'.
     * N4659 §5.13.2/3 actually requires checking a list of candidate
     * types per literal shape (decimal vs hex/octal), and the suffix
     * may force the answer. None of that is modelled here.
     *
     * The 0x7fffffff check tests 'does the value fit in signed 32-bit
     * int' — so lo > INT32_MAX, or any set bit in hi, promotes to
     * long. Correct for LP64 with unsuffixed decimal literals; both
     * simplifications hold for all current sea-front targets.
     *
     * TODO(seafront#literal-suffix): read n->num's suffix and
     * N4659-conformant candidate-type selection. */
    if (n->num.hi != 0 || n->num.lo > 0x7fffffffu)
        n->resolved_type = ty_long(s);
    else
        n->resolved_type = ty_int(s);
}

static void visit_bool_lit(Sema *s, Node *n) {
    n->resolved_type = ty_bool(s);
}

static void visit_fnum(Sema *s, Node *n) {
    /* Floating literal — N4659 §5.13.4 [lex.fcon]. Always 'double'
     * here; suffix 'f' / 'F' would yield float, 'l' / 'L' long double.
     * Parser currently discards the suffix; revisit when literal
     * suffixes are tracked. TODO(seafront#literal-suffix). */
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
    /* Cloned identifiers (template instantiations) carry a
     * pre-substituted resolved_type from the clone pass. Don't
     * blow it away with a fresh lookup that may resolve to the
     * un-instantiated decl (whose type is still the un-substituted
     * template version). resolved_decl is preserved across runs
     * either way; we just guard the type write. */
    bool already_typed = n->resolved_type != NULL;
    if (!s->cur_scope) {
        if (!already_typed && n->ident.resolved_decl &&
            n->ident.resolved_decl->type)
            n->resolved_type = n->ident.resolved_decl->type;
        return;
    }
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
    if (!d) {
        /* Same fallback as the cur_scope-NULL case: prefer the cloned
         * resolved_decl's type over leaving resolved_type NULL. */
        if (!already_typed && n->ident.resolved_decl &&
            n->ident.resolved_decl->type)
            n->resolved_type = n->ident.resolved_decl->type;
        return;
    }
    n->ident.resolved_decl = d;
    /* N4659 §6.4.1/1 [basic.lookup.unqual] + §16.3 [over.match]:
     * don't collapse overloaded names at lookup time — carry the
     * full overload set so a per-call resolver can pick among them.
     * Only bother for function-typed entities; other entities aren't
     * overloadable (§6.3.10 hides type-names behind objects, etc.).
     *
     * Arena-allocate a small copy so the caller doesn't have to
     * retain scope tables. Cap at 16 which comfortably covers
     * everything gcc 4.8 throws at us — gt_pch_nx has 4 overloads,
     * vec's operator[] has 2, etc. */
    if (d->type && d->type->kind == TY_FUNC) {
        Declaration *buf[16];
        int n_ov = lookup_overload_set_from(s->cur_scope,
                                             name->loc, name->len,
                                             buf, 16);
        if (n_ov > 1) {
            Declaration **arr = arena_alloc(s->arena,
                n_ov * sizeof(Declaration *));
            for (int i = 0; i < n_ov; i++) arr[i] = buf[i];
            n->ident.overload_set = arr;
            n->ident.n_overloads = n_ov;
        }
    }
    /* N4659 §6.4.5 [class.qual]: member lookup resolves through the
     * class scope. For class members, always use the declaration's
     * type — it has the correctly patched class_region from post-
     * instantiation member-type patching. The clone's subst_type
     * result may be a different Type* without class_region. */
    bool member_type = d->home && d->home->kind == REGION_CLASS;
    if ((!already_typed || member_type) && d->type)
        n->resolved_type = d->type;
    if (member_type)
        n->ident.implicit_this = true;
    /* Phase 1: mark dependent — N4659 §17.7 [temp.res] */
    if (type_is_dependent(n->resolved_type))
        n->is_type_dependent = true;
}

static void visit_binary(Sema *s, Node *n) {
    visit(s, n->binary.lhs);
    visit(s, n->binary.rhs);
    /* Propagate dependence from children */
    if ((n->binary.lhs && n->binary.lhs->is_type_dependent) ||
        (n->binary.rhs && n->binary.rhs->is_type_dependent))
        n->is_type_dependent = true;

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

    /* Pointer arithmetic — N4659 §8.7 [expr.add] / C11 §6.5.6:
     *   ptr + int / int + ptr / ptr - int → same pointer type
     *   ptr - ptr                         → ptrdiff_t
     * ptrdiff_t is a typedef; on the LP64 targets sea-front supports
     * it is 'long'. Using ty_long here is exact on LP64 and would
     * need revisiting only if we added an LLP64 target (Windows x64
     * has long=32, ptrdiff_t=long long). Without this, any 'p + n'
     * expression produces NULL resolved_type and chained member
     * accesses don't resolve. */
    if (n->binary.op == TK_PLUS || n->binary.op == TK_MINUS) {
        bool lt_ptr = lt && (lt->kind == TY_PTR || lt->kind == TY_ARRAY);
        bool rt_ptr = rt && (rt->kind == TY_PTR || rt->kind == TY_ARRAY);
        if (lt_ptr && !rt_ptr) { n->resolved_type = lt; return; }
        if (!lt_ptr && rt_ptr && n->binary.op == TK_PLUS) {
            n->resolved_type = rt; return;
        }
        if (lt_ptr && rt_ptr && n->binary.op == TK_MINUS) {
            n->resolved_type = ty_long(s); return;
        }
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
    if (n->unary.operand && n->unary.operand->is_type_dependent)
        n->is_type_dependent = true;
}

static void visit_assign(Sema *s, Node *n) {
    visit(s, n->binary.lhs);
    visit(s, n->binary.rhs);
    /* Result of assignment is the lvalue's type. */
    n->resolved_type = n->binary.lhs->resolved_type;
    if ((n->binary.lhs && n->binary.lhs->is_type_dependent) ||
        (n->binary.rhs && n->binary.rhs->is_type_dependent))
        n->is_type_dependent = true;
}

static void visit_ternary(Sema *s, Node *n) {
    visit(s, n->ternary.cond);
    visit(s, n->ternary.then_);
    visit(s, n->ternary.else_);
    /* TODO(seafront#ternary-common-type): pick the composite type
     * per N4659 §8.16/6 [expr.cond]. The full rules go through
     * lvalue-to-rvalue, qualification, and derived-to-base
     * conversions, then a tiebreaker between the arms. For now we
     * take the then-branch type.
     *
     * Guard rails: assert on mismatches that would definitely break
     * downstream — same kind + same tag for tagged types. If either
     * arm is TY_DEPENDENT (template body, pre-instantiation) or
     * NULL (unresolved sub-expression), skip the check — the
     * substitution pass fills those in. Arithmetic mismatches (int
     * vs long) pass because downstream callers treat 'arithmetic'
     * uniformly. If the assertion fires we've found a case that
     * demands the real §8.16/6 algorithm. */
    Type *tt = n->ternary.then_ ? n->ternary.then_->resolved_type : NULL;
    Type *et = n->ternary.else_ ? n->ternary.else_->resolved_type : NULL;
    bool either_dependent = (tt && tt->kind == TY_DEPENDENT) ||
                            (et && et->kind == TY_DEPENDENT);
    if (tt && et && tt != et && !either_dependent) {
        /* Structural-compatibility check. Pure arithmetic / FP
         * mismatches (int vs long, int vs double) are ubiquitous in
         * C/C++ and survive the then-branch-wins placeholder because
         * downstream callers don't dispatch on exact arithmetic kind.
         * The cases that genuinely break are ones where a class type
         * on one side and a non-class on the other drive different
         * codegen (method dispatch, ctor materialisation, ref
         * handling, etc.), or two different struct tags. */
        bool tt_cls = tt->kind == TY_STRUCT || tt->kind == TY_UNION;
        bool et_cls = et->kind == TY_STRUCT || et->kind == TY_UNION;
        bool concerning = false;
        if (tt_cls != et_cls) {
            concerning = true;                    /* class vs non-class */
        } else if (tt_cls && et_cls) {
            if (!tt->tag || !et->tag ||
                tt->tag->len != et->tag->len ||
                memcmp(tt->tag->loc, et->tag->loc, tt->tag->len) != 0)
                concerning = true;                /* different tags */
        }
        if (concerning) {
            fprintf(stderr, "sea-front: ternary arms have incompatible "
                    "types (kind %d vs %d); the then-branch-wins "
                    "placeholder in visit_ternary needs the real "
                    "§8.16/6 [expr.cond] composite-type rule. See "
                    "TODO(seafront#ternary-common-type).\n",
                    tt->kind, et->kind);
            abort();
        }
    }
    /* Prefer the non-dependent arm when one is TY_DEPENDENT: after
     * instantiation the dependent side becomes concrete, but pre-
     * instantiation callers need a usable type NOW. */
    if (tt && tt->kind == TY_DEPENDENT && et && et->kind != TY_DEPENDENT)
        n->resolved_type = et;
    else
        n->resolved_type = tt;
    if ((n->ternary.cond && n->ternary.cond->is_type_dependent) ||
        (n->ternary.then_ && n->ternary.then_->is_type_dependent) ||
        (n->ternary.else_ && n->ternary.else_->is_type_dependent))
        n->is_type_dependent = true;
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
        visit(s, n->var_decl.ctor_args[i]);
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
            visit(s, mi->args[k]);
    }
    visit(s, n->func.body);
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

/* The per-field 'if (x) visit(s, x)' idiom is redundant — visit()
 * already returns on NULL at its entry. We drop the guards below and
 * just call visit() with potentially-NULL children. */

static void visit_return(Sema *s, Node *n) {
    visit(s, n->ret.expr);
}

static void visit_expr_stmt(Sema *s, Node *n) {
    visit(s, n->expr_stmt.expr);
}

static void visit_if(Sema *s, Node *n) {
    /* Push the if-statement scope when one was captured by the parser
     * (always set for if-with-init-declaration — N4659 §9.4.1/2,
     * §9.4.1/3). The scope contains the init's declared name so
     * references in then/else resolve.
     *
     * Only push when there's an actual init-declaration (cond is an
     * ND_VAR_DECL). The parser pushes a block region for every if,
     * but when there's no decl the block is empty — pushing it would
     * cut the enclosing chain for the common case (the if_.scope was
     * built at parse time against a chain that may not include the
     * class/param scopes sema layers in at visit_func_def time). */
    DeclarativeRegion *saved = s->cur_scope;
    bool has_init_decl = n->if_.cond && n->if_.cond->kind == ND_VAR_DECL;
    DeclarativeRegion *saved_enclosing = NULL;
    if (has_init_decl && n->if_.scope) {
        /* Re-chain the if-scope's enclosing onto the current sema
         * cur_scope so lookups walk out through class/param regions
         * that weren't visible at parse time. Restore after. */
        saved_enclosing = n->if_.scope->enclosing;
        n->if_.scope->enclosing = s->cur_scope;
        s->cur_scope = n->if_.scope;
    }
    visit(s, n->if_.init);
    visit(s, n->if_.cond);
    visit(s, n->if_.then_);
    visit(s, n->if_.else_);
    s->cur_scope = saved;
    if (has_init_decl && n->if_.scope)
        n->if_.scope->enclosing = saved_enclosing;
}

static void visit_while(Sema *s, Node *n) {
    visit(s, n->while_.cond);
    visit(s, n->while_.body);
}

static void visit_do(Sema *s, Node *n) {
    visit(s, n->do_.body);
    visit(s, n->do_.cond);
}

static void visit_for(Sema *s, Node *n) {
    visit(s, n->for_.init);
    visit(s, n->for_.cond);
    visit(s, n->for_.inc);
    visit(s, n->for_.body);
}

static void visit_member(Sema *s, Node *n) {
    visit(s, n->member.obj);
    /* For p->member, the obj's type is a pointer to a struct/union;
     * for s.member it's the struct/union directly. */
    Type *ot = n->member.obj->resolved_type;
    if (!ot) return;
    if (ot->kind == TY_PTR && ot->base) ot = ot->base;
    /* Peel TY_REF/TY_RVALREF — reference members that resolved to
     * their lowered form (pointer). */
    if (ot->kind == TY_REF || ot->kind == TY_RVALREF) ot = ot->base;
    if (!ot) return;
    if (ot->kind != TY_STRUCT && ot->kind != TY_UNION) return;
    /* For template instantiations whose Type copy lacks class_region
     * but has class_def (e.g. function parameter types), fall back
     * to scanning the class_def's members directly. This mirrors
     * the codegen's class_def scan (emit_c.c). */
    /* When a Type copy lacks class_region (e.g. from a typedef that
     * was parsed before the struct body), look up the tag in scope
     * to find the canonical Type with class_region/class_def.
     * Use the canonical type for lookup WITHOUT modifying the copy.
     * N4659 §6.4.1 [basic.lookup.unqual]. */
    if (!ot->class_region && ot->tag && s->cur_scope) {
        /* Prefer ENTITY_TAG (the struct/union tag registered by the
         * 'struct X { ... }' definition carries class_region/class_def)
         * over ENTITY_TYPE (a typedef name may alias a pre-body Type
         * copy that was registered before the body was parsed).
         * N4659 §10.1.7.3 [dcl.type.elab]. */
        Declaration *td = lookup_kind_from(s->cur_scope,
            ot->tag->loc, ot->tag->len, ENTITY_TAG);
        if (!td)
            td = lookup_unqualified_from(s->cur_scope,
                ot->tag->loc, ot->tag->len);
        if (td && td->type &&
            (td->type->class_region || td->type->class_def))
            ot = td->type;
    }
    if (!ot->class_region && ot->class_def) {
        Token *m = n->member.member;
        if (m && m->kind == TK_IDENT) {
            Node *cd = ot->class_def;
            for (int ci = 0; ci < cd->class_def.nmembers; ci++) {
                Node *cm = cd->class_def.members[ci];
                if (!cm) continue;
                Token *cmn = NULL; Type *cmt = NULL;
                if (cm->kind == ND_VAR_DECL) {
                    cmn = cm->var_decl.name; cmt = cm->var_decl.ty;
                } else if (cm->kind == ND_FUNC_DEF) {
                    cmn = cm->func.name;
                }
                if (cmn && cmn->len == m->len &&
                    memcmp(cmn->loc, m->loc, m->len) == 0) {
                    if (cmt) n->resolved_type = cmt;
                    break;
                }
            }
        }
        return;
    }
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
    if (n->member.obj && n->member.obj->is_type_dependent)
        n->is_type_dependent = true;
}

static void visit_subscript(Sema *s, Node *n) {
    visit(s, n->subscript.base);
    visit(s, n->subscript.index);
    /* arr[i] / p[i] — element type. */
    Type *bt = n->subscript.base->resolved_type;
    if (bt && (bt->kind == TY_ARRAY || bt->kind == TY_PTR) && bt->base)
        n->resolved_type = bt->base;
    if ((n->subscript.base && n->subscript.base->is_type_dependent) ||
        (n->subscript.index && n->subscript.index->is_type_dependent))
        n->is_type_dependent = true;
}

/* Minimal implicit conversion sequence rank — N4659 §16.3.3.1
 * [over.best.ics]. Lower rank = better. We only distinguish two
 * tiers for now: EXACT (types match structurally) and INCOMPATIBLE
 * (no viable conversion). Promotion, qualification, user-defined
 * etc. are future tiers that slot between these without changing
 * the best-viable comparison code.
 * TODO(seafront#over-match-ics): add promotion/conversion tiers. */
enum {
    ICS_EXACT        = 0,
    ICS_PTR_SAME_TAG = 1,  /* T* ↔ T* where T's tag matches (same classes) */
    ICS_INCOMPATIBLE = 100,
};

/* Structural equality for free-function overload resolution.
 * Mirrors codegen's types_equivalent but kept separate to avoid
 * pulling codegen into sema. Struct/union match on tag (and
 * template_args if both sides carry them); pointers/refs recurse
 * into base; primitives match on (kind, unsigned, cv).
 * TODO(seafront#types-equiv-consolidate): factor out a shared
 * helper in parse/ once emit_c.c's copy and this one have
 * stabilised.
 */
static bool sema_types_equal(Type *a, Type *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case TY_PTR: case TY_REF: case TY_RVALREF: case TY_ARRAY:
        return sema_types_equal(a->base, b->base);
    case TY_STRUCT: case TY_UNION: case TY_ENUM:
        if (!a->tag || !b->tag) return a->tag == b->tag;
        if (a->tag->len != b->tag->len) return false;
        if (memcmp(a->tag->loc, b->tag->loc, a->tag->len) != 0) return false;
        if (a->n_template_args != b->n_template_args) return false;
        for (int i = 0; i < a->n_template_args; i++)
            if (!sema_types_equal(a->template_args[i], b->template_args[i]))
                return false;
        return true;
    case TY_FUNC:
        /* Function types as template args — rare. Punt to identity. */
        return false;
    default:
        return a->is_unsigned == b->is_unsigned &&
               a->is_const    == b->is_const &&
               a->is_volatile == b->is_volatile;
    }
}

static int ics_rank(Type *param, Type *arg) {
    if (!param || !arg) return ICS_INCOMPATIBLE;
    if (sema_types_equal(param, arg)) return ICS_EXACT;
    /* Pointer-to-same-tag: T* vs T* where both Ts are class types
     * with matching tag but distinct Type* identity. Catches the
     * common case where two free-function overloads differ only in
     * their struct-pointer parameter type (e.g. gcc 4.8's
     * dump_bitmap(bitmap_head_def*) vs dump_bitmap(simple_bitmap_def*)
     * — at the call site we want the overload whose pointee tag
     * matches the argument's pointee tag). */
    if (param->kind == TY_PTR && arg->kind == TY_PTR &&
        param->base && arg->base) {
        Type *pb = param->base;
        Type *ab = arg->base;
        if ((pb->kind == TY_STRUCT || pb->kind == TY_UNION) &&
            (ab->kind == TY_STRUCT || ab->kind == TY_UNION) &&
            pb->tag && ab->tag &&
            pb->tag->len == ab->tag->len &&
            memcmp(pb->tag->loc, ab->tag->loc, pb->tag->len) == 0)
            return ICS_PTR_SAME_TAG;
    }
    return ICS_INCOMPATIBLE;
}

/* Resolve an overloaded free-function call — N4659 §16.3.3
 * [over.match.best].
 *
 *   Viable = arity-compatible + every arg has a non-INCOMPATIBLE ICS
 *            to the matching parameter.
 *   Best   = F such that for all viable G != F:
 *              F's ICS[i] ≤ G's ICS[i] for all i, AND
 *              F's ICS[i] <  G's ICS[i] for some i.
 *   If no unique best → ambiguous → return NULL (keep existing
 *   resolved_decl untouched; codegen will fall back to whatever
 *   sema's first-found pick chose).
 *
 * Doesn't yet handle: variadic ellipsis in the param list, default
 * arguments, function templates as candidates (each requires
 * deduction first), ADL, user-defined conversions, reference-
 * binding sub-ranks, partial ordering of templates.
 */
#define MAX_OVLD_CANDS 16
static Declaration *resolve_free_function_overload(
        Declaration **cands, int ncands,
        Type **arg_types, int nargs) {
    if (ncands <= 1) return ncands == 1 ? cands[0] : NULL;

    Declaration *viable[MAX_OVLD_CANDS];
    int ranks[MAX_OVLD_CANDS][MAX_OVLD_CANDS];
    int nv = 0;
    for (int i = 0; i < ncands && nv < MAX_OVLD_CANDS; i++) {
        Declaration *c = cands[i];
        if (!c || !c->type || c->type->kind != TY_FUNC) continue;
        Type *ft = c->type;
        /* Arity filter — §16.3.2/2. Variadic pass-through for
         * '...' handled by nargs >= nparams; strict match otherwise.
         * TODO(seafront#over-defaults): admit nargs < nparams when
         * trailing params have default-arg annotations. */
        bool arity_ok = ft->is_variadic ? nargs >= ft->nparams
                                        : nargs == ft->nparams;
        if (!arity_ok) continue;
        bool ok = true;
        for (int j = 0; j < nargs && ok; j++) {
            /* Variadic slot → ellipsis conversion (rank
             * ICS_INCOMPATIBLE + 1 in a future tier; for now
             * treat ellipsis args as exact-matching so variadic
             * overloads stay viable when used literally). */
            int r;
            if (j >= ft->nparams) {
                r = ICS_EXACT;
            } else {
                r = ics_rank(ft->params[j], arg_types[j]);
            }
            if (r >= ICS_INCOMPATIBLE) ok = false;
            ranks[nv][j] = r;
        }
        if (ok) viable[nv++] = c;
    }
    if (nv == 0) return NULL;
    if (nv == 1) return viable[0];

    /* Pick best viable. */
    for (int i = 0; i < nv; i++) {
        bool is_best = true;
        for (int j = 0; j < nv && is_best; j++) {
            if (i == j) continue;
            bool le_all = true;
            bool lt_any = false;
            for (int k = 0; k < nargs; k++) {
                if (ranks[i][k] > ranks[j][k]) { le_all = false; break; }
                if (ranks[i][k] < ranks[j][k]) lt_any = true;
            }
            if (!le_all || !lt_any) is_best = false;
        }
        if (is_best) return viable[i];
    }
    return NULL;  /* ambiguous */
}

static void visit_call(Sema *s, Node *n) {
    visit(s, n->call.callee);
    for (int i = 0; i < n->call.nargs; i++)
        visit(s, n->call.args[i]);
    /* Free-function overload resolution — N4659 §16.3 [over.match].
     * When the callee is an overloaded name (the parser carried the
     * full candidate set on ident.overload_set), pick the best
     * viable per §16.3.3 using the arg types we just visited.
     * Updates resolved_decl/resolved_type to the winner so codegen
     * sees the correct signature.
     *
     * Only runs for simple ident-callees (qualified-id calls and
     * member calls have their own resolution paths). Dependent
     * callees are skipped — they'll be resolved post-instantiation. */
    if (n->call.callee && n->call.callee->kind == ND_IDENT &&
        n->call.callee->ident.n_overloads > 1 &&
        !n->call.callee->is_type_dependent) {
        Type *at[MAX_OVLD_CANDS];
        int na = n->call.nargs;
        if (na > MAX_OVLD_CANDS) na = MAX_OVLD_CANDS;
        for (int i = 0; i < na; i++)
            at[i] = n->call.args[i] ? n->call.args[i]->resolved_type : NULL;
        Declaration *winner = resolve_free_function_overload(
            n->call.callee->ident.overload_set,
            n->call.callee->ident.n_overloads,
            at, na);
        if (winner) {
            n->call.callee->ident.resolved_decl = winner;
            if (winner->type)
                n->call.callee->resolved_type = winner->type;
        }
    }
    /* Functional-cast / explicit-type-conversion: 'Foo(args)' where
     * Foo is a type-name. N4659 §8.2.3 [expr.type.conv]: a simple-
     * type-specifier (or typename-specifier) followed by a
     * parenthesised expression-list is an explicit type conversion
     * whose value is a prvalue of that type. For class types the
     * prvalue materialises a temporary via direct-initialisation
     * from the argument list — codegen emits that via D-Hoist when
     * the type has a non-trivial dtor.
     *
     * The callee is an ND_IDENT whose resolved declaration is either
     * ENTITY_TYPE or ENTITY_TAG (a class tag can be registered as
     * both; see §10.1.7.3 [dcl.type.elab]/2 and the injected-class-
     * name rule §12.2 [class.pre]/2). */
    if (n->call.callee && n->call.callee->kind == ND_IDENT) {
        Declaration *d = n->call.callee->ident.resolved_decl;
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
    /* Propagate dependence from callee and args */
    if (n->call.callee && n->call.callee->is_type_dependent)
        n->is_type_dependent = true;
    for (int i = 0; i < n->call.nargs; i++)
        if (n->call.args[i] && n->call.args[i]->is_type_dependent)
            n->is_type_dependent = true;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                           */
/* ------------------------------------------------------------------ */

static void visit(Sema *s, Node *n) {
    if (!n) return;
    /* Convention: 'break' means 'done with this case, continue after
     * the switch'. 'return' is reserved for the single early-exit at
     * the top of the function. Nothing runs after the switch today;
     * the convention keeps the difference meaningful if a later
     * change adds post-switch work. */
    switch (n->kind) {
    /* Literals */
    case ND_NUM:       visit_num(s, n);       break;
    case ND_FNUM:      visit_fnum(s, n);      break;
    case ND_CHAR:      visit_chr(s, n);       break;
    case ND_BOOL_LIT:  visit_bool_lit(s, n);  break;
    case ND_IDENT:     visit_ident(s, n);     break;

    /* Operators */
    case ND_BINARY:    visit_binary(s, n);    break;
    case ND_UNARY:     visit_unary(s, n);     break;
    case ND_POSTFIX:   visit_unary(s, n);     break;
    case ND_ASSIGN:    visit_assign(s, n);    break;
    case ND_TERNARY:   visit_ternary(s, n);   break;
    case ND_CAST:
        visit(s, n->cast.operand);
        break;
    case ND_SIZEOF:
        visit(s, n->sizeof_.expr);
        break;
    case ND_COMMA:     visit_binary(s, n);    break;

    case ND_QUALIFIED:
        /* N4659 §6.4.3 [basic.lookup.qual]: qualified name lookup.
         * Resolve 'Class::method' by looking up the leading segment
         * as a type, then the trailing segment in its class_region.
         * Sets resolved_type so codegen can use decl param types
         * for mangling instead of call-site arg types. */
        if (n->qualified.nparts >= 2 && s->cur_scope) {
            Token *lead = n->qualified.parts[0];
            Token *member = n->qualified.parts[n->qualified.nparts - 1];
            if (lead && member) {
                Declaration *ld = lookup_unqualified_from(s->cur_scope,
                    lead->loc, lead->len);
                if (ld && ld->type && ld->type->class_region) {
                    Declaration *md = lookup_in_scope(ld->type->class_region,
                        member->loc, member->len);
                    if (md && md->type)
                        n->resolved_type = md->type;
                }
            }
        }
        break;

    /* Statements */
    case ND_BLOCK:     visit_block(s, n);     break;
    case ND_RETURN:    visit_return(s, n);    break;
    case ND_EXPR_STMT: visit_expr_stmt(s, n); break;
    case ND_IF:        visit_if(s, n);        break;
    case ND_WHILE:     visit_while(s, n);     break;
    case ND_DO:        visit_do(s, n);        break;
    case ND_FOR:       visit_for(s, n);       break;
    case ND_CALL:      visit_call(s, n);      break;
    case ND_SUBSCRIPT: visit_subscript(s, n); break;
    case ND_MEMBER:    visit_member(s, n);    break;

    /* switch / case / labels — N4659 §9.4.2 [stmt.switch], §9.1
     * [stmt.label]. Walk the sub-expressions so identifiers inside
     * (e.g. 'switch(obj.method())') get resolved_type and member
     * access dispatches correctly at codegen. */
    case ND_SWITCH:
        visit(s, n->switch_.init);
        visit(s, n->switch_.expr);
        visit(s, n->switch_.body);
        break;
    case ND_CASE:
        visit(s, n->case_.expr);
        visit(s, n->case_.stmt);
        break;
    case ND_DEFAULT:
        visit(s, n->default_.stmt);
        break;

    /* Declarations */
    case ND_VAR_DECL:  visit_var_decl(s, n);  break;
    case ND_FUNC_DEF:  visit_func_def(s, n);  break;
    case ND_CLASS_DEF: visit_class_def(s, n); break;

    case ND_TEMPLATE_DECL:
        /* Descend into the template's inner declaration so identifiers
         * inside template bodies get resolved_type. The clone pass
         * relies on this — cloned ident nodes inherit resolved_decl
         * from the original, so without first-sema'ing the template
         * body, expressions like '(__pos += __off)' inside an
         * instantiated method end up un-typed and don't get rewritten
         * to the operator+= call. */
        visit(s, n->template_decl.decl);
        break;

    case ND_TRANSLATION_UNIT:
        for (int i = 0; i < n->tu.ndecls; i++)
            visit(s, n->tu.decls[i]);
        break;

    default:
        /* Everything else: walk children we know about, leave
         * resolved_type as NULL. The codegen falls back to source-form
         * dumping for these. */
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                 */
/* ------------------------------------------------------------------ */

void sema_run(Node *tu, Arena *arena) {
    Sema s = { .arena = arena, .cur_scope = NULL };
    visit(&s, tu);
}
