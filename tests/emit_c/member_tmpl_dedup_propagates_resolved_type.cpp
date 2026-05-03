// EXPECT: 42
// Member template called multiple times with the SAME (class, member,
// deduced template-args) tuple. The first call drives instantiation
// and emits the substituted definition; the second-and-later calls
// dedup-hit. Without propagating the substituted TY_FUNC across the
// dedup hit, those later call sites' callee->resolved_type stays
// dependent and codegen mangles the param suffix as the source
// template's TY_DEPENDENT param name (e.g. '_p_Argument_pe_'),
// producing a symbol the unique definition never emits.
//
// Pattern from gcc 4.8 tree-ssa-threadupdate.c, where three identical
//   redirection_data.traverse<ssa_local_info_t*, callbackN>(&local)
// calls produced one '_p_ssa_local_info_t_ptr_pe_' definition and
// two '_p_Argument_pe_' call sites — link error.
//
// Standard: N4659 §17.7.1 [temp.inst] — each unique
// specialization is one entity; identical (template, args) calls
// all reach it.

template<typename T>
struct Box {
    T base;
    template<typename U>
    int op(U u) { return (int)base + (int)u; }
};

int main() {
    Box<int> a;
    a.base = 20;
    int r1 = a.op<int>(10);   // first  — drives instantiation
    int r2 = a.op<int>(10);   // second — dedup hit
    int r3 = a.op<int>(2);    // third  — dedup hit (same op<int> sig)
    return r1 + r2 + r3 - 40;
}
