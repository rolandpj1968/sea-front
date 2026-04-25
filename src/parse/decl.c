/*
 * decl.c — Declaration parser.
 *
 * N4659 §10 [dcl.dcl] — Declarations
 *
 *   declaration:
 *       block-declaration
 *       function-definition
 *       template-declaration            (handled — see parse_template_decl)
 *       explicit-instantiation          (skip-parsed)
 *       explicit-specialization         (handled via template-declaration)
 *       linkage-specification           (handled — extern "C" / "C++")
 *       namespace-definition            (handled)
 *       empty-declaration               ( ; )
 *       attribute-declaration           (skip-parsed via parser_skip_*_attributes)
 *
 *   block-declaration:
 *       simple-declaration
 *       asm-declaration                 (skip-parsed)
 *       namespace-alias-definition      (handled)
 *       using-declaration               (handled — using T::name;)
 *       using-directive                 (handled — using namespace N;)
 *       static_assert-declaration       (skip-parsed)
 *       alias-declaration               (handled — using T = type;)
 *       opaque-enum-declaration         (deferred)
 *
 *   simple-declaration:
 *       decl-specifier-seq init-declarator-list(opt) ;
 *       attribute-specifier-seq decl-specifier-seq init-declarator-list ;
 *       // C++17: attribute-specifier-seq(opt) decl-specifier-seq
 *       //        ref-qualifier(opt) [ identifier-list ] initializer ;
 *       //        (structured bindings — deferred)
 *
 * C++20 changes: adds concept-definition, module-declaration,
 *   export-declaration, constinit, consteval.
 * C++23 changes: adds deducing this (explicit object parameter).
 *
 * Handles: simple-declaration, function-definition, template-declaration,
 * namespace-definition, linkage-specification, using-declaration/directive,
 * friend-declaration, static_assert. User-defined type names are resolved
 * via name lookup (§6.4).
 */

#include "parse.h"
#include "../template/clone.h"

/* Forward declaration. consume_trailing_qualifiers returns true if
 * the trailing qualifier list included 'const'. It is a helper of
 * parse_declarator and lives immediately below it (per the
 * "general above helpers" convention). The forward decl is needed
 * because parse_declarator calls it from within its body. */
static bool consume_trailing_qualifiers(Parser *p);

/*
 * parse_declarator — N4659 §11.3 [dcl.meaning]
 *
 *   declarator:
 *       ptr-declarator
 *       noptr-declarator parameters-and-qualifiers trailing-return-type
 *
 *   ptr-declarator:
 *       noptr-declarator
 *       ptr-operator ptr-declarator
 *
 *   ptr-operator:
 *       * cv-qualifier-seq(opt)                    (pointer)
 *       & attribute-specifier-seq(opt)             (lvalue reference)
 *       && attribute-specifier-seq(opt)            (rvalue reference)
 *       nested-name-specifier * cv-qualifier-seq   (pointer-to-member)
 *
 *   noptr-declarator:
 *       declarator-id attribute-specifier-seq(opt)
 *       noptr-declarator parameters-and-qualifiers  (function)
 *       noptr-declarator [ constant-expression(opt) ] attribute-specifier-seq(opt) (array)
 *       ( ptr-declarator )                          (grouping parens)
 *
 * C++11: trailing return types (auto f() -> int)
 * C++20: abbreviated function templates (void f(auto x))
 * C++23: deducing this
 *
 * Returns an ND_VAR_DECL node (or ND_PARAM) with the fully-built type
 * and the declared name. For function declarators, the type is TY_FUNC.
 *
 * The declarator is one of the most complex parts of C/C++ syntax.
 * The "inside-out" reading rule: pointer operators wrap from the left,
 * while array/function suffixes wrap from the right, and parentheses
 * override the default grouping. We handle this recursively.
 *
 * Helpers (consume_trailing_qualifiers and the template-default
 * pair) are defined below this function in the file, immediately
 * below the general parsers they support.
 */
/* Re-apply a single deferred wrapper from a grouped declarator.
 * Preserves cv-qualifiers (for ptrs) and array length / size-expr
 * (for arrays) — the existing code dropped these, which silently
 * lost 'const' on '(*const NAME)(args)' and the size on
 * '(*NAME[N])(args)'. */
static Type *apply_pending_wrap(Parser *p, Type *ty, Type *w) {
    Type *nt;
    if (w->kind == TY_PTR)        nt = new_ptr_type(p, ty);
    else if (w->kind == TY_REF)   nt = new_ref_type(p, ty);
    else if (w->kind == TY_RVALREF) nt = new_rvalref_type(p, ty);
    else if (w->kind == TY_ARRAY) {
        nt = new_array_type(p, ty, w->array_len);
        nt->array_size_expr = w->array_size_expr;
    } else {
        return ty;
    }
    nt->is_const = w->is_const;
    nt->is_volatile = w->is_volatile;
    return nt;
}

