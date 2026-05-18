// EXPECT: 42
// Copying a class value from a reference parameter — 'A c = a;'
// where 'a' is 'A &'. In C the lowered signature is
// 'void foo(struct A *a)' and the bare ref-param 'a' is dereferenced
// to its value by emit_ident. The var-decl init path also wraps a
// ref-typed init with '(*...)' so the C-level pointer becomes a
// struct value. Without coordination, both layers fire and the
// emitted C is '*(*a)' — a double deref that the C compiler
// rejects ('invalid type argument of unary *').
//
// Sea-front suppresses the inner ref-deref while emitting the init
// so the outer '(*...)' wrapper is the only deref.
//
// Test pattern modelled on g++.dg/torture/pr59163.C 'A c = a;'.

struct A {
    int x;
};

void foo(A &a) {
    A c = a;        // copy ctor via ref — single deref
    c.x += 41;
    a = c;
}

int main() {
    A a = {1};
    foo(a);
    return a.x;
}
