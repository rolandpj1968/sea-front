// EXPECT: 0
// Mem-init `: m(t)` where m has class type but no user-declared copy
// ctor — N4659 §15.8 [class.copy]/8 says an implicit copy ctor is
// generated. Sea-front's overload resolution doesn't enumerate that
// implicit ctor, so resolve_overload returns -1 and the prior emit
// aborted with "no matching overload".
//
// The fall-back: if the 1-arg mem-init's arg type is the member's
// type, lower it as `this->m = src` (C bitwise struct copy). Matches
// the implicit copy ctor's memberwise-copy semantics for POD-shaped
// classes. Pattern: g++.dg/opt/const1.C — `C<B>::C(const B &t) : c(t)`.

struct B { int x; };

template <class T>
struct C {
    T c;
    C(const T &t) : c(t) {}
};

int main() {
    B b;
    b.x = 42;
    C<B> c(b);
    return c.c.x == 42 ? 0 : 1;
}
