// EXPECT: 0
// Implicitly-declared `B::operator=(const B&)` must override a
// base virtual `A::operator=(const B&)` and run as the dynamic
// dispatch through `A&` bound to a `B`. Three cooperating pieces:
//
//   1. Assignment codegen routes through the vtable when the
//      resolved op= is virtual on the static LHS type.
//   2. emit_class_def synthesizes B::operator= (chain-to-base,
//      memberwise copy of own members, return this).
//   3. The vtable instance fill overrides the op= slot with the
//      synth's mangled name when class_needs_synth_op_assign_override.
//   4. The any_virtual_has_body gate is widened so B's user ctor
//      installs B's vptr (otherwise it inherits A's vtable and
//      dispatch routes to A::op=).
//
// Real-world hit: g++.dg/inherit/virtual5.C
//   struct A { virtual B& operator=(const B&); };
//   struct B : A { ... };
//   B& A::operator=(const B&) { return static_cast<B&>(*this); }
//   A &ar = b1; ar = b2;   // should memberwise-copy b2 into b1

extern "C" void abort();

struct B;

struct A {
    virtual B& operator=(const B&);
};

struct B : A {
    int i;
    B(int x) : i(x) {}
    // implicitly-declared op=(const B&) — overrides A's virtual op=
};

B& A::operator=(const B&) {
    return static_cast<B&>(*this);
}

int main() {
    B b1(123);
    B b2(0);
    A &ar = b1;
    ar = b2;        // virtual dispatch → B's synth op= → b1.i = b2.i = 0
    if (b1.i != 0) abort();
    return 0;
}