Node *parse_declarator(Parser *p, Type *base_ty) {
    /* ptr-operator — N4659 §11.3 [dcl.meaning]
     *   ptr-operator:
     *       * cv-qualifier-seq(opt)                    — pointer
     *       & attribute-specifier-seq(opt)             — lvalue reference
     *       && attribute-specifier-seq(opt)            — rvalue reference
     *       nested-name-specifier * cv-qualifier-seq   — pointer-to-member
     *
     * N4659 §11.3.2 [dcl.ref]:
     *   "In a declaration T D where D has either of the forms
     *    & attribute-specifier-seq(opt) D1
     *    && attribute-specifier-seq(opt) D1
     *    and the type of the identifier in the declaration T D1 is
     *    'derived-declarator-type-list T', then the type of the
     *    identifier of D is 'derived-declarator-type-list reference to T'."
     *
     * Note: && in declarator context is TK_LAND (logical AND token),
     * not two separate & tokens. This is unambiguous because we're
     * already committed to parsing a declarator.
     * Terminates: each iteration consumes a ptr-operator token (*, &, &&)
     * or breaks. Finite tokens, each consumed at most once for & and &&. */
    for (;;) {
        if (parser_consume(p, TK_STAR)) {
            base_ty = new_ptr_type(p, base_ty);
            /* cv-qualifiers after * — N4659 §11.3.1/1 [dcl.ptr] */
            while (parser_at(p, TK_KW_CONST) || parser_at(p, TK_KW_VOLATILE)) {
                if (parser_consume(p, TK_KW_CONST))    base_ty->is_const = true;
                if (parser_consume(p, TK_KW_VOLATILE)) base_ty->is_volatile = true;
            }
            /* GCC __attribute__ after pointer cv-qualifiers:
             * 'const char * const __attribute__((unused)) name'. */
            parser_skip_gnu_attributes(p);
        } else if (parser_consume(p, TK_LAND)) {
            /* && — rvalue reference (C++11) */
            base_ty = new_rvalref_type(p, base_ty);
        } else if (parser_consume(p, TK_AMP)) {
            /* & — lvalue reference */
            base_ty = new_ref_type(p, base_ty);
        } else if (parser_at(p, TK_IDENT) &&
                   parser_peek_ahead(p, 1)->kind == TK_SCOPE) {
            /* nested-name-specifier * — pointer-to-member.
             * Look ahead for '*' at the end of the nested-name chain. */
            int n = 0;
            while (parser_peek_ahead(p, n)->kind == TK_IDENT &&
                   parser_peek_ahead(p, n + 1)->kind == TK_SCOPE)
                n += 2;
            if (parser_peek_ahead(p, n)->kind == TK_STAR) {
                for (int i = 0; i <= n; i++) parser_advance(p);
                base_ty = new_ptr_type(p, base_ty);
                while (parser_at(p, TK_KW_CONST) || parser_at(p, TK_KW_VOLATILE)) {
                    if (parser_consume(p, TK_KW_CONST))    base_ty->is_const = true;
                    if (parser_consume(p, TK_KW_VOLATILE)) base_ty->is_volatile = true;
                }
            } else {
                break;
            }
        } else {
            break;
        }
    }

    /* Pack expansion '...' between ptr-operators and declarator-id —
     * 'T&&... args'. Consume so the rest of parse_declarator sees the
     * name as the next token. */
    parser_consume(p, TK_ELLIPSIS);

    /* Tracks whether the declarator-id passed through '::' (qualified
     * name like Foo::bar or Foo<T>::operator=). Used by the
     * looks_like_params heuristic below — when the name is qualified,
     * a following '(' is overwhelmingly a parameter list, never a
     * call expression. */
    bool name_was_qualified = false;
    Token *name = NULL;
    Type *ty = base_ty;
    /* Deferred wrappers from a grouped declarator like '(*fp)' — the
     * inner '*' must bind AFTER the outer suffix runs. See the grouped-
     * declarator branch below. */
    Type *pending_wrap[16];
    int   pending_nwrap = 0;

    /* Parenthesized declarator: ( ptr-declarator )
     *
     * N4659 §11.3.4 [dcl.fct] gives the grammar for the abstract
     * shape and §11.2 [dcl.ambig.res]/1 prescribes how to
     * disambiguate the THREE constructs that all begin with '(':
     *
     *   (a) Grouping parens: int (*fp)(int)  — '(' starts a nested
     *       declarator that wraps the eventual declarator-id.
     *   (b) Function parameter list after an unnamed declarator:
     *       int (int) — i.e. an abstract function declarator.
     *   (c) Redundant parens around a name: T(x) declaring x of type T.
     *
     * The standard's algorithm requires unbounded lookahead through
     * balanced parentheses: parse the contents tentatively as a
     * parameter-declaration-clause and commit if it succeeds.
     *
     * --- SHORTCUT (our implementation, not the standard's algorithm):
     * We use a one-token lookahead instead of full tentative parsing.
     * Specifically: '(' followed by '*', '(', '&', or a non-type
     * IDENT is treated as case (a)/(c) — grouping. '(' followed by
     * a type keyword is treated as case (b) — parameter list.
     *
     * This works for every shape we've seen in the libstdc++ smoke
     * set, but it is NOT equivalent to the standard's algorithm for
     * pathological inputs. TODO(seafront#decl-ambig): replace with
     * a tentative parse of the parenthesised contents as a
     * parameter-declaration-clause to match §11.2/1 exactly.
     *
     * Special case (the ptm_grouped check below): a pointer-to-member
     * grouped declarator '(T::*name)(...)' starts with the IDENT 'T',
     * which parser_at_type_specifier reports as a type. Our shortcut
     * would otherwise misclassify it as case (b). The 'IDENT :: *'
     * shape is detected explicitly and forces the grouped branch.
     * This patch is also a shortcut, not a standard rule. */
    bool ptm_grouped = parser_at(p, TK_LPAREN) &&
        parser_peek_ahead(p, 1)->kind == TK_IDENT &&
        parser_peek_ahead(p, 2)->kind == TK_SCOPE &&
        parser_peek_ahead(p, 3)->kind == TK_STAR;
    if (parser_at(p, TK_LPAREN) && (ptm_grouped || !parser_at_type_specifier(p))) {
        Token *next = parser_peek_ahead(p, 1);
        /* For (ident ...): only treat as grouped declarator if the ident is
         * immediately followed by ')', '(', or '[' — i.e., it could really
         * be the name in a redundant-paren declarator. Anything else (like
         * a comma) means we're in a direct-init or call, not a declaration. */
        bool ident_grouped = next && next->kind == TK_IDENT &&
                             !lookup_is_type_name(p, next) &&
                             (parser_peek_ahead(p, 2)->kind == TK_RPAREN ||
                              parser_peek_ahead(p, 2)->kind == TK_LPAREN ||
                              parser_peek_ahead(p, 2)->kind == TK_LBRACKET);
        if (ptm_grouped || (next && (next->kind == TK_STAR || next->kind == TK_AMP ||
                     next->kind == TK_LAND || next->kind == TK_LPAREN ||
                     ident_grouped))) {
            /* Grouping parens or redundant parens around a name.
             * E.g.: int (*fp)(int,int)  or  T(x)  */
            parser_advance(p);  /* skip ( */
            Node *inner = parse_declarator(p, base_ty);
            parser_expect(p, TK_RPAREN);

            name = inner->var_decl.name;
            /* N4659 §11.3 [dcl.meaning]: in a grouped declarator the
             * inner modifiers (*, &, &&) apply AFTER the outer
             * suffix (function params, arrays). Example:
             *   int (*fp)(int)
             * The inner '*' binds later than the outer '(int)' —
             * fp has type 'pointer to function taking int returning int',
             * not 'function returning pointer to int'.
             *
             * Implementation: the inner recursion wrapped base_ty with
             * its *, &, && operators. Unwrap them into pending_wrap[]
             * and re-apply after the outer suffix has mutated ty. */
            ty = inner->var_decl.ty;
            /* TY_ARRAY also peels: '(*const NAME[])(args)' has the
             * inner '[]' as a suffix on NAME but the outer '(args)'
             * applies to the array element. Without peeling the
             * array, the function-suffix wraps the array as the
             * return type — wrong shape (function returning array,
             * which is invalid). N4659 §11.3 [dcl.meaning] precedence
             * is what this re-stack restores. Pattern: gcc 4.8
             * internal-fn.c
             *   static void (*const internal_fn_expanders[])(gimple) */
            while (pending_nwrap < 16 && ty != base_ty &&
                   (ty->kind == TY_PTR || ty->kind == TY_REF ||
                    ty->kind == TY_RVALREF || ty->kind == TY_ARRAY)) {
                pending_wrap[pending_nwrap++] = ty;
                ty = ty->base;
            }
            goto parse_suffixes;
        }
    }

    /* declarator-id — N4659 §11.3 [dcl.meaning]
     *   declarator-id: ...(opt) id-expression
     *   id-expression: unqualified-id | qualified-id
     *
     *   unqualified-id:
     *       identifier
     *       operator-function-id      (§16.5 [over.oper])
     *       conversion-function-id    (deferred)
     *       ~ class-name              (destructor)
     *
     * Handles bare identifiers, qualified names (Foo::bar),
     * operator overloads (operator[]), and destructors (~Foo). */
    if (parser_at(p, TK_IDENT)) {
        name = parser_advance(p);
        /* Track the resolved scope of the qualifier — N4659 §6.4.3.
         * For 'void Foo::bar(...)', this ends up pointing at Foo's
         * class_region so the method body can resolve members via
         * lookup. Reset to NULL on template-id segments (instantiation-
         * dependent, no resolved scope).
         *
         * Use kind-specific lookups: a class template has BOTH an
         * ENTITY_TEMPLATE entry (type=NULL) and ENTITY_TYPE/TAG
         * entries (with class_region). Plain lookup_unqualified
         * returns the most-recent insertion, which is usually the
         * TEMPLATE entry — wrong for our purposes. */
        Declaration *qres = lookup_unqualified_kind(p, name->loc, name->len, ENTITY_TYPE);
        if (!qres)
            qres = lookup_unqualified_kind(p, name->loc, name->len, ENTITY_TAG);
        if (!qres)
            qres = lookup_unqualified_kind(p, name->loc, name->len, ENTITY_NAMESPACE);
        DeclarativeRegion *qscope = NULL;
        if (qres) {
            if (qres->entity == ENTITY_NAMESPACE)
                qscope = qres->ns_region;
            else if (qres->type && qres->type->class_region)
                qscope = qres->type->class_region;
        }
        /* Template-id in declarator: 'vec<T, A, vl_embed>::operator[]'.
         *
         * Standard rule — N4659 §17.2/2 [temp.names]:
         *   "After name lookup (§6.4) finds that a name is a
         *    template-name ..., this name, when followed by <, is
         *    always taken as the [start of a] template-id."
         * The §11.3 [dcl.meaning] grammar for declarator-id offers
         * no relational-operator production, so the only valid
         * reading of '<' here is the template-id opener. If lookup
         * had found the name to be NOT a template-name, the program
         * would be ill-formed and the standard would have us
         * diagnose.
         *
         * --- SHORTCUT (our implementation, not the standard's):
         * We skip the lookup check entirely and parse '<...>' as
         * template-args unconditionally. For every valid program
         * this is correct (template-args is the only valid reading).
         * For ill-formed programs — a non-template name followed by
         * '<' in declarator-id position — we silently accept rather
         * than diagnose. The difference matters for diagnostic
         * quality, not for the output of any correct program.
         *
         * The shortcut also lets us parse declarator-ids whose names
         * lookup can't see (inline-namespace members, friend
         * declarations not yet visible, etc.) — for those, the
         * standard's algorithm would also need a fully-modeled
         * lookup chain to know they're templates. We get the right
         * answer in practice without that machinery.
         *
         * TODO(seafront#decl-tmpl-id): when our lookup covers the
         * inline-namespace and friend-injection cases, restore the
         * standard check and emit a diagnostic on lookup failure. */
        Node *leading_tid = NULL;
        if (parser_at(p, TK_LT)) {
            leading_tid = parse_template_id(p, name);
            /* Keep qscope: 'C<T>::f' should still resolve into C's
             * class_region for member visibility — an instantiation
             * may differ in dependent details, but member names
             * declared in the primary template are visible. The
             * template-id's args are preserved below on the FUNC_DEF
             * so the instantiation pass can bind the OOL method to
             * the matching partial specialization (not just the tag). */
        }
        /* Consume qualified-id: ident(opt <args>) :: ident :: ... :: ident
         * N4659 §6.4.3 [basic.lookup.qual] / §11.3 grammar.
         * Terminates: each iteration consumes :: + ident/operator, or breaks. */
        while (parser_at(p, TK_SCOPE)) {
            name_was_qualified = true;
            Token *after = parser_peek_ahead(p, 1);
            if (after->kind == TK_IDENT) {
                parser_advance(p);  /* :: */
                name = parser_advance(p);
                /* Template-id at intermediate segments — e.g.
                 * 'std::basic_streambuf<C, T>::operator='. Same
                 * §17.2/2 standard rule and same shortcut as the
                 * leading segment above: lookup-required in theory,
                 * elided in practice. See the comment block on the
                 * leading-segment template-id parse for the full
                 * discussion. */
                if (parser_at(p, TK_LT)) {
                    Node *seg_tid = parse_template_id(p, name);
                    /* Resolve template-id segment via lookup in current
                     * qscope and refine. */
                    if (qscope) {
                        Declaration *next = lookup_in_scope(qscope, name->loc, name->len);
                        if (next && next->type && next->type->class_region)
                            qscope = next->type->class_region;
                    }
                    /* Track the deepest template-id so OOL methods whose
                     * qualifier ends in a template-id carry its args. */
                    if (seg_tid) leading_tid = seg_tid;
                }
                /* Resolve this segment in the previous scope. The LAST
                 * segment is the declarator-id name itself; we don't
                 * descend into it. We only update qscope when the
                 * qualifier (everything-but-the-last) refines further. */
                else if (parser_at(p, TK_SCOPE) && qscope) {
                    Declaration *next = lookup_in_scope(qscope, name->loc, name->len);
                    if (next && next->type && next->type->class_region)
                        qscope = next->type->class_region;
                    else if (next && next->entity == ENTITY_NAMESPACE)
                        qscope = next->ns_region;
                    else
                        qscope = NULL;
                }
            } else if (after->kind == TK_TILDE) {
                /* Qualified destructor: Foo::~Foo */
                parser_advance(p);  /* :: */
                parser_advance(p);  /* ~ */
                if (parser_at(p, TK_IDENT))
                    name = parser_advance(p);
                p->pending_is_destructor = true;
                break;
            } else if (after->kind == TK_KW_OPERATOR) {
                /* Qualified operator: Foo::operator[] */
                parser_advance(p);  /* :: */
                /* Stash the class scope BEFORE the goto — the normal
                 * post-loop stash at line ~393 is skipped by the jump.
                 * Without this, OOL operator definitions like
                 * 'DI::operator++()' lose their class_type and codegen
                 * emits them as free functions (no 'this' param). */
                if (qscope) p->qualified_decl_scope = qscope;
                if (leading_tid) p->qualified_decl_tid = leading_tid;
                goto parse_operator_id;
            } else {
                break;
            }
        }
        /* Stash the resolved class scope for parse_declaration's
         * function-def branch to push. Gate on name_was_qualified:
         * without '::', qscope came only from the leading ident's
         * own type lookup, which triggers when a free function shares
         * its name with a struct ('inline_edge_summary' in gcc) or a
         * parameter name shadows a class type ('bitmap_obstack *obstack').
         * In those cases the declarator is NOT an OOL member — stashing
         * would cause the function-def branch to mis-tag it as a method.
         * N4659 §6.4.3 [basic.lookup.qual] — only qualified-ids name
         * out-of-class members. */
        if (name_was_qualified && qscope)
            p->qualified_decl_scope = qscope;
        /* Preserve the qualifier template-id, if any, so the
         * function-def branch can copy it onto func.qual_tid. */
        if (name_was_qualified && leading_tid)
            p->qualified_decl_tid = leading_tid;
        /* Out-of-class constructor definition: 'Foo::Foo(...)'.
         * If the qualified-id ends in a name that matches the
         * qualifier scope's class name, this is a ctor. The
         * pending_is_destructor branch above already handles the
         * dtor case ('Foo::~Foo'); ctors don't have a leading
         * marker so we have to check name equality. */
        /* Only treat as out-of-class ctor when the declarator WAS a
         * qualified name: 'Foo::Foo(...)'. Without 'name_was_qualified'
         * the leading qscope was set merely from the ident's own type
         * lookup — this happens for parameter names or field names
         * that share a type's identifier ('bitmap_obstack *obstack'
         * → 'obstack' is a param NAME that also names a class type).
         * In those cases the ident is NOT a ctor and we must not set
         * pending_is_constructor (it'd leak into the next declaration).
         * N4659 §6.4.3 [basic.lookup.qual] — only qualified-ids name
         * out-of-class members. */
        if (name_was_qualified &&
            qscope && qscope->kind == REGION_CLASS && qscope->owner_type &&
            qscope->owner_type->tag && name &&
            name->len == qscope->owner_type->tag->len &&
            memcmp(name->loc, qscope->owner_type->tag->loc, name->len) == 0) {
            p->pending_is_constructor = true;
        }
    }
    /* Destructor at current scope: ~ClassName */
    if (!name && parser_at(p, TK_TILDE) && parser_peek_ahead(p, 1)->kind == TK_IDENT) {
        parser_advance(p);  /* ~ */
        name = parser_advance(p);
        p->pending_is_destructor = true;
    }
    /* operator-function-id — N4659 §16.5 [over.oper]
     *   operator-function-id: operator operator-symbol
     * Handles: operator+, operator[], operator(), operator<<, etc. */
    if (!name && parser_at(p, TK_KW_OPERATOR)) {
parse_operator_id:
        name = parser_advance(p);  /* consume 'operator' */
        /* conversion-function-id — N4659 §16.3.2 [class.conv.fct]
         *   operator conversion-type-id
         * The conversion-type-id IS the return type of the function
         * (§16.3.2/1: "A conversion function shall have no parameters
         * and specifies a conversion from its class type to the type
         * specified by the conversion-type-id"). Parse it and overwrite
         * ty/base_ty — which were set to the TY_VOID placeholder
         * produced by parse_type_specifiers for operator-id syntax —
         * so the function type built at parse_suffixes uses the
         * conversion target as ret. */
        if (parser_at_type_specifier(p)) {
            DeclSpec cspec = parse_type_specifiers(p);
            Type *conv_ty = cspec.type;
            /* Optional ptr/ref operators on the conversion target */
            for (;;) {
                if (parser_consume(p, TK_STAR)) {
                    conv_ty = new_ptr_type(p, conv_ty);
                    while (parser_at(p, TK_KW_CONST) || parser_at(p, TK_KW_VOLATILE)) {
                        if (parser_consume(p, TK_KW_CONST))    conv_ty->is_const = true;
                        if (parser_consume(p, TK_KW_VOLATILE)) conv_ty->is_volatile = true;
                    }
                } else if (parser_consume(p, TK_LAND)) {
                    conv_ty = new_rvalref_type(p, conv_ty);
                } else if (parser_consume(p, TK_AMP)) {
                    conv_ty = new_ref_type(p, conv_ty);
                } else if (parser_consume(p, TK_KW_CONST)) {
                    conv_ty->is_const = true;
                } else if (parser_consume(p, TK_KW_VOLATILE)) {
                    conv_ty->is_volatile = true;
                } else {
                    break;
                }
            }
            base_ty = conv_ty;
            ty = conv_ty;
        }
        /* Consume the operator symbol(s).
         * Special cases: operator() and operator[] are multi-token. */
        else if (parser_consume(p, TK_LPAREN)) {
            parser_expect(p, TK_RPAREN);   /* operator() */
        } else if (parser_consume(p, TK_LBRACKET)) {
            parser_expect(p, TK_RBRACKET); /* operator[] */
        } else if (parser_at(p, TK_KW_NEW) || parser_at(p, TK_KW_DELETE)) {
            parser_advance(p);  /* operator new / operator delete */
            /* operator new[] / operator delete[] */
            if (parser_consume(p, TK_LBRACKET))
                parser_expect(p, TK_RBRACKET);
        } else if (parser_at(p, TK_STR)) {
            /* C++11 user-defined literal — N4659 §16.5.8 [over.literal]
             *   operator-function-id: operator string-literal identifier
             * The lexer fuses '""sv' into a single STR token with the
             * udl suffix on it; we just consume the STR. The IDENT
             * suffix may follow as a separate token in some lexer
             * configurations — accept it. */
            parser_advance(p);
            if (parser_at(p, TK_IDENT)) parser_advance(p);
        } else if (parser_peek(p)->kind >= TK_LPAREN &&
                   parser_peek(p)->kind <= TK_HASHHASH) {
            /* Any single operator/punctuator token */
            parser_advance(p);
        }
    }

parse_suffixes:
    ;  /* empty statement — C11 requires a statement after a label,
        * a declaration alone doesn't satisfy (gcc 4.7 -pedantic). */
    /* Function declarator suffix — N4659 §11.3.5 [dcl.fct]
     *   parameters-and-qualifiers:
     *       ( parameter-declaration-clause ) cv-qualifier-seq(opt)
     *           ref-qualifier(opt) noexcept-specifier(opt)
     *
     * C++11: trailing return type (-> type-id)
     * C++20: requires-clause after param list
     * C++23: explicit object parameter (this auto& self)
     *
     * N4659 §9.8 [stmt.ambig] / §11.2 [dcl.ambig.res]:
     * When we see '(' after a declarator name, it could be:
     *   (a) A function parameter list:  T f(int x);
     *   (b) Direct-initialization:      T x(42);
     *   (c) The "most vexing parse":    T x(T());  — function decl!
     *
     * Per §9.8: "any statement that could be a declaration IS a declaration."
     *
     * Standard rule (§11.2/1 [dcl.ambig.res]): tentatively parse as a
     * parameter-declaration-clause; if it succeeds, that's the
     * interpretation.
     *
     * SHORTCUT (ours, not the standard): we gate entry to the
     * tentative param parse with a one-token-lookahead 'looks_like_params'
     * heuristic — only if the token after '(' could plausibly start a
     * param-decl (type-spec, ')', '...', a known type-name, or a
     * qualified name) do we even attempt the tentative parse. Inside
     * that gate we DO use a real ParseState save/restore so mid-stream
     * failures (e.g. an unparseable param expression) back out to
     * direct-init. The gate prunes the §11.2 algorithm's worst case
     * but is not equivalent to it.
     * TODO(seafront#decl-ambig-gate): drop the gate and run a true
     * §11.2/1 tentative parse once perf allows. */
    /* For qualified declarator-ids (Foo<T>::method), temporarily push
     * the class scope so member typedefs (value_type, etc.) are
     * visible during parameter list parsing. */
    DeclarativeRegion *saved_region_for_params = NULL;
    if (name_was_qualified && p->qualified_decl_scope && !p->tentative) {
        saved_region_for_params = p->region;
        p->region = p->qualified_decl_scope;
    }

    if (parser_at(p, TK_LPAREN) && name) {
        /* Peek inside the parens to decide: parameter list or init?
         * A parameter list starts with: ), void), type-specifier, or ...
         * Anything else (literal, non-type identifier, etc.) is init.
         *
         * BUT: if the declarator name was qualified (Foo::bar,
         * Foo<T>::baz, etc.), '(args)' is overwhelmingly a parameter
         * list — qualified names rarely appear at expression position.
         * Force looks_like_params=true in that case so the heuristic
         * doesn't fall into the most-vexing-parse trap when an arg is
         * a member typedef we can't see ('catalog' in messages<char>).
         */
        Token *after_paren = parser_peek_ahead(p, 1);
        bool looks_like_params =
            name_was_qualified ||
            after_paren->kind == TK_RPAREN ||
            after_paren->kind == TK_ELLIPSIS ||
            after_paren->kind == TK_KW_VOID ||
            /* __attribute__ before a param type: gengtype-generated
             * code uses '__attribute__((unused)) void *this_obj'.
             * Treat __attribute__ as a param-list signal. */
            (after_paren->kind == TK_IDENT &&
             after_paren->len >= 13 &&
             memcmp(after_paren->loc, "__attribute__", 13) == 0) ||
            /* Check if it's a type-specifier keyword or known type/template name */
            (after_paren->kind >= TK_KW_ALIGNAS /* any keyword */ ||
             (after_paren->kind == TK_IDENT &&
              (lookup_is_type_name(p, after_paren) ||
               lookup_is_template_name(p, after_paren))));

        /* For the most vexing parse, we need the full heuristic.
         * Refine: a keyword that's a type-specifier signals params.
         * But never override a qualified-name forced-true. */
        if (!name_was_qualified &&
            after_paren->kind >= TK_KW_ALIGNAS &&
            after_paren->kind <= TK_KW_WHILE) {
            /* It's a keyword — check if it could start a type */
            switch (after_paren->kind) {
            case TK_KW_VOID: case TK_KW_BOOL: case TK_KW_CHAR:
            case TK_KW_SHORT: case TK_KW_INT: case TK_KW_LONG:
            case TK_KW_FLOAT: case TK_KW_DOUBLE:
            case TK_KW_SIGNED: case TK_KW_UNSIGNED:
            case TK_KW_WCHAR_T: case TK_KW_CHAR16_T: case TK_KW_CHAR32_T:
            case TK_KW_CONST: case TK_KW_VOLATILE:
            case TK_KW_STRUCT: case TK_KW_UNION: case TK_KW_ENUM:
            case TK_KW_AUTO: case TK_KW_TYPENAME: case TK_KW_DECLTYPE:
                looks_like_params = true;
                break;
            default:
                looks_like_params = false;
                break;
            }
        } else if (!name_was_qualified && after_paren->kind == TK_IDENT) {
            /* __attribute__ before a param type signals param list. */
            if (after_paren->len >= 13 &&
                memcmp(after_paren->loc, "__attribute__", 13) == 0) {
                looks_like_params = true;
            } else {
                /* A qualified-name (ident::...) is treated as a potential type
                 * — qualified lookup isn't resolved here, but the leading
                 * segment is overwhelmingly a namespace or class scope.
                 * Also: 'IDENT IDENT' inside parens (after a function name)
                 * looks like a parameter declaration with an inherited
                 * member typedef we can't resolve. */
                Token *t2 = parser_peek_ahead(p, 2);
                looks_like_params = lookup_is_type_name(p, after_paren) ||
                                    lookup_is_template_name(p, after_paren) ||
                                    t2->kind == TK_SCOPE ||
                                    t2->kind == TK_STAR || t2->kind == TK_AMP ||
                                    t2->kind == TK_LAND || t2->kind == TK_LT ||
                                    (t2->kind == TK_IDENT &&
                                     !lookup_unqualified(p, t2->loc, t2->len));
            }
        } else if (after_paren->kind == TK_SCOPE) {
            /* '::Foo::Bar' — fully qualified type at start of param list. */
            looks_like_params = true;
        } else if (!name_was_qualified &&
                   after_paren->kind != TK_RPAREN &&
                   after_paren->kind != TK_ELLIPSIS &&
                   after_paren->kind != TK_KW_VOID) {
            looks_like_params = false;
        }

        if (looks_like_params) {
            /* Tentatively try parsing as a parameter list. If anything
             * inside fails (e.g. an unparseable param expression), the
             * '(' was actually a direct-init paren — restore and let the
             * caller handle it as 'T x(args)' init. */
            ParseState saved = parser_save(p);
            bool saved_failed = p->tentative_failed;
            p->tentative_failed = false;
            bool prev_tentative = p->tentative;
            p->tentative = true;
            parser_advance(p);  /* consume ( */
            Vec params = vec_new(p->arena);
            Vec param_types = vec_new(p->arena);
            bool variadic = false;

            if (!parser_at(p, TK_RPAREN)) {
                if (parser_at(p, TK_KW_VOID) &&
                    parser_peek_ahead(p, 1)->kind == TK_RPAREN) {
                    parser_advance(p);  /* (void) — no params */
                } else {
                    /* Terminates: each iteration parses a parameter
                     * (consuming tokens), then breaks on non-comma. */
                    for (;;) {
                        if (parser_consume(p, TK_ELLIPSIS)) {
                            variadic = true;
                            break;
                        }

                        parser_skip_cxx_attributes(p);
                        parser_skip_gnu_attributes(p);
                        DeclSpec pspec = parse_type_specifiers(p);
                        parse_absorb_trailing_cv(p, &pspec);
                        Type *param_base = pspec.type;
                        /* parse_declarator for a parameter must not
                         * leak its qualified_decl_scope / ctor-pending
                         * state into the enclosing declaration. Param
                         * names sometimes coincide with class type
                         * names (e.g. 'bitmap_obstack *obstack'), which
                         * would otherwise stash obstack's class_region
                         * as the enclosing function's qualified scope. */
                        DeclarativeRegion *saved_qds = p->qualified_decl_scope;
                        bool saved_pic = p->pending_is_constructor;
                        bool saved_pid = p->pending_is_destructor;
                        Node *param_decl = parse_declarator(p, param_base);
                        p->qualified_decl_scope = saved_qds;
                        p->pending_is_constructor = saved_pic;
                        p->pending_is_destructor = saved_pid;
                        if (param_decl)
                            param_decl->kind = ND_PARAM;
                        /* C++11 attribute-specifier-seq AFTER the
                         * declarator — libstdc++ uses
                         * 'void f(int x [[gnu::unused]])'. */
                        parser_skip_cxx_attributes(p);
                        parser_skip_gnu_attributes(p);

                        /* Default argument — N4659 §11.3.6 [dcl.fct.default]
                         *   parameter-declaration = assignment-expression
                         * Capture the expr so call sites that pass fewer
                         * args than params can inject it at emit time. */
                        Node *def_val = NULL;
                        if (parser_consume(p, TK_ASSIGN))
                            def_val = parse_assign_expr(p);

                        if (param_decl) {
                            param_decl->param.default_value = def_val;
                            vec_push(&params, param_decl);
                            vec_push(&param_types, param_decl->var_decl.ty);
                        }

                        if (!parser_consume(p, TK_COMMA))
                            break;

                        if (parser_at(p, TK_ELLIPSIS)) {
                            parser_advance(p);
                            variadic = true;
                            break;
                        }
                    }
                }
            }
            parser_expect(p, TK_RPAREN);

            bool inner_failed = p->tentative_failed;
            p->tentative = prev_tentative;
            p->tentative_failed = saved_failed || (prev_tentative && inner_failed);

            if (inner_failed) {
                /* Param parsing failed — '(' was actually direct-init.
                 * Restore the position and let the caller see the '('. */
                parser_restore(p, saved);
                p->tentative_failed = saved_failed;
                if (saved_region_for_params)
                    p->region = saved_region_for_params;
                return new_var_decl_node(p, ty, name, name);
            }

            bool method_const = consume_trailing_qualifiers(p);

            /* C++11 trailing return type: '-> type-id' */
            if (parser_consume(p, TK_ARROW))
                parse_type_name(p);  /* parsed and discarded */

            ty = new_func_type(p, ty, (Type **)param_types.data,
                               param_types.len, variadic);
            /* Extract per-param default values so call-site
             * default-arg injection can find them via callee's
             * TY_FUNC. Only allocate when at least one param has a
             * default. N4659 §11.3.6 [dcl.fct.default]. */
            {
                bool any_default = false;
                for (int i = 0; i < param_types.len; i++) {
                    Node *pn = ((Node **)params.data)[i];
                    if (pn && pn->param.default_value) { any_default = true; break; }
                }
                if (any_default) {
                    Node **defs = arena_alloc(p->arena,
                        param_types.len * sizeof(Node *));
                    for (int i = 0; i < param_types.len; i++) {
                        Node *pn = ((Node **)params.data)[i];
                        defs[i] = pn ? pn->param.default_value : NULL;
                    }
                    ty->param_defaults = defs;
                }
            }
            /* N4659 §10.1.7.1 [dcl.type.cv]: const/volatile
             * after the parameter list qualifies the implicit
             * object parameter (§16.3.1/4). Store on the function
             * type so mangling can distinguish overloads. */
            if (method_const) ty->is_const = true;

            /* Apply deferred wrappers from a grouped declarator
             * (see the grouped branch — inner '*' binds AFTER outer
             * suffix). Example: int (*fp)(int). */
            for (int i = pending_nwrap - 1; i >= 0; i--) {
                Type *w = pending_wrap[i];
                ty = apply_pending_wrap(p, ty, w);
            }

            Node *node = new_var_decl_node(p, ty, name,
                                           name ? name : parser_peek(p));
            node->func.params = (Node **)params.data;
            node->func.nparams = params.len;
            node->func.is_variadic = variadic;
            if (saved_region_for_params)
                p->region = saved_region_for_params;
            return node;
        }
        /* else: not a parameter list — fall through, leave ( for caller */
    }
    /* Restore the saved region after qualified-param scope push. */
    if (saved_region_for_params)
        p->region = saved_region_for_params;

    /* Unnamed declarator: '(' starts parameter list ONLY if what follows
     * is plausibly a parameter (type-spec, ')', '...'). Otherwise leave
     * '(' for the caller — it may be direct-init in a tentative parse. */
    if (!name && parser_at(p, TK_LPAREN)) {
        Token *after = parser_peek_ahead(p, 1);
        Token *after2 = parser_peek_ahead(p, 2);
        bool plausible =
            after->kind == TK_RPAREN || after->kind == TK_ELLIPSIS ||
            after->kind == TK_SCOPE ||
            (after->kind >= TK_KW_ALIGNAS && after->kind <= TK_KW_WHILE) ||
            (after->kind == TK_IDENT &&
             (lookup_is_type_name(p, after) ||
              lookup_is_template_name(p, after) ||
              after2->kind == TK_SCOPE ||
              after2->kind == TK_STAR || after2->kind == TK_AMP ||
              after2->kind == TK_LAND ||
              (after2->kind == TK_IDENT &&
               !lookup_unqualified(p, after2->loc, after2->len))));
        if (!plausible)
            return new_var_decl_node(p, ty, name, parser_peek(p));
        /* Tentatively parse the parameter list — same fallback as the
         * named-decl path. If anything fails, restore and treat '(' as
         * direct-init. */
        ParseState saved = parser_save(p);
        bool saved_failed = p->tentative_failed;
        p->tentative_failed = false;
        bool prev_tentative = p->tentative;
        p->tentative = true;
        parser_advance(p);  /* consume ( */
        Vec params = vec_new(p->arena);
        Vec param_types = vec_new(p->arena);
        bool variadic = false;

        if (!parser_at(p, TK_RPAREN)) {
            if (parser_at(p, TK_KW_VOID) &&
                parser_peek_ahead(p, 1)->kind == TK_RPAREN) {
                parser_advance(p);
            } else {
                /* Terminates: same as named-param loop above. */
                for (;;) {
                    if (parser_consume(p, TK_ELLIPSIS)) { variadic = true; break; }
                    parser_skip_cxx_attributes(p);
                    parser_skip_gnu_attributes(p);
                    DeclSpec pspec2 = parse_type_specifiers(p);
                    parse_absorb_trailing_cv(p, &pspec2);
                    Type *param_base = pspec2.type;
                    /* Save/restore — see the matching block above. */
                    DeclarativeRegion *saved_qds = p->qualified_decl_scope;
                    bool saved_pic = p->pending_is_constructor;
                    bool saved_pid = p->pending_is_destructor;
                    Node *param_decl = parse_declarator(p, param_base);
                    p->qualified_decl_scope = saved_qds;
                    p->pending_is_constructor = saved_pic;
                    p->pending_is_destructor = saved_pid;
                    if (param_decl) param_decl->kind = ND_PARAM;
                    /* Trailing attribute-specifier-seq after the
                     * declarator (libstdc++ uses [[gnu::unused]]). */
                    parser_skip_cxx_attributes(p);
                    parser_skip_gnu_attributes(p);
                    Node *def_val2 = NULL;
                    if (parser_consume(p, TK_ASSIGN))
                        def_val2 = parse_assign_expr(p);  /* default arg — §11.3.6 */
                    if (param_decl) {
                        param_decl->param.default_value = def_val2;
                        vec_push(&params, param_decl);
                        vec_push(&param_types, param_decl->var_decl.ty);
                    }
                    if (!parser_consume(p, TK_COMMA)) break;
                    if (parser_at(p, TK_ELLIPSIS)) { parser_advance(p); variadic = true; break; }
                }
            }
        }
        parser_expect(p, TK_RPAREN);

        bool inner_failed = p->tentative_failed;
        p->tentative = prev_tentative;
        p->tentative_failed = saved_failed || (prev_tentative && inner_failed);

        if (inner_failed) {
            parser_restore(p, saved);
            p->tentative_failed = saved_failed;
            /* Apply pending wrappers before returning, so a grouped
             * declarator '(*name)' that's followed by a non-parens
             * suffix (direct-init lookalike) still gets the ptr. */
            for (int i = pending_nwrap - 1; i >= 0; i--) {
                Type *w = pending_wrap[i];
                ty = apply_pending_wrap(p, ty, w);
            }
            return new_var_decl_node(p, ty, name, parser_peek(p));
        }

        {
            bool mc = consume_trailing_qualifiers(p);

            ty = new_func_type(p, ty, (Type **)param_types.data,
                               param_types.len, variadic);
            /* Capture per-param defaults — same as the other
             * new_func_type call site. Pattern: gcc 4.8 rtl.h
             * 'extern rtx *strip_address_mutations(rtx*, enum* = 0);'. */
            {
                bool any_default = false;
                for (int i = 0; i < param_types.len; i++) {
                    Node *pn = ((Node **)params.data)[i];
                    if (pn && pn->param.default_value) { any_default = true; break; }
                }
                if (any_default) {
                    Node **defs = arena_alloc(p->arena,
                        param_types.len * sizeof(Node *));
                    for (int i = 0; i < param_types.len; i++) {
                        Node *pn = ((Node **)params.data)[i];
                        defs[i] = pn ? pn->param.default_value : NULL;
                    }
                    ty->param_defaults = defs;
                }
            }
            if (mc) ty->is_const = true;
        }

        /* Apply deferred wrappers from a grouped declarator (reverse
         * order — innermost modifier wraps first). */
        for (int i = pending_nwrap - 1; i >= 0; i--) {
            Type *w = pending_wrap[i];
            ty = apply_pending_wrap(p, ty, w);
        }

        Node *node = new_var_decl_node(p, ty, name,
                                       name ? name : parser_peek(p));
        node->func.params = (Node **)params.data;
        node->func.nparams = params.len;
        node->func.is_variadic = variadic;
        return node;
    }

    /* Array declarator suffix — N4659 §11.3.4 [dcl.array]
     *   noptr-declarator [ constant-expression(opt) ] attribute-specifier-seq(opt)
     *
     * Be careful: '[[' is the C++ attribute-specifier opener, not an
     * unsized-array bracket. Skip attributes here so they don't get
     * misparsed as array dimensions. */
    parser_skip_cxx_attributes(p);
    while (parser_at(p, TK_LBRACKET) &&
           parser_peek_ahead(p, 1)->kind != TK_LBRACKET) {
        parser_advance(p);  /* consume [ */
        int len = -1;  /* unsized / expression-sized */
        Node *size_expr = NULL;
        if (!parser_at(p, TK_RBRACKET)) {
            /* N4659 §11.3.4 [dcl.array]: the size is a constant-
             * expression. For integer literals we extract the value
             * into array_len. For non-literal expressions (macros
             * that expand to sizeof(...)/N, template params, etc.)
             * we keep the Node so codegen can re-emit it verbatim —
             * essential for sys-header structs like sigset_t whose
             * size is 1024/(8*sizeof(long)). */
            size_expr = parse_assign_expr(p);
            if (size_expr && size_expr->kind == ND_NUM)
                len = (int)size_expr->num.lo;
        }
        parser_expect(p, TK_RBRACKET);
        ty = new_array_type(p, ty, len);
        if (len < 0 && size_expr && size_expr->kind != ND_NUM)
            ty->array_size_expr = size_expr;
    }

    /* Apply deferred wrappers from a grouped declarator — see the
     * grouped-declarator branch for the rationale. */
    for (int i = pending_nwrap - 1; i >= 0; i--) {
        Type *w = pending_wrap[i];
        ty = apply_pending_wrap(p, ty, w);
    }

    return new_var_decl_node(p, ty, name, name ? name : parser_peek(p));
}

