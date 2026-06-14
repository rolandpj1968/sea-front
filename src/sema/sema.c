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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sema.h"
#include "../sea-front.h"
#include "../template/clone.h"

typedef struct {
    Arena *arena;
    /* Translation unit root — used by helpers that need to search
     * the TU for an ND_CLASS_DEF matching (tag, template_args). The
     * instantiation/clone pipeline can produce multiple Type*
     * instances for the same logical struct; some carry class_def
     * (hooked up to a real class-body Node), others don't. When a
     * visit-site has one of the un-hooked copies, a TU-wide lookup
     * by tag+args finds the hooked one. */
    Node *tu;
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
/* Type predicates — N4659 §6.7.1 [basic.fundamental]                 */
/* ------------------------------------------------------------------ */

/* Integer types — N4659 §6.7.1/4 [basic.fundamental]: the standard
 * (signed/unsigned) integer types plus bool, char, char16_t,
 * char32_t, wchar_t. Kept lockstep with the §8 [expr] arithmetic
 * conversions and §16.3.3.1 [over.best.ics] integer-rank logic. */
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

/* Floating-point types — N4659 §6.7.1/8 [basic.fundamental]. */
static bool is_fp(const Type *t) {
    return t && (t->kind == TY_FLOAT || t->kind == TY_DOUBLE || t->kind == TY_LDOUBLE);
}

/* Arithmetic types — N4659 §6.7.1/9: integer + floating-point. */
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
    /* Return a fresh copy so callers can mutate freely. Preserve
     * the winner's signedness — without this, `int * unsigned long`
     * loses its unsigned flag and downstream consumers (e.g. the
     * Itanium placement-new aliasing path that keys on `size_t`)
     * see a signed long instead. N4659 §7.6 [conv.prom] requires
     * preserving signedness of the operand that won the rank
     * comparison. */
    Type *out = sema_new_type(s, winner->kind);
    out->is_unsigned = winner->is_unsigned;
    return out;
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

/* Boolean literal — N4659 §5.13.6 [lex.bool]: 'true' / 'false'. */
static void visit_bool_lit(Sema *s, Node *n) {
    n->resolved_type = ty_bool(s);
}

/* nullptr / NULL / __null — N4659 §5.13.7 [lex.nullptr] / §4.10/1
 * [conv.ptr]: a null pointer constant has type std::nullptr_t and
 * converts to any pointer type. We model it as 'void *' so it
 * passes overload resolution against any pointer parameter (the
 * ics_rank doesn't distinguish nullptr_t vs void* for pointer
 * candidates yet; the conversion-rank machinery is a TODO).
 *
 * Without this, the overload resolution at a call site like
 *   f(parser, false, true, NULL)
 * would see arg3 as having no resolved type, fail to find a viable
 * candidate, and fall through to the historical 'first found' decl
 * — which is wrong when the matching overload is later in the
 * overload set. */
static void visit_nullptr(Sema *s, Node *n) {
    Type *p = sema_new_type(s, TY_PTR);
    p->base = sema_new_type(s, TY_VOID);
    n->resolved_type = p;
}

static void visit_fnum(Sema *s, Node *n) {
    /* Floating literal — N4659 §5.13.4 [lex.fcon]. Always 'double'
     * here; suffix 'f' / 'F' would yield float, 'l' / 'L' long double.
     * Parser currently discards the suffix; revisit when literal
     * suffixes are tracked. TODO(seafront#literal-suffix). */
    n->resolved_type = ty_double(s);
}

/* Character literal — N4659 §5.13.3 [lex.ccon]. Currently always
 * 'char'; the standard prescribes wchar_t / char16_t / char32_t for
 * L'x' / u'x' / U'x' prefixes (deferred). */
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
    /* Two-phase name lookup (N4659 §17.5.6 [temp.res]): a non-
     * dependent name inside a template body is bound at template
     * definition time (phase 1). If phase-1 bound this ident to a
     * NAMESPACE-scope entity (a free function or namespace
     * variable) and phase-2 now finds an inherited CLASS member
     * made visible only because a previously-dependent base
     * became concrete, that rebind is the classic two-phase
     * failure — keep the phase-1 binding. We DON'T apply the
     * rule when phase-1's own binding was already a class member:
     * there phase-2 is just picking the instantiated version of
     * the same lookup (e.g. `using B<T>::foo;` rebinds to the
     * instantiated B<int>::foo at phase 2). Pattern:
     * g++.dg/lookup/template1.C. */
    if (n->ident.resolved_decl &&
        n->ident.resolved_decl->home &&
        n->ident.resolved_decl->home->kind != REGION_CLASS &&
        d && d->home &&
        d->home->kind == REGION_CLASS &&
        d->home->owner_type != s->cur_class_type) {
        if (!already_typed && n->ident.resolved_decl->type)
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
     * real-world C++ overload sets (4-5 overloads per name is
     * typical even in heavily-overloaded library code). */
    /* Populate overload_set when the name could be overloaded — i.e.
     * is a function (d->type TY_FUNC) OR a function template (ENTITY_
     * TEMPLATE whose tmpl_node wraps a function). Non-function kinds
     * (variable, type, enum constant) aren't overloadable; skip. */
    bool is_fn = d->type && d->type->kind == TY_FUNC;
    bool is_fn_tmpl = d->entity == ENTITY_TEMPLATE && d->tmpl_node;
    if (is_fn || is_fn_tmpl) {
        /* Unbounded — overload sets in real C++ TUs can be large
         * (a free function name with a per-type non-template
         * overload for every GC-traced struct, sitting alongside
         * a couple of templated overloads). A fixed-cap stack
         * buffer would silently truncate the LIFO bucket chain
         * and drop earlier-registered overloads, blinding overload
         * resolution to candidates that should be viable. */
        Vec ov = vec_new(s->arena);
        lookup_overload_set_into_vec(s->cur_scope,
                                      name->loc, name->len, &ov);
        if (ov.len > 1) {
            n->ident.overload_set = (Declaration **)ov.data;
            n->ident.n_overloads = ov.len;
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
    /* Enumerators declared at class scope (N4659 §10.2 [dcl.enum]) are
     * named constants, not data members — `this->ENUMERATOR` is wrong;
     * the bare name is the value. Same applies to nested types and
     * static data members; static handling is downstream in emit. */
    if (member_type && d->entity != ENTITY_ENUMERATOR &&
        d->entity != ENTITY_TYPE && d->entity != ENTITY_TAG)
        n->ident.implicit_this = true;
    /* Phase 1: mark dependent — N4659 §17.7 [temp.res] */
    if (type_is_dependent(n->resolved_type))
        n->is_type_dependent = true;
}

static Type *find_class_operator_return_type(Sema *s, Type *class_ty,
                                              const char *op, int op_len);
static const char *binop_token_to_op_str(TokenKind op, int *out_len);
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
    /* Class operator overload — N4659 §16.5 [over.oper]. When LHS is
     * a struct/union, look up the operator method on the class and
     * use its return type. Without this, '(a - b).method()' on a
     * value-type struct loses its type at the binary node, breaking
     * downstream member resolution. Real-world shape:
     *   (maxv - minv).zext(nprec) != double_int::mask(nprec) */
    if (!n->resolved_type && lt &&
        (lt->kind == TY_STRUCT || lt->kind == TY_UNION) && lt->tag) {
        int op_len = 0;
        const char *op_str = binop_token_to_op_str(n->binary.op, &op_len);
        if (op_str) {
            Type *ret = find_class_operator_return_type(s, lt, op_str, op_len);
            if (ret) {
                if (ret->kind == TY_REF || ret->kind == TY_RVALREF)
                    ret = ret->base;
                n->resolved_type = ret;
            }
        }
    }
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
        /* Address-of: result is pointer-to-operand-type. There are
         * no pointers-to-references in C++ (N4659 §11.3.1/1), so
         * if the operand's lvalue type is T& or T&&, strip to T —
         * '&ref' yields T*, not T&* or T&&*. */
        if (!ot) break;
        Type *referent = ot;
        if (referent->kind == TY_REF || referent->kind == TY_RVALREF)
            referent = referent->base;
        Type *ptr = sema_new_type(s, TY_PTR);
        ptr->base = referent;
        n->resolved_type = ptr;
        break;
    }
    case TK_STAR: {
        /* Indirection: operand should be a pointer; result is the
         * pointed-to type. Conservative — if operand isn't a pointer,
         * leave NULL and let codegen fall back. For references (lowered
         * to T* in our C but TY_REF in the AST): a source-level '*ref'
         * reads the refferee, so the resolved_type is the referent type
         * with the ref stripped. N4659 §11.3.2/1 [dcl.ref]. */
        if (ot && (ot->kind == TY_PTR || ot->kind == TY_ARRAY ||
                   ot->kind == TY_REF || ot->kind == TY_RVALREF))
            n->resolved_type = ot->base;
        /* '*x' where x is a class value dispatches to operator*().
         * Look up the method's return type so downstream consumers
         * (e.g. method-call on the result) can see the class. Fall
         * back to the class tag itself if we can't find the method —
         * common case is a self-returning deref like insn_gen_fn's
         * 'operator*() const { return *this; }' which yields the
         * same class. N4659 §16.5 [over.oper]. */
        else if (ot && (ot->kind == TY_STRUCT || ot->kind == TY_UNION) &&
                 ot->tag)
            n->resolved_type = ot;
        break;
    }
    default:
        n->resolved_type = ot;
        break;
    }
    if (n->unary.operand && n->unary.operand->is_type_dependent)
        n->is_type_dependent = true;
}

/* Assignment — N4659 §8.18 [expr.ass]. Result type and value
 * category: "the type of an assignment expression is that of its
 * left operand; the result is an lvalue referring to the left
 * operand." Compound assignments (+=, -=, etc.) follow the same
 * rule via the unfolded `lhs = lhs op rhs` shape; sema doesn't
 * distinguish them at this level. */
