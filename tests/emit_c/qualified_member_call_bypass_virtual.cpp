// EXPECT: 0
// N4659 §13.3/15 [class.virtual]:
//   "Explicit qualification with the scope operator (8.1.4.3)
//    suppresses the virtual call mechanism."
//
// 'obj->Base::method()' binds directly to Base::method even when
// method is virtual. Sea-front previously dropped the qualifier
// entirely in the member-access parser and routed every virtual
// call through the vptr, getting B::f at line 11 instead of A::f.
//
// Real-world hit: g++.dg/template/overload7.C ('static_cast<A*>(a)
// ->A::Foo()' inside a template body). Also a common idiom in
// derived-class implementations that forward to a specific base
// override.

struct A { virtual int f() { return 1; } };
struct B : A { virtual int f() { return 2; } };

int main() {
    B b;
    // Qualified call — must dispatch directly to A::f, not via vptr.
    int via_a = (&b)->A::f();
    if (via_a != 1) return 1;
    // Unqualified call — virtual dispatch, should reach B::f.
    int via_b = (&b)->f();
    if (via_b != 2) return 2;
    return 0;
}