/*
 * parse_declaration — N4659 §10 [dcl.dcl]
 *
 * Parses a simple-declaration or function-definition.
 *
 *   simple-declaration:
 *       decl-specifier-seq init-declarator-list(opt) ;
 *
 *   function-definition:
 *       decl-specifier-seq declarator function-body
 *
 *   init-declarator:
 *       declarator initializer(opt)
 *
 *   initializer:
 *       brace-or-equal-initializer
 *       ( expression-list )             (direct-init — handled)
 *
 *   brace-or-equal-initializer:
 *       = initializer-clause
 *       braced-init-list               (handled)
 */

/*
 * consume_trailing_qualifiers — helper used by parse_declarator's
 * parameter-list path to consume the qualifiers that may follow
 * the closing ')' of a function declarator.
 *
 * N4659 §11.3.5 [dcl.fct]: parameters-and-qualifiers includes
 *   cv-qualifier-seq(opt) ref-qualifier(opt) noexcept-specifier(opt)
 *
 * Also consumes virt-specifiers (override, final) per N4659 §13.3
 * [class.virtual] (renumbered from §12.3 in older drafts), the
 * deprecated-but-still-pervasive throw-spec from §15.4 [except.spec],
 * the pure-specifier '= 0', and the C++11 trailing-return-type
 * arrow.
 *
 * Terminates: each iteration consumes a qualifier token or breaks.
 */
