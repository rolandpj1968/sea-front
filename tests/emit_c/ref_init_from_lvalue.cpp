// EXPECT: 42
// A reference variable initialized from an lvalue must lower to a
// pointer holding the lvalue's ADDRESS. Previously we emitted
// 'T* r = x;' (without &), which tripped '-Wincompatible-pointer-types'
// because x is a struct value, not a pointer. N4659 §11.3.2 [dcl.ref].
struct X { int v; };

int main() {
    X x;
    x.v = 42;
    X& r = x;
    const X& cr = x;
    return r.v + cr.v - x.v;   // 42 + 42 - 42 = 42
}
