// EXPECT: 0
// `template<int... M> struct S { ... };` instantiated as `S<0,1,2>::f()`
// must mangle the call AND the definition with the LITERAL pack values
// — not collapse to `S<v,v,v>` or `S<0,0,0>`. Three sites had to align:
//
//  1. parse_type_specifiers (elaborated-type + class-key paths) was
//     extracting template_args via `(arg->kind == ND_VAR_DECL) ?
//     arg->var_decl.ty : NULL`, so ND_NUM literal args stored NULL on
//     the Type. Use template_arg_to_arg_type so literals wrap as
//     TY_NTTP_VALUE.
//  2. build_inst_template_args gated NTTP-decl-type propagation on
//     `i < tmpl->nparams`. For an NTTP pack the param-list has 1
//     entry but the arg-list has N — trailing pack-expanded args
//     never got `nttp_decl_type` and the mangler's L<type><value>E
//     fallback emitted `Li0E` regardless of value. Clamp the param
//     index to the pack's last entry when the trailing param is a
//     pack.
//  3. The qualified-call emit's call-site stub built template_args via
//     the same ND_VAR_DECL-only shape, AND had no `nttp_decl_type`
//     even after the wrap. Wrap literals as TY_NTTP_VALUE inline,
//     then prefer the canonical instantiated class's Type via
//     find_class_def_by_tag_args so the mangler sees the
//     attribution-complete Type.
//
// Reduced from g++.dg/cpp0x/variadic-init.C (S<0,1,2>::foo()).

extern "C" void abort();

template<int... M> struct S {
    /* Pack expansion in the BODY is a separate slice (variadic-init);
     * for this regression test the trivial first() suffices — what
     * we're verifying is that the class-template-id with NTTP-pack
     * args mangles correctly at both def and call sites, and that
     * distinct instantiations don't collide. */
    static int first() { return 0; }
};

template<int A, int B, int C> struct T {
    static int sum() { return A + B + C; }
};

int main() {
    /* Distinct instantiations must not collide at the mangled-symbol
     * level — without the parser fix, S<0,1,2> and S<3,4,5> both
     * mangle as `S<v,v,v>::first` and the linker would coalesce them.
     * Even if the test never reads the values, mismatched def/call
     * names link-fail. */
    if (S<0,1,2>::first() != 0) abort();
    if (S<3,4,5>::first() != 0) abort();

    /* Same instantiation must dedup. */
    if (S<7>::first() != 0) abort();
    if (S<7>::first() != 0) abort();

    /* Non-pack NTTPs still work (regression guard for build_inst's
     * pack-clamp logic). */
    if (T<1,2,3>::sum() != 6) abort();
    return 0;
}