static bool consume_trailing_qualifiers(Parser *p) {
    bool saw_const = false;
    for (;;) {
        if (parser_consume(p, TK_KW_CONST))    { saw_const = true; continue; }
        if (parser_consume(p, TK_KW_VOLATILE)) continue;
        if (parser_consume(p, TK_KW_NOEXCEPT)) {
            /* noexcept(expr) — N4659 §15.4 [except.spec]. Skip
             * balanced parens; the expression is consumed and
             * discarded. */
            if (parser_consume(p, TK_LPAREN)) {
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
            }
            continue;
        }
        break;
    }
    /* throw(type-id-list(opt)) — N4659 §15.4 [except.spec], deprecated
     * in C++17 but still pervasive in libstdc++ headers. */
    if (parser_consume(p, TK_KW_THROW)) {
        parser_expect(p, TK_LPAREN);
        int depth = 1;
        while (depth > 0 && !parser_at_eof(p)) {
            if (parser_at(p, TK_LPAREN)) depth++;
            else if (parser_at(p, TK_RPAREN)) { depth--; if (depth == 0) break; }
            parser_advance(p);
        }
        parser_expect(p, TK_RPAREN);
    }
    parser_skip_gnu_attributes(p);
    /* override / final — N4659 §13.3 [class.virtual]. Contextual
     * keywords (not reserved); we recognise by name. */
    while (parser_at(p, TK_IDENT) &&
           (token_equal(parser_peek(p), "override") ||
            token_equal(parser_peek(p), "final")))
        parser_advance(p);
    parser_consume(p, TK_AMP);
    parser_consume(p, TK_LAND);
    if (parser_consume(p, TK_KW_NOEXCEPT)) {
        if (parser_consume(p, TK_LPAREN)) {
            parse_expr(p);
            parser_expect(p, TK_RPAREN);
        }
    }
    /* pure-specifier '= 0' — N4659 §13.3/2 [class.virtual]. Marks
     * a virtual method as pure (abstract). We consume the tokens
     * but don't yet propagate the abstractness flag. */
    if (parser_at(p, TK_ASSIGN) && parser_peek_ahead(p, 1)->kind == TK_NUM) {
        parser_advance(p);
        parser_advance(p);
    }
    /* trailing-return-type — N4659 §11.3.5 [dcl.fct]
     *   parameters-and-qualifiers trailing-return-type
     *   trailing-return-type: -> type-id
     * Parse and discard; the return type isn't propagated yet. */
    if (parser_consume(p, TK_ARROW))
        parse_type_name(p);
    return saw_const;
}

/*
 * parse_func_body — push prototype scope, register parameters, parse
 * the optional ctor-initializer list and the function-body compound
 * statement, then pop the prototype scope. Used by both the eager
 * function-definition path in parse_declaration and the deferred
 * replay path in parse_class_body (for inline member functions
 * defined inside a class — N4659 §6.4.7/1 [class.mem]/6).
 *
 * Pre: parser is positioned at the ':' (ctor-initializer) or '{'
 * (function body).
 * Post: parser is just past the closing '}' of the body.
 */
static void parse_func_body(Parser *p, Node *func) {
    /* N4659 §6.3.4 [basic.scope.proto]: function parameters have
     * function prototype scope. The compound-statement pushes its
     * own REGION_BLOCK as a child. */
    region_push(p, REGION_PROTOTYPE, /*name=*/NULL);
    func->func.param_scope = p->region;
    for (int i = 0; i < func->func.nparams; i++) {
        Node *param = func->func.params[i];
        if (param->param.name)
            region_declare(p, param->param.name->loc,
                          param->param.name->len, ENTITY_VARIABLE,
                          param->param.ty);
    }

    /* ctor-initializer — N4659 §15.6.2 [class.base.init]
     *   ': ' mem-initializer-list
     *   mem-initializer: identifier ( expression-list(opt) )
     *                  | identifier braced-init-list
     *                  | identifier ... (pack expansion)
     * Captures simple 'name(args)' entries. Templates, base-class
     * inits with template arguments, and braced-init-lists fall
     * through to a skip path that discards them — these will get
     * proper parsing once we need them. */
    if (parser_consume(p, TK_COLON)) {
        Vec inits = vec_new(p->arena);
        for (;;) {
            /* mem-initializer — N4659 §15.6.2 [class.base.init]:
             *   mem-initializer-id ( expression-list(opt) )     C++03 form
             *   mem-initializer-id braced-init-list             C++11 uniform init
             * Both capture the same (name, args) shape; the choice of
             * paren vs. brace is only a syntactic wrapping. Pattern from
             * libstdc++ 13 atomic_base.h / stl_list.h / bitset:
             *   : _M_to{__to}, _M_goff{-1, -1, -1}
             *   : __atomic_flag_base{ _S_init(__i) } */
            bool simple = parser_at(p, TK_IDENT) &&
                          (parser_peek_ahead(p, 1)->kind == TK_LPAREN ||
                           parser_peek_ahead(p, 1)->kind == TK_LBRACE);
            if (simple) {
                Token *member_name = parser_advance(p);
                bool braced = parser_at(p, TK_LBRACE);
                parser_advance(p);  /* '(' or '{' */
                TokenKind close = braced ? TK_RBRACE : TK_RPAREN;
                Vec args = vec_new(p->arena);
                if (!parser_at(p, close)) {
                    vec_push(&args, parse_assign_expr(p));
                    parser_consume(p, TK_ELLIPSIS);
                    while (parser_consume(p, TK_COMMA)) {
                        vec_push(&args, parse_assign_expr(p));
                        parser_consume(p, TK_ELLIPSIS);
                    }
                }
                parser_expect(p, close);

                MemInit *mi = arena_alloc(p->arena, sizeof(MemInit));
                mi->name = member_name;
                mi->args = (Node **)args.data;
                mi->nargs = args.len;
                vec_push(&inits, mi);
            } else {
                int angle = 0;
                while (!parser_at_eof(p)) {
                    TokenKind k = parser_peek(p)->kind;
                    if (k == TK_LT) angle++;
                    else if (k == TK_GT) {
                        if (angle == 0) break;
                        angle--;
                    } else if (k == TK_SHR) {
                        if (angle == 0) break;
                        angle -= 2;
                        if (angle < 0) angle = 0;
                    } else if (k == TK_COMMA) {
                        if (angle == 0) break;
                    } else if (k == TK_LPAREN || k == TK_LBRACE) {
                        if (angle == 0) break;
                    }
                    parser_advance(p);
                }
                if (parser_consume(p, TK_LPAREN)) {
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
                } else if (parser_consume(p, TK_LBRACE)) {
                    int depth = 1;
                    while (depth > 0 && !parser_at_eof(p)) {
                        if (parser_at(p, TK_LBRACE)) depth++;
                        else if (parser_at(p, TK_RBRACE)) {
                            depth--;
                            if (depth == 0) break;
                        }
                        parser_advance(p);
                    }
                    parser_expect(p, TK_RBRACE);
                }
            }
            parser_consume(p, TK_ELLIPSIS);
            if (!parser_consume(p, TK_COMMA)) break;
        }
        int n = inits.len;
        if (n > 0) {
            MemInit *arr = arena_alloc(p->arena, sizeof(MemInit) * n);
            for (int i = 0; i < n; i++)
                arr[i] = *((MemInit **)inits.data)[i];
            func->func.mem_inits = arr;
            func->func.n_mem_inits = n;
        }
    }

    func->func.body = parse_compound_stmt(p);
    region_pop(p);  /* pop prototype scope */
}

/*
 * parse_deferred_func_body — replay a captured in-class member
 * function body. The class scope is pushed back onto the lookup
 * chain so that members declared *after* this function in the
 * class body are visible to its body's name lookups (the
 * complete-class context rule). Pre: func->func.body_start_pos
 * captured by the eager pass.
 */
void parse_deferred_func_body(Parser *p, Node *func) {
    int saved_pos = p->pos;
    DeclarativeRegion *saved_region = p->region;
    bool saved_split_shr = p->split_shr;
    p->split_shr = false;
    p->pos = func->func.body_start_pos;
    p->region = func->func.deferred_class_region;
    parse_func_body(p, func);
    /* Sanity: we should be at or just past body_end_pos. */
    p->pos = saved_pos;
    p->region = saved_region;
    p->split_shr = saved_split_shr;
    /* Mark as no longer deferred (in case anything walks twice). */
    func->func.body_start_pos = -1;
}

/* Skip GCC __extension__ keyword if present. */
static void skip_extension(Parser *p) {
    while (parser_peek(p)->kind == TK_IDENT &&
           parser_peek(p)->len == 13 &&
           memcmp(parser_peek(p)->loc, "__extension__", 13) == 0)
        parser_advance(p);
}

