// EXPECT: 0
// N4659 §17.7.1/8 [temp.inst]: an NTTP default value is substituted
// at the template-id's instantiation point. Sea-front previously
// special-cased only literals, ND_TYPE_TRAIT, and bare idents — any
// arithmetic-expression default fell through unbound and the body
// emitted with an unresolved param identifier (e.g. `return N` →
// `return N` with N undeclared in the C output).
//
// Routing the default through nttp_arg_to_literal_token (which
// covers literals directly and arbitrary const-int expressions via
// eval_const_int) closes that gap. Same mechanism the slice for
// explicit NTTP args uses.

extern "C" void abort();

template<int N = 5 + 3> struct D {
    static int v() { return N; }
};

template<int N = (1 << 3) | 1> struct E {
    static int v() { return N; }
};

template<int N = (int)42> struct F {
    static int v() { return N; }
};

int main() {
    if (D<>::v() != 8) abort();
    if (E<>::v() != 9) abort();
    if (F<>::v() != 42) abort();

    /* Explicit arg still overrides the default. */
    if (D<100>::v() != 100) abort();
    return 0;
}
