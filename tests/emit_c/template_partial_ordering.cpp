// EXPECT: 7
// Partial ordering of function templates per N4659 §17.5.5.2
// [temp.func.order]: when two viable templates differ at one
// position by `T` vs `T*`, the latter is strictly more specialized
// and must win the resolution. Both candidates deduce successfully,
// so the deduce-failure path doesn't cover this — partial ordering
// is a separate resolution stage.
struct vl_embed {};
struct va_heap {};
template<typename T, typename A, typename L> struct vec {};

template<typename T, typename A>
int picker(vec<T, A, vl_embed> *v) { (void)v; return 1; }

template<typename T, typename A>
int picker(vec<T*, A, vl_embed> *v) { (void)v; return 7; }

int main() {
    vec<int*, va_heap, vl_embed> v;
    return picker(&v);
}