Node *parse_declaration(Parser *p) {
    /* Leading C++11 attributes and GCC __extension__. */
    parser_skip_cxx_attributes(p);
    parser_skip_gnu_attributes(p);
    skip_extension(p);

    Token *start_tok = parser_peek(p);

    /* using-declaration / using-directive / alias-declaration may appear
     * inside class bodies (§12.2.4) and as the inner decl of a template.
     * Delegate to the top-level handler which knows all forms. */
    if (parser_at(p, TK_KW_USING))
        return parse_top_level_decl(p);

    /* static_assert can also appear in member-specification (§10.1.4). */
    if (parser_at(p, TK_KW_STATIC_ASSERT))
        return parse_top_level_decl(p);

    /* typedef — N4659 §10.1.3 [dcl.typedef]
     *   typedef-name: identifier
     *
     * N4659 §6.3.2/1 [basic.scope.pdecl]: the point of declaration
     * is after the complete declarator, so the typedef name becomes
     * visible for subsequent declarations in the same scope.
     *
     * C++11 alias-declaration ('using identifier = type-id') is
     * handled separately via the TK_KW_USING branch above. */
    if (parser_consume(p, TK_KW_TYPEDEF)) {
        DeclSpec spec = parse_type_specifiers(p);
        parse_absorb_trailing_cv(p, &spec);
        Type *base_ty = spec.type;
        Node *decl = parse_declarator(p, base_ty);
        /* GCC __attribute__ after typedef declarator — e.g.
         * 'typedef int register_t __attribute__((__mode__(__word__)));'
         * Common in system headers (sys/types.h).
         *
         * Recognise __mode__(X) to upgrade the declared type's size.
         * Without this, 'typedef unsigned int word_type
         *   __attribute__((__mode__(__word__)))' becomes a plain
         * 32-bit unsigned int in sea-front, but the surrounding
         * libcpp lex code assumes the native word size (64-bit on
         * x86_64), producing out-of-bounds reads in search_line_acc_char.
         * Not in ISO C++ — gcc extension. */
        Token *mode_tok = NULL;
        parser_skip_gnu_attributes_with_mode(p, &mode_tok);
        if (mode_tok && decl && decl->var_decl.ty) {
            const char *m = mode_tok->loc;
            int ml = mode_tok->len;
            Type *new_ty = NULL;
            if ((ml == 8 && memcmp(m, "__word__", 8) == 0) ||
                (ml == 6 && memcmp(m, "__DI__", 6) == 0) ||
                (ml == 2 && memcmp(m, "DI", 2) == 0)) {
                new_ty = new_type(p, TY_LONG);
            } else if ((ml == 6 && memcmp(m, "__SI__", 6) == 0) ||
                       (ml == 2 && memcmp(m, "SI", 2) == 0)) {
                new_ty = new_type(p, TY_INT);
            } else if ((ml == 6 && memcmp(m, "__HI__", 6) == 0) ||
                       (ml == 2 && memcmp(m, "HI", 2) == 0)) {
                new_ty = new_type(p, TY_SHORT);
            } else if ((ml == 6 && memcmp(m, "__QI__", 6) == 0) ||
                       (ml == 2 && memcmp(m, "QI", 2) == 0)) {
                new_ty = new_type(p, TY_CHAR);
            }
            if (new_ty) {
                new_ty->is_unsigned = decl->var_decl.ty->is_unsigned;
                new_ty->is_const    = decl->var_decl.ty->is_const;
                new_ty->is_volatile = decl->var_decl.ty->is_volatile;
                decl->var_decl.ty = new_ty;
            }
        }

        /* Register the first typedef-name into the current declarative region */
        if (decl->var_decl.name) {
            region_declare(p, decl->var_decl.name->loc,
                          decl->var_decl.name->len, ENTITY_TYPE,
                          decl->var_decl.ty);
            /* For anonymous types: set the tag to the typedef name */
            if (decl->var_decl.ty && !decl->var_decl.ty->tag) {
                if (decl->var_decl.ty->kind == TY_ENUM)
                    decl->var_decl.ty->tag = decl->var_decl.name;
                else if ((decl->var_decl.ty->kind == TY_STRUCT ||
                          decl->var_decl.ty->kind == TY_UNION) &&
                         decl->var_decl.ty->class_def)
                    decl->var_decl.ty->tag = decl->var_decl.name;
            }
        }

        /* Comma-separated typedef declarators:
         * 'typedef struct S s_t, *s_ptr;'
         * Each additional declarator shares the base type. */
        while (parser_consume(p, TK_COMMA)) {
            Node *next_decl = parse_declarator(p, base_ty);
            parser_skip_gnu_attributes(p);
            if (next_decl->var_decl.name)
                region_declare(p, next_decl->var_decl.name->loc,
                              next_decl->var_decl.name->len, ENTITY_TYPE,
                              next_decl->var_decl.ty);
        }

        parser_expect(p, TK_SEMI);

        /* Reset the qualified-decl / pending-ctor state that
         * parse_declarator may have stashed while parsing the
         * typedef name. 'typedef struct Item { ... } Item;' has the
         * typedef name 'Item' resolving to the just-created struct
         * tag, which sets qualified_decl_scope to Item's class_region.
         * Without this reset, the NEXT declaration (e.g. 'int main()')
         * picks it up and gets class_type=Item. Same pattern as the
         * param-declarator save/restore. */
        p->qualified_decl_scope = NULL;
        p->qualified_decl_tid = NULL;
        p->pending_is_constructor = false;
        p->pending_is_destructor = false;

        return new_typedef_node(p, decl->var_decl.ty, decl->var_decl.name,
                                start_tok);
    }

    /* friend declaration — N4659 §14.3 [class.friend]
     *   friend declaration
     *   friend function-definition
     *
     * §14.3/1: "A friend of a class is a function or class that is
     * given permission to use the private and protected member names
     * from the class."
     *
     * §14.3/11: "For a friend class declaration, if there is no prior
     * declaration, the class that is specified belongs to the innermost
     * enclosing non-class scope, but if it is subsequently referenced,
     * its name is not found by name lookup until a matching declaration
     * is provided in the innermost enclosing non-class scope."
     *
     * SHORTCUT (ours, not the standard): §14.3/11 says the friend
     * declaration must NOT make the name visible to lookup until a
     * matching real declaration appears. We eagerly register the
     * friend's name in the enclosing namespace so template-id
     * parsing finds it. Safe for valid bootstrap input (where the
     * real declaration exists somewhere); would mis-accept code that
     * relies on §14.3/11 hiding.
     * TODO(seafront#friend-visibility): honour §14.3/11 if/when
     * sema needs accurate friend-only visibility.
     *
     * The inner declaration is wrapped in ND_FRIEND for sema. */
    if (parser_consume(p, TK_KW_FRIEND)) {
        Node *inner;
        if (parser_at(p, TK_KW_TEMPLATE))
            inner = parse_template_declaration(p);
        else
            inner = parse_declaration(p);

        Node *node = new_node(p, ND_FRIEND, start_tok);
        node->friend_decl.decl = inner;
        return node;
    }

    /* decl-specifier-seq — §10.1 [dcl.spec] */
    DeclSpec spec = parse_type_specifiers(p);
    parse_absorb_trailing_cv(p, &spec);
    /* GCC __attribute__ between type specifier and declarator:
     * 'struct stat __attribute__((unused)) buf;'. */
    parser_skip_gnu_attributes(p);
    Type *base_ty = spec.type;
    Node *class_def = spec.class_def;
    if (!base_ty) {
        if (p->tentative) {
            p->tentative_failed = true;
            return NULL;
        }
        error_tok(start_tok, "expected declaration");
    }

    /* Bare type with no declarator: 'struct Foo { ... };' */
    if (parser_at(p, TK_SEMI)) {
        parser_advance(p);
        if (class_def)
            return class_def;
        return new_var_decl_node(p, base_ty, /*name=*/NULL, start_tok);
    }

    /* declarator — §11.3 [dcl.meaning] */
    Node *decl = parse_declarator(p, base_ty);

    /* Function definition: type + declarator(func-type) + '{' body '}'
     *
     * N4659 §6.3.4 [basic.scope.proto]: function parameters have
     * function prototype scope. We push a REGION_PROTOTYPE before
     * parsing the body so parameter names are visible inside.
     * The compound-statement pushes its own REGION_BLOCK as a child. */
    if (decl->var_decl.ty && decl->var_decl.ty->kind == TY_FUNC &&
        (parser_at(p, TK_LBRACE) || parser_at(p, TK_COLON))) {

        Node *func = new_node(p, ND_FUNC_DEF, decl->tok);
        func->func.ret_ty = decl->var_decl.ty->ret;
        func->func.name = decl->var_decl.name;
        func->func.params = decl->func.params;
        func->func.nparams = decl->func.nparams;
        func->func.is_variadic = decl->func.is_variadic;
        func->func.body = NULL;
        func->func.param_scope = NULL;
        func->func.class_type = NULL;
        func->func.is_destructor = p->pending_is_destructor;
        p->pending_is_destructor = false;
        func->func.is_constructor = p->pending_is_constructor;
        p->pending_is_constructor = false;
        func->func.is_virtual = (spec.flags & DECL_VIRTUAL) != 0;
        func->func.is_const_method = decl->var_decl.ty->is_const;
        func->func.storage_flags = spec.flags;
        func->func.body_start_pos = -1;
        func->func.body_end_pos = -1;
        func->func.deferred_class_region = NULL;

        /* Register the function name in the enclosing scope */
        if (func->func.name)
            region_declare(p, func->func.name->loc,
                          func->func.name->len, ENTITY_VARIABLE,
                          decl->var_decl.ty);

        /* Out-of-class definition 'void Foo::bar() { ... }': swap the
         * current region for Foo's class_region so the body resolves
         * Foo's members via lookup. The newly pushed prototype/block
         * regions get qscope as their enclosing chain; we restore the
         * original region after the body. */
        DeclarativeRegion *qscope = p->qualified_decl_scope;
        p->qualified_decl_scope = NULL;
        Node *qtid = p->qualified_decl_tid;
        p->qualified_decl_tid = NULL;
        DeclarativeRegion *saved_region = p->region;
        if (qscope && !p->tentative) {
            /* N4659 §17.7/4 [temp.res]: in a template OOL definition
             * like 'template<T,A> void vec<T,A,vl_ptr>::release()',
             * the body must see BOTH the class members (via qscope)
             * AND the template parameters (via the enclosing template
             * scope). Create a shallow copy of the class scope with
             * its enclosing chain re-pointed through the current
             * template scope. We don't modify the original class
             * scope (it's persistent and shared across parse sites). */
            {
                DeclarativeRegion *wrapper = arena_alloc(p->arena,
                    sizeof(DeclarativeRegion));
                *wrapper = *qscope;
                wrapper->enclosing = p->region;
                qscope = wrapper;
            }
            p->region = qscope;
            /* Tag this function definition as a method of the class
             * the qualifier resolved to, so codegen can mangle the
             * name and inject a 'this' parameter.
             *
             * Full specialization bound to a concrete class:
             *   template<> template<> inline bool
             *   is_a_helper<cgraph_node>::test(...) { ... }
             * The qualifier is 'is_a_helper<cgraph_node>' — qtid has
             * a fully concrete template-id. Without extra handling,
             * class_type is the primary template's Type and the
             * cgraph_node / varpool_node variants mangle identically,
             * causing link errors. Build a shallow Type copy carrying
             * the concrete template_args so mangle_class_tag appends
             * '_t_cgraph_node_te_'. N4659 §17.7.3 [temp.expl.spec]. */
            Type *eff_class_type = qscope->owner_type;
            if (qtid && qtid->kind == ND_TEMPLATE_ID &&
                qtid->template_id.nargs > 0) {
                bool all_concrete = true;
                for (int i = 0; i < qtid->template_id.nargs; i++) {
                    Node *a = qtid->template_id.args[i];
                    Type *t = (a && a->kind == ND_VAR_DECL) ? a->var_decl.ty : NULL;
                    if (!t || t->kind == TY_DEPENDENT) {
                        all_concrete = false;
                        break;
                    }
                }
                if (all_concrete) {
                    Type *spec = arena_alloc(p->arena, sizeof(Type));
                    *spec = *qscope->owner_type;
                    spec->n_template_args = qtid->template_id.nargs;
                    spec->template_args = arena_alloc(p->arena,
                        spec->n_template_args * sizeof(Type *));
                    for (int i = 0; i < spec->n_template_args; i++) {
                        Node *a = qtid->template_id.args[i];
                        spec->template_args[i] = (a && a->kind == ND_VAR_DECL) ?
                            a->var_decl.ty : NULL;
                    }
                    spec->template_id_node = qtid;
                    eff_class_type = spec;
                }
            }
            if (qscope->kind == REGION_CLASS && qscope->owner_type) {
                func->func.class_type = eff_class_type;
                /* Propagate 'static' from the in-class declaration to
                 * the OOL definition. C++ puts 'static' only on the
                 * declaration (N4659 §10.1.1/6 [dcl.stc]); the OOL
                 * definition doesn't repeat it. Look up the method
                 * in the class scope and copy storage_flags. */
                if (func->func.name) {
                    /* Walk the class body looking for a matching method
                     * declaration. The in-class declaration can be:
                     *   - ND_VAR_DECL with TY_FUNC (plain method)
                     *   - ND_TEMPLATE_DECL wrapping ND_VAR_DECL (member template;
                     *     pattern for is_a_helper<T>::test which is
                     *     'static inline bool test(U*)' inside the class)
                     * We propagate DECL_STATIC from whichever matches.
                     * N4659 §10.1.1/6 [dcl.stc] — 'static' sits on the
                     * declaration, not the out-of-class definition. */
                    Type *oty = qscope->owner_type;
                    if (oty && oty->class_def) {
                        Node *cdef = oty->class_def;
                        for (int mi = 0; mi < cdef->class_def.nmembers; mi++) {
                            Node *mm = cdef->class_def.members[mi];
                            if (!mm) continue;
                            Node *vd = mm;
                            if (vd->kind == ND_TEMPLATE_DECL && vd->template_decl.decl)
                                vd = vd->template_decl.decl;
                            if (vd->kind != ND_VAR_DECL) continue;
                            if (!vd->var_decl.name) continue;
                            if (vd->var_decl.name->len != func->func.name->len) continue;
                            if (memcmp(vd->var_decl.name->loc, func->func.name->loc,
                                       func->func.name->len) != 0) continue;
                            func->func.storage_flags |= (vd->var_decl.storage_flags & DECL_STATIC);
                            break;
                        }
                    }
                }
            }
            /* Preserve the qualifier template-id on the func so the
             * template instantiation pass can bind OOL methods to the
             * matching specialization. */
            func->func.qual_tid = qtid;
        }

        /* In-class member function: defer body parsing until the
         * closing '}' of the class. N4659 §6.4.7/1 [class.mem]/6 —
         * the body is in "complete-class context", so members
         * declared LATER in the class are visible. We capture the
         * token range (mem-init list + body) and store it on the
         * func node; parse_class_body's replay loop walks all
         * deferred bodies after the class is fully parsed. Skipped
         * for tentative parses, out-of-class defs (qscope set —
         * class is already complete), and when the class type isn't
         * yet recorded on the region. */
        if (!p->tentative && qscope == NULL &&
            p->region && p->region->kind == REGION_CLASS &&
            p->region->owner_type) {
            int start = p->pos;
            /* Walk past optional ctor-init list ': ...' to the body
             * '{', tracking parens/braces/angles to handle the
             * mem-init list correctly without parsing it.
             *
             * A mem-init can be 'name(args)' OR 'name{args}' (C++11
             * uniform init). For the brace-init form, the '{' sits at
             * depth 0 but is NOT the body — it's the mem-init's args.
             * Distinguish by looking at the previous token: a '{'
             * preceded by an IDENT (the mem-init-id) opens a brace-
             * init-list; a '{' preceded by ')' / '}' (closing of a
             * previous mem-init) or the colon itself opens the body.
             * Pattern: libstdc++ 13 atomic_base.h
             *   : __atomic_flag_base{ _S_init(__i) } { }
             * N4659 §15.6.2 [class.base.init]. */
            int paren = 0, brace = 0, angle = 0;
            TokenKind prev = TK_EOF;
            while (!parser_at_eof(p)) {
                TokenKind k = parser_peek(p)->kind;
                if (paren == 0 && angle == 0 && brace == 0 &&
                    k == TK_LBRACE) {
                    if (prev == TK_IDENT || prev == TK_GT || prev == TK_SHR) {
                        /* mem-init brace-init ('name{args}' or
                         * 'template_id<...>{args}'). Skip balanced. */
                        brace = 1;
                        parser_advance(p);
                        while (brace > 0 && !parser_at_eof(p)) {
                            TokenKind kk = parser_peek(p)->kind;
                            if (kk == TK_LBRACE) brace++;
                            else if (kk == TK_RBRACE) brace--;
                            parser_advance(p);
                        }
                        prev = TK_RBRACE;
                        continue;
                    }
                    /* Top of body */
                    brace = 1;
                    parser_advance(p);
                    while (brace > 0 && !parser_at_eof(p)) {
                        TokenKind kk = parser_peek(p)->kind;
                        if (kk == TK_LBRACE) brace++;
                        else if (kk == TK_RBRACE) brace--;
                        parser_advance(p);
                    }
                    break;
                }
                if (k == TK_LPAREN) paren++;
                else if (k == TK_RPAREN) { if (paren > 0) paren--; }
                else if (k == TK_LT) angle++;
                else if (k == TK_GT) { if (angle > 0) angle--; }
                else if (k == TK_SHR) { if (angle >= 2) angle -= 2; else if (angle > 0) angle = 0; }
                prev = k;
                parser_advance(p);
            }
            int end = p->pos;
            func->func.body_start_pos = start;
            func->func.body_end_pos = end;
            func->func.deferred_class_region = p->region;
            /* Register the function as a class member so subsequent
             * sibling members can see it (already done above for
             * region_declare). Skip prototype-scope push and body
             * parse — they happen at replay time. */
            if (qscope && !p->tentative)
                p->region = saved_region;
            return func;
        }

        /* Push prototype scope and register parameter names */
        parse_func_body(p, func);
        if (qscope && !p->tentative)
            p->region = saved_region;  /* unsplice qscope */
        return func;
    }
    /* Variable with initializer — N4659 §11.6 [dcl.init]
     *
     *   initializer:
     *       brace-or-equal-initializer
     *       ( expression-list )                (direct-initialization)
     *
     *   brace-or-equal-initializer:
     *       = initializer-clause
     *       braced-init-list                   (handled)
     *
     * Direct-initialization T x(expr) is distinguished from a function
     * declarator by the heuristic in parse_declarator() — if the '(' was
     * not consumed as a parameter list, it arrives here as init. */
    /* GCC asm-label / symbol rename:
     *   void foo() __asm("foo_v2");
     * The asm-string gives the function a different external name.
     * Consume and discard — sea-front doesn't yet honor asm-labels. */
    if (parser_at(p, TK_KW_ASM)) {
        parser_advance(p);
        parser_expect(p, TK_LPAREN);
        while (!parser_at_eof(p) && !parser_at(p, TK_RPAREN))
            parser_advance(p);
        parser_expect(p, TK_RPAREN);
    }
    /* Bit-field — N4659 §12.2.4 [class.bit]
     *   member-declarator: identifier(opt) : constant-expression
     * The colon after a declarator name in a class indicates bit-field width.
     * Store the width expression for C emission. */
    if (parser_consume(p, TK_COLON)) {
        decl->var_decl.bitfield_width = parse_assign_expr(p);
    }

    /* GCC __attribute__ between declarator and initializer:
     * 'int x __attribute__((unused)) = 5;'. Common in gcc source. */
    parser_skip_gnu_attributes(p);

    if (parser_consume(p, TK_ASSIGN)) {
        /* = initializer-clause — could be expression or braced-init-list.
         * Both route through parse_assign_expr: a leading '{' is parsed
         * into ND_INIT_LIST (see expr.c TK_LBRACE branch). Aggregate
         * initializers for arrays and plain structs are preserved this
         * way; class types with user-defined ctors should prefer
         * paren-init 'T x(args)' which sets has_ctor_init below. */
        decl->var_decl.init = parse_assign_expr(p);
    } else if (parser_consume(p, TK_LPAREN)) {
        /* Direct-initialization: T x(arg-list) — N4659 §11.6/16
         * Collect ALL the args. Codegen lowers this as
         *   struct T x;
         *   T_ctor(&x, args...);
         * The trailing '...' pack expansion is consumed but not
         * yet propagated. */
        decl->var_decl.has_ctor_init = true;
        Vec args = vec_new(p->arena);
        if (!parser_at(p, TK_RPAREN)) {
            vec_push(&args, parse_assign_expr(p));
            parser_consume(p, TK_ELLIPSIS);
            while (parser_consume(p, TK_COMMA)) {
                vec_push(&args, parse_assign_expr(p));
                parser_consume(p, TK_ELLIPSIS);
            }
        }
        decl->var_decl.ctor_args  = (Node **)args.data;
        decl->var_decl.ctor_nargs = args.len;
        parser_expect(p, TK_RPAREN);
    } else if (parser_consume(p, TK_LBRACE)) {
        /* Direct-list-init 'T x{args}' — N4659 §11.6.4 [dcl.init.list]/3.
         * For class types this picks an overloaded ctor (same as the
         * paren form). Same lowering: capture as ctor_args. */
        decl->var_decl.has_ctor_init = true;
        Vec args = vec_new(p->arena);
        if (!parser_at(p, TK_RBRACE)) {
            vec_push(&args, parse_assign_expr(p));
            parser_consume(p, TK_ELLIPSIS);
            while (parser_consume(p, TK_COMMA)) {
                if (parser_at(p, TK_RBRACE)) break;  /* trailing comma */
                vec_push(&args, parse_assign_expr(p));
                parser_consume(p, TK_ELLIPSIS);
            }
        }
        decl->var_decl.ctor_args  = (Node **)args.data;
        decl->var_decl.ctor_nargs = args.len;
        parser_expect(p, TK_RBRACE);
    }

    /* N4659 §6.3.2/1 [basic.scope.pdecl]: register the variable name */
    if (decl->var_decl.name)
        region_declare(p, decl->var_decl.name->loc,
                      decl->var_decl.name->len, ENTITY_VARIABLE,
                      decl->var_decl.ty);

    /* Comma-separated declarators — N4659 §10 [dcl.dcl]
     *   init-declarator-list:
     *       init-declarator
     *       init-declarator-list , init-declarator
     *
     * Each subsequent declarator shares the same base type (decl-specifier-seq)
     * but may add its own pointer/array/function modifiers.
     * E.g.: int x = 1, *y, z[10]; */
    if (parser_at(p, TK_COMMA)) {
        Vec decls = vec_new(p->arena);
        vec_push(&decls, decl);

        while (parser_consume(p, TK_COMMA)) {
            Node *next_decl = parse_declarator(p, base_ty);

            /* Bit-field width in comma-separated member declarations:
             * 'unsigned int a : 1, b : 1;' */
            if (parser_consume(p, TK_COLON))
                next_decl->var_decl.bitfield_width = parse_assign_expr(p);

            /* GCC __attribute__ between declarator and init/comma:
             * 'int x __attribute__((unused)), y __attribute__((unused));' */
            parser_skip_gnu_attributes(p);

            if (parser_consume(p, TK_ASSIGN))
                next_decl->var_decl.init = parse_assign_expr(p);
            else if (parser_consume(p, TK_LPAREN)) {
                next_decl->var_decl.init = parse_expr(p);
                parser_expect(p, TK_RPAREN);
            } else if (parser_at(p, TK_LBRACE)) {
                /* Braced-init-list 'T x{...}' — parse into ND_INIT_LIST
                 * so aggregate init is preserved. */
                next_decl->var_decl.init = parse_assign_expr(p);
            }

            if (next_decl->var_decl.name)
                region_declare(p, next_decl->var_decl.name->loc,
                              next_decl->var_decl.name->len, ENTITY_VARIABLE,
                              next_decl->var_decl.ty);

            vec_push(&decls, next_decl);
        }

        parser_expect(p, TK_SEMI);

        /* Wrap multiple declarators in a flat block — no braces in C output,
         * so variables remain visible in the enclosing scope. */
        Node *blk = new_block_node(p, (Node **)decls.data, decls.len, start_tok);
        blk->block.is_flat = true;
        return blk;
    }

    /* GCC __attribute__ after declarator — e.g. 'typedef int foo __attribute__((...));'
     * or 'extern void bar(int) __attribute__((noreturn));'. Common in system
     * headers. Already handled inside the function-declarator trailing path
     * (decl.c:829), but may also appear on simple declarations. */
    parser_skip_gnu_attributes(p);
    parser_expect(p, TK_SEMI);
    /* Drop any qualified_decl_scope set by parse_declarator — only the
     * function-def branch consumes it; for plain declarations (incl.
     * declare-only ctors/dtors) it must not leak into the next decl. */
    p->qualified_decl_scope = NULL;
    /* Propagate pending ctor/dtor flags onto the var-decl when this
     * is a method declaration (no body, e.g. 'Foo();' inside a class
     * body). emit_class_def's forward-decl loop reads these to mangle
     * the right way. */
    if (decl) {
        if (p->pending_is_constructor) {
            decl->var_decl.is_constructor = true;
            p->pending_is_constructor = false;
        }
        if (p->pending_is_destructor) {
            decl->var_decl.is_destructor = true;
            p->pending_is_destructor = false;
        }
        if ((spec.flags & DECL_VIRTUAL) &&
            decl->var_decl.ty && decl->var_decl.ty->kind == TY_FUNC)
            decl->var_decl.is_virtual = true;
        decl->var_decl.storage_flags = spec.flags;
    }
    return decl;
}

