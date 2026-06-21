// EXPECT: 0
// `A &aa = b;` where b is a Derived class object — sea-front lowers
// the reference to a pointer. The init expression `b` is an lvalue
// of the derived class; the surrounding init emits `&` to take the
// address, then emit_init_with_target must thread the
// derived-to-base offset through that address so the pointer
// targets the base subobject. Without the upcast, the C-level
// `struct sf__A* aa = &b;` triggers an incompatible-pointer-type
// warning and dispatching through aa reads garbage past the base.
//
// Real-world hit: g++.dg/cpp0x/range-for15.C
// `A &aa = b;` then `for (int x : aa)`.

extern "C" void abort();

struct A {
    int va;
    A() : va(11) { }
};

struct B : A {
    int vb;
    B() : vb(22) { }
};

int main() {
    B b;
    A &aa = b;
    if (aa.va != 11) abort();
    if (&aa != (A *)&b) abort();  // base subobject identity
    return 0;
}
