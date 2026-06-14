// EXPECT: 0
// Variadic template instantiated with empty pack: a direct-init
// declarator inside the template body — `T y(args...);` — must
// pack-expand the args at instantiation. With the empty pack, the
// declarator becomes `T y();`, which is direct-init from an empty
// expression-list and value-initializes scalar T to 0.
//
// Two slice-fixes meet here:
//   - parse_declaration's direct-init arg loop now marks
//     trailing `...` as pack-expansion (was: consumed and dropped).
//   - clone_node ND_VAR_DECL now routes ctor_args through
//     clone_node_array_pack so `args...` expands per the bound
//     pack at instantiation.
//   - emit ND_VAR_DECL adds the scalar value-init store for
//     has_ctor_init with ctor_nargs==0.
//
// N4659 §17.5.3.4 [temp.variadic.expand] for the pack expansion.
// N4659 §11.6/8 [dcl.init] for scalar value-init from `T()` → 0.
// Reduced from g++.dg/cpp0x/variadic-new2.C.

extern "C" void abort(void);

template <class... Args>
void f(Args... args) {
    int y(args...);
    if (y != 0) abort();
}

int main() {
    f();
    return 0;
}
