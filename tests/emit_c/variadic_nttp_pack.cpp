// EXPECT: 0
// NTTP pack: `template<typename T, T... Values>` declares a
// non-type template parameter pack whose elements bind to the
// trailing run of explicit / deduced template args. N4659 §17.5.3
// [temp.variadic] + §17.1/4 [temp.param].
//
// Sea-front handled type packs (`typename... Ts`) but not NTTP
// packs: the parser dropped the `...` between type and name, and
// even when present the instantiator's pack-bind path used
// type_arg_from_node (only recognises ND_VAR_DECL args), so the
// numeric literals at `f<int, 1, 2, 3>` produced empty bindings.
//
// Now: parser sets `param.is_pack = true` for NTTP packs;
// the pack-bind path uses template_arg_to_arg_type_resolved so
// literals wrap as TY_NTTP_VALUE; the cloner's pack-expansion
// branch morphs each cloned ND_IDENT into the literal NodeKind
// (TK_NUM → ND_NUM etc.) so `{Values...}` lowers to `{1, 2, 3}`
// and `sizeof...(Values)` constant-folds to the pack count.
//
// Reduced from g++.dg/cpp0x/variadic68.C.

extern "C" void abort();

template<typename T, T... Values>
void f(T *expected, int n) {
    if (sizeof...(Values) != n) abort();
    T values[] = { Values... };
    for (int i = 0; i < n; ++i)
        if (values[i] != expected[i]) abort();
}

int main() {
    int a3[3] = { 1, 2, 3 };
    f<int, 1, 2, 3>(a3, 3);

    int a1[1] = { 42 };
    f<int, 42>(a1, 1);

    return 0;
}
