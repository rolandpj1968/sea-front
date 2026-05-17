// EXPECT: 42
// Mem-initializer list with a base-class entry that takes arguments
// — N4659 §15.6.2 [class.base.init]. The base ctor must be called
// with the user-supplied args before the derived ctor body runs.
// Previously emit_ctor_member_inits only emitted default-ctor calls
// for bases (the explicit-args path was unimplemented), so
// 'B(int x) : A(x) {}' compiled to an empty body and left A's
// fields uninitialised.

struct A {
    int v;
    A(int x) : v(x) {}
};

struct B : A {
    B(int x) : A(x) {}    // pass-through to base ctor
};

int main() {
    B b(42);
    return b.v;           // reads inherited A::v through __sf_base
}
