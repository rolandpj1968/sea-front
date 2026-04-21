// EXPECT: 42
// An instantiated function template that's CALLED from another
// instantiation (not from user code) may be emitted AFTER its caller
// in the TU. Without a forward declaration for the callee, the C
// compiler hits an implicit-function declaration at the call site and
// then 'conflicting types' when the real definition appears later.
//
// Pattern from gcc 4.8 is-a.h: dyn_cast<T>(p)'s body calls is_a<T>(p).
// Both get instantiated per request; the emission order puts dyn_cast
// before is_a (or vice versa — either order needs the forward decl).
//
// Fix: extend the forward-decl sweep (previously methods-only) to
// also forward-declare free function templates whose mangled name has
// the '<name>_t_..._te_' instantiation shape.
//
// N4659 §17.8.1 [temp.inst], C11 §6.5.2.2 (function calls).
template<typename T, typename U>
bool is_a(U *p) { return p != 0; }

template<typename T, typename U>
T *dyn_cast(U *p) {
    if (is_a<T>(p)) return (T *)p;
    return 0;
}

struct A { int x; };
struct B { int y; };

int main() {
    B b;
    // Instantiates dyn_cast<A, B> AND (transitively) is_a<A, B>.
    A *a = dyn_cast<A>(&b);
    return a ? 42 : 0;
}