static void visit_assign(Sema *s, Node *n) {
    visit(s, n->binary.lhs);
    visit(s, n->binary.rhs);
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
        /* Enums interconvert with int per §7.3 [conv]; ternary
         * with one enum arm and one int arm is ubiquitous and not
         * actually concerning. */
        bool tt_int_or_enum = tt->kind == TY_INT || tt->kind == TY_ENUM ||
                              tt->kind == TY_LONG || tt->kind == TY_LLONG ||
                              tt->kind == TY_SHORT || tt->kind == TY_CHAR ||
                              tt->kind == TY_BOOL;
        bool et_int_or_enum = et->kind == TY_INT || et->kind == TY_ENUM ||
                              et->kind == TY_LONG || et->kind == TY_LLONG ||
                              et->kind == TY_SHORT || et->kind == TY_CHAR ||
                              et->kind == TY_BOOL;
        bool concerning = false;
        if (tt_int_or_enum && et_int_or_enum) {
            /* Both arithmetic-like — survives the placeholder. */
        } else if (tt_cls != et_cls) {
            concerning = true;                    /* class vs non-class */
        } else if (tt_cls && et_cls) {
            if (!tt->tag || !et->tag ||
                tt->tag->len != et->tag->len ||
                memcmp(tt->tag->loc, et->tag->loc, tt->tag->len) != 0)
                concerning = true;                /* different tags */
        }
        if (concerning) {
            /* Soft warning rather than abort — the then-branch-wins
             * placeholder usually produces usable downstream behavior
             * for the cases we hit in real source. Aborting blocks
             * progress on real builds. TODO(seafront#ternary-common-type)
             * for the proper §8.16/6 algorithm. */
            fprintf(stderr, "sea-front: ternary arms have incompatible "
                    "types (kind %d vs %d) — using then-branch type. "
                    "TODO(seafront#ternary-common-type).\n",
                    tt->kind, et->kind);
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

/* Variable declaration — N4659 §10 [dcl.dcl] / §11.6 [dcl.init].
 * The parser has already built the var-decl's type; sema visits the
 * initializer (if any) and the direct-init ctor-args, then exposes
 * the declared type on resolved_type so consumers can ask "what's
 * the type of this declaration?" uniformly. */
static void visit_var_decl(Sema *s, Node *n) {
    if (n->var_decl.init)
        visit(s, n->var_decl.init);
    /* Direct-init args ('T x(args)') are part of the construction
     * expression — sema must walk them so identifiers in the arg list
     * get resolved_type, which downstream overload resolution needs
     * to pick the right ctor (e.g. copy vs converting). */
    for (int i = 0; i < n->var_decl.ctor_nargs; i++)
        visit(s, n->var_decl.ctor_args[i]);
    /* C++11 'auto' deduction — N4659 §10.1.7.4 [dcl.spec.auto]/6.
     * Three forms supported:
     *   auto  x = e;   — T = e (refs stripped, decay-ish)
     *   auto& x = e;   — T = e (refs stripped); outer is ref
     *   auto* x = e;   — T = pointee of e; outer is ptr
     * Find the 'is_auto' leaf inside any TY_PTR / TY_REF / TY_RVALREF
     * wrappers and replace it with the deduced base. The outer
     * wrappers (and their cv-qualifiers) survive unchanged. */
    if (n->var_decl.ty && n->var_decl.init) {
        Type *outer = n->var_decl.ty;
        Type *leaf = outer;
        while (leaf && !leaf->is_auto &&
               (leaf->kind == TY_PTR || leaf->kind == TY_REF ||
                leaf->kind == TY_RVALREF))
            leaf = leaf->base;
        if (leaf && leaf->is_auto) {
            Type *init_ty = n->var_decl.init->resolved_type;
            Type *deduced = init_ty;
            /* Strip initializer's outermost ref — auto sees the
             * referent type, not the reference. §10.1.7.4.1/3 */
            if (deduced && (deduced->kind == TY_REF ||
                            deduced->kind == TY_RVALREF))
                deduced = deduced->base;
            /* Function-to-pointer decay — §7.1.6.4.1: deduction with
             * 'auto x = f;' where f names a function gives x the
             * function pointer type. Also applies when the initializer
             * is a captureless lambda (lowered to a fn name). */
            if (deduced && deduced->kind == TY_FUNC && outer == leaf) {
                Type *fp = sema_new_type(s, TY_PTR);
                fp->base = deduced;
                deduced = fp;
            }
            /* For 'auto*': peel one pointer level off the initializer
             * to get the pointee type. */
            if (deduced && outer != leaf && outer->kind == TY_PTR &&
                deduced->kind == TY_PTR)
                deduced = deduced->base;
            if (deduced) {
                bool keep_const    = leaf->is_const;
                bool keep_volatile = leaf->is_volatile;
                *leaf = *deduced;
                leaf->is_auto     = false;
                leaf->is_const    |= keep_const;
                leaf->is_volatile |= keep_volatile;
            }
        }
    }
    n->resolved_type = n->var_decl.ty;
}

/* Compound statement — N4659 §9.3 [stmt.block]. Pushes the block's
 * declarative region onto the sema scope chain so identifiers in
 * nested statements resolve via inner-scope-first lookup.
 *
 * Cloned blocks (template instantiation) have block.scope = NULL —
 * clone.c clears it because the parser-built scope's bucket entries
 * point at the un-substituted Type*. Without re-creating a block
 * scope here, local variable declarations in the cloned body never
 * land in any region and references to them fall through to the
 * enclosing class scope — sema then incorrectly inserts 'this->'
 * on a 'local'-shadowing class member. Shape:
 *   'size_t size = htab->size; for (int i = size - 1; ...)'
 * would produce 'this->size' on the second 'size' reference,
 * which the struct doesn't have — link/compile fails downstream.
 * N4659 §6.3.3 [basic.scope.block] + §6.3.10 [basic.scope.hiding]. */
static void visit_block(Sema *s, Node *n) {
    DeclarativeRegion *saved = s->cur_scope;
    bool created = false;
    if (!n->block.scope) {
        DeclarativeRegion *r = arena_alloc(s->arena, sizeof(*r));
        memset(r, 0, sizeof(*r));
        r->kind = REGION_BLOCK;
        r->enclosing = s->cur_scope;
        n->block.scope = r;
        created = true;
    }
    s->cur_scope = n->block.scope;
    for (int i = 0; i < n->block.nstmts; i++) {
        Node *st = n->block.stmts[i];
        visit(s, st);
        /* Register local variable declarations into the freshly-
         * created scope after visiting their initializer. Order
         * matches N4659 §6.3.3/2: a name's potential scope begins
         * at its declarator and ends at the block's closing brace,
         * so the init expression sees the OUTER name when shadowed. */
        if (created && st && st->kind == ND_VAR_DECL &&
            st->var_decl.name) {
            region_declare_raw(n->block.scope, s->arena,
                st->var_decl.name->loc, st->var_decl.name->len,
                ENTITY_VARIABLE, st->var_decl.ty);
        }
    }
    s->cur_scope = saved;
}

/* Function definition — N4659 §11.4 [dcl.fct.def]. Establishes the
 * function's prototype scope (so parameters resolve) and, for member
 * functions, the enclosing class type (so 'this'-typed lookups
 * inside the body know which class). Mem-initializers are walked
 * separately because they live outside the body but inside the
 * function's lexical scope. */
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

/* Class definition — N4659 §12 [class]. Visits class members so
 * in-class method bodies get sema'd. */
static void visit_class_def(Sema *s, Node *n) {
    /* Method bodies need the class scope active so unqualified
     * member references resolve via the chain. The parser already
     * set up the function body's enclosing chain to include the
     * class scope (during in-class parsing), so we don't need to
     * push anything extra here — visit_func_def will pick up the
     * func.param_scope which itself has the class region as its
     * enclosing.
     *
     * NSDMIs ('int j = i;' in-class) are visited HERE rather than
     * through a method body, so they need cur_scope pointed at the
     * class region for the ident lookup to find member 'i'. Without
     * this, the lookup walks straight to the enclosing namespace and
     * the bare 'i' resolves nowhere — emit then drops it as an
     * unrewritten identifier and the C compiler fails with 'i
     * undeclared'. N4659 §12.6.2/9 [class.base.init].
     *
     * Push the enclosing class type so 'this' inside in-class method
     * bodies (which don't have class_type stamped on each ND_FUNC_DEF
     * — only out-of-class definitions get it) gets a resolved_type. */
    Type *saved_ct = s->cur_class_type;
    DeclarativeRegion *saved_sc = s->cur_scope;
    if (n->class_def.ty) s->cur_class_type = n->class_def.ty;
    if (n->class_def.ty && n->class_def.ty->class_region)
        s->cur_scope = n->class_def.ty->class_region;
    for (int i = 0; i < n->class_def.nmembers; i++)
        visit(s, n->class_def.members[i]);
    s->cur_class_type = saved_ct;
    s->cur_scope = saved_sc;
}

/* The per-field 'if (x) visit(s, x)' idiom is redundant — visit()
 * already returns on NULL at its entry. We drop the guards below and
 * just call visit() with potentially-NULL children. */

/* Return statement — N4659 §9.6.3 [stmt.return]. The return-value
 * expression is the only sub-node; conversion to the function's
 * declared return type is handled at codegen, not here. */
static void visit_return(Sema *s, Node *n) {
    visit(s, n->ret.expr);
}

/* Expression statement — N4659 §9.5 [stmt.expr]. The contained
 * expression is evaluated for its side effects; its value is
 * discarded. */
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

/* while-statement — N4659 §9.5.2 [stmt.while]. */
static void visit_while(Sema *s, Node *n) {
    visit(s, n->while_.cond);
    visit(s, n->while_.body);
}

/* do-while — N4659 §9.5.3 [stmt.do]. */
static void visit_do(Sema *s, Node *n) {
    visit(s, n->do_.body);
    visit(s, n->do_.cond);
}

/* for-statement — N4659 §9.5.4 [stmt.for]. The init, cond, and inc
 * sub-expressions live in for-init scope (§6.3.3 [basic.scope.for]).
 * Mirror the if-with-init handling: re-chain the captured for-init
 * scope onto the current sema cur_scope and push it before walking
 * the children, so a 'for (struct X *x = ...; x != y; ...)' resolves
 * x to the variable (not the type X) inside the cond and inc.
 * Without this, a shadowing variable like vec.h's
 *   for (struct loop *loop = ...; loop != root; ...)
 * looks up bare 'loop' in scopes that contain only the type tag,
 * and codegen mis-classifies the comparison as a struct-operator
 * overload. */
static void visit_for(Sema *s, Node *n) {
    DeclarativeRegion *saved = s->cur_scope;
    DeclarativeRegion *saved_enclosing = NULL;
    bool pushed = false;
    if (n->for_.scope) {
        saved_enclosing = n->for_.scope->enclosing;
        n->for_.scope->enclosing = s->cur_scope;
        s->cur_scope = n->for_.scope;
        pushed = true;
    }
    visit(s, n->for_.init);
    visit(s, n->for_.cond);
    visit(s, n->for_.inc);
    visit(s, n->for_.body);
    if (pushed) {
        s->cur_scope = saved;
        n->for_.scope->enclosing = saved_enclosing;
    }
}

/* Substitute the class-template parameter bindings encoded on
 * `class_ty` (a TY_STRUCT/TY_UNION carrying a template_id_node)
 * into a member's declared type. Returns the substituted type, or
 * `member_ty` unchanged if class_ty isn't a template instance or
 * member_ty has no dependent components.
 *
 * Without this, `outer.last()` on a class-template instance has
 * resolved_type = TY_REF(T) where T is TY_DEPENDENT — a chained
 * `.field` access on the result strips ref to T (still dependent)
 * and looks up `field` on a TY_DEPENDENT, which silently returns
 * resolved_type=NULL. Overload resolution at a containing call
 * sees NULL arg-types and skips the rewrite to ND_TEMPLATE_ID,
 * leaving the free function template uninstantiated at link time.
 *
 * N4659 §17.5.2 [temp.mem] / §17.6.7 [temp.dep]. */
static Type *subst_member_type_with_class_args(Sema *s,
                                                Type *member_ty,
                                                Type *class_ty) {
    if (!member_ty || !class_ty) return member_ty;
    if (!type_has_dependent(member_ty)) return member_ty;
    /* Find the source primary template — needed for the parameter
     * list. Prefer class_ty->template_args (direct Type* slots),
     * which are populated per-clone and don't share state with other
     * instantiations. Fall back to template_id_node's arg-node walk
     * when no template_args slot is set (mostly source-template
     * sema, pre-instantiation). The reversed precedence matters
     * because the substituted template-id Node can be aliased across
     * instantiations via arena reuse — its args may carry stale
     * TY_DEPENDENT slots that don't match the per-instance Type.
     * Pattern: gcc 4.8 va_stack::alloc<T>'s
     * 'v.vec_->embedded_init(...)' where v's cloned class type has
     * concrete template_args but its template_id_node still aliases
     * an earlier-pass dependent shape. N4659 §17.7.1 [temp.inst]. */
    Node *tmpl = NULL;
    Type **arg_types = NULL;
    int nargs = 0;
    if (class_ty->tag && class_ty->n_template_args > 0 && s->cur_scope) {
        tmpl = find_primary_template_in_scope(s->cur_scope,
            class_ty->tag->loc, class_ty->tag->len);
        arg_types = class_ty->template_args;
        nargs = class_ty->n_template_args;
    } else if (class_ty->template_id_node &&
               class_ty->template_id_node->kind == ND_TEMPLATE_ID) {
        Node *tid = class_ty->template_id_node;
        tmpl = tid->template_id.resolved_tmpl;
        if ((!tmpl || tmpl->kind != ND_TEMPLATE_DECL) &&
            tid->template_id.name && s->cur_scope) {
            tmpl = find_primary_template_in_scope(s->cur_scope,
                tid->template_id.name->loc,
                tid->template_id.name->len);
        }
        nargs = tid->template_id.nargs;
        /* Convert tid arg nodes to Type* */
        arg_types = arena_alloc(s->arena, nargs * sizeof(Type *));
        for (int i = 0; i < nargs; i++) {
            Node *a = tid->template_id.args[i];
            arg_types[i] = (a && a->kind == ND_VAR_DECL) ? a->var_decl.ty : NULL;
        }
    }
    if (!tmpl || tmpl->kind != ND_TEMPLATE_DECL) return member_ty;
    int nparams = tmpl->template_decl.nparams;
    if (nparams == 0 || nargs == 0 || !arg_types) return member_ty;
    /* The picked template must accommodate every arg — otherwise
     * we're keying a SubstMap from a partial spec's narrower head
     * against a wider arg list, leaving the trailing args unbound
     * and leaking TY_DEPENDENT through subst_type.
     * find_primary_template_in_scope should already prevent this;
     * the check makes the invariant load-bearing rather than
     * best-effort. */
    if (nparams < nargs) return member_ty;
    SubstMap map = subst_map_new(s->arena, nparams);
    /* Direct Type**-arg binding. Mirrors subst_map_bind_args's
     * per-param loop but takes Type* directly. */
    for (int i = 0; i < nparams && i < nargs; i++) {
        Node *param = tmpl->template_decl.params[i];
        if (!param || !param->param.name || !arg_types[i]) continue;
        subst_map_add(&map, param->param.name, arg_types[i]);
    }
    if (map.nentries == 0) return member_ty;
    return subst_type(member_ty, &map, s->arena);
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
    /* Preserve the call-site type's template_id_node BEFORE swapping
     * to the canonical type — class-template parameter substitution
     * for the looked-up member's declared type needs the call-site
     * args, but the canonical-tag Type carries the primary template's
     * un-substituted shape (template_id_node=NULL). N4659 §17.6.7
     * [temp.dep]. */
    Type *call_site_ty = ot;
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
        /* Class-template instance: tag matches a template name. The
         * registered ENTITY_TAG/ENTITY_TYPE may be a pre-body copy
         * with NULL class_region; the class body lives on a template
         * specialization's inner ND_CLASS_DEF. Walk all templates with
         * this name (primary + partial specs) and prefer one whose
         * class body is non-empty AND matches the call-site args.
         *
         * Common shape: an empty primary
         * `template<class,class,class> struct C { };` paired with one
         * or more populated partial specializations carrying every
         * method. Naively walking to the primary returns the empty
         * body and the lookup misses every member.
         * N4659 §17.8.3.2 [temp.class.spec.match]. */
        if (!ot->class_region && !ot->class_def && ot->tag) {
            Declaration *cands[16];
            int n = lookup_overload_set_from(s->cur_scope,
                ot->tag->loc, ot->tag->len, cands, 16);
            Type *best = NULL;
            int best_score = -1;
            for (int i = 0; i < n; i++) {
                Declaration *cd = cands[i];
                if (!cd || cd->entity != ENTITY_TEMPLATE ||
                    !cd->tmpl_node ||
                    cd->tmpl_node->kind != ND_TEMPLATE_DECL)
                    continue;
                Node *inner = cd->tmpl_node->template_decl.decl;
                if (!inner || inner->kind != ND_CLASS_DEF) continue;
                Type *ity = inner->class_def.ty;
                if (!ity) continue;
                /* Score: prefer specializations with members, then
                 * those whose template_id_node matches the call-site
                 * args. Without proper §17.8.3.2 matching this is a
                 * heuristic — sufficient for the empty-primary +
                 * populated-partial-spec shape. */
                int score = 0;
                if (inner->class_def.nmembers > 0) score += 10;
                if (ity->template_id_node) score += 1;
                if (score > best_score) {
                    best_score = score;
                    best = ity;
                }
            }
            if (best) ot = best;
        }
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
                    /* Synthesize a TY_FUNC so callers (visit_call →
                     * ct->ret) can read the return type. Without this,
                     * `outer.last()` on a class-template fallback path
                     * leaves resolved_type=NULL, any subsequent
                     * `.field` access silently drops out, and overload
                     * resolution at a containing call sees NULL
                     * arg-types and skips the rewrite to ND_TEMPLATE_ID
                     * so the free function template is never
                     * instantiated. N4659 §17.5.2 [temp.mem] / §8.2.5
                     * [expr.ref]. */
                    cmt = func_type_from_func_def(s->arena, cm);
                }
                if (cmn && tokens_equal(cmn, m)) {
                    if (cmt)
                        n->resolved_type = subst_member_type_with_class_args(
                            s, cmt, call_site_ty);
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
    /* Class-template instance fallback: if the canonical class
     * (the primary template's region) doesn't have the member, try
     * each specialization's region until one resolves. An empty
     * primary `template<class,class,class> struct C { };` paired
     * with populated partial specializations is the shape that
     * needs this — the canonical lookup hits the empty primary,
     * misses every method, and any chained `obj.method().field`
     * propagates NULL up through the call.
     * N4659 §17.8.3.2 [temp.class.spec.match]. */
    if (!d && ot->tag && s->cur_scope) {
        Declaration *cands[16];
        int n_cands = lookup_overload_set_from(s->cur_scope,
            ot->tag->loc, ot->tag->len, cands, 16);
        for (int i = 0; i < n_cands && !d; i++) {
            Declaration *cd = cands[i];
            if (!cd || cd->entity != ENTITY_TEMPLATE ||
                !cd->tmpl_node ||
                cd->tmpl_node->kind != ND_TEMPLATE_DECL)
                continue;
            Node *inner = cd->tmpl_node->template_decl.decl;
            if (!inner || inner->kind != ND_CLASS_DEF) continue;
            Type *ity = inner->class_def.ty;
            if (!ity || !ity->class_region) continue;
            d = lookup_in_scope(ity->class_region, m->loc, m->len);
        }
    }
    if (d && d->type)
        n->resolved_type = subst_member_type_with_class_args(
            s, d->type, call_site_ty);
    if (n->member.obj && n->member.obj->is_type_dependent)
        n->is_type_dependent = true;
}

/* Look up a class operator method by token-suffix string (e.g. "+",
 * "-", "==", "!=") and return its declared return type — N4659
 * §16.5 [over.oper]. Used by visit_binary so that 'a op b' on
 * struct-typed operands gets a usable resolved_type — needed for
 * chained expressions like
 *   (a - b).method()
 * where the inner '.method()' lookup needs the LHS resolved. The
 * matcher requires the suffix to terminate at the next non-operator
 * character (paren/space/null) so that "+" doesn't match "+=". */
static Type *find_class_operator_return_type(Sema *s, Type *class_ty,
                                              const char *op, int op_len) {
    if (!class_ty || !op) return NULL;
    Node *cd = class_ty->class_def;
    if (!cd && s && s->tu) {
        Node *d = find_class_def_in_tu(s->tu, class_ty);
        if (d) cd = d;
    }
    if (!cd) return NULL;
    for (int i = 0; i < cd->class_def.nmembers; i++) {
        Node *m = cd->class_def.members[i];
        if (!m) continue;
        Token *mn = NULL; Type *ret = NULL;
        if (m->kind == ND_FUNC_DEF) {
            mn = m->func.name; ret = m->func.ret_ty;
        } else if (m->kind == ND_VAR_DECL && m->var_decl.ty &&
                   m->var_decl.ty->kind == TY_FUNC) {
            mn = m->var_decl.name; ret = m->var_decl.ty->ret;
        }
        if (!mn || mn->kind != TK_KW_OPERATOR) continue;
        const char *after = mn->loc + mn->len;
        while (*after == ' ' || *after == '\t') after++;
        if (memcmp(after, op, op_len) != 0) continue;
        char nc = after[op_len];
        if (nc == '(' || nc == ' ' || nc == '\t' || nc == '\0')
            return ret;
    }
    return NULL;
}

/* Map a binary-operator TokenKind to its operator-name suffix
 * (e.g. TK_PLUS → "+"). Mirrors the codegen-side
 * binop_to_operator_suffix mapping but produces the source-form
 * string used in operator-method names. */
static const char *binop_token_to_op_str(TokenKind op, int *out_len) {
    switch (op) {
    case TK_PLUS:  *out_len = 1; return "+";
    case TK_MINUS: *out_len = 1; return "-";
    case TK_STAR:  *out_len = 1; return "*";
    case TK_SLASH: *out_len = 1; return "/";
    case TK_PERCENT: *out_len = 1; return "%";
    case TK_AMP:   *out_len = 1; return "&";
    case TK_PIPE:  *out_len = 1; return "|";
    case TK_CARET: *out_len = 1; return "^";
    case TK_SHL:   *out_len = 2; return "<<";
    case TK_SHR:   *out_len = 2; return ">>";
    default:       *out_len = 0; return NULL;
    }
}

/* Find a class member whose name is 'operator' and whose operator-
 * suffix matches '[]' — i.e. the operator[] member (N4659 §16.5.5
 * [over.sub]). Returns the method node (ND_FUNC_DEF or ND_VAR_DECL
 * with TY_FUNC) or NULL. Linear scan of the class's member list.
 * Falls back through the TU when class_ty->class_def isn't hooked
 * up on this Type* copy. */
/* Walk a class_def's members for the operator[] entry. Inline helper
 * factored out so the partial-spec fallback below can reuse it. */
static Node *scan_op_subscript(Node *cd) {
    if (!cd) return NULL;
    for (int i = 0; i < cd->class_def.nmembers; i++) {
        Node *m = cd->class_def.members[i];
        if (!m) continue;
        Token *mn = NULL;
        if (m->kind == ND_FUNC_DEF) mn = m->func.name;
        else if (m->kind == ND_VAR_DECL && m->var_decl.ty &&
                 m->var_decl.ty->kind == TY_FUNC)
            mn = m->var_decl.name;
        if (!mn || mn->kind != TK_KW_OPERATOR) continue;
        const char *after = mn->loc + mn->len;
        while (*after == ' ' || *after == '\t') after++;
        if (after[0] == '[') return m;
    }
    return NULL;
}

static Node *find_class_operator_subscript(Sema *s, Type *class_ty) {
    if (!class_ty) return NULL;
    Node *cd = class_ty->class_def;
    if (!cd && s && s->tu) {
        Node *d = find_class_def_in_tu(s->tu, class_ty);
        if (d) cd = d;
    }
    Node *m = scan_op_subscript(cd);
    if (m) return m;
    /* Class-template instance fallback — same shape as visit_member.
     * The canonical class_def may be the empty primary; the operator[]
     * lives on a partial spec (e.g. vec<T,A,vl_ptr>). Walk every
     * specialization with the same tag and check each. N4659 §17.8.3.2
     * [temp.class.spec.match]. */
    if (class_ty->tag && s && s->cur_scope) {
        Declaration *cands[16];
        int n_cands = lookup_overload_set_from(s->cur_scope,
            class_ty->tag->loc, class_ty->tag->len, cands, 16);
        for (int i = 0; i < n_cands; i++) {
            Declaration *c = cands[i];
            if (!c || c->entity != ENTITY_TEMPLATE ||
                !c->tmpl_node ||
                c->tmpl_node->kind != ND_TEMPLATE_DECL)
                continue;
            Node *inner = c->tmpl_node->template_decl.decl;
            if (!inner || inner->kind != ND_CLASS_DEF) continue;
            m = scan_op_subscript(inner);
            if (m) return m;
        }
    }
    return NULL;
}

static void visit_subscript(Sema *s, Node *n) {
    visit(s, n->subscript.base);
    visit(s, n->subscript.index);
    /* arr[i] / p[i] — element type. */
    Type *bt = n->subscript.base->resolved_type;
    if (bt && (bt->kind == TY_REF || bt->kind == TY_RVALREF) && bt->base)
        bt = bt->base;
    if (bt && (bt->kind == TY_ARRAY || bt->kind == TY_PTR) && bt->base)
        n->resolved_type = bt->base;
    /* Class-type subscript: dispatch through operator[] — N4659 §16.5
     * [over.oper]. Use the method's return type so downstream
     * callers (e.g. free-function-overload mangling at the call site
     * wrapping this subscript) see a concrete type rather than NULL.
     *
     * Strip the outer TY_REF if the method returns a reference — the
     * 'value category' downstream code expects from 'v[i]' is the
     * element type T, not T&. Returning TY_REF here would make
     * member access 'v[i].foo' fail downstream (codegen treats the
     * value as a struct rather than a dereffed ref). */
    if (!n->resolved_type && bt &&
        (bt->kind == TY_STRUCT || bt->kind == TY_UNION)) {
        Node *m = find_class_operator_subscript(s, bt);
        if (m) {
            Type *ret = NULL;
            if (m->kind == ND_FUNC_DEF) ret = m->func.ret_ty;
            else if (m->kind == ND_VAR_DECL && m->var_decl.ty)
                ret = m->var_decl.ty->ret;
            /* If the looked-up operator[] came from a class-template
             * partial spec, the return type carries TY_DEPENDENT in
             * the spec's pattern variables. Substitute the call-site
             * template-id args so the result is concrete (T& → C& →
             * C after the ref-strip). Otherwise downstream `.field`
             * access on the subscript result silently NULLs and any
             * free function template called with that result emits
             * bare. */
            if (ret) ret = subst_member_type_with_class_args(s, ret, bt);
            if (ret && (ret->kind == TY_REF || ret->kind == TY_RVALREF) &&
                ret->base)
                ret = ret->base;
            if (ret) n->resolved_type = ret;
        }
    }
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
    ICS_QUAL_CONV    = 1,  /* qualification conversion — N4659 §7.5
                              [conv.qual]: T* → const T* (etc.) is
                              implicit but ranks below an exact match
                              per §16.3.3.2 [over.ics.rank]. */
    ICS_PTR_SAME_TAG = 2,  /* T* ↔ T* where T's tag matches (same classes) */
    ICS_INTEGER_CONV = 3,  /* int↔long, signed↔unsigned, etc. — N4659
                              §7.8 [conv.integral] standard conversion */
    ICS_FLOATING_CONV= 3,  /* int↔float / float↔float — N4659 §7.9
                              [conv.fpint] + §7.10 [conv.double].
                              Same tier as integer; both are
                              "Conversion" rank in §16.3.3.2. */
    ICS_COMPLEX_CONV = 4,  /* real → _Complex T — gcc extension. C++
                              treats this as a worse conversion than
                              real-to-real per §16.3.3.2 tiebreakers
                              (matches gcc's PR c++/31780 ranking). */
    ICS_INCOMPATIBLE = 100,
};

static int ics_rank(Type *param, Type *arg) {
    if (!param) return ICS_INCOMPATIBLE;
    /* Argument with no resolved_type — sema didn't compute it (the
     * arithmetic-expression typing path has gaps; e.g. a call's
     * second argument written as 'a * b + (T)c' may come through
     * with a NULL resolved_type). Treat as a wildcard EXACT for
     * this slot so the resolver doesn't drop ALL viable candidates
     * and fall back to whichever overload the parser happened to
     * register first. The other args' ICS scores still discriminate
     * between candidates; only the wildcard slot doesn't contribute.
     *
     * Concrete shape:
     *   bitmap_set_bit(subregs_of_mode, regno*NUM_MACHINE_MODES + (unsigned)mode)
     * where bitmap_set_bit has overloads on sbitmap* and bitmap*.
     * The 2nd arg's resolved_type was NULL, ics_rank returned
     * INCOMPATIBLE for both bitmap_set_bit overloads (sbitmap and
     * bitmap), the picker returned NULL, and codegen fell back to
     * the parser's initial resolved_decl — the sbitmap overload —
     * so the call mangled against the wrong signature: 'void' return
     * type (sbitmap version) used in 'if (...)' context, "void value
     * not ignored as it ought to be". The bitmap overload IS the
     * right pick: arg[0] (subregs_of_mode, bitmap_head_def*) matches
     * its first param exactly, ruling out the sbitmap overload via
     * tag mismatch on arg[0]. */
    if (!arg) return ICS_EXACT;
    /* Reference binding — N4659 §16.3.3.1.4 [over.ics.ref]: the ICS of
     * an arg to a reference parameter is the conversion sequence to
     * the referent type. Strip TY_REF/TY_RVALREF on both sides and
     * re-rank against the referent types. Stripping only the param
     * side fails when sema's resolved_type for an lvalue arg also
     * carries a TY_REF (passing 'T &v' as 'v') — types_equivalent
     * sees PTR vs REF, returns false, and the cand drops. */
    if ((param->kind == TY_REF || param->kind == TY_RVALREF) ||
        (arg->kind == TY_REF || arg->kind == TY_RVALREF)) {
        Type *p = (param->kind == TY_REF || param->kind == TY_RVALREF)
                  ? param->base : param;
        Type *a = (arg->kind == TY_REF || arg->kind == TY_RVALREF)
                  ? arg->base : arg;
        if (!p || !a) return ICS_INCOMPATIBLE;
        return ics_rank(p, a);
    }
    /* Complex-vs-real mismatch must be tested BEFORE types_equivalent
     * (which treats `_Complex int` and `int` as the same type — its
     * job is structural equivalence, not overload ranking). Per
     * §16.3.3.2 plus gcc's PR c++/31780 ranking, real→complex is one
     * tier worse than real→real, so f(double) wins over f(_Complex
     * int) for an int arg. */
    if (param->is_complex != arg->is_complex)
        return ICS_COMPLEX_CONV;
    /* Cv-only differences on fundamental types — types_equivalent's
     * default arm rejects (int, int const) because it requires
     * is_const match. For ICS purposes, after the ref-binding strip
     * above, an `int` arg to a `T const` parameter is an exact
     * match (the reference binding's qualification conversion is
     * §16.3.3.1.4/3 [over.ics.ref] "bound directly", rank Identity).
     * Same for pointer levels: `int*` to `int const*` is a
     * qualification conversion (§7.5 [conv.qual]). N4659
     * §16.3.3.2.1 [over.ics.scs] ranks both as Identity for
     * best-viable purposes; differentiated only against an exact
     * non-cv match via tie-break rules not yet modelled. */
    if (param->kind == arg->kind &&
        param->is_unsigned == arg->is_unsigned &&
        param->kind != TY_STRUCT && param->kind != TY_UNION &&
        param->kind != TY_ENUM && param->kind != TY_PTR &&
        param->kind != TY_REF && param->kind != TY_RVALREF &&
        param->kind != TY_ARRAY && param->kind != TY_FUNC)
        return ICS_EXACT;
    /* Pointer pair whose bases differ only in cv-qualifiers — N4659
     * §7.5 [conv.qual] / §16.3.3.2.1 [over.ics.scs]: T* → T const*
     * is a qualification conversion (rank QUAL_CONV). types_
     * equivalent's recursive base check rejects on the cv mismatch,
     * so we re-check by structurally stripping cv at the base
     * level. Same-direction only (add const to pointee, not
     * remove). */
    if (param->kind == TY_PTR && arg->kind == TY_PTR &&
        param->base && arg->base &&
        param->base->kind == arg->base->kind &&
        param->base->is_unsigned == arg->base->is_unsigned) {
        /* §7.5 [conv.qual]: only T* → const T* qualifies — the pointee
         * type (sans cv) must match. For aggregates that means same
         * tag; without this check, `bitmap_head_def *` and
         * `simple_bitmap_def *` both score QUAL_CONV against each
         * other (kind=TY_STRUCT matches on both) and ties drop into
         * "pick first viable", so overloaded `bitmap_empty_p` on
         * both struct pointers always picked the first-declared
         * overload regardless of the argument's actual struct. */
        bool tag_ok = true;
        Type *pb = param->base;
        Type *ab = arg->base;
        if (pb->kind == TY_STRUCT || pb->kind == TY_UNION ||
            pb->kind == TY_ENUM) {
            tag_ok = pb->tag && ab->tag && tokens_equal(pb->tag, ab->tag);
        }
        if (tag_ok) {
            bool p_const = pb->is_const;
            bool a_const = ab->is_const;
            if (p_const && !a_const) return ICS_QUAL_CONV;
            if (!p_const && a_const) return ICS_INCOMPATIBLE;
            if (p_const == a_const) {
                /* Same cv but types_equivalent failed → kind diff
                 * at base (e.g. int* vs long*). Falls through to
                 * integer-conv at the bottom; let it. */
            }
        }
    }
    if (types_equivalent(param, arg)) {
        /* types_equivalent ignores cv-qualifiers on the pointee
         * (TY_STRUCT / fundamental types compare by tag/kind only).
         * Per N4659 §7.5 [conv.qual] / §16.3.3.2 [over.ics.rank], a
         * pointer-to-T → pointer-to-const-T is a qualification
         * conversion — implicit but ranked below an exact match;
         * the reverse direction is *not* implicit. Distinguish
         * those four cases at the pointee level so two overloads
         * differing only in const-qualification on the pointee
         * (e.g. `f(T*)` vs `f(const T*)`) get separate scores when
         * the call passes a `T*`. */
        if ((param->kind == TY_PTR || param->kind == TY_REF ||
             param->kind == TY_RVALREF) &&
            param->base && arg->base) {
            bool p_const = param->base->is_const;
            bool a_const = arg->base->is_const;
            if (p_const && !a_const) return ICS_QUAL_CONV;
            if (!p_const && a_const) return ICS_INCOMPATIBLE;
        }
        return ICS_EXACT;
    }
    /* Null pointer constant — N4659 §4.10/1 [conv.ptr]: a null
     * pointer constant converts to ANY pointer type. We model nullptr
     * / NULL / __null as TY_PTR(TY_VOID) in visit_nullptr; treat that
     * as compatible with any TY_PTR parameter so overload resolution
     * doesn't reject candidates merely because the call passed NULL
     * instead of a typed pointer. Without this, calls like
     *   f(parser, false, true, NULL)
     * would fail to find a viable candidate and fall back to the
     * historical first-found resolved_decl (which may be the wrong
     * arity overload). */
    if (param->kind == TY_PTR && arg->kind == TY_PTR && arg->base &&
        arg->base->kind == TY_VOID)
        return ICS_PTR_SAME_TAG;  /* null → any pointer */
    /* Any T* → void* — C++ §4.10/2 [conv.ptr] allows the implicit
     * conversion. ICS_QUAL_CONV ranks it BELOW an exact same-type
     * pointer match so overload resolution prefers a more
     * specific overload when both are viable. Pattern:
     * g++.dg/init/placement2.C — `new (heap) A;` where heap is
     * char[N], decays to char*, and the user's `operator new
     * (size_t, void*)` takes void*. */
    if (param->kind == TY_PTR && param->base &&
        param->base->kind == TY_VOID &&
        arg->kind == TY_PTR && arg->base)
        return ICS_QUAL_CONV;
    /* Pointer-to-same-tag: T* vs T* where both Ts are class types
     * with matching tag but distinct Type* identity. Catches the
     * common case where two free-function overloads differ only in
     * their struct-pointer parameter type (e.g.
     * dump_bitmap(bitmap_head_def*) vs dump_bitmap(simple_bitmap_def*))
     * — at the call site we want the overload whose pointee tag
     * matches the argument's pointee tag. */
    if (param->kind == TY_PTR && arg->kind == TY_PTR &&
        param->base && arg->base) {
        Type *pb = param->base;
        Type *ab = arg->base;
        if ((pb->kind == TY_STRUCT || pb->kind == TY_UNION) &&
            (ab->kind == TY_STRUCT || ab->kind == TY_UNION) &&
            pb->tag && ab->tag && tokens_equal(pb->tag, ab->tag))
            return ICS_PTR_SAME_TAG;
    }
    /* Integer / floating / complex conversion — N4659 §7.8, §7.9,
     * §7.10. Any integral converts to any other, any floating to
     * any other floating, integer↔floating, and integer↔_Complex
     * (gcc extension). Complex is ranked one tier worse than
     * floating per gcc's PR c++/31780 — so f(double) wins over
     * f(_Complex int) when called with an int. */
    {
        bool p_int = param->kind == TY_BOOL || param->kind == TY_CHAR ||
            param->kind == TY_CHAR16 || param->kind == TY_CHAR32 ||
            param->kind == TY_WCHAR || param->kind == TY_SHORT ||
            param->kind == TY_INT || param->kind == TY_LONG ||
            param->kind == TY_LLONG || param->kind == TY_ENUM;
        bool a_int = arg->kind == TY_BOOL || arg->kind == TY_CHAR ||
            arg->kind == TY_CHAR16 || arg->kind == TY_CHAR32 ||
            arg->kind == TY_WCHAR || arg->kind == TY_SHORT ||
            arg->kind == TY_INT || arg->kind == TY_LONG ||
            arg->kind == TY_LLONG || arg->kind == TY_ENUM;
        bool p_float = param->kind == TY_FLOAT || param->kind == TY_DOUBLE ||
                       param->kind == TY_LDOUBLE;
        bool a_float = arg->kind == TY_FLOAT || arg->kind == TY_DOUBLE ||
                       arg->kind == TY_LDOUBLE;
        bool p_complex = param->is_complex;
        bool a_complex = arg->is_complex;
        if ((p_complex || a_complex) && (p_int || p_float || a_int || a_float))
            return ICS_COMPLEX_CONV;
        if ((p_float || p_int) && (a_float || a_int))
            return p_int && a_int ? ICS_INTEGER_CONV : ICS_FLOATING_CONV;
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

/* For each candidate we reduce to a concrete function signature before
 * running ICS: non-template candidates use their declared TY_FUNC as-
 * is; template candidates get their parameters deduced against the
 * call's arg types (§17.8.2 [temp.deduct]) and then substituted, so
 * ICS runs against concrete param types. The 'is_template' flag is
 * carried through for the §16.3.3 tiebreaker (non-template beats
 * equally-ranked template). The deduced SubstMap is carried so the
 * caller can build a synthetic ND_TEMPLATE_ID (driving instantiation)
 * when a template is the winner. */
typedef struct {
    Declaration *decl;
    Type       **params;          /* effective (post-substitution) types */
    Type       **pattern_params;  /* pre-substitution; used for partial
                                   * ordering of templates per N4659
                                   * §17.5.5.2 [temp.func.order]. NULL
                                   * for non-template cands. */
    int          nparams;
    bool         is_variadic;
    bool         is_template;
    SubstMap     deduced;  /* only valid when is_template */
} ViableCand;

/* Extract the inner function node from a template declaration.
 * Returns NULL if the template doesn't wrap a function (class
 * template etc.) or the node shape is unexpected. */
static Node *tmpl_inner_func(Node *tmpl) {
    if (!tmpl || tmpl->kind != ND_TEMPLATE_DECL) return NULL;
    Node *d = tmpl->template_decl.decl;
    if (!d) return NULL;
    /* Unwrap double template (member templates): template<T>
     * template<U> — the outer template_decl wraps another
     * template_decl. Peel to reach the func. */
    while (d && d->kind == ND_TEMPLATE_DECL)
        d = d->template_decl.decl;
    if (d && (d->kind == ND_FUNC_DEF || d->kind == ND_FUNC_DECL))
        return d;
    return NULL;
}

/* Structural specialization comparison for partial ordering of
 * function templates per N4659 §17.5.5.2 [temp.func.order].
 *
 * Returns:
 *   +1 if A's pattern is strictly more specialized than B's
 *   -1 if B is strictly more specialized than A
 *    0 if equal generality (same shape down to dependent leaves)
 *   -2 if A and B are INCOMPARABLE (concrete shapes that disagree
 *      and neither contains a dependent) — patterns accept disjoint
 *      sets of types, so no specialization ordering applies
 *
 * Heuristic: compound (PTR/REF/ARRAY/STRUCT) is more specialized
 * than a bare TY_DEPENDENT at the same position; compound-vs-
 * compound of matching kind recurses; concrete-vs-concrete with
 * disagreement is INCOMPARABLE. Doesn't fully implement the
 * standard's transform-and-deduce algorithm but captures the
 * common patterns.
 *
 * TODO(seafront#partial-order-fully): full §17.5.5.2 algorithm with
 * synthesized fresh types and pairwise deduction.
 */
enum { SPEC_CMP_INCOMPARABLE = -2 };

static int type_specialization_compare(Type *a, Type *b) {
    if (!a || !b) return 0;
    if (a == b) return 0;
    /* Strip references — partial ordering rule strips top-level refs
     * (§17.5.5.2/2). */
    if (a->kind == TY_REF || a->kind == TY_RVALREF) a = a->base;
    if (b->kind == TY_REF || b->kind == TY_RVALREF) b = b->base;
    if (!a || !b) return 0;
    if (a->kind == TY_DEPENDENT && b->kind == TY_DEPENDENT) return 0;
    if (a->kind == TY_DEPENDENT) return -1;
    if (b->kind == TY_DEPENDENT) return +1;
    if (a->kind != b->kind) return SPEC_CMP_INCOMPARABLE;
    switch (a->kind) {
    case TY_PTR: case TY_ARRAY:
        return type_specialization_compare(a->base, b->base);
    case TY_STRUCT: case TY_UNION:
        /* Different tags = different concrete classes = incomparable. */
        if (a->tag && b->tag) {
            if (a->tag->len != b->tag->len ||
                memcmp(a->tag->loc, b->tag->loc, a->tag->len) != 0)
                return SPEC_CMP_INCOMPARABLE;
        }
        if (a->n_template_args != b->n_template_args) return 0;
        if (a->n_template_args == 0) return 0;
        {
            int saw_pos = 0, saw_neg = 0, saw_inc = 0;
            for (int i = 0; i < a->n_template_args; i++) {
                int c = type_specialization_compare(
                    a->template_args[i], b->template_args[i]);
                if (c == SPEC_CMP_INCOMPARABLE) saw_inc = 1;
                else if (c > 0) saw_pos++;
                else if (c < 0) saw_neg++;
            }
            if (saw_inc) return SPEC_CMP_INCOMPARABLE;
            if (saw_pos && !saw_neg) return +1;
            if (saw_neg && !saw_pos) return -1;
            return 0;
        }
    case TY_FUNC:
        return 0;
    default:
        return 0;
    }
}

/* Pick the best-viable overload. On return, *out_deduced is set to
 * the SubstMap deduced against the winner IF the winner is a
 * template (so the caller can build the ND_TEMPLATE_ID to drive
 * instantiation); otherwise out_deduced is left untouched. */
/* Bind explicit template-args from a `name<args>(...)` call into the
 * SubstMap so subsequent deduction sees them as already-bound. Each
 * arg is a Node carrying a Type (for typename params) or a literal
 * (for NTTPs). This mirrors subst_map_bind_args but tolerates a
 * count smaller than the template's nparams (the remaining params
 * are deduced from call args).  N4659 §17.8.1 [temp.arg.explicit]. */
static void seed_explicit_targs(SubstMap *map,
                                 Node **tparams, int ntparams,
                                 Node **targs, int ntargs) {
    int n = ntargs < ntparams ? ntargs : ntparams;
    if (n <= 0) return;
    subst_map_bind_args(map, tparams, n, targs, n);
}

static Declaration *resolve_free_function_overload_with_explicit(
        Declaration **cands, int ncands,
        Type **arg_types, int nargs,
        Node **explicit_targs, int n_explicit_targs,
        Arena *arena,
        SubstMap *out_deduced, bool *out_is_template);

static Declaration *resolve_free_function_overload(
        Declaration **cands, int ncands,
        Type **arg_types, int nargs,
        Arena *arena,
        SubstMap *out_deduced, bool *out_is_template) {
    return resolve_free_function_overload_with_explicit(
        cands, ncands, arg_types, nargs, NULL, 0,
        arena, out_deduced, out_is_template);
}

static Declaration *resolve_free_function_overload_with_explicit(
        Declaration **cands, int ncands,
        Type **arg_types, int nargs,
        Node **explicit_targs, int n_explicit_targs,
        Arena *arena,
        SubstMap *out_deduced, bool *out_is_template) {
    if (out_is_template) *out_is_template = false;
    if (ncands <= 1) return ncands == 1 ? cands[0] : NULL;

    ViableCand *viable = arena_alloc(arena, ncands * sizeof(ViableCand));
    /* ranks indexed as ranks[i * nargs + j] (cand × arg position). */
    int rstride = nargs > 0 ? nargs : 1;
    int *ranks  = arena_alloc(arena, ncands * rstride * sizeof(int));
    int nv = 0;
    for (int i = 0; i < ncands; i++) {
        Declaration *c = cands[i];
        if (!c) continue;
        ViableCand vc = {0};
        vc.decl = c;
        /* Populate effective params list depending on kind. */
        if (c->type && c->type->kind == TY_FUNC) {
            vc.params      = c->type->params;
            vc.nparams     = c->type->nparams;
            vc.is_variadic = c->type->is_variadic;
            vc.is_template = false;
        } else if (c->entity == ENTITY_TEMPLATE && c->tmpl_node) {
            Node *inner = tmpl_inner_func(c->tmpl_node);
            if (!inner) continue;
            int np = inner->func.nparams;
            /* Template argument deduction against the call args —
             * §17.8.2.1 [temp.deduct.call]. SubstMap capacity is the
             * number of TEMPLATE parameters (not function params),
             * since each binding holds one template-arg→type mapping.
             * Sizing by func.nparams silently drops bindings beyond
             * the function arity — e.g. 1-arg foo<T,A>(vec<T,A>*)
             * needs two slots (T, A) but np=1 left only one, so 'A'
             * never bound and build_template_id_from_deduced returned
             * NULL, leaving the call unmangled. */
            int ntp = c->tmpl_node->template_decl.nparams;
            int cap = ntp > np ? ntp : np;
            if (cap < 1) cap = 1;
            SubstMap map = subst_map_new(arena, cap);
            /* Pre-seed with explicit template-args from `f<T1,T2>(args)`
             * so deduce_template_args sees them already-bound. Required
             * for candidates whose template-params don't appear in the
             * function-param list — without the seed, deduction adds
             * zero entries and the candidate is dropped, leaving an
             * over-general sibling to win by default. Pattern:
             * g++.dg/template/spec21.C `f<int>(1)` with `T f(int)` vs
             * `T f(U)` — T is explicit, A's param has nothing to
             * deduce from. */
            if (explicit_targs && n_explicit_targs > 0)
                seed_explicit_targs(&map,
                                     c->tmpl_node->template_decl.params,
                                     ntp,
                                     explicit_targs, n_explicit_targs);
            Type **pp = NULL;
            if (np > 0) {
                pp = arena_alloc(arena, np * sizeof(Type *));
                for (int k = 0; k < np; k++)
                    pp[k] = inner->func.params[k]->param.ty;
            }
            /* N4659 §17.8.2 [temp.deduct]/4: deduction failure removes
             * the template from the candidate set. Honour the deduction
             * return value — when it returns false (no bindings made,
             * or a per-pair unification failed), drop this candidate
             * before it can pollute viability via an empty SubstMap.
             * Without this, candidates whose template args couldn't
             * bind (e.g. arity mismatch on inner template-args:
             * 'vec<T,A,vl_embed>*' vs 'vec<X,Y>*') would become
             * "viable" with no bindings, win resolution by falling
             * through to ICS_PTR_SAME_TAG, then skip the rewrite to
             * template-id (build_template_id_from_deduced bails on
             * the unbound T) — the emitted call mangles bare and
             * misses the actual instantiation's symbol. */
            if (!deduce_template_args(inner, arg_types, nargs, &map))
                continue;
            /* Substitute the deduced bindings into the param types. */
            Type **eff = pp;
            if (np > 0 && map.nentries > 0) {
                eff = arena_alloc(arena, np * sizeof(Type *));
                for (int k = 0; k < np; k++)
                    eff[k] = subst_type(pp[k], &map, arena);
            }
            vc.params         = eff;
            vc.pattern_params = pp;   /* keep originals for §17.5.5.2 */
            vc.nparams        = np;
            vc.is_variadic    = inner->func.is_variadic;
            vc.is_template    = true;
            vc.deduced        = map;
        } else {
            continue;
        }
        /* Arity filter — §16.3.2/2. Variadic pass-through for
         * '...' handled by nargs >= nparams; strict match otherwise.
         * TODO(seafront#over-defaults): admit nargs < nparams when
         * trailing params have default-arg annotations. */
        bool arity_ok = vc.is_variadic ? nargs >= vc.nparams
                                       : nargs == vc.nparams;
        if (!arity_ok) continue;
        bool ok = true;
        for (int j = 0; j < nargs && ok; j++) {
            int r;
            if (j >= vc.nparams) {
                r = ICS_EXACT;  /* variadic slot — see non-template branch */
            } else {
                r = ics_rank(vc.params[j], arg_types[j]);
            }
            if (r >= ICS_INCOMPATIBLE) ok = false;
            ranks[nv * rstride + j] = r;
        }
        if (ok) viable[nv++] = vc;
    }
    if (nv == 0) return NULL;
    if (nv == 1) {
        if (viable[0].is_template) {
            if (out_is_template) *out_is_template = true;
            if (out_deduced) *out_deduced = viable[0].deduced;
        }
        return viable[0].decl;
    }

    /* Pick best viable. */
    /* N4659 §17.5.5.2 [temp.func.order] — partial ordering of function
     * templates. When two viable cands are both templates, the one
     * whose pattern is "more specialized" wins; the more general one
     * is dropped. This prevents the primary 'gt_pch_nx<T,A>(vec<T,A,
     * vl_embed>*)' from being picked when the partial spec
     * 'gt_pch_nx<T,A>(vec<T*,A,vl_embed>*)' is also viable for the
     * same call — the standard mandates the more-specialized form.
     *
     * Heuristic implementation: walk paired param patterns
     * structurally; at each TY_DEPENDENT-vs-compound divergence, the
     * compound side is more specialized; at compound-vs-compound,
     * recurse. Doesn't fully implement the standard's transform-and-
     * deduce-against-other algorithm (no synthesized fresh types),
     * but covers the common patterns the standard handles. */
    /* spec_order indexed as spec_order[i * nv + j]. */
    int *spec_order = arena_alloc(arena, nv * nv * sizeof(int));
    memset(spec_order, 0, nv * nv * sizeof(int));
    /* spec_order[i][j] == 1 iff i is strictly more specialized than j */
    for (int i = 0; i < nv; i++) {
        for (int j = 0; j < nv; j++) {
            if (i == j) continue;
            if (!viable[i].is_template || !viable[j].is_template) continue;
            int saw_strict = 0;
            int j_better = 0;
            int incomparable = 0;
            /* Compare PRE-substitution patterns; post-substitution
             * both cands collapse to the same concrete type and
             * structural specialization is invisible. */
            Type **pi = viable[i].pattern_params;
            Type **pj = viable[j].pattern_params;
            if (!pi || !pj) continue;
            for (int k = 0; k < nargs && !incomparable; k++) {
                int cmp = type_specialization_compare(pi[k], pj[k]);
                if (cmp == SPEC_CMP_INCOMPARABLE) incomparable = 1;
                else if (cmp > 0) saw_strict = 1;
                else if (cmp < 0) j_better = 1;
            }
            if (!incomparable && saw_strict && !j_better)
                spec_order[i * nv + j] = 1;
        }
    }
    /* Drop any cand strictly less specialized than another viable.
     * Mark vc.is_template=false won't work — use a separate kept[] mask. */
    bool *kept = arena_alloc(arena, nv * sizeof(bool));
    for (int i = 0; i < nv; i++) {
        kept[i] = true;
        for (int j = 0; j < nv; j++) {
            if (spec_order[j * nv + i]) { kept[i] = false; break; }
        }
    }

    for (int i = 0; i < nv; i++) {
        if (!kept[i]) continue;  /* dropped by partial-order pruning */
        bool is_best = true;
        for (int j = 0; j < nv && is_best; j++) {
            if (i == j || !kept[j]) continue;
            bool le_all = true;
            bool lt_any = false;
            for (int k = 0; k < nargs; k++) {
                if (ranks[i * rstride + k] > ranks[j * rstride + k]) { le_all = false; break; }
                if (ranks[i * rstride + k] < ranks[j * rstride + k]) lt_any = true;
            }
            /* §16.3.3/1 final bullet: a non-template F beats an
             * equally-ranked template G. Apply as an extra 'better'
             * dimension. */
            if (le_all && !lt_any) {
                if (!viable[i].is_template && viable[j].is_template)
                    lt_any = true;  /* i wins the tiebreak */
            }
            if (!le_all || !lt_any) is_best = false;
        }
        if (is_best) {
            if (viable[i].is_template) {
                if (out_is_template) *out_is_template = true;
                if (out_deduced) *out_deduced = viable[i].deduced;
            }
            return viable[i].decl;
        }
    }
    /* Tied — but in C/C++ multiple declarations of the SAME function
     * (forward decl + definition + re-declaration) coexist in scope
     * and produce identical viable candidates. Don't return NULL
     * (which leaves the call site mangling against an arbitrary
     * other overload via the historical first-found resolved_decl).
     * Pick the first viable: any of them is a valid resolution.
     *
     * Critical: also propagate is_template + deduced when picking
     * via this tiebreak. Without it, vec_free<T,A> calls (where
     * both vec_free overloads are ENTITY_TEMPLATE and may be tied
     * after deduction) returned the right Decl but with
     * out_is_template=false → visit_call skipped the
     * ND_TEMPLATE_ID rewrite → the instantiation pass never saw
     * the call → the template was never instantiated → cc1plus
     * link missed 49 vec_free symbols. */
    if (viable[0].is_template) {
        if (out_is_template) *out_is_template = true;
        if (out_deduced) *out_deduced = viable[0].deduced;
    }
    return viable[0].decl;
}

/* Build an ND_TEMPLATE_ID node for a function template call whose
 * template parameters have been deduced into `deduced`. Returns NULL
 * if any template parameter remains unbound (we don't synthesize a
 * partial template-id — the instantiation pass would produce broken
 * mangled names like 'foo_t___te_'). N4659 §17.8.1 [temp.inst].
 *
 * Once the callee becomes ND_TEMPLATE_ID, the instantiation pass
 * scans ND_CALL for ND_TEMPLATE_ID callees, creates an InstRequest,
 * clones the template with the substitution, and rewrites the callee
 * again to an ND_IDENT pointing at the mangled instantiation.
 *
 * TODO(seafront#over-defaults): once default template args are
 * threaded through, unbound-then-default would be fine; for now the
 * caller bails when any param is unbound. */
static Node *build_template_id_from_deduced(Sema *s, Token *tname,
                                             Token *call_tok, Node *tmpl,
                                             SubstMap *deduced) {
    if (!tmpl || tmpl->kind != ND_TEMPLATE_DECL) return NULL;
    int ntp = tmpl->template_decl.nparams;
    if (ntp <= 0) return NULL;
    /* Pre-compute total args after pack expansion: each non-pack
     * param contributes 1 slot; the (single) pack param contributes
     * N slots from its deduced binding. N4659 §17.5.3 [temp.variadic]. */
    int total = 0;
    for (int k = 0; k < ntp; k++) {
        Node *tp = tmpl->template_decl.params[k];
        Token *pname = tp ? tp->param.name : NULL;
        bool counted = false;
        if (tp && tp->param.is_pack && pname) {
            for (int e = 0; e < deduced->nentries; e++) {
                Token *en = deduced->entries[e].param_name;
                if (en && tokens_equal(en, pname)) {
                    total += deduced->entries[e].is_pack
                               ? deduced->entries[e].pack_ntypes
                               : 1;  /* single-arg fallback */
                    counted = true;
                    break;
                }
            }
        }
        if (!counted) total++;
    }
    Node **tid_args = arena_alloc(s->arena, total * sizeof(Node *));
    int ai = 0;
    for (int k = 0; k < ntp; k++) {
        Node *tp = tmpl->template_decl.params[k];
        Token *pname = tp ? tp->param.name : NULL;
        if (tp && tp->param.is_pack && pname) {
            bool expanded = false;
            for (int e = 0; e < deduced->nentries; e++) {
                if (!deduced->entries[e].is_pack) continue;
                Token *en = deduced->entries[e].param_name;
                if (en && tokens_equal(en, pname)) {
                    for (int j = 0; j < deduced->entries[e].pack_ntypes; j++) {
                        Node *arg = arena_alloc(s->arena, sizeof(Node));
                        memset(arg, 0, sizeof(Node));
                        arg->kind = ND_VAR_DECL;
                        arg->var_decl.ty = deduced->entries[e].pack_types[j];
                        tid_args[ai++] = arg;
                    }
                    expanded = true;
                    break;
                }
            }
            if (expanded) continue;
            /* Single-arg fallback: deduce bound this pack as a
             * non-pack entry. Emit one type-arg from that entry. */
            for (int e = 0; e < deduced->nentries; e++) {
                if (deduced->entries[e].is_pack) continue;
                Token *en = deduced->entries[e].param_name;
                if (en && tokens_equal(en, pname) &&
                    deduced->entries[e].concrete_type) {
                    Node *arg = arena_alloc(s->arena, sizeof(Node));
                    memset(arg, 0, sizeof(Node));
                    arg->kind = ND_VAR_DECL;
                    arg->var_decl.ty = deduced->entries[e].concrete_type;
                    tid_args[ai++] = arg;
                    expanded = true;
                    break;
                }
            }
            if (!expanded) return NULL;
            continue;
        }
        Type *ct = NULL;
        if (pname) {
            for (int e = 0; e < deduced->nentries; e++) {
                if (deduced->entries[e].is_pack) continue;
                Token *en = deduced->entries[e].param_name;
                if (en && tokens_equal(en, pname)) {
                    ct = deduced->entries[e].concrete_type;
                    break;
                }
            }
        }
        if (!ct) return NULL;
        Node *arg = arena_alloc(s->arena, sizeof(Node));
        memset(arg, 0, sizeof(Node));
        arg->kind = ND_VAR_DECL;
        arg->var_decl.ty = ct;
        tid_args[ai++] = arg;
    }
    Node *tid = arena_alloc(s->arena, sizeof(Node));
    memset(tid, 0, sizeof(Node));
    tid->kind = ND_TEMPLATE_ID;
    tid->tok = call_tok;
    tid->template_id.name = tname;
    tid->template_id.args = tid_args;
    tid->template_id.nargs = total;
    /* Carry the specific template the overload resolver picked, so
     * the instantiation pass uses it instead of doing a name-only
     * registry lookup that may return a different overload (real-
     * world shape: a templated container header with multiple
     * template overloads of an allocation helper). */
    tid->template_id.resolved_tmpl = tmpl;
    return tid;
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
    /* Free-function overload resolution — skip implicit-this method
     * calls (those route through the class-method resolver in
     * codegen; sema picking a different overload here would change
     * resolved_decl->home and confuse that dispatch). */
    if (n->call.callee && n->call.callee->kind == ND_IDENT &&
        !n->call.callee->ident.implicit_this &&
        n->call.callee->ident.n_overloads > 1 &&
        !n->call.callee->is_type_dependent) {
        int na = n->call.nargs;
        Type **at = na > 0 ? arena_alloc(s->arena, na * sizeof(Type *)) : NULL;
        for (int i = 0; i < na; i++)
            at[i] = n->call.args[i] ? n->call.args[i]->resolved_type : NULL;
        SubstMap deduced = {0};
        bool winner_is_template = false;
        Declaration *winner = resolve_free_function_overload(
            n->call.callee->ident.overload_set,
            n->call.callee->ident.n_overloads,
            at, na, s->arena,
            &deduced, &winner_is_template);
        if (winner) {
            n->call.callee->ident.resolved_decl = winner;
            if (winner->type)
                n->call.callee->resolved_type = winner->type;
            /* Only synthesise an ND_TEMPLATE_ID for FREE function
             * templates. The OOL definition of a class-template
             * member method is wrapped in an ND_TEMPLATE_DECL whose
             * head re-states the outer class's template params; the
             * method itself is not a free function template. If we
             * synthesise here, the call gets routed through the
             * free-fn-template instantiation path in instantiate.c,
             * which mangles the symbol as
             * 'name_t_<args>_te__p_<params>_pe_' with no class
             * scope — a member-template call inside the enclosing
             * class body would link against a non-existent free
             * function instead of the synthesised member.
             * Detect via inner func's class_type / qual_tid and
             * skip — the implicit-this method-call lowering in
             * codegen handles these correctly. N4659 §17.5.2/2
             * [temp.mem]. */
            Node *tn_inner = winner->tmpl_node ?
                winner->tmpl_node->template_decl.decl : NULL;
            bool is_ool_method = tn_inner &&
                (tn_inner->kind == ND_FUNC_DEF ||
                 tn_inner->kind == ND_FUNC_DECL) &&
                (tn_inner->func.class_type != NULL ||
                 tn_inner->func.qual_tid != NULL);
            if (winner_is_template && winner->tmpl_node && !is_ool_method) {
                Node *tid = build_template_id_from_deduced(s,
                    n->call.callee->ident.name,
                    n->call.callee->tok,
                    winner->tmpl_node, &deduced);
                if (tid) n->call.callee = tid;
            }
        }
    }
    /* Bare-ident call to a single-overload function template — N4659
     * §17.8.2.1 [temp.deduct.call]. visit_ident only populates
     * overload_set when n_overloads > 1, so single-template-candidate
     * calls (helper templates with one viable form, e.g. an
     * `alloc<T>(n)` family with one specialisation) miss the
     * multi-overload rewrite above. Run deduction directly against
     * the lone template and synthesize ND_TEMPLATE_ID so the
     * instantiation pass picks it up. Without this branch the
     * templates are never instantiated and the call sites reference
     * un-emitted symbols at link time. */
    if (n->call.callee && n->call.callee->kind == ND_IDENT &&
        !n->call.callee->ident.implicit_this &&
        n->call.callee->ident.n_overloads <= 1 &&
        !n->call.callee->is_type_dependent) {
        Declaration *d = n->call.callee->ident.resolved_decl;
        if (d && d->entity == ENTITY_TEMPLATE && d->tmpl_node) {
            Node *inner = tmpl_inner_func(d->tmpl_node);
            /* Skip OOL definitions of class-template member methods —
             * they carry the outer class's template head but are not
             * free function templates. See the matching guard above
             * in the multi-overload path. N4659 §17.5.2/2 [temp.mem]. */
            bool is_ool_method = inner &&
                (inner->kind == ND_FUNC_DEF ||
                 inner->kind == ND_FUNC_DECL) &&
                (inner->func.class_type != NULL ||
                 inner->func.qual_tid != NULL);
            if (inner && !is_ool_method) {
                int na = n->call.nargs;
                Type **at = na > 0
                    ? arena_alloc(s->arena, na * sizeof(Type *)) : NULL;
                for (int i = 0; i < na; i++)
                    at[i] = n->call.args[i]
                        ? n->call.args[i]->resolved_type : NULL;
                int ntp = d->tmpl_node->template_decl.nparams;
                SubstMap deduced = subst_map_new(s->arena,
                    ntp > 0 ? ntp : 1);
                deduce_template_args(inner, at, na, &deduced);
                Node *tid = build_template_id_from_deduced(s,
                    n->call.callee->ident.name,
                    n->call.callee->tok,
                    d->tmpl_node, &deduced);
                if (tid) n->call.callee = tid;
            }
        }
    }
    /* Explicit-template-id call 'name<args>(call-args)' — resolve the
     * template overload and stamp template_id.resolved_tmpl so the
     * instantiation pass picks the right candidate. Without this, the
     * registry-side fallback returns the FIRST entry with the matching
     * name, which can be of the wrong arity entirely (e.g. picking
     * 'foo<T,A>(Vec<T,A>*&, unsigned)' for a 3-arg call to
     * 'foo<T,A>(Vec<T,A>*, op_t, void*)'). N4659 §16.3 [over.match] +
     * §17.8.2.1 [temp.deduct.call]. */
    if (n->call.callee && n->call.callee->kind == ND_TEMPLATE_ID &&
        !n->call.callee->template_id.resolved_tmpl &&
        n->call.callee->template_id.name &&
        s->cur_scope) {
        Token *tname = n->call.callee->template_id.name;
        Vec ov = vec_new(s->arena);
        lookup_overload_set_into_vec(s->cur_scope,
                                      tname->loc, tname->len, &ov);
        if (ov.len >= 1) {
            int na = n->call.nargs;
            Type **at = na > 0
                ? arena_alloc(s->arena, na * sizeof(Type *)) : NULL;
            for (int i = 0; i < na; i++)
                at[i] = n->call.args[i] ? n->call.args[i]->resolved_type : NULL;
            SubstMap deduced = {0};
            bool winner_is_template = false;
            Declaration *winner = resolve_free_function_overload_with_explicit(
                (Declaration **)ov.data, ov.len,
                at, na,
                n->call.callee->template_id.args,
                n->call.callee->template_id.nargs,
                s->arena,
                &deduced, &winner_is_template);
            /* Skip when the winner is a class member (in-class member
             * template or OOL member method). Stamping resolved_tmpl
             * would route the call through the FREE-function template
             * path in instantiate.c, which mangles without class scope
             * and never inserts the implicit 'this'. The sibling-
             * member-template detection in instantiate.c handles those
             * shapes natively from the (still NULL) resolved_tmpl.
             * N4659 §17.5.2 [temp.mem]. */
            bool winner_is_member = false;
            if (winner) {
                if (winner->home && winner->home->kind == REGION_CLASS)
                    winner_is_member = true;
                else if (winner->tmpl_node) {
                    Node *inner = tmpl_inner_func(winner->tmpl_node);
                    if (inner &&
                        (inner->kind == ND_FUNC_DEF ||
                         inner->kind == ND_FUNC_DECL) &&
                        (inner->func.class_type != NULL ||
                         inner->func.qual_tid != NULL))
                        winner_is_member = true;
                }
            }
            if (winner && !winner_is_member &&
                winner->entity == ENTITY_TEMPLATE && winner->tmpl_node)
                n->call.callee->template_id.resolved_tmpl = winner->tmpl_node;
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
    case ND_NULLPTR:   visit_nullptr(s, n);   break;
    case ND_IDENT:     visit_ident(s, n);     break;

    /* Operators */
    case ND_BINARY:    visit_binary(s, n);    break;
    case ND_UNARY:     visit_unary(s, n);     break;
    case ND_POSTFIX:   visit_unary(s, n);     break;
    case ND_ASSIGN:    visit_assign(s, n);    break;
    case ND_TERNARY:   visit_ternary(s, n);   break;
    case ND_CAST:
        visit(s, n->cast.operand);
        /* The cast expression's type is its target — set it so
         * downstream consumers (e.g. emit_arg_for_param's
         * compound-literal trick for '&(rvalue)' passed to T&)
         * can read it. N4659 §8.4 [expr.cast]. */
        if (n->cast.ty) n->resolved_type = n->cast.ty;
        if (n->cast.operand && n->cast.operand->is_type_dependent)
            n->is_type_dependent = true;
        /* New-expr ctor args + placement args sit on the cast as
         * extra slots, not visited via the operand walk. Codegen
         * needs each arg's resolved_type for overload resolution
         * (especially per-element ctor pick in
         * `new T[N]{a0, a1, ...}`). */
        if (n->cast.is_new_expr) {
            for (int i = 0; i < n->cast.new_ctor_nargs; i++)
                visit(s, n->cast.new_ctor_args[i]);
            for (int i = 0; i < n->cast.new_placement_nargs; i++)
                visit(s, n->cast.new_placement_args[i]);
        }
        break;
    case ND_SIZEOF:
        visit(s, n->sizeof_.expr);
        break;
    case ND_TYPEID:
        /* N4659 §8.2.7 [expr.typeid]. Walk the operand so its
         * resolved_type gets filled in — emit reads it to pick the
         * per-static-type sentinel. The result is a sea-front-private
         * 'const void *' lvalue; we synthesize a void* type here so
         * downstream operators (== / !=) see a pointer rather than
         * the C++ source-level 'const std::type_info&'. */
        visit(s, n->typeid_.operand);
        {
            Type *vp = arena_alloc(s->arena, sizeof(Type));
            memset(vp, 0, sizeof(Type));
            vp->kind = TY_PTR;
            vp->base = arena_alloc(s->arena, sizeof(Type));
            memset(vp->base, 0, sizeof(Type));
            vp->base->kind = TY_VOID;
            vp->base->is_const = true;
            n->resolved_type = vp;
        }
        break;
    case ND_COMMA:
        /* ND_COMMA's struct layout differs from ND_BINARY's (no 'op'
         * field), so the union aliasing with visit_binary read the
         * wrong offsets and skipped the LHS. Walk both sides directly.
         * N4659 §8.19 [expr.comma] — value category is the RHS's. */
        visit(s, n->comma.lhs);
        visit(s, n->comma.rhs);
        if (n->comma.rhs)
            n->resolved_type = n->comma.rhs->resolved_type;
        if ((n->comma.lhs && n->comma.lhs->is_type_dependent) ||
            (n->comma.rhs && n->comma.rhs->is_type_dependent))
            n->is_type_dependent = true;
        break;

    case ND_QUALIFIED:
        /* N4659 §6.4.3 [basic.lookup.qual]: qualified name lookup.
         * Resolve 'Class::method' by looking up the leading segment
         * as a type, then the trailing segment in its class_region.
         * Sets resolved_type so codegen can use decl param types
         * for mangling instead of call-site arg types. */
        /* Single-part global qualifier '::name' — N4659 §8.1.4.3
         * [expr.prim.id.qual]: lookup in global namespace, ignoring
         * local/class shadows. Walk up to global scope and resolve
         * there; without this, callee_ft is NULL at the call site
         * and emit_arg_for_param's "unknown-callee, ref-param ident"
         * pass-through path suppresses the deref — '::free(v)' where
         * v is a 'vec*&' parameter emits as 'free(v)' (frees the
         * stack address-of-pointer) instead of 'free(*v)'. */
        if (n->qualified.global_scope && n->qualified.nparts == 1 &&
            s->cur_scope && n->qualified.parts[0]) {
            DeclarativeRegion *global = s->cur_scope;
            while (global->enclosing) global = global->enclosing;
            Token *nm = n->qualified.parts[0];
            Declaration *d = lookup_in_scope(global, nm->loc, nm->len);
            if (d && d->type)
                n->resolved_type = d->type;
        }
        if (n->qualified.nparts >= 2 && s->cur_scope) {
            Token *lead = n->qualified.parts[0];
            Token *member = n->qualified.parts[n->qualified.nparts - 1];
            if (lead && member) {
                /* N4659 §6.4.3 [basic.lookup.qual]: in a qualified-id
                 * 'C::m', the lookup of `C` is class-name lookup —
                 * variables, functions, and enumerators of the same
                 * name are ignored. Prefer ENTITY_TAG / ENTITY_TYPE /
                 * ENTITY_TEMPLATE; only fall back to general
                 * unqualified lookup if none of those resolve. Real-
                 * world shape (also legal C):
                 *   struct C { static int hash(...); };
                 *   static SomeOther C;   // variable shadows the tag
                 *   ... C::hash(arg);    // class-name lookup wins
                 * Without this, the leading qualifier resolves to the
                 * variable's type and codegen mangles the call with
                 * the wrong class tag. */
                Declaration *ld = lookup_kind_from(s->cur_scope,
                    lead->loc, lead->len, ENTITY_TAG);
                if (!ld)
                    ld = lookup_kind_from(s->cur_scope,
                        lead->loc, lead->len, ENTITY_TYPE);
                if (!ld)
                    ld = lookup_kind_from(s->cur_scope,
                        lead->loc, lead->len, ENTITY_TEMPLATE);
                if (!ld)
                    ld = lookup_unqualified_from(s->cur_scope,
                        lead->loc, lead->len);
                /* Namespace lead (N4659 §6.4.3 [basic.lookup.qual]):
                 * 'NS::name' looks up `name` in the namespace's region.
                 * Entries like a global variable inside the namespace
                 * (e.g. `namespace bidi { vec_t vec; }`, then
                 * `bidi::vec.method()` from outside) need the variable's
                 * type recorded on the qualified-id so the enclosing
                 * member-call emit can dispatch through the class. */
                if (ld && ld->entity == ENTITY_NAMESPACE && ld->ns_region) {
                    Declaration *md = lookup_in_scope(ld->ns_region,
                        member->loc, member->len);
                    if (md && md->type)
                        n->resolved_type = md->type;
                    /* Stash the Declaration so codegen can read
                     * c_linkage / asm_name. 'std::exit' resolves
                     * here via 'using ::exit;' inside namespace std
                     * — the propagated Declaration is the global
                     * extern "C" exit, which must mangle bare. */
                    if (md)
                        n->qualified.resolved_decl = md;
                }
                if (ld && ld->type && ld->type->class_region) {
                    Declaration *md = lookup_in_scope(ld->type->class_region,
                        member->loc, member->len);
                    if (md && md->type)
                        n->resolved_type = md->type;
                    /* Typedef-resolution: when 'lead' is a typedef
                     * for a class type (e.g. 'stackv' typedef'd to
                     * 'vec<T,va_stack,vl_embed>' inside the
                     * vec_stack_alloc macro), record the underlying
                     * class Type so codegen can mangle through
                     * mangle_class_tag (which preserves template
                     * args). Without this, 'stackv::embedded_size(N)'
                     * mangled as 'sf__stackv__embedded_size_*' OR
                     * 'sf__vec__embedded_size_*' (tag-only) on the
                     * call side, while the def lived under
                     * 'sf__vec_t_<T>_va_stack_vl_embed_te___embedded_size_*'.
                     * 24+ unresolved refs in cc1plus on df-scan.c. */
                    if (ld->type->tag &&
                        (ld->type->tag->len != lead->len ||
                         memcmp(ld->type->tag->loc, lead->loc, lead->len) != 0))
                        n->qualified.resolved_class_type = ld->type;
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
    case ND_RANGE_FOR: {
        DeclarativeRegion *saved = s->cur_scope;
        if (n->range_for.scope) s->cur_scope = n->range_for.scope;
        visit(s, n->range_for.range);
        visit(s, n->range_for.decl);
        visit(s, n->range_for.body);
        s->cur_scope = saved;
        break;
    }
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
    case ND_LABEL:
        /* Labeled statement — N4659 §9.1 [stmt.label]. Recurse into
         * the inner statement so identifiers get resolved_type set.
         * Without this, the labeled stmt's expressions never see
         * sema and method-dispatch lowering at codegen fails (obj's
         * resolved_type is NULL, so obj.method() can't be lowered
         * to the mangled free-function form and emits as literal
         * C member access which is invalid for non-fptr fields). */
        visit(s, n->label.stmt);
        break;
    /* Exception handling — N4659 §18 [except]. Sema work for
     * Phase 2 of EH lowering (docs/exceptions.md) is currently a
     * walk-children pass — the heavy work (catch-type registration,
     * rethrow-outside-handler diagnostics, noexcept inference)
     * lands in later slices. */
    case ND_TRY:
        visit(s, n->try_.body);
        for (int i = 0; i < n->try_.nhandlers; i++)
            visit(s, n->try_.handlers[i]);
        break;
    case ND_HANDLER: {
        DeclarativeRegion *saved = s->cur_scope;
        if (n->handler.scope) s->cur_scope = n->handler.scope;
        visit(s, n->handler.body);
        s->cur_scope = saved;
        break;
    }
    case ND_THROW:
        if (n->throw_.operand) visit(s, n->throw_.operand);
        break;
    case ND_INIT_LIST:
        /* Braced initializer — N4659 §11.6.4 [dcl.init.list]. Walk
         * each element so subscript/method dispatch lowering can see
         * the element types. Without this, an expression like
         * 'CONSTRUCTOR_ELT(arg0, i)->value' nested inside a struct
         * initializer never has its subscript-on-class dispatched
         * and emits as literal '[]' on a struct value. */
        for (int i = 0; i < n->init_list.nelems; i++)
            visit(s, n->init_list.elems[i]);
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

    case ND_TRANSLATION_UNIT: {
        /* Push the global region as cur_scope so visit_ident /
         * lookup_unqualified_from can find file-scope names from
         * file-scope var-decl inits (e.g. `T *p = &g;` references
         * the prior `g`). Without this, cur_scope is NULL and the
         * lookup short-circuits — leaving resolved_decl NULL and
         * downstream is-this-TLS checks blind. Pattern:
         * g++.dg/tls/thread_local-order{1,2}.C — `A* ap = &a1;`
         * needs to see that a1 is `thread_local` so the init is
         * deferred to __sf_global_init instead of being emitted
         * as a static C init (which the C front rejects because
         * `&<TLS>` isn't a constant). */
        DeclarativeRegion *saved = s->cur_scope;
        if (n->tu.global_scope) s->cur_scope = n->tu.global_scope;
        for (int i = 0; i < n->tu.ndecls; i++)
            visit(s, n->tu.decls[i]);
        s->cur_scope = saved;
        break;
    }

    default:
        /* Everything else: walk children we know about, leave
         * resolved_type as NULL. The codegen falls back to source-form
         * dumping for these. */
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                */
/* ------------------------------------------------------------------ */

/* Whole-TU semantic analysis pass — N4659 §6.4 [basic.lookup] +
 * §16.3 [over.match] + §17.7 [temp.res]. Walks the AST once,
 * resolving identifiers, picking overloads, and propagating types
 * onto every expression node's resolved_type field. */
/* ------------------------------------------------------------------ */
/* Template-id type canonicalization                                   */
/* ------------------------------------------------------------------ */
/*
 * N4659 §17.3 [temp.names]/3 + §17.6.2.2 [temp.inst]/9: a template-id
 * that omits trailing args denotes the specialization with those args
 * substituted from the template parameter list's defaults. The type
 * `vec<X, va_gc>` IS `vec<X, va_gc, va_gc::default_layout>` —
 * `vec<X, va_gc, vl_embed>` once the typedef is resolved. There's no
 * 2-arg specialization of vec; the resulting Type should already
 * carry 3 template_args by the time sema's overload resolution runs.
 *
 * Sea-front's parser doesn't expand defaults — it emits the Type
 * with whatever args the user wrote. We canonicalize here, before
 * visit() runs: build a top-level template-name index, then walk
 * every Type in the AST and pad missing trailing args with the
 * template's default_type expressions (substituted against the
 * already-bound earlier params).
 *
 * Without this, deduction in visit_call against a parameter pattern
 * like `vec<T, A, vl_embed>*` arity-mismatches the call's 2-arg
 * `vec<X, va_gc>*` and silently drops the candidate (as of the
 * deduce-failure fix), so calls in gengtype-generated code resolve
 * to nothing and link bare. After canonicalization the arg type IS
 * the 3-arg form and deduction succeeds.
 */

#include <stdint.h>

typedef struct TmplIdxEntry {
    const char *name;
    int         name_len;
    Node       *tmpl;            /* ND_TEMPLATE_DECL */
    struct TmplIdxEntry *next;
} TmplIdxEntry;

#define TMPL_IDX_SIZE 64
typedef struct {
    TmplIdxEntry *buckets[TMPL_IDX_SIZE];
    Arena        *arena;
} TmplIdx;

static void tmpl_idx_add(TmplIdx *idx, Node *tmpl) {
    if (!tmpl || tmpl->kind != ND_TEMPLATE_DECL || !tmpl->template_decl.decl)
        return;
    Node *inner = tmpl->template_decl.decl;
    Token *name = NULL;
    if (inner->kind == ND_CLASS_DEF) name = inner->class_def.tag;
    else if (inner->kind == ND_FUNC_DEF || inner->kind == ND_FUNC_DECL)
        name = inner->func.name;
    else if (inner->kind == ND_VAR_DECL || inner->kind == ND_TYPEDEF)
        name = inner->var_decl.name;
    if (!name) return;
    uint32_t h = hash_name(name->loc, name->len) % TMPL_IDX_SIZE;
    TmplIdxEntry *e = arena_alloc(idx->arena, sizeof(TmplIdxEntry));
    e->name = name->loc;
    e->name_len = name->len;
    e->tmpl = tmpl;
    e->next = idx->buckets[h];
    idx->buckets[h] = e;
}

static Node *tmpl_idx_find_class(TmplIdx *idx, Token *name) {
    if (!name) return NULL;
    uint32_t h = hash_name(name->loc, name->len) % TMPL_IDX_SIZE;
    /* Prefer a primary class template (no template_id_node on its
     * inner Type) — an ad-hoc proxy for "this is the one whose
     * defaults we should consult." */
    Node *primary = NULL;
    Node *fallback = NULL;
    for (TmplIdxEntry *e = idx->buckets[h]; e; e = e->next) {
        if (e->name_len != name->len ||
            memcmp(e->name, name->loc, name->len) != 0) continue;
        Node *inner = e->tmpl->template_decl.decl;
        if (!inner || inner->kind != ND_CLASS_DEF) continue;
        Type *ty = inner->class_def.ty;
        if (ty && !ty->template_id_node) { primary = e->tmpl; break; }
        if (!fallback) fallback = e->tmpl;
    }
    return primary ? primary : fallback;
}

/* Build name→ND_TEMPLATE_DECL by walking the TU. Top-level + namespace
 * + class scope. Skip member templates inside classes — they don't
 * appear at the call sites we canonicalize. */
static void tmpl_idx_build_walk(TmplIdx *idx, Node *n) {
    if (!n) return;
    if (n->kind == ND_TEMPLATE_DECL) tmpl_idx_add(idx, n);
    else if (n->kind == ND_BLOCK) {
        /* Namespace block — recurse into children. */
        for (int i = 0; i < n->block.nstmts; i++)
            tmpl_idx_build_walk(idx, n->block.stmts[i]);
    } else if (n->kind == ND_TRANSLATION_UNIT) {
        for (int i = 0; i < n->tu.ndecls; i++)
            tmpl_idx_build_walk(idx, n->tu.decls[i]);
    }
}

/* Pad ty's template_args/template_id_node trailing slots with
 * substituted defaults. Mutates ty in place. */
static void canonicalize_type(Type *ty, TmplIdx *idx, Arena *arena) {
    if (!ty || !ty->template_id_node) return;
    Node *tid = ty->template_id_node;
    if (tid->kind != ND_TEMPLATE_ID || !tid->template_id.name) return;
    Node *tmpl = tmpl_idx_find_class(idx, tid->template_id.name);
    if (!tmpl) return;
    int nparams = tmpl->template_decl.nparams;
    int nargs   = tid->template_id.nargs;
    if (nargs >= nparams) return;
    /* Verify trailing params have type-param defaults — bail if any
     * don't. NTTP defaults (param.default_value) are evaluated by
     * the instantiation pass, not here. */
    for (int i = nargs; i < nparams; i++) {
        Node *p = tmpl->template_decl.params[i];
        if (!p || !p->param.default_type) return;
    }
    /* Build a SubstMap from the explicit args so 'A::default_layout'-
     * style defaults can resolve against earlier bindings. */
    SubstMap map = subst_map_new(arena, nparams > 0 ? nparams : 1);
    subst_map_bind_args(&map,
        tmpl->template_decl.params, nparams,
        tid->template_id.args, nargs);
    /* Materialize defaults. */
    Node **new_args = arena_alloc(arena, nparams * sizeof(Node *));
    Type **new_targs = arena_alloc(arena, nparams * sizeof(Type *));
    for (int i = 0; i < nargs; i++) {
        new_args[i] = tid->template_id.args[i];
        Node *a = tid->template_id.args[i];
        new_targs[i] = (a && a->kind == ND_VAR_DECL) ? a->var_decl.ty : NULL;
    }
    for (int i = nargs; i < nparams; i++) {
        Node *p = tmpl->template_decl.params[i];
        Type *defty = subst_type(p->param.default_type, &map, arena);
        Node *narg = arena_alloc(arena, sizeof(Node));
        memset(narg, 0, sizeof(Node));
        narg->kind = ND_VAR_DECL;
        narg->var_decl.ty = defty;
        new_args[i] = narg;
        new_targs[i] = defty;
        /* Bind for any later default that depends on this position. */
        if (p->param.name) subst_map_add(&map, p->param.name, defty);
    }
    /* Rewrite the existing template-id node in place so other Types
     * sharing this same Node also see the expanded args. */
    tid->template_id.args = new_args;
    tid->template_id.nargs = nparams;
    /* Update the cached flat array on the Type. */
    ty->template_args = new_targs;
    ty->n_template_args = nparams;
}

/* Walk a Type and recurse into compounds. Visit each TY_STRUCT that
 * carries a template_id_node. */
static void canonicalize_walk_type(Type *ty, TmplIdx *idx, Arena *arena) {
    if (!ty) return;
    switch (ty->kind) {
    case TY_PTR: case TY_REF: case TY_RVALREF: case TY_ARRAY:
        canonicalize_walk_type(ty->base, idx, arena);
        break;
    case TY_FUNC:
        canonicalize_walk_type(ty->ret, idx, arena);
        for (int i = 0; i < ty->nparams; i++)
            canonicalize_walk_type(ty->params[i], idx, arena);
        break;
    case TY_STRUCT: case TY_UNION:
        canonicalize_type(ty, idx, arena);
        /* Recurse into the (possibly newly-padded) template_args. */
        for (int i = 0; i < ty->n_template_args; i++)
            canonicalize_walk_type(ty->template_args[i], idx, arena);
        break;
    default: break;
    }
}

/* Walk every Node and apply canonicalize_walk_type to every Type field
 * we know about. Mirrors patch_node_types in instantiate.c. */
static void canonicalize_walk_node(Node *n, TmplIdx *idx, Arena *arena) {
    if (!n) return;
    switch (n->kind) {
    case ND_VAR_DECL: case ND_PARAM:
        canonicalize_walk_type(n->var_decl.ty, idx, arena);
        if (n->var_decl.init) canonicalize_walk_node(n->var_decl.init, idx, arena);
        break;
    case ND_TYPEDEF:
        canonicalize_walk_type(n->var_decl.ty, idx, arena);
        break;
    case ND_FUNC_DEF: case ND_FUNC_DECL:
        canonicalize_walk_type(n->func.ret_ty, idx, arena);
        for (int i = 0; i < n->func.nparams; i++)
            canonicalize_walk_node(n->func.params[i], idx, arena);
        if (n->func.body) canonicalize_walk_node(n->func.body, idx, arena);
        break;
    case ND_CLASS_DEF:
        for (int i = 0; i < n->class_def.nbase_types; i++)
            canonicalize_walk_type(n->class_def.base_types[i], idx, arena);
        for (int i = 0; i < n->class_def.nmembers; i++)
            canonicalize_walk_node(n->class_def.members[i], idx, arena);
        break;
    case ND_TEMPLATE_DECL:
        canonicalize_walk_node(n->template_decl.decl, idx, arena);
        break;
    case ND_BLOCK:
        for (int i = 0; i < n->block.nstmts; i++)
            canonicalize_walk_node(n->block.stmts[i], idx, arena);
        break;
    case ND_IF:
        canonicalize_walk_node(n->if_.cond, idx, arena);
        canonicalize_walk_node(n->if_.then_, idx, arena);
        canonicalize_walk_node(n->if_.else_, idx, arena);
        break;
    case ND_WHILE: case ND_DO:
        canonicalize_walk_node(n->while_.cond, idx, arena);
        canonicalize_walk_node(n->while_.body, idx, arena);
        break;
    case ND_FOR:
        canonicalize_walk_node(n->for_.init, idx, arena);
        canonicalize_walk_node(n->for_.cond, idx, arena);
        canonicalize_walk_node(n->for_.inc, idx, arena);
        canonicalize_walk_node(n->for_.body, idx, arena);
        break;
    case ND_RANGE_FOR:
        canonicalize_walk_node(n->range_for.decl, idx, arena);
        canonicalize_walk_node(n->range_for.range, idx, arena);
        canonicalize_walk_node(n->range_for.body, idx, arena);
        break;
    case ND_RETURN:
        canonicalize_walk_node(n->ret.expr, idx, arena);
        break;
    case ND_EXPR_STMT:
        canonicalize_walk_node(n->expr_stmt.expr, idx, arena);
        break;
    case ND_CAST:
        canonicalize_walk_type(n->cast.ty, idx, arena);
        if (n->cast.operand) canonicalize_walk_node(n->cast.operand, idx, arena);
        break;
    case ND_SIZEOF:
        canonicalize_walk_type(n->sizeof_.ty, idx, arena);
        if (n->sizeof_.expr) canonicalize_walk_node(n->sizeof_.expr, idx, arena);
        break;
    case ND_ALIGNOF:
        canonicalize_walk_type(n->alignof_.ty, idx, arena);
        break;
    case ND_OFFSETOF:
        canonicalize_walk_type(n->offsetof_.ty, idx, arena);
        break;
    case ND_CALL:
        canonicalize_walk_node(n->call.callee, idx, arena);
        for (int i = 0; i < n->call.nargs; i++)
            canonicalize_walk_node(n->call.args[i], idx, arena);
        break;
    case ND_BINARY: case ND_ASSIGN:
        canonicalize_walk_node(n->binary.lhs, idx, arena);
        canonicalize_walk_node(n->binary.rhs, idx, arena);
        break;
    case ND_UNARY:
        canonicalize_walk_node(n->unary.operand, idx, arena);
        break;
    case ND_SUBSCRIPT:
        canonicalize_walk_node(n->subscript.base, idx, arena);
        canonicalize_walk_node(n->subscript.index, idx, arena);
        break;
    case ND_MEMBER:
        canonicalize_walk_node(n->member.obj, idx, arena);
        break;
    case ND_TERNARY:
        canonicalize_walk_node(n->ternary.cond, idx, arena);
        canonicalize_walk_node(n->ternary.then_, idx, arena);
        canonicalize_walk_node(n->ternary.else_, idx, arena);
        break;
    /* Exception handling — recurse into bodies so template-ids
     * nested inside try-bodies, handler-bodies, or throw operands
     * (e.g. 'throw vec<int>{};') get canonicalised. */
    case ND_TRY:
        canonicalize_walk_node(n->try_.body, idx, arena);
        for (int i = 0; i < n->try_.nhandlers; i++)
            canonicalize_walk_node(n->try_.handlers[i], idx, arena);
        break;
    case ND_HANDLER:
        if (n->handler.param) canonicalize_walk_node(n->handler.param, idx, arena);
        canonicalize_walk_node(n->handler.body, idx, arena);
        break;
    case ND_THROW:
        if (n->throw_.operand) canonicalize_walk_node(n->throw_.operand, idx, arena);
        break;
    default: break;
    }
}

void sema_run(Node *tu, Arena *arena) {
    /* Pre-pass: canonicalize template-id Types by expanding default
     * trailing args. Per N4659 §17.3 [temp.names]/3, a template-id
     * with omitted defaults denotes the fully-defaulted spec. */
    TmplIdx idx = { .arena = arena };
    tmpl_idx_build_walk(&idx, tu);
    canonicalize_walk_node(tu, &idx, arena);

    Sema s = { .arena = arena, .tu = tu, .cur_scope = NULL };
    visit(&s, tu);
}

/* Phase-2 sema entry point — N4659 §17.7.2 [temp.dep]: re-visit a
 * single node (typically a freshly-cloned template instantiation
 * body) to resolve names that became non-dependent after type
 * substitution. Equivalent to running sema_run on the subtree. */
void sema_visit_node(Node *n, Arena *arena) {
    if (!n) return;
    /* No tu reference — phase 2 doesn't need TU-wide context.
     * cur_scope starts NULL; the visitor pushes function/block
     * scopes as it descends. */
    Sema s = { .arena = arena, .tu = NULL, .cur_scope = NULL };
    visit(&s, n);
}
