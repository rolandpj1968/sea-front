// EXPECT: 1
// gcc 4.8 cgraph.h: explicit specialization of a member template
// inside a class template.
//   template<> template<>
//   inline bool is_a_helper<cgraph_node>::test(symtab_node_def *p);
//
// Without 837a62c → next-fix: the OOL search picked up this
// explicit specialization as if it were the primary's OOL def
// and CLONED it as a fresh instantiation. Source-level emit + the
// cloned emit then collided on the same mangled symbol — 'error:
// redefinition of sf__is_a_helper_t_cgraph_node_te___test_p_..._pe_'.
//
// Standard: N4659 §17.8.4 [temp.expl.spec] (an explicit
// specialization is a distinct entity from the primary; the
// primary is not used to generate it). The two-head shape is
// recognised by all heads having nparams == 0.

struct cgraph_node {};
struct symtab_node_def {};

template<typename T>
struct is_a_helper {
    template<typename U>
    static bool test(U *p);
};

template<>
template<>
inline bool is_a_helper<cgraph_node>::test(symtab_node_def *p) {
    return p != 0;
}

int main() {
    symtab_node_def n;
    return is_a_helper<cgraph_node>::test(&n) ? 1 : 0;
}
