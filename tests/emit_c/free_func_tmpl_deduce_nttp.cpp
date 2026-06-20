// EXPECT: 0
// Free function template with NTTP deducible from a class-template
// arg: `template<int I> int Foo(A<I> a)` called as `Foo(a)`. Three
// interlocking gaps closed:
//
//   1. sema/visit_call's is_type_call detection for ND_TEMPLATE_ID
//      callees (`A<2>()`) was setting resolved_type to the PRIMARY
//      template's Type (no template-args), so downstream deduction
//      couldn't see the literal arg. Now synthesises a
//      per-instantiation Type carrying the template-id's args
//      (TY_NTTP_VALUE for literal NTTPs).
//
//   2. deduce_from_pair's TY_STRUCT-vs-TY_STRUCT unification only
//      handled ND_VAR_DECL pattern args. ND_IDENT args (NTTP
//      param refs like `A<I>` in Foo's param type) bypassed the
//      binding loop. Now binds the param-name to A's
//      TY_NTTP_VALUE when at is one. (Iteration 9, commit
//      97dcdfa.)
//
//   3. subst_type's needs_subst check for TY_STRUCT with
//      template_id_node only flagged ND_VAR_DECL args carrying
//      dependent types. ND_IDENT args naming outer NTTPs slipped
//      through and the cloned function's param type kept the
//      un-substituted `A<I>` (with `I` as a bare ident),
//      producing spurious `_ZN1AILiIEE`-style instantiations.
//      Now scans for ND_IDENT args bound via the SubstMap and
//      morphs them to the concrete arg in the cloned template-id.
//
//   4. collect_from_type's dep-arg skip extended to recognise
//      ND_IDENT NTTP-param-ref args via REGION_TEMPLATE home.
//      (Iteration 10, commit b1ace11.)
//
// N4659 §17.8.2.5 [temp.deduct.type] + §13.8.3 [temp.dep.type].

extern "C" void abort();

template<int I> struct A {
    int v;
    A() : v(I) {}
};

template<int I> int Foo(A<I> a) { return a.v + I; }

int main() {
    A<5>  a5;
    A<10> a10;
    if (Foo(a5)  != 10) abort();  /* I deduced as 5;  5 + 5 = 10 */
    if (Foo(a10) != 20) abort();  /* I deduced as 10; 10 + 10 = 20 */
    return 0;
}
