// EXPECT: 5
// NTTP arg from a class static-const-int member — N4659 §17.1/4
// [temp.param] + §17.7.1 [temp.inst]. Two access shapes, both
// resolved to the literal initializer at instantiation:
//
//   - In-class (ND_IDENT inside the class body where the member is
//     declared): 'Vec<int, N>' inside Cfg, where N is Cfg::N.
//   - Qualified (ND_QUALIFIED): 'Vec<int, Cfg::N>' from outside.
//
// The literal value substitutes into both the mangled tag (so two
// instantiations with different constants get distinct C symbols)
// and the cloned body (so 'T data[N]' becomes 'T data[5]').
//
// Real-world hit: gcc 14 libcpp/include/rich-location.h's
// 'static const int STATICALLY_ALLOCATED_RANGES = 3;' used as a
// template arg of semi_embedded_vec inside the same class.
//
// SHORTCUT (TODO seafront#nttp-scope-aware): the in-class case
// uses a TU-wide name walk because sema doesn't yet visit
// template-id arg expressions; threading the enclosing class
// context would let us disambiguate properly across classes that
// share a member name.

struct Cfg {
    static const int N = 5;
};

template <typename T, int K>
struct Vec {
    T data[K];
};

int main() {
    Vec<int, Cfg::N> v;
    int sum = 0;
    for (int i = 0; i < 5; ++i) {
        v.data[i] = i;
        sum++;
    }
    return sum;
}
