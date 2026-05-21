// EXPECT: 0
// MI override of a base virtual: the derived class's method is
// IMPLICITLY virtual even when the source omits the `virtual`
// keyword (N4659 §13.3/2 [class.virtual]). Sea-front must:
//   1. Mark the derived method as virtual at class finalization,
//      so the vtable struct + instance include the slot.
//   2. For non-first bases, emit a `this`-adjusting thunk so
//      dispatch through a `Base2*` reaches the override via the
//      offset-adjusted derived `this`.
//
// Pattern: g++.dg/inherit/thunk10.C. The override declarations
// here drop the `virtual` keyword to exercise the implicit path.

struct B1 { virtual int foo1(); int b1; };
struct B2 { virtual int foo2(); int b2; };
struct D : B1, B2 {
    int foo1();     // implicit-virtual override of B1::foo1
    int foo2();     // implicit-virtual override of B2::foo2
    int d;
};
int B1::foo1() { return 3; }
int B2::foo2() { return 4; }
int D::foo1()  { return 1; }
int D::foo2()  { return 2; }

int main() {
    D d;
    B1 *b1p = &d;
    B2 *b2p = &d;
    if (b1p->foo1() != 1) return 1;
    if (b2p->foo2() != 2) return 2;
    return 0;
}
