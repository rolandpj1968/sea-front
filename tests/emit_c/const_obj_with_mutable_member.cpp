// EXPECT: 0
// A `const` TU-scope object whose type has a `mutable` data member
// (directly or via base / member-of-member) must NOT land in
// .rodata. C++ lets the write proceed via §10.1.1/8 [dcl.stc] —
// sea-front's ND_ASSIGN emits a `*(int*)&obj.m = ...` cast-trick
// to satisfy cc. The trick segfaults on .rodata-mapped objects,
// so the C-level declaration drops the const when the type has
// any mutable transitive member.
//
// Reduced from g++.dg/cpp0x/mutable1.C.

extern "C" void abort(void);

struct Base { mutable int i; };
struct Derived : Base {};

const Derived d_inherited{};

struct Wrap {
    Base inner;
};
const Wrap d_nested{};

int main() {
    d_inherited.i = 42;  // mutable via base
    if (d_inherited.i != 42) abort();
    d_nested.inner.i = 99;  // mutable via member
    if (d_nested.inner.i != 99) abort();
    return 0;
}