/*
 * parse_top_level_decl — top-level declaration
 *
 * At file scope, C++ allows:
 *   - function definitions
 *   - variable declarations
 *   - linkage-specification (extern "C")
 *   - namespace definitions
 *   - template declarations
 *   - using-directive / alias-declaration / using-declaration
 *   - empty declarations ( ; )
 */
Node *parse_top_level_decl(Parser *p) {
    skip_extension(p);
    /* Empty declaration — N4659 §10/6 */
    if (parser_consume(p, TK_SEMI))
        return NULL;

    /* Skip preprocessor leftovers — #pragma, #line, etc.
     * These should have been consumed by the preprocessor, but mcpp
     * in independent mode passes through unknown pragmas. We skip
     * everything from # to the end of its line. */
    if (parser_at(p, TK_HASH)) {
        int line = parser_peek(p)->line;
        while (!parser_at_eof(p) && parser_peek(p)->line == line)
            parser_advance(p);
        return NULL;
    }

    /* namespace-definition — N4659 §10.3.1 [namespace.def]
     *   namespace identifier(opt) { namespace-body }
     *   inline namespace identifier(opt) { namespace-body }
     *
     * C++17: nested namespace: namespace A::B::C { ... }
     * C++20: inline nested: namespace A::inline B { ... }
     *
     * Also handles:
     *   using-directive: using namespace name ;  (§10.3.4)
     *   using-declaration: using name ;          (§10.3.3)
     *   alias-declaration: using T = type-id ;   (§10.1.3)
     */
    if (parser_at(p, TK_KW_USING)) {
        Token *tok = parser_advance(p);

        /* using namespace foo; — §10.3.4 [namespace.udir]
         * Find the named namespace's region and add it to the current
         * region's using list so its declarations become visible. */
        if (parser_consume(p, TK_KW_NAMESPACE)) {
            Token *ns_tok = NULL;
            if (parser_at(p, TK_IDENT))
                ns_tok = parser_advance(p);
            /* Skip any qualified parts (A::B::C) for now */
            /* Terminates: advances past :: and ident pairs */
            while (parser_consume(p, TK_SCOPE)) {
                if (parser_at(p, TK_IDENT))
                    ns_tok = parser_advance(p);
            }
            /* Find and register the using-directive */
            if (ns_tok) {
                DeclarativeRegion *ns = region_find_namespace(
                    p, ns_tok->loc, ns_tok->len);
                if (ns)
                    region_add_using(p, ns);
            }
            parser_expect(p, TK_SEMI);
            return NULL;
        }

        /* using T = type-id; — C++11 alias declaration (§10.1.3 [dcl.typedef])
         *   alias-declaration: using identifier = type-id ;
         * Equivalent to: typedef type-id identifier; */
        if (parser_at(p, TK_IDENT) && parser_peek_ahead(p, 1)->kind == TK_ASSIGN) {
            Token *alias_name = parser_advance(p);
            parser_advance(p);  /* consume = */
            Type *ty = parse_type_name(p);
            parser_expect(p, TK_SEMI);

            if (alias_name && ty)
                region_declare(p, alias_name->loc, alias_name->len,
                              ENTITY_TYPE, ty);

            return new_typedef_node(p, ty, alias_name, tok);
        }

        /* using-declaration: using Base::member; — §10.3.3 [namespace.udecl]
         * SHORTCUT (ours, not the standard): we skip the body to ';'
         * without registering the introduced name. The standard
         * (§10.3.3/4) says the using-declaration introduces a name
         * into the current declarative region, which sema needs for
         * lookup of the introduced member. We get away with this
         * because most uses in libstdc++ are pulling base-class
         * members into a derived class — our base-class lookup walk
         * already finds them.
         * TODO(seafront#using-decl): introduce the name properly. */
        /* Terminates: advances toward ; or EOF */
        while (!parser_at(p, TK_SEMI) && !parser_at_eof(p))
            parser_advance(p);
        parser_expect(p, TK_SEMI);
        (void)tok;
        return NULL;
    }

    /* C++11 'inline namespace X' — N4659 §10.3.1 [namespace.def]/8.
     * The 'inline' may precede the 'namespace' keyword. */
    bool is_inline_ns = false;
    if (parser_at(p, TK_KW_INLINE) &&
        parser_peek_ahead(p, 1)->kind == TK_KW_NAMESPACE) {
        is_inline_ns = true;
        parser_advance(p);
    }

    if (parser_at(p, TK_KW_NAMESPACE)) {
        Token *tok = parser_advance(p);

        /* Optional 'inline' namespace */
        if (parser_consume(p, TK_KW_INLINE))
            is_inline_ns = true;

        /* Optional namespace name (unnamed namespaces are valid) */
        Token *ns = NULL;
        if (parser_at(p, TK_IDENT))
            ns = parser_advance(p);

        /* C++17 nested namespace: namespace A::B { }
         * Terminates: each iteration consumes :: and ident */
        while (parser_consume(p, TK_SCOPE)) {
            parser_consume(p, TK_KW_INLINE);  /* C++20: inline nested */
            if (parser_at(p, TK_IDENT))
                parser_advance(p);
        }

        /* N4659 §10.3.1/2 [namespace.def]: a namespace-definition with a
         * given name reopens that namespace if one already exists in the
         * enclosing scope. Reuse the existing region so multiple
         * 'namespace std { }' blocks share state. */
        Declaration *ns_decl = NULL;
        DeclarativeRegion *existing = NULL;
        if (ns) {
            ns_decl = lookup_unqualified_kind(p, ns->loc, ns->len,
                                              ENTITY_NAMESPACE);
            if (ns_decl)
                existing = ns_decl->ns_region;
            else
                ns_decl = region_declare(p, ns->loc, ns->len,
                                         ENTITY_NAMESPACE, /*type=*/NULL);
        }

        /* GCC __attribute__((__abi_tag__(...))) and friends after the
         * namespace name — pervasive in libstdc++. */
        parser_skip_gnu_attributes(p);

        parser_expect(p, TK_LBRACE);

        /* N4659 §6.3.6 [basic.scope.namespace]: push named namespace scope */
        DeclarativeRegion *parent_for_inline = p->region;
        if (existing) {
            existing->enclosing = p->region;
            p->region = existing;
        } else {
            region_push(p, REGION_NAMESPACE, ns);
        }
        /* C++11 inline namespace — N4659 §10.3.1/7: members of an
         * inline namespace are visible from the enclosing namespace
         * as if they had been declared there directly. We model this
         * by registering the inner region as a using-directive of
         * the parent so unqualified lookup from the parent walks
         * into the inline namespace. */
        if (is_inline_ns && parent_for_inline) {
            DeclarativeRegion *saved = p->region;
            p->region = parent_for_inline;
            region_add_using(p, saved);
            p->region = saved;
        }

        Vec decls = vec_new(p->arena);
        /* Terminates: each iteration parses a declaration or hits } / EOF */
        while (!parser_at(p, TK_RBRACE) && !parser_at_eof(p)) {
            if (parser_at(p, TK_HASH)) {
                int line = parser_peek(p)->line;
                while (!parser_at_eof(p) && parser_peek(p)->line == line)
                    parser_advance(p);
                continue;
            }
            Node *decl = parse_top_level_decl(p);
            if (decl)
                vec_push(&decls, decl);
        }
        parser_expect(p, TK_RBRACE);

        /* Stash the namespace region on the declaration so
         * 'using namespace foo' can find it after the region is popped. */
        if (ns_decl)
            ns_decl->ns_region = p->region;

        region_pop(p);

        /* Return as a flat block — namespace body, no braces needed. */
        Node *blk = new_block_node(p, (Node **)decls.data, decls.len, tok);
        blk->block.is_flat = true;
        return blk;
    }

    /* static_assert — N4659 §10.1.4 [dcl.dcl]
     *   static_assert ( constant-expression , string-literal ) ;
     *   C++17: static_assert ( constant-expression ) ;
     * Parse and skip for now. */
    if (parser_at(p, TK_KW_STATIC_ASSERT)) {
        parser_advance(p);
        parser_expect(p, TK_LPAREN);
        /* Terminates: advances through balanced parens toward ) */
        int depth = 1;
        while (depth > 0 && !parser_at_eof(p)) {
            if (parser_at(p, TK_LPAREN)) depth++;
            if (parser_at(p, TK_RPAREN)) depth--;
            if (depth > 0) parser_advance(p);
        }
        parser_expect(p, TK_RPAREN);
        parser_expect(p, TK_SEMI);
        return NULL;
    }

    /* template-declaration — N4659 §17.1 [temp] (Annex A.12)
     *   template < template-parameter-list > declaration */
    if (parser_at(p, TK_KW_TEMPLATE))
        return parse_template_declaration(p);

    /* linkage-specification — N4659 §10.5 [dcl.link]
     *   extern string-literal { declaration-seq(opt) }
     *   extern string-literal declaration
     *
     * The string-literal is "C" or "C++". This controls name mangling:
     * extern "C" suppresses C++ mangling.
     *
     * For now, we parse the syntax and ignore the linkage specifier.
     * The semantic layer will need to track it for name mangling and
     * overload resolution (C linkage prohibits overloading).
     *
     * C++20/23: unchanged. */
    /* extern template explicit instantiation declaration — N4659 §17.7.2
     *   extern template declaration */
    if (parser_at(p, TK_KW_EXTERN) &&
        parser_peek_ahead(p, 1)->kind == TK_KW_TEMPLATE) {
        parser_advance(p);  /* consume 'extern' */
        return parse_template_declaration(p);
    }

    if (parser_at(p, TK_KW_EXTERN) &&
        parser_peek_ahead(p, 1)->kind == TK_STR) {
        parser_advance(p);  /* consume 'extern' */
        parser_advance(p);  /* consume string literal ("C" or "C++") */

        if (parser_at(p, TK_LBRACE)) {
            /* extern "C" { ... } — parse as a block of declarations.
             * We reuse ND_BLOCK to hold the inner declarations.
             * The linkage specifier is ignored for now. */
            Token *brace = parser_peek(p);
            parser_advance(p);
            Vec decls = vec_new(p->arena);
            while (!parser_at(p, TK_RBRACE) && !parser_at_eof(p)) {
                Node *decl = parse_top_level_decl(p);
                if (decl)
                    vec_push(&decls, decl);
            }
            parser_expect(p, TK_RBRACE);
            Node *blk = new_block_node(p, (Node **)decls.data, decls.len, brace);
            blk->block.is_flat = true;
            return blk;
        }

        /* extern "C" single-declaration */
        return parse_declaration(p);
    }

    return parse_declaration(p);
}

