// EXPECT: 0
// Variadic TYPE pack appearing inside a TYPE position of a
// pack-expanded expression — `sizeof(Ts)...` for `Tuple<int, char>`.
// N4659 §17.5.3.4/4 [temp.variadic.expand]: each expansion of a
// pack-expansion produces a comma-separated list where the i-th
// expansion substitutes the i-th element of each pack mentioned.
//
// clone_node_array_pack iterated j = 0..pack_ntypes-1, cloned the
// source expression each iteration, then morphed the cloned ident
// for NTTP packs. For type packs, the per-iteration TYPE binding
// of `Ts` wasn't reaching subst_type — the pack entry's
// concrete_type is NULL (pack entries store pack_types separately),
// and subst_map_lookup skips is_pack entries — so `Ts` stayed as
// TY_DEPENDENT and emit_type fell back to `int /*dep:Ts*/`.
// Result: `sizeof(Ts)...` for <int, char> emitted
// `sizeof(int), sizeof(int)` instead of `sizeof(int), sizeof(char)`.
//
// Fix: per-iteration, push a single-element non-pack shadow entry
// {param_name=pack_name, concrete_type=pack_types[j]} into the
// SubstMap, clone, then pop. SubstMap capacity gets +1 headroom
// at the instantiate site to make room for the shadow.

extern "C" void abort();

template<typename... Ts> struct Tuple {
    static int sum_sizeof() {
        int s = 0;
        int arr[] = { (int)sizeof(Ts)... };
        for (unsigned i = 0; i < sizeof(arr)/sizeof(arr[0]); i++)
            s += arr[i];
        return s;
    }
};

int main() {
    /* int=4, char=1 → 5 */
    if (Tuple<int, char>::sum_sizeof() != 5) abort();
    /* int=4, int=4, int=4 → 12 (regression guard: pack iterates) */
    if (Tuple<int, int, int>::sum_sizeof() != 12) abort();
    /* Single-element pack */
    if (Tuple<long>::sum_sizeof() != (int)sizeof(long)) abort();
    return 0;
}
