// EXPECT: 0
// Same as mem_init_implicit_copy.cpp but for base mem-init. When
// `class D : public B` lists `B(B())` in the mem-init list and B
// has no user-declared copy ctor, the implicit one applies — N4659
// §15.8 [class.copy]/8. Sea-front's overload resolution doesn't
// enumerate that implicit ctor, so the prior emit aborted at
// "no matching overload for base mem-init ctor call".
//
// Fall-back: when na==1 and source-type matches base-type, emit
// `this->__sf_baseN = src` (C struct copy). Pattern:
// g++.dg/init/empty1.C — class P inheriting from EmptyBase1 with
// `: EmptyBase1(EmptyBase1())`.

struct Base { int v; };

struct Derived : public Base {
    Derived(int x) : Base(make_base(x)) {}
    static Base make_base(int x) {
        Base b;
        b.v = x;
        return b;
    }
};

int main() {
    Derived d(42);
    return d.v == 42 ? 0 : 1;
}
