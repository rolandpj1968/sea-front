// EXPECT: 1
// Confirm 3-arg overload mangles separately from 1-arg overload —
// dedup key collision regression test.
template<typename T, typename A> struct vec {};
template<typename T, typename A> void foo(vec<T,A> *p) { (void)p; }
template<typename T, typename A> void foo(vec<T*,A> *p, void *a, void *b) { (void)p; (void)a; (void)b; }
template<typename T, typename A> void foo(vec<T,A> *p, void *x, void *y) { (void)p; (void)x; (void)y; }
struct Bar {}; struct Alloc {};
int main() {
    vec<Bar, Alloc> v;
    foo(&v);
    foo(&v, (void*)0, (void*)0);
    return 1;
}