/* ------------------------------------------------------------------ */
/* Template declarations — N4659 §17 [temp]                            */
/*                          N4861 §13 [temp] (C++20)                   */
/*                          N4950 §13 [temp] (C++23)                   */
/* ------------------------------------------------------------------ */

/*
 * parse_template_value_default — non-type / template-template
 * template-parameter default value (an assignment-expression).
 *
 * Helper for parse_template_parameter (the call sites are
 * immediately below). N4659 §17.1/8 [temp.param]: a non-type
 * template-parameter default is an assignment-expression.
 * §17.2/3 [temp.names]: inside a template-argument-list (and, by
 * extension, a template-parameter default value), the first non-
 * nested > is the closing delimiter rather than the greater-than
 * operator; >> splits into two >'s.
 *
 * We bump template_depth so the binary-operator parser stops at
 * the top-level >, then call parse_assign_expr. The result is
 * currently discarded — sema doesn't yet substitute defaults at
 * instantiation sites.
 */
static void parse_template_value_default(Parser *p) {
    p->template_depth++;
    (void)parse_assign_expr(p);
    p->template_depth--;
}

/*
 * parse_template_type_default — type-parameter default (a type-id).
 *
 * 'template<typename T = int*>' / 'template<typename T = vector<int>>'.
 * N4659 §17.1/8 [temp.param] applies the same > / >> termination
 * rules as parse_template_value_default. The default is a type-id,
 * not an expression, so this calls parse_type_name instead of
 * parse_assign_expr.
 */
static Type *parse_template_type_default(Parser *p) {
    p->template_depth++;
    Type *ty = parse_type_name(p);
    p->template_depth--;
    return ty;
}

/*
 * parse_template_parameter — parse one template parameter.
 *
 * N4659 §17.1 [temp] (Annex A.12):
 *   template-parameter:
 *       type-parameter
 *       parameter-declaration
 *
 *   type-parameter:
 *       type-parameter-key ...(opt) identifier(opt)
 *       type-parameter-key identifier(opt) = type-id
 *       template < template-parameter-list > type-parameter-key ...(opt) identifier(opt)
 *       template < template-parameter-list > type-parameter-key identifier(opt) = id-expression
 *
 *   type-parameter-key: class | typename
 *
 * If the parameter starts with 'class' or 'typename' (and isn't followed
 * by something that makes it a non-type parameter like 'class Foo' where
 * Foo is already a type), it's a type parameter. Otherwise it's a non-type
 * parameter (parsed as a regular parameter-declaration).
 */
static Node *parse_template_parameter(Parser *p) {
    Token *tok = parser_peek(p);

    /* type-parameter: typename T or class T
     * N4659 §17.1/1: type-parameter-key is 'class' or 'typename' */
    if (tok->kind == TK_KW_TYPENAME || tok->kind == TK_KW_CLASS) {
        /* Disambiguate: 'typename T' (type param) vs 'typename Foo::bar'
         * (dependent name in non-type param) and 'class X' (type
         * param) vs elaborated-type-specifier.
         *
         * SHORTCUT (ours, not the standard): we use a one-token
         * lookahead — if the token after 'typename'/'class' is
         * ident/comma/>/>>/=/..., treat as type-parameter. The
         * standard's grammar (§17.1) requires recognising the FULL
         * type-parameter production, not just its lookahead set; in
         * particular, distinguishing 'typename T' from 'typename
         * T<U>' (a dependent template-id non-type param) needs the
         * trailing-token check below. We get away with the lookahead
         * because real template-parameter-lists don't mix the two
         * forms ambiguously.
         * TODO(seafront#tparam-ambig): true grammar match if needed. */
        Token *next = parser_peek_ahead(p, 1);
        Token *next2 = parser_peek_ahead(p, 2);
        /* 'typename T<...>' or 'typename T::U' is a dependent-type
         * non-type template parameter, not a type-parameter declaration. */
        bool is_dependent_type =
            next->kind == TK_IDENT &&
            (next2->kind == TK_LT || next2->kind == TK_SCOPE);
        if (!is_dependent_type &&
            (next->kind == TK_IDENT || next->kind == TK_COMMA ||
             next->kind == TK_GT || next->kind == TK_SHR ||
             next->kind == TK_ASSIGN || next->kind == TK_ELLIPSIS ||
             next->kind == TK_EOF)) {

            parser_advance(p);  /* consume class/typename */

            /* Optional pack expansion ... */
            bool is_pack = parser_consume(p, TK_ELLIPSIS);
            (void)is_pack;  /* used in later stages */

            /* Optional identifier */
            Token *name = NULL;
            if (parser_at(p, TK_IDENT))
                name = parser_advance(p);

            /* Register the type parameter name in the template scope.
             * N4659 §6.3.9 [basic.scope.temp]: template parameter names
             * are in the template's declarative region.
             *
             * We create a TY_DEPENDENT type so the AST cleanly marks
             * where template parameters appear. During instantiation,
             * subst_type replaces TY_DEPENDENT with the concrete arg. */
            if (name) {
                Type *dep = new_type(p, TY_DEPENDENT);
                dep->tag = name;
                region_declare(p, name->loc, name->len, ENTITY_TYPE, dep);
            }

            /* Optional default: = type-id */
            Type *default_ty = NULL;
            if (parser_consume(p, TK_ASSIGN))
                default_ty = parse_template_type_default(p);

            Node *pnode = new_param_node(p, /*ty=*/NULL, name, tok);
            pnode->param.default_type = default_ty;
            return pnode;
        }
        /* else: fall through to non-type parameter parsing */
    }

    /* template-template parameter — N4659 §17.1 [temp.param]
     *   template < template-parameter-list > type-parameter-key
     *       ...(opt) identifier(opt) (= id-expression)(opt)
     *
     * SHORTCUT (ours, not the standard): we don't recursively parse
     * the inner template-parameter-list — just balance angles and
     * skip. The introduced name is registered as ENTITY_TEMPLATE so
     * lookup of references works; the inner parameter shape is
     * thrown away. Sufficient for our libstdc++ workload.
     * TODO(seafront#tt-param): recurse into the inner parameter
     * list for full sema. */
    if (tok->kind == TK_KW_TEMPLATE) {
        /* Skip nested template<...> */
        parser_advance(p);  /* template */
        parser_expect(p, TK_LT);
        int depth = 1;
        while (depth > 0 && !parser_at_eof(p)) {
            if (parser_at(p, TK_LT)) depth++;
            if (parser_at(p, TK_GT)) { depth--; if (depth == 0) break; }
            if (parser_at(p, TK_SHR) && depth <= 1) { depth--; break; }
            parser_advance(p);
        }
        if (parser_at(p, TK_GT)) parser_advance(p);

        /* Consume class/typename */
        if (parser_at(p, TK_KW_CLASS) || parser_at(p, TK_KW_TYPENAME))
            parser_advance(p);

        /* Optional identifier */
        Token *name = NULL;
        if (parser_at(p, TK_IDENT)) {
            name = parser_advance(p);
            region_declare(p, name->loc, name->len, ENTITY_TEMPLATE, /*type=*/NULL);
        }

        /* Optional default */
        if (parser_consume(p, TK_ASSIGN))
            parse_template_value_default(p);

        return new_param_node(p, /*ty=*/NULL, name, tok);
    }

    /* Non-type template parameter: parsed as a parameter-declaration
     * e.g., 'int N', 'bool B = true', 'auto V', 'size_t... Idx' */
    Type *ty = parse_type_specifiers(p).type;
    /* Pack expansion '...' between the type and the name —
     * 'template<size_t... _Idx>'. */
    parser_consume(p, TK_ELLIPSIS);
    Node *param = parse_declarator(p, ty);
    param->kind = ND_PARAM;

    /* Register non-type parameter name */
    if (param->var_decl.name)
        region_declare(p, param->var_decl.name->loc,
                      param->var_decl.name->len, ENTITY_VARIABLE, ty);

    /* Optional default value: = constant-expression */
    if (parser_consume(p, TK_ASSIGN))
        parse_template_value_default(p);

    return param;
}

/*
 * parse_template_declaration — N4659 §17.1 [temp]
 *
 *   template-declaration:
 *       template < template-parameter-list > declaration
 *
 * N4659 §6.3.9 [basic.scope.temp]:
 *   "The declarative region of the name of a template parameter
 *    is the smallest template-declaration in which the name
 *    was introduced."
 */
