// EXPECT: 0
// typeid — N4659 §8.2.7 [expr.typeid]. Static-type form: typeid(T)
// and typeid(expr) where expr is not a polymorphic glvalue. Sea-front
// lowers these to the address of a per-type sentinel char; the test's
// comparisons reduce to pointer equality, which is correct for static
// types.
//
// Real-world hit: libstdc++ <bits/exception_ptr.h> uses
// '&typeid(_Ex)' to record the static type of a thrown exception
// (templated function, _Ex is the type-id form).
//
// Note: polymorphic-glvalue typeid (typeid(*virtual_base_ptr)) is NOT
// yet wired through; that's the dynamic-RTTI slice (docs/rtti.md).

/* Deliberately NOT #including <typeinfo> — libstdc++ 13's header
 * has a static-vs-extern inline mismatch that sea-front's lowering
 * surfaces (separate bug, not the typeid feature itself). Slice 1
 * compares typeids as pointer-equality, which works without the
 * type_info type itself being visible. */

struct A {};
struct B {};

int main() {
    // typeid(type-id) form — both sides static
    if (typeid(int) == typeid(double)) return 1;
    if (typeid(int) != typeid(int))    return 2;

    // typeid(expr) form — static type from the expression's resolved
    // type. Non-polymorphic class types.
    A a; B b;
    if (typeid(a) == typeid(b)) return 3;
    if (typeid(a) != typeid(A)) return 4;

    // Mixed type-id and expr forms over the same static type — must
    // collapse to the same sentinel.
    int x = 0;
    if (typeid(x) != typeid(int)) return 5;

    return 0;
}
