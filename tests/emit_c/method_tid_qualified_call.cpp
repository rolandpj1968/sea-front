// EXPECT: 0
// Qualified static-method call with explicit method-template-args:
// `S<10>::foo<3>()` — body+call mangle must both include the
// `<method-name>I<args>E` Itanium segment. Without symmetric
// updates the body emitted `_ZN1SILi10EE3fooEv` while the call
// site emitted `_ZN1SILi10EE3fooILi3EEEv` (or vice versa) and the
// link broke.
//
// Slice covers:
//   - Parser: capture trailing template-id as qualified.tail_tid
//   - Clone:  preserve tail_tid through template-body cloning
//   - Inst:   bind explicit method NTTPs into the SubstMap so
//             the body's `M` substitutes to `3`; populate
//             cloned->func.template_args so emit reads them
//   - Codegen call site (ND_QUALIFIED): thread tail_tid through
//             mangle_class_method_tid
//   - Codegen body emit: route through mangle_class_method_tid
//             when func.template_args is set
//   - SubstMap capacity doubled so concrete + tt entries for both
//             outer (N=10) and inner (M=3) all fit
//
// Sibling extension: sibling-call reduction (ND_TEMPLATE_ID →
// ND_IDENT(implicit_this)) preserves the pre-reduction
// template-id on ident.method_template_id; the implicit-this
// call-site mangle reads it back.
//
// N4659 §17.2/3 [temp.names] + §17.7.1 [temp.inst];
// Itanium ABI §5.1.6.7.

extern "C" void abort();

template<int N> struct S {
    template<int M> static int foo() { return N + M; }
};

int main() {
    if (S<10>::foo<3>()  != 13) abort();
    if (S<10>::foo<7>()  != 17) abort();
    if (S<100>::foo<3>() != 103) abort();
    return 0;
}