Node *parse_template_declaration(Parser *p) {
    Token *tok = parser_expect(p, TK_KW_TEMPLATE);

    /* explicit-specialization: template <> declaration
     * N4659 §17.7.3 [temp.expl.spec]. May be nested:
     * 'template<> template<>' for member templates of specialized classes. */
    if (parser_at(p, TK_LT) && parser_peek_ahead(p, 1)->kind == TK_GT) {
        parser_advance(p);  /* < */
        parser_advance(p);  /* > */
        /* Nested template<>: 'template<> template<> ...' */
        Node *decl;
        if (parser_at(p, TK_KW_TEMPLATE))
            decl = parse_template_declaration(p);
        else
            decl = parse_declaration(p);
        return new_template_decl_node(p, /*params=*/NULL, /*nparams=*/0,
                                      decl, tok);
    }

    /* explicit instantiation — N4659 §17.7.2 [temp.explicit]
     *   template declaration       (no template-parameter-list)
     * E.g. 'template class allocator<char>;'. */
    if (!parser_at(p, TK_LT)) {
        Node *decl = parse_declaration(p);
        return new_template_decl_node(p, /*params=*/NULL, /*nparams=*/0,
                                      decl, tok);
    }

    parser_expect(p, TK_LT);

    /* Push template parameter scope — §6.3.9 [basic.scope.temp] */
    region_push(p, REGION_TEMPLATE, /*name=*/NULL);

    /* Parse template-parameter-list. Increment template_depth so any
     * nested >> in a default value (e.g. 'typename _D = decay_t<_T>>')
     * is split into two > tokens by the lexer-virtual-GT mechanism. */
    p->template_depth++;
    Vec params = vec_new(p->arena);
    if (!parser_at(p, TK_GT) && !parser_at(p, TK_SHR)) {
        vec_push(&params, parse_template_parameter(p));
        while (parser_consume(p, TK_COMMA))
            vec_push(&params, parse_template_parameter(p));
    }
    p->template_depth--;

    parser_expect(p, TK_GT);

    /* Parse the templated declaration.
     * This can be a class, function, variable, alias, or nested template. */
    Node *decl;
    if (parser_at(p, TK_KW_TEMPLATE))
        decl = parse_template_declaration(p);  /* nested template */
    else
        decl = parse_declaration(p);

    /* Pop template parameter scope */
    region_pop(p);

    /* Register the template name in the enclosing scope.
     * The name comes from the inner declaration — may be in different
     * places depending on the declaration kind:
     *   - function: func.name
     *   - variable: var_decl.name
     *   - struct/class/enum with no declarator: var_decl.ty->tag */
    Token *tmpl_name = NULL;
    if (decl) {
        switch (decl->kind) {
        case ND_FUNC_DEF:
        case ND_FUNC_DECL:
            tmpl_name = decl->func.name;
            break;
        case ND_VAR_DECL:
        case ND_TYPEDEF:    /* alias-template: template<...> using X = ...; */
            tmpl_name = decl->var_decl.name;
            /* Bare struct/class/enum: name is in the type's tag */
            if (!tmpl_name && decl->var_decl.ty && decl->var_decl.ty->tag)
                tmpl_name = decl->var_decl.ty->tag;
            break;
        case ND_CLASS_DEF:
            tmpl_name = decl->class_def.tag;
            break;
        case ND_FRIEND:
            /* friend template: extract name from the inner declaration */
            if (decl->friend_decl.decl) {
                Node *inner = decl->friend_decl.decl;
                if (inner->kind == ND_VAR_DECL && inner->var_decl.ty &&
                    inner->var_decl.ty->tag)
                    tmpl_name = inner->var_decl.ty->tag;
                else if (inner->kind == ND_CLASS_DEF)
                    tmpl_name = inner->class_def.tag;
                else if (inner->kind == ND_FUNC_DEF || inner->kind == ND_FUNC_DECL)
                    tmpl_name = inner->func.name;
            }
            break;
        default:
            break;
        }
    }
    /* For a class template, also extract the underlying Type so we
     * can register it as ENTITY_TYPE in the enclosing scope. The
     * Type carries class_region, which lets out-of-class method
     * definitions ('void Foo<T>::bar() { ... }') walk into the
     * class scope at parse time. */
    Type *tmpl_class_type = NULL;
    if (decl) {
        if (decl->kind == ND_CLASS_DEF)
            tmpl_class_type = decl->class_def.ty;
        else if (decl->kind == ND_VAR_DECL && decl->var_decl.ty &&
                 (decl->var_decl.ty->kind == TY_STRUCT ||
                  decl->var_decl.ty->kind == TY_UNION) &&
                 decl->var_decl.ty->tag == tmpl_name)
            tmpl_class_type = decl->var_decl.ty;
        /* Alias template (N4659 §17.6.1 [temp.alias]):
         *   template<typename... _Args> using __uses_alloc_t = ...;
         * names a TYPE, not a function — parser_at_type_specifier's
         * class-template-only check needs to find ENTITY_TYPE for it,
         * so register the aliased type under the template name. */
        else if (decl->kind == ND_TYPEDEF && decl->var_decl.ty)
            tmpl_class_type = decl->var_decl.ty;
    }
    /* Build the ND_TEMPLATE_DECL node first so the registration
     * below can stash a pointer to it on the Declaration — overload
     * resolution reaches the inner function decl via this pointer
     * to run template argument deduction (§17.8.2). */
    Node *tmpl_node = new_template_decl_node(p, (Node **)params.data,
                                              params.len, decl, tok);

    if (tmpl_name) {
        Declaration *td = region_declare(p, tmpl_name->loc, tmpl_name->len,
                                          ENTITY_TEMPLATE, /*type=*/NULL);
        if (td) td->tmpl_node = tmpl_node;
        /* Also register the class type so out-of-class qualifier
         * lookups ('void Foo<T>::bar') can reach the class_region. */
        if (tmpl_class_type) {
            region_declare(p, tmpl_name->loc, tmpl_name->len,
                          ENTITY_TYPE, tmpl_class_type);
            region_declare(p, tmpl_name->loc, tmpl_name->len,
                          ENTITY_TAG, tmpl_class_type);
        }

        /* SHORTCUT (ours, not the standard): §14.3/11 says a friend-
         * declared template name is NOT found by lookup until a
         * matching real declaration appears at namespace scope. We
         * eagerly register in the enclosing namespace so template-id
         * parsing works regardless of forward-declaration order.
         * Mirrors the same shortcut on the non-template friend path
         * in parse_declaration above (TODO seafront#friend-visibility). */
        if (decl && decl->kind == ND_FRIEND) {
            DeclarativeRegion *ns = p->region;
            while (ns && ns->kind != REGION_NAMESPACE)
                ns = ns->enclosing;
            if (ns && ns != p->region) {
                Declaration *fd = region_declare_in(p, ns,
                    tmpl_name->loc, tmpl_name->len,
                    ENTITY_TEMPLATE, /*type=*/NULL);
                if (fd) fd->tmpl_node = tmpl_node;
            }
        }
    }

    return tmpl_node;
}

/*
 * parse_template_id — N4659 §17.2 [temp.names]
 *
 *   simple-template-id:
 *       template-name < template-argument-list(opt) >
 *
 *   template-argument:
 *       constant-expression
 *       type-id
 *       id-expression
 *
 * Called from primary_expr/postfix_expr when an identifier is known
 * to be a template-name (via lookup_is_template_name).
 *
 * Handles >> splitting: when template_depth > 0 and we see TK_SHR,
 * we treat it as two '>' tokens. The first closes the inner template-
 * argument-list; the remaining '>' is left for the outer.
 *
 * N4659 §17.2/3: "When parsing a template-argument-list, the first
 *   non-nested > is taken as the ending delimiter rather than a
 *   greater-than operator."
 */
Node *parse_template_id(Parser *p, Token *name) {
    Token *tok = name;
    parser_expect(p, TK_LT);
    p->template_depth++;

    Vec args = vec_new(p->arena);

    if (!parser_at(p, TK_GT) && !(parser_at(p, TK_SHR) && p->template_depth > 0)) {
        /* Terminates: each iteration parses a template argument
         * (consuming tokens), then breaks on non-comma. */
        for (;;) {
            /* Skip preprocessor leftovers (#line) that mcpp may emit
             * inside multi-line template-argument-lists. */
            while (parser_at(p, TK_HASH)) {
                int line = parser_peek(p)->line;
                while (!parser_at_eof(p) && parser_peek(p)->line == line)
                    parser_advance(p);
            }

            /* template-argument: type-id or constant-expression or id-expression.
             * N4659 §17.3/2 [temp.arg] Rule 5: "type-id always wins."
             * Try type-id first whenever the leading token COULD start
             * one — including unknown identifiers (which our heuristic
             * accepts as opaque types). */
            bool try_type =
                parser_at_type_specifier(p) ||
                parser_peek(p)->kind == TK_IDENT;
            if (try_type) {
                /* Tentative: try type-id */
                ParseState saved = parser_save(p);
                bool prev_tentative = p->tentative;
                p->tentative = true;
                bool saved_failed = p->tentative_failed;
                p->tentative_failed = false;
                Type *ty = parse_type_name(p);
                bool ty_ok = (ty != NULL) && !p->tentative_failed;
                p->tentative = prev_tentative;
                p->tentative_failed = saved_failed;

                /* Skip any #line directives before the , or > follows. */
                while (parser_at(p, TK_HASH)) {
                    int line = parser_peek(p)->line;
                    while (!parser_at_eof(p) && parser_peek(p)->line == line)
                        parser_advance(p);
                }

                if (ty_ok && (parser_at(p, TK_COMMA) || parser_at(p, TK_GT) ||
                              parser_at(p, TK_ELLIPSIS) ||
                              (parser_at(p, TK_SHR) && p->template_depth > 0))) {
                    /* Successfully parsed as type-id, and next is , or > */
                    /* Create a node to represent the type argument */
                    parser_restore(p, saved);
                    ty = parse_type_name(p);
                    vec_push(&args, new_var_decl_node(p, ty, /*name=*/NULL,
                                                      parser_peek(p)));
                } else {
                    /* Not a clean type-id — parse as expression */
                    parser_restore(p, saved);
                    vec_push(&args, parse_assign_expr(p));
                }
            } else {
                /* Not a type specifier — parse as expression */
                vec_push(&args, parse_assign_expr(p));
            }

            /* Pack expansion — N4659 §17.6.3 [temp.variadic]
             *   template-argument ... */
            parser_consume(p, TK_ELLIPSIS);

            while (parser_at(p, TK_HASH)) {
                int line = parser_peek(p)->line;
                while (!parser_at_eof(p) && parser_peek(p)->line == line)
                    parser_advance(p);
            }

            if (!parser_consume(p, TK_COMMA))
                break;
        }
    }

    /* Closing > — handle >> splitting.
     * N4659 §17.2/3 [temp.names]: "When parsing a template-argument-list,
     * the first non-nested > is taken as the ending delimiter rather than
     * a greater-than operator."
     *
     * For >>: advance past the >> token and set split_shr so the next
     * parser_peek()/parser_at()/parser_advance() returns a virtual TK_GT for the outer
     * template-argument-list. No token mutation — safe for tentative parsing. */
    if (parser_at(p, TK_SHR) && p->template_depth > 0) {
        parser_advance(p);         /* consume the >> token */
        p->split_shr = true; /* leave a virtual > for the outer consumer */
    } else {
        parser_expect(p, TK_GT);
    }

    p->template_depth--;

    /* Expand missing template arguments from declared defaults —
     * N4659 §17.6.4 [temp.arg.default]. A template-id with fewer
     * args than the template parameter list pulls defaults from
     * the template's declaration. Defaults may be dependent on
     * earlier parameters (e.g. 'typename A::default_layout'); for
     * those, substitute the already-bound earlier args via
     * subst_type, which replaces TY_DEPENDENT param references
     * with concrete types.
     *
     * Without this, a call-site like 'vec<int, va_gc>' carries 2
     * template-id args while the primary 'vec<T,A,L>' declares 3,
     * so overload resolution and deduction both short-circuit on
     * arity mismatch. The instantiation pass filled defaults later,
     * but only ran after the first sema pass — so sema's pass-1
     * overload resolution couldn't see viable templates. */
    /* Walk ALL ENTITY_TEMPLATE declarations for this name and pick
     * the PRIMARY — the one whose inner class/func has no
     * template_id_node (partial specs have one, primary doesn't).
     * Template defaults are declared on the primary (§17.6.4/9);
     * partial specs inherit their pattern but don't carry defaults
     * we can use for arg filling.
     *
     * Defaults can accumulate across multiple primary declarations
     * (§17.6.4/10). When more than one candidate primary is found,
     * prefer the one with the most default_types populated — that's
     * the latest declaration in source order and carries merged
     * defaults. */
    Declaration *decls[16];
    int nd = lookup_overload_set_from(p->region, name->loc, name->len,
                                       decls, 16);
    Node *primary = NULL;
    int primary_defaults = -1;
    for (int i = 0; i < nd; i++) {
        Declaration *cand = decls[i];
        if (!cand || cand->entity != ENTITY_TEMPLATE ||
            !cand->tmpl_node ||
            cand->tmpl_node->kind != ND_TEMPLATE_DECL)
            continue;
        Node *inner = cand->tmpl_node->template_decl.decl;
        if (!inner) continue;
        Type *ity = NULL;
        if (inner->kind == ND_CLASS_DEF)     ity = inner->class_def.ty;
        else if (inner->kind == ND_VAR_DECL) ity = inner->var_decl.ty;
        /* Primary: no template_id_node on inner type. For function
         * templates there's no inner type; treat any func-template
         * as primary. */
        bool is_primary = false;
        if (inner->kind == ND_FUNC_DEF || inner->kind == ND_FUNC_DECL)
            is_primary = true;
        else if (ity && !ity->template_id_node)
            is_primary = true;
        if (!is_primary) continue;
        /* Score by how many default_types are populated. */
        int ndef = 0;
        int np = cand->tmpl_node->template_decl.nparams;
        for (int k = 0; k < np; k++) {
            Node *tp = cand->tmpl_node->template_decl.params[k];
            if (tp && tp->param.default_type) ndef++;
        }
        if (ndef > primary_defaults) {
            primary_defaults = ndef;
            primary = cand->tmpl_node;
        }
    }

    if (primary) {
        int np = primary->template_decl.nparams;
        if (args.len < np) {
            SubstMap map = subst_map_new(p->arena, np > 0 ? np : 1);
            for (int i = 0; i < args.len && i < np; i++) {
                Node *tp = primary->template_decl.params[i];
                Node *arg = (Node *)args.data[i];
                if (!tp || !tp->param.name) continue;
                Type *at = (arg && arg->kind == ND_VAR_DECL)
                    ? arg->var_decl.ty : NULL;
                if (at) subst_map_add(&map, tp->param.name, at);
            }
            for (int i = args.len; i < np; i++) {
                Node *tp = primary->template_decl.params[i];
                if (!tp || !tp->param.default_type) break;
                Type *dt = subst_type(tp->param.default_type, &map, p->arena);
                Node *vd = new_var_decl_node(p, dt, /*name=*/NULL, tok);
                vec_push(&args, vd);
                if (tp->param.name)
                    subst_map_add(&map, tp->param.name, dt);
            }
        }
    }

    return new_template_id_node(p, name, (Node **)args.data, args.len, tok);
}
