// EXPECT: 42
// NTTP default substitution with a type-trait intrinsic — N4659
// §17.1/8 [temp.param] permits NTTPs to carry defaults; §17.7.1/8
// [temp.inst] requires substitution at the instantiation point.
//
// The default '__is_class(T)' must be evaluated against the bound T
// at instantiation: M<A> has v=1 (A is a class), M<int> has v=0
// (int is not). This is the libstdc++ <type_traits> shape for
// __is_class/__is_pod/__is_empty etc. that route the trait through
// an NTTP default.
//
// Verifies three things together:
//   - NTTP default expression survives parse without being misread
//     as a type-param default (var_decl.init and param.default_type
//     alias the same Node-union slot at offset 16);
//   - the trait is re-evaluated per instantiation point (deferred
//     ND_TYPE_TRAIT, substituted via the param→arg map);
//   - the mangled class tag carries the evaluated NTTP value so the
//     definition site and the qualified-id reference site agree.
//
// gcc/clang both accept this; covered by g++.dg/ext/is_class.C
// cluster in the gcc dg suite.

struct A { int x; };

template<typename T, int v = __is_class(T)>
struct M { static const int trait = v; };

int r1 = M<A>::trait;     // expects 1 (class)
int r2 = M<int>::trait;   // expects 0 (not a class)

int main() {
    return (r1 == 1 && r2 == 0) ? 42 : 1;
}
