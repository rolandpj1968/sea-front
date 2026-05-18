// EXPECT: 42
// GCC/Clang type-trait intrinsics that appear inside a template body
// with a dependent type argument must be deferred until template
// instantiation. Parse-time eval against TY_DEPENDENT folds to 0 for
// every category, so a folded ND_NUM would lock the result regardless
// of the concrete type the template later substitutes in.
//
// Sea-front preserves these as ND_TYPE_TRAIT through parse + clone,
// then re-evaluates at emit time with the substituted Type. N4659
// §17.6.4 [temp.point] — instantiation point semantics + standard
// library traits in <type_traits> (§23.15 [meta]).

struct A { int x; };
union U { int u; };

template<typename T>
int trait_value() {
    return __is_class(T) ? 1 : 0;
}

int main() {
    int r_class    = trait_value<A>();    // 1
    int r_union    = trait_value<U>();    // 0
    int r_scalar   = trait_value<int>();  // 0
    return (r_class == 1 && r_union == 0 && r_scalar == 0) ? 42 : 1;
}
